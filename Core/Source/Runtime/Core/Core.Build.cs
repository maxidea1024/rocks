using Rocks.Build;
using Rocks.Build.Rules;

// 컨테이너·할당기·문자열·수학·진단·시간(UE Core 대응). 창·입력 의존 금지.
public sealed class Core : CoreModuleRules
{
    protected override string ModuleName => "Core";

    protected override string[] RequiredModules => ["PlatformBase", "DeterministicMath"];

    // Core/Build/BuildInfo.h(rocks::build::ToolchainFingerprint·ToolBuildId)를 제공한다. 이 모듈에 닿는 실행 파일마다 빌드가 정의
    // TU를 만들어 더한다(Docs/Design/ReflectionFoundations.ko.md 8.1절).
    public override NativeModuleInput Configure(BuildContext context) => base.Configure(context) with { ProvidesBuildInfo = true };
}
