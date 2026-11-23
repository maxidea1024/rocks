#include "ShaderSourceTestSupport.h"

#include <Core/Text/String.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace shader_source_tree_tests_file {

using namespace rocks::shader;
using namespace rocks::shader_source_test;

// 셰이더 소스 트리(Docs/Design/ShaderSystem.ko.md 11.1절 SC2)의 가상 경로·mount 규칙 시험이다.
// 음성 대조는 규칙 하나씩만 어긴 입력이다.
namespace {

bool IsError(const ShaderSourceStatus& status, ShaderSourceError code) noexcept {
    return status.code == code;
}

} // namespace

TEST(ShaderSourceTree_AcceptsValidVirtualPaths) {
    const char* const valid[] = {
        "/Engine/Public/Platform.rsh",    "/Engine/Private/Sample.rsf",
        "/Engine/Shared/BindingSlots.h",  "/Plugin/Water/Private/Wave.rsf",
        "/Plugin/My-Plugin_2/A.b.rsh",    "/Project/Shaders/Deep/Nested/X.rsf",
        "/Plugin/Water/Shared/Common.h",  "/Project/Shared/Types.h",
        "/Engine/Generated/Material.rsh", "/Engine/Public/SharedHelpers.rsh",
    };
    for (const char* path : valid) {
        const ShaderSourceStatus status = ValidateShaderVirtualPath(path);
        PrintStatus(path, status);
        CHECK(status);
    }
}

TEST(ShaderSourceTree_RejectsInvalidVirtualPaths) {
    // 성분·문자·루트 규칙(InvalidPath).
    const char* const invalidPaths[] = {
        "",
        "Engine/Public/A.rsh",             // '/'로 시작하지 않는다
        "/Engine/Public/",                 // 파일 이름이 없다
        "/Engine//A.rsh",                  // 빈 성분
        "/Engine/./A.rsh",                 // '.'
        "/Engine/Public/../A.rsh",         // '..'
        "/Engine/Public\\A.rsh",           // '\'
        "/Engine/Pu blic/A.rsh",           // 공백
        "/Engine/Public/\xEC\x85\xB0.rsh", // ASCII 밖
        "/Other/A.rsh",                    // 모르는 루트
        "/Plugin/A.rsh",                   // 플러그인 이름 없음
        "/Plugin/Bad.Name/A.rsh",          // 플러그인 이름에 '.'
        "/Engine",                         // 루트만
        "/Plugin/Water",                   // 루트만
    };
    for (const char* path : invalidPaths) {
        const ShaderSourceStatus status = ValidateShaderVirtualPath(path);
        if (!IsError(status, ShaderSourceError::InvalidPath)) {
            fprintf(stderr, "expected InvalidPath: '%s' -> %s\n", path,
                    ShaderSourceErrorName(status.code));
        }

        CHECK(IsError(status, ShaderSourceError::InvalidPath));
    }

    // 길이 경계: MaxShaderVirtualPathBytes - 1 byte는 받고 그 이상은 거부한다.
    char path[MaxShaderVirtualPathBytes + 1];
    const char prefix[] = "/Engine/";
    const char suffix[] = ".rsh";
    const usize fill = MaxShaderVirtualPathBytes - 1 - (sizeof(prefix) - 1) - (sizeof(suffix) - 1);
    memcpy(path, prefix, sizeof(prefix) - 1);
    memset(path + sizeof(prefix) - 1, 'a', fill);
    memcpy(path + sizeof(prefix) - 1 + fill, suffix, sizeof(suffix));
    CHECK(strlen(path) == MaxShaderVirtualPathBytes - 1);
    CHECK(ValidateShaderVirtualPath(path));
    memset(path + sizeof(prefix) - 1, 'a', fill + 1);
    memcpy(path + sizeof(prefix) - 1 + fill + 1, suffix, sizeof(suffix));
    CHECK(IsError(ValidateShaderVirtualPath(path), ShaderSourceError::InvalidPath));

    // 확장자 규칙(InvalidExtension): Shared 밖은 .rsf·.rsh, Shared 안은 .h만.
    const char* const invalidExtensions[] = {
        "/Engine/Public/A.h",   "/Engine/Public/A.usf",       "/Engine/Public/A",
        "/Engine/Shared/A.rsh", "/Plugin/Water/Shared/A.rsf", "/Project/Shared/A.hlsl",
        "/Engine/Public/A.RSH",
    };
    for (const char* invalid : invalidExtensions) {
        const ShaderSourceStatus status = ValidateShaderVirtualPath(invalid);
        if (!IsError(status, ShaderSourceError::InvalidExtension)) {
            fprintf(stderr, "expected InvalidExtension: '%s' -> %s\n", invalid,
                    ShaderSourceErrorName(status.code));
        }

        CHECK(IsError(status, ShaderSourceError::InvalidExtension));
    }
}

