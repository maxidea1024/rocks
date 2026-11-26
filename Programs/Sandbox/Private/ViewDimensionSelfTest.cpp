#include "ViewDimensionSelfTest.h"
#include "SandboxShaders.h"

#include <Core/Containers/Array.h>
#include <Platform/Memory.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// 픽셀 대조 허용 차이(8-bit 단위)다.
constexpr int ColorTolerance = 2;
// 시험 cube 배열의 cube 수, 읽는 cube 면(2D 배열 view의 층), cube 배열에서 읽는 slice다.
constexpr uint32 CubeArrayCubes = 2;
constexpr uint32 ReadFace = 4;
constexpr uint32 SecondCubeFirstFace = CubeFaceCount;
// cube Storage 시험의 한 변과 면 0에 쓰는 값이다.
constexpr uint32 StorageExtent = 4;
constexpr float FaceValue = 3.0f;
// slice마다 다른 색을 만드는 단계다(slice × 단계).
constexpr uint8 SliceColorStep = 40;
// 채널 최댓값이다.
constexpr uint8 Full = 255;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s view dimensions: %s\n", backendName, what);
    }
}

void PrintLastError(Device& device) noexcept {
    printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
           device.LastError().Data());
}

template <usize Count>
StringView Text(const char (&text)[Count]) noexcept {
    return StringView(text, Count - 1);
}

// slice s의 색이다(R = s × 40, G = 255 - s × 40, B = 255, A = 255).
void SliceColor(uint32 slice, uint8* out) noexcept {
    out[0] = static_cast<uint8>(slice * SliceColorStep);
    out[1] = static_cast<uint8>(Full - slice * SliceColorStep);
    out[2] = Full;
    out[3] = Full;
}

bool Near(const uint8* actual, const uint8* expected) noexcept {
    for (uint32 channel = 0; channel < 3; ++channel) {
        const int delta = static_cast<int>(actual[channel]) - static_cast<int>(expected[channel]);
        if (delta > ColorTolerance || delta < -ColorTolerance) {
            printf("    (%u, %u, %u), expected (%u, %u, %u)\n", actual[0], actual[1], actual[2],
                   expected[0], expected[1], expected[2]);
            return false;
        }
    }

    return true;
}

// 셰이더는 FullScreen.rsf·ViewDimension.rsf다.

struct FillConstants {
    float values[4] = {};
};

// 1×1 RGBA8 렌더 타깃에 full-screen 삼각형을 그려 픽셀을 읽는다.
struct Canvas {
    Device& device;
    TextureHandle target;
    uint8 pixel[4] = {};

    explicit Canvas(Device& owner) noexcept : device(owner) {
    }

    bool Initialize() noexcept {
        TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.format = PixelFormat::RGBA8Unorm;
        desc.usage = TextureUsage::RenderTarget;
        auto created = device.CreateTexture(desc, {});
        if (created) {
            target = created.Value();
        }

        return created.IsOk();
    }

    // texture를 묶고 그 PS로 그린 픽셀을 pixel에 읽는다.
    bool Draw(const shaders::ShaderTypeDesc& shader, const char* name,
              TextureHandle texture) noexcept {
        PipelineDesc desc;
        desc.colorFormats[0] = PixelFormat::RGBA8Unorm;
        auto pipeline =
            shaders::CreateGraphicsPipeline(device, shaders::FullScreenVS, shader, desc);
        if (!pipeline) {
            PrintLastError(device);
            return false;
        }

        auto frame = device.BeginOffscreen(target, {});
        if (frame) {
            frame.Value()->SetPipeline(pipeline.Value());
            frame.Value()->SetTexture(ShaderStage::Pixel, shaders::TextureSlot(shader, name),
                                      texture);
            frame.Value()->Draw(3);
        }

        const bool drawn = frame && device.EndFrame() &&
                           device.ReadPixels(target, Span<uint8>(pixel, sizeof(pixel))).IsOk();
        device.Retire(pipeline.Value());
        return drawn;
    }

    void Release() noexcept {
        if (!target.IsNull()) {
            device.Retire(target);
        }
    }
};

TextureViewDesc DimensionView(TextureViewDimension dimension, uint32 firstSlice,
                              uint32 sliceCount) noexcept {
    TextureViewDesc desc;
    desc.dimension = dimension;
    desc.firstSlice = firstSlice;
    desc.sliceCount = sliceCount;
    return desc;
}

