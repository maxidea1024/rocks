#include <TextLayout/FontFaceData.h>
#include <TextLayout/FontTypes.h>

namespace rocks::text {

namespace {

// 26.6·16.16 고정소수의 1이다.
constexpr int64 Fixed26Dot6One = 64;
constexpr int64 Fixed16Dot16One = 65536;

// FreeType FT_MulFix와 같은 반올림 곱이다((a × b + 0x8000) >> 16, 부호는 따로).
int64 MulFix(int64 a, int64 b) noexcept {
    const bool negative = (a < 0) != (b < 0);
    const int64 magnitude =
        ((a < 0 ? -a : a) * (b < 0 ? -b : b) + Fixed16Dot16One / 2) / Fixed16Dot16One;
    return negative ? -magnitude : magnitude;
}

} // namespace

uint32 ComputeFontPixelSize(float size, float scale) noexcept {
    // UE와 같이 float → 고정소수는 버림이다(ConvertPixelTo26Dot6·ConvertPixelTo16Dot16).
    const int64 fixedSize = static_cast<int64>((size > 0.0f ? size : 0.0f) * Fixed26Dot6One);
    const int64 fixedScale = static_cast<int64>((scale > 0.0f ? scale : 0.0f) * Fixed16Dot16One);
    int64 pixels = (fixedSize * FontRenderDpi + FontPointsPerInch / 2) / FontPointsPerInch;
    pixels = MulFix(pixels, fixedScale);
    return static_cast<uint32>((pixels + Fixed26Dot6One / 2) / Fixed26Dot6One);
}

Result<SharedPtr<const FontFaceData>> FontFaceData::TryCreate(Allocator& allocator,
                                                              Span<const uint8> bytes) noexcept {
    if (bytes.Size() == 0) {
        return Result<SharedPtr<const FontFaceData>>::Failure(ErrorCode::InvalidArgument);
    }

    auto made = SharedPtr<FontFaceData>::TryMake(allocator, allocator);
    if (!made) {
        return Result<SharedPtr<const FontFaceData>>::Failure(made.Error());
    }

    FontFaceData& data = *made.Value();
    if (auto resized = data.bytes_.TryResize(bytes.Size()); !resized) {
        return Result<SharedPtr<const FontFaceData>>::Failure(resized.Error());
    }

    for (usize index = 0; index < bytes.Size(); ++index) {
        data.bytes_[index] = bytes[index];
    }

    return Result<SharedPtr<const FontFaceData>>::Success(
        SharedPtr<const FontFaceData>(Move(made.Value())));
}

} // namespace rocks::text
