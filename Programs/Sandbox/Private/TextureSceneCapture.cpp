#include "TextureSceneCapture.h"

#include "TexelCodec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace sandbox {

namespace {

// 2D·배열·volume의 표본점(사분면 가운데)이다.
constexpr uint32 QuadrantPoints[4][2] = {{SceneCaptureWidth / 4, SceneCaptureHeight / 4},
                                         {SceneCaptureWidth * 3 / 4, SceneCaptureHeight / 4},
                                         {SceneCaptureWidth / 4, SceneCaptureHeight * 3 / 4},
                                         {SceneCaptureWidth * 3 / 4, SceneCaptureHeight * 3 / 4}};

// 가로 십자(4 × 3 칸) 칸 가운데의 화면 점과 그 면이다(TextureInspector.rsf
// InspectorCrossDirection).
struct CrossCell {
    uint32 column;
    uint32 row;
    uint32 face;
};

constexpr CrossCell CrossCells[graphics::CubeFaceCount] = {{2, 1, 0}, {0, 1, 1}, {1, 0, 2},
                                                           {1, 2, 3}, {1, 1, 4}, {3, 1, 5}};
constexpr uint32 CrossColumns = 4;
constexpr uint32 CrossRows = 3;
// 장치 사이 대조의 기준(처음 돈 장치)을 담을 수 있는 key 수다(장면 64 × 점 8).
constexpr uint32 MaxReferenceKeys = 64 * MaxScenePoints;
constexpr usize MaxDeviceName = 64;
constexpr usize MaxGroups = 2;

struct Reference {
    char group[MaxDeviceName] = {};
    char device[MaxDeviceName] = {};
    bool valid[MaxReferenceKeys] = {};
    int rgb[MaxReferenceKeys][3] = {};
};

Reference references[MaxGroups];

void CopyText(const char* text, char* out, usize capacity) noexcept {
    snprintf(out, capacity, "%s", text);
}

bool PixelAt(const uint8* pixels, usize size, uint32 x, uint32 y, int (&rgb)[3]) noexcept {
    const usize offset = (static_cast<usize>(y) * SceneCaptureWidth + x) * 4;
    if (x >= SceneCaptureWidth || y >= SceneCaptureHeight || offset + 4 > size) {
        return false;
    }

    rgb[0] = pixels[offset];
    rgb[1] = pixels[offset + 1];
    rgb[2] = pixels[offset + 2];
    return true;
}

void AddCheck(Array<SceneCheck>& checks, uint32 key, uint32 slot, uint32 x, uint32 y,
              const DisplayPixel& expected, const char* sceneName, const char* point) noexcept {
    SceneCheck check;
    check.key = key;
    check.slot = slot;
    check.x = x;
    check.y = y;
    check.expected = expected;
    snprintf(check.what, sizeof(check.what), "%s %s", sceneName, point);
    (void)checks.TryPushBack(check);
}

} // namespace

