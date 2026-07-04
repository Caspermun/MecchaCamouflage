using System.Diagnostics;
using System.Security.Cryptography;
using MecchaCamouflage.Core;

namespace MecchaCamouflage.Controller;

public sealed class RuntimeBridgeService
{
    public const int BridgePort = 50262;
    private static readonly int[] BridgePortCandidates = [BridgePort, 50263, 50264, 50265];

    private readonly AppPaths paths;
    private readonly RuntimeLog log;

    private string bridgePath = "";
    private string injectorPath = "";
    private string progressPath = "";
    private string waitingForProcessName = "";
    private int lastInjectionProcessId;
    private int activeBridgePort = BridgePort;
    private bool bridgeReadyTimeoutLogged;

    public RuntimeBridgeService(AppPaths paths, RuntimeLog log)
    {
        this.paths = paths;
        this.log = log;
    }

    public string BridgePath => bridgePath;
    public string ProgressPath => progressPath;

    public Process? FindGameProcess(string processName)
    {
        var name = Path.GetFileNameWithoutExtension(processName);
        return Process.GetProcessesByName(name).FirstOrDefault();
    }

    public Task<BridgeReply> PingAsync(CancellationToken cancellationToken = default) =>
        Client(activeBridgePort).PingAsync(cancellationToken);

    public Task<BridgeReply> CancelPaintAsync(CancellationToken cancellationToken = default) =>
        Client(activeBridgePort).CancelPaintAsync(cancellationToken);

    public Task<BridgeReply> SendPaintAsync(string payload, CancellationToken cancellationToken = default) =>
        Client(activeBridgePort).RequestAsync(payload, cancellationToken);

    public Task<BridgeReply> ShutdownAsync(CancellationToken cancellationToken = default) =>
        Client(activeBridgePort).ShutdownAsync(cancellationToken);

    public async Task<bool> EnsureReadyAsync(string processName, CancellationToken cancellationToken = default)
    {
        var process = FindGameProcess(processName);
        if (process is null)
        {
            if (!string.Equals(waitingForProcessName, processName, StringComparison.OrdinalIgnoreCase))
            {
                log.Warn($"Waiting for process {processName}.");
                waitingForProcessName = processName;
            }
            return false;
        }
        waitingForProcessName = "";

        var occupiedPorts = new HashSet<int>();
        foreach (var port in OrderedPortCandidates())
        {
            var ping = await Client(port).PingAsync(cancellationToken);
            if (IsBridgeReadyForProcess(ping, process.Id))
            {
                activeBridgePort = port;
                bridgeReadyTimeoutLogged = false;
                waitingForProcessName = "";
                return true;
            }
            if (ping.Ok && ping.Success && ping.ProcessId is not null && ping.ProcessId != process.Id)
                occupiedPorts.Add(port);
        }

        foreach (var port in BridgePortCandidates)
        {
            if (occupiedPorts.Contains(port))
                continue;
            if (await TryInjectAndWaitAsync(processName, process, port, cancellationToken))
            {
                activeBridgePort = port;
                bridgeReadyTimeoutLogged = false;
                return true;
            }
        }
        if (!bridgeReadyTimeoutLogged)
        {
            log.Warn("Bridge did not become ready after injection.");
            bridgeReadyTimeoutLogged = true;
        }
        return false;
    }

