#include "ConstantSelfTest.h"
#include "SandboxShaders.h"

#include <Core/Containers/Array.h>
#include <Graphics/ShaderBindings.h>
#include <Platform/Memory.h>
#include <stdio.h>

// 상수 버퍼 self-test다(Docs/Design/RenderBase 9.11절 GX6). 셰이더는 Constant.rsf·
// ConstantMany.rsf·ConstantWide.rsf이고, slot은 장치 형식의 값 1에서 이름으로 찾는다(형식마다
// 다르다). 색은 k/255라 UNORM8에서 정확하다(허용 오차 ±2).
namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// offscreen 대상 크기다(가로 16픽셀 — 많은 상수 버퍼 그리기는 픽셀 x마다 slot 하나를 낸다).
constexpr uint32 TargetWidth = 16;
constexpr uint32 TargetHeight = 4;
constexpr int ColorTolerance = 2;
// 많은 상수 버퍼 그리기의 단계당 수다(형식 정의 최소 Vulkan·GLES 12, D3D12 14).
constexpr uint32 ManyConstantBuffers = 12;
constexpr uint32 WideConstantBuffers = 14;
// compute 결과 원소 수(ConstantCS numthreads)다.
constexpr uint32 ComputeElements = 4;
// 루트 상수 계약 최대(D3D12 root 예산을 가장 좁게 쓰는 선언)다.
constexpr uint32 FullDrawConstants = MaxDrawConstantBytes;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s constants: %s\n", backendName, what);
    }
}

struct Rgba {
    uint8 r = 0;
    uint8 g = 0;
    uint8 b = 0;
};

float Unit(uint32 value) noexcept {
    return static_cast<float>(value) / 255.0f;
}

struct alignas(16) Float4 {
    float values[4] = {};
};

Float4 Color(uint32 r, uint32 g, uint32 b, uint32 a = 0) noexcept {
    return Float4{{Unit(r), Unit(g), Unit(b), Unit(a)}};
}

// Lighting cbuffer(32 byte)다.
struct alignas(16) Lighting {
    Float4 scale;
    Float4 bias;
};

struct alignas(16) Params {
    uint32 values[4] = {};
    uint32 more[4] = {};
};

Span<const uint8> BytesOf(const void* data, usize size) noexcept {
    return Span<const uint8>(static_cast<const uint8*>(data), size);
}

bool Near(uint8 actual, uint8 expected) noexcept {
    const int difference = static_cast<int>(actual) - static_cast<int>(expected);
    return difference <= ColorTolerance && difference >= -ColorTolerance;
}

struct Bench {
    explicit Bench(Allocator& allocator) noexcept : pixels(allocator) {
    }

    Device* device = nullptr;
    TextureHandle target;
    Array<uint8> pixels;

    bool Read() noexcept {
        return pixels.TryResize(static_cast<usize>(TargetWidth) * TargetHeight * 4).IsOk() &&
               device->ReadPixels(target, pixels.View()).IsOk();
    }

    bool PixelIs(uint32 x, uint32 y, Rgba expected, const char* label) const noexcept {
        const usize offset = (static_cast<usize>(y) * TargetWidth + x) * 4;
        const bool same = Near(pixels[offset], expected.r) &&
                          Near(pixels[offset + 1], expected.g) &&
                          Near(pixels[offset + 2], expected.b);
        if (!same) {
            printf("    %s pixel (%u, %u) is (%u, %u, %u), expected (%u, %u, %u)\n", label, x, y,
                   pixels[offset], pixels[offset + 1], pixels[offset + 2], expected.r, expected.g,
                   expected.b);
        }

        return same;
    }

    // 왼쪽 반(left)과 오른쪽 반(right)이 각 색인지다.
    bool HalvesAre(Rgba left, Rgba right, const char* label) const noexcept {
        return PixelIs(TargetWidth / 4, TargetHeight / 2, left, label) &&
               PixelIs(TargetWidth * 3 / 4, TargetHeight / 2, right, label);
    }
};

