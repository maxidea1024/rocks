#include "CullingSelfTest.h"
#include "SelfTestEnvironment.h"

#include <Base/Compiler.h>
#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Platform/Memory.h>
#include <PlatformWindow/Window.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// 대상 크기다(창 client 영역과 RenderTarget이 같다). 창은 OS 최소 폭보다 커야 client 크기가 요청과
// 같으므로 넉넉히 잡고, 검사 좌표는 64칸 격자로 적어 Zoom배 한 칸의 가운데 픽셀을 본다.
constexpr uint32 Size = 256;
constexpr uint32 GridSize = 64;
constexpr uint32 Zoom = Size / GridSize;
constexpr usize ImageBytes = static_cast<usize>(Size) * Size * 4;
// 배경으로 보는 채널 상한과 흰색으로 보는 채널 하한(8-bit)이다.
constexpr uint8 BackgroundLimit = 8;
constexpr uint8 WhiteFloor = 200;
// Dominant: 주 채널이 다른 채널보다 이만큼 커야 그 색으로 본다.
constexpr int DominantMargin = 60;

const char* backendName = "";
uint32 failures = 0;

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s culling: %s\n", backendName, what);
    }
}

// 샘플 렌더러 정점 형식(위치 float3, UV float2, 색 float4)이다.
struct Vertex {
    float position[3];
    float uv[2];
    float color[4];
};

// 단위 행렬 상수로 그리면 위치가 곧 clip 좌표다. 위쪽 절반에 둘을 둔다(위아래 뒤집힘도 드러난다).
// 왼쪽은 화면 반시계(앞면), 오른쪽은 화면 시계(뒤면)다.
constexpr Vertex WindingVertices[] = {
    {{-0.9f, 0.1f, 0.5f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{-0.1f, 0.1f, 0.5f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{-0.5f, 0.9f, 0.5f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.1f, 0.1f, 0.5f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.5f, 0.9f, 0.5f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.9f, 0.1f, 0.5f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
};
constexpr uint32 WindingVertexCount = sizeof(WindingVertices) / sizeof(WindingVertices[0]);

// 앞면 판정 대조 셰이더다. 컬링 없이 그려 셰이더가 본 앞면(SV_IsFrontFace·gl_FrontFacing)을 색으로
// 낸다: 앞면 초록, 뒤면 빨강. 컬링 결과만으로는 backend가 앞면 정의를 뒤집어 컬도 함께 뒤집은
// 경우(UE Vulkan 매핑)를 잡지 못한다(VulkanGraphics 11절).
constexpr char FacingHlsl[] = R"(
struct VSInput { float3 position : ATTRIBUTE0; };
float4 VSMain(VSInput input) : SV_Position { return float4(input.position, 1.0); }
float4 PSMain(float4 position : SV_Position, bool front : SV_IsFrontFace) : SV_Target {
    return front ? float4(0.0, 1.0, 0.0, 1.0) : float4(1.0, 0.0, 0.0, 1.0);
}
)";

constexpr char FacingVertexGlsl[] = R"(#version 450 core
layout(location = 0) in vec3 aPosition;
void main() {
    gl_Position = vec4(aPosition, 1.0);
}
)";

constexpr char FacingPixelGlsl[] = R"(#version 450 core
layout(location = 0) out vec4 oColor;
void main() {
    oColor = gl_FrontFacing ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);
}
)";

// 두 삼각형의 무게중심 픽셀과, 그것을 위아래로 뒤집은 픽셀이다.
constexpr uint32 CounterClockwiseX = 16;
constexpr uint32 ClockwiseX = 48;
constexpr uint32 WindingY = 20;
constexpr uint32 FlippedWindingY = 43;

// 로컬 Y(오른쪽)·Z(위)를 clip X(오른쪽)·Y(위)로 보내는 변환이다(+X를 바라보는 정면, UE 좌표).
// 단위 메시가 화면을 가득 채운다.
Mat4 FillScreen() noexcept {
    Mat4 matrix = Mat4::Zero();
    matrix.m[4] = 2.0f;  // y → clip x
    matrix.m[9] = 2.0f;  // z → clip y
    matrix.m[14] = 0.5f; // clip z
    matrix.m[15] = 1.0f; // w
    return matrix;
}

struct Pixel {
    uint8 r;
    uint8 g;
    uint8 b;
};

