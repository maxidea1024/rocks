#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Object/ModuleRegistration.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <TestSupport/Test.h>

// 클래스 기본값 지문 golden이다(Docs/Design/RuntimeClassMetadata.ko.md 'A5c 지문 연결' 대조 1):
// 시험 fixture 모듈(ReflectionTestFixtures)의 구체 클래스마다 `<TypeId> present <editor 지문>
// <cooked 지문>`·`<TypeId> without <이유>`·`<TypeId> failed <이유>` 줄을 만들어 아래 표와 비교한다.
// 같은 표를 Windows·macOS·Debug·Release 체인이 함께 보므로 플랫폼·구성 사이 지문 차이(생성자
// 값·codec wire·지문 공식)가 여기서 드러난다. fixture 타입·생성자·지문 공식을 바꾸면 표를 새로
// 만든다(시험 실패 메시지의 실제 줄을 그대로 옮긴다).
using namespace rocks;
namespace class_defaults_golden_tests_file {

namespace {

// 기대 줄이다(TypeId 순). 빌드 재개 때(화면 마일스톤 뒤 첫 빌드) 실제 출력으로 채운다 — 그 전에는
// 비어 있어 이 시험이 실패한다(빈 golden이 통과하지 않게).
constexpr const char* GoldenLines[] = {
    nullptr,
};

struct Collected {
    explicit Collected(Allocator& allocator) noexcept : text(allocator) {
    }

    StringBuilder<4096> text;
};

Result<void> AppendLine(void* user, TypeId type, const ClassDefaults* defaults,
                        const ClassDefaultsDiagnostic& diagnostic) noexcept {
    auto& out = *static_cast<Collected*>(user);
    if (defaults != nullptr) {
        char editor[Sha256Digest::HexBytes + 1];
        char cooked[Sha256Digest::HexBytes + 1];
        defaults->Fingerprint(ClassDefaultsConfiguration::Editor).ToHex(editor);
        defaults->Fingerprint(ClassDefaultsConfiguration::Cooked).ToHex(cooked);
        return out.text.TryAppendFormat("{} present {} {}\n", type, StringView(editor),
                                        StringView(cooked));
    }

    return out.text.TryAppendFormat(
        "{} {} {}\n", type,
        StringView(IsClassWithoutDefaults(diagnostic.reason) ? "without" : "failed"),
        StringView(ClassDefaultsReasonName(diagnostic.reason)));
}

} // namespace

TEST(ClassDefaults_FingerprintGolden) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        Collected collected(allocator);
        REQUIRE(ForEachClassDefaults(types, allocator, &AppendLine, &collected));
        // 표가 채워졌는지 먼저 본다(아니면 실제 출력을 옮겨 채운다).
        REQUIRE(GoldenLines[0] != nullptr);
        StringBuilder<4096> expected(allocator);
        for (const char* line : GoldenLines) {
            REQUIRE(expected.TryAppend(StringView(line)));
            REQUIRE(expected.TryAppend("\n"));
        }

        CHECK(collected.text.View() == expected.View());
        CHECK(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

} // namespace class_defaults_golden_tests_file
