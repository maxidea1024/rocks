#include <Base/Endian.h>
#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderFormatFacts.h>
#include <ShaderCore/ShaderGlesBindingHeader.h>
#include <ShaderCore/ShaderLibrary.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <ShaderCore/ShaderParameterTypes.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <TestSupport/Test.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// ShaderCore 형식 시험이다(Docs/Design/ShaderSystem 12.2.1절 SSD7·SSD25–SSD27, SH2a2): 형식 사실
// id, 손으로 쓴 파라미터 레이아웃 규칙(규칙마다 양성·음성)과 서명(정규 byte를 손으로 만들어 대조),
// 정렬 타입 오프셋, 바인딩 layout·GLES 바인딩 머리의 왕복·규칙·모든 잘린 길이·버전, 라이브러리 v1의
// 왕복· 입력 순서와 무관한 같은 byte·코드 중복 제거·만들기 거부·모든 byte 뒤집기·다시 봉인한 표적
// 손상 (이름 해시 불일치 — CP7 포함)·버전.
using namespace rocks;

namespace shader_core_format_tests_file {

using namespace rocks::shader;

namespace {

SystemAllocator& TestAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 파일 SHA-256 칸 오프셋이다(공개 헤더 표).
constexpr usize FileShaAt = 232;
// 첫 항목 행 오프셋이다.
constexpr usize FirstEntryAt = ShaderLibraryHeaderBytes;

// ---------------------------------------------------------------------------------------------
// 파라미터 레이아웃

struct alignas(16) SampleConstants {
    ShaderFloat4x4 transform;
    ShaderFloat2 uvScale;
    ShaderFloat2 uvBias;
    ShaderFloat4 colors[2];
    ShaderUint4 flags;
};

static_assert(offsetof(SampleConstants, transform) == 0);
static_assert(offsetof(SampleConstants, uvScale) == 64);
static_assert(offsetof(SampleConstants, uvBias) == 72);
static_assert(offsetof(SampleConstants, colors) == 80);
static_assert(offsetof(SampleConstants, flags) == 112);
static_assert(sizeof(SampleConstants) == 128);

constexpr ShaderParameterMember SampleMembers[] = {
    {"transform", ShaderParameterType::Float4x4, offsetof(SampleConstants, transform), 0},
    {"uvScale", ShaderParameterType::Float2, offsetof(SampleConstants, uvScale), 0},
    {"uvBias", ShaderParameterType::Float2, offsetof(SampleConstants, uvBias), 0},
    {"colors", ShaderParameterType::Float4, offsetof(SampleConstants, colors), 2},
    {"flags", ShaderParameterType::Uint4, offsetof(SampleConstants, flags), 0}};
constexpr ShaderParameterLayout SampleLayout{"SampleConstants", true, sizeof(SampleConstants),
                                             SampleMembers};

// 멤버 하나로 된 표의 검사 결과다.
bool AcceptsMembers(std::initializer_list<ShaderParameterMember> members, uint32 totalBytes,
                    bool rootConstants = false) noexcept {
    const ShaderParameterLayout layout{
        "Probe", rootConstants, totalBytes,
        Span<const ShaderParameterMember>(members.begin(), members.size())};
    return static_cast<bool>(ValidateShaderParameterLayout(layout));
}

// 서명 정규 byte를 손으로 만든다(공개 헤더 순서).
void AppendU32(Array<uint8>& out, uint32 value) noexcept {
    uint8 bytes[4];
    StoreLittleEndian<uint32>(bytes, value);
    for (const uint8 byte : bytes) {
        REQUIRE(out.TryPushBack(byte));
    }
}

void AppendName(Array<uint8>& out, StringView name) noexcept {
    REQUIRE(out.TryPushBack(static_cast<uint8>(name.ByteCount())));
    for (usize index = 0; index < name.ByteCount(); ++index) {
        REQUIRE(out.TryPushBack(static_cast<uint8>(name.Data()[index])));
    }
}

// ---------------------------------------------------------------------------------------------
// 바인딩 layout

// 픽셀 셰이더 하나의 layout(상수 버퍼 둘·텍스처·구조화 SRV·샘플러 둘)이다.
void FillSampleLayout(ShaderBindingLayout& layout) noexcept {
    layout.stage = ShaderStagePixel;
    layout.rootConstantBytes = 64;
    layout.rootConstantStructId = 0x0102030405060708ull;
    layout.renderTargetMask = 0b101;
    ShaderBindingResource view;
    view.kind = ShaderResourceKind::ConstantBuffer;
    view.slot = 0;
    view.name = "View";
    view.constantBufferBytes = 32;
    view.structId = 0x1112131415161718ull;
    view.firstMember = 0;
    view.memberCount = 2;
    ShaderBindingResource pass = view;
    pass.structId = 0;
    pass.slot = 1;
    pass.name = "Pass";
    pass.constantBufferBytes = 16;
    pass.firstMember = 2;
    pass.memberCount = 1;
    ShaderBindingResource albedo;
    albedo.kind = ShaderResourceKind::SampledTexture;
    albedo.name = "Albedo";
    albedo.samplerSource = ShaderSamplerSource::Own;
    albedo.shape = ShaderResourceShape::Texture2D;
    albedo.component = ShaderComponentType::Float;
    albedo.firstMember = 3;
    ShaderBindingResource lights;
    lights.kind = ShaderResourceKind::StorageBufferRead;
    lights.slot = 2;
    lights.name = "Lights";
    lights.shape = ShaderResourceShape::StructuredBuffer;
    lights.structureStride = 32;
    lights.firstMember = 3;
    ShaderBindingResource sampler;
    sampler.kind = ShaderResourceKind::Sampler;
    sampler.name = "AlbedoSampler";
    sampler.firstMember = 3;
    ShaderBindingResource shared = sampler;
    shared.slot = 1;
    shared.name = "SharedWrapSampler";
    shared.sharedSampler = true;
    for (const ShaderBindingResource& resource : {view, pass, albedo, lights, sampler, shared}) {
        REQUIRE(layout.resources.TryPushBack(resource));
    }

    for (const ShaderBindingMember& member :
         {ShaderBindingMember{"viewProjection", 0, 16}, ShaderBindingMember{"time", 16, 4},
          ShaderBindingMember{"tint", 0, 16}}) {
        REQUIRE(layout.members.TryPushBack(member));
    }
}

// layout을 한 번 바꿔 쓰기가 거부되는가.
template <typename Change>
bool EncodeRejects(Change change) noexcept {
    ShaderBindingLayout layout(TestAllocator());
    FillSampleLayout(layout);
    change(layout);
    Array<uint8> bytes(TestAllocator());
    const Result<void> encoded = EncodeShaderBindingLayout(layout, bytes);
    return !encoded && encoded.Error() == ErrorCode::InvalidArgument && bytes.IsEmpty();
}

// ---------------------------------------------------------------------------------------------
// GLES 바인딩 머리

void FillSampleGles(ShaderGlesBindingHeader& header) noexcept {
    header.stage = ShaderStageVertex;
    header.attributeMask = 0b11;
    for (const ShaderGlesUniformArray& array :
         {ShaderGlesUniformArray{0, ShaderGlesUniformType::Float, 3},
          ShaderGlesUniformArray{0, ShaderGlesUniformType::Uint, 1},
          ShaderGlesUniformArray{ShaderGlesRootConstantsBuffer, ShaderGlesUniformType::Float, 4}}) {
        REQUIRE(header.arrays.TryPushBack(array));
    }

    REQUIRE(header.samplers.TryPushBack(ShaderGlesSampler{16, "vs0", ShaderSamplerSource::Own}));
    REQUIRE(header.samplers.TryPushBack(
        ShaderGlesSampler{17, "vs1", ShaderSamplerSource::SharedClamp}));
    for (const ShaderGlesCopy& copy :
         {ShaderGlesCopy{0, 0, 48, 0, 0}, ShaderGlesCopy{0, 48, 16, 1, 0},
          ShaderGlesCopy{ShaderGlesRootConstantsBuffer, 0, 64, 2, 0}}) {
        REQUIRE(header.copies.TryPushBack(copy));
    }
}

template <typename Change>
bool GlesEncodeRejects(Change change) noexcept {
    ShaderGlesBindingHeader header(TestAllocator());
    FillSampleGles(header);
    change(header);
    Array<uint8> bytes(TestAllocator());
    const Result<void> encoded = EncodeShaderGlesBindingHeader(header, bytes);
    return !encoded && bytes.IsEmpty();
}

// ---------------------------------------------------------------------------------------------
// 라이브러리

ROCKS_SHADER_TYPE_NAME(LibraryVertexName, "LibraryVertex");
ROCKS_SHADER_TYPE_NAME(LibraryPixelName, "LibraryPixel");

constexpr uint8 CodeA[] = {1, 2, 3, 4, 5};
constexpr uint8 CodeB[] = {9, 8, 7};
constexpr uint8 LayoutBytes[] = {0x10, 0x20};
constexpr ShaderLibraryValue ValuesA[] = {{ShaderLibraryValueCode, CodeA},
                                          {ShaderLibraryValueBindingLayout, LayoutBytes}};
constexpr ShaderLibraryValue ValuesB[] = {{ShaderLibraryValueCode, CodeB}};

ShaderFormatFacts SampleFacts() noexcept {
    ShaderFormatFacts facts;
    CHECK(SetShaderFormatFactsId(facts, "VULKAN_SM5"));
    facts.backend = ShaderBackend::Vulkan;
    facts.shaderModel = ShaderVersion{6, 6};
    facts.limits.constantBuffers = 12;
    facts.limits.sampledTextures = 128;
    facts.limits.uniformVectors[1] = 224;
    facts.features = ShaderFeatureCompute | ShaderFeatureVertexViewportLayerOutput;
    facts.deviceRequirements = DeviceRequiresVulkan1_1;
    facts.platformClasses = ShaderPlatformClassPc | ShaderPlatformClassMobile;
    return facts;
}

Sha256Digest Digest(uint8 fill) noexcept {
    Sha256Digest digest;
    memset(digest.bytes, fill, sizeof(digest.bytes));
    return digest;
}

// 항목 넷(두 항목이 같은 코드)이다.
struct SampleEntries {
    SampleEntries() noexcept {
        ShaderLibraryBuildEntry vertex;
        vertex.key = ShaderLibraryKey{0, LibraryVertexName.hash, NoVertexInputType, 0};
        vertex.typeName = LibraryVertexName.name;
        vertex.stage = ShaderStageVertex;
        vertex.values = ValuesA;
        entries[0] = vertex;
        entries[1] = vertex;
        entries[1].key.permutationId = 3;
        entries[1].developmentOnly = true;
        entries[1].values = ValuesB;
        ShaderLibraryBuildEntry pixel;
        pixel.key = ShaderLibraryKey{0, LibraryPixelName.hash, 77, 1};
        pixel.typeName = LibraryPixelName.name;
        pixel.stage = ShaderStagePixel;
        pixel.runtimeFeatures = ShaderFeatureVertexViewportLayerOutput;
        pixel.layoutSignature = Digest(0x5a);
        pixel.values = ValuesA;
        entries[2] = pixel;
        entries[3] = pixel;
        entries[3].key.bundleId = 9;
    }

