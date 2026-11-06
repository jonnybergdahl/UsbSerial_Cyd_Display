namespace Bergdahl.UsbSerialService;

using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging;
using MQTTnet;
using MQTTnet.Client;

public sealed class MqttPublisher : IAsyncDisposable
{
    private readonly ILogger<MqttPublisher> _log;
    private readonly IConfiguration _cfg;

    private IMqttClient? _client;
    private MqttClientOptions? _options;

    public string TopicBase { get; private set; } = "windows/unknown/serialports";

    public MqttPublisher(ILogger<MqttPublisher> log, IConfiguration cfg)
    {
        _log = log;
        _cfg = cfg;
        BuildOptions();
    }

    private void BuildOptions()
    {
        var host = _cfg["Mqtt:Host"] ?? throw new InvalidOperationException("Mqtt:Host missing");
        var port = int.TryParse(_cfg["Mqtt:Port"], out var p) ? p : 1883;

        var clientId = _cfg["Mqtt:ClientId"] ?? "serialport-watcher";
        TopicBase = (_cfg["Mqtt:TopicBase"] ?? "windows/${COMPUTERNAME}/serialports")
            .Replace("${COMPUTERNAME}", Environment.MachineName, StringComparison.OrdinalIgnoreCase);

        var user = _cfg["Mqtt:Username"];
        var pass = _cfg["Mqtt:Password"];

        var willTopic = $"{TopicBase}/availability";

        var builder = new MqttClientOptionsBuilder()
            .WithClientId($"{clientId}-{Environment.MachineName}")
            .WithTcpServer(host, port)
            .WithWillTopic(willTopic)
            .WithWillPayload("offline")
            .WithWillRetain(true);

        if (!string.IsNullOrWhiteSpace(user))
            builder = builder.WithCredentials(user, pass);

        _options = builder.Build();
    }

    public async Task EnsureConnectedAsync(CancellationToken ct)
    {
        if (_client is { IsConnected: true })
            return;

        _client ??= new MqttFactory().CreateMqttClient();

        // Simple reconnect loop
        var delay = TimeSpan.FromSeconds(1);

        while (!ct.IsCancellationRequested)
        {
            try
            {
                if (_client.IsConnected)
                    return;

                _log.LogInformation("Connecting to MQTT...");
                await _client.ConnectAsync(_options!, ct);

                _log.LogInformation("MQTT connected.");
                await PublishAvailabilityAsync("online", ct);
                return;
            }
            catch (Exception ex) when (!ct.IsCancellationRequested)
            {
                _log.LogWarning(ex, "MQTT connect failed. Retrying in {Delay}s...", delay.TotalSeconds);
                await Task.Delay(delay, ct);
                delay = TimeSpan.FromSeconds(Math.Min(delay.TotalSeconds * 2, 30));
            }
        }
    }

    public async Task PublishAvailabilityAsync(string status, CancellationToken ct)
    {
        if (_client is null || !_client.IsConnected) return;

        var msg = new MqttApplicationMessageBuilder()
            .WithTopic($"{TopicBase}/availability")
            .WithPayload(status)
            .WithQualityOfServiceLevel(MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce)
            .WithRetainFlag(true)
            .Build();

        await _client.PublishAsync(msg, ct);
    }

    public async Task PublishStateAsync(string hostName, IReadOnlyList<SerialPortInfo> ports, CancellationToken ct)
    {
        if (_client is null || !_client.IsConnected) return;

        var payloadObj = new
        {
            host = hostName,
            ts = DateTimeOffset.Now,
            ports = ports.Select(p => new
            {
                port = p.Port,
                name = p.Name,
                pnpId = p.PnpDeviceId,
                vid = p.Vid,
                pid = p.Pid,
                manufacturer = p.Manufacturer
            })
        };

        var json = JsonSerializer.Serialize(payloadObj, new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            WriteIndented = false
        });

        var msg = new MqttApplicationMessageBuilder()
            .WithTopic($"{TopicBase}/state")
            .WithPayload(Encoding.UTF8.GetBytes(json))
            .WithQualityOfServiceLevel(MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce)
            .WithRetainFlag(true)
            .Build();

        await _client.PublishAsync(msg, ct);
    }

    public async ValueTask DisposeAsync()
    {
        try
        {
            if (_client is { IsConnected: true })
            {
                await PublishAvailabilityAsync("offline", CancellationToken.None);
                await _client.DisconnectAsync();
            }
        }
        catch
        {
            // best-effort
        }
    }
}
