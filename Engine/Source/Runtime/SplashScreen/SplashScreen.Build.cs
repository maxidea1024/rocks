using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

// Startup splash screen (Docs/Design/SplashScreen): visibility decision, splash image lookup, text slot layout, progress model and the
// platform splash window that a dedicated thread owns. Hosts call SplashScreen::Show after config and logging start and Hide after the
// main window shows its first frame. ClientOnly: server targets have no window (the ui.splash.* settings live in the Engine module so
// that server targets still accept them). Private/Common is platform neutral; Windows uses Private/Win64 (WIC, GDI and DWM), other
// platforms Private/Unsupported until their presenter exists (macOS is stage SP3).
public sealed class SplashScreen : EngineModuleRules
{
    protected override string ModuleName => "SplashScreen";

    protected override ModuleType ModuleType => ModuleType.ClientOnly;

    // DesktopSessionState appears in the public visibility input. Progress (with Localization): StartupProgress (Docs/Design/SplashScreen
    // sections 7 and 8) opens the startup task and its step scope, and its splash source pulls ProgressHub snapshots.
    protected override string[] PublicDependencies => ["Core", "PlatformWindow", "Progress"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        bool windows = context.Platform == NativePlatform.Windows;
        return new()
        {
            Name = ModuleName,
            ModuleDirectory = ModuleDirectory,
            Sources = [ModuleEntrySource(context), .. context.Sources($"{backend}/Common", windows ? $"{backend}/Win64" : $"{backend}/Unsupported")],
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            PublicDependencies = PublicDependencies,
            // user32 and gdi32 draw the window, ole32 and windowscodecs decode the image (WIC), dwmapi rounds the Windows 11 corners and
            // shcore answers the monitor DPI (GetDpiForMonitor).
            SystemLibraries = windows ? ["user32", "gdi32", "ole32", "windowscodecs", "dwmapi", "shcore"] : []
        };
    }
}
