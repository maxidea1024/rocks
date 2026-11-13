using Rocks.Build.Rules;

// Brotli 1.1.1 고정 버전(MIT). 해제기만(c/common·c/dec) 빌드하고 압축기·도구는 들이지 않는다
// (Godot Compression의 Brotli 해제 대응, WOFF2 글꼴 등). 소스·정의·라이선스·파일 해시는 같은 폴더의
// ThirdParty.json이 단일 출처다(Docs/Design/Compression.ko.md 3·13절).
public sealed class Brotli : ThirdPartyModuleRules
{
    protected override string ModuleName => "Brotli";
}