// 타입 layout에서 이름의 상수 버퍼 slot이다(없으면 MaxConstantBufferSlots).
uint32 SlotOf(const shaders::ShaderTypeDesc& type, const char* name) noexcept {
    const shader::ShaderBindingLayout* layout = shaders::Layout(type);
    if (layout == nullptr) {
        return MaxConstantBufferSlots;
    }

    auto slot = FindConstantBufferSlot(*layout, StringView(name));
    return slot ? slot.Value() : MaxConstantBufferSlots;
}

Viewport Half(bool right) noexcept {
    Viewport viewport;
    viewport.x = right ? static_cast<float>(TargetWidth / 2) : 0.0f;
    viewport.width = static_cast<float>(TargetWidth / 2);
    viewport.height = static_cast<float>(TargetHeight);
    return viewport;
}

Result<PipelineHandle> GraphicsPipeline(Device& device, const shaders::ShaderTypeDesc& vertex,
                                        const shaders::ShaderTypeDesc& pixel,
                                        uint32 drawConstantBytes) noexcept {
    PipelineDesc desc;
    desc.colorFormats[0] = PixelFormat::RGBA8Unorm;
    desc.drawConstantBytes = drawConstantBytes;
    return shaders::CreateGraphicsPipeline(device, vertex, pixel, desc);
}

