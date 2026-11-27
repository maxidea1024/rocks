#pragma once

#include <Audio/AudioPlatformData.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/SharedPtr.h>

// 실시간 디코드(스트리밍)용 공유 불변 압축 데이터다(Docs/Design/AudioSystem 3.40.3절). 쿠킹 플랫폼
// 데이터 byte 사본과 그 보기를 쥔다. 만들 때 모든 청크의 크기·SHA-256을 확인하므로, 이 보기로 만든
// ResidentAudioChunkSource는 "검증된 payload" 계약을 지킨다. 재생 스냅숏(SoundPlayable)이 공유
// 참조로 붙잡아, 패키지가 재생 중에 내려가도 byte가 산다.
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

private:
    Array<uint8> bytes_;
    audio::AudioPlatformDataView view_;
};

} // namespace rocks
