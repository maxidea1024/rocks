using Rocks.Build;
using Rocks.Build.Rules;

// Window creation, event pump and input backend (keyboard, mouse and gamepad). The public interface is platform independent and
// implementations live in Private/<Platform>/. Knows nothing about graphics or World (exposes only native window handles).
public sealed class PlatformWindow : EngineModuleRules
{
    protected override string ModuleName => "PlatformWindow";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Input"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        bool windows = context.Platform == NativePlatform.Windows;
        bool mac = context.Platform == NativePlatform.MacOS;
        string platformDirectory = windows ? "Win64" : mac ? "Mac" : "Unsupported";
        return new()
        {
            Name = ModuleName,
            ModuleDirectory = ModuleDirectory,
            Unity = UnityFor(context),
            UnityExclude = UnityExclude(context),
            // Windows uses Win32 windows and XInput, macOS Cocoa windows and GameController (Docs/Design/MacPlatform.ko.md);
            // other platforms have no window backend yet and return Unsupported. The entry point (Private/PlatformWindowModule.cpp)
            // and the platform-neutral input conversions (Private/Common, tested on every platform) are built everywhere.
            Sources = [ModuleEntrySource(context), .. context.Sources($"{backend}/Common", $"{backend}/{platformDirectory}")],
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            PublicDependencies = PublicDependencies,
            // The macOS main thread host forwards Cmd+Q and logout to launch::RequestTermination.
            PrivateDependencies = mac ? ["Launch"] : [],
            // XInput is loaded dynamically at run time (the SDK's xinput.lib makes lld-link warn about a DllMain import).
            // wtsapi32 answers the desktop session connect state (QueryDesktopSessionState).
            SystemLibraries = windows ? ["user32", "wtsapi32"] : [],
            // Cocoa windows and events, QuartzCore for the layer-backed view, GameController for gamepads.
            Frameworks = mac ? ["Cocoa", "QuartzCore", "GameController"] : []
        };
    }
}