// 기본 그리기: handle 두 수명, 기록 순서 갱신, 기록 명령, 파이프라인 전환, 빈·작은 버퍼 무시.
void RunBasicDraws(Device& device, Bench& bench) noexcept {
    const uint32 placement = SlotOf(shaders::ConstantVS, "Placement");
    const uint32 material = SlotOf(shaders::ConstantPS, "Material");
    const uint32 lighting = SlotOf(shaders::ConstantPS, "Lighting");
    Check(placement < MaxConstantBufferSlots && material < MaxConstantBufferSlots &&
              lighting < MaxConstantBufferSlots,
          "constant buffer slots are found in the binding layouts");
    auto pipeline =
        GraphicsPipeline(device, shaders::ConstantVS, shaders::ConstantPS, sizeof(Float4));
    auto alternate = GraphicsPipeline(device, shaders::ConstantVS, shaders::ConstantAltPS, 0);
    Check(pipeline.IsOk() && alternate.IsOk(), "constant buffer pipelines are created");
    if (!pipeline || !alternate || placement >= MaxConstantBufferSlots ||
        material >= MaxConstantBufferSlots || lighting >= MaxConstantBufferSlots) {
        return;
    }

    // 셰이더: shift + color * scale + bias + tint(루트 상수).
    const Float4 shift = Color(20, 0, 0);
    const Float4 red = Color(200, 0, 0);
    const Float4 green = Color(0, 160, 0);
    const Lighting light{Color(255, 255, 255), Color(0, 0, 40)};
    const Lighting dim{Color(128, 128, 128), Color(0, 0, 0)};
    const Float4 tint = Color(0, 10, 0);
    auto shiftBuffer = device.CreateConstantBuffer(ConstantBufferLifetime::SingleFrame,
                                                   ConstantBufferBytes(shift));
    auto materialBuffer =
        device.CreateConstantBuffer(ConstantBufferLifetime::MultiFrame, ConstantBufferBytes(red));
    auto lightBuffer = device.CreateConstantBuffer(ConstantBufferLifetime::SingleFrame,
                                                   ConstantBufferBytes(light));
    auto small = device.CreateConstantBuffer(ConstantBufferLifetime::SingleFrame,
                                             ConstantBufferBytes(green));
    Check(shiftBuffer.IsOk() && materialBuffer.IsOk() && lightBuffer.IsOk() && small.IsOk(),
          "constant buffers are created");
    // 음성: SingleFrame 갱신·크기 다른 갱신·크기 규칙 위반.
    Check(device.UpdateConstantBuffer(shiftBuffer.Value(), ConstantBufferBytes(shift)).Error() ==
              ErrorCode::InvalidArgument,
          "SingleFrame constant buffers cannot be updated");
    Check(device.UpdateConstantBuffer(materialBuffer.Value(), ConstantBufferBytes(light)).Error() ==
              ErrorCode::InvalidArgument,
          "updates keep the constant buffer size");
    Check(device.CreateConstantBuffer(ConstantBufferLifetime::MultiFrame, BytesOf(&red, 12))
                  .Error() == ErrorCode::InvalidArgument,
          "constant buffer sizes are 16-byte multiples");
    if (!shiftBuffer || !materialBuffer || !lightBuffer || !small) {
        return;
    }

    // 1) 기록 순서 갱신: 왼쪽 반은 옛 내용(red), 갱신 뒤 오른쪽 반은 새 내용(green).
    const uint64 ignoredBefore = device.IgnoredDraws();
    auto frame = device.BeginOffscreen(bench.target, {});
    Check(frame.IsOk(), "offscreen recording begins");
    if (!frame) {
        return;
    }

    CommandContext& commands = *frame.Value();
    commands.SetPipeline(pipeline.Value());
    Check(commands.SetDrawConstants(ConstantBufferBytes(tint)).IsOk(), "root constants are set");
    // 아무것도 묶지 않은 그리기는 무시한다.
    commands.Draw(3);
    commands.SetConstantBuffer(ShaderStage::Vertex, placement, shiftBuffer.Value());
    commands.SetConstantBuffer(ShaderStage::Pixel, material, materialBuffer.Value());
    // 선언 32 byte인 Lighting에 16 byte 버퍼는 작아서 무시한다.
    commands.SetConstantBuffer(ShaderStage::Pixel, lighting, small.Value());
    commands.Draw(3);
    commands.SetConstantBuffer(ShaderStage::Pixel, lighting, lightBuffer.Value());
    commands.SetViewport(Half(false));
    commands.Draw(3);
    Check(device.UpdateConstantBuffer(materialBuffer.Value(), ConstantBufferBytes(green)).IsOk(),
          "MultiFrame constant buffer updates while recording");
    commands.SetViewport(Half(true));
    commands.Draw(3);
    Check(device.EndFrame().IsOk() && bench.Read(), "recording-order update frame");
    Check(device.IgnoredDraws() == ignoredBefore + 2,
          "draws with unbound or too-small constant buffers are ignored");
    // 왼쪽: 20 + 200 + 0, 0 + 0 + 10, 0 + 40 → (220, 10, 40). 오른쪽: (20, 170, 40).
    Check(bench.HalvesAre({220, 10, 40}, {20, 170, 40}, "update order"),
          "draws before an update read the old contents and draws after read the new");

    // 2) 기록 명령(SetConstantData): 반마다 다른 Lighting을 그 자리에서 올린다. 그다음 파이프라인을
    //    바꿔도 묶음이 남는다(Alt PS: shift + color × 0.5 + bias).
    frame = device.BeginOffscreen(bench.target, {});
    Check(frame.IsOk(), "second recording begins");
    if (!frame) {
        return;
    }

    CommandContext& again = *frame.Value();
    again.SetPipeline(pipeline.Value());
    Check(again.SetDrawConstants(ConstantBufferBytes(tint)).IsOk(), "root constants are set");
    again.SetConstantBuffer(ShaderStage::Vertex, placement, shiftBuffer.Value());
    again.SetConstantBuffer(ShaderStage::Pixel, material, materialBuffer.Value());
    Check(again.SetConstantData(ShaderStage::Pixel, lighting, ConstantBufferBytes(dim)).IsOk(),
          "SetConstantData binds inline constants");
    again.SetViewport(Half(false));
    again.Draw(3);
    const bool sameSlots = SlotOf(shaders::ConstantAltPS, "Material") == material &&
                           SlotOf(shaders::ConstantAltPS, "Lighting") == lighting;
    again.SetPipeline(alternate.Value());
    again.SetViewport(Half(true));
    again.Draw(3);
    Check(again.SetConstantData(ShaderStage::Pixel, lighting, BytesOf(&dim, 8)).Error() ==
              ErrorCode::InvalidArgument,
          "SetConstantData keeps the size rules");
    Check(device.EndFrame().IsOk() && bench.Read(), "inline constants frame");
    // 왼쪽: green × 128/255 = (0, 80, 0) + shift 20 + tint 10 → (20, 90, 0).
    // 오른쪽(Alt): 20 + green × 0.5 = (20, 80, 0) + bias 0.
    Check(bench.PixelIs(TargetWidth / 4, TargetHeight / 2, {20, 90, 0}, "inline"),
          "inline constants are drawn");
    if (sameSlots) {
        Check(bench.PixelIs(TargetWidth * 3 / 4, TargetHeight / 2, {20, 80, 0}, "switch"),
              "bindings survive a pipeline switch");
    } else {
        printf("  [NOT RUN] %s constants: pipeline switch (the two pixel shaders place the "
               "buffers at different slots)\n",
               backendName);
    }

    device.Retire(shiftBuffer.Value());
    device.Retire(materialBuffer.Value());
    device.Retire(lightBuffer.Value());
    device.Retire(small.Value());
    // Retire한 handle은 빈 slot이다(갱신은 StaleHandle).
    Check(device.UpdateConstantBuffer(materialBuffer.Value(), ConstantBufferBytes(red)).Error() ==
              ErrorCode::StaleHandle,
          "retired constant buffers are stale");
    device.Retire(pipeline.Value());
    device.Retire(alternate.Value());
}