Result<Span<const uint8>> MipBytes(const TexturePlatformData& data, PlatformBulkReader reader,
                                   void* context, uint32 level) noexcept {
    if (level >= data.mips.Size() || data.residentTailMips > data.mips.Size()) {
        return Result<Span<const uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    const TextureMipData& mip = data.mips[level];
    const uint32 streamed = static_cast<uint32>(data.mips.Size()) - data.residentTailMips;
    if (level < streamed) {
        return reader(context, mip.bulk);
    }

    auto tail = reader(context, data.tailBulk);
    if (!tail) {
        return tail;
    }

    if (mip.tailOffset > tail.Value().Size() ||
        mip.byteSize > tail.Value().Size() - mip.tailOffset) {
        return Result<Span<const uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<Span<const uint8>>::Success(
        Span<const uint8>(tail.Value().Data() + mip.tailOffset, static_cast<usize>(mip.byteSize)));
}

void AppendInspectorChecks(const screen::InspectorTexture& texture, uint32 layer, uint32 mip,
                           uint32 width, uint32 height, uint32 slices, uint32 depth,
                           graphics::PixelFormat format, Span<const uint8> bytes, uint32 sceneIndex,
                           uint32 slot, const char* sceneName, Array<SceneCheck>& checks) noexcept {
    (void)mip;
    const uint32 base = sceneIndex * MaxScenePoints;
    const DisplayPixel black{{0, 0, 0}};
    const bool cube = graphics::IsCubeDimension(texture.dimension);
    if (texture.cpuPlaceholder ||
        (cube && texture.component != screen::InspectorComponent::Float)) {
        AddCheck(checks, base, slot, SceneCaptureWidth / 2, SceneCaptureHeight / 2, black,
                 sceneName,
                 texture.cpuPlaceholder ? "placeholder is black" : "integer cube is black");
        return;
    }

    // plane은 slice 다음 z다(6.5절 배치 — 블록 형식은 plane마다 블록 행).
    auto texelAt = [&](uint32 slice, uint32 x, uint32 y, uint32 z,
                       float (&value)[4]) noexcept -> bool {
        return DecodeImageTexel(format, bytes, width, height, slice * depth + z, x, y, value);
    };
    if (cube) {
        const uint32 cellWidth = SceneCaptureWidth / CrossColumns;
        const uint32 cellHeight = SceneCaptureHeight / CrossRows;
        for (uint32 point = 0; point < graphics::CubeFaceCount; ++point) {
            const CrossCell& cell = CrossCells[point];
            float value[4];
            if (!texelAt(layer * graphics::CubeFaceCount + cell.face, width / 2, height / 2, 0,
                         value)) {
                continue;
            }

            char label[32];
            snprintf(label, sizeof(label), "face %u", cell.face);
            AddCheck(checks, base + point, slot, cell.column * cellWidth + cellWidth / 2,
                     cell.row * cellHeight + cellHeight / 2, ExpectedInspectorPixel(texture, value),
                     sceneName, label);
        }

        return;
    }

    const bool volume = texture.dimension == graphics::TextureDimension::Texture3D;
    const uint32 slice = volume ? 0u : layer;
    const uint32 z = volume ? layer : 0u;
    (void)slices;
    for (uint32 point = 0; point < 4; ++point) {
        const uint32 px = QuadrantPoints[point][0];
        const uint32 py = QuadrantPoints[point][1];
        // 화면 픽셀 가운데의 uv가 덮는 texel이다(표본점은 사분면 안 — 필터와 무관하게 한 값).
        const double u = (px + 0.5) / SceneCaptureWidth;
        const double v = (py + 0.5) / SceneCaptureHeight;
        uint32 x = static_cast<uint32>(floor(u * width));
        uint32 y = static_cast<uint32>(floor(v * height));
        x = x < width ? x : width - 1;
        y = y < height ? y : height - 1;
        float value[4];
        if (!texelAt(slice, x, y, z, value)) {
            continue;
        }

        char label[32];
        snprintf(label, sizeof(label), "quadrant %u", point);
        AddCheck(checks, base + point, slot, px, py, ExpectedInspectorPixel(texture, value),
                 sceneName, label);
    }
}

void CheckCapturePixel(const Array<uint8>& pixels, uint32 x, uint32 y, const DisplayPixel& expected,
                       int tolerance, const char* device, const char* mode, const char* what,
                       uint32& failures) noexcept {
    int rgb[3];
    if (!PixelAt(pixels.Data(), pixels.Size(), x, y, rgb)) {
        ++failures;
        printf("  [FAIL] %s (%s): %s (no capture)\n", device, mode, what);
        return;
    }

    bool close = true;
    for (int channel = 0; channel < 3; ++channel) {
        const int delta = rgb[channel] - expected.rgb[channel];
        close = close && delta <= tolerance && delta >= -tolerance;
    }

    if (!close) {
        ++failures;
        printf("  [FAIL] %s (%s): %s\n    pixel (%u, %u) is (%d, %d, %d), expected (%d, %d, %d)\n",
               device, mode, what, x, y, rgb[0], rgb[1], rgb[2], expected.rgb[0], expected.rgb[1],
               expected.rgb[2]);
    }
}

void CompareWithReferenceBackend(const char* group, const char* device,
                                 Span<const SceneCheck> checks, Span<const uint8> captures,
                                 uint32 slotCount, uint32& failures) noexcept {
    Reference* reference = nullptr;
    for (Reference& candidate : references) {
        if (candidate.group[0] == '\0' || strcmp(candidate.group, group) == 0) {
            reference = &candidate;
            break;
        }
    }

    if (reference == nullptr || slotCount == 0) {
        return;
    }

    const usize slotBytes = captures.Size() / slotCount;
    const bool first = reference->group[0] == '\0';
    if (first) {
        CopyText(group, reference->group, sizeof(reference->group));
        CopyText(device, reference->device, sizeof(reference->device));
    }

    uint32 compared = 0;
    for (const SceneCheck& check : checks) {
        int rgb[3];
        if (check.key >= MaxReferenceKeys || check.slot >= slotCount ||
            !PixelAt(captures.Data() + check.slot * slotBytes, slotBytes, check.x, check.y, rgb)) {
            continue;
        }

        if (!reference->valid[check.key]) {
            reference->valid[check.key] = true;
            for (int channel = 0; channel < 3; ++channel) {
                reference->rgb[check.key][channel] = rgb[channel];
            }

            continue;
        }

        ++compared;
        bool close = true;
        for (int channel = 0; channel < 3; ++channel) {
            const int delta = rgb[channel] - reference->rgb[check.key][channel];
            close = close && delta <= SceneBackendTolerance && delta >= -SceneBackendTolerance;
        }

        if (!close) {
            ++failures;
            printf("  [FAIL] %s %s: %s differs from %s by more than %d\n", device, group,
                   check.what, reference->device, SceneBackendTolerance);
        }
    }

    if (!first) {
        printf("  %s %s: %u sample points compared with %s\n", device, group, compared,
               reference->device);
    }
}

} // namespace sandbox
