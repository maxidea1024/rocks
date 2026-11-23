#include "ShaderSourceTestSupport.h"

#include <Core/Encoding/Sha256.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <ShaderCompiler/ShaderPreprocessDefines.h>
#include <Base/InitializerList.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace shader_preprocess_defines_tests_file {

using namespace rocks::shader;
using namespace rocks::shader_source_test;

// 전처리 정의 조립(Docs/Design/ShaderSystem.ko.md 11.1절 SC10)과 저장소 Engine/Shaders/Public/
// Platform.rsh 시험이다. 형식 정의는 저장소 파일(Engine/Config/ShaderFormats)을 읽는다(시험은
// 저장소 루트에서 돈다).
namespace {

struct ExpectedDefine {
    const char* name;
    const char* value;
};

bool LoadFormat(const char* id, ShaderFormatDefinition& format) noexcept {
    const ShaderFormatStatus loaded = LoadShaderFormatDefinition(ShaderFormatDirectory, id, format);
    if (!loaded) {
        fprintf(stderr, "format %s: %s %s\n", id, ShaderFormatErrorName(loaded.code), loaded.path);
    }

    return static_cast<bool>(loaded);
}

// 조립 결과가 expected(이름순)와 같은지 본다.
bool SameDefines(const ShaderDefineSet& set, Span<const ExpectedDefine> expected) noexcept {
    const Span<const ShaderDefine> defines = set.Defines();
    bool same = defines.Size() == expected.Size();
    for (usize index = 0; same && index < defines.Size(); ++index) {
        same = defines[index].name == StringView(expected[index].name) &&
               defines[index].value == StringView(expected[index].value);
    }

    if (!same) {
        for (const ShaderDefine& define : defines) {
            fprintf(stderr, "  %.*s=%.*s\n", static_cast<int>(define.name.ByteCount()),
                    define.name.Data(), static_cast<int>(define.value.ByteCount()),
                    define.value.Data());
        }
    }

    return same;
}

// 저장소 Engine/Shaders를 /Engine으로, fixture를 /Plugin/Test로 mount한 경우다.
struct PlatformCase {
    PlatformCase() noexcept
        : tree(TestAllocator()), shader(TestAllocator()), defines(TestAllocator()) {
    }

    bool Make(const char* tag, StringView main) noexcept {
        char engine[FixturePathBytes];
        char plugin[FixturePathBytes];
        return fixture.Make(tag) && fixture.Write("Plugin/Main.rsf", main) &&
               fixture.Child("Plugin", plugin, sizeof(plugin)) &&
               RepositoryShaderDirectory(engine, sizeof(engine)) &&
               static_cast<bool>(tree.Mount("/Engine", engine)) &&
               static_cast<bool>(tree.Mount(FixturePlugin, plugin));
    }

    ShaderSourceStatus Run(const char* formatId, uint32 stage) noexcept {
        ShaderFormatDefinition format;
        if (!LoadFormat(formatId, format)) {
            ShaderSourceStatus failed;
            failed.code = ShaderSourceError::InvalidArgument;
            return failed;
        }

        if (const ShaderSourceStatus built =
                BuildShaderPreprocessDefines(format, stage, format.hlsl2021, {}, defines);
            !built) {
            return built;
        }

        return PreprocessShader(tree, Entry, defines.Defines(), shader);
    }

    bool Contains(const char* text) const noexcept {
        const bool found =
            shader.text.View().Find(StringView(text), StringComparison::CaseSensitive) !=
            StringView::NotFound;
        if (!found) {
            fprintf(stderr, "missing '%s' in:\n%.*s\n", text,
                    static_cast<int>(shader.text.ByteCount()), shader.text.Data());
        }

        return found;
    }

