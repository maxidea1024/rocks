#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <ShaderCompilerCommon/ShaderReflection.h>
#include <Base/InitializerList.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 공용 바인딩 규칙 시험이다(Docs/Design/ShaderSystem 16.1절 SSD30·SSD33, SH2b1 — 도구 없이 리플렉션
// 모델을 손으로 만든다): 정상 셰이더의 값 1(attribute·render target 마스크, 정렬, 짝 출처, 공유
// 샘플러, 루트 상수 분리, 상수 버퍼 멤버), 레이아웃 표 대조 양성·음성, 5항 반례(스칼라
// 배열·float3x3·bool·double· half·암묵 padding·슬롯 타입 섞기·구조체 배열·2단 구조체·padding 없는
// 구조체·$Globals), job 형식 상한 (넓은 형식 통과·좁은 형식 오류 — 텍스처 17장·SV_Target4·PS
// UAV·루트 상수 크기·단계 사이 vec4), 시맨틱 음성, 짝 이름 음성, binding 충돌·자원 배열·루트 상수
// 둘.
using namespace rocks;

namespace shader_reflection_rules_tests_file {

using namespace rocks::shader;

namespace {

SystemAllocator& TestAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

using B = ShaderReflectedBase;
constexpr uint32 Top = NoShaderReflectedParent;

// 멤버 하나다(parent는 같은 상수 버퍼 안 상대 색인 또는 Top).
ShaderReflectedMember M(const char* name, B base, uint32 columns, uint32 offset,
                        uint32 elements = 0, uint32 parent = Top, uint32 rows = 1,
                        uint32 bytes = 0) noexcept {
    ShaderReflectedMember member;
    member.name = StringView(name);
    member.base = base;
    member.rows = rows;
    member.columns = columns;
    member.elements = elements;
    member.offset = offset;
    member.bytes = bytes;
    member.parent = parent;
    return member;
}

ShaderReflectedMember Matrix(const char* name, uint32 offset, uint32 rows = 4,
                             uint32 columns = 4) noexcept {
    return M(name, B::Float, columns, offset, 0, Top, rows);
}

struct Case {
    explicit Case(uint32 stage) noexcept : reflection(TestAllocator()) {
        reflection.stage = stage;
    }

    void Resource(ShaderResourceKind kind, const char* name, uint32 slot) noexcept {
        ShaderReflectedResource resource;
        resource.kind = kind;
        resource.name = StringView(name);
        resource.slot = slot;
        REQUIRE(reflection.resources.TryPushBack(resource));
    }

    void Buffer(const char* name, uint32 slot, std::initializer_list<ShaderReflectedMember> members,
                bool root = false) noexcept {
        ShaderReflectedResource resource;
        resource.kind = ShaderResourceKind::ConstantBuffer;
        resource.name = StringView(name);
        resource.slot = slot;
        resource.rootConstants = root;
        resource.firstMember = static_cast<uint32>(reflection.members.Size());
        for (ShaderReflectedMember member : members) {
            member.parent = member.parent == Top ? Top : member.parent + resource.firstMember;
            REQUIRE(reflection.members.TryPushBack(member));
        }

        resource.memberCount = static_cast<uint32>(members.size());
        REQUIRE(reflection.resources.TryPushBack(resource));
    }

    void In(const char* semantic, uint32 index = 0) noexcept {
        REQUIRE(
            reflection.inputs.TryPushBack(ShaderReflectedSignature{StringView(semantic), index}));
    }

    void Out(const char* semantic, uint32 index = 0) noexcept {
        REQUIRE(
            reflection.outputs.TryPushBack(ShaderReflectedSignature{StringView(semantic), index}));
    }

    ShaderReflection reflection;
};

struct Formats {
    Formats() noexcept {
        REQUIRE(LoadShaderFormatDefinition(ShaderFormatDirectory, "D3D12_SM6", d3d12));
        REQUIRE(LoadShaderFormatDefinition(ShaderFormatDirectory, "D3D11_SM5", d3d11));
        REQUIRE(LoadShaderFormatDefinition(ShaderFormatDirectory, "VULKAN_SM5", vulkan));
        REQUIRE(LoadShaderFormatDefinition(ShaderFormatDirectory, "GLES31_PC", gles));
    }

