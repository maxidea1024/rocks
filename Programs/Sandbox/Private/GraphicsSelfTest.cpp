#include "GraphicsSelfTest.h"

#include <Core/Containers/Array.h>
#include <D3D12Graphics/D3D12Device.h>
#include <OpenGLGraphics/OpenGLDevice.h>
#include <Renderer/Renderer.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;

constexpr uint32 Size = 64;

// 로컬 Y(왼쪽)·Z(위)를 clip X(오른쪽)·Y(위)로 보내는 변환이다. 단위 메시가 화면을 가득 채운다.
Mat4 FillScreen() noexcept {
    Mat4 matrix = Mat4::Zero();
    matrix.m[4] = -2.0f; // y → clip x(오른쪽이 -Y)
    matrix.m[9] = 2.0f;  // z → clip y
    matrix.m[14] = 0.5f; // clip z
    matrix.m[15] = 1.0f; // w
    return matrix;
}

struct Pixel {
    uint8 r;
    uint8 g;
    uint8 b;
    uint8 a;
};

Pixel At(const Array<uint8>& pixels, uint32 x, uint32 y) noexcept {
    const usize offset = (static_cast<usize>(y) * Size + x) * 4;
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
}

bool Dominant(Pixel pixel, int channel) noexcept {
    const int values[3] = {pixel.r, pixel.g, pixel.b};
    for (int index = 0; index < 3; ++index) {
        if (index != channel && values[index] + 60 > values[channel]) {
            return false;
        }
    }
    return true;
}

struct BackendResult {
    bool ran = false;
    bool passed = false;
    Array<uint8> triangle;
    Array<uint8> quad;

    explicit BackendResult(Allocator& allocator) noexcept : triangle(allocator), quad(allocator) {
    }
};

uint32 failures = 0;

void Check(bool condition, const char* backend, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s: %s\n", backend, what);
    }
}

// 한 backend에서 삼각형·사각형을 그려 픽셀을 검사한다.
void RunBackend(const char* name, Result<UniquePtr<graphics::Device>> created,
                BackendResult& result) noexcept {
    if (!created) {
        printf("  [NOT RUN] %s: device unavailable (error %u)\n", name,
               static_cast<unsigned>(created.Error()));
        return;
    }
    result.ran = true;
    const uint32 before = failures;
    graphics::Device& device = *created.Value();
    const graphics::DeviceInfo info = device.Info();
    printf("  %s: %.*s (validation %s%s)\n", name, static_cast<int>(info.adapterName.ByteCount()),
           info.adapterName.Data(), info.validationEnabled ? "on" : "off",
           info.software ? ", software" : "");

    render::Renderer renderer;
    auto initialized = renderer.Initialize(device, graphics::Format::RGBA8UnormSrgb);
    if (!initialized) {
        printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
               device.LastError().Data());
        Check(false, name, "renderer initialization");
        return;
    }

    graphics::TextureDesc targetDesc;
    targetDesc.width = Size;
    targetDesc.height = Size;
    targetDesc.format = graphics::Format::RGBA8UnormSrgb;
    targetDesc.usage = graphics::TextureUsage::RenderTarget;
    auto target = device.CreateTexture(targetDesc, {});
    // 2x2 텍스처: 좌상 빨강, 우상 초록, 좌하 파랑, 우하 흰색(행 위→아래).
    const uint8 checker[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    graphics::TextureDesc checkerDesc;
    checkerDesc.width = 2;
    checkerDesc.height = 2;
    auto texture = device.CreateTexture(checkerDesc, Span<const uint8>(checker, sizeof(checker)));
    Check(target.IsOk() && texture.IsOk(), name, "texture creation");
    if (!target || !texture) {
        renderer.Shutdown();
        return;
    }

    render::RenderView view;
    view.viewProjection = FillScreen();
    view.clear = {0.0f, 0.0f, 0.0f, 1.0f};
    auto draw = [&](render::MeshKind mesh, graphics::TextureHandle bound, Array<uint8>& out) {
        render::RenderItem item;
        item.mesh = mesh;
        item.texture = bound;
        auto frame = device.BeginOffscreen(target.Value(), view.clear);
        if (!frame) {
            return false;
        }
        renderer.Draw(*frame.Value(), view, Span<const render::RenderItem>(&item, 1));
        if (!device.EndFrame() || !out.TryResize(static_cast<usize>(Size) * Size * 4)) {
            return false;
        }
        return device.ReadPixels(target.Value(), out.View()).IsOk();
    };

    Check(draw(render::MeshKind::Triangle, {}, result.triangle), name, "triangle render/readback");
    Check(draw(render::MeshKind::Quad, texture.Value(), result.quad), name, "quad render/readback");
    if (result.triangle.Size() == static_cast<usize>(Size) * Size * 4) {
        // 위 꼭짓점 빨강, 오른쪽 아래 파랑, 왼쪽 아래 초록, 위 모서리는 배경(검정).
        Check(Dominant(At(result.triangle, 32, 4), 0), name, "triangle top is red");
        Check(Dominant(At(result.triangle, 60, 61), 2), name, "triangle bottom-right is blue");
        Check(Dominant(At(result.triangle, 3, 61), 1), name, "triangle bottom-left is green");
        const Pixel corner = At(result.triangle, 1, 1);
        Check(corner.r < 8 && corner.g < 8 && corner.b < 8, name, "triangle corner is background");
    }
    if (result.quad.Size() == static_cast<usize>(Size) * Size * 4) {
        // UV (0,0)이 좌상단: 좌상 빨강, 우상 초록, 좌하 파랑, 우하 흰색.
        Check(Dominant(At(result.quad, 4, 4), 0), name, "quad top-left samples red");
        Check(Dominant(At(result.quad, 59, 4), 1), name, "quad top-right samples green");
        Check(Dominant(At(result.quad, 4, 59), 2), name, "quad bottom-left samples blue");
        const Pixel white = At(result.quad, 59, 59);
        Check(white.r > 200 && white.g > 200 && white.b > 200, name,
              "quad bottom-right samples white");
    }
    Check(device.IgnoredDraws() == 0, name, "no ignored draws");
    Check(device.ValidationErrorCount() == 0, name, "no validation errors");

    device.Destroy(texture.Value());
    device.Destroy(target.Value());
    renderer.Shutdown();
    auto idle = device.WaitIdle();
    Check(idle.IsOk(), name, "wait idle");
    result.passed = failures == before;
    printf("  [%s] %s\n", result.passed ? "PASS" : "FAIL", name);
}