TEST(ShaderSourceTree_ValidatesVirtualDirectories) {
    CHECK(ValidateShaderVirtualDirectory("/Engine"));
    CHECK(ValidateShaderVirtualDirectory("/Project"));
    CHECK(ValidateShaderVirtualDirectory("/Plugin/Water_2-x"));
    const char* const invalid[] = {"",          "Engine",      "/Engine/",    "/Engine/Public",
                                   "/Plugin",   "/Plugin/",    "/Plugin/a.b", "/Plugin/A/B",
                                   "/Projects", "/Plugin/A B", "/Plugin/A/",  "/engine"};
    for (const char* directory : invalid) {
        CHECK(IsError(ValidateShaderVirtualDirectory(directory), ShaderSourceError::InvalidMount));
    }

    // 플러그인 이름 길이 경계.
    char name[sizeof(PluginShaderVirtualPrefix) + MaxShaderPluginNameBytes + 1];
    memcpy(name, PluginShaderVirtualPrefix, sizeof(PluginShaderVirtualPrefix) - 1);
    memset(name + sizeof(PluginShaderVirtualPrefix) - 1, 'p', MaxShaderPluginNameBytes);
    name[sizeof(PluginShaderVirtualPrefix) - 1 + MaxShaderPluginNameBytes] = '\0';
    CHECK(ValidateShaderVirtualDirectory(name));
    name[sizeof(PluginShaderVirtualPrefix) - 1 + MaxShaderPluginNameBytes] = 'p';
    name[sizeof(PluginShaderVirtualPrefix) + MaxShaderPluginNameBytes] = '\0';
    CHECK(IsError(ValidateShaderVirtualDirectory(name), ShaderSourceError::InvalidMount));
}

TEST(ShaderSourceTree_MountRules) {
    Fixture fixture;
    REQUIRE(fixture.Make("mount"));
    REQUIRE(fixture.MakeDirectory("A"));
    REQUIRE(fixture.MakeDirectory("B"));
    REQUIRE(fixture.MakeDirectory("WithGenerated/Generated"));
    char a[FixturePathBytes];
    char b[FixturePathBytes];
    char generated[FixturePathBytes];
    char missing[FixturePathBytes];
    char file[FixturePathBytes];
    REQUIRE(fixture.Child("A", a, sizeof(a)));
    REQUIRE(fixture.Child("B", b, sizeof(b)));
    REQUIRE(fixture.Child("WithGenerated", generated, sizeof(generated)));
    REQUIRE(fixture.Child("Missing", missing, sizeof(missing)));
    REQUIRE(fixture.Write("File.rsh", "int x;\n"));
    REQUIRE(fixture.Child("File.rsh", file, sizeof(file)));

    ShaderSourceTree tree(TestAllocator());
    CHECK(IsError(tree.Mount("/Engine/", a), ShaderSourceError::InvalidMount));
    CHECK(IsError(tree.Mount("/Plugin/a.b", a), ShaderSourceError::InvalidMount));
    CHECK(IsError(tree.Mount("/Plugin/Test", "relative/dir"),
                  ShaderSourceError::MountDirectoryNotFound));
    CHECK(IsError(tree.Mount("/Plugin/Test", missing), ShaderSourceError::MountDirectoryNotFound));
    CHECK(IsError(tree.Mount("/Plugin/Test", file), ShaderSourceError::MountDirectoryNotFound));
    const ShaderSourceStatus withGenerated = tree.Mount("/Plugin/Test", generated);
    CHECK(IsError(withGenerated, ShaderSourceError::GeneratedDirectoryExists));
    CHECK(strcmp(withGenerated.path, "/Plugin/Test") == 0);
    CHECK(tree.MountCount() == 0);

    CHECK(tree.Mount("/Plugin/Test", a));
    // 같은 가상 디렉터리는 다른 실제 디렉터리여도 거부한다.
    CHECK(IsError(tree.Mount("/Plugin/Test", b), ShaderSourceError::DuplicateMount));
    CHECK(tree.Mount("/Plugin/Other", a));
    // 끝 구분자가 있는 실제 디렉터리도 받는다.
    char trailing[FixturePathBytes];
    snprintf(trailing, sizeof(trailing), "%s/", b);
    CHECK(tree.Mount("/Project", trailing));
    CHECK(tree.MountCount() == 3);
    CHECK(tree.MountedDirectory(0) == StringView("/Plugin/Test"));
}