    ShaderLibraryBuildDesc Desc() const noexcept {
        ShaderLibraryBuildDesc desc;
        desc.kind = ShaderLibraryKind::Project;
        desc.facts = SampleFacts();
        desc.formatDefinitionSha256 = Digest(0x11);
        desc.typeSetSha256 = Digest(0x22);
        desc.entries = entries;
        return desc;
    }

    ShaderLibraryBuildEntry entries[4];
};

bool BuildSample(Array<uint8>& bytes) noexcept {
    const SampleEntries sample;
    return static_cast<bool>(BuildShaderLibrary(sample.Desc(), TestAllocator(), bytes));
}

// 파일 SHA-256을 다시 계산해 넣는다(표적 손상이 SHA 검사 뒤 규칙에 걸리게).
void Reseal(Array<uint8>& bytes) noexcept {
    memset(bytes.Data() + FileShaAt, 0, Sha256Digest::Bytes);
    const Sha256Digest sha = Sha256::Hash(bytes.View());
    memcpy(bytes.Data() + FileShaAt, sha.bytes, Sha256Digest::Bytes);
}

// 한 번 바꾸고 다시 봉인한 byte가 CorruptPackage인가(이유에 part가 있어야 한다).
template <typename Change>
bool CorruptAfter(Change change, const char* part) noexcept {
    Array<uint8> bytes(TestAllocator());
    if (!BuildSample(bytes)) {
        CHECK(false);
        return false;
    }

    change(bytes);
    Reseal(bytes);
    ShaderLibraryView view;
    const ShaderReadStatus opened = view.Open(bytes.View());
    if (opened.error != ShaderReadError::CorruptPackage || strstr(opened.reason, part) == nullptr) {
        fprintf(stderr, "library corruption '%s' gave %s: %s\n", part,
                ShaderReadErrorName(opened.error), opened.reason);
        return false;
    }

    return !view.IsOpen();
}

// 만들기 입력을 한 번 바꿨을 때 거부되는가.
template <typename Change>
bool BuildRejects(Change change) noexcept {
    SampleEntries sample;
    ShaderLibraryBuildDesc desc = sample.Desc();
    change(sample, desc);
    Array<uint8> bytes(TestAllocator());
    const char* reason = nullptr;
    const Result<void> built = BuildShaderLibrary(desc, TestAllocator(), bytes, &reason);
    return !built && reason != nullptr && bytes.IsEmpty();
}

} // namespace

TEST(ShaderCore_FormatFactsId) {
    ShaderFormatFacts facts;
    CHECK(SetShaderFormatFactsId(facts, "D3D12_SM6"));
    CHECK(facts.FormatId() == StringView("D3D12_SM6"));
    // 남는 byte는 0이다(짧은 id로 다시 채워도).
    CHECK(SetShaderFormatFactsId(facts, "GL"));
    CHECK(facts.FormatId() == StringView("GL"));
    for (usize index = 2; index < MaxShaderFormatIdBytes; ++index) {
        CHECK(facts.formatId[index] == '\0');
    }

    const char longest[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ01234";
    static_assert(sizeof(longest) == MaxShaderFormatIdBytes);
    CHECK(SetShaderFormatFactsId(facts, longest));
    for (const char* bad : {"", "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", "D3D12-SM6", "D3D12 SM6"}) {
        CHECK(!SetShaderFormatFactsId(facts, bad));
    }

    CHECK(facts.FormatId() == StringView(longest));
    CHECK(StringView(ShaderReadErrorName(ShaderReadError::CorruptPackage)) == "CorruptPackage");
    CHECK(StringView(ShaderReadErrorName(static_cast<ShaderReadError>(9))) == "unknown");
}

TEST(ShaderCore_ParameterLayoutRules) {
    CHECK(ValidateShaderParameterLayout(SampleLayout));
    // 타입 표.
    CHECK(ShaderParameterTypeBytes(ShaderParameterType::Float3) == 12);
    CHECK(ShaderParameterTypeAlignment(ShaderParameterType::Float3) == 16);
    CHECK(ShaderParameterTypeAlignment(ShaderParameterType::Int2) == 8);
    CHECK(ShaderParameterTypeBytes(ShaderParameterType::Float4x4) == 64);
    CHECK(ShaderParameterTypeBase(ShaderParameterType::Uint3) == ShaderParameterBaseType::Uint);
    CHECK(StringView(ShaderParameterTypeName(ShaderParameterType::Int4)) == "int4");
    CHECK(ShaderParameterTypeBytes(static_cast<ShaderParameterType>(13)) == 0);

    using T = ShaderParameterType;
    // 양성: 3성분 뒤 같은 타입 스칼라(D3D·std140 모두 12), 정렬 맞는 2성분, 16 byte 원소 배열.
    CHECK(AcceptsMembers({{"a", T::Float3, 0, 0}, {"b", T::Float, 12, 0}}, 16));
    CHECK(AcceptsMembers({{"a", T::Int, 0, 0}, {"b", T::Int, 4, 0}, {"c", T::Int2, 8, 0}}, 16));
    CHECK(AcceptsMembers({{"a", T::Uint4, 0, 4}, {"b", T::Float4x4, 64, 2}}, 192));
    CHECK(AcceptsMembers({{"a", T::Float, 0, 0}, {"b", T::Int, 16, 0}}, 32));
    CHECK(AcceptsMembers({{"a", T::Float4, 0, 0}}, 128, true));
    // 음성: 규칙마다.
    CHECK(!AcceptsMembers({}, 16));
    CHECK(!AcceptsMembers({{"a", static_cast<T>(13), 0, 0}}, 16));
    CHECK(!AcceptsMembers({{"a", T::Float2, 4, 0}}, 16));
    CHECK(!AcceptsMembers({{"a", T::Float3, 4, 0}}, 32));
    CHECK(!AcceptsMembers({{"a", T::Float4x4, 8, 0}}, 80));
    CHECK(!AcceptsMembers({{"a", T::Float, 0, 4}}, 16));
    CHECK(!AcceptsMembers({{"a", T::Float2, 0, 2}}, 16));
    CHECK(!AcceptsMembers({{"a", T::Float, 4, 0}, {"b", T::Float, 0, 0}}, 16));
    CHECK(!AcceptsMembers({{"a", T::Float2, 0, 0}, {"b", T::Float, 4, 0}}, 16));
    CHECK(!AcceptsMembers({{"a", T::Float, 0, 0}, {"b", T::Int, 4, 0}}, 16));
    CHECK(!AcceptsMembers({{"a", T::Float3, 0, 0}, {"b", T::Uint, 12, 0}}, 16));
    CHECK(!AcceptsMembers({{"a", T::Float4, 0, 2}}, 16));
    CHECK(!AcceptsMembers({{"a", T::Float, 0, 0}}, 20));
    CHECK(!AcceptsMembers({{"a", T::Float, 0, 0}}, 0));
    CHECK(!AcceptsMembers({{"a", T::Float, 0, 0}}, MaxShaderConstantBufferBytes + 16));
    CHECK(!AcceptsMembers({{"a", T::Float4, 0, 0}}, 144, true));
    CHECK(!AcceptsMembers({{"a", T::Float, 0, 0}, {"a", T::Float, 4, 0}}, 16));
    CHECK(!AcceptsMembers({{"2a", T::Float, 0, 0}}, 16));
    const char* reason = nullptr;
    const ShaderParameterLayout unnamed{"", false, 16, SampleMembers};
    CHECK(!ValidateShaderParameterLayout(unnamed, &reason));
    CHECK(reason != nullptr && strstr(reason, "name") != nullptr);

    // 목록 규칙: 이름 중복·루트 상수 둘·표 수 상한.
    const ShaderParameterLayout plain{"PassConstants", false, 128, SampleMembers};
    const ShaderParameterLayout pair[] = {SampleLayout, plain};
    CHECK(ValidateShaderParameterLayouts(pair));
    const ShaderParameterLayout twoRoots[] = {SampleLayout, {"Other", true, 128, SampleMembers}};
    CHECK(!ValidateShaderParameterLayouts(twoRoots, &reason));
    CHECK(strstr(reason, "root constants") != nullptr);
    const ShaderParameterLayout sameName[] = {plain, plain};
    CHECK(!ValidateShaderParameterLayouts(sameName));
    ShaderParameterLayout many[MaxShaderParameterLayouts + 1];
    char names[MaxShaderParameterLayouts + 1][8];
    for (usize index = 0; index < MaxShaderParameterLayouts + 1; ++index) {
        snprintf(names[index], sizeof(names[index]), "L%zu", index);
        many[index] = ShaderParameterLayout{StringView(names[index]), false, 128, SampleMembers};
    }

    CHECK(ValidateShaderParameterLayouts(
        Span<const ShaderParameterLayout>(many, MaxShaderParameterLayouts)));
    CHECK(!ValidateShaderParameterLayouts(many));
}

TEST(ShaderCore_ParameterLayoutSignatureIsCanonicalBytes) {
    CHECK(ShaderParameterLayoutSignature({}) == Sha256Digest{});
    const ShaderParameterMember passMembers[] = {{"tint", ShaderParameterType::Float4, 0, 0},
                                                 {"count", ShaderParameterType::Uint, 16, 0}};
    const ShaderParameterLayout layouts[] = {SampleLayout, {"Pass", false, 32, passMembers}};
    Array<uint8> canonical(TestAllocator());
    AppendU32(canonical, 2);
    for (const ShaderParameterLayout& layout : layouts) {
        AppendName(canonical, layout.name);
        // 구조체 이름(손으로 쓴 표는 빈 값 — 16.2절 SSD53).
        AppendName(canonical, layout.structName);
        REQUIRE(canonical.TryPushBack(layout.rootConstants ? 1 : 0));
        AppendU32(canonical, layout.totalBytes);
        AppendU32(canonical, static_cast<uint32>(layout.members.Size()));
        for (const ShaderParameterMember& member : layout.members) {
            AppendName(canonical, member.name);
            REQUIRE(canonical.TryPushBack(static_cast<uint8>(member.type)));
            AppendU32(canonical, member.offset);
            AppendU32(canonical, member.arrayCount);
        }

        // 자원 수(손으로 쓴 표는 0 — 16.2절 SSD62).
        AppendU32(canonical, 0);
    }

    const Sha256Digest signature = ShaderParameterLayoutSignature(layouts);
    CHECK(signature == Sha256::Hash(canonical.View()));
    // 순서·칸 하나가 바뀌면 서명이 바뀐다.
    const ShaderParameterLayout swapped[] = {layouts[1], layouts[0]};
    CHECK(!(ShaderParameterLayoutSignature(swapped) == signature));
    const ShaderParameterMember renamed[] = {{"tint", ShaderParameterType::Float4, 0, 0},
                                             {"total", ShaderParameterType::Uint, 16, 0}};
    const ShaderParameterLayout changed[] = {SampleLayout, {"Pass", false, 32, renamed}};
    CHECK(!(ShaderParameterLayoutSignature(changed) == signature));
    const ShaderParameterMember retyped[] = {{"tint", ShaderParameterType::Float4, 0, 0},
                                             {"count", ShaderParameterType::Int, 16, 0}};
    const ShaderParameterLayout changedType[] = {SampleLayout, {"Pass", false, 32, retyped}};
    CHECK(!(ShaderParameterLayoutSignature(changedType) == signature));
    const ShaderParameterLayout root[] = {SampleLayout, {"Pass", true, 32, passMembers}};
    CHECK(!(ShaderParameterLayoutSignature(root) == signature));
}

TEST(ShaderCore_ParameterLayoutBytesRoundTrip) {
    const ShaderParameterMember passMembers[] = {{"tint", ShaderParameterType::Float4, 0, 0},
                                                 {"count", ShaderParameterType::Uint, 16, 0}};
    const ShaderParameterLayout layouts[] = {SampleLayout, {"Pass", false, 32, passMembers}};
    Array<uint8> bytes(TestAllocator());
    REQUIRE(EncodeShaderParameterLayouts(layouts, bytes));
    // 정규 byte의 SHA-256이 서명이다(SSD31 — 키 8행과 워커가 같은 byte를 본다).
    CHECK(Sha256::Hash(bytes.View()) == ShaderParameterLayoutSignature(layouts));
    ShaderParameterLayoutSet decoded(TestAllocator());
    REQUIRE(DecodeShaderParameterLayouts(bytes.View(), decoded));
    REQUIRE(decoded.View().Size() == 2);
    CHECK(ShaderParameterLayoutSignature(decoded.View()) ==
          ShaderParameterLayoutSignature(layouts));
    const ShaderParameterLayout* pass = decoded.Find("Pass");
    REQUIRE(pass != nullptr);
    CHECK(!pass->rootConstants && pass->totalBytes == 32 && pass->members.Size() == 2 &&
          pass->members[1].name == StringView("count") && pass->members[1].offset == 16);
    CHECK(decoded.Find("SampleConstants")->rootConstants && decoded.Find("Missing") == nullptr);
    // 빈 byte는 빈 목록이다.
    CHECK(DecodeShaderParameterLayouts({}, decoded) && decoded.View().IsEmpty());
    // 모든 잘린 길이·남는 byte·타입 범위 밖·규칙 위반은 손상이다.
    for (usize length = 1; length < bytes.Size(); ++length) {
        CHECK(DecodeShaderParameterLayouts(bytes.View().Left(length), decoded).error ==
              ShaderReadError::CorruptPackage);
    }

    REQUIRE(bytes.TryPushBack(0));
    CHECK(!DecodeShaderParameterLayouts(bytes.View(), decoded));
    (void)bytes.TryResize(bytes.Size() - 1);
    // 첫 멤버 타입 byte(u32 수, u8 길이 + 이름 15, u8 구조체 이름 길이 0, u8 표시, u32 크기, u32
    // 멤버 수, u8 길이 + 이름 9).
    const usize typeAt = 4 + 1 + 15 + 1 + 1 + 4 + 4 + 1 + 9;
    bytes[typeAt] = 14;
    CHECK(!DecodeShaderParameterLayouts(bytes.View(), decoded));
    bytes[typeAt] = static_cast<uint8>(ShaderParameterType::Float4x4);
    // 둘째 멤버(uvScale)를 float4x4로: uvBias와 겹친다.
    bytes[typeAt + 17] = static_cast<uint8>(ShaderParameterType::Float4x4);
    CHECK(!DecodeShaderParameterLayouts(bytes.View(), decoded));
    // 규칙을 어기는 목록은 쓰지 않는다.
    const ShaderParameterLayout twice[] = {SampleLayout, SampleLayout};
    Array<uint8> rejected(TestAllocator());
    CHECK(!EncodeShaderParameterLayouts(twice, rejected) && rejected.IsEmpty());
}

TEST(ShaderCore_RegistryChecksParameterLayouts) {
    constexpr ShaderParameterMember badMembers[] = {{"a", ShaderParameterType::Float2, 4, 0}};
    const ShaderParameterLayout bad[] = {{"Bad", false, 16, badMembers}};
    const ShaderParameterLayout good[] = {SampleLayout};
    ShaderTypeDesc desc = MakeShaderTypeDesc(LibraryVertexName, "/Engine/Private/Test.rsf",
                                             "MainVS", ShaderStageVertex, ShaderTypeOwner::Engine);
    desc.parameterLayouts = bad;
    ShaderTypeRegistry registry(TestAllocator());
    const ShaderTypeDesc* const list[] = {&desc};
    const Result<void> rejected = registry.Register(list);
    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
    CHECK(strstr(registry.LastFailure().reason, "alignment") != nullptr);
    CHECK(registry.Types().IsEmpty());
    desc.parameterLayouts = good;
    CHECK(registry.Register(list));
    registry.Unregister(list);
}

TEST(ShaderCore_BindingLayoutRoundTrip) {
    ShaderBindingLayout layout(TestAllocator());
    FillSampleLayout(layout);
    Array<uint8> bytes(TestAllocator());
    REQUIRE(EncodeShaderBindingLayout(layout, bytes));
    // 머리 golden(버전·단계·루트 상수·attribute·int·uint attribute·render target·단계 사이·자원
    // 수). v2: 단계 사이 마스크(0 — 시험 layout은 정하지 않았다)가 render target 마스크 뒤에 있다.
    // v3: attribute 마스크 뒤에 int·uint attribute 마스크가 있다. v4: 루트 상수 byte 뒤에 u64 루트
    // 상수 구조체 identity가 있다(ShaderSystem 16.2절 SSD54). v5: 형식 버전 5.
    const uint8 header[] = {5, 0, 0, 0, 2, 0, 0, 0, 64, 0, 0, 0, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0,  0, 5, 0, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0};
    REQUIRE(bytes.Size() > sizeof(header));
    CHECK(memcmp(bytes.Data(), header, sizeof(header)) == 0);
    // 첫 자원: 종류 0, 순번 0, 이름 "View", byte 32, 구조체 identity(v4), 멤버 오프셋 없음(v5 —
    // 상수 버퍼는 NoShaderParameterMember), 출처 0, 공유 0, 모양 0, 원소 0, stride 0, 멤버 2.
    const uint8 first[] = {0,    0,    0,    4,    'V',  'i',  'e',  'w',  32,   0,    0,    0,
                           0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0xff, 0xff, 0xff, 0xff,
                           0,    0,    0,    0,    0,    0,    0,    0,    2,    0};
    CHECK(memcmp(bytes.Data() + sizeof(header), first, sizeof(first)) == 0);

    ShaderBindingLayout decoded(TestAllocator());
    REQUIRE(DecodeShaderBindingLayout(bytes.View(), decoded));
    CHECK(decoded.rootConstantStructId == layout.rootConstantStructId);
    CHECK(decoded.resources[0].structId == layout.resources[0].structId);
    for (usize index = 0; index < layout.resources.Size(); ++index) {
        CHECK(decoded.resources[index].memberOffset == layout.resources[index].memberOffset);
    }
    CHECK(decoded.stage == layout.stage && decoded.rootConstantBytes == 64 &&
          decoded.renderTargetMask == 0b101 && decoded.attributeMask == 0);
    REQUIRE(decoded.resources.Size() == layout.resources.Size());
    REQUIRE(decoded.members.Size() == layout.members.Size());
    for (usize index = 0; index < layout.resources.Size(); ++index) {
        const ShaderBindingResource& a = layout.resources[index];
        const ShaderBindingResource& b = decoded.resources[index];
        CHECK(a.kind == b.kind && a.slot == b.slot && a.name == b.name &&
              a.constantBufferBytes == b.constantBufferBytes &&
              a.samplerSource == b.samplerSource && a.sharedSampler == b.sharedSampler &&
              a.shape == b.shape && a.component == b.component &&
              a.structureStride == b.structureStride && a.firstMember == b.firstMember &&
              a.memberCount == b.memberCount);
    }

    for (usize index = 0; index < layout.members.Size(); ++index) {
        CHECK(layout.members[index].name == decoded.members[index].name &&
              layout.members[index].offset == decoded.members[index].offset &&
              layout.members[index].bytes == decoded.members[index].bytes);
    }

    // 다시 쓰면 같은 byte다.
    Array<uint8> again(TestAllocator());
    REQUIRE(EncodeShaderBindingLayout(decoded, again));
    CHECK(again.Size() == bytes.Size() && memcmp(again.Data(), bytes.Data(), bytes.Size()) == 0);
    // 조회.
    const ShaderBindingResource* lights = decoded.Find(ShaderResourceKind::StorageBufferRead, 2);
    REQUIRE(lights != nullptr);
    CHECK(lights->name == StringView("Lights"));
    CHECK(decoded.Find(ShaderResourceKind::StorageBufferRead, 1) == nullptr);
    CHECK(decoded.MembersOf(decoded.resources[0]).Size() == 2);
    CHECK(StringView(ShaderResourceKindName(ShaderResourceKind::StorageImage)) == "StorageImage");

    // 모든 잘린 길이·남는 byte·버전.
    for (usize length = 0; length < bytes.Size(); ++length) {
        const ShaderReadStatus read = DecodeShaderBindingLayout(bytes.View().Left(length), decoded);
        CHECK(read.error == ShaderReadError::CorruptPackage);
    }

    REQUIRE(bytes.TryPushBack(0));
    CHECK(DecodeShaderBindingLayout(bytes.View(), decoded).error ==
          ShaderReadError::CorruptPackage);
    (void)bytes.TryResize(bytes.Size() - 1);
    bytes[0] = 4;
    CHECK(DecodeShaderBindingLayout(bytes.View(), decoded).error ==
          ShaderReadError::UnsupportedVersion);
    bytes[0] = 5;
    // 종류 byte를 범위 밖으로(첫 자원).
    bytes[sizeof(header)] = 9;
    CHECK(DecodeShaderBindingLayout(bytes.View(), decoded).error ==
          ShaderReadError::CorruptPackage);
}

TEST(ShaderCore_BindingLayoutRules) {
    using L = ShaderBindingLayout;
    CHECK(EncodeRejects([](L& l) noexcept { l.stage = ShaderStageVertex | ShaderStagePixel; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.stage = 1u << 5; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.rootConstantBytes = 136; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.rootConstantBytes = 20; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.attributeMask = 1; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.renderTargetMask = 1u << 8; }));
    CHECK(EncodeRejects([](L& l) noexcept {
        l.stage = ShaderStageVertex;
        l.renderTargetMask = 1;
    }));
    CHECK(EncodeRejects(
        [](L& l) noexcept { l.resources[0].kind = static_cast<ShaderResourceKind>(6); }));
    CHECK(EncodeRejects([](L& l) noexcept { l.resources[1].slot = 0; }));
    CHECK(EncodeRejects([](L& l) noexcept {
        const ShaderBindingResource first = l.resources[2];
        l.resources[2] = l.resources[3];
        l.resources[3] = first;
    }));
    CHECK(EncodeRejects([](L& l) noexcept { l.resources[2].name = "9Albedo"; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.resources[0].constantBufferBytes = 0; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.resources[0].constantBufferBytes = 24; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.resources[2].constantBufferBytes = 16; }));
    CHECK(EncodeRejects(
        [](L& l) noexcept { l.resources[4].samplerSource = ShaderSamplerSource::Own; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.resources[2].sharedSampler = true; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.members[1].offset = 8; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.members[1].bytes = 20; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.members[1].bytes = 0; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.members[2].name = ""; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.resources[1].firstMember = 1; }));
    CHECK(EncodeRejects(
        [](L& l) noexcept { (void)l.members.TryPushBack(ShaderBindingMember{"x", 0, 4}); }));
    // v3 모양 규칙: 종류에 맞는 모양, typed만 원소 타입, 구조화만 stride(4의 배수).
    CHECK(EncodeRejects([](L& l) noexcept { l.resources[2].shape = ShaderResourceShape::None; }));
    CHECK(EncodeRejects(
        [](L& l) noexcept { l.resources[2].shape = ShaderResourceShape::RawBuffer; }));
    CHECK(
        EncodeRejects([](L& l) noexcept { l.resources[2].component = ShaderComponentType::None; }));
    CHECK(EncodeRejects(
        [](L& l) noexcept { l.resources[3].component = ShaderComponentType::Float; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.resources[3].structureStride = 0; }));
    CHECK(EncodeRejects([](L& l) noexcept { l.resources[3].structureStride = 6; }));
    CHECK(EncodeRejects(
        [](L& l) noexcept { l.resources[3].shape = ShaderResourceShape::RawBuffer; }));
    CHECK(EncodeRejects(
        [](L& l) noexcept { l.resources[0].shape = ShaderResourceShape::Texture2D; }));
    CHECK(EncodeRejects(
        [](L& l) noexcept { l.resources[2].shape = static_cast<ShaderResourceShape>(13); }));
    CHECK(EncodeRejects([](L& l) noexcept {
        l.stage = ShaderStageVertex;
        l.renderTargetMask = 0;
        l.attributeMask = 0b11;
        l.attributeIntMask = 0b1;
        l.attributeUintMask = 0b1;
    }));
    CHECK(EncodeRejects([](L& l) noexcept {
        l.stage = ShaderStageVertex;
        l.renderTargetMask = 0;
        l.attributeMask = 0b1;
        l.attributeUintMask = 0b10;
    }));
    CHECK(!EncodeRejects([](L& l) noexcept {
        l.stage = ShaderStageVertex;
        l.renderTargetMask = 0;
        l.attributeMask = 0b111;
        l.attributeIntMask = 0b10;
        l.attributeUintMask = 0b100;
    }));
    CHECK(!EncodeRejects([](L& l) noexcept {
        l.resources[3].shape = ShaderResourceShape::RawBuffer;
        l.resources[3].structureStride = 0;
    }));
    CHECK(!EncodeRejects([](L& l) noexcept {
        l.resources[2].shape = ShaderResourceShape::TypedBuffer;
        l.resources[2].component = ShaderComponentType::Uint;
    }));
    // storage image는 cube·cube 배열을 받고(GLES RWTextureCube, ShaderSystem 16.1절 SH2g)
    // multisample은 받지 않는다.
    for (const ShaderResourceShape shape :
         {ShaderResourceShape::TextureCube, ShaderResourceShape::TextureCubeArray,
          ShaderResourceShape::Texture2DMultisample}) {
        const bool rejected = EncodeRejects([shape](L& l) noexcept {
            l.resources[3].kind = ShaderResourceKind::StorageImage;
            l.resources[3].shape = shape;
            l.resources[3].component = ShaderComponentType::Float;
            l.resources[3].structureStride = 0;
        });
        CHECK(rejected == (shape == ShaderResourceShape::Texture2DMultisample));
    }
    {
        ShaderBindingLayout typed(TestAllocator());
        typed.stage = ShaderStageVertex;
        typed.attributeMask = 0b111;
        typed.attributeIntMask = 0b10;
        typed.attributeUintMask = 0b100;
        CHECK(ShaderAttributeComponent(typed, 0) == ShaderComponentType::Float);
        CHECK(ShaderAttributeComponent(typed, 1) == ShaderComponentType::Int);
        CHECK(ShaderAttributeComponent(typed, 2) == ShaderComponentType::Uint);
        CHECK(ShaderAttributeComponent(typed, 3) == ShaderComponentType::None);
        CHECK(ShaderAttributeComponent(typed, 16) == ShaderComponentType::None);
        CHECK(StringView(ShaderResourceShapeName(ShaderResourceShape::TextureCubeArray)) ==
              "TextureCubeArray");
    }

    // 정상 바꿈은 받는다(대조).
    CHECK(!EncodeRejects(
        [](L& l) noexcept { l.resources[2].samplerSource = ShaderSamplerSource::SharedClamp; }));
    CHECK(!EncodeRejects([](L& l) noexcept {
        l.rootConstantBytes = 0;
        l.rootConstantStructId = 0;
    }));
}

TEST(ShaderCore_StageLinkUsesLeadingInterStageOutputs) {
    ShaderBindingLayout vertex(TestAllocator());
    ShaderBindingLayout pixel(TestAllocator());
    vertex.stage = ShaderStageVertex;
    pixel.stage = ShaderStagePixel;
    vertex.interStageMask = 0b111;
    const char* reason = nullptr;
    for (const uint32 mask : {0u, 0b1u, 0b11u, 0b111u}) {
        pixel.interStageMask = mask;
        CHECK(CheckShaderStageLink(vertex, pixel));
    }

    // 앞부분이 아니면(빠진 n, VS에 없는 n) 연결되지 않는다(D3D register가 선언 순서다).
    for (const uint32 mask : {0b101u, 0b10u, 0b1000u, 0b1111u}) {
        pixel.interStageMask = mask;
        CHECK(!CheckShaderStageLink(vertex, pixel, &reason));
    }

    CHECK(strstr(reason, "leading TEXCOORDn") != nullptr);
    CHECK(!CheckShaderStageLink(pixel, vertex, &reason));
    // 값 규칙: CS에는 단계 사이 마스크가 없고 TEXCOORD0–15만이다.
    ShaderBindingLayout compute(TestAllocator());
    compute.stage = ShaderStageCompute;
    compute.interStageMask = 1;
    Array<uint8> bytes(TestAllocator());
    CHECK(!EncodeShaderBindingLayout(compute, bytes));
    vertex.interStageMask = 1u << 16;
    CHECK(!EncodeShaderBindingLayout(vertex, bytes));
    vertex.interStageMask = 0b1011;
    REQUIRE(EncodeShaderBindingLayout(vertex, bytes));
    ShaderBindingLayout decoded(TestAllocator());
    REQUIRE(DecodeShaderBindingLayout(bytes.View(), decoded));
    CHECK(decoded.interStageMask == 0b1011);
}

TEST(ShaderCore_GlesBindingHeaderRoundTripAndRules) {
    ShaderGlesBindingHeader header(TestAllocator());
    FillSampleGles(header);
    Array<uint8> bytes(TestAllocator());
    REQUIRE(EncodeShaderGlesBindingHeader(header, bytes));
    const uint8 start[] = {1, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0, 0, 0, 'h', 3, 0, 0, 0};
    CHECK(memcmp(bytes.Data(), start, sizeof(start)) == 0);
    ShaderGlesBindingHeader decoded(TestAllocator());
    REQUIRE(DecodeShaderGlesBindingHeader(bytes.View(), decoded));
    CHECK(decoded.stage == ShaderStageVertex && decoded.attributeMask == 0b11);
    REQUIRE(decoded.arrays.Size() == 3 && decoded.samplers.Size() == 2 &&
            decoded.copies.Size() == 3);
    CHECK(decoded.arrays[2].buffer == ShaderGlesRootConstantsBuffer &&
          decoded.arrays[1].type == ShaderGlesUniformType::Uint &&
          decoded.arrays[0].vec4Count == 3);
    CHECK(decoded.samplers[1].unit == 17 && decoded.samplers[1].name == StringView("vs1") &&
          decoded.samplers[1].source == ShaderSamplerSource::SharedClamp);
    CHECK(decoded.copies[1].sourceOffset == 48 && decoded.copies[1].arrayIndex == 1 &&
          decoded.copies[2].bytes == 64);
    Array<uint8> again(TestAllocator());
    REQUIRE(EncodeShaderGlesBindingHeader(decoded, again));
    CHECK(again.Size() == bytes.Size() && memcmp(again.Data(), bytes.Data(), bytes.Size()) == 0);
    for (usize length = 0; length < bytes.Size(); ++length) {
        CHECK(DecodeShaderGlesBindingHeader(bytes.View().Left(length), decoded).error ==
              ShaderReadError::CorruptPackage);
    }

    bytes[0] = 7;
    CHECK(DecodeShaderGlesBindingHeader(bytes.View(), decoded).error ==
          ShaderReadError::UnsupportedVersion);
    bytes[0] = 1;
    bytes[18] = 'm';
    CHECK(DecodeShaderGlesBindingHeader(bytes.View(), decoded).error ==
          ShaderReadError::CorruptPackage);

    // 배열 이름 규칙(UE와 같다).
    char name[ShaderGlesArrayNameBytes];
    CHECK(ShaderGlesUniformArrayName(ShaderStagePixel, {0, ShaderGlesUniformType::Float, 1}, name));
    CHECK(StringView(name) == "pc0_h");
    CHECK(
        ShaderGlesUniformArrayName(ShaderStageCompute, {12, ShaderGlesUniformType::Uint, 1}, name));
    CHECK(StringView(name) == "cc12_u");
    CHECK(ShaderGlesUniformArrayName(
        ShaderStageVertex, {ShaderGlesRootConstantsBuffer, ShaderGlesUniformType::Int, 1}, name));
    CHECK(StringView(name) == "vu_i");
    CHECK(!ShaderGlesUniformArrayName(ShaderStageVertex | ShaderStagePixel,
                                      {0, ShaderGlesUniformType::Float, 1}, name));

    using H = ShaderGlesBindingHeader;
    CHECK(GlesEncodeRejects([](H& h) noexcept { h.stage = ShaderStagePixel; }));
    CHECK(GlesEncodeRejects([](H& h) noexcept { h.arrays[0].vec4Count = 0; }));
    CHECK(GlesEncodeRejects(
        [](H& h) noexcept { h.arrays[0].type = static_cast<ShaderGlesUniformType>('x'); }));
    CHECK(
        GlesEncodeRejects([](H& h) noexcept { h.arrays[1].type = ShaderGlesUniformType::Float; }));
    CHECK(GlesEncodeRejects([](H& h) noexcept { h.samplers[1].unit = 16; }));
    CHECK(GlesEncodeRejects([](H& h) noexcept { h.samplers[1].unit = 32; }));
    CHECK(GlesEncodeRejects([](H& h) noexcept { h.samplers[0].name = "0s"; }));
    CHECK(GlesEncodeRejects([](H& h) noexcept { h.copies[0].bytes = 6; }));
    CHECK(GlesEncodeRejects([](H& h) noexcept { h.copies[0].bytes = 0; }));
    CHECK(GlesEncodeRejects([](H& h) noexcept { h.copies[0].bytes = 52; }));
    CHECK(GlesEncodeRejects([](H& h) noexcept { h.copies[1].sourceOffset = 44; }));
    CHECK(GlesEncodeRejects([](H& h) noexcept { h.copies[0].arrayIndex = 3; }));
    CHECK(GlesEncodeRejects([](H& h) noexcept { h.copies[0].arrayIndex = 2; }));
    CHECK(GlesEncodeRejects([](H& h) noexcept { h.copies[2].vec4Offset = 1; }));
    CHECK(GlesEncodeRejects([](H& h) noexcept {
        (void)h.copies.TryPushBack(ShaderGlesCopy{ShaderGlesRootConstantsBuffer, 64, 16, 2, 3});
    }));
    // 원본은 겹치지 않지만 대상이 겹친다.
    CHECK(GlesEncodeRejects([](H& h) noexcept { h.copies[1] = ShaderGlesCopy{0, 48, 16, 0, 2}; }));
    CHECK(!GlesEncodeRejects([](H& h) noexcept { h.attributeMask = 0; }));
}

TEST(ShaderCore_LibraryRoundTrip) {
    SampleEntries sample;
    Array<uint8> bytes(TestAllocator());
    REQUIRE(BuildShaderLibrary(sample.Desc(), TestAllocator(), bytes));
    ShaderLibraryView view;
    REQUIRE(view.Open(bytes.View()));
    CHECK(view.Kind() == ShaderLibraryKind::Project);
    CHECK(view.EntryCount() == 4 && view.CodeCount() == 2 && view.NameCount() == 2);
    const ShaderFormatFacts facts = SampleFacts();
    CHECK(view.Facts().FormatId() == StringView("VULKAN_SM5"));
    CHECK(view.Facts().backend == ShaderBackend::Vulkan &&
          view.Facts().shaderModel == facts.shaderModel);
    CHECK(view.Facts().features == facts.features &&
          view.Facts().deviceRequirements == facts.deviceRequirements &&
          view.Facts().platformClasses == facts.platformClasses);
    CHECK(memcmp(&view.Facts().limits, &facts.limits, sizeof(facts.limits)) == 0);
    CHECK(view.FormatDefinitionSha256() == Digest(0x11) && view.TypeSetSha256() == Digest(0x22));
    // 파일 SHA는 자기 칸을 0으로 둔 byte의 SHA다.
    Array<uint8> zeroed(TestAllocator());
    REQUIRE(zeroed.TryAppend(bytes.View()));
    memset(zeroed.Data() + FileShaAt, 0, Sha256Digest::Bytes);
    CHECK(view.FileSha256() == Sha256::Hash(zeroed.View()));
    // 머리 golden 몇 칸.
    CHECK(memcmp(bytes.Data(), "RSHL", 4) == 0 && LoadLittleEndian<uint32>(bytes.Data() + 4) == 2);
    // v2(16.5절 SSD90): offset 115는 플랫폼 분류 비트다.
    CHECK(bytes[115] == (ShaderPlatformClassPc | ShaderPlatformClassMobile));
    CHECK(bytes[8] == 1 &&
          LoadLittleEndian<uint64>(bytes.Data() + 192) == ShaderLibraryHeaderBytes);
    CHECK(bytes.Size() % ShaderLibraryAlignment == 0);

    // 항목은 키 순이다(묶음 0의 Vertex·Pixel 해시 순, 묶음 9는 끝).
    for (uint32 index = 1; index < view.EntryCount(); ++index) {
        CHECK(ShaderLibraryKeyLess(view.Entry(index - 1).key, view.Entry(index).key));
    }

    CHECK(view.Entry(3).key.bundleId == 9);
    for (const ShaderLibraryBuildEntry& input : sample.entries) {
        ShaderLibraryEntry entry;
        REQUIRE(view.Find(input.key, entry));
        CHECK(entry.stage == input.stage && entry.developmentOnly == input.developmentOnly &&
              entry.runtimeFeatures == input.runtimeFeatures &&
              entry.layoutSignature == input.layoutSignature);
        const Span<const uint8> code = view.Value(entry, ShaderLibraryValueCode);
        CHECK(code.Size() == input.values[0].bytes.Size() &&
              memcmp(code.Data(), input.values[0].bytes.Data(), code.Size()) == 0);
        const Span<const uint8> layout = view.Value(entry, ShaderLibraryValueBindingLayout);
        CHECK(layout.Size() == (input.values.Size() > 1 ? sizeof(LayoutBytes) : 0));
        CHECK(view.Value(entry, ShaderLibraryValueGlesCopyTable).IsEmpty());
        CHECK(view.TypeName(input.key.nameHash) == input.typeName);
    }

    // 같은 코드는 한 번이다.
    ShaderLibraryEntry vertex;
    ShaderLibraryEntry pixel;
    REQUIRE(view.Find(sample.entries[0].key, vertex));
    REQUIRE(view.Find(sample.entries[2].key, pixel));
    CHECK(vertex.codeIndex == pixel.codeIndex);
    ShaderLibraryEntry missing;
    CHECK(!view.Find(ShaderLibraryKey{0, LibraryVertexName.hash, NoVertexInputType, 1}, missing));
    CHECK(!view.Find(ShaderLibraryKey{10, 0, 0, 0}, missing));
    CHECK(view.TypeName(12345).IsEmpty());

    // 입력 순서와 무관하게 같은 byte다.
    ShaderLibraryBuildEntry reversed[4] = {sample.entries[3], sample.entries[2], sample.entries[1],
                                           sample.entries[0]};
    ShaderLibraryBuildDesc desc = sample.Desc();
    desc.entries = reversed;
    Array<uint8> second(TestAllocator());
    REQUIRE(BuildShaderLibrary(desc, TestAllocator(), second));
    CHECK(second.Size() == bytes.Size() && memcmp(second.Data(), bytes.Data(), bytes.Size()) == 0);

    // 빈 라이브러리도 열린다.
    desc.entries = {};
    REQUIRE(BuildShaderLibrary(desc, TestAllocator(), second));
    CHECK(second.Size() == ShaderLibraryHeaderBytes);
    REQUIRE(view.Open(second.View()));
    CHECK(view.EntryCount() == 0 && !view.Find(sample.entries[0].key, missing));
    view.Close();
    CHECK(!view.IsOpen());
    CHECK(StringView(ShaderLibraryKindName(ShaderLibraryKind::Engine)) == "Engine");
    CHECK(ShaderLibraryKindOf(EngineShaderBundleId, ShaderTypeOwner::ProjectPlugin) ==
              ShaderLibraryKind::Project &&
          ShaderLibraryKindOf(EngineShaderBundleId, ShaderTypeOwner::EnginePlugin) ==
              ShaderLibraryKind::Engine);
    // 머티리얼 항목은 소속이 Engine이어도 묶음 id로 Project에 간다(16.5절 SSD85).
    CHECK(ShaderLibraryKindOf(9, ShaderTypeOwner::Engine) == ShaderLibraryKind::Project);
}

TEST(ShaderCore_MaterialShaderBundleIdIsLittleEndianPrefix) {
    // 묶음 id = 셰이더 집합 id 앞 8 byte(little-endian), 0이면 1(16.5절 SSD85).
    Sha256Digest set = Digest(0xee);
    for (usize index = 0; index < sizeof(uint64); ++index) {
        set.bytes[index] = static_cast<uint8>(index + 1);
    }

    CHECK(MaterialShaderBundleId(set) == 0x0807060504030201ull);
    CHECK(MaterialShaderBundleId(Sha256Digest{}) == EngineShaderBundleId + 1);
    Sha256Digest zeroPrefix = Digest(0xab);
    memset(zeroPrefix.bytes, 0, sizeof(uint64));
    CHECK(MaterialShaderBundleId(zeroPrefix) == 1u);
    // 앞 8 byte 밖은 묶음 id에 닿지 않는다(충돌은 쿠커가 진단한다 — SSD88).
    Sha256Digest tail = set;
    tail.bytes[sizeof(uint64)] ^= 0xff;
    CHECK(MaterialShaderBundleId(tail) == MaterialShaderBundleId(set));
}

TEST(ShaderCore_LibraryBuildRejectsInvalidInput) {
    using E = SampleEntries;
    using D = ShaderLibraryBuildDesc;
    CHECK(BuildRejects([](E& e, D&) noexcept { e.entries[1].key = e.entries[0].key; }));
    CHECK(BuildRejects([](E& e, D&) noexcept { e.entries[0].key.nameHash += 1; }));
    CHECK(BuildRejects([](E& e, D&) noexcept { e.entries[0].typeName = "Library Vertex"; }));
    // 대소문자만 다른 이름은 같은 해시다(CP7 — 한 해시 한 이름).
    CHECK(BuildRejects([](E& e, D&) noexcept { e.entries[1].typeName = "LIBRARYVERTEX"; }));
    CHECK(BuildRejects([](E& e, D&) noexcept { e.entries[0].stage = 0; }));
    CHECK(BuildRejects(
        [](E& e, D&) noexcept { e.entries[0].stage = ShaderStageVertex | ShaderStagePixel; }));
    CHECK(BuildRejects(
        [](E& e, D&) noexcept { e.entries[0].runtimeFeatures = ShaderFeatureCompute; }));
    CHECK(BuildRejects([](E& e, D&) noexcept { e.entries[0].values = {}; }));
    static constexpr ShaderLibraryValue noCode[] = {{ShaderLibraryValueBindingLayout, LayoutBytes}};
    CHECK(BuildRejects([](E& e, D&) noexcept { e.entries[0].values = noCode; }));
    static constexpr ShaderLibraryValue unordered[] = {{ShaderLibraryValueCode, CodeA},
                                                       {ShaderLibraryValueGlesCopyTable, CodeB},
                                                       {ShaderLibraryValueBindingLayout, CodeB}};
    CHECK(BuildRejects([](E& e, D&) noexcept { e.entries[0].values = unordered; }));
    static constexpr ShaderLibraryValue debug[] = {{ShaderLibraryValueCode, CodeA}, {3, CodeB}};
    CHECK(BuildRejects([](E& e, D&) noexcept { e.entries[0].values = debug; }));
    CHECK(BuildRejects([](E&, D& d) noexcept { d.kind = static_cast<ShaderLibraryKind>(2); }));
    CHECK(BuildRejects([](E&, D& d) noexcept { d.facts.backend = static_cast<ShaderBackend>(5); }));
    CHECK(BuildRejects([](E&, D& d) noexcept { d.facts.formatId[0] = '\0'; }));
    CHECK(BuildRejects([](E&, D& d) noexcept { d.facts.formatId[0] = '-'; }));
    CHECK(BuildRejects([](E&, D& d) noexcept { d.facts.platformClasses = 1u << 7; }));
    // Engine 라이브러리는 묶음 id 0만 담는다(표본의 넷째 항목은 묶음 9 — 16.5절 SSD85).
    CHECK(BuildRejects([](E&, D& d) noexcept { d.kind = ShaderLibraryKind::Engine; }));
    CHECK(!BuildRejects([](E& e, D& d) noexcept {
        d.kind = ShaderLibraryKind::Engine;
        e.entries[3].key.bundleId = EngineShaderBundleId;
        e.entries[3].key.permutationId = 5;
    }));
    // 대조: 정상 입력은 받는다.
    CHECK(!BuildRejects([](E&, D&) noexcept {}));
}

TEST(ShaderCore_LibraryOpenRejectsCorruption) {
    Array<uint8> bytes(TestAllocator());
    REQUIRE(BuildSample(bytes));
    ShaderLibraryView view;
    // 모든 byte 하나 뒤집기: SHA가 잡는다(버전 칸은 UnsupportedVersion, 나머지는 CorruptPackage).
    for (usize index = 0; index < bytes.Size(); ++index) {
        bytes[index] ^= 0x40;
        const ShaderReadStatus opened = view.Open(bytes.View());
        const bool versionField = index >= 4 && index < 8;
        CHECK(opened.error == (versionField ? ShaderReadError::UnsupportedVersion
                                            : ShaderReadError::CorruptPackage));
        CHECK(!view.IsOpen());
        bytes[index] ^= 0x40;
    }

    // 모든 잘린 길이.
    for (usize length = 0; length < bytes.Size(); length += 7) {
        CHECK(view.Open(bytes.View().Left(length)).error == ShaderReadError::CorruptPackage);
    }

    CHECK(view.Open(bytes.View()));

    // 다시 봉인한 표적 손상(SHA 뒤 규칙).
    const usize nameTable = static_cast<usize>(LoadLittleEndian<uint64>(bytes.Data() + 208));
    const usize codeTable = static_cast<usize>(LoadLittleEndian<uint64>(bytes.Data() + 200));
    // CP7: 이름 byte를 바꾸면 다시 계산한 해시가 저장된 해시와 다르다.
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[nameTable + 16] ^= 0x01; },
                       "hash does not match"));
    // 대소문자만 바꾼 이름은 같은 해시지만 항목과 정렬은 그대로다 — 해시가 같으므로 열린다(이름이
    // 정체성인 비교는 런타임 등록부와의 대조가 한다).
    {
        Array<uint8> renamed(TestAllocator());
        REQUIRE(BuildSample(renamed));
        renamed[nameTable + 16] = static_cast<uint8>(renamed[nameTable + 16] ^ 0x20);
        Reseal(renamed);
        CHECK(view.Open(renamed.View()));
    }

    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[nameTable + 16] = '9'; },
                       "type name breaks"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[nameTable + 8] = 0; }, "name row"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[nameTable + 8] = 65; }, "name row"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[nameTable + 9] = 1; }, "name row"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[nameTable + 79] = 1; }, "name row"));
    // 이름 표 순서(두 행 바꿈).
    CHECK(CorruptAfter(
        [&](Array<uint8>& b) noexcept {
            uint8 row[ShaderLibraryNameStride];
            memcpy(row, b.Data() + nameTable, sizeof(row));
            memmove(b.Data() + nameTable, b.Data() + nameTable + ShaderLibraryNameStride,
                    sizeof(row));
            memcpy(b.Data() + nameTable + ShaderLibraryNameStride, row, sizeof(row));
        },
        "strict hash order"));
    // 항목: 순서, 단계, 개발 표시, 빈 byte, 기능, 코드 색인, 이름 표에 없는 해시.
    CHECK(CorruptAfter(
        [&](Array<uint8>& b) noexcept {
            uint8 row[ShaderLibraryEntryStride];
            memcpy(row, b.Data() + FirstEntryAt, sizeof(row));
            memmove(b.Data() + FirstEntryAt, b.Data() + FirstEntryAt + ShaderLibraryEntryStride,
                    sizeof(row));
            memcpy(b.Data() + FirstEntryAt + ShaderLibraryEntryStride, row, sizeof(row));
        },
        "entry"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[FirstEntryAt + 28] = 3; }, "entry"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[FirstEntryAt + 29] = 2; }, "entry"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[FirstEntryAt + 30] = 1; }, "entry"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[FirstEntryAt + 32] = 1; }, "entry"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[FirstEntryAt + 36] = 2; }, "entry"));
    CHECK(CorruptAfter(
        [&](Array<uint8>& b) noexcept {
            // 마지막 항목(묶음 9)의 해시를 이름 표에 없는 값으로(키 순서는 그대로).
            const usize last = FirstEntryAt + 3 * ShaderLibraryEntryStride;
            StoreLittleEndian<uint64>(b.Data() + last + 8, 1);
        },
        "missing from the name table"));
    // 코드: SHA·순서·오프셋·틀·빈 byte.
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[codeTable] ^= 0x01; }, "code"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[codeTable + 32] = 8; }, "code range"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[codeTable + 40] += 8; }, "code"));
    CHECK(CorruptAfter(
        [&](Array<uint8>& b) noexcept {
            const usize blob = static_cast<usize>(LoadLittleEndian<uint64>(b.Data() + 216));
            b[blob] = 9;
        },
        "value framing"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[b.Size() - 1] = 1; }, "code"));
    // 머리: 종류·빈 byte·backend·형식 id·개수·오프셋.
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[8] = 2; }, "header"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[9] = 1; }, "header"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[112] = 5; }, "header"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[115] = 0x80; }, "platform classes"));
    // Engine 라이브러리의 머티리얼 묶음 항목(표본은 Project — 종류만 바꾼다).
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[8] = 0; }, "material bundle id"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[188] = 1; }, "header"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[16] = '-'; }, "format id"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[16 + 20] = 'X'; }, "format id"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { memset(b.Data() + 16, 'A', 32); },
                       "NUL terminated"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[176] = 5; }, "offsets"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[184] = 0; }, "counts"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[192] = 0; }, "offsets"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { b[224] += 8; }, "offsets"));
    CHECK(CorruptAfter([&](Array<uint8>& b) noexcept { (void)b.TryResize(b.Size() + 8); },
                       "offsets"));
    // 버전은 SHA보다 먼저 본다(v1은 더 읽지 않는다 — 16.5절 SSD90).
    bytes[4] = 1;
    CHECK(view.Open(bytes.View()).error == ShaderReadError::UnsupportedVersion);
}

} // namespace shader_core_format_tests_file

