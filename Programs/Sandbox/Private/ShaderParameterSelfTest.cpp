#include "ShaderParameterSelfTest.h"
#include "SandboxShaders.h"

#include <Core/Containers/Array.h>
#include <Graphics/ShaderBindings.h>
#include <math.h>
#include <stdio.h>

// 생성 셰이더 파라미터 구조체 self-test다(Docs/Design/ShaderSystem 16.2절 SSD58 3). 셰이더는
// GeneratedParameters.rsf이고 상수 구조체는 SandboxShaderParameters.h다.
namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;
using shaders::GeneratedProbeDraw;
using shaders::GeneratedProbeParameters;

constexpr uint32 TargetSize = 4;
// UNORM8 반올림 허용 오차다.
constexpr int ColorTolerance = 2;
constexpr uint32 PixelBytes = 4;
constexpr double UnormMax = 255.0;
constexpr usize MatrixRows = 4;
// 행렬의 기본 칸 값이다(전치해 읽으면 빨강이 1을 넘는다).
constexpr float MatrixFill = 0.9f;
// green 식의 정수 항 나눗수다(GeneratedParametersPS와 같다).
constexpr double IntegerScale = 64.0;
constexpr double SelectScale = 32.0;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s shader parameters: %s\n", backendName, what);
    }
}

// 시험 값이다.
GeneratedProbeParameters ProbeValues() noexcept {
    GeneratedProbeParameters values;
    for (usize row = 0; row < MatrixRows; ++row) {
        for (usize column = 0; column < MatrixRows; ++column) {
            values.transform[row][column] = MatrixFill;
        }
    }

    values.transform[0][1] = 0.5f;
    values.transform[1][1] = 0.25f;
    values.transform[2][1] = 0.1f;
    values.transform[3][1] = 0.05f;
    values.tint = {0.2f, 0.4f, 0.6f};
    values.exposure = 2.0f;
    values.offsets = {7, 3, 5};
    values.layer = 5;
    values.weights[0] = {0.9f, 0.9f, 0.9f, 0.9f};
    values.weights[1] = {0.7f, 0.8f, 0.1f, 0.6f};
    values.weights[2] = {0.3f, 0.5f, 0.4f, 0.2f};
    values.light.direction = {0.9f, 0.8f, 0.15f};
    values.light.intensity = 0.6f;
    values.light.color = {0.5f, 0.9f, 0.9f, 0.9f};
    values.tail = 0.1f;
    return values;
}

GeneratedProbeDraw DrawValues() noexcept {
    GeneratedProbeDraw draw;
    draw.bias = {0.9f, 0.1f, 0.9f};
    draw.scale = 0.5f;
    draw.select = {1, 2, 4, 8};
    return draw;
}

uint8 Unorm(double value) noexcept {
    const double clamped = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
    return static_cast<uint8>(lround(clamped * UnormMax));
}

// GeneratedParametersPS의 CPU 식이다(행 벡터 v·M).
void Expected(const GeneratedProbeParameters& p, const GeneratedProbeDraw& d,
              uint8 (&out)[3]) noexcept {
    const float v[MatrixRows] = {p.tint.x, p.tint.y, p.tint.z, 1.0f};
    double y = 0.0;
    for (usize row = 0; row < MatrixRows; ++row) {
        y += static_cast<double>(v[row]) * p.transform[row][1];
    }

    out[0] = Unorm(y * p.exposure + static_cast<double>(d.bias.y) * d.scale);
    out[1] = Unorm(static_cast<double>(p.weights[1].z) + p.weights[2].w +
                   static_cast<double>(p.offsets.y + p.layer) / IntegerScale +
                   static_cast<double>(d.select.z) / SelectScale);
    out[2] = Unorm(static_cast<double>(p.light.color.x) * p.light.intensity +
                   p.light.direction.z + p.tail);
}

bool Near(uint8 actual, uint8 expected) noexcept {
    const int difference = static_cast<int>(actual) - static_cast<int>(expected);
    return difference <= ColorTolerance && difference >= -ColorTolerance;
}

Span<const uint8> BytesOf(const void* data, usize size) noexcept {
    return Span<const uint8>(static_cast<const uint8*>(data), size);
}

} // namespace