TEST(ShaderSourceTree_LoadsFilesAndReportsErrors) {
    Fixture fixture;
    REQUIRE(fixture.Make("load"));
    REQUIRE(fixture.Write("P/Private/A.rsf", "float4 Main();\n"));
    REQUIRE(fixture.Write("Q/B.rsh", "int b;\n"));
    REQUIRE(fixture.MakeDirectory("P/Dir.rsh"));
    char p[FixturePathBytes];
    char q[FixturePathBytes];
    REQUIRE(fixture.Child("P", p, sizeof(p)));
    REQUIRE(fixture.Child("Q", q, sizeof(q)));
    ShaderSourceTree tree(TestAllocator());
    REQUIRE(tree.Mount("/Plugin/Test", p));
    REQUIRE(tree.Mount("/Project", q));

    Array<uint8> bytes(TestAllocator());
    CHECK(tree.Load("/Plugin/Test/Private/A.rsf", bytes));
    CHECK(bytes.Size() == 15 && memcmp(bytes.Data(), "float4 Main();\n", 15) == 0);
    CHECK(tree.Load("/Project/B.rsh", bytes));
    CHECK(bytes.Size() == 7);

    // 이름이 겹치는 플러그인(/Plugin/Tester)은 /Plugin/Test mount로 찾지 않는다.
    CHECK(IsError(tree.Load("/Plugin/Tester/A.rsh", bytes), ShaderSourceError::UnknownMount));
    CHECK(bytes.IsEmpty());
    CHECK(IsError(tree.Load("/Engine/Public/A.rsh", bytes), ShaderSourceError::UnknownMount));
    CHECK(IsError(tree.Load("/Plugin/Test/Missing.rsh", bytes), ShaderSourceError::FileNotFound));
    CHECK(IsError(tree.Load("/Plugin/Test/Dir.rsh", bytes), ShaderSourceError::FileNotFound));
    CHECK(IsError(tree.Load("/Plugin/Test/Generated/M.rsh", bytes),
                  ShaderSourceError::GeneratedSource));
    CHECK(IsError(tree.Load("/Plugin/Test/../A.rsh", bytes), ShaderSourceError::InvalidPath));
    CHECK(IsError(tree.Load("/Plugin/Test/A.txt", bytes), ShaderSourceError::InvalidExtension));

    // 크기 경계: maxBytes와 같으면 읽고 1 byte 작으면 거부한다.
    CHECK(tree.Load("/Plugin/Test/Private/A.rsf", bytes, 15));
    const ShaderSourceStatus tooLarge = tree.Load("/Plugin/Test/Private/A.rsf", bytes, 14);
    CHECK(IsError(tooLarge, ShaderSourceError::FileTooLarge));
    CHECK(strcmp(tooLarge.path, "/Plugin/Test/Private/A.rsf") == 0);
    CHECK(bytes.IsEmpty());
}

TEST(ShaderSourceTree_RejectsFileOverDefaultSizeLimit) {
    // 기본 한도(16 MiB)와 같은 크기는 읽고, 1 byte 넘으면 읽기 전에 거부한다.
    CHECK(MaxShaderSourceFileBytes == usize{16} * 1024 * 1024);
    Fixture fixture;
    REQUIRE(fixture.Make("size"));
    Array<uint8> content(TestAllocator());
    REQUIRE(content.TryResize(MaxShaderSourceFileBytes + 1));
    memset(content.Data(), ' ', content.Size());
    REQUIRE(fixture.WriteBytes("P/Big.rsh", content.Data(), content.Size()));
    REQUIRE(fixture.WriteBytes("P/Edge.rsh", content.Data(), MaxShaderSourceFileBytes));
    char p[FixturePathBytes];
    REQUIRE(fixture.Child("P", p, sizeof(p)));
    ShaderSourceTree tree(TestAllocator());
    REQUIRE(tree.Mount("/Plugin/Test", p));
    Array<uint8> bytes(TestAllocator());
    CHECK(IsError(tree.Load("/Plugin/Test/Big.rsh", bytes), ShaderSourceError::FileTooLarge));
    CHECK(tree.Load("/Plugin/Test/Edge.rsh", bytes));
    CHECK(bytes.Size() == MaxShaderSourceFileBytes);
}

TEST(ShaderSourceTree_MountsRepositoryEngineShaders) {
    char engine[FixturePathBytes];
    REQUIRE(RepositoryShaderDirectory(engine, sizeof(engine)));
    ShaderSourceTree tree(TestAllocator());
    const ShaderSourceStatus mounted = tree.Mount("/Engine", engine);
    PrintStatus("mount Engine/Shaders", mounted);
    REQUIRE(mounted);
    Array<uint8> bytes(TestAllocator());
    CHECK(tree.Load(ShaderPlatformHeaderPath, bytes));
    CHECK(!bytes.IsEmpty());
}

} // namespace shader_source_tree_tests_file