// 64칸 격자 (x, y) 칸의 가운데 픽셀이다.
Pixel At(const Array<uint8>& pixels, uint32 x, uint32 y) noexcept {
    const usize px = static_cast<usize>(x) * Zoom + Zoom / 2;
    const usize py = static_cast<usize>(y) * Zoom + Zoom / 2;
    const usize offset = (py * Size + px) * 4;
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
}

bool IsBackground(const Array<uint8>& pixels, uint32 x, uint32 y) noexcept {
    const Pixel pixel = At(pixels, x, y);
    return pixel.r < BackgroundLimit && pixel.g < BackgroundLimit && pixel.b < BackgroundLimit;
}

bool IsWhite(const Array<uint8>& pixels, uint32 x, uint32 y) noexcept {
    const Pixel pixel = At(pixels, x, y);
    return pixel.r > WhiteFloor && pixel.g > WhiteFloor && pixel.b > WhiteFloor;
}

// channel(0 빨강, 1 초록, 2 파랑)이 나머지보다 뚜렷이 큰지다.
bool Dominant(const Array<uint8>& pixels, uint32 x, uint32 y, int channel) noexcept {
    const Pixel pixel = At(pixels, x, y);
    const int values[3] = {pixel.r, pixel.g, pixel.b};
    for (int index = 0; index < 3; ++index) {
        if (index != channel && values[index] + DominantMargin > values[channel]) {
            return false;
        }
    }

    return true;
}

// 그릴 경로다.
enum class Path : uint8 {
    Offscreen,
    Window
};

// 장치 자원과 그리기 도우미다.
struct Bench {
    Device* device = nullptr;
    sample::Renderer* renderer = nullptr;
    TextureHandle target;
    TextureHandle capture;
    SwapChainHandle swapChain;
    BufferHandle winding;

    // path로 프레임 하나를 그리고 이미지(첫 행이 위)를 out에 읽는다. 창 경로는 백버퍼를 capture에
    // 복사해 읽는다.
    template <class Draw>
    bool Render(Path path, Draw&& draw, Array<uint8>& out) noexcept {
        const ClearColor clear = {0.0f, 0.0f, 0.0f, 1.0f};
        auto frame = path == Path::Window ? device->BeginFrame(swapChain, clear)
                                          : device->BeginOffscreen(target, clear);
        if (!frame) {
            return false;
        }

        draw(*frame.Value());
        bool copied = true;
        if (path == Path::Window) {
            copied = frame.Value()->CopyPassTarget(capture).IsOk();
        }
        const bool ended = device->EndFrame().IsOk();
        return copied && ended && out.TryResize(ImageBytes).IsOk() &&
               device->ReadPixels(path == Path::Window ? capture : target, out.View()).IsOk();
    }

    Result<PipelineHandle> WindingPipeline(CullMode cull) noexcept {
        const VertexAttribute attributes[] = {
            {0, VertexFormat::Float3, 0, ROCKS_OFFSET_OF(Vertex, position)},
            {1, VertexFormat::Float2, 0, ROCKS_OFFSET_OF(Vertex, uv)},
            {2, VertexFormat::Float4, 0, ROCKS_OFFSET_OF(Vertex, color)},
        };
        const VertexStream streams[] = {{sizeof(Vertex), VertexStepRate::Vertex}};
        PipelineDesc desc;
        desc.vertex = {sample::BuiltinShaderHlsl(), sample::BuiltinVertexGlsl()};
        desc.pixel = {sample::BuiltinShaderHlsl(), sample::BuiltinPixelGlsl()};
        desc.layout = {attributes, streams};
        desc.drawConstantBytes = sizeof(sample::ObjectConstants);
        desc.colorFormats[0] = PixelFormat::RGBA8UnormSrgb;
        desc.raster.cullMode = cull;
        return device->CreatePipeline(desc);
    }
};

