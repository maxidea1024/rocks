#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <ShaderCompilerCommon/ShaderGlesBindings.h>
#include <ShaderCompilerCommon/ShaderReflection.h>
#include <ShaderCore/ShaderGlesBindingHeader.h>
#include <Base/InitializerList.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// GLES 배치·상수 버퍼 에뮬레이션 시험이다(Docs/Design/ShaderSystem 16.1절 SSD42, SH2c — 도구 없이):
// GLES slot(상수 버퍼 순번·텍스처 유닛 PS 0·VS 16·storage binding)·값 2(배열·샘플러·복사 표
// golden)· combined sampler 이름과 유닛·멤버 참조 바꿈(스칼라·벡터 swizzle·행렬·정수 배열·루트 상수
// pu_h·배열 멤버 함수·색인 식 안의 참조)·통째 읽기 오류·선언 없음 오류, 모바일 상한(에뮬레이션 vec4
// 합·VS·PS storage block).
using namespace rocks;

namespace shader_gles_bindings_tests_file {

using namespace rocks::shader;

namespace {

SystemAllocator& TestAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

using B = ShaderReflectedBase;
constexpr uint32 Top = NoShaderReflectedParent;

ShaderReflectedMember M(const char* name, B base, uint32 columns, uint32 offset,
                        uint32 elements = 0, uint32 rows = 1) noexcept {
    ShaderReflectedMember member;
    member.name = StringView(name);
    member.base = base;
    member.rows = rows;
    member.columns = columns;
    member.elements = elements;
    member.offset = offset;
    member.parent = Top;
    return member;
}

// 종류의 기본 모양이다(값 1 v3 규칙 — 텍스처는 Texture2D float, 구조화는 raw).
void DefaultShape(ShaderReflectedResource& resource) noexcept {
    switch (resource.kind) {
    case ShaderResourceKind::SampledTexture:
    case ShaderResourceKind::StorageImage:
        resource.shape = ShaderResourceShape::Texture2D;
        resource.component = ShaderComponentType::Float;
        break;
    case ShaderResourceKind::StorageBufferRead:
    case ShaderResourceKind::StorageBufferReadWrite:
        resource.shape = ShaderResourceShape::RawBuffer;
        break;
    default:
        break;
    }
}

void Buffer(ShaderReflection& reflection, const char* name, uint32 slot,
            std::initializer_list<ShaderReflectedMember> members, bool root = false) noexcept {
    ShaderReflectedResource resource;
    resource.kind = ShaderResourceKind::ConstantBuffer;
    resource.name = StringView(name);
    resource.slot = slot;
    resource.rootConstants = root;
    resource.firstMember = static_cast<uint32>(reflection.members.Size());
    for (const ShaderReflectedMember& member : members) {
        REQUIRE(reflection.members.TryPushBack(member));
    }

    resource.memberCount = static_cast<uint32>(members.size());
    REQUIRE(reflection.resources.TryPushBack(resource));
}

void Resource(ShaderReflection& reflection, ShaderResourceKind kind, const char* name,
              uint32 slot) noexcept {
    ShaderReflectedResource resource;
    resource.kind = kind;
    resource.name = StringView(name);
    resource.slot = slot;
    DefaultShape(resource);
    REQUIRE(reflection.resources.TryPushBack(resource));
}

// 정상 PS 한 벌(상수 버퍼 View — 행렬·벡터·정수 슬롯, 루트 상수 Draw, 텍스처 둘·typed buffer·샘플러
// 둘)이다.
struct Sample {
    Sample() noexcept
        : reflection(TestAllocator()), layout(TestAllocator()), plan(TestAllocator()) {
        REQUIRE(LoadShaderFormatDefinition(ShaderFormatDirectory, "GLES31_PC", gles));
        reflection.stage = ShaderStagePixel;
        REQUIRE(reflection.inputs.TryPushBack(ShaderReflectedSignature{"SV_Position", 0}));
        REQUIRE(reflection.outputs.TryPushBack(ShaderReflectedSignature{"SV_Target", 0}));
        Buffer(reflection, "View", 0,
               {M("transform", B::Float, 4, 0, 0, 4), M("tint", B::Float, 4, 64),
                M("count", B::Int, 1, 80), M("pad", B::Int, 2, 88),
                M("colors", B::Float, 4, 96, 2)});
        Buffer(reflection, "Draw", 0, {M("color", B::Float, 4, 0)}, true);
        Resource(reflection, ShaderResourceKind::SampledTexture, "Albedo", 1);
        Resource(reflection, ShaderResourceKind::SampledTexture, "Noise", 2);
        Resource(reflection, ShaderResourceKind::SampledTexture, "Lights", 3);
        // Lights는 typed buffer(samplerBuffer)다 — 결합 sampler 목록 밖이다.
        reflection.resources[reflection.resources.Size() - 1].shape =
            ShaderResourceShape::TypedBuffer;
        Resource(reflection, ShaderResourceKind::Sampler, "AlbedoSampler", 4);
        Resource(reflection, ShaderResourceKind::Sampler, "SharedClampSampler", 5);
        reflection.hasSamplerUses = true;
        REQUIRE(reflection.samplerUses.TryPushBack(ShaderSamplerUse{"Albedo", "AlbedoSampler"}));
        REQUIRE(
            reflection.samplerUses.TryPushBack(ShaderSamplerUse{"Noise", "SharedClampSampler"}));
    }