// compute: 정수 상수 버퍼(handle과 기록 명령)를 읽어 raw 버퍼에 쓴다.
void RunCompute(Device& device, Allocator& allocator) noexcept {
    if (!device.Caps().features.computeShaders) {
        printf("  [NOT RUN] %s constants: compute (no compute shaders)\n", backendName);
        return;
    }

    ComputePipelineDesc desc;
    desc.bindings.writableBuffers[0] = "Output";
    auto pipeline = shaders::CreateComputePipeline(device, shaders::ConstantCS, desc);
    const uint32 params = SlotOf(shaders::ConstantCS, "Params");
    BufferDesc bufferDesc;
    bufferDesc.size = ComputeElements * sizeof(uint32);
    bufferDesc.usage = BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Raw;
    const uint32 zeros[ComputeElements] = {};
    auto buffer = device.CreateBuffer(bufferDesc, BytesOf(zeros, sizeof(zeros)));
    BufferViewDesc viewDesc;
    viewDesc.kind = BufferViewKind::Raw;
    viewDesc.access = ViewAccess::ReadWrite;
    auto view = buffer ? device.CreateBufferView(buffer.Value(), viewDesc)
                       : Result<BufferViewHandle>::Failure(ErrorCode::InvalidArgument);
    Check(pipeline.IsOk() && buffer.IsOk() && view.IsOk() && params < MaxConstantBufferSlots,
          "compute pipeline and output buffer");
    if (!pipeline || !buffer || !view || params >= MaxConstantBufferSlots) {
        return;
    }

    const Params first{{1, 2, 3, 4}, {100, 200, 300, 400}};
    const Params second{{7, 8, 9, 10}, {5, 6, 7, 8}};
    auto handle = device.CreateConstantBuffer(ConstantBufferLifetime::SingleFrame,
                                              ConstantBufferBytes(first));
    Check(handle.IsOk(), "compute constant buffer");
    uint32 out[ComputeElements] = {};
    for (uint32 round = 0; round < 2 && handle; ++round) {
        PassDesc pass;
        auto frame = device.BeginPass(pass);
        if (!frame) {
            Check(false, "compute recording begins");
            break;
        }

        frame.Value()->SetPipeline(pipeline.Value());
        frame.Value()->SetWritableBufferView(0, view.Value());
        if (round == 0) {
            frame.Value()->SetConstantBuffer(ShaderStage::Compute, params, handle.Value());
        } else {
            Check(frame.Value()
                      ->SetConstantData(ShaderStage::Compute, params, ConstantBufferBytes(second))
                      .IsOk(),
                  "compute SetConstantData");
        }

        frame.Value()->Dispatch(1);
        Check(device.EndFrame().IsOk() &&
                  device
                      .ReadBuffer(buffer.Value(), 0,
                                  Span<uint8>(reinterpret_cast<uint8*>(out), sizeof(out)))
                      .IsOk(),
              "compute result readback");
        const Params& expected = round == 0 ? first : second;
        bool same = true;
        for (uint32 index = 0; index < ComputeElements; ++index) {
            same = same && out[index] == expected.values[index] * 3 + expected.more[index];
        }

        if (!same) {
            printf("    compute round %u: %u %u %u %u\n", round, out[0], out[1], out[2], out[3]);
        }

        Check(same, round == 0 ? "compute reads a constant buffer handle"
                               : "compute reads inline constants");
    }

    if (handle) {
        device.Retire(handle.Value());
    }

    (void)allocator;
    device.Retire(view.Value());
    device.Retire(buffer.Value());
    device.Retire(pipeline.Value());
}

