using Rocks.Build;
using Rocks.Build.Rules;

// OpenGL 4.5 core graphics backend (Windows WGL). On other platforms device creation is Unsupported.
public sealed class OpenGLGraphics : EngineModuleRules
{
    protected override string ModuleName => "OpenGLGraphics";

    protected override string[] RequiredModules => ["Graphics"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        bool windows = context.Platform == NativePlatform.Windows;
        return new()
        {
            Name = ModuleName,
            Sources = context.Sources(windows ? $"{backend}/Win64" : $"{backend}/Unsupported"),
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            Dependencies = RequiredModules,
            // GL 1.1 entry points and WGL come from opengl32; the rest are obtained at run time with wglGetProcAddress.
            SystemLibraries = windows ? ["opengl32", "gdi32", "user32"] : []
        };
    }
}
