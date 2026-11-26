#pragma once

#include <AssetRuntime/BulkData.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>

// SoundClip 필드에 쓰는 enum·struct다(Docs/Design/AudioSystem 3.36.2절).
namespace rocks {

// 원본 코덱이다(3.10.1절 — 손실 원본을 다시 손실 코덱으로 쿠킹하면 세대 손실이 생기므로 남긴다).
ROCKS_ENUM()
enum class SoundSourceCodec : uint8 {
    Pcm = 0,
    Flac = 1,
    Vorbis = 2,
    Opus = 3,
    Mp3 = 4
};

// 원본 bulk의 표본 저장 형식이다(3.10.1절 저장 규칙). planar, little-endian, 헤더 없음.
ROCKS_ENUM()
enum class SoundSourceStorage : uint8 {
    // 원본 없음.
    None = 0,
    // 8·16비트 정수 원본.
    Int16 = 1,
    // 24비트 정수 원본(3 byte packed).
    Int24 = 2,
    // 32비트 정수·float·손실 원본, downmix·normalize한 결과.
    Float32 = 3
};

// 저장 형식별 표본 byte 수와 정수 형식의 full scale(2^(비트−1) — 임포터가 곱하고 에디터 재생 PCM
// 변환이 나눈다, 3.36.3절)·24비트 부호 비트다. 양자화와 역변환이 같은 값을 쓴다.
inline constexpr uint32 SoundSourceInt16Bytes = 2;
inline constexpr uint32 SoundSourceInt24Bytes = 3;
inline constexpr uint32 SoundSourceFloat32Bytes = 4;
inline constexpr float SoundSourceInt16FullScale = 32768.0f;
inline constexpr float SoundSourceInt24FullScale = 8388608.0f;
inline constexpr int32 SoundSourceInt16Min = -32768;
inline constexpr int32 SoundSourceInt16Max = 32767;
inline constexpr int32 SoundSourceInt24Min = -8388608;
inline constexpr int32 SoundSourceInt24Max = 8388607;
inline constexpr uint32 SoundSourceInt16Mask = 0xFFFFu;
inline constexpr uint32 SoundSourceInt24Mask = 0xFFFFFFu;
inline constexpr uint32 SoundSourceInt24SignBit = 0x800000u;
inline constexpr uint32 SoundSourceInt24SignExtension = 0xFF000000u;

// 표본 하나의 byte 수다(None은 0).
ROCKS_AUDIOASSETS_API uint32 SoundSourceStorageBytes(SoundSourceStorage storage) noexcept;

// 파일 cue·marker다(UE FSoundWaveCuePoint 대응 — 파일 loop 구간은 hasFileLoop 필드로 따로 둔다).
ROCKS_STRUCT()
struct SoundCuePoint {
    ROCKS_GENERATED_BODY()

    explicit SoundCuePoint(Allocator& allocator) noexcept : name(allocator) {
    }

    ROCKS_PROPERTY() uint32 id = 0;
    ROCKS_PROPERTY() uint32 frame = 0;
    // marker 이름(파일 `LIST adtl labl` — 없으면 빈 값)이다.
    ROCKS_PROPERTY() String name;
};

// 사용자 loop 구간이다(설정 — 재임포트가 덮지 않는다, 3.10.2절). enabled면 파일 loop보다 앞선다.
ROCKS_STRUCT()
struct SoundLoopOverride {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() bool enabled = false;
    ROCKS_PROPERTY() uint32 begin = 0;
    ROCKS_PROPERTY() uint32 end = 0;
};

// 에디터 원본이다(EditorOnly — 쿠킹 패키지에 넣지 않는다). 디코드한 PCM을 채널마다 이어 담는다.
ROCKS_STRUCT()
struct SoundClipSource {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() SoundSourceStorage format = SoundSourceStorage::None;
    ROCKS_PROPERTY() BulkDataRef bulk;
};

} // namespace rocks
