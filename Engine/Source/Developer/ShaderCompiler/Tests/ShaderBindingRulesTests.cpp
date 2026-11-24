#include "ShaderManagerTestSupport.h"

#include <Base/NumericLimits.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderNugget.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <ShaderCore/ShaderParameterTypes.h>
#include <Base/InitializerList.h>
#include <TestSupport/Test.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// 워커 바인딩 정규화 시험이다(Docs/Design/ShaderSystem 16.1절 SSD29–SSD33 SH2b1, 7.3절 — 실제
// DXC·FXC· spirv-val을 관리자로 부른다): (a) D3D12·D3D11 FXC·Vulkan에서 리플렉션 오프셋 = 규칙
// 오프셋 = C++ 오프셋(루트 상수와 상수 버퍼, 값 1 대조)과 표가 다르면 모든 형식 오류, (b) 텍스처
// 17장을 공유 샘플러로 쓰는 PS가 넓은 형식에서 통과하고 D3D11_SM5만 오류, (f) 스칼라
// 배열·float3x3·이름 없는 전역(모든 형식)과 bool(D3D 형식 — SPIR-V 경로는 DXC가 uint로 바꿔 구별할
// 수 없다)의 컴파일 거부, (g) Vulkan 단계 사이 location이 선언 순서가 아니라 TEXCOORDn이고 정점
// 입력이 ATTRIBUTEn이며 고쳐 쓴 모듈이 spirv-val을 통과한다, (h) 짝 없는 샘플러의 모든 형식 오류.
// 워커·도구가 없으면 SKIP이다.
using namespace rocks;

