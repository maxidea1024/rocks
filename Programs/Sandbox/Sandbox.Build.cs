using Rocks.Build;
using Rocks.Build.Rules;

// 엔진 수직 경로 확인용 실행 파일이다. 창·입력·그래픽 장치(D3D12/OpenGL)·Engine·World를 연결한다.
// --self-test는 창 없이 offscreen 렌더 결과를 검사한다.
public sealed class SandboxProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "Sandbox",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/Sandbox/Private"),
        PrivateIncludeDirectories = [context.PathInRoot("Programs/Sandbox/Private")],
        Dependencies = ["Engine", "PlatformWindow", "Renderer", "D3D12Graphics", "OpenGLGraphics"]
    };
}
