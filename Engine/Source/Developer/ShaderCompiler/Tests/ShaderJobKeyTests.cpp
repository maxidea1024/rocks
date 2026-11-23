#include <AssetNugget/NuggetKey.h>
#include <Core/Build/BuildInfo.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <ShaderCompiler/ShaderCompileOptions.h>
#include <ShaderCompiler/ShaderJobKey.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <Base/InitializerList.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;
namespace shader_job_key_tests_file {

using namespace rocks::shader;

// 셰이더 job 키(Docs/Design/ShaderSystem 10절) 시험이다: 입력별 무효화와 불변(완료 기준
// 목록), 공통 머리가 A3 머리 그대로임, trace golden(순서·태그·필드 이름은 고정 — 툴체인 지문·결정적
// 수학 버전 줄은 실행 중 값), 리터럴 키 golden(공통 머리의 두 항목을 고정값으로 쓰는 기록 경로),
// 오류. 형식 정의는 fixture 도구 표로 읽은 fixture byte라 도구 SHA·형식 SHA가 호스트·manifest와
// 무관하다.
namespace {

// fixture 형식 정의(VULKAN_SM5와 같은 내용, 바이트 고정)다.
constexpr char FixtureVulkan[] =
    "{\n"
    "  \"schema\": 1,\n"
    "  \"id\": \"VULKAN_SM5\",\n"
    "  \"status\": \"supported\",\n"
    "  \"backend\": \"Vulkan\",\n"
    "  \"processor\": { \"module\": \"ShaderFormatVulkan\", \"version\": 1 },\n"
    "  \"bindingConvention\": 1,\n"
    "  \"tools\": [\"DirectXShaderCompiler\", \"SpirvTools\"],\n"
    "  \"language\": { \"hlsl2021\": true },\n"
    "  \"compiler\": {\n"
    "    \"path\": \"DxcSpirv\",\n"
    "    \"shaderModel\": \"6_0\",\n"
    "    \"imageFormat\": \"VkImageFormat\",\n"
    "    \"spirvTargetEnv\": \"vulkan1.1\",\n"
    "    \"dxPositionW\": true\n"
    "  },\n"
    "  \"stages\": [\"vertex\", \"pixel\", \"compute\"],\n"
    "  \"limits\": {\n"
    "    \"constantBuffers\": 12,\n"
    "    \"sampledTextures\": 128,\n"
    "    \"storageBuffers\": 4,\n"
    "    \"storageImages\": 4,\n"
    "    \"samplers\": 16,\n"
    "    \"uavStages\": [\"compute\"],\n"
    "    \"renderTargets\": 8,\n"
    "    \"interStageVectors\": 16,\n"
    "    \"rootConstantBytes\": 128\n"
    "  },\n"
    "  \"features\": [\"compute\", \"vertexViewportLayerOutput\"],\n"
    "  \"deviceRequirements\": [\"VULKAN_1_1\"],\n"
    "  \"producers\": [{ \"host\": \"windows-x64\" }, { \"host\": \"macos-arm64\" }]\n"
    "}\n";

// 리터럴 키 golden(공통 머리의 결정적 수학 버전 = GoldenMathVersion, 툴체인 지문 = 0x5a×32)이다.
// 기록 순서·태그·번호 표를 바꾸면 이 값이 바뀐다(그때는 ShaderJobProcessor 버전 Guid도 바꾼다).
constexpr char GoldenKey[] = "4e02c5b0a4a127223a064a17dc32da1cc1c84e954bd680e47b87c62ed80460ef";
constexpr uint32 GoldenMathVersion = 7;
constexpr uint8 GoldenToolchainByte = 0x5a;

Sha256Digest Digest(uint8 seed) noexcept {
    Sha256Digest digest;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] = static_cast<uint8>(seed + index);
    }

    return digest;
}

// fixture 도구 표다(파일 SHA·role 고정). Symbols·Notice 파일은 키에 들어가지 않는다.
struct FixtureTools {
    FixtureTools() noexcept {
        dxcFiles[0] = {"bin/dxcompiler.dll", Digest(0x10), 100, ShaderToolFileRole::Runtime, true};
        dxcFiles[1] = {"bin/dxcompiler.pdb", Digest(0x20), 200, ShaderToolFileRole::Symbols, true};
        dxcFiles[2] = {"LICENSE.TXT", Digest(0x30), 10, ShaderToolFileRole::Notice, true};
        spirvFiles[0] = {"bin/SPIRV-Tools-c.dll", Digest(0x40), 300, ShaderToolFileRole::Runtime,
                         true};
        spirvFiles[1] = {"bin/spirv-val.exe", Digest(0x50), 50, ShaderToolFileRole::Tool, true};
        spirvFiles[2] = {"bin/vcruntime140.dll", Digest(0x60), 60, ShaderToolFileRole::Redist,
                         true};
        tools[0] = {
            "DirectXShaderCompiler", "1.9.2609", ShaderToolOrigin::Build, "dxc", dxcFiles, 3};
        tools[1] = {"SpirvTools", "2025.1", ShaderToolOrigin::Build, "spirv", spirvFiles, 3};
    }