namespace shader_binding_rules_tests_file {

using namespace rocks::shader;
using namespace rocks::shader_source_test;
using namespace rocks::shader_manager_test;

namespace {

constexpr char BindSource[] =
    "struct DrawConstants { float4x4 transform; float4 color; };\n"
    "ROCKS_ROOT_CONSTANTS(DrawConstants, Draw);\n"
    "cbuffer View { float4x4 viewProjection; float4 tint; float time; float padA; float2 padB; };\n"
    "Texture2D Albedo;\n"
    "SamplerState AlbedoSampler;\n"
    "Texture2D Noise;\n"
    "SamplerState SharedWrapSampler;\n"
    "struct VertexInput { float3 position : ATTRIBUTE0; float2 uv : ATTRIBUTE3; };\n"
    "struct Varyings { float4 position : SV_Position; float2 uv : TEXCOORD0; float4 color : "
    "TEXCOORD1; };\n"
    "Varyings MainVS(VertexInput input) {\n"
    "    Varyings output;\n"
    "    output.position = mul(float4(input.position, 1.0), mul(Draw.transform, viewProjection));\n"
    "    output.color = Draw.color * tint * (time + padA + padB.x);\n"
    "    output.uv = input.uv;\n"
    "    return output;\n"
    "}\n"
    "float4 MainPS(Varyings input) : SV_Target0 {\n"
    "    return Albedo.Sample(AlbedoSampler, input.uv) * Noise.Sample(SharedWrapSampler, input.uv) "
    "*\n"
    "           input.color;\n"
    "}\n";

// 5항 반례·짝·상한 원문이다(엔트리마다 하나).
constexpr char BadSource[] =
    "cbuffer ScalarArray { float scalars[2]; float after; };\n"
    "float4 ScalarArrayPS() : SV_Target0 { return scalars[1] + after; }\n"
    "cbuffer Matrix3 { float3x3 rotation; float scale; };\n"
    "float4 Matrix3PS() : SV_Target0 { return rotation[0].xyzz * scale; }\n"
    "cbuffer Flags { bool enabled; };\n"
    "float4 BoolPS() : SV_Target0 { return enabled ? 1.0 : 0.0; }\n"
    "float looseGlobal;\n"
    "float4 GlobalsPS() : SV_Target0 { return looseGlobal; }\n"
    "Texture2D Lonely;\n"
    "SamplerState LinearSampler;\n"
    "float4 PairPS(float4 p : SV_Position) : SV_Target0 { return Lonely.Sample(LinearSampler, "
    "p.xy); }\n"
    "struct Colored { float4 position : SV_Position; float4 color : COLOR0; };\n"
    "Colored ColorVS(float3 p : ATTRIBUTE0) { Colored o; o.position = p.xyzz; o.color = 1.0; "
    "return o; }\n";

// SH2b2 원문이다(SSD34–SSD36·SSD38·SSD40).
constexpr char MoreSource[] =
    "struct Reversed { float4 position : SV_Position; float4 b : TEXCOORD2; float4 a : TEXCOORD0; "
    "};\n"
    "Reversed ReversedVS(float3 p : ATTRIBUTE0) { Reversed o; o.position = p.xyzz; o.a = 1; o.b = "
    "2; return o; }\n"
    "struct Layered { float4 position : SV_Position; uint layer : SV_RenderTargetArrayIndex; };\n"
    "Layered LayerVS(float3 p : ATTRIBUTE0, uint id : SV_InstanceID) {\n"
    "    Layered o; o.position = p.xyzz; o.layer = id; return o;\n"
    "}\n"
    "float4 DebugPS(float4 p : SV_Position) : SV_Target0 {\n"
    "#if defined(ROCKS_DEBUG_BUFFER)\n"
    "    RocksDebugBuffer.Store(0, 1u);\n"
    "#endif\n"
    "    return p;\n"
    "}\n"
    "RWByteAddressBuffer Stray;\n"
    "float4 StrayPS(float4 p : SV_Position) : SV_Target0 { Stray.Store(0, 1u); return p; }\n"
    "Texture2D Albedo;\n"
    "SamplerState AlbedoSampler;\n"
    "SamplerState SharedWrapSampler;\n"
    "SamplerState SharedClampSampler;\n"
    "Texture2D Noise;\n"
    "float4 TwoSourcesPS(float4 p : SV_Position) : SV_Target0 {\n"
    "    return Albedo.Sample(AlbedoSampler, p.xy) + Albedo.Sample(SharedWrapSampler, p.xy);\n"
    "}\n"
    "float4 SharedPS(float4 p : SV_Position) : SV_Target0 {\n"
    "    return Albedo.Sample(AlbedoSampler, p.xy) + Noise.Sample(SharedClampSampler, p.xy);\n"
    "}\n"
    "ROCKS_IMAGE_FORMAT(rgba8) RWTexture2D<float4> Formatted;\n"
    "[numthreads(8, 8, 1)] void FormattedCS(uint3 id : SV_DispatchThreadID) { Formatted[id.xy] = "
    "1; }\n";

// 형식 지정 없는 storage image는 원문 전체 검사라 따로 둔다(SSD40).
constexpr char UnformattedSource[] = "RWTexture2D<float4> Unformatted;\n"
                                     "[numthreads(8, 8, 1)] void UnformattedCS(uint3 id : "
                                     "SV_DispatchThreadID) { Unformatted[id.xy] = 1; }\n";

constexpr char BindEntry[] = "/Project/Bind.rsf";
constexpr char UnformattedEntry[] = "/Project/Unformatted.rsf";
constexpr char MoreEntry[] = "/Project/More.rsf";
constexpr char BadEntry[] = "/Project/Bad.rsf";

// C++ 쪽 구조체와 손으로 쓴 표(7.2절 5항 — static_assert로 맞춘다)다.
struct alignas(16) DrawConstantsCpp {
    ShaderFloat4x4 transform;
    ShaderFloat4 color;
};

struct alignas(16) ViewCpp {
    ShaderFloat4x4 viewProjection;
    ShaderFloat4 tint;
    float time;
    float padA;
    ShaderFloat2 padB;
};

static_assert(offsetof(DrawConstantsCpp, color) == 64 && sizeof(DrawConstantsCpp) == 80);
static_assert(offsetof(ViewCpp, tint) == 64 && offsetof(ViewCpp, time) == 80 &&
              offsetof(ViewCpp, padA) == 84 && offsetof(ViewCpp, padB) == 88 &&
              sizeof(ViewCpp) == 96);

constexpr ShaderParameterMember DrawMembers[] = {
    {"transform", ShaderParameterType::Float4x4, offsetof(DrawConstantsCpp, transform), 0},
    {"color", ShaderParameterType::Float4, offsetof(DrawConstantsCpp, color), 0}};
constexpr ShaderParameterMember ViewMembers[] = {
    {"viewProjection", ShaderParameterType::Float4x4, offsetof(ViewCpp, viewProjection), 0},
    {"tint", ShaderParameterType::Float4, offsetof(ViewCpp, tint), 0},
    {"time", ShaderParameterType::Float, offsetof(ViewCpp, time), 0},
    {"padA", ShaderParameterType::Float, offsetof(ViewCpp, padA), 0},
    {"padB", ShaderParameterType::Float2, offsetof(ViewCpp, padB), 0}};
constexpr ShaderParameterLayout BindLayouts[] = {
    {"Draw", true, sizeof(DrawConstantsCpp), DrawMembers},
    {"View", false, sizeof(ViewCpp), ViewMembers}};

// 관리자 시험 한 벌에 D3D11_SM5 snapshot과 시험 원문을 더한다.
struct BindingCase {
    BindingCase() noexcept : d3d11(TestAllocator()) {
    }

