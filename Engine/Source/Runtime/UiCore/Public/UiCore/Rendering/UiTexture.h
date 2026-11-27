#pragma once

#include <Core/Containers/SlotArray.h>

namespace rocks::ui {

// 렌더러가 발급하는 텍스처 id다(UE FSlateResourceHandle, Docs/Design/UiToolkit 18.3절). slot과
// 세대를 담은 불투명 값이고, 크기를 바꿔도 같은 id가 유지된다. 그리기 명령의 resource가 이것이다.
using UiTextureId = SlotHandle;

// 텍스처가 없다는 표지다.
inline constexpr UiTextureId NoUiTexture{};

// UI 텍스처의 픽셀 형식이다. 글리프 cache 세 형식(Alpha R8·Color BGRA8 sRGB·거리장 RGBA8)과
// 이미지·렌더 타깃(RGBA8 sRGB)을 덮는다.
enum class UiTextureFormat : uint8 {
    Rgba8Srgb,
    Bgra8Srgb,
    Rgba8,
    R8
};

// 텍스처를 쓰는 방식이다. 렌더 타깃은 그리기 대상과 렌더 타깃 읽기(보관 표면·viewport·post
// buffer)에 쓰고, 갱신 텍스처는 UpdateTexture로 내용 전체를 바꾼다.
enum class UiTextureUsage : uint8 {
    Static,
    Dynamic,
    RenderTarget
};

// 셰이더가 텍스처를 읽는 방식이다. 둥근 상자가 아닌 명령의 셰이더는 이것으로 정해진다.
enum class UiTextureSampling : uint8 {
    // 직선 알파 색이다(이미지·컬러 글리프·장면).
    Image,
    // R을 덮임률로 읽어 정점 색 알파에 곱한다(회색조 글리프).
    Mask,
    // 알파를 미리 곱한 색이다(투명 바탕에 그린 보관 표면).
    Premultiplied
};

struct UiTextureDesc {
    uint32 width = 0;
    uint32 height = 0;
    UiTextureFormat format = UiTextureFormat::Rgba8Srgb;
    UiTextureUsage usage = UiTextureUsage::Static;
    UiTextureSampling sampling = UiTextureSampling::Image;
};

// 형식 한 픽셀의 byte 수다.
inline constexpr uint32 UiTextureBytesPerPixel(UiTextureFormat format) noexcept {
    return format == UiTextureFormat::R8 ? 1u : 4u;
}

} // namespace rocks::ui