    ShaderFormatDefinition d3d12;
    ShaderFormatDefinition d3d11;
    ShaderFormatDefinition vulkan;
    ShaderFormatDefinition gles;
};

// 규칙 결과다. 실패면 진단에 part가 있어야 참이다(part가 null이면 성공을 기대한다).
bool Expect(const ShaderFormatDefinition& format, const Case& test, const char* part,
            Span<const ShaderParameterLayout> layouts = {}) noexcept {
    ShaderBindingLayout layout(TestAllocator());
    String diagnostics(TestAllocator());
    const bool ok =
        NormalizeShaderReflection(format, test.reflection, layouts, layout, diagnostics);
    const bool expected =
        part == nullptr ? ok : (!ok && strstr(diagnostics.CStr(), part) != nullptr);
    if (!expected) {
        fprintf(stderr, "binding rules on %s (expected %s): %s\n", format.id,
                part == nullptr ? "success" : part, ok ? "success" : diagnostics.CStr());
    }

    return expected;
}

// 상수 버퍼 멤버 하나로 된 PS의 결과다.
bool ExpectBuffer(const ShaderFormatDefinition& format,
                  std::initializer_list<ShaderReflectedMember> members, const char* part) noexcept {
    Case test(ShaderStagePixel);
    test.Out("SV_Target");
    test.Buffer("Probe", 0, members);
    return Expect(format, test, part);
}

// 정상 VS 한 벌이다.
Case SampleVertex() noexcept {
    Case test(ShaderStageVertex);
    test.In("ATTRIBUTE", 0);
    test.In("ATTRIBUTE", 3);
    test.In("SV_VertexID");
    test.Out("SV_Position");
    test.Out("TEXCOORD", 0);
    test.Out("TEXCOORD", 1);
    test.Buffer(
        "View", 0,
        {Matrix("viewProjection", 0), M("tint", B::Float, 4, 64), M("time", B::Float, 1, 80)});
    test.Buffer("Draw", 0, {M("color", B::Float, 4, 0)}, true);
    test.Resource(ShaderResourceKind::SampledTexture, "Albedo", 0);
    test.Resource(ShaderResourceKind::SampledTexture, "Noise", 1);
    test.Resource(ShaderResourceKind::Sampler, "AlbedoSampler", 0);
    test.Resource(ShaderResourceKind::Sampler, "SharedWrapSampler", 1);
    test.Resource(ShaderResourceKind::StorageBufferRead, "Lights", 2);
    return test;
}

constexpr ShaderParameterMember ViewMembers[] = {
    {"viewProjection", ShaderParameterType::Float4x4, 0, 0},
    {"tint", ShaderParameterType::Float4, 64, 0},
    {"time", ShaderParameterType::Float, 80, 0}};

} // namespace

TEST(ShaderBindingRules_NormalizesASampleShader) {
    const Formats formats;
    const Case test = SampleVertex();
    ShaderBindingLayout layout(TestAllocator());
    String diagnostics(TestAllocator());
    REQUIRE(NormalizeShaderReflection(formats.d3d12, test.reflection, {}, layout, diagnostics));
    CHECK(layout.stage == ShaderStageVertex && layout.attributeMask == 0b1001 &&
          layout.renderTargetMask == 0 && layout.rootConstantBytes == 16);
    // 자원은 (종류, slot) 순이고 루트 상수는 자원 표에 없다.
    REQUIRE(layout.resources.Size() == 6);
    CHECK(layout.resources[0].kind == ShaderResourceKind::ConstantBuffer &&
          layout.resources[0].name == StringView("View") &&
          layout.resources[0].constantBufferBytes == 96 && layout.resources[0].memberCount == 3);
    CHECK(layout.resources[1].name == StringView("Albedo") &&
          layout.resources[1].samplerSource == ShaderSamplerSource::Own);
    CHECK(layout.resources[2].name == StringView("Noise") &&
          layout.resources[2].samplerSource == ShaderSamplerSource::None);
    CHECK(layout.resources[3].kind == ShaderResourceKind::StorageBufferRead &&
          layout.resources[3].slot == 2);
    CHECK(layout.resources[4].name == StringView("AlbedoSampler") &&
          !layout.resources[4].sharedSampler);
    CHECK(layout.resources[5].sharedSampler);
    const Span<const ShaderBindingMember> members = layout.MembersOf(layout.resources[0]);
    REQUIRE(members.Size() == 3);
    CHECK(members[0].bytes == 64 && members[1].offset == 64 && members[2].offset == 80 &&
          members[2].bytes == 4);
    // 같은 리플렉션은 모든 형식에서 같은 규칙이다(상한 안).
    CHECK(Expect(formats.vulkan, test, nullptr));
    CHECK(Expect(formats.d3d11, test, nullptr));

    // 레이아웃 표 대조: 맞으면 통과, 오프셋·이름·크기·루트 표시가 다르면 오류.
    const ShaderParameterLayout good[] = {{"View", false, 96, ViewMembers}};
    CHECK(Expect(formats.d3d12, test, nullptr, good));
    const ShaderParameterMember moved[] = {{"viewProjection", ShaderParameterType::Float4x4, 0, 0},
                                           {"tint", ShaderParameterType::Float4, 64, 0},
                                           {"time", ShaderParameterType::Float, 84, 0}};
    const ShaderParameterLayout wrongOffset[] = {{"View", false, 96, moved}};
    CHECK(Expect(formats.d3d12, test, "does not match the shader member", wrongOffset));
    const ShaderParameterLayout wrongSize[] = {{"View", false, 112, ViewMembers}};
    CHECK(Expect(formats.d3d12, test, "total size", wrongSize));
    const ShaderParameterLayout wrongRoot[] = {{"View", true, 96, ViewMembers}};
    CHECK(Expect(formats.d3d12, test, "root constants", wrongRoot));
    const ShaderParameterMember color[] = {{"color", ShaderParameterType::Float4, 0, 0}};
    const ShaderParameterLayout rootTable[] = {{"Draw", true, 16, color}};
    CHECK(Expect(formats.d3d12, test, nullptr, rootTable));
    const ShaderParameterLayout rootAsBuffer[] = {{"Draw", false, 16, color}};
    CHECK(Expect(formats.d3d12, test, "root constants", rootAsBuffer));
}

TEST(ShaderBindingRules_ConstantBufferCounterexamples) {
    const Formats formats;
    const ShaderFormatDefinition& format = formats.d3d12;
    // 양성 대조.
    CHECK(ExpectBuffer(format, {M("a", B::Float, 3, 0), M("b", B::Float, 1, 12)}, nullptr));
    CHECK(ExpectBuffer(format, {M("a", B::Float, 4, 0, 4), Matrix("m", 64)}, nullptr));
    CHECK(ExpectBuffer(format,
                       {M("a", B::Float, 1, 0), M("s", B::Struct, 0, 16, 0, Top, 0, 32),
                        M("x", B::Float, 4, 0, 0, 1), M("y", B::Int, 4, 16, 0, 1)},
                       nullptr));
    // 5항 반례.
    CHECK(ExpectBuffer(format, {M("a", B::Float, 1, 0, 2), M("b", B::Float, 1, 20)},
                       "arrays need 16-byte elements"));
    CHECK(ExpectBuffer(format, {Matrix("m", 0, 3, 3), M("s", B::Float, 1, 44)},
                       "type is not allowed"));
    CHECK(ExpectBuffer(format, {M("flag", B::Bool, 1, 0)}, "type is not allowed"));
    CHECK(ExpectBuffer(format, {M("d", B::Double, 1, 0)}, "type is not allowed"));
    CHECK(ExpectBuffer(format, {M("h", B::Half, 1, 0)}, "type is not allowed"));
    CHECK(ExpectBuffer(format, {M("a", B::Float, 1, 0), M("b", B::Float, 3, 4)},
                       "packing rule gives 16"));
    CHECK(ExpectBuffer(format, {M("a", B::Float, 1, 0), M("b", B::Float, 1, 8)},
                       "packing rule gives 4"));
    CHECK(ExpectBuffer(format, {M("a", B::Float, 1, 0), M("b", B::Int, 1, 4)}, "must not mix"));
    CHECK(ExpectBuffer(format,
                       {M("s", B::Struct, 0, 0, 2, Top, 0, 32), M("x", B::Float, 4, 0, 0, 0)},
                       "arrays of structs"));
    CHECK(ExpectBuffer(format,
                       {M("s", B::Struct, 0, 0, 0, Top, 0, 32),
                        M("t", B::Struct, 0, 0, 0, 0, 0, 16), M("x", B::Float, 4, 0, 0, 1)},
                       "nest only one level"));
    CHECK(ExpectBuffer(format,
                       {M("s", B::Struct, 0, 0, 0, Top, 0, 4), M("x", B::Float, 1, 0, 0, 0)},
                       "explicit padding"));
    CHECK(ExpectBuffer(format,
                       {M("s", B::Struct, 0, 8, 0, Top, 0, 16), M("x", B::Float, 4, 0, 0, 0)},
                       "packing rule gives 0"));
    CHECK(ExpectBuffer(format, {}, "has no members"));
    // 이름 없는 전역.
    Case globals(ShaderStagePixel);
    globals.Out("SV_Target");
    globals.reflection.hasGlobals = true;
    CHECK(Expect(format, globals, "$Globals"));
}

TEST(ShaderBindingRules_LimitsUseTheJobFormat) {
    const Formats formats;
    // 공유 샘플러로 텍스처 17장: 128장 형식은 통과, 16장 형식은 그 형식만 오류(7.3절 (b)·(h)).
    Case textures(ShaderStagePixel);
    textures.Out("SV_Target");
    char names[17][16];
    for (uint32 index = 0; index < 17; ++index) {
        snprintf(names[index], sizeof(names[index]), "Layer%u", index);
        textures.Resource(ShaderResourceKind::SampledTexture, names[index], index);
    }

    textures.Resource(ShaderResourceKind::Sampler, "SharedWrapSampler", 0);
    CHECK(Expect(formats.d3d12, textures, nullptr));
    CHECK(Expect(formats.vulkan, textures, nullptr));
    CHECK(
        Expect(formats.d3d11, textures, "sampled texture count 17 exceeds the D3D11_SM5 limit 16"));
    CHECK(Expect(formats.gles, textures, "exceeds the GLES31_PC limit"));

    // render target 5개: GLES 형식(4)만 오류.
    Case targets(ShaderStagePixel);
    targets.Out("SV_Target", 0);
    targets.Out("SV_Target", 4);
    targets.Out("SV_Depth");
    CHECK(Expect(formats.d3d12, targets, nullptr));
    CHECK(Expect(formats.gles, targets, "render target count 5"));

    // PS UAV는 uavStages(CS) 밖이다. CS는 통과한다.
    Case pixelUav(ShaderStagePixel);
    pixelUav.Out("SV_Target");
    pixelUav.Resource(ShaderResourceKind::StorageBufferReadWrite, "Output", 0);
    CHECK(Expect(formats.d3d12, pixelUav, "uavStages"));
    Case computeUav(ShaderStageCompute);
    computeUav.In("SV_DispatchThreadID");
    computeUav.Resource(ShaderResourceKind::StorageBufferReadWrite, "Output", 0);
    computeUav.Resource(ShaderResourceKind::StorageImage, "Image", 1);
    CHECK(Expect(formats.d3d12, computeUav, nullptr));

    // 루트 상수 144 byte는 128 상한 밖이다.
    Case root(ShaderStageVertex);
    root.Buffer("Draw", 0, {M("a", B::Float, 4, 0, 9)}, true);
    CHECK(Expect(formats.d3d12, root, "rootConstantBytes"));

    // 단계 사이 vec4 16개: D3D12(16 이상)는 통과, 한도가 작은 형식은 오류.
    Case interStage(ShaderStageVertex);
    interStage.Out("SV_Position");
    for (uint32 index = 0; index < 16; ++index) {
        interStage.Out("TEXCOORD", index);
    }

    const bool d3d12Fits = formats.d3d12.limits.interStageVectors >= 16;
    CHECK(Expect(formats.d3d12, interStage, d3d12Fits ? nullptr : "inter-stage vector count"));
    if (formats.gles.limits.interStageVectors < 16) {
        CHECK(Expect(formats.gles, interStage, "inter-stage vector count 16"));
    }

    // D3D register 번호도 상한 안이어야 한다(SPIRV-Cross 대체 경로의 s24 같은 값).
    Case highRegister(ShaderStagePixel);
    highRegister.Out("SV_Target");
    highRegister.Resource(ShaderResourceKind::SampledTexture, "Albedo", 0);
    highRegister.Resource(ShaderResourceKind::Sampler, "AlbedoSampler", 24);
    CHECK(Expect(formats.d3d11, highRegister, "sampler register end 25"));
    CHECK(Expect(formats.vulkan, highRegister, nullptr));
}

TEST(ShaderBindingRules_SemanticsPairsAndCollisions) {
    const Formats formats;
    const ShaderFormatDefinition& format = formats.d3d12;
    const auto vertexWith = [](const char* input, uint32 inputIndex, const char* output,
                               uint32 outputIndex) noexcept {
        Case test(ShaderStageVertex);
        test.In(input, inputIndex);
        test.Out("SV_Position");
        test.Out(output, outputIndex);
        return test;
    };
    CHECK(Expect(format, vertexWith("ATTRIBUTE", 15, "TEXCOORD", 15), nullptr));
    CHECK(Expect(format, vertexWith("POSITION", 0, "TEXCOORD", 0),
                 "vertex input semantic POSITION0"));
    CHECK(Expect(format, vertexWith("ATTRIBUTE", 16, "TEXCOORD", 0), "out of range"));
    CHECK(Expect(format, vertexWith("ATTRIBUTE", 0, "COLOR", 0), "vertex output semantic COLOR0"));
    CHECK(Expect(format, vertexWith("ATTRIBUTE", 0, "SV_RenderTargetArrayIndex", 0),
                 "vertexViewportLayerOutput"));
    Case twice = vertexWith("ATTRIBUTE", 1, "TEXCOORD", 0);
    twice.In("ATTRIBUTE", 1);
    CHECK(Expect(format, twice, "used twice"));
    // PS: SV_IsFrontFace 입력은 되고 SV_Target8은 오류.
    Case pixel(ShaderStagePixel);
    pixel.In("SV_Position");
    pixel.In("SV_IsFrontFace");
    pixel.In("TEXCOORD", 2);
    pixel.Out("SV_Target", 7);
    CHECK(Expect(format, pixel, nullptr));
    pixel.Out("SV_Target", 8);
    CHECK(Expect(format, pixel, "SV_Target8"));
    Case compute(ShaderStageCompute);
    compute.In("SV_GroupIndex");
    compute.In("SV_GroupID");
    CHECK(Expect(format, compute, nullptr));
    compute.In("SV_Position");
    CHECK(Expect(format, compute, "compute input"));

    // 짝 이름(9항).
    Case pairs(ShaderStagePixel);
    pairs.Out("SV_Target");
    pairs.Resource(ShaderResourceKind::SampledTexture, "Albedo", 0);
    pairs.Resource(ShaderResourceKind::Sampler, "LinearSampler", 0);
    CHECK(Expect(format, pairs, "sampler 'LinearSampler'"));
    Case reserved(ShaderStagePixel);
    reserved.Out("SV_Target");
    reserved.Resource(ShaderResourceKind::SampledTexture, "SharedWrap", 0);
    CHECK(Expect(format, reserved, "reserved shared sampler name"));
    Case bare(ShaderStagePixel);
    bare.Out("SV_Target");
    bare.Resource(ShaderResourceKind::Sampler, "Sampler", 0);
    CHECK(Expect(format, bare, "sampler 'Sampler'"));

    // binding 충돌·자원 배열·루트 상수 둘·이름.
    Case collision(ShaderStagePixel);
    collision.Out("SV_Target");
    collision.Resource(ShaderResourceKind::SampledTexture, "A", 3);
    collision.Resource(ShaderResourceKind::SampledTexture, "B", 3);
    CHECK(Expect(format, collision, "binding collision"));
    Case array(ShaderStagePixel);
    array.Out("SV_Target");
    array.Resource(ShaderResourceKind::SampledTexture, "A", 0);
    array.reflection.resources[0].count = 4;
    CHECK(Expect(format, array, "resource arrays"));
    Case roots(ShaderStageVertex);
    roots.Buffer("DrawA", 0, {M("a", B::Float, 4, 0)}, true);
    roots.Buffer("DrawB", 0, {M("a", B::Float, 4, 0)}, true);
    CHECK(Expect(format, roots, "at most one ROCKS_ROOT_CONSTANTS"));
    Case badName(ShaderStagePixel);
    badName.Out("SV_Target");
    badName.Resource(ShaderResourceKind::SampledTexture, "type.Texture", 0);
    CHECK(Expect(format, badName, "value 1 rules"));
    // 시맨틱 이름 나누기.
    const ShaderReflectedSignature split = SplitShaderSemantic("TEXCOORD12");
    CHECK(split.semantic == StringView("TEXCOORD") && split.index == 12);
    const ShaderReflectedSignature plain = SplitShaderSemantic("SV_Target");
    CHECK(plain.semantic == StringView("SV_Target") && plain.index == 0);
}

} // namespace shader_reflection_rules_tests_file
