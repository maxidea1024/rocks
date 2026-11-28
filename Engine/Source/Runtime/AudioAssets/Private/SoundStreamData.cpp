#include <AudioAssets/SoundStreamData.h>

#include <Platform/Atomics.h>

// 실시간 디코드용 공유 불변 압축 데이터다(Docs/Design/AudioSystem 3.40.3·3.46.3절).
namespace rocks {

namespace {

// 다음 스트림 데이터 번호다(0은 쓰지 않는다 — 청크 캐시 키).
uint64 soundStreamDataSerial = 0;

uint64 NextSoundStreamDataSerial() noexcept {
    return platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&soundStreamDataSerial,
                                                                    uint64{1}) +
           1;
}

} // namespace

Result<SharedPtr<SoundStreamData>> SoundStreamData::TryCreate(Allocator& allocator,
                                                              Span<const uint8> bytes) noexcept {
    using Made = Result<SharedPtr<SoundStreamData>>;
    auto made = SharedPtr<SoundStreamData>::TryMake(allocator, allocator);
    if (!made) {
        return Made::Failure(made.Error());
    }

    SoundStreamData& data = *made.Value();
    if (auto copied = data.bytes_.TryAppend(bytes); !copied) {
        return Made::Failure(copied.Error());
    }

    // 보기는 이 객체의 byte를 가리킨다(객체는 힙에 있고 byte는 다시 할당하지 않는다).
    auto view = audio::AudioPlatformDataView::Parse(data.bytes_.View());
    if (!view) {
        return Made::Failure(view.Error());
    }

    // reader가 준 byte는 플랫폼 데이터 전체여야 한다(payload가 모자라면 잘린 데이터다).
    if (!view.Value().HasAllPayload()) {
        return Made::Failure(ErrorCode::CorruptData);
    }

    if (auto verified = audio::VerifyAllAudioChunks(view.Value()); !verified) {
        return Made::Failure(verified.Error());
    }

    data.view_ = view.Value();
    data.serial_ = NextSoundStreamDataSerial();
    return made;
}

Result<SharedPtr<SoundStreamData>>
SoundStreamData::TryCreateStreamed(Allocator& allocator, Span<const uint8> prefix,
                                   SharedPtr<SoundStreamedPayload> streamed) noexcept {
    using Made = Result<SharedPtr<SoundStreamData>>;
    if (!streamed) {
        return Made::Failure(ErrorCode::InvalidArgument);
    }

    auto made = SharedPtr<SoundStreamData>::TryMake(allocator, allocator);
    if (!made) {
        return Made::Failure(made.Error());
    }

    SoundStreamData& data = *made.Value();
    if (auto copied = data.bytes_.TryAppend(prefix); !copied) {
        return Made::Failure(copied.Error());
    }

    auto view = audio::AudioPlatformDataView::Parse(data.bytes_.View());
    if (!view) {
        return Made::Failure(view.Error());
    }

    const audio::AudioPlatformDataView& parsed = view.Value();
    if (streamed->Bytes() != parsed.Header().payloadBytes) {
        return Made::Failure(ErrorCode::InvalidArgument);
    }

    // 앞부분은 쿠킹이 나눈 모양 그대로다: 청크가 둘 이상이고 byte가 정확히 메타 + 청크 0 끝이다
    // (3.46.2절). 그래서 sidecar 형태는 늘 HasAllPayload()가 거짓이다(상주·sidecar 둘 중 하나).
    if (parsed.Header().chunkCount < 2) {
        return Made::Failure(ErrorCode::CorruptData);
    }

    const audio::AudioChunkEntry first = parsed.Chunk(0);
    const uint64 prefixBytes = parsed.MetaBytes() + first.payloadOffset + first.payloadBytes;
    if (data.bytes_.Size() != prefixBytes) {
        return Made::Failure(ErrorCode::CorruptData);
    }

    // 청크 0을 검증한다(sidecar 블록 해시가 덮지 않는 패키지 byte다 — 3.46.3절).
    const Span<const uint8> chunk0 = parsed.ChunkPayload(0);
    if (auto verified = audio::VerifyAudioChunk(first, chunk0); !verified) {
        return Made::Failure(verified.Error());
    }

    data.view_ = parsed;
    data.streamed_ = Move(streamed);
    data.serial_ = NextSoundStreamDataSerial();
    return made;
}

} // namespace rocks