uint32 RunShaderParameterChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    const shader::ShaderBindingLayout* pixel = shaders::Layout(shaders::GeneratedParametersPS);
    if (pixel == nullptr) {
        printf("  [NOT RUN] %s shader parameters: the generated parameter shaders are not in the "
               "shader set\n",
               name);
        return 0;
    }

    // 값 1: cbuffer와 루트 상수의 구조체 identity가 생성 표와 같다(SX8).
    const shader::ShaderParameterLayout& probe =
        shader::ShaderParameterLayoutOf<GeneratedProbeParameters>();
    const shader::ShaderParameterLayout& root =
        shader::ShaderParameterLayoutOf<GeneratedProbeDraw>();
    auto slot = FindConstantBufferSlot(*pixel, probe.name);
    Check(slot.IsOk(), "the generated cbuffer is in the pixel binding layout");
    bool identity = false;
    for (const shader::ShaderBindingResource& resource : pixel->resources) {
        identity = identity || (resource.kind == shader::ShaderResourceKind::ConstantBuffer &&
                                resource.name == probe.name &&
                                resource.structId == probe.structId &&
                                resource.constantBufferBytes == sizeof(GeneratedProbeParameters));
    }

    Check(identity, "the binding layout carries the generated struct identity and size");
    Check(pixel->rootConstantStructId == root.structId &&
              pixel->rootConstantBytes == sizeof(GeneratedProbeDraw),
          "the root constants carry the generated struct identity and size");
    if (!slot) {
        return failures;
    }

    TextureDesc targetDesc;
    targetDesc.width = TargetSize;
    targetDesc.height = TargetSize;
    targetDesc.format = PixelFormat::RGBA8Unorm;
    targetDesc.usage = TextureUsage::RenderTarget;
    auto target = device.CreateTexture(targetDesc, {});
    PipelineDesc desc;
    desc.colorFormats[0] = PixelFormat::RGBA8Unorm;
    desc.drawConstantBytes = sizeof(GeneratedProbeDraw);
    auto pipeline = shaders::CreateGraphicsPipeline(device, shaders::GeneratedParametersVS,
                                                    shaders::GeneratedParametersPS, desc);
    Check(target.IsOk() && pipeline.IsOk(), "target and pipeline are created");
    if (!target || !pipeline) {
        return failures;
    }

    const GeneratedProbeParameters values = ProbeValues();
    const GeneratedProbeDraw draw = DrawValues();
    auto frame = device.BeginOffscreen(target.Value(), {});
    Check(frame.IsOk(), "offscreen recording begins");
    if (frame) {
        CommandContext& commands = *frame.Value();
        commands.SetPipeline(pipeline.Value());
        Check(commands.SetDrawConstants(BytesOf(&draw, sizeof(draw))).IsOk(),
              "generated root constants are set as C++ bytes");
        Check(commands
                  .SetConstantData(ShaderStage::Pixel, slot.Value(),
                                   BytesOf(&values, sizeof(values)))
                  .IsOk(),
              "the generated cbuffer is bound as C++ bytes");
        commands.Draw(3);
        Array<uint8> pixels(allocator);
        Check(device.EndFrame().IsOk() &&
                  pixels.TryResize(static_cast<usize>(TargetSize) * TargetSize * PixelBytes)
                      .IsOk() &&
                  device.ReadPixels(target.Value(), pixels.View()).IsOk(),
              "the frame is drawn and read back");
        uint8 expected[3] = {};
        Expected(values, draw, expected);
        const usize offset =
            (static_cast<usize>(TargetSize / 2) * TargetSize + TargetSize / 2) * PixelBytes;
        const bool read = pixels.Size() > offset + 2;
        const bool same = read && Near(pixels[offset], expected[0]) &&
                          Near(pixels[offset + 1], expected[1]) &&
                          Near(pixels[offset + 2], expected[2]);
        if (!same && read) {
            printf("    pixel is (%u, %u, %u), expected (%u, %u, %u)\n", pixels[offset],
                   pixels[offset + 1], pixels[offset + 2], expected[0], expected[1], expected[2]);
        }

        Check(same, "pixels drawn from generated structs match the CPU result");
    }

    device.Retire(pipeline.Value());
    device.Retire(target.Value());
    if (failures == 0) {
        printf("  %s shader parameters: generated cbuffer and root constants PASS\n", name);
    }

    return failures;
}

} // namespace sandbox