    bool Make(const char* tag, const char*& skip) noexcept {
        if (!test.Make(tag, skip)) {
            return false;
        }

        SHADER_MANAGER_TEST_REQUIRE(test.fixture.Write("Project/Shaders/Bind.rsf", BindSource));
        SHADER_MANAGER_TEST_REQUIRE(test.fixture.Write("Project/Shaders/Bad.rsf", BadSource));
        SHADER_MANAGER_TEST_REQUIRE(test.fixture.Write("Project/Shaders/More.rsf", MoreSource));
        SHADER_MANAGER_TEST_REQUIRE(
            test.fixture.Write("Project/Shaders/Unformatted.rsf", UnformattedSource));
        SHADER_MANAGER_TEST_REQUIRE(
            LoadShaderFormatSnapshot(ShaderFormatDirectory, "D3D11_SM5", d3d11));
        return true;
    }

    ShaderCompileJob Job(const ShaderFormatSnapshot& format, const char* entry, const char* point,
                         uint32 stage,
                         Span<const ShaderParameterLayout> layouts = {}) const noexcept {
        ShaderCompileJob job;
        job.format = &format;
        job.entryPath = StringView(entry);
        job.entryPoint = StringView(point);
        job.stage = stage;
        job.options = DefaultShaderCompileOptions(format.definition);
        job.parameterLayouts = layouts;
        return job;
    }

