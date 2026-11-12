using Rocks.Build.Rules;

// Zstandard 1.5.7 고정 버전(BSD-3-Clause 선택, 사전 생성기의 divsufsort는 MIT). zstd frame 압축·해제와 사전 학습
// (dictBuilder, 도구 전용)을 담는다(단일 스레드, legacy 제외).
// 소스·정의·라이선스·파일 해시는 같은 폴더의 ThirdParty.json이 단일 출처다(Docs/Design/Compression.ko.md 3절).
public sealed class Zstd : ThirdPartyModuleRules
{
    protected override string ModuleName => "Zstd";
}