// slice마다 SliceColor로 채운 1×1 Sampled 텍스처를 만든다.
Result<TextureHandle> ColoredTexture(Device& device, Allocator& allocator,
                                     TextureDimension dimension, uint32 arraySize) noexcept {
    TextureDesc desc;
    desc.dimension = dimension;
    desc.width = 1;
    desc.height = 1;
    desc.arraySize = arraySize;
    desc.format = PixelFormat::RGBA8Unorm;
    desc.sampler.filter = Filter::Point;
    const uint32 slices = TextureSliceCount(desc);
    Array<uint8> pixels(allocator);
    if (!pixels.TryResize(static_cast<usize>(slices) * 4)) {
        return Result<TextureHandle>::Failure(ErrorCode::OutOfMemory);
    }

    for (uint32 slice = 0; slice < slices; ++slice) {
        SliceColor(slice, pixels.Data() + slice * 4);
    }

    return device.CreateTexture(desc, pixels.View());
}

// view를 만들어 PS로 읽은 색이 slice 색과 같은지 본다. 범위 view가 없는 장치는 Unsupported를
// 단언한다.
void ExpectView(Device& device, Canvas& canvas, TextureHandle texture, const TextureViewDesc& desc,
                const shaders::ShaderTypeDesc& pixel, const char* name, uint32 slice,
                const char* what) noexcept {
    auto view = device.CreateTextureView(texture, desc);
    if (!device.Caps().features.textureViewRanges) {
        char message[128];
        snprintf(message, sizeof(message), "%s is Unsupported without texture view ranges", what);
        Check(view.Error() == ErrorCode::Unsupported, message);
        return;
    }

    Check(view.IsOk(), what);
    if (!view) {
        PrintLastError(device);
        return;
    }

    uint8 expected[4] = {};
    SliceColor(slice, expected);
    Check(canvas.Draw(pixel, name, view.Value()) && Near(canvas.pixel, expected), what);
    device.Retire(view.Value());
}

void RunReadViews(Device& device, Allocator& allocator) noexcept {
    Canvas canvas(device);
    auto array = ColoredTexture(device, allocator, TextureDimension::Texture2DArray, 2);
    auto cube = ColoredTexture(device, allocator, TextureDimension::TextureCube, 1);
    auto cubeArray =
        device.Caps().features.textureCubeArray
            ? ColoredTexture(device, allocator, TextureDimension::TextureCubeArray, CubeArrayCubes)
            : Result<TextureHandle>::Failure(ErrorCode::Unsupported);
    Check(canvas.Initialize() && array && cube, "canvas and colored textures");
    if (array && cube && !canvas.target.IsNull()) {
        ExpectView(device, canvas, array.Value(),
                   DimensionView(TextureViewDimension::Texture2D, 0, 1), shaders::ViewRead2DPS,
                   "Plane", 0, "layer 0 of a 2D array as a Texture2D view");
        ExpectView(device, canvas, array.Value(),
                   DimensionView(TextureViewDimension::Texture2DArray, 1, 1),
                   shaders::ViewReadLayer0PS, "Layers", 1,
                   "layer 1 of a 2D array as a one layer 2D array view");
        ExpectView(device, canvas, cube.Value(),
                   DimensionView(TextureViewDimension::Texture2DArray, 0, 0),
                   shaders::ViewReadLayer4PS, "Layers", ReadFace,
                   "cube face 4 through a 2D array view of the faces");
        // 2D view의 첫 slice 밖은 규칙 위반이다(모든 장치).
        Check(device.CreateTextureView(array.Value(),
                                       DimensionView(TextureViewDimension::Texture2D, 1, 1))
                      .Error() == ErrorCode::InvalidArgument,
              "a Texture2D view beyond the first slice is rejected");
    }

    if (cubeArray) {
        ExpectView(device, canvas, cubeArray.Value(),
                   DimensionView(TextureViewDimension::TextureCubeArray, SecondCubeFirstFace,
                                 CubeFaceCount),
                   shaders::ViewReadCubeArrayPS, "Cubes", SecondCubeFirstFace,
                   "the second cube through a one cube cube array view");
        device.Retire(cubeArray.Value());
    }

    if (array) {
        device.Retire(array.Value());
    }

    if (cube) {
        device.Retire(cube.Value());
    }

    canvas.Release();
}

