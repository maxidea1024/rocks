#include "GraphicsSelfTest.h"

#include <Core/Containers/Array.h>
#include <D3D12Graphics/D3D12Device.h>
#include <OpenGLGraphics/OpenGLDevice.h>
#include <PlatformWindow/Window.h>
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
    // quad 결과(RenderTarget)를 텍스처로 샘플링해 다른 RenderTarget에 옮긴 결과다.
    Array<uint8> copy;

    explicit BackendResult(Allocator& allocator) noexcept
        : triangle(allocator), quad(allocator), copy(allocator) {
    }
};

uint32 failures = 0;

void Check(bool condition, const char* backend, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s: %s\n", backend, what);
    }
}

// 숨김 창에 스왑체인을 만들어 프레임·크기 변경·기록 중 파괴를 확인한다(화면 픽셀은 읽지 않는다).
void RunSwapChainChecks(const char* name, graphics::Device& device, render::Renderer& renderer,
                        Allocator& allocator) noexcept {
    platform::WindowDesc windowDesc;
    windowDesc.title = "Rocks graphics self-test";
    windowDesc.width = 96;
    windowDesc.height = 64;
    windowDesc.visible = false;
    auto window = platform::Window::Create(allocator, windowDesc);
    input::InputQueue queue(allocator);
    if (!window || !queue.TryInitialize(64)) {
        printf("  [NOT RUN] %s: swap chain checks (window unavailable)\n", name);
        return;
    }
    const platform::NativeWindowHandle native = window.Value()->NativeHandle();
    graphics::SwapChainDesc desc;
    desc.surface.window = native.window;
    desc.surface.instance = native.instance;
    desc.width = windowDesc.width;
    desc.height = windowDesc.height;
    desc.vsync = false;
    auto swapChain = device.CreateSwapChain(desc);
    Check(swapChain.IsOk(), name, "swap chain creation");
    if (!swapChain) {
        printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
               device.LastError().Data());
        return;
    }
    render::RenderView view;
    view.viewProjection = FillScreen();
    view.clear = {0.1f, 0.1f, 0.1f, 1.0f};
    render::RenderItem item;
    item.mesh = render::MeshKind::Triangle;
    auto frameOnce = [&]() {
        window.Value()->PumpEvents(queue);
        auto frame = device.BeginFrame(swapChain.Value(), view.clear);
        if (!frame) {
            return false;
        }
        renderer.Draw(*frame.Value(), view, Span<const render::RenderItem>(&item, 1));
        return device.EndFrame().IsOk();
    };
    bool frames = true;
    for (int index = 0; index < 4; ++index) {
        frames = frameOnce() && frames;
    }
    Check(frames, name, "swap chain frames present");
    Check(device.ResizeSwapChain(swapChain.Value(), 128, 80).IsOk() && frameOnce() && frameOnce(),
          name, "swap chain resize and present");

    // 기록 중인 스왑체인을 파괴해도 EndFrame이 안전해야 한다(present는 생략된다).
    auto frame = device.BeginFrame(swapChain.Value(), view.clear);
    bool destroyed = frame.IsOk();
    if (frame) {
        renderer.Draw(*frame.Value(), view, Span<const render::RenderItem>(&item, 1));
        device.Destroy(swapChain.Value());
        destroyed = device.EndFrame().IsOk();
    }
    Check(destroyed && device.WaitIdle().IsOk(), name, "swap chain destroyed while recording");
    Check(device.BeginFrame(swapChain.Value(), view.clear).Error() == ErrorCode::StaleHandle, name,
          "destroyed swap chain handle is stale");
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
    checkerDesc.sampler.filter = graphics::Filter::Bilinear;
    checkerDesc.sampler.addressU = graphics::AddressMode::Clamp;
    checkerDesc.sampler.addressV = graphics::AddressMode::Clamp;
    auto texture = device.CreateTexture(checkerDesc, Span<const uint8>(checker, sizeof(checker)));
    Check(target.IsOk() && texture.IsOk(), name, "texture creation");
    if (!target || !texture) {
        renderer.Shutdown();
        return;
    }

    render::RenderView view;
    view.viewProjection = FillScreen();
    view.clear = {0.0f, 0.0f, 0.0f, 1.0f};
    auto drawTo = [&](graphics::TextureHandle into, render::MeshKind mesh,
                      graphics::TextureHandle bound, Array<uint8>& out) {
        render::RenderItem item;
        item.mesh = mesh;
        item.texture = bound;
        auto frame = device.BeginOffscreen(into, view.clear);
        if (!frame) {
            return false;
        }
        renderer.Draw(*frame.Value(), view, Span<const render::RenderItem>(&item, 1));
        if (!device.EndFrame() || !out.TryResize(static_cast<usize>(Size) * Size * 4)) {
            return false;
        }
        return device.ReadPixels(into, out.View()).IsOk();
    };
    auto draw = [&](render::MeshKind mesh, graphics::TextureHandle bound, Array<uint8>& out) {
        return drawTo(target.Value(), mesh, bound, out);
    };

    Check(draw(render::MeshKind::Triangle, {}, result.triangle), name, "triangle render/readback");
    Check(draw(render::MeshKind::Quad, texture.Value(), result.quad), name, "quad render/readback");

    // RenderTarget을 텍스처로 샘플링해도 위아래가 바뀌지 않아야 한다(GL은 원점 규약이 다르다).
    auto copyTarget = device.CreateTexture(targetDesc, {});
    Check(copyTarget.IsOk() &&
              drawTo(copyTarget.Value(), render::MeshKind::Quad, target.Value(), result.copy),
          name, "render target sampled as a texture");
    if (result.copy.Size() == result.quad.Size()) {
        Check(Dominant(At(result.copy, 4, 4), 0), name, "sampled render target keeps top-left red");
        Check(Dominant(At(result.copy, 4, 59), 2), name,
              "sampled render target keeps bottom-left blue");
    }
    if (copyTarget) {
        device.Destroy(copyTarget.Value());
    }

    // 기록 중 파괴한 텍스처는 그 프레임이 끝날 때까지 살아 있어야 한다. 파괴 뒤 업로드(별도
    // signal)나 WaitIdle이 끼어도 해제·descriptor 재사용이 일어나면 안 된다.
    auto destroyWhileRecording = [&](bool waitIdle) {
        const uint8 red[] = {255, 0, 0, 255};
        const uint8 green[] = {0, 255, 0, 255};
        graphics::TextureDesc oneDesc;
        oneDesc.width = 1;
        oneDesc.height = 1;
        auto doomed = device.CreateTexture(oneDesc, Span<const uint8>(red, 4));
        if (!doomed) {
            return false;
        }
        render::RenderItem item;
        item.mesh = render::MeshKind::Quad;
        item.texture = doomed.Value();
        auto frame = device.BeginOffscreen(target.Value(), view.clear);
        if (!frame) {
            device.Destroy(doomed.Value());
            return false;
        }
        renderer.Draw(*frame.Value(), view, Span<const render::RenderItem>(&item, 1));
        device.Destroy(doomed.Value());
        bool ok = true;
        if (waitIdle) {
            ok = device.WaitIdle().IsOk();
        }
        auto replacement = device.CreateTexture(oneDesc, Span<const uint8>(green, 4));
        ok = device.EndFrame().IsOk() && ok && replacement.IsOk();
        Array<uint8> pixels(result.triangle.GetAllocator());
        ok = ok && pixels.TryResize(static_cast<usize>(Size) * Size * 4).IsOk() &&
             device.ReadPixels(target.Value(), pixels.View()).IsOk() &&
             Dominant(At(pixels, 32, 32), 0);
        if (replacement) {
            device.Destroy(replacement.Value());
        }
        return ok;
    };
    // 텍스처별 샘플러: 최근접은 경계 직전까지 순색, Wrap은 가장자리에서 반대편과 섞이고 Clamp는
    // 순색이다.
    auto samplerProbe = [&](graphics::Filter filter, graphics::AddressMode address, uint32 x,
                            uint32 y, Pixel& out) {
        graphics::TextureDesc desc = checkerDesc;
        desc.sampler.filter = filter;
        desc.sampler.addressU = address;
        desc.sampler.addressV = address;
        auto probe = device.CreateTexture(desc, Span<const uint8>(checker, sizeof(checker)));
        Array<uint8> pixels(result.triangle.GetAllocator());
        const bool ok = probe.IsOk() && draw(render::MeshKind::Quad, probe.Value(), pixels);
        if (probe) {
            device.Destroy(probe.Value());
        }
        if (ok) {
            out = At(pixels, x, y);
        }
        return ok;
    };
    Pixel probe = {};
    Check(samplerProbe(graphics::Filter::Nearest, graphics::AddressMode::Clamp, 31, 4, probe) &&
              probe.r > 250 && probe.g < 5 && probe.b < 5,
          name, "nearest filter keeps texel color up to the edge");
    Check(samplerProbe(graphics::Filter::Bilinear, graphics::AddressMode::Clamp, 0, 0, probe) &&
              probe.r > 250 && probe.g < 5,
          name, "clamp address keeps the edge texel");
    Check(samplerProbe(graphics::Filter::Bilinear, graphics::AddressMode::Wrap, 0, 0, probe) &&
              probe.g > 100 && probe.b > 100,
          name, "wrap address blends with the opposite edge");
    // Border(투명 검정): 가장자리 픽셀이 테두리와 섞여 알파가 줄고 검정 배경이 비친다.
    Check(samplerProbe(graphics::Filter::Bilinear, graphics::AddressMode::Border, 0, 0, probe) &&
              probe.r > 30 && probe.r < 230 && probe.g < 5,
          name, "border address blends with the transparent border color");

    // mip: 64×64 텍스처를 4×4 픽셀 크기로 그리면 LOD 4(4×4 mip)를 읽는다. 4번 이후 mip만 파랑이다.
    {
        constexpr uint32 MipSize = 64;
        const uint32 levels = graphics::FullMipCount(MipSize, MipSize);
        Array<uint8> chain(result.triangle.GetAllocator());
        bool built = chain
                         .TryResize(static_cast<usize>(graphics::MipChainBytes(
                             MipSize, MipSize, levels, graphics::Format::RGBA8UnormSrgb)))
                         .IsOk();
        usize offset = 0;
        for (uint32 level = 0; built && level < levels; ++level) {
            const uint32 extent = graphics::MipExtent(MipSize, level);
            const uint8 color[4] = {static_cast<uint8>(level >= 4 ? 0 : 255), 0,
                                    static_cast<uint8>(level >= 4 ? 255 : 0), 255};
            for (uint32 index = 0; index < extent * extent; ++index) {
                __builtin_memcpy(chain.Data() + offset + index * 4u, color, 4);
            }
            offset += static_cast<usize>(extent) * extent * 4u;
        }
        graphics::TextureDesc mipDesc;
        mipDesc.width = MipSize;
        mipDesc.height = MipSize;
        mipDesc.mipLevels = levels;
        mipDesc.sampler.filter = graphics::Filter::Trilinear;
        auto mipped = built ? device.CreateTexture(mipDesc, chain.View())
                            : Result<graphics::TextureHandle>::Failure(ErrorCode::OutOfMemory);
        Array<uint8> pixels(result.triangle.GetAllocator());
        bool drawn = false;
        if (mipped) {
            render::RenderItem item;
            item.mesh = render::MeshKind::Quad;
            item.texture = mipped.Value();
            // 단위 사각형을 1/16로 줄여 64픽셀 대상에서 4×4 픽셀을 덮는다.
            item.world = Mat4::Identity();
            item.world.m[0] = 1.0f / 16.0f;
            item.world.m[5] = 1.0f / 16.0f;
            item.world.m[10] = 1.0f / 16.0f;
            auto frame = device.BeginOffscreen(target.Value(), view.clear);
            if (frame) {
                renderer.Draw(*frame.Value(), view, Span<const render::RenderItem>(&item, 1));
                drawn = device.EndFrame().IsOk() &&
                        pixels.TryResize(static_cast<usize>(Size) * Size * 4).IsOk() &&
                        device.ReadPixels(target.Value(), pixels.View()).IsOk();
            }
            device.Destroy(mipped.Value());
        }
        Check(mipped.IsOk() && drawn && Dominant(At(pixels, 31, 31), 2), name,
              "minified texture samples the matching mip level");
    }

    Check(destroyWhileRecording(false), name, "texture destroyed while recording survives uploads");
    Check(destroyWhileRecording(true), name, "texture destroyed while recording survives WaitIdle");
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
    RunSwapChainChecks(name, device, renderer, result.triangle.GetAllocator());
    Check(device.IgnoredDraws() == 0, name, "no ignored draws");
    Check(device.ValidationErrorCount() == 0, name, "no validation errors");

    // 검증 오류 집계가 실제로 동작하는지 확인한다: D3D12는 파이프라인 RTV 형식과 다른 대상에
    // 그리면 debug layer 오류다(OpenGL에는 대응 오류가 없다).
    if (info.backend == graphics::Backend::D3D12 && info.validationEnabled) {
        const uint64 before = device.ValidationErrorCount();
        render::Renderer mismatched;
        if (mismatched.Initialize(device, graphics::Format::RGBA8Unorm)) {
            render::RenderItem item;
            item.mesh = render::MeshKind::Triangle;
            auto frame = device.BeginOffscreen(target.Value(), view.clear);
            if (frame) {
                mismatched.Draw(*frame.Value(), view, Span<const render::RenderItem>(&item, 1));
                auto ended = device.EndFrame();
                (void)ended;
            }
            mismatched.Shutdown();
        }
        Check(device.ValidationErrorCount() > before, name,
              "validation layer error is counted (intentional RTV format mismatch)");
    }

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
        int copyDelta = 0;
        if (left.copy.Size() == left.quad.Size() && right.copy.Size() == right.quad.Size()) {
            copyDelta = static_cast<int>(left.copy[index]) - static_cast<int>(right.copy[index]);
        }
        if (delta > 8 || delta < -8 || triangleDelta > 8 || triangleDelta < -8 || copyDelta > 8 ||
            copyDelta < -8) {
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
    // 검증 계층이 프로세스를 끝내도 그때까지의 진행이 남도록 출력 버퍼를 쓰지 않는다.
    setvbuf(stdout, nullptr, _IONBF, 0);
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
