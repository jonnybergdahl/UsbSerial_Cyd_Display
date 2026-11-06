namespace Bergdahl.UsbSerialService;

using System.Management;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;

public sealed class Worker : BackgroundService
{
    private readonly ILogger<Worker> _log;
    private readonly SerialPortScanner _scanner;
    private readonly MqttPublisher _mqtt;
    private readonly int _debounceMs;
    private readonly int _publishIntervalSeconds;

    private readonly object _gate = new();
    private DateTimeOffset _lastSignal = DateTimeOffset.MinValue;
    private bool _pending;

    private ManagementEventWatcher? _watcher;

    public Worker(
        ILogger<Worker> log,
        SerialPortScanner scanner,
        MqttPublisher mqtt,
        IConfiguration cfg)
    {
        _log = log;
        _scanner = scanner;
        _mqtt = mqtt;

        _debounceMs = int.TryParse(cfg["SerialPorts:DebounceMs"], out var d) ? d : 400;
        _publishIntervalSeconds = int.TryParse(cfg["SerialPorts:PublishIntervalSeconds"], out var s) ? s : 0;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        _log.LogInformation("SerialPort MQTT service starting on {Machine}.", Environment.MachineName);

        StartDeviceWatcher();

        // Initial publish
        await PublishNowAsync(stoppingToken);

        var periodic = _publishIntervalSeconds > 0
            ? new PeriodicTimer(TimeSpan.FromSeconds(_publishIntervalSeconds))
            : null;

        try
        {
            while (!stoppingToken.IsCancellationRequested)
            {
                // Debounced publish on device-change signals
                if (ShouldPublishNow())
                {
                    await PublishNowAsync(stoppingToken);
                }

                if (periodic is not null)
                {
                    // Wait for either debounce window or periodic tick
                    var debounceDelay = Task.Delay(100, stoppingToken);
                    var periodicTick = periodic.WaitForNextTickAsync(stoppingToken).AsTask();
                    await Task.WhenAny(debounceDelay, periodicTick);

                    if (periodicTick.IsCompletedSuccessfully && periodicTick.Result)
                    {
                        await PublishNowAsync(stoppingToken);
                    }
                }
                else
                {
                    await Task.Delay(100, stoppingToken);
                }
            }
        }
        finally
        {
            periodic?.Dispose();
            StopDeviceWatcher();
            await _mqtt.PublishAvailabilityAsync("offline", CancellationToken.None);
        }
    }

    private void StartDeviceWatcher()
    {
        // Fires on device insert/remove/config change (a bit noisy, but we debounce)
        var query = new WqlEventQuery("SELECT * FROM Win32_DeviceChangeEvent");
        _watcher = new ManagementEventWatcher("root\\CIMV2", query.QueryString);
        _watcher.EventArrived += (_, __) => SignalChange();
        _watcher.Start();

        _log.LogInformation("WMI device-change watcher started.");
    }

    private void StopDeviceWatcher()
    {
        try
        {
            if (_watcher is null) return;
            _watcher.Stop();
            _watcher.Dispose();
            _watcher = null;
        }
        catch { /* best-effort */ }
    }

    private void SignalChange()
    {
        lock (_gate)
        {
            _pending = true;
            _lastSignal = DateTimeOffset.UtcNow;
        }
    }

    private bool ShouldPublishNow()
    {
        lock (_gate)
        {
            if (!_pending) return false;
            var elapsedMs = (DateTimeOffset.UtcNow - _lastSignal).TotalMilliseconds;
            if (elapsedMs < _debounceMs) return false;

            _pending = false;
            return true;
        }
    }

    private async Task PublishNowAsync(CancellationToken ct)
    {
        await _mqtt.EnsureConnectedAsync(ct);

        var ports = _scanner.GetPorts();
        _log.LogInformation("Publishing {Count} serial ports.", ports.Count);

        await _mqtt.PublishStateAsync(Environment.MachineName, ports, ct);
    }
}