    Fixture fixture;
    ShaderSourceTree tree;
    PreprocessedShader shader;
    ShaderDefineSet defines;
};

constexpr char PlatformProbe[] = "ROCKS_IMAGE_FORMAT(rgba8) RWTexture2D<float4> Output;\n"
                                 "int spirv = ROCKS_SPIRV;\n"
                                 "#if ROCKS_SHADER_MODEL_MAJOR >= 6\n"
                                 "int sm6;\n"
                                 "#endif\n"
                                 "#if ROCKS_COMPILER_FXC\n"
                                 "int fxc;\n"
                                 "#endif\n"
                                 "#ifdef ROCKS_COMPUTE_SHADER\n"
                                 "int compute;\n"
                                 "#endif\n";

} // namespace

TEST(ShaderPreprocessDefines_AssemblesFormatDefines) {
    ShaderFormatDefinition d3d12;
    ShaderFormatDefinition d3d11;
    ShaderFormatDefinition vulkan;
    ShaderFormatDefinition metal;
    ShaderFormatDefinition gles;
    REQUIRE(LoadFormat("D3D12_SM6", d3d12));
    REQUIRE(LoadFormat("D3D11_SM5", d3d11));
    REQUIRE(LoadFormat("VULKAN_SM5", vulkan));
    REQUIRE(LoadFormat("METAL_SM5", metal));
    REQUIRE(LoadFormat("GLES31_PC", gles));
    ShaderDefineSet set(TestAllocator());

    REQUIRE(BuildShaderPreprocessDefines(d3d12, ShaderStagePixel, true, {}, set));
    const ExpectedDefine d3d12Pixel[] = {
        {"ROCKS_BACKEND_D3D12", "1"},      {"ROCKS_COMPILER_DXC", "1"},
        {"ROCKS_FORMAT_D3D12_SM6", "1"},   {"ROCKS_HLSL2021", "1"},
        {"ROCKS_PIXEL_SHADER", "1"},       {"ROCKS_SHADER_MODEL_MAJOR", "6"},
        {"ROCKS_SHADER_MODEL_MINOR", "6"},
    };
    CHECK(SameDefines(set, d3d12Pixel));

    // D3D11_SM5: 1차 경로가 FXC이므로 HLSL 2021(대체 경로)이어도 FXC 정의다.
    REQUIRE(BuildShaderPreprocessDefines(d3d11, ShaderStageVertex, true, {}, set));
    const ExpectedDefine d3d11Vertex[] = {
        {"ROCKS_BACKEND_D3D11", "1"},      {"ROCKS_COMPILER_FXC", "1"},
        {"ROCKS_FORMAT_D3D11_SM5", "1"},   {"ROCKS_HLSL2021", "1"},
        {"ROCKS_SHADER_MODEL_MAJOR", "5"}, {"ROCKS_SHADER_MODEL_MINOR", "0"},
        {"ROCKS_VERTEX_SHADER", "1"},
    };
    CHECK(SameDefines(set, d3d11Vertex));

    // job 정의는 함께 이름순으로 놓인다.
    const ShaderDefine permutation[] = {{"USE_FOG", "1"}, {"A_FIRST", ""}, {"QUALITY", "(2 + 1)"}};
    REQUIRE(BuildShaderPreprocessDefines(vulkan, ShaderStageCompute, false, permutation, set));
    const ExpectedDefine vulkanCompute[] = {
        {"A_FIRST", ""},
        {"QUALITY", "(2 + 1)"},
        {"ROCKS_BACKEND_VULKAN", "1"},
        {"ROCKS_COMPILER_DXC", "1"},
        {"ROCKS_COMPUTE_SHADER", "1"},
        {"ROCKS_FORMAT_VULKAN_SM5", "1"},
        {"ROCKS_HLSL2021", "0"},
        {"ROCKS_SHADER_MODEL_MAJOR", "6"},
        {"ROCKS_SHADER_MODEL_MINOR", "0"},
        {"USE_FOG", "1"},
    };
    CHECK(SameDefines(set, vulkanCompute));
    bool found = false;
    CHECK(set.Find("QUALITY", found) == StringView("(2 + 1)") && found);
    (void)set.Find("MISSING", found);
    CHECK(!found);

    REQUIRE(BuildShaderPreprocessDefines(metal, ShaderStagePixel, true, {}, set));
    CHECK(set.Find("ROCKS_BACKEND_METAL", found) == StringView("1") && found);
    REQUIRE(BuildShaderPreprocessDefines(gles, ShaderStagePixel, true, {}, set));
    CHECK(set.Find("ROCKS_BACKEND_GLES", found) == StringView("1") && found);
    CHECK(set.Find("ROCKS_FORMAT_GLES31_PC", found) == StringView("1") && found);
}