bool Fill(Device& device, PipelineHandle pipeline, TextureHandle view, float value,
          uint32 layers) noexcept {
    auto compute = device.BeginPass(PassDesc{});
    if (!compute) {
        return false;
    }

    FillConstants constants;
    constants.values[0] = value;
    compute.Value()->SetPipeline(pipeline);
    compute.Value()->SetWritableTexture(0, view);
    const bool set = compute.Value()->SetDrawConstants(DrawConstantBytes(constants)).IsOk();
    compute.Value()->Dispatch(1, 1, layers);
    return device.EndFrame().IsOk() && set;
}

// cube Storage의 면 0만 Texture2D 쓰기 view로 쓴다.
void RunFaceWriteView(Device& device, Allocator& allocator) noexcept {
    TextureDesc desc;
    desc.dimension = TextureDimension::TextureCube;
    desc.width = StorageExtent;
    desc.height = StorageExtent;
    desc.format = PixelFormat::R32Float;
    desc.usage = TextureUsage::Storage;
    if (!device.Caps().features.computeShaders ||
        !HasPixelFormatCaps(device.GetPixelFormatCaps(desc.format),
                            RequiredPixelFormatCaps(desc))) {
        return;
    }

    auto cube = device.CreateTexture(desc, {});
    ComputePipelineDesc faceDesc;
    faceDesc.drawConstantBytes = sizeof(FillConstants);
    faceDesc.bindings.writableTextures[0] = "Face";
    ComputePipelineDesc cubeDesc;
    cubeDesc.drawConstantBytes = sizeof(FillConstants);
    cubeDesc.bindings.writableTextures[0] = "Faces";
    auto fillFace = shaders::CreateComputePipeline(device, shaders::ViewFillFaceCS, faceDesc);
    auto fillCube = shaders::CreateComputePipeline(device, shaders::ViewFillCubeCS, cubeDesc);
    TextureViewDesc allDesc;
    allDesc.access = ViewAccess::ReadWrite;
    allDesc.mipCount = 1;
    TextureViewDesc faceView = DimensionView(TextureViewDimension::Texture2D, 0, 1);
    faceView.access = ViewAccess::ReadWrite;
    faceView.mipCount = 1;
    auto all = cube ? device.CreateTextureView(cube.Value(), allDesc)
                    : Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    auto face = cube ? device.CreateTextureView(cube.Value(), faceView)
                     : Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    Check(cube && fillFace && fillCube && all && face,
          "cube Storage with a Texture2D writable view of face 0");
    if (cube && fillFace && fillCube && all && face) {
        Check(Fill(device, fillCube.Value(), all.Value(), 0.0f, CubeFaceCount) &&
                  Fill(device, fillFace.Value(), face.Value(), FaceValue, 1),
              "fill the cube and then face 0");
        const usize texels = static_cast<usize>(StorageExtent) * StorageExtent;
        Array<uint8> bytes(allocator);
        bool same = bytes.TryResize(texels * CubeFaceCount * sizeof(float)).IsOk() &&
                    device.ReadTexture(cube.Value(), 0, 0, bytes.View()).IsOk();
        for (usize index = 0; same && index < texels * CubeFaceCount; ++index) {
            float value = 0.0f;
            platform::MemoryCopy(&value, bytes.Data() + index * sizeof(float), sizeof(float));
            same = value == (index < texels ? FaceValue : 0.0f);
        }

        Check(same, "only face 0 changes through the Texture2D writable view");
    } else {
        PrintLastError(device);
    }

    if (all) {
        device.Retire(all.Value());
    }

    if (face) {
        device.Retire(face.Value());
    }

    if (fillFace) {
        device.Retire(fillFace.Value());
    }

    if (fillCube) {
        device.Retire(fillCube.Value());
    }

    if (cube) {
        device.Retire(cube.Value());
    }
}

} // namespace

uint32 RunViewDimensionChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    const uint64 validationBefore = device.ValidationErrorCount();
    RunReadViews(device, allocator);
    RunFaceWriteView(device, allocator);
    Check(device.WaitIdle().IsOk(), "wait idle");
    Check(device.ValidationErrorCount() == validationBefore, "no validation errors");
    printf("  %s view dimensions: ranges %s, %u failures\n", name,
           device.Caps().features.textureViewRanges ? "yes" : "no (Unsupported)", failures);
    return failures;
}

} // namespace sandbox
