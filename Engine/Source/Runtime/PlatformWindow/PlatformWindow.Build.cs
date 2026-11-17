using Rocks.Build;
using Rocks.Build.Rules;

// Window creation, event pump and input backend (keyboard, mouse and gamepad). The public interface is platform independent and
// implementations live in Private/<Platform>/. Knows nothing about graphics or World (exposes only native window handles).
public sealed class PlatformWindow : EngineModuleRules
{
    protected override string ModuleName => "PlatformWindow";

    protected override string[] RequiredModules => ["Input"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        bool windows = context.Platform == NativePlatform.Windows;
        return new()
        {
            Name = ModuleName,
            // Windows uses Win32 windows and XInput; other platforms have no window backend yet and return Unsupported.
            Sources = context.Sources(windows ? $"{backend}/Win64" : $"{backend}/Unsupported"),
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            Dependencies = RequiredModules,
            // XInput is loaded dynamically at run time (the SDK's xinput.lib makes lld-link warn about a DllMain import).
            // wtsapi32 answers the desktop session connect state (QueryDesktopSessionState).
            SystemLibraries = windows ? ["user32", "wtsapi32"] : []
        };
    }
}