// 두 backend의 결과가 픽셀 단위로 거의 같은지 본다(래스터화·필터링 차이 허용 범위 안).
void Compare(const BackendResult& left, const BackendResult& right, const char* name) noexcept {
    if (!left.ran || !right.ran || left.triangle.Size() != right.triangle.Size() ||
        left.quad.Size() != right.quad.Size()) {
        return;
    }
    uint32 different = 0;
    for (usize index = 0; index < left.quad.Size(); ++index) {
        const int delta = static_cast<int>(left.quad[index]) - static_cast<int>(right.quad[index]);
        const int triangleDelta =
            static_cast<int>(left.triangle[index]) - static_cast<int>(right.triangle[index]);
        if (delta > 8 || delta < -8 || triangleDelta > 8 || triangleDelta < -8) {
            ++different;
        }
    }
    // 가장자리 픽셀의 커버리지 판정 차이만 허용한다(전체의 2% 미만).
    const bool same = different * 50 < static_cast<uint32>(left.quad.Size());
    Check(same, name, "backends produce the same image");
    printf("  [%s] %s (%u differing channels)\n", same ? "PASS" : "FAIL", name, different);
}

} // namespace

int RunGraphicsSelfTest(Allocator& allocator) noexcept {
    printf("Graphics self-test\n");
    failures = 0;
    graphics::DeviceDesc desc;
    desc.enableValidation = true;
    BackendResult d3d12(allocator);
    BackendResult warp(allocator);
    BackendResult opengl(allocator);
    RunBackend("D3D12", graphics::CreateD3D12Device(allocator, desc), d3d12);
    graphics::DeviceDesc software = desc;
    software.preferSoftware = true;
    RunBackend("D3D12 WARP", graphics::CreateD3D12Device(allocator, software), warp);
    RunBackend("OpenGL", graphics::CreateOpenGLDevice(allocator, desc), opengl);
    Compare(d3d12, opengl, "D3D12 vs OpenGL");
    Compare(d3d12, warp, "D3D12 vs WARP");
    const bool anyRan = d3d12.ran || warp.ran || opengl.ran;
    printf("Graphics self-test: %s (%u failure%s)\n",
           failures == 0 && anyRan ? "PASS" : (anyRan ? "FAIL" : "NOT RUN"), failures,
           failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

} // namespace sandbox
