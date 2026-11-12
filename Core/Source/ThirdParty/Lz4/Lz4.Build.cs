using Rocks.Build.Rules;

// LZ4 1.11.1 고정 버전(BSD-2-Clause). LZ4 raw block 압축·해제. lz4.c·lz4hc.c를 Lz4Unity.c 한 번역 단위로 빌드한다.
// 소스·정의·라이선스·파일 해시는 같은 폴더의 ThirdParty.json이 단일 출처다(Docs/Design/Compression.ko.md 3절).
public sealed class Lz4 : ThirdPartyModuleRules
{
    protected override string ModuleName => "Lz4";
}
