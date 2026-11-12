using Rocks.Build.Rules;

// 내장 메시·셰이더로 RenderScene(평범한 데이터 목록)을 그린다. World를 순회하지 않는다(장면 추출은
// WorldIntegration이 맡는다).
public sealed class Renderer : EngineModuleRules
{
    protected override string ModuleName => "Renderer";
    protected override string[] RequiredModules => ["Graphics"];
}
