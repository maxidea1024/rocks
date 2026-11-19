#include <ShaderToolTable/ShaderToolTable.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;
namespace shader_tool_table_tests_file {

using namespace rocks::shader;

// 생성 도구 표(Engine 루트 binary manifest → GeneratedSources/ShaderToolTable)의 내용과 불변식을
// 검사한다(Docs/Design/ShaderSystem.ko.md 9절). 값 대조는 manifest에 고정한 SHA-256이다.
namespace {

bool IsLowerHex(const char* text, usize bytes) noexcept {
    if (text == nullptr || strlen(text) != bytes) {
        return false;
    }

    for (usize index = 0; index < bytes; ++index) {
        const char value = text[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }

    return true;
}

} // namespace

TEST(ShaderToolTable_ListsEveryBinaryToolInNameOrder) {
    const Span<const ShaderTool> tools = ShaderTools();
    REQUIRE(tools.Size() == 5u);
    for (usize index = 1; index < tools.Size(); ++index) {
        CHECK(strcmp(tools[index - 1].name, tools[index].name) < 0);
    }

    const char* const names[] = {DxcToolName, FxcToolName, SpirvCrossToolName, SpirvToolsToolName,
                                 MetalToolName};
    for (const char* name : names) {
        const ShaderTool* tool = FindShaderTool(name);
        REQUIRE(tool != nullptr);
        CHECK(strcmp(tool->name, name) == 0);
        CHECK(tool->version != nullptr && tool->version[0] != '\0');
    }

    CHECK(FindShaderTool("directxshadercompiler") == nullptr); // 대소문자를 구분한다.
    CHECK(FindShaderTool("") == nullptr);
    CHECK(FindShaderTool(DxcToolName)->origin == ShaderToolOrigin::Build);
    CHECK(FindShaderTool(FxcToolName)->origin == ShaderToolOrigin::Sdk);
    CHECK(FindShaderTool(MetalToolName)->origin == ShaderToolOrigin::System);
}

TEST(ShaderToolTable_EntriesFollowStoreRules) {
    for (const ShaderTool& tool : ShaderTools()) {
        if (tool.fileCount == 0) {
            // 이 호스트 파일이 없다(다른 호스트 전용 도구·Android 대상).
            CHECK(tool.files == nullptr && tool.storeKey == nullptr);
            continue;
        }

        REQUIRE(tool.files != nullptr);
        if (tool.origin == ShaderToolOrigin::System) {
            // 시스템 도구는 저장소에 두지 않고 SHA를 고정하지 않는다.
            CHECK(tool.storeKey == nullptr);
            for (usize index = 0; index < tool.fileCount; ++index) {
                CHECK(!tool.files[index].hasDigest && tool.files[index].size == 0);
                CHECK(tool.files[index].role == ShaderToolFileRole::Tool);
            }

            continue;
        }

        CHECK(IsLowerHex(tool.storeKey, Sha256Digest::HexBytes));
        CHECK(FindShaderToolFile(tool, ShaderToolFileRole::Runtime) != nullptr);
        for (usize index = 0; index < tool.fileCount; ++index) {
            CHECK(tool.files[index].hasDigest && tool.files[index].path != nullptr);
            CHECK(strchr(tool.files[index].path, '\\') == nullptr);
        }

        if (tool.origin == ShaderToolOrigin::Sdk) {
            // SDK 출처는 파일 하나이고 저장소 디렉터리 이름이 그 파일의 SHA-256이다.
            REQUIRE(tool.fileCount == 1u);
            char hex[Sha256Digest::HexBytes + 1];
            tool.files[0].sha256.ToHex(hex);
            CHECK(strcmp(hex, tool.storeKey) == 0);
        }
    }
}

TEST(ShaderToolTable_WindowsHostMatchesManifest) {
    if (strcmp(ShaderToolHostKey(), "windows-x64") != 0) {
        printf("[SKIP] ShaderToolTable_WindowsHostMatchesManifest: host key is '%s'\n",
               ShaderToolHostKey());
        return;
    }

    // Engine/Source/ThirdParty/DirectXShaderCompiler/ThirdParty.json hosts.windows-x64.
    const ShaderTool* dxc = FindShaderTool(DxcToolName);
    REQUIRE(dxc != nullptr && dxc->fileCount != 0);
    CHECK(strcmp(dxc->storeKey,
                 "ca36ac720f580ecccb1f12d548723ab046df6fd92e0e53be69f3f6812c99cd22") == 0);
    const ShaderToolFile* runtime = FindShaderToolFile(*dxc, ShaderToolFileRole::Runtime);
    REQUIRE(runtime != nullptr);
    CHECK(strcmp(runtime->path, "bin/dxcompiler.dll") == 0);
    CHECK(runtime->size == 21943296u);
    char hex[Sha256Digest::HexBytes + 1];
    runtime->sha256.ToHex(hex);
    CHECK(strcmp(hex, "a0f75767be8ec0ca732af29668524707b6ccee78ec45547e163267d82899783b") == 0);
    CHECK(FindShaderToolFile(*dxc, ShaderToolFileRole::Symbols) != nullptr);
    CHECK(FindShaderToolFile(*dxc, ShaderToolFileRole::Redist) == nullptr);

    const ShaderTool* fxc = FindShaderTool(FxcToolName);
    REQUIRE(fxc != nullptr && fxc->fileCount == 1u);
    CHECK(strcmp(fxc->files[0].path, "d3dcompiler_47.dll") == 0);

    // Apple 도구는 macOS 전용이라 Windows 표에는 파일이 없다.
    CHECK(FindShaderTool(MetalToolName)->fileCount == 0u);
}
} // namespace shader_tool_table_tests_file
