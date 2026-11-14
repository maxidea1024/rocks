using Rocks.Build.Rules;

// libsodium 1.0.22-stable 고정 commit(ISC). Crypto 모듈의 유일한 외부 암호 라이브러리다(Docs/Design/Crypto.ko.md 5.1절).
// 소스·정의·라이선스·파일 해시·추적 가능한 수정은 같은 폴더의 ThirdParty.json·Rocks.patch가 단일 출처다.
public sealed class Sodium : ThirdPartyModuleRules
{
    protected override string ModuleName => "Sodium";
}
