#include <Core/Memory/SystemAllocator.h>
#include <ShaderFormatVulkan/VulkanShaderCompiler.h>
#include <TestSupport/Test.h>
#include <stdio.h>

using namespace rocks;
namespace vulkan_shader_compiler_tests_file {

using namespace rocks::graphics;

// 개발용 Vulkan 셰이더 경로(Docs/Design/VulkanGraphics.ko.md 13절)를 실제 DXC·SPIRV-Tools로
// 확인한다. 도구 저장소가 없으면(bootstrap 전, CI) SKIP이며 실패가 아니다.
namespace {

// 샘플 렌더러와 같은 모양이다. 정점 입력을 location과 다른 순서로 선언해 시맨틱 번호로 location을
// 다시 쓰는지 본다.
constexpr char SampleHlsl[] = R"(
cbuffer DrawConstants : register(b0) {
    float4x4 transform;
    float4 color;
};

struct VSInput {
    float4 tint : ATTRIBUTE2;
    float3 position : ATTRIBUTE0;
    int2 cell : ATTRIBUTE5;
};

struct VSOutput {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

Texture2D albedo : register(t0);
SamplerState linearClamp : register(s0);

VSOutput VSMain(VSInput input, uint vertexId : SV_VertexID) {
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), transform);
    output.color = input.tint * color + float(input.cell.x + vertexId);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    return albedo.Sample(linearClamp, float2(0.5, 0.5)) * input.color;
}
)";

constexpr char BadSemanticHlsl[] = R"(
struct VSInput { float3 position : TEXCOORD0; };
float4 VSMain(VSInput input) : SV_Position { return float4(input.position, 1.0); }
)";

// 지운 SPV_GOOGLE 장식(OpDecorateString)이 남았는지 본다.
bool HasDecorateString(const Array<uint32>& code) noexcept {
    constexpr uint32 OpDecorateString = 5632;
    for (usize offset = 5; offset < code.Size();) {
        const uint32 count = code[offset] >> 16;
        if ((code[offset] & 0xFFFFu) == OpDecorateString) {
            return true;
        }

        offset += count == 0 ? code.Size() : count;
    }

    return false;
}

const ShaderVertexInput* FindInput(const CompiledShader& shader, uint32 location) noexcept {
    for (const ShaderVertexInput& input : shader.vertexInputs) {
        if (input.location == location) {
            return &input;
        }
    }

    return nullptr;
}

} // namespace

TEST(VulkanShaderCompiler_RewritesLocationsConstantsAndBindings) {
    SystemAllocator allocator;
    String diagnostics(allocator);
    auto compiler = shader::VulkanShaderCompiler::Create(allocator, diagnostics);
    if (!compiler) {
        printf("[SKIP] VulkanShaderCompiler_RewritesLocationsConstantsAndBindings: %s\n",
               diagnostics.CStr());
        return;
    }

    const StringView source(SampleHlsl, sizeof(SampleHlsl) - 1);
    CompiledShader vertex(allocator);
    auto compiledVertex =
        compiler.Value()->CompileSpirv(ShaderStage::Vertex, source, vertex, diagnostics);
    if (!compiledVertex) {
        printf("    %s\n", diagnostics.CStr());
    }

    REQUIRE(compiledVertex);
    CHECK(vertex.vertexInputs.Size() == 3);
    const ShaderVertexInput* position = FindInput(vertex, 0);
    const ShaderVertexInput* tint = FindInput(vertex, 2);
    const ShaderVertexInput* cell = FindInput(vertex, 5);
    CHECK(position != nullptr && position->type == VertexShaderType::Float);
    CHECK(tint != nullptr && tint->type == VertexShaderType::Float);
    CHECK(cell != nullptr && cell->type == VertexShaderType::Int);
    // 그리기 상수는 push constant(float4x4 + float4 = 80 byte)이고 descriptor가 아니다.
    CHECK(vertex.rootConstantBytes == 80);
    CHECK(vertex.bindings.Size() == 0);
    CHECK(!HasDecorateString(vertex.code));

    CompiledShader pixel(allocator);
    auto compiledPixel =
        compiler.Value()->CompileSpirv(ShaderStage::Pixel, source, pixel, diagnostics);
    REQUIRE(compiledPixel);
    // 픽셀 단계는 set 1이고 종류 순서(SRV 텍스처 → 샘플러)로 0부터 매긴다.
    REQUIRE(pixel.bindings.Size() == 2);
    bool texture = false;
    bool sampler = false;
    for (const ShaderBinding& binding : pixel.bindings) {
        texture = texture || (binding.set == 1 && binding.binding == 0 &&
                              binding.kind == ShaderBindingKind::SampledImage);
        sampler = sampler || (binding.set == 1 && binding.binding == 1 &&
                              binding.kind == ShaderBindingKind::Sampler);
    }

    CHECK(texture && sampler);
    CHECK(pixel.vertexInputs.Size() == 0);
}

TEST(VulkanShaderCompiler_RejectsInputsWithoutAttributeSemantic) {
    SystemAllocator allocator;
    String diagnostics(allocator);
    auto compiler = shader::VulkanShaderCompiler::Create(allocator, diagnostics);
    if (!compiler) {
        printf("[SKIP] VulkanShaderCompiler_RejectsInputsWithoutAttributeSemantic: %s\n",
               diagnostics.CStr());
        return;
    }

    CompiledShader vertex(allocator);
    auto compiled = compiler.Value()->CompileSpirv(
        ShaderStage::Vertex, StringView(BadSemanticHlsl, sizeof(BadSemanticHlsl) - 1), vertex,
        diagnostics);
    CHECK(compiled.Error() == ErrorCode::InvalidArgument);
    CHECK(diagnostics.View().Find(StringView("ATTRIBUTE0-15")) != StringView::NotFound);
    // 컴파일 오류는 DXC 진단과 함께 DependencyFailed다.
    diagnostics.Clear();
    constexpr char Broken[] = "float4 VSMain() : SV_Position { return undefined_name; }";
    CHECK(compiler.Value()
              ->CompileSpirv(ShaderStage::Vertex, StringView(Broken, sizeof(Broken) - 1), vertex,
                             diagnostics)
              .Error() == ErrorCode::DependencyFailed);
    CHECK(diagnostics.ByteCount() != 0);
}
} // namespace vulkan_shader_compiler_tests_file