// 한 장면을 두 경로로 그려 offscreen 결과를 out에 남기고, 창 결과가 byte 단위로 같은지 본다.
template <class Draw>
bool RenderBoth(Bench& bench, bool window, const char* what, Draw&& draw,
                Array<uint8>& out) noexcept {
    const bool drawn = bench.Render(Path::Offscreen, draw, out);
    Check(drawn, what);
    if (!drawn) {
        printf("    %.*s\n", static_cast<int>(bench.device->LastError().ByteCount()),
               bench.device->LastError().Data());
        return false;
    }
    if (!window) {
        return true;
    }

    Array<uint8> windowed(out.GetAllocator());
    const bool windowDrawn = bench.Render(Path::Window, draw, windowed);
    Check(windowDrawn, what);
    if (!windowDrawn) {
        printf("    window path: %.*s\n", static_cast<int>(bench.device->LastError().ByteCount()),
               bench.device->LastError().Data());
        return true;
    }

    usize different = 0;
    for (usize index = 0; index < ImageBytes; ++index) {
        different += windowed[index] != out[index] ? 1u : 0u;
    }
    if (different != 0) {
        ++failures;
        printf("  [FAIL] %s culling: %s: window and offscreen differ in %zu bytes\n", backendName,
               what, different);
    }
    return true;
}

void RunWindingChecks(Bench& bench, bool window, Array<uint8>& pixels) noexcept {
    struct Case {
        CullMode cull;
        const char* what;
        bool counterClockwiseVisible;
        bool clockwiseVisible;
    };
    const Case cases[] = {
        {CullMode::None, "cull none draws both windings", true, true},
        {CullMode::Back, "cull back keeps the screen counter-clockwise triangle", true, false},
        {CullMode::Front, "cull front keeps the screen clockwise triangle", false, true},
    };
    for (const Case& test : cases) {
        auto pipeline = bench.WindingPipeline(test.cull);
        Check(pipeline.IsOk(), "winding pipeline creation");
        if (!pipeline) {
            continue;
        }

        auto draw = [&](CommandContext& commands) {
            commands.SetPipeline(pipeline.Value());
            // 단위 변환·흰색이다(정점이 NDC 그대로 그려진다).
            const sample::ObjectConstants identity;
            Check(commands.SetDrawConstants(DrawConstantBytes(identity)).IsOk(),
                  "identity draw constants are accepted");
            commands.SetTexture({});
            const VertexBufferBinding binding{bench.winding, 0};
            commands.SetVertexBuffers(0, Span<const VertexBufferBinding>(&binding, 1));
            commands.Draw(WindingVertexCount);
        };
        if (RenderBoth(bench, window, test.what, draw, pixels)) {
            const bool ccw = IsWhite(pixels, CounterClockwiseX, WindingY);
            const bool cw = IsWhite(pixels, ClockwiseX, WindingY);
            const bool culledCcw = IsBackground(pixels, CounterClockwiseX, WindingY);
            const bool culledCw = IsBackground(pixels, ClockwiseX, WindingY);
            const bool lowerEmpty = IsBackground(pixels, CounterClockwiseX, FlippedWindingY) &&
                                    IsBackground(pixels, ClockwiseX, FlippedWindingY);
            Check((test.counterClockwiseVisible ? ccw : culledCcw) &&
                      (test.clockwiseVisible ? cw : culledCw) && lowerEmpty,
                  test.what);
        }
        bench.device->Retire(pipeline.Value());
    }
}

// 셰이더가 보는 앞면이 화면 반시계인지 본다(D3D12 FrontCounterClockwise와 같은 뜻).
void RunFacingChecks(Bench& bench, bool window, Array<uint8>& pixels) noexcept {
    const VertexAttribute attributes[] = {
        {0, VertexFormat::Float3, 0, ROCKS_OFFSET_OF(Vertex, position)}};
    const VertexStream streams[] = {{sizeof(Vertex), VertexStepRate::Vertex}};
    PipelineDesc desc;
    const StringView hlsl(FacingHlsl, sizeof(FacingHlsl) - 1);
    desc.vertex = {hlsl, StringView(FacingVertexGlsl, sizeof(FacingVertexGlsl) - 1)};
    desc.pixel = {hlsl, StringView(FacingPixelGlsl, sizeof(FacingPixelGlsl) - 1)};
    desc.layout = {attributes, streams};
    desc.colorFormats[0] = PixelFormat::RGBA8UnormSrgb;
    auto pipeline = bench.device->CreatePipeline(desc);
    Check(pipeline.IsOk(), "facing pipeline creation");
    if (!pipeline) {
        printf("    %.*s\n", static_cast<int>(bench.device->LastError().ByteCount()),
               bench.device->LastError().Data());
        return;
    }

    auto draw = [&](CommandContext& commands) {
        commands.SetPipeline(pipeline.Value());
        const VertexBufferBinding binding{bench.winding, 0};
        commands.SetVertexBuffers(0, Span<const VertexBufferBinding>(&binding, 1));
        commands.Draw(WindingVertexCount);
    };
    const char* what = "shader front face is the screen counter-clockwise triangle";
    if (RenderBoth(bench, window, what, draw, pixels)) {
        Check(Dominant(pixels, CounterClockwiseX, WindingY, 1) &&
                  Dominant(pixels, ClockwiseX, WindingY, 0),
              what);
    }

    bench.device->Retire(pipeline.Value());
}

