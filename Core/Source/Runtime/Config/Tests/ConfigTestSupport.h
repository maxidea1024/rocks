#pragma once

#include <ConfigTestFixtures/ConfigTestFixtures.module.generated.h>
#include <ConfigTestFixtures/SettingsFixture.generated.h>
#include <Config/ConfigContext.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Object/ModuleRegistration.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// 설정 계층 시험의 공용 준비다(Docs/Design/ConfigSystem.ko.md 8절 "시험" — 전역 싱글턴 없이
// 시험마다 설정 문맥을 만든다, 기본은 봉인 모드). fixture: ConfigTestFixtures/SettingsFixture.h.
namespace rocks::test::config {

using namespace rocks::config;
using config_fixture::FixtureQuality;
using config_fixture::GameplaySettings;
using config_fixture::GraphSettings;
using config_fixture::ReloadArgs;

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

// 타입 등록·Name pool·객체 Registry와 설정 문맥이다. 설정 struct
// 둘(GraphSettings·GameplaySettings)을 등록해 둔다. 소멸 순서: 문맥(TypeLease) → Registry → 등록 →
// pool → 타입.
struct ConfigFixture {
    explicit ConfigFixture(const ConfigContextOptions& options = {}) noexcept
        : types(system), registration(types, system), names(system),
          objects(system, types, {0x434f4e46, 1}, MakeOptions(names)) {
        REQUIRE(generated::ConfigTestFixtures::RegisterModuleTypes(registration));
        REQUIRE(names.Initialize());
        auto made = UniquePtr<ConfigContext>::TryMake(system, system, objects, options);
        REQUIRE(made);
        context = Move(made.Value());
        REQUIRE(context->Initialize());
        REQUIRE(context->RegisterSettings<GraphSettings>());
        REQUIRE(context->RegisterSettings<GameplaySettings>());
        REQUIRE(context->Sync());
    }

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
