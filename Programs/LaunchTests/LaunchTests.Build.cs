using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Test executables of the common entry layer (Launch, Docs/Design/CrashHandlingAndLaunch.ko.md LC3). Rocks.ToolTests starts them as
/// children (Native.Launch.*) to check Unicode arguments, start and exit, initialization failure cleanup, exit codes, the Windows GUI
/// subsystem and the embedded host mode. Every executable shares the probe body in Private/Common.
/// </summary>
public abstract class LaunchTestProgram : ModuleRules
{
    protected const string Root = "Programs/LaunchTests/Private";

    protected abstract string ExecutableName { get; }

    protected abstract string[] EntrySources(BuildContext context);

    protected virtual WindowsSubsystem Subsystem => WindowsSubsystem.Console;

    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = ExecutableName,
        Kind = NativeModuleKind.Executable,
        // TerminationSignal.cpp: the real OS termination event the probe sends to itself (console Ctrl-Break, SIGINT).
        Sources = [.. context.Sources($"{Root}/Common"), .. EntrySources(context).Select(context.PathInRoot),
            context.PathInRoot(context.Platform == NativePlatform.Windows ? $"{Root}/Win64/TerminationSignal.cpp" : $"{Root}/Posix/TerminationSignal.cpp")],
        PrivateIncludeDirectories = [context.PathInRoot($"{Root}/Common")],
        WindowsSubsystem = Subsystem,
        PublicDependencies = ["Launch"]
    };
}

// Console probe: ROCKS_IMPLEMENT_APPLICATION (wmain on Windows).
public sealed class LaunchProbe : LaunchTestProgram
{
    protected override string ExecutableName => "LaunchProbe";

    protected override string[] EntrySources(BuildContext context) => [$"{Root}/Console/ConsoleEntry.cpp"];
}

// Windows GUI subsystem probe: ROCKS_IMPLEMENT_WINDOWED_APPLICATION (wWinMain, no console window).
public sealed class LaunchWindowedProbe : LaunchTestProgram
{
    protected override string ExecutableName => "LaunchWindowedProbe";

    protected override string[] EntrySources(BuildContext context) => [$"{Root}/Windowed/WindowedEntry.cpp"];

    protected override WindowsSubsystem Subsystem => WindowsSubsystem.Windows;
}

// Embedded host: owns its own platform entry and unhandled crash handling, and calls RunApplication with CrashReportingMode.Embedded.
public sealed class LaunchEmbeddedHost : LaunchTestProgram
{
    protected override string ExecutableName => "LaunchEmbeddedHost";

    protected override string[] EntrySources(BuildContext context) =>
        [context.Platform == NativePlatform.Windows ? $"{Root}/Win64/EmbeddedHost.cpp" : $"{Root}/Posix/EmbeddedHost.cpp"];
}