    ManagerCase test;
    ShaderFormatSnapshot d3d11;
};

bool Compile(BindingCase& binding, const char* work, Span<const ShaderCompileJob> jobs,
             Array<ShaderCompileJobResult>& results) noexcept {
    ShaderCompileManager manager(binding.test.Config(work));
    SHADER_MANAGER_TEST_REQUIRE(manager.Start());
    SHADER_MANAGER_TEST_REQUIRE(manager.Compile(jobs, results));
    manager.Shutdown();
    return results.Size() == jobs.Size();
}

bool Failed(const ShaderCompileJobResult& result, const char* part) noexcept {
    const bool failed = result.status == ShaderCompileStatus::CompileError &&
                        strstr(result.diagnostics.CStr(), part) != nullptr;
    if (!failed) {
        fprintf(stderr, "expected a compile error with '%s', got %s: %s\n", part,
                ShaderCompileStatusName(result.status), result.diagnostics.CStr());
    }

    return failed;
}

// 결과의 값 1을 읽는다(bytes가 layout보다 오래 산다 — 결과가 들고 있다).
bool ReadLayout(const ShaderCompileJobResult& result, ShaderBindingLayout& layout) noexcept {
    SHADER_MANAGER_TEST_REQUIRE(result.status == ShaderCompileStatus::Success);
    SHADER_MANAGER_TEST_REQUIRE(result.values.Has(ShaderNuggetValue::BindingLayout));
    const ShaderReadStatus read = DecodeShaderBindingLayout(
        result.values.Part(ShaderNuggetValue::BindingLayout).View(), layout);
    SHADER_MANAGER_TEST_REQUIRE(read);
    return true;
}

const ShaderBindingResource* Named(const ShaderBindingLayout& layout, const char* name) noexcept {
    for (const ShaderBindingResource& resource : layout.resources) {
        if (resource.name == StringView(name)) {
            return &resource;
        }
    }

    return nullptr;
}

// SPIR-V에서 이름에 part가 든 변수의 Location 장식이다. 없으면 NumericLimits<uint32>::Max().
uint32 SpirvLocation(const Array<uint8>& code, const char* part) noexcept {
    constexpr uint32 OpName = 5;
    constexpr uint32 OpDecorate = 71;
    constexpr uint32 DecorationLocation = 30;
    constexpr usize HeaderWords = 5;
    const usize words = code.Size() / sizeof(uint32);
    const auto word = [&](usize index) noexcept {
        uint32 value = 0;
        memcpy(&value, code.Data() + index * sizeof(uint32), sizeof(value));
        return value;
    };
    uint32 target = 0;
    for (usize at = HeaderWords; at < words;) {
        const uint32 first = word(at);
        const uint32 count = first >> 16;
        if (count == 0) {
            break;
        }

        if ((first & 0xFFFFu) == OpName && count >= 3) {
            const char* name =
                reinterpret_cast<const char*>(code.Data() + (at + 2) * sizeof(uint32));
            if (strstr(name, part) != nullptr) {
                target = word(at + 1);
            }
        }

        at += count;
    }

    for (usize at = HeaderWords; target != 0 && at < words;) {
        const uint32 first = word(at);
        const uint32 count = first >> 16;
        if (count == 0) {
            break;
        }

        if ((first & 0xFFFFu) == OpDecorate && count >= 4 && word(at + 1) == target &&
            word(at + 2) == DecorationLocation) {
            return word(at + 3);
        }

        at += count;
    }

    return NumericLimits<uint32>::Max();
}

} // namespace

TEST(ShaderBindingRules_WorkerLayoutsMatchCxxOnEveryFormat) {
    BindingCase binding;
    const char* skip = nullptr;
    if (!binding.Make("bindinglayouts", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderBindingRules_WorkerLayoutsMatchCxxOnEveryFormat: %s\n", skip);
        }

        return;
    }

    const ShaderFormatSnapshot* formats[] = {&binding.test.d3d12, &binding.d3d11,
                                             &binding.test.vulkan};
    Array<ShaderCompileJob> jobs(TestAllocator());
    for (const ShaderFormatSnapshot* format : formats) {
        REQUIRE(jobs.TryPushBack(
            binding.Job(*format, BindEntry, "MainVS", ShaderStageVertex, BindLayouts)));
        REQUIRE(jobs.TryPushBack(
            binding.Job(*format, BindEntry, "MainPS", ShaderStagePixel, BindLayouts)));
    }

    Array<ShaderCompileJobResult> results(TestAllocator());
    REQUIRE(Compile(binding, "BindWork", jobs.View(), results));
    PrintFailures(results, results.Size());
    for (usize format = 0; format < 3; ++format) {
        ShaderBindingLayout vertex(TestAllocator());
        ShaderBindingLayout pixel(TestAllocator());
        REQUIRE(ReadLayout(results[format * 2], vertex));
        REQUIRE(ReadLayout(results[format * 2 + 1], pixel));
        // VS: 정점 입력 ATTRIBUTE0·3, 루트 상수 80 byte, 상수 버퍼 View의 멤버 오프셋 = C++ 오프셋.
        CHECK(vertex.attributeMask == 0b1001 &&
              vertex.rootConstantBytes == sizeof(DrawConstantsCpp));
        const ShaderBindingResource* view = Named(vertex, "View");
        REQUIRE(view != nullptr);
        CHECK(view->kind == ShaderResourceKind::ConstantBuffer &&
              view->constantBufferBytes == sizeof(ViewCpp));
        const Span<const ShaderBindingMember> members = vertex.MembersOf(*view);
        REQUIRE(members.Size() == 5);
        for (usize index = 0; index < members.Size(); ++index) {
            CHECK(members[index].name == ViewMembers[index].name &&
                  members[index].offset == ViewMembers[index].offset);
        }

        // PS: render target 0, 자기 샘플러·공유 샘플러, 위치는 형식마다 다르지만 겹치지 않는다.
        CHECK(pixel.renderTargetMask == 1 && pixel.rootConstantBytes == 0);
        const ShaderBindingResource* albedo = Named(pixel, "Albedo");
        const ShaderBindingResource* noise = Named(pixel, "Noise");
        const ShaderBindingResource* shared = Named(pixel, "SharedWrapSampler");
        REQUIRE(albedo != nullptr && noise != nullptr && shared != nullptr &&
                Named(pixel, "AlbedoSampler") != nullptr);
        // 출처: D3D는 이름 규칙까지(Noise는 None), SPIR-V 형식은 사용 쌍으로 SharedWrap이다(SSD36).
        const ShaderSamplerSource noiseSource =
            format == 2 ? ShaderSamplerSource::SharedWrap : ShaderSamplerSource::None;
        CHECK(albedo->samplerSource == ShaderSamplerSource::Own &&
              noise->samplerSource == noiseSource && shared->sharedSampler);
        if (format == 2) {
            // Vulkan binding은 종류 순서(텍스처 → 샘플러)로 0부터 잇는다(7.2절 4항).
            CHECK(albedo->slot == 0 && noise->slot == 1 &&
                  Named(pixel, "AlbedoSampler")->slot == 2 && shared->slot == 3);
        } else {
            // D3D register는 선언 순서(t0·t1, s0·s1 — SSD30)다.
            CHECK(albedo->slot == 0 && noise->slot == 1 && shared->slot == 1);
        }
    }

    // VS·PS 연결(SSD37): 모든 형식에서 PS 입력이 VS 출력의 앞부분이다.
    for (usize format = 0; format < 3; ++format) {
        ShaderBindingLayout vertex(TestAllocator());
        ShaderBindingLayout pixel(TestAllocator());
        REQUIRE(ReadLayout(results[format * 2], vertex));
        REQUIRE(ReadLayout(results[format * 2 + 1], pixel));
        CHECK(vertex.interStageMask == 0b11 && pixel.interStageMask == 0b11);
        CHECK(CheckShaderStageLink(vertex, pixel));
    }

    // Vulkan: 단계 사이 location은 TEXCOORDn이다(7.2절 7항, 7.3절 (g)).
    const Array<uint8>& vulkanVertex = results[4].values.Part(ShaderNuggetValue::Code);
    const Array<uint8>& vulkanPixel = results[5].values.Part(ShaderNuggetValue::Code);
    CHECK(SpirvLocation(vulkanVertex, "TEXCOORD0") == 0 &&
          SpirvLocation(vulkanVertex, "TEXCOORD1") == 1);
    CHECK(SpirvLocation(vulkanPixel, "TEXCOORD0") == 0 &&
          SpirvLocation(vulkanPixel, "TEXCOORD1") == 1);
    CHECK(SpirvLocation(vulkanVertex, "ATTRIBUTE3") == 3);

    // 표가 셰이더와 다르면 모든 형식에서 오류다(7.3절 (a)).
    const ShaderParameterMember shifted[] = {
        {"viewProjection", ShaderParameterType::Float4x4, 0, 0},
        {"tint", ShaderParameterType::Float4, 64, 0},
        {"time", ShaderParameterType::Float, 84, 0},
        {"padA", ShaderParameterType::Float, 88, 0},
        {"padB", ShaderParameterType::Float2, 88, 0}};
    const ShaderParameterLayout wrong[] = {{"Draw", true, sizeof(DrawConstantsCpp), DrawMembers},
                                           {"View", false, sizeof(ViewCpp), shifted}};
    // 표 자체가 규칙을 어기면 job 오류다(관리자가 거른다).
    Array<ShaderCompileJob> wrongJobs(TestAllocator());
    for (const ShaderFormatSnapshot* format : formats) {
        REQUIRE(wrongJobs.TryPushBack(
            binding.Job(*format, BindEntry, "MainVS", ShaderStageVertex, wrong)));
    }

    Array<ShaderCompileJobResult> wrongResults(TestAllocator());
    REQUIRE(Compile(binding, "BindWrong", wrongJobs.View(), wrongResults));
    for (const ShaderCompileJobResult& result : wrongResults) {
        CHECK(result.status == ShaderCompileStatus::InvalidJob);
    }

    const ShaderParameterMember late[] = {{"viewProjection", ShaderParameterType::Float4x4, 0, 0},
                                          {"tint", ShaderParameterType::Float4, 64, 0},
                                          {"time", ShaderParameterType::Float, 80, 0},
                                          {"padA", ShaderParameterType::Float, 84, 0},
                                          {"padC", ShaderParameterType::Float2, 88, 0}};
    const ShaderParameterLayout renamed[] = {{"Draw", true, sizeof(DrawConstantsCpp), DrawMembers},
                                             {"View", false, sizeof(ViewCpp), late}};
    wrongJobs.Clear();
    for (const ShaderFormatSnapshot* format : formats) {
        REQUIRE(wrongJobs.TryPushBack(
            binding.Job(*format, BindEntry, "MainVS", ShaderStageVertex, renamed)));
    }

    REQUIRE(Compile(binding, "BindRenamed", wrongJobs.View(), wrongResults));
    for (const ShaderCompileJobResult& result : wrongResults) {
        CHECK(Failed(result, "does not match the shader member"));
    }
}

TEST(ShaderBindingRules_WorkerRejectsCounterexamplesAndNarrowFormats) {
    BindingCase binding;
    const char* skip = nullptr;
    if (!binding.Make("bindingbad", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderBindingRules_WorkerRejectsCounterexamplesAndNarrowFormats: %s\n",
                   skip);
        }

        return;
    }

