#pragma once

#include <ConfigTestFixtures/SettingsFixture.generated.h>
#include <Config/ConfigContext.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Object/ModuleRegistration.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 설정 계층 시험의 공용 준비다(Docs/Design/ConfigSystem.ko.md 8절 "시험" — 전역 싱글턴 없이
// 시험마다 설정 문맥을 만든다, 기본은 봉인 모드). fixture: ConfigTestFixtures/SettingsFixture.h.
namespace rocks::test::config {

using namespace rocks::config;
using config_fixture::DisplaySettings;
using config_fixture::FixtureQuality;
using config_fixture::GameplaySettings;
using config_fixture::GraphSettings;

// 문자열 원소 컨테이너(Array<String>·OrderedSet<String>)를 순서대로 `,`로 이은 텍스트다.
template <class Container>
bool JoinedEquals(const Container& values, StringView expected) noexcept {
    SystemAllocator allocator;
    String joined(allocator);
    bool first = true;
    for (const String& value : values) {
        if (!first && !joined.TryAppend(",")) {
            return false;
        }

        if (!joined.TryAppend(value.View())) {
            return false;
        }

        first = false;
    }

    if (joined.View() != expected) {
        printf("joined '%.*s', expected '%.*s'\n", static_cast<int>(joined.ByteCount()),
               joined.Data(), static_cast<int>(expected.ByteCount()), expected.Data());
        return false;
    }

    return true;
}

// 작업 디렉터리 아래 시험 전용 폴더다(소멸 때 지운다). 이름은 실행·시험마다 다르다(시간은 이름에만
// 쓴다).
struct ScratchTree {
    char root[200]{};
    usize length = 0;

    bool Make() noexcept {
        static int32 counter = 0;
        const int count =
            snprintf(root, sizeof(root), "rocks-config-%llu-%d",
                     static_cast<unsigned long long>(platform::MonotonicNanoseconds()),
                     platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&counter, 1) + 1);
        if (count <= 0 || static_cast<usize>(count) >= sizeof(root)) {
            return false;
        }

        length = static_cast<usize>(count);
        return static_cast<bool>(platform::PhysicalFileSystem().MakeDirectory(root, length));
    }

    // root/relative 파일을 content로 만든다(중간 디렉터리 포함).
    bool Write(const char* relative, const char* content) noexcept {
        char path[400];
        const int count = snprintf(path, sizeof(path), "%s/%s", root, relative);
        if (count <= 0 || static_cast<usize>(count) >= sizeof(path)) {
            return false;
        }

        const char* slash = strrchr(path, '/');
        if (slash == nullptr || !platform::MakeDirectoryTree(platform::PhysicalFileSystem(), path,
                                                             static_cast<usize>(slash - path))) {
            return false;
        }

        platform::File file;
        if (!file.Open(platform::PhysicalFileSystem(), path, static_cast<usize>(count),
                       platform::FileOpenMode::CreateAlways)) {
            return false;
        }

        const bool written = static_cast<bool>(file.WriteAll(content, strlen(content)).status);
        const bool closed = static_cast<bool>(file.Close());
        return written && closed;
    }

    // root/relative 파일 내용이다(없으면 빈 문자열, exists는 false).
    String Read(Allocator& allocator, const char* relative, bool* exists = nullptr) const noexcept {
        String text(allocator);
        const String path = Path(allocator, relative);
        platform::File file;
        const bool opened =
            static_cast<bool>(file.Open(platform::PhysicalFileSystem(), path.Data(),
                                        path.ByteCount(), platform::FileOpenMode::ReadExisting));
        if (exists != nullptr) {
            *exists = opened;
        }

        if (!opened) {
            return text;
        }

        char buffer[4096];
        for (;;) {
            const platform::FileTransfer read = file.Read(buffer, sizeof(buffer));
            if (!read.status || read.bytes == 0) {
                break;
            }

            CHECK(text.TryAppend(StringView(buffer, read.bytes)));
        }

        CHECK(file.Close());
        return text;
    }

    // root/relative 파일을 지운다.
    bool Remove(const char* relative) noexcept {
        SystemAllocator allocator;
        const String path = Path(allocator, relative);
        return static_cast<bool>(
            platform::PhysicalFileSystem().Delete(path.Data(), path.ByteCount()));
    }

    // root/relative 경로 텍스트다.
    String Path(Allocator& allocator, const char* relative) const noexcept {
        String text(allocator);
        CHECK(text.TryAppend(StringView(root, length)));
        CHECK(text.TryAppend("/"));
        CHECK(text.TryAppend(relative));
        return text;
    }

