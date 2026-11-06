namespace Bergdahl.UsbSerialService;

using System.Management;
using System.Text.RegularExpressions;

public sealed record SerialPortInfo(
    string Port,          // "COM7"
    string Name,          // "USB-SERIAL CH340 (COM7)"
    string? PnpDeviceId,  // full PNPDeviceID
    string? Vid,
    string? Pid,
    string? Manufacturer
);

public sealed class SerialPortScanner
{
    private static readonly Regex VidPidRegex =
        new(@"VID_([0-9A-Fa-f]{4}).*PID_([0-9A-Fa-f]{4})", RegexOptions.Compiled);

    public IReadOnlyList<SerialPortInfo> GetPorts()
    {
        var results = new List<SerialPortInfo>();

        // Win32_SerialPort gives you COM + friendly Name for classic serial & many USB-serial adapters
        using var searcher = new ManagementObjectSearcher(
            "root\\CIMV2",
            "SELECT DeviceID, Name, PNPDeviceID, Manufacturer FROM Win32_SerialPort"
        );

        foreach (ManagementObject mo in searcher.Get())
        {
            var deviceId = (mo["DeviceID"] as string)?.Trim(); // e.g., "COM7"
            if (string.IsNullOrWhiteSpace(deviceId))
                continue;

            var name = (mo["Name"] as string)?.Trim() ?? deviceId;
            var pnp = (mo["PNPDeviceID"] as string)?.Trim();
            var manufacturer = (mo["Manufacturer"] as string)?.Trim();

            string? vid = null;
            string? pid = null;

            if (!string.IsNullOrWhiteSpace(pnp))
            {
                var m = VidPidRegex.Match(pnp);
                if (m.Success)
                {
                    vid = m.Groups[1].Value.ToUpperInvariant();
                    pid = m.Groups[2].Value.ToUpperInvariant();
                }
            }

            results.Add(new SerialPortInfo(
                Port: deviceId,
                Name: name,
                PnpDeviceId: pnp,
                Vid: vid,
                Pid: pid,
                Manufacturer: manufacturer
            ));
        }

        // Sort COM ports naturally (COM2 before COM10)
        return results
            .OrderBy(p => TryParseComNumber(p.Port))
            .ThenBy(p => p.Port, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    private static int TryParseComNumber(string port)
    {
        // "COM12" -> 12; non-matching -> large number
        if (port.StartsWith("COM", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(port.AsSpan(3), out var n))
            return n;

        return int.MaxValue;
    }
}