    struct Bad {
        const char* entry;
        uint32 stage;
        const char* part;
        bool spirv;
    };

    const Bad cases[] = {{"ScalarArrayPS", ShaderStagePixel, "arrays need 16-byte elements", true},
                         {"Matrix3PS", ShaderStagePixel, "type is not allowed", true},
                         {"BoolPS", ShaderStagePixel, "type is not allowed", false},
                         {"GlobalsPS", ShaderStagePixel, "$Globals", true},
                         {"PairPS", ShaderStagePixel, "sampler 'LinearSampler'", true},
                         {"ColorVS", ShaderStageVertex, "COLOR", true}};
    const ShaderFormatSnapshot* formats[] = {&binding.test.d3d12, &binding.d3d11,
                                             &binding.test.vulkan};
    Array<ShaderCompileJob> jobs(TestAllocator());
    for (const Bad& bad : cases) {
        for (const ShaderFormatSnapshot* format : formats) {
            REQUIRE(jobs.TryPushBack(binding.Job(*format, BadEntry, bad.entry, bad.stage)));
        }
    }

    // 텍스처 17장(공유 샘플러): 128장 형식 통과, D3D11_SM5만 오류(7.3절 (b)·(h)).
    char wide[2048];
    int written = snprintf(wide, sizeof(wide), "SamplerState SharedWrapSampler;\n");
    for (int index = 0; index < 17; ++index) {
        written += snprintf(wide + written, sizeof(wide) - static_cast<usize>(written),
                            "Texture2D Layer%d;\n", index);
    }