    bool Build() noexcept {
        String diagnostics(TestAllocator());
        const bool ok =
            NormalizeShaderReflection(gles, reflection, {}, layout, diagnostics) &&
            BuildGlesPlan(reflection, layout, reflection.samplerUses.View(), plan, diagnostics);
        if (!ok) {
            fprintf(stderr, "GLES plan: %s\n", diagnostics.CStr());
        }

        return ok;
    }

    ShaderFormatDefinition gles;
    ShaderReflection reflection;
    ShaderBindingLayout layout;
    GlesPlan plan;
};

const GlesUniformReference* Reference(const GlesPlan& plan, const char* buffer,
                                      const char* path) noexcept {
    for (const GlesUniformBuffer& candidate : plan.buffers) {
        if (candidate.name.View() != StringView(buffer)) {
            continue;
        }

        for (const GlesUniformReference& reference : candidate.references) {
            if (reference.path.View() == StringView(path)) {
                return &reference;
            }
        }
    }

    return nullptr;
}

constexpr char SampleEssl[] = "#version 320 es\n"
                              "struct type_View { highp mat4 transform; highp vec4 tint; int "
                              "count; ivec2 pad; highp vec4 colors[2]; };\n"
                              "uniform type_View View;\n"
                              "struct type_Draw { highp vec4 color; };\n"
                              "uniform type_Draw Draw;\n"
                              "layout(location = 0) out highp vec4 out_var_SV_Target0;\n"
                              "void main()\n"
                              "{\n"
                              "    out_var_SV_Target0 = (View.transform * View.tint) + Draw.color "
                              "+ View.colors[View.count + 1]"
                              " + vec4(float(View.pad.y));\n"
                              "}\n";

} // namespace

TEST(ShaderGlesBindings_PlanSlotsHeaderAndReferences) {
    Sample sample;
    REQUIRE(sample.Build());
    // slot: 상수 버퍼 순번 0, 텍스처 유닛 0·1·2(PS), 샘플러 순번.
    const auto slotOf = [&](const char* name) noexcept {
        for (const ShaderBindingResource& resource : sample.layout.resources) {
            if (resource.name == StringView(name)) {
                return static_cast<int>(resource.slot);
            }
        }

        return -1;
    };
    CHECK(slotOf("View") == 0 && slotOf("Albedo") == 0 && slotOf("Noise") == 1 &&
          slotOf("Lights") == 2);
    CHECK(slotOf("AlbedoSampler") == 0 && slotOf("SharedClampSampler") == 1);
    // combined sampler: 이름 ps<순번>, 유닛.
    REQUIRE(sample.plan.samplers.Size() == 2);
    CHECK(sample.plan.samplers[0].name.View() == StringView("ps0") &&
          sample.plan.samplers[0].unit == 0 &&
          sample.plan.samplers[0].texture.View() == StringView("Albedo"));
    CHECK(sample.plan.samplers[1].name.View() == StringView("ps1") &&
          sample.plan.samplers[1].unit == 1);
    // typed buffer(사용 쌍 없음)는 SPIR-V binding을 유닛으로 바꾼다.
    REQUIRE(sample.plan.bindings.Size() == 1);
    CHECK(sample.plan.bindings[0].set == 1 && sample.plan.bindings[0].binding == 3 &&
          sample.plan.bindings[0].glBinding == 2);

    // 값 2 golden: 배열(View h 6·i 1, 루트 h 1), 샘플러 셋, 복사 표 셋.
    ShaderGlesBindingHeader header(TestAllocator());
    REQUIRE(DecodeShaderGlesBindingHeader(sample.plan.header.View(), header));
    REQUIRE(header.arrays.Size() == 3);
    CHECK(header.arrays[0].buffer == 0 && header.arrays[0].type == ShaderGlesUniformType::Float &&
          header.arrays[0].vec4Count == 7);
    CHECK(header.arrays[1].buffer == 0 && header.arrays[1].type == ShaderGlesUniformType::Int &&
          header.arrays[1].vec4Count == 1);
    CHECK(header.arrays[2].buffer == ShaderGlesRootConstantsBuffer &&
          header.arrays[2].vec4Count == 1);
    REQUIRE(header.samplers.Size() == 3);
    CHECK(header.samplers[0].source == ShaderSamplerSource::Own &&
          header.samplers[1].source == ShaderSamplerSource::SharedClamp &&
          header.samplers[2].source == ShaderSamplerSource::None &&
          header.samplers[2].name == StringView("Lights"));
    // 슬롯 0–4 float → h[0..4], 5 int → i[0], 6·7 float(colors) → h[5..6].
    REQUIRE(header.copies.Size() == 4);
    CHECK(header.copies[0].sourceOffset == 0 && header.copies[0].bytes == 80 &&
          header.copies[0].arrayIndex == 0 && header.copies[0].vec4Offset == 0);
    CHECK(header.copies[1].sourceOffset == 80 && header.copies[1].bytes == 16 &&
          header.copies[1].arrayIndex == 1 && header.copies[1].vec4Offset == 0);
    CHECK(header.copies[2].sourceOffset == 96 && header.copies[2].bytes == 32 &&
          header.copies[2].arrayIndex == 0 && header.copies[2].vec4Offset == 5);
    CHECK(header.copies[3].buffer == ShaderGlesRootConstantsBuffer &&
          header.copies[3].arrayIndex == 2);

    // 멤버 참조.
    const GlesUniformReference* transform = Reference(sample.plan, "View", "transform");
    REQUIRE(transform != nullptr);
    CHECK(transform->replacement.View() ==
          StringView("mat4(pc0_h[0], pc0_h[1], pc0_h[2], pc0_h[3])"));
    CHECK(Reference(sample.plan, "View", "tint")->replacement.View() == StringView("pc0_h[4]"));
    CHECK(Reference(sample.plan, "View", "count")->replacement.View() == StringView("pc0_i[0].x"));
    CHECK(Reference(sample.plan, "View", "pad")->replacement.View() == StringView("pc0_i[0].zw"));
    CHECK(Reference(sample.plan, "Draw", "color")->replacement.View() == StringView("pu_h[0]"));
    const GlesUniformReference* colors = Reference(sample.plan, "View", "colors");
    REQUIRE(colors != nullptr);
    CHECK(colors->indexed && colors->replacement.View() == StringView("View_colors"));
}

TEST(ShaderGlesBindings_RewritesEssl) {
    Sample sample;
    REQUIRE(sample.Build());
    String essl(TestAllocator());
    REQUIRE(essl.TryAssign(SampleEssl));
    String diagnostics(TestAllocator());
    REQUIRE(RewriteGlesUniforms(essl, sample.plan, diagnostics));
    const char* text = essl.CStr();
    CHECK(strstr(text, "uniform highp vec4 pc0_h[7];") != nullptr);
    CHECK(strstr(text, "uniform highp ivec4 pc0_i[1];") != nullptr);
    CHECK(strstr(text, "uniform highp vec4 pu_h[1];") != nullptr);
    CHECK(strstr(text, "highp vec4 View_colors(int i) { return pc0_h[5 + i]; }") != nullptr);
    CHECK(strstr(text, "(mat4(pc0_h[0], pc0_h[1], pc0_h[2], pc0_h[3]) * pc0_h[4]) + pu_h[0]") !=
          nullptr);
    // 색인 식 안의 참조도 바뀐다.
    CHECK(strstr(text, "View_colors(pc0_i[0].x + 1)") != nullptr);
    CHECK(strstr(text, "float(pc0_i[0].zw.y)") != nullptr);
    CHECK(strstr(text, "uniform type_View View;") == nullptr && strstr(text, "View.") == nullptr);

    // 통째 읽기는 오류다.
    REQUIRE(essl.TryAssign("struct type_View { highp vec4 tint; };\nuniform type_View View;\n"
                           "uniform type_Draw Draw;\nvoid main() { type_View copy = View; }\n"));
    diagnostics.Clear();
    CHECK(!RewriteGlesUniforms(essl, sample.plan, diagnostics));
    CHECK(strstr(diagnostics.CStr(), "whole buffer") != nullptr);
    // 선언이 없으면 오류다.
    REQUIRE(essl.TryAssign("void main() {}\n"));
    diagnostics.Clear();
    CHECK(!RewriteGlesUniforms(essl, sample.plan, diagnostics));
    CHECK(strstr(diagnostics.CStr(), "did not find") != nullptr);
}

TEST(ShaderGlesBindings_MobileLimits) {
    ShaderFormatDefinition gles;
    REQUIRE(LoadShaderFormatDefinition(ShaderFormatDirectory, "GLES31_PC", gles));
    // PS 에뮬레이션 vec4 합: 상한(형식 값)을 한 칸 넘긴다.
    const uint32 limit = gles.limits.uniformVectors[1];
    ShaderReflection big(TestAllocator());
    big.stage = ShaderStagePixel;
    REQUIRE(big.outputs.TryPushBack(ShaderReflectedSignature{"SV_Target", 0}));
    Buffer(big, "Big", 0, {M("values", B::Float, 4, 0, limit + 1)});
    ShaderBindingLayout layout(TestAllocator());
    String diagnostics(TestAllocator());
    CHECK(!NormalizeShaderReflection(gles, big, {}, layout, diagnostics));
    CHECK(strstr(diagnostics.CStr(), "emulated uniform vector count") != nullptr);
    // 상한 그대로는 통과한다.
    big.members[0].elements = limit;
    diagnostics.Clear();
    CHECK(NormalizeShaderReflection(gles, big, {}, layout, diagnostics));
    // VS의 storage block은 OpenGL ES에서 받지 않는다(CS는 받는다).
    ShaderReflection vertex(TestAllocator());
    vertex.stage = ShaderStageVertex;
    Resource(vertex, ShaderResourceKind::StorageBufferRead, "Instances", 0);
    diagnostics.Clear();
    CHECK(!NormalizeShaderReflection(gles, vertex, {}, layout, diagnostics));
    CHECK(strstr(diagnostics.CStr(), "OpenGL ES vertex and pixel shaders") != nullptr);
    vertex.stage = ShaderStageCompute;
    diagnostics.Clear();
    CHECK(NormalizeShaderReflection(gles, vertex, {}, layout, diagnostics));
}

} // namespace shader_gles_bindings_tests_file
