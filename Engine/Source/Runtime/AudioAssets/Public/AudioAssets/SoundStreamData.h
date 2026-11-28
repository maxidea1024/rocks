#pragma once

#include <Audio/AudioPlatformData.h>
#include <AudioAssets/SoundStreamedPayload.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/SharedPtr.h>

// 실시간 디코드(스트리밍)용 공유 불변 압축 데이터다(Docs/Design/AudioSystem 3.40.3·3.46.3절). 두
// 형태다.
// - 상주(TryCreate): 쿠킹 플랫폼 데이터 전체 byte 사본과 그 보기다. 만들 때 모든 청크의
// 크기·SHA-256을
//   확인하므로, 이 보기로 만든 ResidentAudioChunkSource는 "검증된 payload" 계약을 지킨다.
// - sidecar(TryCreateStreamed): 메타 + 청크 0 사본과 payload 범위 읽기 경계다. 청크 0은 늘 상주하고
//   (UE zeroth chunk), 나머지는 청크 캐시가 읽는다(에셋 블록 해시가 검증한다).
// 재생 스냅숏(SoundPlayable)이 공유 참조로 붙잡아, 패키지가 재생 중에 내려가도 byte가 산다.
//
// 사용 예:
//   auto stream = SoundStreamData::TryCreate(allocator, bytes);
//   audio::ResidentAudioChunkSource chunks(stream.Value()->View());
//
// 스레드: 만든 뒤에는 바뀌지 않으므로 여러 스레드가 읽어도 된다.
namespace rocks {

class ROCKS_AUDIOASSETS_API SoundStreamData {
public:
    // bytes를 복사하고 해석·검증한다. 형식 위반·청크 손상·payload 모자람(잘림)은 CorruptData,
    // 모르는 형식 판·codec id는 Unsupported(Parse 그대로), 할당 실패는 OutOfMemory다.
    [[nodiscard]] static Result<SharedPtr<SoundStreamData>>
    TryCreate(Allocator& allocator, Span<const uint8> bytes) noexcept;

    // sidecar 형태를 만든다. prefix = 쿠킹이 나눈 패키지 bulk(정확히 메타 + 청크 0 끝 — 3.46.2절),
    // streamed = payload 영역 전체 읽기 경계다. 형식 위반·청크 0 손상·청크가 하나뿐·prefix가 그
    // 모양이 아님은 CorruptData, streamed가 null이거나 Bytes()가 머리의 payload byte와 다르면
    // InvalidArgument, 할당 실패는 OutOfMemory다. 그래서 sidecar 형태는 HasAllPayload()가 늘
    // 거짓이다.
    [[nodiscard]] static Result<SharedPtr<SoundStreamData>>
    TryCreateStreamed(Allocator& allocator, Span<const uint8> prefix,
                      SharedPtr<SoundStreamedPayload> streamed) noexcept;

    explicit SoundStreamData(Allocator& allocator) noexcept : bytes_(allocator) {
    }

    SoundStreamData(const SoundStreamData&) = delete;
    SoundStreamData& operator=(const SoundStreamData&) = delete;

    const audio::AudioPlatformDataView& View() const noexcept {
        return view_;
    }

    uint32 ChannelCount() const noexcept {
        return view_.Header().channels;
    }

    uint32 SampleRate() const noexcept {
        return view_.Header().sampleRate;
    }

    uint64 FrameCount() const noexcept {
        return view_.Header().frames;
    }

    // 상주 압축 byte 수다(진단).
    usize ByteCount() const noexcept {
        return bytes_.Size();
    }

    // payload 영역이 모두 메모리에 있는지다(상주 형태).
    bool HasAllPayload() const noexcept {
        return view_.HasAllPayload();
    }

    // sidecar 읽기 경계다(상주 형태면 null).
    const SharedPtr<SoundStreamedPayload>& Streamed() const noexcept {
        return streamed_;
    }

    // 청크 0 payload다(두 형태 모두 메모리에 있다).
    Span<const uint8> Chunk0() const noexcept {
        return view_.ChunkPayload(0);
    }

    // 프로세스 안에서 유일한 번호다(청크 캐시 키 — 같은 데이터를 쓰는 재생끼리 청크를 공유한다).
    uint64 Serial() const noexcept {
        return serial_;
    }

private:
    Array<uint8> bytes_;
    audio::AudioPlatformDataView view_;
    SharedPtr<SoundStreamedPayload> streamed_;
    uint64 serial_ = 0;
};

} // namespace rocks