    written +=
        snprintf(wide + written, sizeof(wide) - static_cast<usize>(written),
                 "float4 WidePS(float4 p : SV_Position) : SV_Target0 {\n    float4 sum = 0;\n");
    for (int index = 0; index < 17; ++index) {
        written += snprintf(wide + written, sizeof(wide) - static_cast<usize>(written),
                            "    sum += Layer%d.Sample(SharedWrapSampler, p.xy);\n", index);
    }

    (void)snprintf(wide + written, sizeof(wide) - static_cast<usize>(written),
                   "    return sum;\n}\n");
    REQUIRE(binding.test.fixture.Write("Project/Shaders/Wide.rsf", StringView(wide)));
    const usize wideFirst = jobs.Size();
    for (const ShaderFormatSnapshot* format : formats) {
        REQUIRE(jobs.TryPushBack(
            binding.Job(*format, "/Project/Wide.rsf", "WidePS", ShaderStagePixel)));
    }

    Array<ShaderCompileJobResult> results(TestAllocator());
    REQUIRE(Compile(binding, "BadWork", jobs.View(), results));
    usize index = 0;
    for (const Bad& bad : cases) {
        for (usize format = 0; format < 3; ++format) {
            const ShaderCompileJobResult& result = results[index++];
            if (format == 2 && !bad.spirv) {
                // SPIR-V 경로는 상수 버퍼 bool을 uint로 바꿔 내므로 규칙이 보지 못한다(D3D 형식이
                // 잡는다).
                CHECK(result.status == ShaderCompileStatus::Success);
                continue;
            }

            CHECK(Failed(result, bad.part));
        }
    }