    ShaderToolFile dxcFiles[3];
    ShaderToolFile spirvFiles[3];
    ShaderTool tools[2];
};

class LineTrace final : public NuggetKeyTrace {
public:
    explicit LineTrace(Allocator& allocator) noexcept : text(allocator) {
    }

    void Line(StringView line) noexcept override {
        CHECK(text.TryAppend(line) && text.TryAppend("\n"));
    }

    String text;
};

// 옵션을 리플렉션으로 기록할 문맥이다.
struct KeyEnv {
    KeyEnv() noexcept
        : types(system), registration(types, system), objects(system, types, {0x53484b, 1}),
          access(objects.OwnerAccess()) {
        registered = static_cast<bool>(RegisterShaderCompilerTypes(registration));
    }

    ~KeyEnv() noexcept {
        CHECK(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    ShaderKeyReflection Reflection() noexcept {
        return ShaderKeyReflection{&objects, access, &types, &system};
    }

    SystemAllocator system;
    TypeRegistry types;
    ModuleRegistration registration;
    ObjectRegistry objects;
    ObjectAccessContext access;
    bool registered = false;
};

// 키 입력 한 벌이다(fixture 형식·도구 표).
struct KeyCase {
    KeyCase() noexcept {
        const Span<const uint8> bytes(reinterpret_cast<const uint8*>(FixtureVulkan),
                                      sizeof(FixtureVulkan) - 1);
        parsed = static_cast<bool>(ParseShaderFormatDefinition(
            bytes, "VULKAN_SM5", format, Span<const ShaderTool>(fixture.tools, 2)));
        options = DefaultShaderCompileOptions(format);
        input.projectGeneration = Guid{0x70726f6a, 0x1};
        input.format = &format;
        input.stage = ShaderStagePixel;
        input.entry = "PSMain";
        input.preprocessedSha256 = Digest(0x80);
        input.options = &options;
        input.reflection = env.Reflection();
        input.profileId = "Engine-Windows";
        input.profileSha256 = Digest(0x90);
    }

    bool Ready() const noexcept {
        return parsed && env.registered;
    }

    Sha256Digest Key(NuggetKeyTrace* trace = nullptr) noexcept {
        auto key = BuildShaderJobKey(input, trace);
        CHECK(key);
        return key ? key.Value() : Sha256Digest{};
    }

    KeyEnv env;
    FixtureTools fixture;
    ShaderFormatDefinition format;
    ShaderCompileOptions options;
    ShaderJobKeyInput input;
    bool parsed = false;
};

// 공통 머리를 손으로 쓴다(시험용 기록 경로 — 바꿀 수 없는 항목을 고정값으로 넣는다).
void Header(NuggetKeyWriter& writer, Guid project, uint32 mathVersion,
            const Sha256Digest& toolchain, const NuggetProcessorId& processor = ShaderJobProcessor,
            Guid engine = EngineNuggetGeneration) noexcept {
    writer.WriteString("RocksAssetNugget");
    writer.WriteU32(NuggetKeyFormatVersion);
    writer.WriteGuid(engine);
    writer.WriteGuid(project);
    writer.WriteGuid(processor.processor);
    writer.WriteGuid(processor.version);
    writer.WriteU32(mathVersion);
    writer.WriteDigest(toolchain);
}

Sha256Digest ManualKey(const ShaderJobKeyInput& input, uint32 mathVersion,
                       const Sha256Digest& toolchain,
                       const NuggetProcessorId& processor = ShaderJobProcessor,
                       Guid engine = EngineNuggetGeneration) noexcept {
    NuggetKeyWriter writer;
    Header(writer, input.projectGeneration, mathVersion, toolchain, processor, engine);
    CHECK(WriteShaderJobKeyBody(writer, input));
    auto key = writer.Finish();
    CHECK(key);
    return key ? key.Value() : Sha256Digest{};
}

void Hex(const Sha256Digest& digest, char (&out)[Sha256Digest::HexBytes + 1]) noexcept {
    digest.ToHex(out);
}

// Guid 추적 표기(8-4-4-4-12, high → low)다.
void GuidText(Guid value, char (&out)[37]) noexcept {
    snprintf(out, sizeof(out), "%08x-%04x-%04x-%04x-%012llx",
             static_cast<unsigned>(value.high >> 32),
             static_cast<unsigned>((value.high >> 16) & 0xffff),
             static_cast<unsigned>(value.high & 0xffff), static_cast<unsigned>(value.low >> 48),
             static_cast<unsigned long long>(value.low & 0xffffffffffffull));
}

} // namespace

TEST(ShaderJobKey_HeaderIsTheCommonNuggetHeader) {
    KeyCase test;
    REQUIRE(test.Ready());
    // BuildShaderJobKey = A3 공통 머리(실행 중 결정적 수학 버전·툴체인 지문) + 2–7행.
    CHECK(test.Key() ==
          ManualKey(test.input, deterministic::Version, build::ToolchainFingerprint()));
    // 같은 입력을 다시 계산해도 같다.
    CHECK(test.Key() == test.Key());
    // 머리의 바꿀 수 없는 항목(툴체인 지문·결정적 수학 버전·엔진 세대·처리기 버전)도 키를 바꾼다.
    const Sha256Digest real =
        ManualKey(test.input, deterministic::Version, build::ToolchainFingerprint());
    CHECK(!(real ==
            ManualKey(test.input, deterministic::Version + 1, build::ToolchainFingerprint())));
    CHECK(!(real == ManualKey(test.input, deterministic::Version, Digest(0x01))));
    CHECK(!(real == ManualKey(test.input, deterministic::Version, build::ToolchainFingerprint(),
                              ShaderJobProcessor, Guid{0x1, 0x2})));
    CHECK(!(real == ManualKey(test.input, deterministic::Version, build::ToolchainFingerprint(),
                              NuggetProcessorId{ShaderJobProcessor.processor, Guid{0x3, 0x4}})));
    CHECK(!(real == ManualKey(test.input, deterministic::Version, build::ToolchainFingerprint(),
                              NuggetProcessorId{Guid{0x5, 0x6}, ShaderJobProcessor.version})));
}

TEST(ShaderJobKey_EveryInputChangesTheKey) {
    KeyCase test;
    REQUIRE(test.Ready());
    const Sha256Digest base = test.Key();
    // 입력 하나를 바꾸고 키를 본 뒤 되돌린다.
    auto changes = [&test, &base](auto&& mutate, auto&& restore) noexcept {
        mutate(test);
        const Sha256Digest changed = test.Key();
        restore(test);
        CHECK(test.Key() == base);
        return !(changed == base);
    };

    // 옵션 필드마다.
    CHECK(changes([](KeyCase& c) noexcept { c.options.hlsl2021 = !c.options.hlsl2021; },
                  [](KeyCase& c) noexcept { c.options.hlsl2021 = !c.options.hlsl2021; }));
    CHECK(changes([](KeyCase& c) noexcept { c.options.optimization = ShaderOptimization::O1; },
                  [](KeyCase& c) noexcept { c.options.optimization = ShaderOptimization::O3; }));
    // 형식 정의(byte SHA, id).
    CHECK(changes([](KeyCase& c) noexcept { c.format.sha256 = Digest(0x01); },
                  [](KeyCase& c) noexcept {
                      c.format.sha256 = Sha256::Hash(
                          Span<const uint8>(reinterpret_cast<const uint8*>(FixtureVulkan),
                                            sizeof(FixtureVulkan) - 1));
                  }));
    CHECK(changes([](KeyCase& c) noexcept { c.format.id[0] = 'W'; },
                  [](KeyCase& c) noexcept { c.format.id[0] = 'V'; }));
    // 도구: 출력에 관여하는 파일(Runtime·Tool·Redist) SHA·도구 버전은 키를 바꾸고, Symbols·Notice는
    // 바꾸지 않는다.
    CHECK(changes([](KeyCase& c) noexcept { c.fixture.dxcFiles[0].sha256 = Digest(0x11); },
                  [](KeyCase& c) noexcept { c.fixture.dxcFiles[0].sha256 = Digest(0x10); }));
    CHECK(changes([](KeyCase& c) noexcept { c.fixture.spirvFiles[1].sha256 = Digest(0x51); },
                  [](KeyCase& c) noexcept { c.fixture.spirvFiles[1].sha256 = Digest(0x50); }));
    CHECK(changes([](KeyCase& c) noexcept { c.fixture.spirvFiles[2].sha256 = Digest(0x61); },
                  [](KeyCase& c) noexcept { c.fixture.spirvFiles[2].sha256 = Digest(0x60); }));
    CHECK(changes([](KeyCase& c) noexcept { c.fixture.tools[0].version = "1.9.2610"; },
                  [](KeyCase& c) noexcept { c.fixture.tools[0].version = "1.9.2609"; }));
    CHECK(!changes([](KeyCase& c) noexcept { c.fixture.dxcFiles[1].sha256 = Digest(0x21); },
                   [](KeyCase& c) noexcept { c.fixture.dxcFiles[1].sha256 = Digest(0x20); }));
    CHECK(!changes([](KeyCase& c) noexcept { c.fixture.dxcFiles[2].sha256 = Digest(0x31); },
                   [](KeyCase& c) noexcept { c.fixture.dxcFiles[2].sha256 = Digest(0x30); }));
    // 프로젝트 세대.
    CHECK(changes([](KeyCase& c) noexcept { c.input.projectGeneration = Guid{}; },
                  [](KeyCase& c) noexcept { c.input.projectGeneration = Guid{0x70726f6a, 0x1}; }));
    // 단계·엔트리.
    CHECK(changes([](KeyCase& c) noexcept { c.input.stage = ShaderStageVertex; },
                  [](KeyCase& c) noexcept { c.input.stage = ShaderStagePixel; }));
    CHECK(changes([](KeyCase& c) noexcept { c.input.entry = "PSMain2"; },
                  [](KeyCase& c) noexcept { c.input.entry = "PSMain"; }));
    // 전처리 결과.
    CHECK(changes([](KeyCase& c) noexcept { c.input.preprocessedSha256 = Digest(0x81); },
                  [](KeyCase& c) noexcept { c.input.preprocessedSha256 = Digest(0x80); }));
    // 레이아웃 서명: 없음 → 있음, 값.
    static const Sha256Digest layoutA = Digest(0xa0);
    static const Sha256Digest layoutB = Digest(0xa1);
    CHECK(changes([](KeyCase& c) noexcept { c.input.layoutSignature = &layoutA; },
                  [](KeyCase& c) noexcept { c.input.layoutSignature = nullptr; }));
    {
        test.input.layoutSignature = &layoutA;
        const Sha256Digest withA = test.Key();
        test.input.layoutSignature = &layoutB;
        CHECK(!(withA == test.Key()));
        test.input.layoutSignature = nullptr;
    }

    // 프로필 출력 설정(id·byte SHA).
    CHECK(changes([](KeyCase& c) noexcept { c.input.profileId = "Engine-MacOS"; },
                  [](KeyCase& c) noexcept { c.input.profileId = "Engine-Windows"; }));
    CHECK(changes([](KeyCase& c) noexcept { c.input.profileSha256 = Digest(0x91); },
                  [](KeyCase& c) noexcept { c.input.profileSha256 = Digest(0x90); }));
}

TEST(ShaderJobKey_SameInputsGiveTheSameKey) {
    // 옵션을 쓴 순서·파서에 준 순서, 형식 정의 snapshot을 둔 곳, 계산한 시각과 무관하다(키에
    // 경로·시각· locale·작업 디렉터리가 없다).
    KeyCase first;
    KeyCase second;
    REQUIRE(first.Ready() && second.Ready());
    const StringView forward[] = {"optimization=O1", "hlsl2021=false"};
    const StringView backward[] = {"hlsl2021=false", "optimization=O1"};
    REQUIRE(ParseShaderCompileOptions(forward, first.options));
    REQUIRE(ParseShaderCompileOptions(backward, second.options));
    CHECK(first.Key() == second.Key());
    ShaderCompileOptions assigned;
    assigned.optimization = ShaderOptimization::O1;
    assigned.hlsl2021 = false;
    second.options = assigned;
    CHECK(first.Key() == second.Key());
    // 기본 옵션을 명시해도 같다(기본값 포함 전체를 쓴다).
    KeyCase defaults;
    KeyCase spelled;
    REQUIRE(defaults.Ready() && spelled.Ready());
    const StringView all[] = {"hlsl2021=true", "optimization=O3", "debugSymbols=false",
                              "matrixLayout=Row"};
    REQUIRE(ParseShaderCompileOptions(all, spelled.options));
    CHECK(defaults.Key() == spelled.Key());
}

TEST(ShaderJobKey_SystemToolFingerprintIsAnInput) {
    // METAL_SM5는 system 도구(MetalToolchain)를 쓴다. 지문이 없으면 UnsupportedCapability, 지문의
    // 버전 줄·metal·metallib SHA는 각각 키를 바꾼다.
    KeyEnv env;
    REQUIRE(env.registered);
    ShaderFormatDefinition metal;
    REQUIRE(LoadShaderFormatDefinition(ShaderFormatDirectory, "METAL_SM5", metal));
    ShaderCompileOptions options = DefaultShaderCompileOptions(metal);
    ShaderJobKeyInput input;
    input.format = &metal;
    input.stage = ShaderStageCompute;
    input.entry = "CSMain";
    input.options = &options;
    input.reflection = env.Reflection();
    input.profileId = "Engine-MacOS";
    CHECK(BuildShaderJobKey(input).Error() == ErrorCode::UnsupportedCapability);
    ShaderSystemToolFingerprint other;
    other.name = "OtherTool";
    input.systemTools = Span<const ShaderSystemToolFingerprint>(&other, 1);
    CHECK(BuildShaderJobKey(input).Error() == ErrorCode::UnsupportedCapability);

    ShaderSystemToolFingerprint fingerprint;
    fingerprint.name = MetalToolName;
    fingerprint.version =
        "Apple metal version 32023.404 (metalfe-32023.404)\nTarget: air64-apple-darwin";
    fingerprint.executables[0] = Digest(0x01);
    fingerprint.executables[1] = Digest(0x02);
    input.systemTools = Span<const ShaderSystemToolFingerprint>(&fingerprint, 1);
    const Result<Sha256Digest> base = BuildShaderJobKey(input);
    if (!base && base.Error() == ErrorCode::UnsupportedCapability) {
        // 이 호스트 도구 표에 METAL_SM5의 저장소 도구 파일이 없다(개발 호스트가 아님).
        printf("[SKIP] ShaderJobKey_SystemToolFingerprintIsAnInput: no host tool files\n");
        return;
    }

    REQUIRE(base);
    fingerprint.version =
        "Apple metal version 32023.405 (metalfe-32023.405)\nTarget: air64-apple-darwin";
    CHECK(!(BuildShaderJobKey(input).Value() == base.Value()));
    fingerprint.version =
        "Apple metal version 32023.404 (metalfe-32023.404)\nTarget: air64-apple-darwin";
    fingerprint.executables[0] = Digest(0x03);
    CHECK(!(BuildShaderJobKey(input).Value() == base.Value()));
    fingerprint.executables[0] = Digest(0x01);
    fingerprint.executables[1] = Digest(0x04);
    CHECK(!(BuildShaderJobKey(input).Value() == base.Value()));
    fingerprint.executables[1] = Digest(0x02);
    CHECK(BuildShaderJobKey(input).Value() == base.Value());
}

TEST(ShaderJobKey_TraceGolden) {
    KeyCase test;
    REQUIRE(test.Ready());
    LineTrace trace(test.env.system);
    const Sha256Digest traced = test.Key(&trace);
    CHECK(traced == test.Key());

    // 호스트·빌드마다 다른 줄(결정적 수학 버전·툴체인 지문)은 실행 중 값이다.
    char toolchain[Sha256Digest::HexBytes + 1];
    Hex(build::ToolchainFingerprint(), toolchain);
    char engine[37];
    char project[37];
    char processor[37];
    char version[37];
    GuidText(EngineNuggetGeneration, engine);
    GuidText(test.input.projectGeneration, project);
    GuidText(ShaderJobProcessor.processor, processor);
    GuidText(ShaderJobProcessor.version, version);
    char formatSha[Sha256Digest::HexBytes + 1];
    char preprocessed[Sha256Digest::HexBytes + 1];
    char profile[Sha256Digest::HexBytes + 1];
    Hex(test.format.sha256, formatSha);
    Hex(test.input.preprocessedSha256, preprocessed);
    Hex(test.input.profileSha256, profile);
    char expected[4096];
    const int written = snprintf(
        expected, sizeof(expected),
        // 1행: 공통 머리.
        "string\nRocksAssetNugget\nu32 %u\nguid %s\nguid %s\nguid %s\nguid %s\nu32 %u\ndigest %s\n"
        // 2행: 도구 목록(형식 정의 순서, Runtime·Redist·Tool SHA만).
        "list 2\nstring\nDirectXShaderCompiler\nstring\n1.9.2609\nbytes 32\n"
        "string\nSpirvTools\nstring\n2025.1\nbytes 96\n"
        // 3–5행.
        "string\nVULKAN_SM5\ndigest %s\nu32 2\nstring\nPSMain\ndigest %s\n"
        // 6행: 옵션 리플렉션 기록(필드 이름 포함 — 순서는 리플렉션 순회 순서)과 레이아웃 없음.
        "struct\nfield\nmatrixLayout\ni64 0\nfield\nhlsl2021\nbool true\nfield\ndebugSymbols\nbool "
        "false\nfield\noptimization\ni64 3\nend\nnone\n"
        // 7행.
        "string\nEngine-Windows\ndigest %s\n",
        NuggetKeyFormatVersion, engine, project, processor, version, deterministic::Version,
        toolchain, formatSha, preprocessed, profile);
    REQUIRE(written > 0 && static_cast<usize>(written) < sizeof(expected));
    if (trace.text.View() != StringView(expected)) {
        fprintf(stderr, "--- expected ---\n%s--- actual ---\n%.*s--- end ---\n", expected,
                static_cast<int>(trace.text.ByteCount()), trace.text.Data());
    }

    CHECK(trace.text.View() == StringView(expected));
}

TEST(ShaderJobKey_LiteralGolden) {
    // 공통 머리의 결정적 수학 버전·툴체인 지문을 고정값으로 넣는 기록 경로에서만 리터럴 키를
    // 비교한다.
    KeyCase test;
    REQUIRE(test.Ready());
    Sha256Digest toolchain;
    memset(toolchain.bytes, GoldenToolchainByte, sizeof(toolchain.bytes));
    char hex[Sha256Digest::HexBytes + 1];
    Hex(ManualKey(test.input, GoldenMathVersion, toolchain), hex);
    if (strcmp(hex, GoldenKey) != 0) {
        fprintf(stderr, "shader job literal key: %s\n", hex);
    }

    CHECK(strcmp(hex, GoldenKey) == 0);
}

TEST(ShaderJobKey_RejectsInvalidInputs) {
    KeyCase test;
    REQUIRE(test.Ready());
    auto errorOf = [&test]() noexcept { return BuildShaderJobKey(test.input).Error(); };
    // 단계: 0, 두 비트, 형식이 받지 않는 비트.
    for (const uint32 stage : {0u, 3u, 8u}) {
        test.input.stage = stage;
        CHECK(errorOf() == ErrorCode::InvalidArgument);
    }

    test.input.stage = ShaderStagePixel;
    test.input.entry = StringView();
    CHECK(errorOf() == ErrorCode::InvalidArgument);
    test.input.entry = "PSMain";
    test.input.profileId = StringView();
    CHECK(errorOf() == ErrorCode::InvalidArgument);
    test.input.profileId = "Engine-Windows";
    test.input.options = nullptr;
    CHECK(errorOf() == ErrorCode::InvalidArgument);
    test.input.options = &test.options;
    test.input.reflection.types = nullptr;
    CHECK(errorOf() == ErrorCode::InvalidArgument);
    test.input.reflection = test.env.Reflection();
    // SH1은 디버그 심볼을 받지 않는다.
    test.options.debugSymbols = true;
    CHECK(errorOf() == ErrorCode::Unsupported);
    test.options.debugSymbols = false;
    // 자리만 둔 형식.
    test.format.state = ShaderFormatState::Placeholder;
    CHECK(errorOf() == ErrorCode::InvalidArgument);
    test.format.state = ShaderFormatState::Supported;
    // 이 호스트 파일이 없는 저장소 도구.
    test.fixture.tools[1].fileCount = 0;
    CHECK(errorOf() == ErrorCode::UnsupportedCapability);
    test.fixture.tools[1].fileCount = 3;
    // 타입을 등록하지 않은 리플렉션 문맥.
    {
        SystemAllocator system;
        TypeRegistry types(system);
        ObjectRegistry objects(system, types, {0x53484b, 2});
        test.input.reflection =
            ShaderKeyReflection{&objects, objects.OwnerAccess(), &types, &system};
        CHECK(!BuildShaderJobKey(test.input));
        test.input.reflection = test.env.Reflection();
        CHECK(objects.Shutdown(objects.OwnerAccess()));
        CHECK(types.Shutdown());
    }

    CHECK(BuildShaderJobKey(test.input));
}

} // namespace shader_job_key_tests_file
