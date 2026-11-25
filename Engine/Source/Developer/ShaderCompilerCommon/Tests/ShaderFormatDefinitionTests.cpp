#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/File.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <Base/InitializerList.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;
namespace shader_format_definition_tests_file {

using namespace rocks::shader;

// 셰이더 형식 정의·엔진 컴파일 프로필(Docs/Design/ShaderSystem 12.1절, P6a)의 저장소 파일과
// 스키마 검증 시험이다. 시험은 저장소 루트에서 돈다(Engine/Config/ShaderFormats). 음성 대조는
// 저장소 파일 byte에서 한 군데만 바꾼 입력으로 만든다.
namespace {

SystemAllocator& TestAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 저장소의 <id>.json byte다.
Array<uint8> ReadConfig(const char* id) noexcept {
    Array<uint8> bytes(TestAllocator());
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.json", ShaderFormatDirectory, id);
    platform::File file;
    uint64 size = 0;
    if (file.Open(path, platform::FileOpenMode::ReadExisting) && file.Size(size) &&
        bytes.TryResize(static_cast<usize>(size))) {
        (void)file.ReadExact(bytes.Data(), bytes.Size());
    }

    (void)file.Close();
    return bytes;
}

// bytes에서 from이 정확히 한 번 나와야 하며 그것을 to로 바꾼 사본이다(아니면 빈 배열).
Array<uint8> Replace(const Array<uint8>& bytes, const char* from, const char* to) noexcept {
    Array<uint8> result(TestAllocator());
    const usize fromBytes = strlen(from);
    const usize toBytes = strlen(to);
    usize found = IndexNone;
    for (usize index = 0; index + fromBytes <= bytes.Size(); ++index) {
        if (memcmp(bytes.Data() + index, from, fromBytes) == 0) {
            if (found != IndexNone) {
                return result;
            }

            found = index;
        }
    }

    if (found == IndexNone) {
        return result;
    }

    const Span<const uint8> all = bytes.View();
    (void)result.TryAppend(Span<const uint8>(all.Data(), found));
    (void)result.TryAppend(Span<const uint8>(reinterpret_cast<const uint8*>(to), toBytes));
    (void)result.TryAppend(
        Span<const uint8>(all.Data() + found + fromBytes, bytes.Size() - found - fromBytes));
    return result;
}

Span<const uint8> Text(const char* text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

// id 파일을 from → to로 바꿔 읽은 결과다. 바꿀 자리가 없으면 시험 자체가 틀렸으므로 REQUIRE한다.
struct Mutated {
    ShaderFormatStatus status;
    ShaderFormatDefinition definition;
};

bool Mutate(const char* id, const char* from, const char* to, Mutated& result) noexcept {
    const Array<uint8> original = ReadConfig(id);
    const Array<uint8> changed = Replace(original, from, to);
    if (changed.Size() == 0) {
        return false;
    }

    result.status = ParseShaderFormatDefinition(changed.View(), id, result.definition);
    return true;
}

bool FailsWith(const char* id, const char* from, const char* to, ShaderFormatErrorCode code,
               const char* path) noexcept {
    Mutated result;
    if (!Mutate(id, from, to, result)) {
        printf("  mutation anchor not found once in %s: %s\n", id, from);
        return false;
    }

    const bool matches = result.status.code == code && strcmp(result.status.path, path) == 0;
    if (!matches) {
        printf("  %s: %s -> got %s at '%s', want %s at '%s'\n", id, from,
               ShaderFormatErrorName(result.status.code), result.status.path,
               ShaderFormatErrorName(code), path);
    }

    return matches;
}

// 저장소 형식 정의 몇 개를 읽어 둔 묶음이다(엔진 컴파일 프로필은 개정 32에서 없앴다).
struct LoadedFormats {
    static constexpr usize Capacity = 8;

    bool Load(std::initializer_list<const char*> ids) noexcept {
        for (const char* id : ids) {
            if (count == Capacity ||
                !LoadShaderFormatDefinition(ShaderFormatDirectory, id, formats[count])) {
                return false;
            }

            ++count;
        }

        return true;
    }

    Span<const ShaderFormatDefinition> Formats() const noexcept {
        return Span<const ShaderFormatDefinition>(formats, count);
    }

    ShaderFormatDefinition formats[Capacity] = {};
    usize count = 0;
};

const ShaderFormatDefinition* Find(const LoadedFormats& loaded, const char* id) noexcept {
    for (const ShaderFormatDefinition& definition : loaded.Formats()) {
        if (definition.Id() == StringView(id)) {
            return &definition;
        }
    }

    return nullptr;
}

struct DirectoryScan {
    usize definitions = 0;
    usize failures = 0;
};

bool VisitConfig(const platform::DirectoryEntry& entry, void* context) noexcept {
    auto& scan = *static_cast<DirectoryScan*>(context);
    const StringView name(entry.name, entry.nameBytes);
    constexpr StringView Extension = ".json";
    if (!name.EndsWith(Extension, StringComparison::CaseSensitive)) {
        return true;
    }

    const StringView id(entry.name, entry.nameBytes - Extension.ByteCount());
    ShaderFormatDefinition definition;
    const ShaderFormatStatus loaded =
        LoadShaderFormatDefinition(ShaderFormatDirectory, id, definition);
    if (!loaded) {
        printf("  %.*s: %s at '%s' line %u\n", static_cast<int>(id.ByteCount()), id.Data(),
               ShaderFormatErrorName(loaded.code), loaded.path, loaded.line);
    }

    scan.failures += loaded ? 0 : 1;
    ++scan.definitions;
    return true;
}

} // namespace

TEST(ShaderFormat_RepositoryDirectoryParses) {
    // 모든 형식 정의(자리만 둔 둘 포함)가 파일 이름 = id로 읽힌다(엔진 컴파일 프로필 파일은 없다).
    DirectoryScan scan;
    REQUIRE(platform::PhysicalFileSystem().EnumerateDirectory(
        ShaderFormatDirectory, strlen(ShaderFormatDirectory), VisitConfig, &scan));
    CHECK(scan.definitions == 8u);
    CHECK(scan.failures == 0u);
}

TEST(ShaderFormat_WindowsFormatsCarryDecisionsAndMeasurements) {
    LoadedFormats profile;
    REQUIRE(profile.Load({"D3D12_SM6", "D3D11_SM5", "VULKAN_SM5", "GLES31_PC", "GLES31_ANDROID"}));
    for (const ShaderFormatDefinition& format : profile.Formats()) {
        CHECK(format.state == ShaderFormatState::Supported);
        CHECK(format.CanProduceOn(ShaderHost::WindowsX64));
    }

    // 결정 3: D3D12 SM 6.6(S2: 이 기계 GPU·WARP는 Agility SDK 없이 6.8).
    const ShaderFormatDefinition* d3d12 = Find(profile, "D3D12_SM6");
    REQUIRE(d3d12 != nullptr);
    CHECK(d3d12->compiler.path == ShaderCompilerPath::DxcDxil);
    CHECK(d3d12->compiler.shaderModel == (ShaderVersion{6, 6}));
    CHECK((d3d12->deviceRequirements & DeviceRequiresD3D12ShaderModel6_6) != 0);
    CHECK((d3d12->features & ShaderFeatureWaveOperations) != 0);
    CHECK(d3d12->compiler.imageFormat == ImageFormatExpansion::Empty);
    // 개정 29(E8): typedUavLoad는 장치 보장 기능이고 D3D12만 장치 요구로 보장한다.
    CHECK((d3d12->features & ShaderFeatureTypedUavLoad) != 0);
    CHECK((d3d12->deviceRequirements & DeviceRequiresD3D12TypedUavLoadAdditionalFormats) != 0);
    CHECK(!IsRuntimeCheckedShaderFeature(ShaderFeatureTypedUavLoad));
    for (const char* id : {"D3D11_SM5", "VULKAN_SM5", "GLES31_PC", "GLES31_ANDROID"}) {
        const ShaderFormatDefinition* other = Find(profile, id);
        REQUIRE(other != nullptr);
        CHECK((other->features & ShaderFeatureTypedUavLoad) == 0);
    }

    CHECK(StringView(ImageFormatName(ImageFormatIndex("r11g11b10f"))) == "r11g11b10f");
    CHECK(StringView(ImageFormatName(IndexNone)) == "unknown");

    // 결정 2: HLSL 2021은 D3D12·Vulkan·Metal 켬, D3D11 끔(D3D11의 2021 셰이더는 SPIRV-Cross 경로).
    const ShaderFormatDefinition* d3d11 = Find(profile, "D3D11_SM5");
    REQUIRE(d3d11 != nullptr);
    CHECK(d3d12->hlsl2021);
    CHECK(!d3d11->hlsl2021);
    CHECK(d3d11->compiler.fallbackPath == ShaderCompilerPath::DxcSpirvHlsl50Fxc);
    CHECK(d3d11->compiler.shaderModel == (ShaderVersion{5, 0}));
    CHECK(!d3d11->CanProduceOn(ShaderHost::MacosArm64));
    CHECK(Find(profile, "VULKAN_SM5")->hlsl2021);

    // SH0: ROCKS_IMAGE_FORMAT은 SPIR-V 경로에서 [[vk::image_format]], ESSL은 r11g11b10f를 받지
    // 않는다.
    const ShaderFormatDefinition* pc = Find(profile, "GLES31_PC");
    const ShaderFormatDefinition* android = Find(profile, "GLES31_ANDROID");
    REQUIRE(pc != nullptr && android != nullptr);
    CHECK(pc->compiler.imageFormat == ImageFormatExpansion::VkImageFormat);
    CHECK((pc->compiler.unsupportedImageFormats & (uint64{1} << ImageFormatIndex("r11g11b10f"))) !=
          0);
    CHECK(pc->compiler.esslVersion == 320u && android->compiler.esslVersion == 320u);
    // S12: 데스크톱 드라이버가 #version 320 es를 받으므로 미리보기도 320이고 클립 공간은
    // glClipControl(확장 요구)로 맞춘다.
    CHECK(!pc->compiler.fixupClipSpace && !pc->compiler.flipVertexY);
    CHECK(android->compiler.fixupClipSpace && android->compiler.flipVertexY);
    CHECK((pc->deviceRequirements & DeviceRequiresGlArbEs32Compatibility) != 0);
    CHECK((pc->deviceRequirements & DeviceRequiresGlArbClipControl) != 0);

    // 상한은 job 형식마다 따로다(7.2절 4항, 개정 13·14·17). 데스크톱 형식은 넓고 GLES·D3D11은
    // 16이다.
    const ShaderFormatDefinition* vulkan = Find(profile, "VULKAN_SM5");
    REQUIRE(vulkan != nullptr);
    CHECK(d3d12->limits.sampledTextures == 128u);
    CHECK(d3d11->limits.sampledTextures == 16u);
    CHECK(vulkan->limits.sampledTextures == 128u);
    CHECK(vulkan->limits.renderTargets == 8u);
    CHECK(pc->limits.sampledTextures == 16u && android->limits.sampledTextures == 16u);
    CHECK(pc->limits.renderTargets == 4u);
    CHECK(pc->limits.interStageVectors == 15u);
    CHECK(pc->limits.uniformVectors[0] == 256u && pc->limits.uniformVectors[1] == 224u &&
          pc->limits.uniformVectors[2] == 128u);
    CHECK(vulkan->limits.uniformVectors[0] == NoShaderLimit);
    for (const ShaderFormatDefinition& format : profile.Formats()) {
        CHECK(format.limits.samplers == 16u);
        CHECK(format.limits.uavStages == ShaderStageCompute);
        CHECK(format.limits.rootConstantBytes == 128u);
    }

    // 결정 12: VS layer·viewport 출력은 D3D12·Vulkan·D3D11(측정 S14: FXC vs_5_0이 받는다)(·Metal)에
    // 런타임 확인 기능으로 있고 GLES에는 없다.
    CHECK((d3d12->features & ShaderFeatureVertexViewportLayerOutput) != 0);
    CHECK((vulkan->features & ShaderFeatureVertexViewportLayerOutput) != 0);
    CHECK((d3d11->features & ShaderFeatureVertexViewportLayerOutput) != 0);
    CHECK((pc->features & ShaderFeatureVertexViewportLayerOutput) == 0);
    CHECK((android->features & ShaderFeatureVertexViewportLayerOutput) == 0);
    CHECK(IsRuntimeCheckedShaderFeature(ShaderFeatureVertexViewportLayerOutput));
    CHECK(!IsRuntimeCheckedShaderFeature(ShaderFeatureCompute));
    CHECK(!IsRuntimeCheckedShaderFeature(ShaderFeatureWaveOperations));
}

TEST(ShaderFormat_MetalUsesSystemMetalTools) {
    LoadedFormats profile;
    REQUIRE(profile.Load({"METAL_SM5", "GLES31_ANDROID"}));
    CHECK(Find(profile, "GLES31_ANDROID")->CanProduceOn(ShaderHost::MacosArm64));
    const ShaderFormatDefinition* metal = Find(profile, "METAL_SM5");
    REQUIRE(metal != nullptr);
    CHECK((metal->features & ShaderFeatureVertexViewportLayerOutput) != 0);
    CHECK(metal->hlsl2021);
    CHECK(metal->compiler.mslVersion == (ShaderVersion{2, 4}));
    // 결정 5: metallib은 Windows에서도 Apple Metal Developer Tools가 있으면 만든다.
    const ShaderHost hosts[] = {ShaderHost::MacosArm64, ShaderHost::WindowsX64};
    for (const ShaderHost host : hosts) {
        const ShaderFormatProducer* producer = metal->Producer(host);
        REQUIRE(producer != nullptr);
        REQUIRE(producer->systemToolCount == 1u);
        CHECK(strcmp(producer->systemTools[0]->name, MetalToolName) == 0);
    }
}

TEST(ShaderFormat_RejectsUnknownProperties) {
    // 최상위·중첩 객체의 모르는 속성, 그 경로에 쓰이지 않는 속성, 자리만 둔 형식의 컴파일 속성.
    CHECK(FailsWith("D3D12_SM6", "\"schema\": 1,", "\"schema\": 1, \"extra\": 0,",
                    ShaderFormatErrorCode::UnknownProperty, "extra"));
    CHECK(FailsWith("D3D12_SM6", "\"samplers\": 16,", "\"samplers\": 16, \"textureUnits\": 4,",
                    ShaderFormatErrorCode::UnknownProperty, "limits.textureUnits"));
    CHECK(FailsWith("D3D12_SM6", "\"shaderModel\": \"6_6\",",
                    "\"shaderModel\": \"6_6\", \"esslVersion\": 320,",
                    ShaderFormatErrorCode::UnknownProperty, "compiler.esslVersion"));
    CHECK(FailsWith("D3D12_SM6", "\"version\": 4 }", "\"version\": 4, \"name\": \"x\" }",
                    ShaderFormatErrorCode::UnknownProperty, "processor.name"));
    // uniformVectors는 GLES 에뮬레이션만의 상한이다.
    CHECK(FailsWith("VULKAN_SM5", "\"rootConstantBytes\": 128",
                    "\"rootConstantBytes\": 128, \"uniformVectors\": { \"vertex\": 1, \"pixel\": "
                    "1, \"compute\": 1 }",
                    ShaderFormatErrorCode::UnknownProperty, "limits.uniformVectors"));
    CHECK(FailsWith("VULKAN_ANDROID", "\"backend\": \"Vulkan\"",
                    "\"backend\": \"Vulkan\", \"tools\": []",
                    ShaderFormatErrorCode::UnknownProperty, "tools"));
}

TEST(ShaderFormat_ToolsMustBeInToolTable) {
    CHECK(FailsWith("D3D12_SM6", "[\"DirectXShaderCompiler\"]",
                    "[\"DirectXShaderCompiler\", \"NoSuchTool\"]",
                    ShaderFormatErrorCode::UnknownTool, "tools"));
    // 대소문자를 구분한다.
    CHECK(FailsWith("D3D12_SM6", "[\"DirectXShaderCompiler\"]", "[\"directxshadercompiler\"]",
                    ShaderFormatErrorCode::UnknownTool, "tools"));
    CHECK(FailsWith("D3D12_SM6", "[\"DirectXShaderCompiler\"]",
                    "[\"DirectXShaderCompiler\", \"DirectXShaderCompiler\"]",
                    ShaderFormatErrorCode::DuplicateValue, "tools"));
    // 경로가 쓰는 도구가 목록에 없다(ESSL 경로는 SPIRV-Cross가 필요하다).
    CHECK(FailsWith("GLES31_PC", "\"SpirvTools\", \"SpirvCross\"]", "\"SpirvTools\"]",
                    ShaderFormatErrorCode::InvalidValue, "tools"));
    // system 도구는 형식 도구 목록에도 있어야 한다.
    CHECK(FailsWith("METAL_SM5", "\"SpirvCross\", \"MetalToolchain\"]", "\"SpirvCross\"]",
                    ShaderFormatErrorCode::InvalidValue, "producers.systemTools"));

    // 표 인자로 다른 도구 표를 줄 수 있다(빈 표면 모든 도구가 모른다).
    ShaderFormatDefinition definition;
    const Array<uint8> bytes = ReadConfig("D3D12_SM6");
    const ShaderFormatStatus empty = ParseShaderFormatDefinition(
        bytes.View(), "D3D12_SM6", definition, Span<const ShaderTool>());
    CHECK(empty.code == ShaderFormatErrorCode::UnknownTool);
}

TEST(ShaderFormat_RejectsInconsistentValues) {
    CHECK(FailsWith("D3D12_SM6", "\"id\": \"D3D12_SM6\"", "\"id\": \"D3D12_SM7\"",
                    ShaderFormatErrorCode::IdMismatch, "id"));
    CHECK(FailsWith("D3D12_SM6", "\"schema\": 1", "\"schema\": 2",
                    ShaderFormatErrorCode::UnsupportedSchema, "schema"));
    CHECK(FailsWith("D3D12_SM6", "\"backend\": \"D3D12\"", "\"backend\": \"D3D13\"",
                    ShaderFormatErrorCode::InvalidValue, "backend"));
    // backend와 경로는 2절 형식 표대로 짝이다.
    CHECK(FailsWith("D3D12_SM6", "\"backend\": \"D3D12\"", "\"backend\": \"D3D11\"",
                    ShaderFormatErrorCode::InvalidValue, "compiler.path"));
    // SH0: DXIL 경로에 [[vk::image_format]] 전개를 두면 DXC가 경고를 내므로 거부한다.
    CHECK(FailsWith("D3D12_SM6", "\"imageFormat\": \"Empty\"", "\"imageFormat\": \"VkImageFormat\"",
                    ShaderFormatErrorCode::InvalidValue, "compiler.imageFormat"));
    CHECK(FailsWith("D3D12_SM6", "\"shaderModel\": \"6_6\"", "\"shaderModel\": \"5_1\"",
                    ShaderFormatErrorCode::InvalidValue, "compiler.shaderModel"));
    CHECK(FailsWith("D3D12_SM6", "\"shaderModel\": \"6_6\"", "\"shaderModel\": \"6.6\"",
                    ShaderFormatErrorCode::InvalidValue, "compiler.shaderModel"));
    CHECK(FailsWith("GLES31_PC", "\"esslVersion\": 320", "\"esslVersion\": 310",
                    ShaderFormatErrorCode::InvalidValue, "compiler.esslVersion"));
    CHECK(FailsWith("GLES31_PC", "\"esslVersion\": 320", "\"esslVersion\": \"320\"",
                    ShaderFormatErrorCode::WrongType, "compiler.esslVersion"));
    CHECK(FailsWith("GLES31_PC", "\"fixupClipSpace\": false,", "",
                    ShaderFormatErrorCode::MissingProperty, "compiler.fixupClipSpace"));
    CHECK(FailsWith("GLES31_PC", "[\"r11g11b10f\"]", "[\"r11g11b10\"]",
                    ShaderFormatErrorCode::InvalidValue, "compiler.unsupportedImageFormats"));
    CHECK(FailsWith("D3D12_SM6", "\"rootConstantBytes\": 128", "\"rootConstantBytes\": 132",
                    ShaderFormatErrorCode::InvalidValue, "limits.rootConstantBytes"));
    CHECK(FailsWith("D3D12_SM6", "\"rootConstantBytes\": 128", "\"rootConstantBytes\": 126",
                    ShaderFormatErrorCode::InvalidValue, "limits.rootConstantBytes"));
    CHECK(FailsWith("D3D12_SM6", "\"renderTargets\": 8", "\"renderTargets\": 9",
                    ShaderFormatErrorCode::InvalidValue, "limits.renderTargets"));
    CHECK(FailsWith("D3D12_SM6", "\"stages\": [\"vertex\", \"pixel\", \"compute\"]",
                    "\"stages\": [\"vertex\", \"vertex\"]", ShaderFormatErrorCode::DuplicateValue,
                    "stages"));
    CHECK(FailsWith("D3D12_SM6", "\"stages\": [\"vertex\", \"pixel\", \"compute\"]",
                    "\"stages\": [\"vertex\", \"pixel\"]", ShaderFormatErrorCode::InvalidValue,
                    "limits.uavStages"));
    CHECK(FailsWith("D3D12_SM6", "[{ \"host\": \"windows-x64\" }]",
                    "[{ \"host\": \"windows-x64\" }, { \"host\": \"windows-x64\" }]",
                    ShaderFormatErrorCode::DuplicateValue, "producers.host"));
    CHECK(FailsWith("D3D12_SM6", "[{ \"host\": \"windows-x64\" }]", "[{ \"host\": \"linux-x64\" }]",
                    ShaderFormatErrorCode::InvalidValue, "producers.host"));

    ShaderFormatDefinition definition;
    const ShaderFormatStatus broken =
        ParseShaderFormatDefinition(Text("{ \"schema\": 1, "), "", definition);
    CHECK(broken.code == ShaderFormatErrorCode::InvalidJson && broken.line == 1u);
    const ShaderFormatStatus missing =
        LoadShaderFormatDefinition(ShaderFormatDirectory, "NO_SUCH", definition);
    CHECK(missing.code == ShaderFormatErrorCode::ReadFailed);
    // id는 파일 이름이 되므로 경로 구분자를 받지 않는다.
    const ShaderFormatStatus traversal =
        LoadShaderFormatDefinition(ShaderFormatDirectory, "../D3D12_SM6", definition);
    CHECK(traversal.code == ShaderFormatErrorCode::InvalidValue);
}

TEST(ShaderFormat_DefinitionDigestFollowsFileBytes) {
    // job 키 3항 입력: 같은 byte는 같은 SHA-256, 값 하나·공백 하나만 바뀌어도 다른 SHA-256이다.
    const Array<uint8> original = ReadConfig("VULKAN_SM5");
    ShaderFormatDefinition first;
    ShaderFormatDefinition again;
    REQUIRE(ParseShaderFormatDefinition(original.View(), "VULKAN_SM5", first));
    REQUIRE(ParseShaderFormatDefinition(original.View(), "VULKAN_SM5", again));
    CHECK(first.sha256 == again.sha256);
    CHECK(first.sha256 == Sha256::Hash(original.View()));

    const char* const changes[][2] = {
        {"\"storageImages\": 4,", "\"storageImages\": 3,"},
        {"\"hlsl2021\": true", "\"hlsl2021\": false"},
        {"\"version\": 4 }", "\"version\": 5 }"},
        {"\"bindingConvention\": 1,", "\"bindingConvention\":  1,"},
    };
    for (const auto& change : changes) {
        const Array<uint8> changed = Replace(original, change[0], change[1]);
        REQUIRE(changed.Size() > 0);
        ShaderFormatDefinition definition;
        REQUIRE(ParseShaderFormatDefinition(changed.View(), "VULKAN_SM5", definition));
        CHECK(!(definition.sha256 == first.sha256));
    }
}
} // namespace shader_format_definition_tests_file
