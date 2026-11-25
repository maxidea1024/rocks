#include <Core/Lifetime/ModuleLease.h>
#include <Base/NumericLimits.h>
#include <Core/Memory/SystemAllocator.h>
#include <ModuleLoader/HostServices.h>
#include <ShaderCore/ShaderFormatTypes.h>
#include <ShaderCore/ShaderPermutation.h>
#include <ShaderCore/ShaderType.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <ShaderCore/ShaderVirtualPath.h>
#include <Base/InitializerList.h>
#include <TestSupport/Test.h>
#include <string.h>

// ShaderCore 시험이다(Docs/Design/ShaderSystem 12.2.1절 SSD1–SSD4, SH2a1): 가상 경로 문법, 형식
// 어휘 이름, 퍼뮤테이션 도메인(검사·ID 왕복·UE 순서 golden·희소 정수·차원 추가의 ID 보존·개발
// 전용), 셰이더 타입 등록부(검사 음성 대조·목록 원자성·Freeze·정렬·조회·타입 집합 SHA·해제), 모듈
// 등록 도우미(등록부 없음·있음·두 번 붙기·실패 전달·해제).
using namespace rocks;

namespace shader_core_tests_file {

using namespace rocks::shader;

namespace {

SystemAllocator& TestAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

ROCKS_SHADER_TYPE_NAME(TestVertexName, "TestVertex");
ROCKS_SHADER_TYPE_NAME(TestPixelName, "TestPixel");
ROCKS_SHADER_TYPE_NAME(TestComputeName, "TestCompute");
ROCKS_SHADER_TYPE_NAME(CaseOnlyName, "TESTVERTEX");
ROCKS_SHADER_TYPE_NAME(OtherName, "Other");
ROCKS_SHADER_TYPE_NAME(LateName, "Late");

constexpr ShaderTypeDesc TestVertex =
    MakeShaderTypeDesc(TestVertexName, "/Engine/Private/Test.rsf", "MainVS", ShaderStageVertex,
                       ShaderTypeOwner::Engine);
constexpr ShaderTypeDesc TestPixel = MakeShaderTypeDesc(
    TestPixelName, "/Engine/Private/Test.rsf", "MainPS", ShaderStagePixel, ShaderTypeOwner::Engine);
constexpr ShaderTypeDesc TestCompute =
    MakeShaderTypeDesc(TestComputeName, "/Plugin/Sample/Compute.rsf", "MainCS", ShaderStageCompute,
                       ShaderTypeOwner::EnginePlugin);

// 한 칸을 바꾼 desc를 만든다.
template <typename Change>
ShaderTypeDesc Changed(Change change) noexcept {
    ShaderTypeDesc desc = TestVertex;
    change(desc);
    return desc;
}

// desc 하나를 등록해 본다(실패하면 등록부는 그대로다).
Result<void> RegisterOne(ShaderTypeRegistry& registry, const ShaderTypeDesc& desc) noexcept {
    const ShaderTypeDesc* const list[] = {&desc};
    return registry.Register(list);
}

} // namespace

TEST(ShaderCore_VirtualPathGrammar) {
    ShaderVirtualPathInfo info;
    REQUIRE(CheckShaderVirtualPath("/Engine/Private/Test.rsf", info));
    CHECK(info.root == ShaderVirtualRoot::Engine && info.extension == "rsf" && !info.shared);
    REQUIRE(CheckShaderVirtualPath("/Plugin/Sample/Shared/Types.h", info));
    CHECK(info.root == ShaderVirtualRoot::Plugin && info.pluginName == "Sample" && info.shared);
    REQUIRE(CheckShaderVirtualPath("/Project/Lib/Common.rsh", info));
    CHECK(info.root == ShaderVirtualRoot::Project && info.extension == "rsh");

    const struct {
        const char* path;
        ShaderVirtualPathError error;
    } invalid[] = {{"Engine/Test.rsf", ShaderVirtualPathError::InvalidPath},
                   {"/Engine", ShaderVirtualPathError::InvalidPath},
                   {"/Engine/", ShaderVirtualPathError::InvalidPath},
                   {"/Engine//Test.rsf", ShaderVirtualPathError::InvalidPath},
                   {"/Engine/../Test.rsf", ShaderVirtualPathError::InvalidPath},
                   {"/Engine/A b.rsf", ShaderVirtualPathError::InvalidPath},
                   {"/Other/Test.rsf", ShaderVirtualPathError::InvalidPath},
                   {"/Plugin/Bad.Name/Test.rsf", ShaderVirtualPathError::InvalidPath},
                   {"/Plugin/Sample", ShaderVirtualPathError::InvalidPath},
                   {"/Engine/Test.usf", ShaderVirtualPathError::InvalidExtension},
                   {"/Engine/Shared/Types.rsh", ShaderVirtualPathError::InvalidExtension}};
    for (const auto& entry : invalid) {
        const ShaderVirtualPathCheck checked = CheckShaderVirtualPath(entry.path, info);
        CHECK(checked.error == entry.error && checked.reason[0] != '\0' && !checked.pathTooLong);
    }

    char longPath[MaxShaderVirtualPathBytes + 8];
    memset(longPath, 'a', sizeof(longPath));
    memcpy(longPath, "/Engine/", 8);
    const ShaderVirtualPathCheck tooLong =
        CheckShaderVirtualPath(StringView(longPath, sizeof(longPath)), info);
    CHECK(tooLong.error == ShaderVirtualPathError::InvalidPath && tooLong.pathTooLong);

    CHECK(CheckShaderVirtualDirectory("/Engine"));
    CHECK(CheckShaderVirtualDirectory("/Plugin/EngineShaderTests"));
    CHECK(CheckShaderVirtualDirectory("/Engine/").error ==
          ShaderVirtualPathError::InvalidDirectory);
    CHECK(CheckShaderVirtualDirectory("/Plugin/").error ==
          ShaderVirtualPathError::InvalidDirectory);
}

TEST(ShaderCore_FormatVocabularyNames) {
    CHECK(StringView(ShaderBackendName(ShaderBackend::D3D11)) == "D3D11");
    CHECK(StringView(ShaderBackendName(ShaderBackend::OpenGL)) == "OpenGL");
    CHECK(StringView(ShaderBackendName(static_cast<ShaderBackend>(99))) == "unknown");
    CHECK(!IsRuntimeCheckedShaderFeature(ShaderFeatureCompute));
    CHECK(IsRuntimeCheckedShaderFeature(ShaderFeatureVertexViewportLayerOutput));
    CHECK(StringView(ShaderTypeOwnerName(ShaderTypeOwner::ProjectPlugin)) == "ProjectPlugin");
}

TEST(ShaderCore_PermutationIdsFollowUnrealOrder) {
    // UE 순서: 첫 차원이 가장 낮은 자리, 희소 정수의 첫 값이 가장 큰 value id.
    constexpr int32 Samples[] = {8, 4, 1};
    const ShaderPermutationDimension dimensions[] = {
        ShaderPermutationDimension::Bool("USE_FOG"),
        ShaderPermutationDimension::SparseInt("SAMPLES", Samples),
        ShaderPermutationDimension::IntRange("QUALITY", 2, 3)};
    const ShaderPermutationDomain domain{dimensions};
    const Result<uint32> size = ValidateShaderPermutationDomain(domain);
    REQUIRE(size && size.Value() == 2 * 3 * 3);

    CHECK(ShaderPermutationValueId(dimensions[1], 8).Value() == 2);
    CHECK(ShaderPermutationValueId(dimensions[1], 4).Value() == 1);
    CHECK(ShaderPermutationValueId(dimensions[1], 1).Value() == 0);
    CHECK(ShaderPermutationValueId(dimensions[1], 2).Error() == ErrorCode::NotFound);
    CHECK(ShaderPermutationDefineValue(dimensions[1], 2).Value() == 8);
    CHECK(ShaderPermutationDefineValue(dimensions[2], 0).Value() == 2);
    CHECK(ShaderPermutationDefineValue(dimensions[2], 3).Error() == ErrorCode::InvalidArgument);

    // golden: USE_FOG = 1, SAMPLES = 8(value id 2), QUALITY = 3(value id 1) → 1 + 2 × (2 + 3 × 1)
    // = 11.
    const uint32 golden[] = {1, 2, 1};
    CHECK(EncodeShaderPermutation(domain, golden).Value() == 11);

    // 모든 ID가 왕복한다.
    for (uint32 id = 0; id < size.Value(); ++id) {
        uint32 values[3] = {};
        REQUIRE(DecodeShaderPermutation(domain, id, values));
        CHECK(EncodeShaderPermutation(domain, values).Value() == id);
    }

    uint32 values[3] = {};
    CHECK(DecodeShaderPermutation(domain, size.Value(), values).Error() ==
          ErrorCode::InvalidArgument);
    const uint32 outOfRange[] = {2, 0, 0};
    CHECK(EncodeShaderPermutation(domain, outOfRange).Error() == ErrorCode::InvalidArgument);

    // 차원을 끝에 더하면(새 차원 value id 0) 기존 ID가 그대로다.
    const ShaderPermutationDimension extended[] = {dimensions[0], dimensions[1], dimensions[2],
                                                   ShaderPermutationDimension::Bool("NEW_PATH")};
    const ShaderPermutationDomain extendedDomain{extended};
    const uint32 extendedGolden[] = {1, 2, 1, 0};
    CHECK(EncodeShaderPermutation(extendedDomain, extendedGolden).Value() == 11);

    // 차원이 없으면 퍼뮤테이션 하나(ID 0)다.
    const ShaderPermutationDomain empty{};
    CHECK(ValidateShaderPermutationDomain(empty).Value() == 1);
    CHECK(EncodeShaderPermutation(empty, Span<const uint32>()).Value() == 0);
}

TEST(ShaderCore_PermutationDomainValidation) {
    constexpr int32 Duplicated[] = {1, 1};
    constexpr StringView BadEnum[] = {"Fast", "2Slow"};
    constexpr StringView Modes[] = {"Off", "Low", "High"};
    const ShaderPermutationDimension cases[][2] = {
        {ShaderPermutationDimension::Bool("ROCKS_FOG"), ShaderPermutationDimension::Bool("A")},
        {ShaderPermutationDimension::Bool("__FOG"), ShaderPermutationDimension::Bool("A")},
        {ShaderPermutationDimension::Bool("1FOG"), ShaderPermutationDimension::Bool("A")},
        {ShaderPermutationDimension::Bool("A"), ShaderPermutationDimension::Bool("A")},
        {ShaderPermutationDimension::IntRange("A", 0, 0), ShaderPermutationDimension::Bool("B")},
        {ShaderPermutationDimension::IntRange("A", NumericLimits<int32>::Max(), 2),
         ShaderPermutationDimension::Bool("B")},
        {ShaderPermutationDimension::SparseInt("A", Duplicated),
         ShaderPermutationDimension::Bool("B")},
        {ShaderPermutationDimension::SparseInt("A", Span<const int32>()),
         ShaderPermutationDimension::Bool("B")},
        {ShaderPermutationDimension::Enum("A", BadEnum), ShaderPermutationDimension::Bool("B")},
        // 크기 상한: 300 × 300 = 90000 > 65536.
        {ShaderPermutationDimension::IntRange("A", 0, 300),
         ShaderPermutationDimension::IntRange("B", 0, 300)}};
    for (const auto& dimensions : cases) {
        const ShaderPermutationDomain domain{dimensions};
        CHECK(ValidateShaderPermutationDomain(domain).Error() == ErrorCode::InvalidArgument);
    }

    // 상한 경계: 256 × 256 = 65536은 된다.
    const ShaderPermutationDimension limit[] = {ShaderPermutationDimension::IntRange("A", 0, 256),
                                                ShaderPermutationDimension::IntRange("B", 0, 256)};
    CHECK(ValidateShaderPermutationDomain(ShaderPermutationDomain{limit}).Value() ==
          MaxShaderPermutations);

    // 차원 수 상한.
    ShaderPermutationDimension many[MaxShaderPermutationDimensions + 1];
    constexpr StringView Names[] = {"D0", "D1",  "D2",  "D3",  "D4",  "D5",  "D6",  "D7", "D8",
                                    "D9", "D10", "D11", "D12", "D13", "D14", "D15", "D16"};
    for (usize index = 0; index < MaxShaderPermutationDimensions + 1; ++index) {
        many[index] =
            ShaderPermutationDimension::Enum(Names[index], Span<const StringView>(Modes, 1));
    }

    CHECK(ValidateShaderPermutationDomain(ShaderPermutationDomain{many}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ValidateShaderPermutationDomain(
              ShaderPermutationDomain{
                  Span<const ShaderPermutationDimension>(many, MaxShaderPermutationDimensions)})
              .Value() == 1);

    // 개발 전용 차원: 기본값(value id 0)이 아닌 퍼뮤테이션만 개발 전용이다.
    const ShaderPermutationDimension debug[] = {
        ShaderPermutationDimension::Enum("MODE", Modes),
        ShaderPermutationDimension::Bool("GPU_MESSAGES", true)};
    const ShaderPermutationDomain debugDomain{debug};
    CHECK(!IsDevelopmentOnlyShaderPermutation(debugDomain, 2));
    CHECK(IsDevelopmentOnlyShaderPermutation(debugDomain, 3));
    CHECK(!IsDevelopmentOnlyShaderPermutation(debugDomain, 6));
}

TEST(ShaderCore_RegistryRejectsInvalidTypes) {
    ShaderTypeRegistry registry(TestAllocator());
    REQUIRE(RegisterOne(registry, TestVertex));
    REQUIRE(registry.Types().Size() == 1);

    // 같은 이름, 대소문자만 다른 이름(같은 해시).
    CHECK(RegisterOne(registry, TestVertex).Error() == ErrorCode::InvalidArgument);
    CHECK(strstr(registry.LastFailure().reason, "already registered") != nullptr &&
          StringView(registry.LastFailure().typeName) == "TestVertex");
    const ShaderTypeDesc caseOnly = Changed([&](ShaderTypeDesc& desc) noexcept {
        desc.name = CaseOnlyName.name;
        desc.nameHash = CaseOnlyName.hash;
    });
    CHECK(RegisterOne(registry, caseOnly).Error() == ErrorCode::InvalidArgument);
    CHECK(strstr(registry.LastFailure().reason, "collides") != nullptr);

    const auto other = [&](auto change) noexcept {
        return Changed([&](ShaderTypeDesc& desc) noexcept {
            desc.name = OtherName.name;
            desc.nameHash = OtherName.hash;
            change(desc);
        });
    };
    const ShaderPermutationDimension badDimension[] = {ShaderPermutationDimension::Bool("ROCKS_X")};
    const ShaderTypeDesc invalid[] = {
        other([](ShaderTypeDesc& desc) noexcept { desc.name = "Bad Name"; }),
        other([](ShaderTypeDesc& desc) noexcept { desc.nameHash ^= 1; }),
        other([](ShaderTypeDesc& desc) noexcept { desc.sourcePath = "/Engine/Private/Test.rsh"; }),
        other([](ShaderTypeDesc& desc) noexcept { desc.sourcePath = "Engine/Test.rsf"; }),
        other([](ShaderTypeDesc& desc) noexcept { desc.sourcePath = "/Plugin/Sample/Test.rsf"; }),
        other([](ShaderTypeDesc& desc) noexcept { desc.owner = ShaderTypeOwner::Project; }),
        other([](ShaderTypeDesc& desc) noexcept { desc.owner = static_cast<ShaderTypeOwner>(9); }),
        other([](ShaderTypeDesc& desc) noexcept { desc.entryPoint = "Main VS"; }),
        other([](ShaderTypeDesc& desc) noexcept { desc.stage = 0; }),
        other([](ShaderTypeDesc& desc) noexcept {
            desc.stage = ShaderStageVertex | ShaderStagePixel;
        }),
        other([](ShaderTypeDesc& desc) noexcept { desc.requiredFeatures = 1u << 30; }),
        other([](ShaderTypeDesc& desc) noexcept { desc.flags = 1u << 30; }),
        other(
            [](ShaderTypeDesc& desc) noexcept { desc.hlsl2021 = static_cast<ShaderHlsl2021>(7); }),
        other([&](ShaderTypeDesc& desc) noexcept {
            desc.permutation = ShaderPermutationDomain{badDimension};
        })};
    for (const ShaderTypeDesc& desc : invalid) {
        CHECK(RegisterOne(registry, desc).Error() == ErrorCode::InvalidArgument);
        CHECK(registry.LastFailure().reason[0] != '\0');
    }

    // null desc와, 목록 하나가 잘못되면 목록 전체를 등록하지 않음.
    const ShaderTypeDesc* const withNull[] = {&TestPixel, nullptr};
    CHECK(registry.Register(withNull).Error() == ErrorCode::InvalidArgument);
    const ShaderTypeDesc* const duplicatedInBatch[] = {&TestPixel, &TestPixel};
    CHECK(registry.Register(duplicatedInBatch).Error() == ErrorCode::InvalidArgument);
    CHECK(registry.Types().Size() == 1 && registry.Find(TestPixelName.hash) == nullptr);
}

TEST(ShaderCore_RegistryFreezesSortedAndFindsByHash) {
    ShaderTypeRegistry first(TestAllocator());
    ShaderTypeRegistry second(TestAllocator());
    const ShaderTypeDesc* const forward[] = {&TestVertex, &TestPixel, &TestCompute};
    const ShaderTypeDesc* const backward[] = {&TestCompute, &TestPixel, &TestVertex};
    REQUIRE(first.Register(forward));
    REQUIRE(second.Register(Span<const ShaderTypeDesc* const>(backward, 1)));
    REQUIRE(second.Register(Span<const ShaderTypeDesc* const>(backward + 1, 2)));

    // Freeze 전에도 조회는 된다.
    CHECK(first.Find(TestPixelName.hash) == &TestPixel);
    REQUIRE(first.Freeze());
    REQUIRE(second.Freeze());
    CHECK(first.IsFrozen() && first.Freeze());

    // 등록 순서와 무관하게 해시순이고 타입 집합 SHA가 같다.
    REQUIRE(first.Types().Size() == 3);
    for (usize index = 0; index < 3; ++index) {
        CHECK(first.Types()[index] == second.Types()[index]);
    }

    for (usize index = 1; index < 3; ++index) {
        CHECK(first.Types()[index - 1]->nameHash < first.Types()[index]->nameHash);
    }

    CHECK(first.TypeSetDigest() == second.TypeSetDigest());
    CHECK(first.Find(TestVertexName.hash) == &TestVertex);
    CHECK(first.Find(TestComputeName.hash) == &TestCompute);
    CHECK(first.Find(ShaderTypeNameHash("Missing")) == nullptr);

    // Freeze 뒤 등록은 닫혔다.
    const ShaderTypeDesc late = Changed([&](ShaderTypeDesc& desc) noexcept {
        desc.name = LateName.name;
        desc.nameHash = LateName.hash;
    });
    CHECK(RegisterOne(first, late).Error() == ErrorCode::ScopeClosed);

    // 해제하면 집합이 바뀌고 정렬은 유지된다.
    const ShaderTypeDesc* const removed[] = {&TestPixel};
    first.Unregister(removed);
    CHECK(first.Types().Size() == 2 && first.Find(TestPixelName.hash) == nullptr &&
          first.Find(TestVertexName.hash) == &TestVertex);
    CHECK(!(first.TypeSetDigest() == second.TypeSetDigest()));
}

TEST(ShaderCore_ModuleRegistrationHelper) {
    HostServices services(TestAllocator());
    const ShaderTypeDesc* const types[] = {&TestVertex, &TestPixel};

    // 등록부가 없는 호스트(셰이더를 쓰지 않는 도구)에서는 아무 일도 하지 않는다.
    {
        ShaderTypeModuleRegistration registration(types);
        REQUIRE(registration.Attach(services, ModuleLease()));
        CHECK(registration.Registry() == nullptr);
        registration.Shutdown();
    }

    ShaderTypeRegistry registry(TestAllocator());
    REQUIRE(services.Register(registry));
    {
        ShaderTypeModuleRegistration registration(types);
        REQUIRE(registration.Attach(services, ModuleLease()));
        CHECK(registration.Registry() == &registry && registry.Types().Size() == 2);
        CHECK(registration.Attach(services, ModuleLease()).Error() == ErrorCode::InvalidArgument);

        // 같은 타입을 다른 모듈이 등록하면 그 모듈의 시작이 실패한다.
        ShaderTypeModuleRegistration duplicate(types);
        CHECK(duplicate.Attach(services, ModuleLease()).Error() == ErrorCode::InvalidArgument);
        CHECK(duplicate.Registry() == nullptr);

        registration.Shutdown();
        CHECK(registration.Registry() == nullptr && registry.Types().IsEmpty());
        REQUIRE(registration.Attach(services, ModuleLease()));
    }

    // 소멸자가 등록을 거둔다.
    CHECK(registry.Types().IsEmpty());
    CHECK(services.Unregister(registry));
}

namespace {

ROCKS_SHADER_TYPE_NAME(InputAName, "TestVertexInputA");
ROCKS_SHADER_TYPE_NAME(InputBName, "TestVertexInputB");
ROCKS_SHADER_TYPE_NAME(InputCaseName, "TESTVERTEXINPUTA");

constexpr ShaderVertexInputType InputA = MakeShaderVertexInputType(
    InputAName, "/Engine/Private/VertexInput/A.rsh", ShaderTypeOwner::Engine);
constexpr ShaderVertexInputType InputB = MakeShaderVertexInputType(
    InputBName, "/Plugin/Sample/VertexInput/B.rsh", ShaderTypeOwner::EnginePlugin);

ShaderVertexInputType ChangedInput(void (*change)(ShaderVertexInputType&)) noexcept {
    ShaderVertexInputType input = MakeShaderVertexInputType(
        OtherName, "/Engine/Private/VertexInput/Other.rsh", ShaderTypeOwner::Engine);
    change(input);
    return input;
}

} // namespace

// 정점 입력 타입 등록(Docs/Design/RenderBase 10.6절 1항): 검사 음성 대조, 목록 원자성, 등록 순서와
// 무관한 id 순, 조회, Freeze 뒤 거부(늦은 등록), 해제, 모듈 등록 도우미의 실패 되돌림.
TEST(ShaderCore_RegistryVertexInputTypes) {
    ShaderTypeRegistry first(TestAllocator());
    ShaderTypeRegistry second(TestAllocator());
    const ShaderVertexInputType forward[] = {InputA, InputB};
    const ShaderVertexInputType backward[] = {InputB, InputA};
    REQUIRE(first.RegisterVertexInputTypes(forward));
    REQUIRE(second.RegisterVertexInputTypes(Span<const ShaderVertexInputType>(backward, 1)));
    REQUIRE(second.RegisterVertexInputTypes(Span<const ShaderVertexInputType>(backward + 1, 1)));
    REQUIRE(first.Freeze());
    REQUIRE(second.Freeze());
    REQUIRE(first.VertexInputTypes().Size() == 2 && second.VertexInputTypes().Size() == 2);
    CHECK(first.VertexInputTypes()[0].id < first.VertexInputTypes()[1].id);
    for (usize index = 0; index < 2; ++index) {
        CHECK(first.VertexInputTypes()[index].id == second.VertexInputTypes()[index].id);
    }

    CHECK(first.FindVertexInputType(InputAName.hash) != nullptr &&
          first.FindVertexInputType(InputAName.hash)->sourcePath == InputA.sourcePath);
    CHECK(first.FindVertexInputType(ShaderTypeNameHash("Missing")) == nullptr);
    // 정점 입력 타입은 타입 집합 SHA에 들지 않는다.
    ShaderTypeRegistry empty(TestAllocator());
    REQUIRE(empty.Freeze());
    CHECK(first.TypeSetDigest() == empty.TypeSetDigest());
    // Freeze 뒤 등록은 닫혔다.
    const ShaderVertexInputType late[] = {ChangedInput([](ShaderVertexInputType&) noexcept {})};
    CHECK(first.RegisterVertexInputTypes(late).Error() == ErrorCode::ScopeClosed);
    const ShaderVertexInputType removed[] = {InputA};
    first.UnregisterVertexInputTypes(removed);
    CHECK(first.VertexInputTypes().Size() == 1 &&
          first.FindVertexInputType(InputAName.hash) == nullptr);

    ShaderTypeRegistry registry(TestAllocator());
    const ShaderVertexInputType one[] = {InputA};
    REQUIRE(registry.RegisterVertexInputTypes(one));
    // 같은 이름, 대소문자만 다른 이름(같은 해시).
    CHECK(registry.RegisterVertexInputTypes(one).Error() == ErrorCode::InvalidArgument);
    CHECK(strstr(registry.LastFailure().reason, "already registered") != nullptr);
    const ShaderVertexInputType caseOnly[] = {MakeShaderVertexInputType(
        InputCaseName, "/Engine/Private/VertexInput/A2.rsh", ShaderTypeOwner::Engine)};
    CHECK(registry.RegisterVertexInputTypes(caseOnly).Error() == ErrorCode::InvalidArgument);
    CHECK(strstr(registry.LastFailure().reason, "collides") != nullptr);
    const ShaderVertexInputType invalid[] = {
        ChangedInput([](ShaderVertexInputType& input) noexcept { input.name = "Bad Name"; }),
        ChangedInput([](ShaderVertexInputType& input) noexcept { input.id ^= 1; }),
        ChangedInput([](ShaderVertexInputType& input) noexcept { input.id = NoVertexInputType; }),
        ChangedInput([](ShaderVertexInputType& input) noexcept {
            input.sourcePath = "/Engine/Private/VertexInput/Other.rsf";
        }),
        ChangedInput(
            [](ShaderVertexInputType& input) noexcept { input.sourcePath = "Engine/Other.rsh"; }),
        ChangedInput([](ShaderVertexInputType& input) noexcept {
            input.sourcePath = "/Engine/Shared/Other.h";
        }),
        ChangedInput([](ShaderVertexInputType& input) noexcept {
            input.sourcePath = "/Engine/Generated/VertexInput.rsh";
        }),
        ChangedInput(
            [](ShaderVertexInputType& input) noexcept { input.owner = ShaderTypeOwner::Project; }),
        ChangedInput([](ShaderVertexInputType& input) noexcept {
            input.owner = static_cast<ShaderTypeOwner>(9);
        })};
    for (const ShaderVertexInputType& input : invalid) {
        CHECK(registry.RegisterVertexInputTypes(Span<const ShaderVertexInputType>(&input, 1))
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(registry.LastFailure().reason[0] != '\0');
    }

    // 목록 하나가 잘못되면(같은 목록 안 중복 포함) 목록 전체를 등록하지 않는다.
    const ShaderVertexInputType duplicated[] = {InputB, InputB};
    CHECK(registry.RegisterVertexInputTypes(duplicated).Error() == ErrorCode::InvalidArgument);
    CHECK(registry.VertexInputTypes().Size() == 1 &&
          registry.FindVertexInputType(InputBName.hash) == nullptr);

    // 모듈 등록 도우미: 정점 입력 타입 등록이 실패하면 앞서 등록한 셰이더 타입도 거둔다.
    HostServices services(TestAllocator());
    REQUIRE(services.Register(registry));
    {
        const ShaderTypeDesc* const types[] = {&TestPixel};
        ShaderTypeModuleRegistration failing(types, one);
        CHECK(failing.Attach(services, ModuleLease()).Error() == ErrorCode::InvalidArgument);
        CHECK(failing.Registry() == nullptr && registry.Find(TestPixelName.hash) == nullptr);
        const ShaderVertexInputType others[] = {InputB};
        ShaderTypeModuleRegistration module(types, others);
        REQUIRE(module.Attach(services, ModuleLease()));
        CHECK(registry.Find(TestPixelName.hash) != nullptr &&
              registry.FindVertexInputType(InputBName.hash) != nullptr);
    }

    CHECK(registry.Find(TestPixelName.hash) == nullptr &&
          registry.FindVertexInputType(InputBName.hash) == nullptr &&
          registry.VertexInputTypes().Size() == 1);
    CHECK(services.Unregister(registry));
}

} // namespace shader_core_tests_file