TEST(ShaderPreprocessDefines_RejectsInvalidJobDefines) {
    ShaderFormatDefinition format;
    REQUIRE(LoadFormat("D3D12_SM6", format));
    ShaderDefineSet set(TestAllocator());
    const ShaderDefine reserved[] = {{"ROCKS_FORMAT_X", "1"}};
    ShaderSourceStatus status =
        BuildShaderPreprocessDefines(format, ShaderStagePixel, true, reserved, set);
    CHECK(status.code == ShaderSourceError::ReservedDefine);
    CHECK(strstr(status.message, "ROCKS_FORMAT_X") != nullptr);
    CHECK(set.Defines().IsEmpty());
    const ShaderDefine compiler[] = {{"__SHADER_TARGET_MAJOR", "6"}};
    CHECK(BuildShaderPreprocessDefines(format, ShaderStagePixel, true, compiler, set).code ==
          ShaderSourceError::ReservedDefine);
    const ShaderDefine duplicate[] = {{"A", "1"}, {"B", "1"}, {"A", "2"}};
    CHECK(BuildShaderPreprocessDefines(format, ShaderStagePixel, true, duplicate, set).code ==
          ShaderSourceError::DuplicateDefine);
    const ShaderDefine badName[] = {{"9LIVES", "1"}};
    CHECK(BuildShaderPreprocessDefines(format, ShaderStagePixel, true, badName, set).code ==
          ShaderSourceError::InvalidDefine);
    const ShaderDefine badValue[] = {{"A", "1\r\n"}};
    CHECK(BuildShaderPreprocessDefines(format, ShaderStagePixel, true, badValue, set).code ==
          ShaderSourceError::InvalidDefine);
    // 대소문자가 다른 rocks_는 예약이 아니다.
    const ShaderDefine lower[] = {{"rocks_value", "1"}};
    CHECK(BuildShaderPreprocessDefines(format, ShaderStagePixel, true, lower, set));

    // 단계: 비트 하나, 형식이 받는 단계, Supported 형식만.
    for (const uint32 stage : {0u, ShaderStageVertex | ShaderStagePixel, 8u}) {
        CHECK(BuildShaderPreprocessDefines(format, stage, true, {}, set).code ==
              ShaderSourceError::UnsupportedStage);
    }

    ShaderFormatDefinition vertexOnly = format;
    vertexOnly.stages = ShaderStageVertex;
    CHECK(BuildShaderPreprocessDefines(vertexOnly, ShaderStageCompute, true, {}, set).code ==
          ShaderSourceError::UnsupportedStage);
    ShaderFormatDefinition placeholder;
    REQUIRE(LoadFormat("VULKAN_ANDROID", placeholder));
    CHECK(BuildShaderPreprocessDefines(placeholder, ShaderStagePixel, true, {}, set).code ==
          ShaderSourceError::UnsupportedStage);
}

