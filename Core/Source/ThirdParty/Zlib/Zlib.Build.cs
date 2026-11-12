using Rocks.Build.Rules;

// zlib 1.4 고정 버전(Zlib). Deflate·Zlib·Gzip 압축·해제(Z_SOLO: 기본 할당기·gz 파일 IO 제외).
// 소스·정의·라이선스·파일 해시는 같은 폴더의 ThirdParty.json이 단일 출처다(Docs/Design/Compression.ko.md 3절).
public sealed class Zlib : ThirdPartyModuleRules
{
    protected override string ModuleName => "Zlib";
}