    private async Task<bool> TryInjectAndWaitAsync(string processName, Process process, int port, CancellationToken cancellationToken)
    {
        try
        {
            PrepareNativeRuntime(port);
            File.WriteAllText(bridgePath + ".port", port + Environment.NewLine);
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or IOException)
        {
            log.Error("Bridge runtime files could not be prepared: " + FriendlyAccessFailure(ex.Message));
            return false;
        }
        if (lastInjectionProcessId != process.Id || activeBridgePort != port)
        {
            log.Info($"Injecting bridge into {process.ProcessName}.exe.");
            lastInjectionProcessId = process.Id;
        }
        try
        {
            var start = new ProcessStartInfo(injectorPath, Quote(processName) + " " + Quote(bridgePath))
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardError = true,
                RedirectStandardOutput = true
            };
            using var injector = Process.Start(start);
            if (injector is null)
                return false;
            await injector.WaitForExitAsync(cancellationToken);
            _ = await injector.StandardOutput.ReadToEndAsync(cancellationToken);
            var stderr = await injector.StandardError.ReadToEndAsync(cancellationToken);
            if (injector.ExitCode != 0)
            {
                log.Error($"Bridge injection failed ({injector.ExitCode}): {FriendlyInjectorFailure(injector.ExitCode, stderr)}");
                return false;
            }
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or System.ComponentModel.Win32Exception)
        {
            log.Error("Bridge injector could not run: " + FriendlyAccessFailure(ex.Message));
            return false;
        }

        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(5);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var ready = await Client(port).PingAsync(cancellationToken);
            if (IsBridgeReadyForProcess(ready, process.Id))
                return true;
            await Task.Delay(250, cancellationToken);
        }
        return false;
    }

    private void PrepareNativeRuntime(int port)
    {
        paths.EnsureBaseDirectories();
        var appDir = PackagedAssets.ResolveAssetRoot(paths, "native");
        var packagedNativeDir = Path.Combine(appDir, "native");
        var packagedBridge = Path.Combine(packagedNativeDir, "runtime-bridge.dll");
        var packagedInjector = Path.Combine(packagedNativeDir, "runtime-injector.exe");
        if (!File.Exists(packagedBridge))
            packagedBridge = Path.Combine(appDir, "runtime-bridge.dll");
        if (!File.Exists(packagedInjector))
            packagedInjector = Path.Combine(appDir, "runtime-injector.exe");
        if (!File.Exists(packagedBridge) || !File.Exists(packagedInjector))
            throw new FileNotFoundException("Packaged native bridge or injector is missing.");

        var runtimeDir = paths.RuntimeHashDirectory(packagedBridge, packagedInjector);
        Directory.CreateDirectory(runtimeDir);
        injectorPath = Path.Combine(runtimeDir, "runtime-injector.exe");
        CopyIfMissing(packagedInjector, injectorPath);

        var hash = ShortHash(packagedBridge);
        bridgePath = Path.Combine(runtimeDir, $"runtime-bridge-{hash}-{port}.dll");
        CopyIfMissing(packagedBridge, bridgePath);
        Directory.CreateDirectory(paths.ProgressDirectory);
        progressPath = Path.Combine(paths.ProgressDirectory, $"bridge-{hash}-{port}.progress.json");
        File.WriteAllText(bridgePath + ".progress.path", progressPath + Environment.NewLine);

        var profileRoot = PackagedAssets.ResolveAssetRoot(paths, "mesh-profiles");
        var sourceProfiles = Path.Combine(profileRoot, "mesh-profiles");
        var targetProfiles = Path.Combine(runtimeDir, "mesh-profiles");
        if (Directory.Exists(sourceProfiles))
        {
            Directory.CreateDirectory(targetProfiles);
            foreach (var file in Directory.EnumerateFiles(sourceProfiles, "*.json"))
                File.Copy(file, Path.Combine(targetProfiles, Path.GetFileName(file)), true);
        }
        paths.CleanupRuntimeBinDirectories(runtimeDir, TimeSpan.FromDays(14), keepNewest: 3);
    }

    private IEnumerable<int> OrderedPortCandidates()
    {
        yield return activeBridgePort;
        foreach (var port in BridgePortCandidates)
        {
            if (port != activeBridgePort)
                yield return port;
        }
    }

    private static bool IsBridgeReadyForProcess(BridgeReply reply, int processId) =>
        reply.Ok &&
        reply.Success &&
        (reply.ProcessId is null || reply.ProcessId == processId);

    private static BridgeClient Client(int port) => new(port: port);

    private static string FriendlyInjectorFailure(int exitCode, string stderr)
    {
        var message = stderr.Trim();
        var lower = message.ToLowerInvariant();
        if (exitCode is 2 or 5 ||
            lower.Contains("access is denied") ||
            lower.Contains("access denied") ||
            lower.Contains("win32=5"))
        {
            return "access denied while opening or injecting into the game process. Run Meccha Camouflage with the same privileges as the game, or try Run as administrator.";
        }
        if (string.IsNullOrWhiteSpace(message))
            return "injector exited without an error message.";
        return message;
    }

    private static string FriendlyAccessFailure(string message)
    {
        var lower = message.ToLowerInvariant();
        if (lower.Contains("access") || lower.Contains("permission") || lower.Contains("denied"))
            return message + " Run Meccha Camouflage with access to its runtime folder.";
        return message;
    }

    private static void CopyIfMissing(string source, string target)
    {
        if (File.Exists(target))
            return;
        File.Copy(source, target, false);
    }

    private static string ShortHash(string path)
    {
        using var sha = SHA256.Create();
        var hash = sha.ComputeHash(File.ReadAllBytes(path));
        return Convert.ToHexString(hash).ToLowerInvariant()[..16];
    }

    private static string Quote(string value) => "\"" + value.Replace("\"", "\\\"") + "\"";
}
