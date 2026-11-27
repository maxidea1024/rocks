#pragma once

#include <Audio/AudioPlatformData.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>

// 스트림 생산자가 청크 payload를 얻는 인터페이스다(Docs/Design/AudioSystem 3.40.2절). AU6a는 상주
// byte 구현(ResidentAudioChunkSource)을 쓰고, AU6b의 청크 캐시(sidecar·비동기 IO)가 같은 자리에
// 들어간다.
//
// 계약:
// - Ready가 준 payload는 검증된 것이다(크기·SHA-256 — VerifyAudioChunk). 생산자는 다시 보지 않는다.
// - payload는 Release(index)할 때까지 산다(디코더 BeginChunk의 "청크를 다 낼 때까지 산다" 계약).
// - Pending은 "아직 없다"이고 생산자는 그 Pump에서 더 쓰지 않는다(다음 Pump에서 다시 묻는다).
// - Failed는 그 스트림의 실패다(error가 원인 — 손상 CorruptData, IO 오류 등).
//
// 사용 예:
//   const AudioChunkAcquire got = source.TryAcquire(index);
//   if (got.status == AudioChunkStatus::Ready) { decoder.BeginChunk(chunk, got.payload, start); }
//   ... 청크를 다 낸 뒤 source.Release(index);
//
// 스레드: 디코드 서비스 스레드 하나가 쓴다.
namespace rocks::audio {

enum class AudioChunkStatus : uint8 {
    Ready,
    Pending,
    Failed
};

struct AudioChunkAcquire {
    AudioChunkStatus status = AudioChunkStatus::Failed;
    Span<const uint8> payload{};
    ErrorCode error = ErrorCode::None;
};

class ROCKS_AUDIO_API AudioChunkSource {
public:
    virtual ~AudioChunkSource() = default;

    // index 청크의 검증된 payload를 얻는다. index가 청크 수 이상이면 Failed(InvalidArgument)다.
    virtual AudioChunkAcquire TryAcquire(uint32 index) noexcept = 0;

    // TryAcquire가 Ready로 준 청크를 놓는다.
    virtual void Release(uint32 index) noexcept = 0;

    // 곧 필요할 청크를 미리 청한다(상주 구현은 아무것도 하지 않는다).
    virtual void Prefetch(uint32 index) noexcept = 0;
};

// 상주 byte 구현이다. view는 payload 전체를 가져야 하고(HasAllPayload), 모든 청크가 이미 검증돼
// 있어야 한다(VerifyAllAudioChunks — 로드 때 한 번). view의 byte는 이 객체보다 오래 살아야 한다.
class ROCKS_AUDIO_API ResidentAudioChunkSource final : public AudioChunkSource {
public:
    explicit ResidentAudioChunkSource(const AudioPlatformDataView& view) noexcept : view_(view) {
    }

    AudioChunkAcquire TryAcquire(uint32 index) noexcept override;

    void Release(uint32 index) noexcept override {
        (void)index;
    }

    void Prefetch(uint32 index) noexcept override {
        (void)index;
    }

private:
    AudioPlatformDataView view_;
};

} // namespace rocks::audio