TEST(ShaderPreprocessDefines_PlatformHeaderExpandsFormatMacros) {
    // SPIR-V 형식: ROCKS_IMAGE_FORMAT은 [[vk::image_format("…")]], ROCKS_SPIRV는 1.
    PlatformCase vulkan;
    REQUIRE(vulkan.Make("platvk", PlatformProbe));
    ShaderSourceStatus status = vulkan.Run("VULKAN_SM5", ShaderStageCompute);
    PrintStatus("VULKAN_SM5", status);
    REQUIRE(status);
    CHECK(vulkan.Contains("#line 1 \"/Plugin/Test/Main.rsf\"\n"
                          "[[vk::image_format(\"rgba8\")]] RWTexture2D<float4> Output;\n"
                          "int spirv = 1;\n"));
    CHECK(vulkan.Contains("int sm6;"));
    CHECK(vulkan.Contains("int compute;"));
    CHECK(!vulkan.shader.text.View().Contains("int fxc;", StringComparison::CaseSensitive));
    // Platform.rsh는 정의·조건만 있어 출력 토큰이 없다.
    CHECK(!vulkan.shader.text.View().Contains("Platform.rsh", StringComparison::CaseSensitive));

    // D3D 형식: 빈 전개, ROCKS_SPIRV는 0.
    PlatformCase d3d12;
    REQUIRE(d3d12.Make("platdx12", PlatformProbe));
    status = d3d12.Run("D3D12_SM6", ShaderStagePixel);
    PrintStatus("D3D12_SM6", status);
    REQUIRE(status);
    CHECK(d3d12.Contains("#line 1 \"/Plugin/Test/Main.rsf\"\n"
                         " RWTexture2D<float4> Output;\n"
                         "int spirv = 0;\n"));
    CHECK(d3d12.Contains("int sm6;"));
    CHECK(!d3d12.shader.text.View().Contains("vk::", StringComparison::CaseSensitive));

    PlatformCase d3d11;
    REQUIRE(d3d11.Make("platdx11", PlatformProbe));
    status = d3d11.Run("D3D11_SM5", ShaderStageVertex);
    PrintStatus("D3D11_SM5", status);
    REQUIRE(status);
    CHECK(d3d11.Contains("int spirv = 0;"));
    CHECK(d3d11.Contains("int fxc;"));
    CHECK(!d3d11.shader.text.View().Contains("int sm6;", StringComparison::CaseSensitive));

    for (const char* format : {"METAL_SM5", "GLES31_PC", "GLES31_ANDROID"}) {
        PlatformCase spirv;
        REQUIRE(spirv.Make("platspirv", PlatformProbe));
        status = spirv.Run(format, ShaderStagePixel);
        PrintStatus(format, status);
        REQUIRE(status);
        CHECK(spirv.Contains("[[vk::image_format(\"rgba8\")]] RWTexture2D<float4> Output;"));
        CHECK(spirv.Contains("int spirv = 1;"));
    }

    // 폐포에 저장소 Platform.rsh가 그 파일 byte의 SHA-256으로 들어간다.
    Array<uint8> bytes(TestAllocator());
    REQUIRE(vulkan.tree.Load(ShaderPlatformHeaderPath, bytes));
    bool listed = false;
    for (const ShaderIncludeFile& file : vulkan.shader.includes) {
        if (file.virtualPath.View() == StringView(ShaderPlatformHeaderPath)) {
            listed = file.sha256 == Sha256::Hash(bytes.View());
        }
    }

    CHECK(listed);
}

TEST(ShaderPreprocessDefines_PlatformHeaderRequiresAssembledDefines) {
    // 정의 조립 없이 전처리하면 Platform.rsh의 #error가 막는다.
    PlatformCase test;
    REQUIRE(test.Make("platnone", "int a;\n"));
    ShaderSourceStatus status = PreprocessShader(test.tree, Entry, {}, test.shader);
    CHECK(status.code == ShaderSourceError::ErrorDirective);
    CHECK(strcmp(status.path, ShaderPlatformHeaderPath) == 0);

    // backend가 둘이면 막는다.
    const ShaderDefine twoBackends[] = {
        {"ROCKS_SHADER_MODEL_MAJOR", "6"}, {"ROCKS_SHADER_MODEL_MINOR", "0"},
        {"ROCKS_HLSL2021", "1"},           {"ROCKS_BACKEND_D3D12", "1"},
        {"ROCKS_BACKEND_VULKAN", "1"},     {"ROCKS_COMPILER_DXC", "1"},
        {"ROCKS_PIXEL_SHADER", "1"}};
    status = PreprocessShader(test.tree, Entry, twoBackends, test.shader);
    CHECK(status.code == ShaderSourceError::ErrorDirective);
    CHECK(strstr(status.message, "ROCKS_BACKEND") != nullptr);
}

} // namespace shader_preprocess_defines_tests_file