// 단계마다 상수 버퍼 count개(VS는 MultiFrame handle, PS는 짝수 slot SingleFrame handle·홀수 slot
// 기록 명령)를 묶어 그린다. PS는 픽셀 x마다 P[x]를, x ≥ count는 VS 합을 낸다. 진단 PS면 D3D12
// 진단 root UAV도 함께다.
void RunManyDraw(Device& device, Bench& bench, const shaders::ShaderTypeDesc& vertex,
                 const shaders::ShaderTypeDesc& pixel, uint32 count, const char* label) noexcept {
    if (!shaders::Has(vertex) || !shaders::Has(pixel)) {
        printf("  [NOT RUN] %s constants: %s (not cooked for this format)\n", backendName, label);
        return;
    }

    char name[8] = {};
    uint32 vertexSlots[WideConstantBuffers] = {};
    uint32 pixelSlots[WideConstantBuffers] = {};
    bool found = true;
    for (uint32 index = 0; index < count; ++index) {
        snprintf(name, sizeof(name), "V%u", index);
        vertexSlots[index] = SlotOf(vertex, name);
        snprintf(name, sizeof(name), "P%u", index);
        pixelSlots[index] = SlotOf(pixel, name);
        found = found && vertexSlots[index] < MaxConstantBufferSlots &&
                pixelSlots[index] < MaxConstantBufferSlots;
    }

    // 루트 상수를 계약 최대로 선언해 D3D12 root 예산을 가장 좁게 쓴다(넘친 CBV는 table로 간다).
    auto pipeline = GraphicsPipeline(device, vertex, pixel, FullDrawConstants);
    char what[128];
    snprintf(what, sizeof(what), "%s pipeline and slots", label);
    Check(found && pipeline.IsOk(), what);
    if (!found || !pipeline) {
        printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
               device.LastError().Data());
        return;
    }

    // VS 값 (16, 8, 4)/255씩, PS 값 ((i + 1) × 16, (count − i) × 16, 128)/255다.
    const Float4 vertexValue = Color(16, 8, 4);
    ConstantBufferHandle vertexBuffers[WideConstantBuffers] = {};
    ConstantBufferHandle pixelBuffers[WideConstantBuffers] = {};
    Float4 pixelValues[WideConstantBuffers] = {};
    bool created = true;
    for (uint32 index = 0; index < count; ++index) {
        auto made = device.CreateConstantBuffer(ConstantBufferLifetime::MultiFrame,
                                                ConstantBufferBytes(vertexValue));
        created = created && made.IsOk();
        vertexBuffers[index] = made ? made.Value() : ConstantBufferHandle{};
        pixelValues[index] = Color((index + 1) * 16, (count - index) * 16, 128, 255);
        if (index % 2 == 0) {
            auto pixelMade = device.CreateConstantBuffer(ConstantBufferLifetime::SingleFrame,
                                                         ConstantBufferBytes(pixelValues[index]));
            created = created && pixelMade.IsOk();
            pixelBuffers[index] = pixelMade ? pixelMade.Value() : ConstantBufferHandle{};
        }
    }

    auto frame = device.BeginOffscreen(bench.target, {});
    bool drawn = created && frame.IsOk();
    if (drawn) {
        CommandContext& commands = *frame.Value();
        commands.SetPipeline(pipeline.Value());
        for (uint32 index = 0; index < count; ++index) {
            commands.SetConstantBuffer(ShaderStage::Vertex, vertexSlots[index],
                                       vertexBuffers[index]);
            if (index % 2 == 0) {
                commands.SetConstantBuffer(ShaderStage::Pixel, pixelSlots[index],
                                           pixelBuffers[index]);
            } else {
                drawn = drawn && commands
                                     .SetConstantData(ShaderStage::Pixel, pixelSlots[index],
                                                      ConstantBufferBytes(pixelValues[index]))
                                     .IsOk();
            }
        }

        const uint64 ignored = device.IgnoredDraws();
        commands.Draw(3);
        drawn = drawn && device.IgnoredDraws() == ignored;
        drawn = device.EndFrame().IsOk() && drawn && bench.Read();
    }

    snprintf(what, sizeof(what), "%s draw is recorded", label);
    Check(drawn, what);
    if (drawn) {
        bool same = true;
        for (uint32 x = 0; x < TargetWidth; ++x) {
            const Rgba expected =
                x < count ? Rgba{static_cast<uint8>((x + 1) * 16),
                                 static_cast<uint8>((count - x) * 16), 128}
                          : Rgba{static_cast<uint8>(16 * count), static_cast<uint8>(8 * count),
                                 static_cast<uint8>(4 * count)};
            same = bench.PixelIs(x, TargetHeight / 2, expected, label) && same;
        }

        snprintf(what, sizeof(what), "%s reads every constant buffer slot", label);
        Check(same, what);
    }

    for (uint32 index = 0; index < count; ++index) {
        device.Retire(vertexBuffers[index]);
        device.Retire(pixelBuffers[index]);
    }

    device.Retire(pipeline.Value());
}

} // namespace

