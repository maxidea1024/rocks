#include "TextAssignTestSupport.h"

#include <ReflectionTestFixtures/NotifyFixture.generated.h>
#include <Object/FieldNotifications.h>

// 대입 묶음 게시의 변경 통지 시험이다(Docs/Design/ChangeNotification.ko.md CN1b — CN-D10): 묶음
// 게시 안의 게시는 통지하지 않고 게시가 끝난 뒤 게시한 최상위 필드를 알린다(같은 필드의 여러 단위는
// 한 번, 통지하지 않는 필드는 알리지 않음).
using namespace rocks;
using namespace rocks::test::assign;
using reflection_fixture::NotifyHolder;

namespace text_assign_notify_tests_file {

namespace {

constexpr uint32 MaxRecorded = 16;

struct Recorder {
    Guid ids[MaxRecorded]{};
    uint32 count = 0;

    uint32 CountOf(Guid id) const noexcept {
        uint32 found = 0;
        for (uint32 index = 0; index < count; ++index) {
            found += ids[index] == id ? 1 : 0;
        }

        return found;
    }
};

Guid FieldIdOf(const char* name) noexcept {
    for (const FieldSchema& field : GeneratedType<NotifyHolder>::Schema().fields) {
        if (StringView(field.name.Data(), field.name.Size()) == StringView(name)) {
            return field.id;
        }
    }

    return Guid{};
}

} // namespace

TEST(TextAssign_PublishNotifiesAssignedTopFields) {
    SystemAllocator system;
    Fixture fixture(system);
    auto created = fixture.objects.NewObject<NotifyHolder>(
        fixture.Access(), GeneratedType<NotifyHolder>::Schema().id);
    REQUIRE(created);
    const ObjectHandle holder = created.Value().Handle();
    Recorder recorder;
    auto watched = fixture.objects.TrySubscribeAll(
        fixture.Access(), holder,
        FieldChangeDelegate::CreateLambda([&recorder](ObjectHandle, Guid id) noexcept {
            REQUIRE(recorder.count < MaxRecorded);
            recorder.ids[recorder.count++] = id;
        }));
    REQUIRE(watched);

    const TextAssignment assignments[] = {Set("ratio", "2.5"), Set("numbers", "1,2,3"),
                                          Set("point.x", "6"), Set("point.y", "7")};
    auto prepared = fixture.Prepare(holder, Span<const TextAssignment>(assignments));
    REQUIRE(prepared);
    CHECK(recorder.count == 0);
    REQUIRE(prepared.Value().Publish());
    CHECK(recorder.CountOf(FieldIdOf("ratio")) == 1);
    CHECK(recorder.CountOf(FieldIdOf("numbers")) == 1);
    CHECK(recorder.CountOf(FieldIdOf("point")) == 1);
    CHECK(recorder.count == 3);
}

} // namespace text_assign_notify_tests_file
