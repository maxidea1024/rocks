using Rocks.Build.Rules;

// 그래픽 장치 계약(장치·스왑체인·버퍼·텍스처·파이프라인·명령 기록)이다. backend 구현은
// D3D12Graphics·OpenGLGraphics 모듈이 맡는다. Vulkan·Metal은 Backend 열거값으로만 자리를 두고, 구현할 때
// 모듈을 추가한다(엔진 Runtime에는 실제로 쓰는 모듈만 둔다). World·Object·창 모듈을 모른다(창은
// NativeSurface 핸들로만 받는다).
public sealed class Graphics : EngineModuleRules
{
    protected override string ModuleName => "Graphics";
    protected override string[] RequiredModules => ["Core"];
}
