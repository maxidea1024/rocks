using Rocks.Build;
using Rocks.Build.Rules;

// OpenGL ES 3.2 graphics backend (ShaderSystem P11, Docs/Design/OpenGLESGraphics.ko.md). Private/Common holds the
// device on the ES 3.2 core API only; Private/<Platform> holds the platform layer: Win64 is the GLES31_PC preview over
// WGL (ES profile or desktop core context), Android is EGL. Other platforms build Private/Unsupported and device
// creation is Unsupported.
public sealed class OpenGLGraphics : EngineModuleRules
{
    protected override string ModuleName => "OpenGLGraphics";

    protected override string[] RequiredModules => ["Graphics"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        return new()
        {
            Name = ModuleName,
            Sources = context.Platform switch
            {
                NativePlatform.Windows => context.Sources($"{backend}/Win64", $"{backend}/Common"),
                NativePlatform.Android => context.Sources($"{backend}/Android", $"{backend}/Common"),
                _ => context.Sources($"{backend}/Unsupported")
            },
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            Dependencies = RequiredModules,
            // Windows: WGL and the GL 1.0/1.1 entry points come from opengl32; every other GL function is obtained at
            // run time with wglGetProcAddress. Android: EGL and GLESv3 are NDK system libraries; GL functions are
            // obtained with eglGetProcAddress (dlsym fallback).
            SystemLibraries = context.Platform switch
            {
                NativePlatform.Windows => ["opengl32", "gdi32", "user32"],
                NativePlatform.Android => ["EGL", "GLESv3"],
                _ => []
            }
        };
    }
}