uint32 RunConstantChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    if (!shaders::Has(shaders::ConstantPS)) {
        printf("  [NOT RUN] %s constants: no shader set\n", name);
        return 0;
    }

    const uint64 validationBefore = device.ValidationErrorCount();
    Bench bench(allocator);
    bench.device = &device;
    TextureDesc targetDesc;
    targetDesc.width = TargetWidth;
    targetDesc.height = TargetHeight;
    targetDesc.format = PixelFormat::RGBA8Unorm;
    targetDesc.usage = TextureUsage::RenderTarget;
    auto target = device.CreateTexture(targetDesc, {});
    Check(target.IsOk(), "render target creation");
    if (!target) {
        return failures;
    }

    bench.target = target.Value();
    RunBasicDraws(device, bench);
    RunCompute(device, allocator);
    RunManyDraw(device, bench, shaders::ConstantManyVS, shaders::ConstantManyPS,
                ManyConstantBuffers, "12 constant buffers per stage");
    RunManyDraw(device, bench, shaders::ConstantWideVS, shaders::ConstantWidePS,
                WideConstantBuffers, "14 constant buffers per stage");
    RunManyDraw(device, bench, shaders::ConstantWideVS, shaders::ConstantWideDebugPS,
                WideConstantBuffers, "14 constant buffers with the pixel debug buffer");
    device.Retire(bench.target);
    Check(device.WaitIdle().IsOk() && device.PendingRetireCount() == 0,
          "constant buffer check resources are reclaimed");
    Check(device.ValidationErrorCount() == validationBefore, "no validation errors");
    printf("  %s constant buffers: %u failure%s\n", name, failures, failures == 1 ? "" : "s");
    return failures;
}

} // namespace sandbox
