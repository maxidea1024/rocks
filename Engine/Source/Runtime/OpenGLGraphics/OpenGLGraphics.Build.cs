using Rocks.Build;
using Rocks.Build.Rules;

// OpenGL 4.5 core 그래픽 backend(Windows WGL)다. 다른 플랫폼에서는 장치 생성이 Unsupported다.
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
            // GL 1.1 진입점과 WGL은 opengl32에서, 나머지는 wglGetProcAddress로 실행 시 얻는다.
            SystemLibraries = windows ? ["opengl32", "gdi32", "user32"] : []
        };
    }
}