namespace shader_core_format_tests_file {

namespace {

constexpr ShaderParameterMember InnerMembers[] = {{"color", ShaderParameterType::Float4, 0, 0},
                                                  {"scale", ShaderParameterType::Float, 16, 0}};
constexpr ShaderParameterLayout InnerLayout{"Inner", false, 32, InnerMembers};

// 표 하나만 바꿔 검사한다.
bool Accepts(const ShaderParameterLayout& layout) noexcept {
    return static_cast<bool>(ValidateShaderParameterLayout(layout));
}

} // namespace

// 중첩 구조체·identity·생성 원문 칸의 규칙이다(ShaderSystem 16.2절 SSD53 — 데이터 경로 표의 음성).
TEST(ShaderCore_ParameterLayoutNestedAndIdentityRules) {
    const ShaderParameterMember outer[] = {
        {"tint", ShaderParameterType::Float4, 0, 0},
        {"inner", ShaderParameterType::Struct, 16, 0, &InnerLayout}};
    const ShaderParameterLayout good("Outer", false, 48, outer, "rocks::test::Outer",
                                     "/Engine/Generated/Test/Outer.rsh", "cbuffer Outer {};");
    CHECK(Accepts(good));
    CHECK(good.structId == ShaderParameterStructId("rocks::test::Outer"));
    CHECK(ShaderParameterMemberBytes(outer[1]) == 32);
    CHECK(ShaderParameterStructId("") == 0);

    // 중첩 표 없음, 배열, 16 byte 경계가 아님, 크기를 넘음.
    const ShaderParameterMember missing[] = {{"inner", ShaderParameterType::Struct, 0, 0}};
    CHECK(!Accepts(ShaderParameterLayout("Outer", false, 32, missing)));
    const ShaderParameterMember array[] = {
        {"inner", ShaderParameterType::Struct, 0, 2, &InnerLayout}};
    CHECK(!Accepts(ShaderParameterLayout("Outer", false, 64, array)));
    const ShaderParameterMember misaligned[] = {
        {"a", ShaderParameterType::Float, 0, 0},
        {"inner", ShaderParameterType::Struct, 8, 0, &InnerLayout}};
    CHECK(!Accepts(ShaderParameterLayout("Outer", false, 48, misaligned)));
    CHECK(!Accepts(
        ShaderParameterLayout("Outer", false, 32, Span<const ShaderParameterMember>(outer, 2))));
    // 2단 중첩.
    const ShaderParameterLayout middle("Middle", false, 48, outer);
    const ShaderParameterMember twice[] = {{"middle", ShaderParameterType::Struct, 0, 0, &middle}};
    CHECK(!Accepts(ShaderParameterLayout("Outer", false, 48, twice)));
    // 루트 상수 중첩 표.
    const ShaderParameterLayout rootInner("Inner", true, 32, InnerMembers);
    const ShaderParameterMember rootNested[] = {
        {"inner", ShaderParameterType::Struct, 0, 0, &rootInner}};
    CHECK(!Accepts(ShaderParameterLayout("Outer", false, 32, rootNested)));
    // 스칼라에 중첩 표를 달면 거부한다.
    const ShaderParameterMember scalarWithLayout[] = {
        {"a", ShaderParameterType::Float, 0, 0, &InnerLayout}};
    CHECK(!Accepts(ShaderParameterLayout("Outer", false, 16, scalarWithLayout)));

    // identity: 이름 해시와 달라야 거부, 정규 이름이 아니면 거부.
    ShaderParameterLayout wrongId = good;
    wrongId.structId = 1;
    CHECK(!Accepts(wrongId));
    for (const char* bad : {"rocks:test", "rocks::", "::rocks", "rocks::1x", "a b"}) {
        ShaderParameterLayout named = good;
        named.structName = StringView(bad);
        named.structId = ShaderParameterStructId(named.structName);
        CHECK(!Accepts(named));
    }

    // 원문: 경로만·원문만·Generated 밖·확장자.
    ShaderParameterLayout pathOnly = good;
    pathOnly.source = StringView();
    CHECK(!Accepts(pathOnly));
    ShaderParameterLayout textOnly = good;
    textOnly.sourcePath = StringView();
    CHECK(!Accepts(textOnly));
    ShaderParameterLayout outside = good;
    outside.sourcePath = "/Engine/Private/Outer.rsh";
    CHECK(!Accepts(outside));
    ShaderParameterLayout extension = good;
    extension.sourcePath = "/Engine/Generated/Test/Outer.rsf";
    CHECK(!Accepts(extension));

    // 같은 원문 경로를 두 표가 쓰면 목록 거부.
    ShaderParameterLayout other = good;
    other.name = "Other";
    const ShaderParameterLayout duplicatePath[] = {good, other};
    CHECK(!ValidateShaderParameterLayouts(duplicatePath));

    // 멤버 상한(257).
    static ShaderParameterMember many[MaxShaderParameterMembers + 1];
    static char names[MaxShaderParameterMembers + 1][8];
    for (usize index = 0; index <= MaxShaderParameterMembers; ++index) {
        (void)snprintf(names[index], sizeof(names[index]), "m%zu", index);
        many[index] = ShaderParameterMember(StringView(names[index]), ShaderParameterType::Float4,
                                            static_cast<uint32>(index * 16), 0);
    }

    CHECK(Accepts(
        ShaderParameterLayout("Many", false, MaxShaderParameterMembers * 16,
                              Span<const ShaderParameterMember>(many, MaxShaderParameterMembers))));
    CHECK(!Accepts(ShaderParameterLayout(
        "Many", false, (MaxShaderParameterMembers + 1) * 16,
        Span<const ShaderParameterMember>(many, MaxShaderParameterMembers + 1))));
    // 루트 상수 128 byte 초과.
    CHECK(!Accepts(
        ShaderParameterLayout("Root", true, 144, Span<const ShaderParameterMember>(many, 9))));

    // 정규 byte 왕복: 중첩 표와 구조체 이름이 돌아오고 서명이 같다.
    const ShaderParameterLayout list[] = {good};
    Array<uint8> bytes(TestAllocator());
    REQUIRE(EncodeShaderParameterLayouts(list, bytes));
    CHECK(Sha256::Hash(bytes.View()) == ShaderParameterLayoutSignature(list));
    ShaderParameterLayoutSet decoded(TestAllocator());
    REQUIRE(DecodeShaderParameterLayouts(bytes.View(), decoded));
    REQUIRE(decoded.View().Size() == 1);
    CHECK(decoded.View()[0].structId == good.structId);
    REQUIRE(decoded.View()[0].members[1].structLayout != nullptr);
    CHECK(decoded.View()[0].members[1].structLayout->members[1].name == StringView("scale"));
    CHECK(ShaderParameterLayoutSignature(decoded.View()) == ShaderParameterLayoutSignature(list));
    for (usize length = 1; length < bytes.Size(); ++length) {
        CHECK(!DecodeShaderParameterLayouts(bytes.View().Left(length), decoded));
    }
}

// 값 1 v4 identity 규칙이다(SX8 — 상수 버퍼만, 루트 상수가 있을 때만).
TEST(ShaderCore_BindingLayoutIdentityRules) {
    ShaderBindingLayout layout(TestAllocator());
    FillSampleLayout(layout);
    Array<uint8> bytes(TestAllocator());
    CHECK(EncodeShaderBindingLayout(layout, bytes));
    layout.rootConstantBytes = 0;
    CHECK(!EncodeShaderBindingLayout(layout, bytes));
    layout.rootConstantBytes = 64;
    ShaderBindingResource* texture = nullptr;
    ShaderBindingResource* constants = nullptr;
    for (ShaderBindingResource& resource : layout.resources) {
        texture = texture == nullptr && resource.kind == ShaderResourceKind::SampledTexture
                      ? &resource
                      : texture;
        constants = constants == nullptr && resource.kind == ShaderResourceKind::ConstantBuffer
                        ? &resource
                        : constants;
    }

    REQUIRE(texture != nullptr && constants != nullptr);
    // v5: 자원의 구조체 identity와 멤버 오프셋은 함께 있다(ShaderSystem 16.2절 SSD63).
    texture->structId = 5;
    CHECK(!EncodeShaderBindingLayout(layout, bytes));
    texture->memberOffset = 48;
    Array<uint8> member(TestAllocator());
    REQUIRE(EncodeShaderBindingLayout(layout, member));
    ShaderBindingLayout decoded(TestAllocator());
    REQUIRE(DecodeShaderBindingLayout(member.View(), decoded));
    bool found = false;
    for (const ShaderBindingResource& resource : decoded.resources) {
        found = found || (resource.kind == ShaderResourceKind::SampledTexture &&
                          resource.structId == 5 && resource.memberOffset == 48);
    }

    CHECK(found);
    texture->structId = 0;
    CHECK(!EncodeShaderBindingLayout(layout, bytes));
    texture->memberOffset = NoShaderParameterMember;
    // 상수 버퍼는 멤버 오프셋을 갖지 않는다.
    constants->memberOffset = 0;
    CHECK(!EncodeShaderBindingLayout(layout, bytes));
}

// 자원 멤버 표 규칙이다(ShaderSystem 16.2절 SSD62): 양성(상수 + 자원, 자원만), 규칙마다 음성,
// 정규 byte 왕복·모든 잘린 길이·서명, 표 사이 이름 겹침.
TEST(ShaderCore_ParameterLayoutResourceRules) {
    using Kind = ShaderParameterResourceKind;
    using Shape = ShaderResourceShape;
    using Component = ShaderComponentType;
    using Pair = ShaderParameterSampler;
    constexpr usize ResourceCount = 6;
    const ShaderParameterMember constants[] = {{"tint", ShaderParameterType::Float4, 0, 0}};
    const ShaderParameterResource resources[ResourceCount] = {
        {"albedo", "albedoSampler", Kind::DeviceTexture, 16, Shape::Texture2D, Component::Float, 0,
         Pair::Filter},
        {"codes", "", Kind::GraphTexture, 32, Shape::Texture2D, Component::Uint, 0, Pair::None},
        {"lights", "", Kind::DeviceBuffer, 48, Shape::StructuredBuffer, Component::None, 16,
         Pair::None},
        {"target", "", Kind::GraphTextureUav, 64, Shape::Texture2D, Component::Float, 0,
         Pair::None},
        {"history", "", Kind::GraphTextureAccess, 80, Shape::None, Component::None, 0, Pair::None},
        {"targets", "", Kind::RenderTargets, 96, Shape::None, Component::None, 0, Pair::None}};
    constexpr StringView StructName("rocks::test::Pass");
    constexpr StringView SourcePath("/Engine/Generated/Test/Pass.rsh");
    constexpr StringView Source("cbuffer Pass { float4 tint; };");
    const ShaderParameterLayout good("Pass", false, 16, constants, resources, StructName,
                                     SourcePath, Source);
    CHECK(Accepts(good));
    CHECK(StringView(ShaderParameterResourceKindName(Kind::GraphBufferUav)) == "GraphBufferUav");
    CHECK(StringView(ShaderParameterResourceKindName(static_cast<Kind>(10))) == "unknown");
    CHECK(IsShaderBoundResource(Kind::GraphBufferUav) &&
          !IsShaderBoundResource(Kind::GraphTextureAccess));
    CHECK(IsGraphResource(Kind::RenderTargets) && !IsGraphResource(Kind::DeviceBuffer));

    // 자원만 가진 구조체(상수 부분 0).
    const ShaderParameterLayout only("Only", false, 0, {},
                                     Span<const ShaderParameterResource>(resources, 2),
                                     "rocks::test::Only", "/Engine/Generated/Test/Only.rsh", "x");
    CHECK(Accepts(only));
    // 상수 멤버가 없는데 크기가 있으면 거부한다.
    CHECK(!Accepts(ShaderParameterLayout(
        "Only", false, 16, {}, Span<const ShaderParameterResource>(resources, 2),
        "rocks::test::Only", "/Engine/Generated/Test/Only.rsh", "x")));

    // 자원 하나를 바꾼 표를 받는가.
    const auto accepts = [&](usize index, auto change) noexcept {
        ShaderParameterResource copy[ResourceCount];
        for (usize at = 0; at < ResourceCount; ++at) {
            copy[at] = resources[at];
        }

        change(copy[index]);
        return Accepts(ShaderParameterLayout("Pass", false, 16, constants, copy, StructName,
                                             SourcePath, Source));
    };
    using Resource = ShaderParameterResource;
    // 이름: 식별자 아님, 상수 멤버·다른 자원·짝 이름·예약 이름과 겹침.
    CHECK(!accepts(1, [](Resource& value) noexcept { value.name = "1codes"; }));
    CHECK(!accepts(1, [](Resource& value) noexcept { value.name = "tint"; }));
    CHECK(!accepts(1, [](Resource& value) noexcept { value.name = "albedo"; }));
    CHECK(!accepts(1, [](Resource& value) noexcept { value.name = "albedoSampler"; }));
    CHECK(!accepts(1, [](Resource& value) noexcept { value.name = "SharedWrap"; }));
    CHECK(!accepts(2, [](Resource& value) noexcept { value.name = "RocksDebugBuffer"; }));
    // 오프셋: 상수 부분 안, 16 byte 배수 아님, 오름차순 아님.
    CHECK(!accepts(0, [](Resource& value) noexcept { value.offset = 0; }));
    CHECK(!accepts(1, [](Resource& value) noexcept { value.offset = 40; }));
    CHECK(!accepts(1, [](Resource& value) noexcept { value.offset = 16; }));
    // 짝: 이름 규칙, float 텍스처의 짝 없음, 정수·MS 텍스처의 짝, 버퍼의 짝.
    CHECK(!accepts(0, [](Resource& value) noexcept { value.samplerName = "albedoSamp"; }));
    CHECK(!accepts(0, [](Resource& value) noexcept {
        value.samplerName = "";
        value.sampler = Pair::None;
    }));
    CHECK(accepts(0, [](Resource& value) noexcept { value.sampler = Pair::Comparison; }));
    CHECK(!accepts(1, [](Resource& value) noexcept {
        value.samplerName = "codesSampler";
        value.sampler = Pair::Filter;
    }));
    CHECK(!accepts(0, [](Resource& value) noexcept { value.shape = Shape::Texture2DMultisample; }));
    CHECK(accepts(0, [](Resource& value) noexcept {
        value.shape = Shape::Texture2DMultisample;
        value.samplerName = "";
        value.sampler = Pair::None;
    }));
    CHECK(!accepts(2, [](Resource& value) noexcept {
        value.samplerName = "lightsSampler";
        value.sampler = Pair::Filter;
    }));
    // 모양·원소·stride가 종류에 맞지 않음.
    CHECK(!accepts(0, [](Resource& value) noexcept { value.shape = Shape::TypedBuffer; }));
    CHECK(!accepts(0, [](Resource& value) noexcept { value.component = Component::None; }));
    CHECK(!accepts(2, [](Resource& value) noexcept { value.component = Component::Float; }));
    CHECK(!accepts(2, [](Resource& value) noexcept { value.structureStride = 0; }));
    CHECK(!accepts(2, [](Resource& value) noexcept { value.structureStride = 6; }));
    CHECK(accepts(2, [](Resource& value) noexcept {
        value.shape = Shape::TypedBuffer;
        value.component = Component::Float;
        value.structureStride = 0;
    }));
    CHECK(accepts(2, [](Resource& value) noexcept {
        value.shape = Shape::RawBuffer;
        value.structureStride = 0;
    }));
    CHECK(!accepts(3, [](Resource& value) noexcept { value.shape = Shape::TextureCube; }));
    CHECK(!accepts(4, [](Resource& value) noexcept { value.shape = Shape::Texture2D; }));
    CHECK(!accepts(5, [](Resource& value) noexcept { value.kind = Kind::GraphTexture; }));
    CHECK(!accepts(4, [](Resource& value) noexcept { value.kind = Kind::RenderTargets; }));
    CHECK(!accepts(4, [](Resource& value) noexcept { value.kind = static_cast<Kind>(10); }));
    CHECK(!accepts(4, [](Resource& value) noexcept { value.sampler = static_cast<Pair>(3); }));
    // 루트 상수·손으로 쓴 표(identity 없음)는 자원을 갖지 않는다.
    CHECK(!Accepts(ShaderParameterLayout("Pass", true, 16, constants, resources, StructName,
                                         SourcePath, Source)));
    CHECK(!Accepts(ShaderParameterLayout("Pass", false, 16, constants, resources, "", "", "")));
    // 중첩 표의 자원.
    const ShaderParameterMember nested[] = {{"inner", ShaderParameterType::Struct, 0, 0, &good}};
    CHECK(!Accepts(ShaderParameterLayout("Outer", false, 16, nested)));
    // 상한 256.
    constexpr usize Many = MaxShaderParameterResources + 1;
    constexpr usize NameBytes = 8;
    static char names[Many][NameBytes];
    static ShaderParameterResource many[Many];
    for (usize index = 0; index < Many; ++index) {
        (void)snprintf(names[index], NameBytes, "r%zu", index);
        many[index] = {StringView(names[index]),
                       "",
                       Kind::GraphTextureAccess,
                       static_cast<uint32>(16 * (index + 1)),
                       Shape::None,
                       Component::None,
                       0,
                       Pair::None};
    }

    CHECK(Accepts(ShaderParameterLayout("Pass", false, 16, constants,
                                        Span<const ShaderParameterResource>(many, Many - 1),
                                        StructName, SourcePath, Source)));
    CHECK(!Accepts(
        ShaderParameterLayout("Pass", false, 16, constants, many, StructName, SourcePath, Source)));

    // 표 사이: 자원·짝 이름이 겹치면 거부한다.
    const ShaderParameterResource other[] = {
        {"albedo", "", Kind::DeviceBuffer, 0, Shape::RawBuffer, Component::None, 0, Pair::None}};
    const ShaderParameterLayout otherLayout("Other", false, 0, {}, other, "rocks::test::Other",
                                            "/Engine/Generated/Test/Other.rsh", "y");
    CHECK(Accepts(otherLayout));
    const ShaderParameterLayout clash[] = {good, otherLayout};
    CHECK(!ValidateShaderParameterLayouts(clash));
    const ShaderParameterResource pairClash[] = {{"albedoSampler", "", Kind::DeviceBuffer, 0,
                                                  Shape::RawBuffer, Component::None, 0,
                                                  Pair::None}};
    const ShaderParameterLayout pairLayout("Other", false, 0, {}, pairClash, "rocks::test::Other",
                                           "/Engine/Generated/Test/Other.rsh", "y");
    const ShaderParameterLayout pairList[] = {good, pairLayout};
    CHECK(!ValidateShaderParameterLayouts(pairList));
    const ShaderParameterLayout sameNames[] = {good, only};
    CHECK(!ValidateShaderParameterLayouts(sameNames));
    const ShaderParameterResource distinct[] = {
        {"words", "", Kind::DeviceBuffer, 0, Shape::RawBuffer, Component::None, 0, Pair::None}};
    const ShaderParameterLayout distinctLayout("Other", false, 0, {}, distinct,
                                               "rocks::test::Other",
                                               "/Engine/Generated/Test/Other.rsh", "y");
    const ShaderParameterLayout list[] = {good, distinctLayout};
    REQUIRE(ValidateShaderParameterLayouts(list));

    // 정규 byte 왕복·서명·모든 잘린 길이.
    Array<uint8> bytes(TestAllocator());
    REQUIRE(EncodeShaderParameterLayouts(list, bytes));
    CHECK(Sha256::Hash(bytes.View()) == ShaderParameterLayoutSignature(list));
    ShaderParameterLayoutSet decoded(TestAllocator());
    REQUIRE(DecodeShaderParameterLayouts(bytes.View(), decoded));
    REQUIRE(decoded.View().Size() == 2);
    REQUIRE(decoded.layouts[0].resources.Size() == ResourceCount);
    for (usize index = 0; index < ResourceCount; ++index) {
        const ShaderParameterResource& a = resources[index];
        const ShaderParameterResource& b = decoded.layouts[0].resources[index];
        CHECK(a.name == b.name && a.samplerName == b.samplerName && a.kind == b.kind &&
              a.offset == b.offset && a.shape == b.shape && a.component == b.component &&
              a.structureStride == b.structureStride && a.sampler == b.sampler);
    }

    CHECK(decoded.layouts[1].resources.Size() == 1 && decoded.layouts[1].totalBytes == 0 &&
          decoded.layouts[1].members.IsEmpty());
    CHECK(ShaderParameterLayoutSignature(decoded.View()) == ShaderParameterLayoutSignature(list));
    for (usize length = 1; length < bytes.Size(); ++length) {
        CHECK(DecodeShaderParameterLayouts(bytes.View().Left(length), decoded).error ==
              ShaderReadError::CorruptPackage);
    }

    // 자원 칸 하나가 바뀌면 서명이 바뀐다.
    ShaderParameterResource retyped[ResourceCount];
    for (usize index = 0; index < ResourceCount; ++index) {
        retyped[index] = resources[index];
    }

    retyped[1].component = Component::Int;
    const ShaderParameterLayout changed[] = {ShaderParameterLayout("Pass", false, 16, constants,
                                                                   retyped, StructName, SourcePath,
                                                                   Source),
                                             distinctLayout};
    CHECK(!(ShaderParameterLayoutSignature(changed) == ShaderParameterLayoutSignature(list)));
}

} // namespace shader_core_format_tests_file