// 샘플 렌더러의 음수 행렬식 컬 반전이다. 삼각형을 로컬 Y로 뒤집으면 화면에서 시계로 감기므로
// 반전하지 않으면 뒤면 컬로 사라진다.
void RunDeterminantChecks(Bench& bench, bool window, Array<uint8>& pixels) noexcept {
    const Mat4 mirror = Mat4::Scale(Vec3(1.0f, -1.0f, 1.0f));
    struct Case {
        const char* what;
        bool mirrorItem;
        bool mirrorView;
        // 화면이 좌우로 뒤집혀 보이는지(왼쪽 아래가 파랑)와 컬을 뒤집은 항목 수다.
        bool mirroredImage;
        uint32 reversed;
    };
    const Case cases[] = {
        {"negative scale item is drawn with reversed culling", true, false, true, 1},
        {"mirrored view reverses culling", false, true, true, 1},
        {"negative scale under a mirrored view keeps normal culling", true, true, false, 0},
    };
    for (const Case& test : cases) {
        sample::RenderView view;
        view.worldToView = test.mirrorView ? mirror : Mat4::Identity();
        view.viewProjection = view.worldToView * FillScreen();
        sample::RenderItem item;
        item.mesh = sample::MeshKind::Triangle;
        item.world = test.mirrorItem ? mirror : Mat4::Identity();
        uint32 reversed = NumericLimits<uint32>::Max();
        auto draw = [&](CommandContext& commands) {
            bench.renderer->Draw(commands, view, Span<const sample::RenderItem>(&item, 1));
            reversed = bench.renderer->LastStats().reversedCulling;
        };
        if (RenderBoth(bench, window, test.what, draw, pixels)) {
            const int left = test.mirroredImage ? 2 : 1;
            const int right = test.mirroredImage ? 1 : 2;
            Check(reversed == test.reversed && Dominant(pixels, 32, 4, 0) &&
                      Dominant(pixels, 3, 61, left) && Dominant(pixels, 60, 61, right),
                  test.what);
        }
    }
}

// 행 우선 행렬 규약: 비균등 scale(Y 0.5, Z 0.25) 뒤 이동(Y +0.25, Z +0.25)한 사각형은 clip x [0,
// 1], y [0.25, 0.75], 곧 열 32–63·행 8–23을 덮는다. 전치되면 이동이 w로 가서 다른 영역이 된다.
void RunRowMajorChecks(Bench& bench, bool window, Array<uint8>& pixels) noexcept {
    sample::RenderView view;
    view.viewProjection = FillScreen();
    sample::RenderItem item;
    item.mesh = sample::MeshKind::Quad;
    item.world = Mat4::Scale(Vec3(1.0f, 0.5f, 0.25f)) * Mat4::Translation(Vec3(0.0f, 0.25f, 0.25f));
    auto draw = [&](CommandContext& commands) {
        bench.renderer->Draw(commands, view, Span<const sample::RenderItem>(&item, 1));
    };
    const char* what = "asymmetric row-major transform covers the expected region";
    if (RenderBoth(bench, window, what, draw, pixels)) {
        const bool inside =
            IsWhite(pixels, 33, 9) && IsWhite(pixels, 62, 22) && IsWhite(pixels, 48, 16);
        const bool outside = IsBackground(pixels, 30, 16) && IsBackground(pixels, 48, 6) &&
                             IsBackground(pixels, 48, 26) && IsBackground(pixels, 16, 40);
        Check(inside && outside, what);
    }
}

} // namespace

