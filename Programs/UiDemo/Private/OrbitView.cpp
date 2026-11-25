#include "OrbitView.h"

namespace rocks::ui {

namespace {

// 화면과 공전면 사이의 회전은 지도와 탐사선 표식이 공유한다.
constexpr float CosTilt = 0.9396926f;
constexpr float SinTilt = 0.3420201f;
constexpr float OrbitX = 0.91f;
constexpr float OrbitY = 0.42f;
constexpr float PlanetRadius = 0.48f;

uint32 StarSample(uint32 x, uint32 y) noexcept {
    uint32 hash = (x * 0x27d4eb2du) ^ (y * 0x165667b1u);
    hash ^= hash >> 15;
    hash *= 0x85ebca6bu;
    return hash ^ (hash >> 13);
}

Vec2 OrbitPoint(float progress) noexcept {
    const float angle = (progress - 0.25f) * math::TwoPi<float>;
    const float x = OrbitX * math::Cos(angle);
    const float y = OrbitY * math::Sin(angle);
    return {CosTilt * x + SinTilt * y, -SinTilt * x + CosTilt * y};
}

Color MapPixel(float x, float y, uint32 sample) noexcept {
    const float u = x * CosTilt - y * SinTilt;
    const float v = x * SinTilt + y * CosTilt;
    const float distanceSquared = u * u + v * v;
    const float depth = PlanetRadius * PlanetRadius - distanceSquared;
    const float orbit = math::Sqrt(u * u / (OrbitX * OrbitX) + v * v / (OrbitY * OrbitY));
    const float ring = math::Sqrt(u * u + (v / 0.29f) * (v / 0.29f));
    const bool ringVisible = ring > 0.59f && ring < 0.79f && (depth < 0 || v > 0);
    if (ringVisible) {
        const float grain = static_cast<float>(sample & 15u) / 15;
        const float bands = 0.5f + 0.5f * math::Cos(ring * 245);
        const float intensity =
            (ring > 0.714f && ring < 0.729f) ? 0.18f : (0.58f + 0.25f * bands + 0.08f * grain);
        return {static_cast<uint8>(237 * intensity), static_cast<uint8>(196 * intensity),
                static_cast<uint8>(144 * intensity)};
    }
    if (depth >= 0) {
        const float z = math::Sqrt(depth);
        const float illumination =
            math::Clamp((-0.66f * x - 0.50f * y + 0.56f * z) / PlanetRadius, 0.0f, 1.0f);
        const float latitude = v + 0.11f * z + 0.012f * math::Sin(12 * u + 9 * v);
        const float bands =
            0.87f + 0.07f * math::Sin(latitude * 79) + 0.055f * math::Sin(latitude * 141);
        const float light = (0.055f + 0.945f * math::Sqrt(illumination)) * bands;
        return {static_cast<uint8>(30 + 225 * light), static_cast<uint8>(20 + 156 * light),
                static_cast<uint8>(23 + 89 * light)};
    }
    if (math::Abs(orbit - 1) < 0.006f) {
        return {85, 141, 146};
    }
    if ((sample & 0x7ffu) == 0u) {
        const uint8 value = static_cast<uint8>(65 + ((sample >> 13) & 95u));
        return {value, value, static_cast<uint8>(value + 10)};
    }
    return {14, 24, 33};
}

} // namespace

OrbitView::OrbitView(Allocator& allocator, SlotHandle image) noexcept
    : Element(allocator), image_(image) {
    SetVisibility(Visibility::HitTestInvisible);
}

Result<graphics::TextureHandle> OrbitView::CreateMap(Allocator& allocator,
                                                     graphics::Device& device) noexcept {
    constexpr uint32 size = 512;
    Array<uint8> pixels(allocator);
    auto allocated = pixels.TryResize(size * size * 4);
    if (!allocated) {
        return Result<graphics::TextureHandle>::Failure(allocated.Error());
    }
    for (uint32 y = 0; y < size; ++y) {
        for (uint32 x = 0; x < size; ++x) {
            const auto color = MapPixel((float(x) + 0.5f) * 2 / size - 1,
                                        (float(y) + 0.5f) * 2 / size - 1, StarSample(x, y));
            const usize i = 4 * (size * y + x);
            pixels[i] = color.r;
            pixels[i + 1] = color.g;
            pixels[i + 2] = color.b;
            pixels[i + 3] = 255;
        }
    }
    graphics::TextureDesc desc;
    desc.width = desc.height = size;
    return device.CreateTexture(desc, pixels.View());
}

void OrbitView::SetProgress(float progress) noexcept {
    if (progress_ != progress) {
        progress_ = progress;
        InvalidatePaint();
    }
}

Result<void> OrbitView::OnPaint(PaintList& list, int32 layer) const noexcept {
    const auto& geometry = GetGeometry();
    // 궤도의 장축을 살리면서 위아래 빈 여백만 잘라낸다.
    const float extent = math::Min(geometry.size.x, geometry.size.y * 1.58f);
    if (extent <= 0) {
        return Result<void>::Success();
    }
    const Vec2 center = geometry.size * 0.5f;
    const Vec2 offset = center - Vec2(extent, extent) * 0.5f;
    const auto map = geometry.MakeChild({extent, extent}, {1, 0, 0, 1, offset.x, offset.y}, {}, {});
    auto drawn = list.TryAddBox({.geometry = map, .layer = layer, .resource = image_});
    if (!drawn) {
        return drawn;
    }
    const Vec2 marker = center + OrbitPoint(progress_) * (extent * 0.5f);
    const auto outer =
        geometry.MakeChild({10, 10}, {1, 0, 0, 1, marker.x - 5, marker.y - 5}, {}, {});
    const auto inner = geometry.MakeChild({4, 4}, {1, 0, 0, 1, marker.x - 2, marker.y - 2}, {}, {});
    drawn = list.TryAddBox({outer, LinearColor::FromSRGB({124, 239, 220}), layer});
    if (!drawn) {
        return drawn;
    }
    return list.TryAddBox({inner, LinearColor::FromSRGB({15, 29, 36}), layer});
}

} // namespace rocks::ui
