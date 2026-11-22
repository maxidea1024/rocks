#include "../../AssetDatabase/Tests/MemoryFileSystem.h"

#include <AssetNugget/NuggetCacheSettings.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Path.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <string.h>

// 에셋 너겟 설정 시험이다(Docs/Design/AssetSystem.ko.md 9.1절 결정 4·10, "A3c1 구현 세부"): 없는
// 파일의 기본값, 모든 칸, 칸마다 거부(문법 위치·루트·모르는 키·타입·backend 값·빈 localPath·NUL·
// Guid 형식), 상한 초과, 읽기 오류, 할당 실패, localPath 해석(상대·절대).
using namespace rocks;

namespace nugget_cache_settings_tests_file {

using rocks::asset_database_test::MemoryFault;
using rocks::asset_database_test::MemoryFileSystem;

namespace {

constexpr StringView ProjectRoot = "nugget-project";

Span<const uint8> Text(const char* text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

struct SettingsScene {
    SettingsScene() noexcept : files(allocator, ProjectRoot) {
    }

    bool Write(Span<const uint8> bytes, MemoryFault fault = MemoryFault::None) noexcept {
        files.Remove(NuggetSettingsFile);
        return files.AddFile(NuggetSettingsFile, bytes, fault);
    }

    Result<NuggetCacheSettings> Read(NuggetSettingsFailure* failure = nullptr) noexcept {
        return ReadNuggetCacheSettings(allocator, files, ProjectRoot, failure);
    }

    // text를 쓰고 읽어 거부 칸을 돌려준다(성공이면 None).
    NuggetSettingsField Reject(const char* text) noexcept {
        CHECK(Write(Text(text)));
        NuggetSettingsFailure failure;
        auto read = Read(&failure);
        if (read) {
            return NuggetSettingsField::None;
        }

        CHECK(read.Error() == ErrorCode::InvalidArgument);
        return failure.field;
    }

    SystemAllocator allocator;
    MemoryFileSystem files;
};

} // namespace

// 파일이 없으면 기본값(filesystem·false·Intermediate/AssetNuggets·null 세대), 빈 객체도 기본값,
// 모든 칸, backend 넷.
TEST(NuggetSettings_DefaultsAndFields) {
    SettingsScene scene;
    REQUIRE(scene.files.Ready());
    NuggetSettingsFailure failure;
    failure.field = NuggetSettingsField::Json;
    auto missing = scene.Read(&failure);
    REQUIRE(missing);
    CHECK(missing.Value().backend == NuggetBackendKind::FileSystem && !missing.Value().required &&
          missing.Value().localPath.View() == NuggetDefaultLocalPath &&
          missing.Value().generation == Guid{} && failure.field == NuggetSettingsField::None);
    CHECK(scene.Reject("{}") == NuggetSettingsField::None);

    REQUIRE(scene.Write(Text("{\"backend\": \"off\", \"required\": true, \"localPath\": "
                             "\"Cache/Nuggets\", \"generation\": "
                             "\"0123abcd-0123-4567-89ab-0123456789ab\"}")));
    auto full = scene.Read();
    REQUIRE(full);
    CHECK(full.Value().backend == NuggetBackendKind::Off && full.Value().required &&
          full.Value().localPath.View() == "Cache/Nuggets" &&
          full.Value().generation.high == 0x0123abcd01234567ull &&
          full.Value().generation.low == 0x89ab0123456789abull);
    struct Case {
        const char* text;
        NuggetBackendKind kind;
    };
    const Case cases[] = {
        {"{\"backend\": \"filesystem\"}", NuggetBackendKind::FileSystem},
        {"{\"backend\": \"rockpile\"}", NuggetBackendKind::Rockpile},
        {"{\"backend\": \"off\"}", NuggetBackendKind::Off},
        {"{\"backend\": \"auto\", \"required\": false}", NuggetBackendKind::Auto}};
    for (const Case& item : cases) {
        REQUIRE(scene.Write(Text(item.text)));
        auto read = scene.Read();
        CHECK(read && read.Value().backend == item.kind && !read.Value().required);
    }
}

// 거부: 문법(위치), 루트가 객체 아님, 모르는 키(대소문자만 다름 포함), 칸 타입, backend 값(대문자·
// 모르는 값), 빈 localPath·NUL, generation 형식(32자·중괄호·잘못된 글자·숫자), 엄격 JSON(주석·끝
// 쉼표), 중복 키.
TEST(NuggetSettings_Rejects) {
    SettingsScene scene;
    REQUIRE(scene.files.Ready());
    REQUIRE(scene.Write(Text("{\n  \"backend\": off\n}")));
    NuggetSettingsFailure failure;
    auto syntax = scene.Read(&failure);
    CHECK(!syntax && syntax.Error() == ErrorCode::InvalidArgument &&
          failure.field == NuggetSettingsField::Json && failure.line == 2 && failure.column == 14);

    struct Case {
        const char* text;
        NuggetSettingsField field;
    };
    const Case cases[] = {
        {"[]", NuggetSettingsField::Root},
        {"\"filesystem\"", NuggetSettingsField::Root},
        {"{\"Backend\": \"off\"}", NuggetSettingsField::UnknownKey},
        {"{\"remote\": true}", NuggetSettingsField::UnknownKey},
        {"{\"backend\": 1}", NuggetSettingsField::Backend},
        {"{\"backend\": \"Off\"}", NuggetSettingsField::Backend},
        {"{\"backend\": \"cloud\"}", NuggetSettingsField::Backend},
        {"{\"required\": \"true\"}", NuggetSettingsField::Required},
        {"{\"required\": 1}", NuggetSettingsField::Required},
        {"{\"localPath\": 5}", NuggetSettingsField::LocalPath},
        {"{\"localPath\": \"\"}", NuggetSettingsField::LocalPath},
        {"{\"localPath\": \"a\\u0000b\"}", NuggetSettingsField::LocalPath},
        {"{\"generation\": 7}", NuggetSettingsField::Generation},
        {"{\"generation\": \"0123abcd0123456789ab0123456789ab\"}", NuggetSettingsField::Generation},
        {"{\"generation\": \"{0123abcd-0123-4567-89ab-0123456789ab}\"}",
         NuggetSettingsField::Generation},
        {"{\"generation\": \"0123abcd-0123-4567-89ab-0123456789ag\"}",
         NuggetSettingsField::Generation},
        {"{\"backend\": \"off\", // note\n}", NuggetSettingsField::Json},
        {"{\"backend\": \"off\",}", NuggetSettingsField::Json},
        {"{\"backend\": \"off\", \"backend\": \"auto\"}", NuggetSettingsField::Json},
    };
    for (const Case& item : cases) {
        CHECK(scene.Reject(item.text) == item.field);
    }

    // 대문자 Guid는 받는다(TryParseGuid 표준 문자열).
    CHECK(scene.Reject("{\"generation\": \"0123ABCD-0123-4567-89AB-0123456789AB\"}") ==
          NuggetSettingsField::None);
    // failure 없이 거부(칸·문법 위치를 쓰지 않는다).
    REQUIRE(scene.Write(Text("[]")));
    CHECK(scene.Read(nullptr).Error() == ErrorCode::InvalidArgument);
    REQUIRE(scene.Write(Text("{")));
    CHECK(scene.Read(nullptr).Error() == ErrorCode::InvalidArgument);
}

// 상한(64 KiB) 초과는 CapacityExceeded(File), 상한 그대로는 읽는다, 열기·크기·읽기 오류는 그
// 오류, 할당 실패는 OutOfMemory(실패 없는 결과는 기준과 같다).
TEST(NuggetSettings_SizeErrorsAndAllocation) {
    SettingsScene scene;
    REQUIRE(scene.files.Ready());
    Array<uint8> large(scene.allocator);
    REQUIRE(large.TryResize(NuggetSettingsMaxBytes + 1));
    memset(large.Data(), ' ', large.Size());
    large[0] = '{';
    large[large.Size() - 1] = '}';
    REQUIRE(scene.Write(large.View()));
    NuggetSettingsFailure failure;
    auto tooLarge = scene.Read(&failure);
    CHECK(!tooLarge && tooLarge.Error() == ErrorCode::CapacityExceeded &&
          failure.field == NuggetSettingsField::File);
    REQUIRE(large.TryResize(NuggetSettingsMaxBytes));
    large[large.Size() - 1] = '}';
    REQUIRE(scene.Write(large.View()));
    CHECK(scene.Read());

    const MemoryFault faults[] = {MemoryFault::OpenBusy, MemoryFault::Size, MemoryFault::Read};
    for (MemoryFault fault : faults) {
        REQUIRE(scene.Write(Text("{}"), fault));
        auto read = scene.Read(&failure);
        CHECK(!read && read.Error() != ErrorCode::InvalidArgument &&
              read.Error() != ErrorCode::None && failure.field == NuggetSettingsField::None);
    }

    REQUIRE(scene.Write(Text("{\"backend\": \"auto\", \"localPath\": "
                             "\"Somewhere/Else/Longer/Than/The/Default/Local/Path\", "
                             "\"generation\": \"0123abcd-0123-4567-89ab-0123456789ab\"}")));
    test::NFailAllocator failing(scene.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto read = ReadNuggetCacheSettings(failing, scene.files, ProjectRoot, nullptr);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(read);
            CHECK(read.Value().backend == NuggetBackendKind::Auto &&
                  read.Value().localPath.View() ==
                      "Somewhere/Else/Longer/Than/The/Default/Local/Path");
            break;
        }

        CHECK(!read && read.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures >= 3);
}

// localPath 해석(Path::Root — 호스트 문법): 상대는 프로젝트 루트 + "/" + 값, 루트에 붙은 경로(POSIX
// 루트·Windows 드라이브 절대·UNC)는 그대로, Windows 현재 드라이브 루트·드라이브 상대·device
// namespace는 오류(설정 읽기도 거부한다 — POSIX에서는 이름이거나 루트), 할당 실패.
TEST(NuggetSettings_ResolveLocalPath) {
    SystemAllocator allocator;
    const bool windows = Path::HostStyle() == PathStyle::Windows;
    NuggetCacheSettings settings(allocator);
    String out(allocator);
    struct Case {
        const char* local;
        const char* windows;
        const char* posix;
    };
    // nullptr은 오류다.
    const Case cases[] = {
        {"Intermediate/AssetNuggets", "Root/Intermediate/AssetNuggets",
         "Root/Intermediate/AssetNuggets"},
        {"/shared/nuggets", nullptr, "/shared/nuggets"},
        {"D:/Nuggets", "D:/Nuggets", "Root/D:/Nuggets"},
        {"D:Nuggets", nullptr, "Root/D:Nuggets"},
        {"\\Nuggets", nullptr, "Root/\\Nuggets"},
        {"//server/share/nuggets", "//server/share/nuggets", "//server/share/nuggets"},
        {"\\\\?\\C:\\Nuggets", nullptr, "Root/\\\\?\\C:\\Nuggets"},
    };
    SettingsScene scene;
    REQUIRE(scene.files.Ready());
    for (const Case& item : cases) {
        REQUIRE(settings.localPath.TryAssign(item.local));
        const char* expected = windows ? item.windows : item.posix;
        auto resolved = ResolveNuggetLocalPath(settings, "Root", out);
        if (expected == nullptr) {
            CHECK(!resolved);
        } else {
            CHECK(resolved && out.View() == expected);
        }

        // 설정 읽기도 같은 판정이다.
        String json(allocator);
        REQUIRE(json.TryAppend("{\"localPath\": \""));
        for (const char* at = item.local; *at != '\0'; ++at) {
            if (*at == '\\') {
                REQUIRE(json.TryAppend("\\"));
            }

            REQUIRE(json.TryAppend(StringView(at, 1)));
        }

        REQUIRE(json.TryAppend("\"}"));
        REQUIRE(scene.Write(Span<const uint8>(reinterpret_cast<const uint8*>(json.View().Data()),
                                              json.View().ByteCount())));
        NuggetSettingsFailure failure;
        auto read = scene.Read(&failure);
        CHECK(static_cast<bool>(read) == (expected != nullptr));
        CHECK(read || failure.field == NuggetSettingsField::LocalPath);
    }

    test::NFailAllocator failing(allocator);
    String failingOut(failing);
    REQUIRE(settings.localPath.TryAssign("Relative"));
    failing.FailOnAttempt(1);
    CHECK(ResolveNuggetLocalPath(settings, "Root", failingOut).Error() == ErrorCode::OutOfMemory);
    failing.DisableFailure();
    REQUIRE(settings.localPath.TryAssign(windows ? "C:/Absolute" : "/Absolute"));
    failing.FailOnAttempt(1);
    CHECK(ResolveNuggetLocalPath(settings, "Root", failingOut).Error() == ErrorCode::OutOfMemory);
    failing.DisableFailure();
}

} // namespace nugget_cache_settings_tests_file
