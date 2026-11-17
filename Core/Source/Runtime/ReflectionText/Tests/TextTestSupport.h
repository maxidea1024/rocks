#pragma once

#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <ReflectionTestFixtures/TextFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Meta/GeneratedType.h>
#include <Object/ModuleRegistration.h>
#include <ReflectionText/TextValue.h>
#include <TestSupport/Test.h>

// ReflectionText 명령행 표기 시험의 공용 준비다(리플렉션 B9i3b — fixture
// ReflectionTestFixtures/TextFixture.h): 모듈 타입 등록, TextHolder lease, Name pool, 진단 모음과
// 경로·왕복·거부 도우미.
namespace rocks::test::text {

using namespace rocks::reflection_text;
using reflection_fixture::TextHolder;

// 모듈 등록·TextHolder lease·Name pool·옵션·진단 모음이다(소멸 순서: lease → 등록 → pool →
// Registry).
struct TextSetup {
    explicit TextSetup(Allocator& allocator) noexcept
        : types(allocator), registration(types, allocator), names(allocator),
          diagnostics(allocator) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        REQUIRE(names.Initialize());
        auto acquired = types.Acquire(GeneratedType<TextHolder>::Schema().id);
        REQUIRE(acquired);
        holder = Move(acquired.Value());
        options.names = &names;
    }

    ~TextSetup() noexcept {
        holder = TypeLease{};
        CHECK(registration.Unregister());
        CHECK(names.Shutdown());
        CHECK(types.Shutdown());
    }

    TypeRegistry types;
    ModuleRegistration registration;
    NamePool names;
    TypeLease holder;
    TextCodecOptions options;
    TextDiagnostics diagnostics;
};

// 경로 텍스트의 자리다(해석이 성공해야 한다).
inline TextSlot SlotOf(TextCodecContext& context, const TypeLease& type, StringView path) noexcept {
    TextPath resolved;
    auto made = ResolveTextPath(context, type, path, TextSource::Assignment(0), resolved);
    CHECK(made && made.Value());
    return made && made.Value() ? resolved.slot : TextSlot{};
}

// input을 읽어 다시 쓴 텍스트가 expected인지 본다.
inline bool RoundTrip(TextCodecContext& context, const TextSlot& slot, StringView input,
                      StringView expected) noexcept {
    TextValue value(context.GetAllocator());
    auto parsed = ParseTextValue(context, slot, input, TextSource::Assignment(0), "p", value);
    if (!parsed || !parsed.Value()) {
        return false;
    }

    String text(context.GetAllocator());
    auto formatted = FormatTextValue(context, slot, value, text);
    return formatted && text.View() == expected;
}

inline bool RoundTrip(TextCodecContext& context, const TextSlot& slot, StringView input) noexcept {
    return RoundTrip(context, slot, input, input);
}

// input을 읽으면 진단 code 하나 이상(첫 진단이 code)을 남기고 false인지 본다. 진단 모음은 먼저
// 비운다.
inline bool Rejects(TextCodecContext& context, const TextSlot& slot, StringView input,
                    TextDiagnosticCode code) noexcept {
    context.Diagnostics().Clear();
    TextValue value(context.GetAllocator());
    auto parsed = ParseTextValue(context, slot, input, TextSource::Assignment(7), "p", value);
    return parsed && !parsed.Value() && value.shape == TextValueShape::Empty &&
           context.Diagnostics().Count() >= 1 && context.Diagnostics().Get(0).code == code &&
           context.Diagnostics().Get(0).source.sourceIndex == 7;
}

// 경로 해석이 진단 code로 거부되는지 본다(진단 모음은 먼저 비운다).
inline bool PathRejects(TextCodecContext& context, const TypeLease& type, StringView path,
                        TextDiagnosticCode code) noexcept {
    context.Diagnostics().Clear();
    TextPath resolved;
    auto made = ResolveTextPath(context, type, path, TextSource::Assignment(3), resolved);
    return made && !made.Value() && context.Diagnostics().Count() == 1 &&
           context.Diagnostics().Get(0).code == code && context.Diagnostics().Get(0).path == path;
}

} // namespace rocks::test::text
