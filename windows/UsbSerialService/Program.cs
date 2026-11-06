namespace Bergdahl.UsbSerialService;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Hosting.WindowsServices;
using Microsoft.Extensions.Logging;

public class Program
{
    public static void Main(string[] args)
    {
        var forceConsole = args.Contains("--console", StringComparer.OrdinalIgnoreCase);
        var isService = !forceConsole && WindowsServiceHelpers.IsWindowsService();

        var builder = Host.CreateApplicationBuilder(args);

        // Activate Windows service lifetime only when started by SCM
        builder.Host.UseWindowsService(options =>
        {
            options.ServiceName = "Serial Port MQTT Publisher";
        });

        // Logging per environment
        if (isService)
        {
            builder.Logging.AddEventLog();
        }
        else
        {
            builder.Logging.AddSimpleConsole(o =>
            {
                o.SingleLine = true;
                o.TimestampFormat = "HH:mm:ss ";
            });
        }

        // Service options (name, etc.)
        builder.Services.AddWindowsService(options =>
        {
            options.ServiceName = "Serial Port MQTT Publisher";
        });

        builder.Services.AddSingleton<SerialPortScanner>();
        builder.Services.AddSingleton<MqttPublisher>();
        builder.Services.AddHostedService<Worker>();

        builder.Build().Run();
    }
}


