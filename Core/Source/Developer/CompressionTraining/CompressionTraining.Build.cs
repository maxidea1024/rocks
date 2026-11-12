using Rocks.Build;
using Rocks.Build.Rules;

// zstd 사전 학습(도구 전용, Docs/Design/Compression.ko.md 11.6절). zstd dictBuilder는 CRT malloc을 직접 쓰므로(승인된
// 좁은 예외: 학습 시점만) 게임 런타임이 아니라 쿠커·도구가 링크한다. zdict.h를 include하는 유일한 모듈이다.
public sealed class CompressionTraining : CoreModuleRules
{
    private static readonly string[] Codecs = ["Zstd"];

    protected override string Group => "Developer";
    protected override string ModuleName => "CompressionTraining";
    protected override string[] RequiredModules => ["Core", "Compression", .. Codecs];

    protected override IReadOnlyList<string> PrivateIncludeDirectories(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerIncludeDirectories(context, Codecs);

    protected override IReadOnlyList<string> PrivateDefines(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerDefines(context, Codecs);
}
