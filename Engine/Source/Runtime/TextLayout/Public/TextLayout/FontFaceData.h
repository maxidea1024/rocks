#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Result.h>

// 글꼴 원본 파일 byte다(Docs/Design/Font 4.5절, UE FFontFaceData). 불변이며 스레드 안전 공유
// 포인터로 글꼴 cache와 face가 함께 붙잡는다. FT2a는 메모리 byte(Inline·LazyLoad)만 다룬다.
//
// 사용 예:
//   auto data = text::FontFaceData::TryCreate(allocator, fileBytes);
//   if (!data) { return data.Error(); }
//   backend.OpenFace({.data = data.Value()});
namespace rocks::text {

class ROCKS_TEXTLAYOUT_API FontFaceData {
public:
    // bytes를 복사해 만든다. 빈 byte는 InvalidArgument, 할당 실패는 OutOfMemory다.
    static Result<SharedPtr<const FontFaceData>> TryCreate(Allocator& allocator,
                                                           Span<const uint8> bytes) noexcept;

    explicit FontFaceData(Allocator& allocator) noexcept : bytes_(allocator) {
    }

    // 원본 byte다. 객체가 사는 동안 바뀌지 않는다.
    Span<const uint8> Bytes() const noexcept {
        return Span<const uint8>(bytes_.Data(), bytes_.Size());
    }

private:
    Array<uint8> bytes_;
};

} // namespace rocks::text
