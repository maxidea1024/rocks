using Rocks.Build;
using Rocks.Build.Rules;

// 범용 암호 모듈(Docs/Design/Crypto.ko.md): libsodium 기반 primitive·스트리밍·키 관리와 HW 전용 AES 경량 계층.
// 외부 라이브러리(Core/Source/ThirdParty/Sodium)의 헤더·정의는 이 모듈의 Private 구현에만 노출한다. 공개 헤더는 sodium.h를
// 포함하지 않는다. 클래스 이름은 System.Security.Cryptography와 헷갈리지 않도록 CryptoModule이다(CompressionModule과 같은 방식).
public sealed class CryptoModule : CoreModuleRules
{
    protected override string ModuleName => "Crypto";

    protected override string[] RequiredModules => ["Core", "Sodium"];

    protected override IReadOnlyList<string> PrivateIncludeDirectories(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerIncludeDirectories(context, "Sodium");

    protected override IReadOnlyList<string> PrivateDefines(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerDefines(context, "Sodium");
}
