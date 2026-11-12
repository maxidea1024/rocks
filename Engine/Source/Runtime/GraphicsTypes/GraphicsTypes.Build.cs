using Rocks.Build.Rules;

// 픽셀 형식·샘플러·mip 크기 같은 그래픽 값 타입이다. 장치 계약(Graphics)·에셋(AssetRuntime)·도구가 함께
// 쓴다. 장치·창·World를 참조하지 않는다.
public sealed class GraphicsTypes : EngineModuleRules
{
    protected override string ModuleName => "GraphicsTypes";
    protected override string[] RequiredModules => ["Core"];
}
