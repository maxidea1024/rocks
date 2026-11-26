#pragma once

#include <AssetDatabase/AssetImporter.h>
#include <Core/Containers/Array.h>

// SoundClip 임포터다(Docs/Design/AudioSystem 3.10·3.36.3절 — AU4c, UE USoundFactory 대응). 원본
// (WAV·AIFF·FLAC·Ogg Vorbis·Ogg Opus·MP3)을 디코드해 PCM bulk(Int16·Int24·Float32 planar)와 임포트
// 결과 필드를 쓴다. 사용자 설정(looping·loopOverride·volume 등)은 건드리지 않는다.
//
// 사용 예(제품 호스트 — 텍스처와 한 칸에 묶어 등록):
//   SoundClipImporter soundImporter;
//   (void)RegisterSoundClipImporter(&soundImporter, table, tokens);
namespace rocks {

// 임포터 이름(`--importer=` 값·기록 importerName)과 버전이다. 결과 byte가 바뀌면 버전을 올린다.
inline constexpr StringView SoundClipImporterName = "sound_clip";
inline constexpr uint32 SoundClipImporterVersion = 1;

class ROCKS_AUDIOTOOLS_API SoundClipImporter final : public AssetImporter {
public:
    SoundClipImporter() noexcept;

    const AssetImporterInfo& Info() const noexcept override {
        return info_;
    }

    // 3.36.3절 순서: 디코더 선택(ogg·opus는 첫 page 식별 header) → 디코드 → downmix → normalize →
    // 저장 형식 → bulk·필드. 오류: 디코드 실패 InvalidSource·UnsupportedSourceFormat 진단, 범위 밖
    // 옵션 InvalidImportOption, 할당 실패.
    Result<void> Import(AssetImportContext& context) noexcept override;

private:
    AssetImporterInfo info_;
};

// state(SoundClipImporter*)를 table에 등록하고 토큰을 tokens에 둔다(commandlet_host::
// AssetImporterSetup 모양). 오류: null state InvalidArgument, 등록 오류(SoundClip 타입이 등록되지
// 않았으면 NotFound), 할당 실패.
ROCKS_AUDIOTOOLS_API Result<void>
RegisterSoundClipImporter(void* state, AssetImporterTable& table,
                          Array<AssetImporterRegistration>& tokens) noexcept;

} // namespace rocks