    ShaderBindingLayout layout(TestAllocator());
    CHECK(ReadLayout(results[wideFirst], layout));
    CHECK(layout.resources.Size() == 18);
    CHECK(
        Failed(results[wideFirst + 1], "sampled texture count 17 exceeds the D3D11_SM5 limit 16"));
    CHECK(ReadLayout(results[wideFirst + 2], layout));
}

TEST(ShaderBindingRules_WorkerFeaturesPairsAndFallback) {
    BindingCase binding;
    const char* skip = nullptr;
    if (!binding.Make("bindingmore", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderBindingRules_WorkerFeaturesPairsAndFallback: %s\n", skip);
        }

        return;
    }

    const ShaderFormatSnapshot& d3d12 = binding.test.d3d12;
    const ShaderFormatSnapshot& d3d11 = binding.d3d11;
    const ShaderFormatSnapshot& vulkan = binding.test.vulkan;
    Array<ShaderCompileJob> jobs(TestAllocator());
    const auto add = [&](const ShaderFormatSnapshot& format, const char* point, uint32 stage,
                         uint32 features = 0, bool development = false) noexcept {
        ShaderCompileJob job = binding.Job(format, MoreEntry, point, stage);
        job.requiredFeatures = features;
        job.developmentOnly = development;
        REQUIRE(jobs.TryPushBack(job));
    };
    // 0–2: 거꾸로 선언한 단계 사이 변수 — D3D는 순서 오류, Vulkan은 location = n.
    add(d3d12, "ReversedVS", ShaderStageVertex);
    add(d3d11, "ReversedVS", ShaderStageVertex);
    add(vulkan, "ReversedVS", ShaderStageVertex);
    // 3–6: layer 출력 — 요구 기능이 있으면 D3D12·Vulkan 통과, 없으면 오류(SSD35, 7.3절 (j) 컴파일).
    const uint32 layerFeature = ShaderFeatureVertexViewportLayerOutput;
    add(d3d12, "LayerVS", ShaderStageVertex, layerFeature);
    add(vulkan, "LayerVS", ShaderStageVertex, layerFeature);
    add(d3d12, "LayerVS", ShaderStageVertex);
    add(vulkan, "LayerVS", ShaderStageVertex);
    // 7–10: PS 진단 버퍼(SSD34, 7.3절 (k)) — 개발 전용 PS는 예약 위치, D3D11은 전처리 오류, 진단
    // 퍼뮤테이션이 아니면 선언이 없다.
    const uint32 debugFeature = ShaderFeatureDebugPixelUav;
    add(d3d12, "DebugPS", ShaderStagePixel, debugFeature, true);
    add(vulkan, "DebugPS", ShaderStagePixel, debugFeature, true);
    add(d3d11, "DebugPS", ShaderStagePixel, debugFeature, true);
    add(d3d12, "DebugPS", ShaderStagePixel);
    // 11–12: 진단 버퍼가 아닌 PS UAV는 오류.
    add(d3d12, "StrayPS", ShaderStagePixel, debugFeature, true);
    add(vulkan, "StrayPS", ShaderStagePixel);
    // 13–16: 사용 쌍(SSD36) — Vulkan만 쌍을 본다.
    add(vulkan, "TwoSourcesPS", ShaderStagePixel);
    add(d3d12, "TwoSourcesPS", ShaderStagePixel);
    add(vulkan, "SharedPS", ShaderStagePixel);
    add(d3d12, "SharedPS", ShaderStagePixel);
    // 17–19: ROCKS_IMAGE_FORMAT 원문 검사(SSD40) — SPIR-V 형식만.
    for (const ShaderFormatSnapshot* format : {&vulkan, &d3d12}) {
        REQUIRE(jobs.TryPushBack(
            binding.Job(*format, UnformattedEntry, "UnformattedCS", ShaderStageCompute)));
    }

    add(vulkan, "FormattedCS", ShaderStageCompute);
    // 20–21: D3D11 대체 경로(HLSL 2021 — SSD38)의 register·ATTRIBUTEn·루트 상수.
    for (const char* point : {"MainVS", "MainPS"}) {
        ShaderCompileJob job =
            binding.Job(d3d11, BindEntry, point,
                        point[4] == 'V' ? ShaderStageVertex : ShaderStagePixel, BindLayouts);
        job.options.hlsl2021 = true;
        REQUIRE(jobs.TryPushBack(job));
    }

    Array<ShaderCompileJobResult> results(TestAllocator());
    REQUIRE(Compile(binding, "MoreWork", jobs.View(), results));
    CHECK(Failed(results[0], "ascending n order"));
    CHECK(Failed(results[1], "ascending n order"));
    REQUIRE(results[2].status == ShaderCompileStatus::Success);
    const Array<uint8>& reversed = results[2].values.Part(ShaderNuggetValue::Code);
    CHECK(SpirvLocation(reversed, "TEXCOORD0") == 0 && SpirvLocation(reversed, "TEXCOORD2") == 2);
    ShaderBindingLayout layout(TestAllocator());
    REQUIRE(ReadLayout(results[2], layout));
    CHECK(layout.interStageMask == 0b101);

    CHECK(results[3].status == ShaderCompileStatus::Success);
    CHECK(results[4].status == ShaderCompileStatus::Success);
    CHECK(Failed(results[5], "vertexViewportLayerOutput"));
    CHECK(Failed(results[6], "vertexViewportLayerOutput"));

    for (const usize index : {7u, 8u}) {
        REQUIRE(ReadLayout(results[index], layout));
        const ShaderBindingResource* debug = Named(layout, "RocksDebugBuffer");
        REQUIRE(debug != nullptr);
        CHECK(debug->kind == ShaderResourceKind::StorageBufferReadWrite && debug->slot == 0);
    }

    CHECK(results[9].status == ShaderCompileStatus::PreprocessError &&
          strstr(results[9].diagnostics.CStr(), "debugPixelUav") != nullptr);
    REQUIRE(ReadLayout(results[10], layout));
    CHECK(Named(layout, "RocksDebugBuffer") == nullptr);
    CHECK(Failed(results[11], "uavStages"));
    CHECK(Failed(results[12], "uavStages"));

    CHECK(Failed(results[13], "exactly one sampler source"));
    CHECK(results[14].status == ShaderCompileStatus::Success);
    REQUIRE(ReadLayout(results[15], layout));
    CHECK(Named(layout, "Noise")->samplerSource == ShaderSamplerSource::SharedClamp &&
          Named(layout, "Albedo")->samplerSource == ShaderSamplerSource::Own);
    REQUIRE(ReadLayout(results[16], layout));
    CHECK(Named(layout, "Noise")->samplerSource == ShaderSamplerSource::None);

    CHECK(Failed(results[17], "ROCKS_IMAGE_FORMAT"));
    CHECK(results[18].status == ShaderCompileStatus::Success);
    CHECK(results[19].status == ShaderCompileStatus::Success);

    // 대체 경로도 FXC 직접 경로와 같은 위치·크기다(register 선언 순서, 루트 상수 b13 → 값 1 밖).
    ShaderBindingLayout vertex(TestAllocator());
    ShaderBindingLayout pixel(TestAllocator());
    REQUIRE(ReadLayout(results[20], vertex));
    REQUIRE(ReadLayout(results[21], pixel));
    CHECK(vertex.attributeMask == 0b1001 && vertex.rootConstantBytes == sizeof(DrawConstantsCpp));
    const ShaderBindingResource* view = Named(vertex, "View");
    REQUIRE(view != nullptr);
    CHECK(view->slot == 0 && view->constantBufferBytes == sizeof(ViewCpp));
    CHECK(Named(pixel, "Albedo")->slot == 0 && Named(pixel, "Noise")->slot == 1 &&
          Named(pixel, "AlbedoSampler")->slot == 0 && Named(pixel, "SharedWrapSampler")->slot == 1);
    CHECK(Named(pixel, "Noise")->samplerSource == ShaderSamplerSource::SharedWrap);
    CHECK(CheckShaderStageLink(vertex, pixel));
}

} // namespace shader_binding_rules_tests_file