    ~ScratchTree() noexcept {
        if (length != 0) {
            static_cast<void>(
                platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), root, length));
        }
    }
};

// 타입 등록·Name pool·객체 Registry와 설정 문맥이다. 설정 struct
// 둘(GraphSettings·GameplaySettings)을 등록해 둔다. 소멸 순서: 문맥(TypeLease) → Registry → 등록 →
// pool → 타입.
struct ConfigFixture {
    // Config와 fixture 모듈(ConfigTestFixtures)의 타입을 등록하고 설정 문맥을 만든다. 등록 함수는
    // 모듈 생성 헤더에 선언되어 있고 손으로 쓴 헤더는 그 헤더를 include하지 않으므로 정의는
    // ConfigTestSupport.cpp에 있다.
    explicit ConfigFixture(const ConfigContextOptions& options = {}) noexcept;

    ~ConfigFixture() noexcept {
        context = nullptr;
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(names.Shutdown());
        CHECK(types.Shutdown());
    }

    static RegistryOptions MakeOptions(NamePool& pool) noexcept {
        RegistryOptions options;
        options.names = &pool;
        return options;
    }

    ConfigContext& Context() noexcept {
        return *context;
    }

    const GraphSettings& Graph() noexcept {
        return context->Get<GraphSettings>();
    }

    const GameplaySettings& Gameplay() noexcept {
        return context->Get<GameplaySettings>();
    }

    // 출처 스택의 층 순서가 expected인지 본다.
    bool SourcesAre(StringView branch, StringView section, StringView path,
                    Span<const ConfigLayer> expected) noexcept {
        Array<ConfigSourceRef> sources(system);
        if (!context->GetSources(branch, section, path, sources) ||
            sources.Size() != expected.Size()) {
            printf("sources of %.*s: %llu entries\n", static_cast<int>(path.ByteCount()),
                   path.Data(), static_cast<unsigned long long>(sources.Size()));
            return false;
        }

        for (usize index = 0; index < expected.Size(); ++index) {
            if (sources[index].layer != expected[index]) {
                return false;
            }
        }

        return true;
    }

    // 진단 모음에 code(와 FieldValue면 textCode)를 가진 진단이 line·column(0이면 위치를 보지
    // 않는다)에 있는지 본다. 없으면 모은 진단을 출력한다.
    bool HasDiagnostic(ConfigDiagnosticCode code, uint32 line = 0, uint32 column = 0,
                       reflection_text::TextDiagnosticCode textCode =
                           reflection_text::TextDiagnosticCode::InvalidValue) noexcept {
        const ConfigDiagnostics& all = context->Diagnostics();
        for (usize index = 0; index < all.Count(); ++index) {
            const ConfigDiagnostic item = all.Get(index);
            const bool sameText =
                code != ConfigDiagnosticCode::FieldValue || item.textCode == textCode;
            const bool samePosition =
                line == 0 || (item.position.line == line && item.position.column == column);
            if (item.code == code && sameText && samePosition) {
                return true;
            }
        }

        PrintDiagnostics();
        return false;
    }

    void PrintDiagnostics() noexcept {
        const ConfigDiagnostics& all = context->Diagnostics();
        for (usize index = 0; index < all.Count(); ++index) {
            const ConfigDiagnostic item = all.Get(index);
            const StringView name = ConfigDiagnosticCodeName(item.code);
            printf("  diagnostic %.*s(%u) at %u:%u path '%.*s': %.*s\n",
                   static_cast<int>(name.ByteCount()), name.Data(),
                   static_cast<unsigned>(item.textCode), item.position.line, item.position.column,
                   static_cast<int>(item.path.ByteCount()), item.path.Data(),
                   static_cast<int>(item.message.ByteCount()), item.message.Data());
        }
    }

    // text를 layer 층 입력으로 넣으면 거부되는지(InvalidArgument) 본다. 진단 모음은 먼저 비운다.
    bool Rejects(ConfigLayer layer, StringView branch, StringView text) noexcept {
        context->Diagnostics().Clear();
        auto added = context->AddLayerText(layer, "test", branch, "<test>", text);
        return !added && added.Error() == ErrorCode::InvalidArgument &&
               !context->HasPendingChanges();
    }

    SystemAllocator system;
    TypeRegistry types;
    ModuleRegistration registration;
    NamePool names;
    ObjectRegistry objects;
    UniquePtr<ConfigContext> context;
};

} // namespace rocks::test::config