uint32 RunCullingChecks(const char* name, Device& device, sample::Renderer& renderer,
                        Allocator& allocator) noexcept {
    backendName = name;
    failures = 0;
    Bench bench;
    bench.device = &device;
    bench.renderer = &renderer;

    TextureDesc targetDesc;
    targetDesc.width = Size;
    targetDesc.height = Size;
    targetDesc.format = PixelFormat::RGBA8UnormSrgb;
    targetDesc.usage = TextureUsage::RenderTarget;
    auto target = device.CreateTexture(targetDesc, {});
    auto capture = device.CreateTexture(targetDesc, {});
    BufferDesc bufferDesc;
    bufferDesc.size = sizeof(WindingVertices);
    auto winding = device.CreateBuffer(
        bufferDesc, Span<const uint8>(reinterpret_cast<const uint8*>(WindingVertices),
                                      sizeof(WindingVertices)));
    Check(target.IsOk() && capture.IsOk() && winding.IsOk(), "resource creation");
    if (!target || !capture || !winding) {
        return failures;
    }

    bench.target = target.Value();
    bench.capture = capture.Value();
    bench.winding = winding.Value();

    // 창 경로: 숨김 창의 스왑체인에 그리고 CopyPassTarget으로 읽는다.
    platform::WindowDesc windowDesc;
    windowDesc.title = "Rocks culling self-test";
    windowDesc.width = Size;
    windowDesc.height = Size;
    windowDesc.resizable = false;
    windowDesc.visible = false;
    const char* unavailable = WindowPresentUnavailableReason(device);
    auto created = unavailable == nullptr
                       ? platform::Window::Create(allocator, windowDesc)
                       : Result<UniquePtr<platform::Window>>::Failure(ErrorCode::Unsupported);
    bool window = false;
    if (unavailable != nullptr) {
        printf("  [NOT RUN] %s culling: window path (%s)\n", name, unavailable);
    } else if (created && (created.Value()->Width() != Size || created.Value()->Height() != Size)) {
        printf("  [NOT RUN] %s culling: window path (client area is %ux%u, not %ux%u)\n", name,
               created.Value()->Width(), created.Value()->Height(), Size, Size);
    } else if (created) {
        const platform::NativeWindowHandle native = created.Value()->NativeHandle();
        SwapChainDesc swapDesc;
        swapDesc.surface.window = native.window;
        swapDesc.surface.instance = native.instance;
        swapDesc.width = Size;
        swapDesc.height = Size;
        swapDesc.vsync = false;
        auto swapChain = device.CreateSwapChain(swapDesc);
        Check(swapChain.IsOk(), "swap chain creation");
        if (swapChain) {
            bench.swapChain = swapChain.Value();
            window = true;
        }
    } else {
        printf("  [NOT RUN] %s culling: window path (window unavailable)\n", name);
    }

    // CopyPassTarget 음성 대조: 패스 대상 자신과 크기가 다른 대상은 거부한다.
    {
        TextureDesc smallDesc = targetDesc;
        smallDesc.width = Size / 2;
        auto small = device.CreateTexture(smallDesc, {});
        auto frame = device.BeginOffscreen(bench.target, {});
        bool rejected = false;
        if (frame && small) {
            rejected =
                frame.Value()->CopyPassTarget(bench.target).Error() == ErrorCode::InvalidArgument &&
                frame.Value()->CopyPassTarget(small.Value()).Error() == ErrorCode::InvalidArgument;
        }
        if (frame) {
            rejected = device.EndFrame().IsOk() && rejected;
        }
        if (small) {
            device.Retire(small.Value());
        }
        Check(rejected, "copy pass target rejects the pass target itself and a size mismatch");
    }

    Array<uint8> pixels(allocator);
    RunWindingChecks(bench, window, pixels);
    RunFacingChecks(bench, window, pixels);
    RunDeterminantChecks(bench, window, pixels);
    RunRowMajorChecks(bench, window, pixels);

    if (window) {
        device.Retire(bench.swapChain);
    }
    device.Retire(bench.winding);
    device.Retire(bench.capture);
    device.Retire(bench.target);
    Check(device.WaitIdle().IsOk(), "wait idle");
    printf("  %s culling: window path %s, %u failure%s\n", name, window ? "checked" : "skipped",
           failures, failures == 1 ? "" : "s");
    return failures;
}

} // namespace sandbox
