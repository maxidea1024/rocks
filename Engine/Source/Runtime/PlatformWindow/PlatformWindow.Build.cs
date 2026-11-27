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
        bool android = context.Platform == NativePlatform.Android;
        string platformDirectory = windows ? "Win64" : android ? "Android" : "Unsupported";
        return new()
        {
            Name = ModuleName,
            ModuleDirectory = ModuleDirectory,
            Unity = UnityFor(context),
            UnityExclude = UnityExclude(context),
            // Windows uses Win32 windows and XInput, Android the activity window of the Launch activity layer
            // (Docs/Design/AndroidPlatform.ko.md); other platforms have no window backend yet and return Unsupported.
            // The entry point (Private/PlatformWindowModule.cpp) is built on every platform.
            // Private/Common holds platform-neutral input conversions (tested on every platform).
            Sources = [ModuleEntrySource(context), .. context.Sources($"{backend}/Common", $"{backend}/{platformDirectory}")],
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            PublicDependencies = PublicDependencies,
            // The Android window listens to the Launch activity layer (launch::android::SetActivityListener).
            PrivateDependencies = android ? ["Launch"] : [],
            // XInput is loaded dynamically at run time (the SDK's xinput.lib makes lld-link warn about a DllMain import).
            // wtsapi32 answers the desktop session connect state (QueryDesktopSessionState), gdi32 builds the drag image
            // window bitmap, imm32 records IME composition for the text input context (Win64DragImage, Win64Window), dwmapi keeps
            // the drop shadow of custom-frame windows (WindowFrame::Custom) and shell32 finds auto-hide taskbars so a
            // maximized custom-frame window leaves them a pixel.
            // libandroid holds ANativeWindow, ALooper and AInputQueue.
            SystemLibraries = windows ? ["user32", "wtsapi32", "gdi32", "imm32", "dwmapi", "shell32"] : android ? ["android"] : []
        };
    }
}
