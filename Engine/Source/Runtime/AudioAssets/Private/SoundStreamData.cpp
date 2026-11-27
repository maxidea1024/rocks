#include <AudioAssets/SoundStreamData.h>

// 실시간 디코드용 공유 불변 압축 데이터다(Docs/Design/AudioSystem 3.40.3절).
namespace rocks {

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
    return made;
}

} // namespace rocks
