#include "OptionalTestSupport.h"
#include <TestSupport/NFailAllocator.h>

// 리플렉션 B9e Optional PROPERTY의 그래프 저장·복원 시험이다(Docs/Design/ReflectionExtensions.ko.md
// 6절, 17절 B9e 항목): set·unset·누락 세 경우(누락 = 생성자 값), 내부 종류별 왕복(단일 값·String·
// Name·enum·digest·값 struct — struct 안 Optional 멤버·원소 struct 포함)과 다시 저장한 byte 같음,
// wire 형식 오류, 2단계 할당 실패 rollback.
using namespace rocks;
using namespace rocks::test::optional;

namespace {

Sha256Digest DigestOf(uint8 seed) noexcept {
    Sha256Digest digest{};
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] = static_cast<uint8>(seed + index);
    }

    return digest;
}

} // namespace

// 내부 종류마다 set 값과 unset이 그대로 왕복한다: 단일 값(uint32·int8·float·bool·Guid·digest)·
// String·Name(다른 pool로 intern)·enum·값 struct(String 멤버와 Optional 멤버 포함), struct 안
// Optional 멤버와 struct 원소 배열의 Optional 멤버. 생성자가 set으로 만든 필드를 unset으로 바꾼
// 값도 unset으로 돌아온다(unset은 present 0으로 저장되고 누락과 다르다). 복원한 그래프를 다시
// 저장하면 byte가 같다.
TEST(OptionalGraph_RoundTripSetUnsetAndInnerKinds) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool sourceNames(allocator);
    NamePool targetNames(allocator);
    REQUIRE(sourceNames.Initialize());
    REQUIRE(targetNames.Initialize());
    {
        ObjectRegistry source(allocator, types, {0x4f5047, 1}, WithNames(&sourceNames));
        ObjectRegistry target(allocator, types, {0x4f5047, 2}, WithNames(&targetNames));
        {
            RootRef<OptionalHolder> holder = New(source);
            auto socket = sourceNames.TryIntern("Socket_3");
            REQUIRE(socket);
            With(source, holder.Handle(), [&](OptionalHolder& value) noexcept {
                value.count.Emplace(5u);
                value.small.Emplace(static_cast<int8>(-7));
                value.ratio.Emplace(0.5f);
                value.flag.Emplace(false);
                value.guid.Emplace(Guid{0x4f50, 0x47});
                value.digest.Emplace(DigestOf(3));
                value.text.Emplace(String(source.ObjectAllocator()));
                REQUIRE(value.text->TryAssign("hello"));
                value.name.Emplace(socket.Value());
                value.mode.Emplace(OptionalMode::Second);
                OptionalPoint& point = value.point.Emplace(source.ObjectAllocator());
                point.x = -1;
                point.y = 2;
                REQUIRE(point.label.TryAssign("spot"));
                point.weight.Emplace(9u);
                value.preset.Reset();
                value.presetPoint.Reset();
                value.slot.count.Emplace(1u);
                value.slot.note.Emplace(String(source.ObjectAllocator()));
                REQUIRE(value.slot.note->TryAssign("note"));
                REQUIRE(value.slots.TryEmplaceBack());
                REQUIRE(value.slots.TryEmplaceBack());
                OptionalPoint& inner = value.slots[1].point.Emplace(source.ObjectAllocator());
                inner.x = 11;
            });

            auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            REQUIRE(saved);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            With(target, loaded.Value().Handle(), [&](OptionalHolder& value) noexcept {
                CHECK(value.count.HasValue() && *value.count == 5u);
                CHECK(value.small.HasValue() && *value.small == -7);
                CHECK(value.ratio.HasValue() && *value.ratio == 0.5f);
                CHECK(value.flag.HasValue() && !*value.flag);
                CHECK(value.guid.HasValue() && *value.guid == (Guid{0x4f50, 0x47}));
                CHECK(value.digest.HasValue() && value.digest->bytes[31] == 34);
                CHECK(value.text.HasValue() && value.text->View() == "hello");
                REQUIRE(value.name.HasValue());
                auto display = targetNames.Acquire(*value.name);
                CHECK(display && display.Value().View() == "Socket_3");
                CHECK(value.mode.HasValue() && *value.mode == OptionalMode::Second);
                REQUIRE(value.point.HasValue());
                CHECK(value.point->x == -1 && value.point->y == 2);
                CHECK(value.point->label.View() == "spot");
                CHECK(value.point->weight.HasValue() && *value.point->weight == 9u);
                // 생성자가 set으로 만든 값을 unset으로 저장했으면 unset으로 돌아온다.
                CHECK(!value.preset.HasValue());
                CHECK(!value.presetPoint.HasValue());
                CHECK(value.slot.count.HasValue() && *value.slot.count == 1u);
                CHECK(value.slot.note.HasValue() && value.slot.note->View() == "note");
                CHECK(!value.slot.point.HasValue());
                REQUIRE(value.slots.Size() == 2);
                CHECK(!value.slots[0].count.HasValue() && !value.slots[0].point.HasValue());
                CHECK(value.slots[1].point.HasValue() && value.slots[1].point->x == 11);
                CHECK(!value.locked.HasValue());
            });

            auto again =
                SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(again);
            CHECK(test::graph::SameBytes(saved.Value().View(), again.Value().View()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(sourceNames.Shutdown());
    CHECK(targetNames.Shutdown());
    CHECK(types.Shutdown());
}

// 누락과 unset은 다르다: Record에 없는 필드는 생성자 값(preset = 7, presetPoint = {3, 4})이고,
// unset(present 0)으로 저장된 필드는 생성자가 set으로 만들었어도 unset이 된다. set으로 저장된 값
// struct의 빠진 멤버는 그 자리의 기준 값이다 — unset이던 자리는 struct 기본 값, 생성자가 set으로
// 만든 자리는 생성자 값(presetPoint.x = 3)이 남는다.
TEST(OptionalGraph_MissingFieldKeepsConstructorValueAndUnsetResets) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4f5047, 3});
        auto count =
            OptionalPayload(allocator, true, WireType::U32,
                            [](ArchiveWriter& out) noexcept { return out.WriteU32(5).IsOk(); });
        auto unset = OptionalPayload(allocator, false, WireType::U32,
                                     [](ArchiveWriter&) noexcept { return true; });
        Array<uint8> onlyX(allocator);
        {
            ArchiveWriter x(allocator);
            REQUIRE(x.WriteI64(-6));
            const FieldBytes members[] = {
                {PointField("x"), static_cast<uint32>(WireType::I64), x.View()}};
            onlyX = test::optional::RecordOf(allocator, TypeIdOf<OptionalPoint>(), 1, members);
        }

        Array<uint8> onlyY(allocator);
        {
            ArchiveWriter y(allocator);
            REQUIRE(y.WriteI64(9));
            const FieldBytes members[] = {
                {PointField("y"), static_cast<uint32>(WireType::I64), y.View()}};
            onlyY = test::optional::RecordOf(allocator, TypeIdOf<OptionalPoint>(), 1, members);
        }

        auto record = [&](Span<const uint8> bytes) noexcept {
            return OptionalPayload(
                allocator, true, WireType::Record,
                [&](ArchiveWriter& out) noexcept { return out.WriteRaw(bytes).IsOk(); });
        };

        auto point = record(onlyX.View());
        auto presetPoint = record(onlyY.View());
        const uint32 optionalWire = static_cast<uint32>(WireType::Optional);
        const FieldBytes fields[] = {
            {HolderField("count"), optionalWire, count.View()},
            {HolderField("preset"), optionalWire, unset.View()},
            {HolderField("point"), optionalWire, point.View()},
            {HolderField("presetPoint"), optionalWire, presetPoint.View()}};
        auto graph = HolderGraph(allocator, fields);
        auto loaded =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), graph.View());
        REQUIRE(loaded);
        With(objects, loaded.Value().Handle(), [](OptionalHolder& value) noexcept {
            CHECK(value.count.HasValue() && *value.count == 5u);
            CHECK(!value.preset.HasValue());
            REQUIRE(value.point.HasValue());
            CHECK(value.point->x == -6 && value.point->y == 0 && value.point->label.View() == "");
            CHECK(!value.point->weight.HasValue());
            REQUIRE(value.presetPoint.HasValue());
            CHECK(value.presetPoint->x == 3 && value.presetPoint->y == 9);
            CHECK(!value.text.HasValue() && !value.slot.count.HasValue());
        });

        // 필드가 모두 빠진 Record는 생성자 값 그대로다.
        auto empty = HolderGraph(allocator, Span<const FieldBytes>());
        auto fresh =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), empty.View());
        REQUIRE(fresh);
        With(objects, fresh.Value().Handle(), [](OptionalHolder& value) noexcept {
            CHECK(value.preset.HasValue() && *value.preset == 7u);
            REQUIRE(value.presetPoint.HasValue());
            CHECK(value.presetPoint->x == 3 && value.presetPoint->y == 4);
            CHECK(!value.count.HasValue() && !value.point.HasValue());
        });

        loaded.Value().Reset();
        fresh.Value().Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// wire 형식 오류는 복원을 거부한다(객체를 만들지 않는다): present byte 2·unset 뒤
// byte(InvalidArgument), 내부 tag가 내부 형식과 다름(TypeMismatch), 내부 자리에 올 수 없는
// tag(InvalidArgument), 필드 wire가 Optional이 아님(TypeMismatch), 값 struct 내부 Record의 schema가
// 다름(TypeMismatch)·Record 뒤 byte(InvalidArgument), 선언되지 않은 enum 값(Reject 정책
// InvalidArgument), 넘치는 int8(Overflow).
TEST(OptionalGraph_WireErrorsAreRejected) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4f5047, 4});
        const uint32 optionalWire = static_cast<uint32>(WireType::Optional);
        auto load = [&](StringView field, uint32 wire, Span<const uint8> payload) noexcept {
            const FieldBytes fields[] = {{HolderField(field), wire, payload}};
            auto graph = HolderGraph(allocator, fields);
            auto loaded =
                LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), graph.View());
            CHECK(objects.Stats().liveObjects == 0);
            return loaded ? ErrorCode::None : loaded.Error();
        };

        const uint8 presentTwo[] = {2};
        CHECK(load("count", optionalWire, presentTwo) == ErrorCode::InvalidArgument);
        const uint8 unsetTrailing[] = {0, 1};
        CHECK(load("count", optionalWire, unsetTrailing) == ErrorCode::InvalidArgument);
        auto wrongInner =
            OptionalPayload(allocator, true, WireType::I64,
                            [](ArchiveWriter& out) noexcept { return out.WriteI64(1).IsOk(); });
        CHECK(load("count", optionalWire, wrongInner.View()) == ErrorCode::TypeMismatch);
        // ValueArray(13)는 Optional 내부 자리의 tag가 아니다(present 1 + u32 13 + 0 byte).
        const uint8 arrayInner[] = {1, 13, 0, 0, 0};
        CHECK(load("count", optionalWire, arrayInner) == ErrorCode::InvalidArgument);
        ArchiveWriter plain(allocator);
        REQUIRE(plain.WriteU32(5));
        CHECK(load("count", static_cast<uint32>(WireType::U32), plain.View()) ==
              ErrorCode::TypeMismatch);

        auto foreign = test::optional::RecordOf(allocator, TypeIdOf<OptionalSlot>(), 1,
                                                Span<const FieldBytes>());
        auto foreignPoint =
            OptionalPayload(allocator, true, WireType::Record, [&](ArchiveWriter& out) noexcept {
                return out.WriteRaw(foreign.View()).IsOk();
            });
        CHECK(load("point", optionalWire, foreignPoint.View()) == ErrorCode::TypeMismatch);
        auto own = test::optional::RecordOf(allocator, TypeIdOf<OptionalPoint>(), 1,
                                            Span<const FieldBytes>());
        ArchiveWriter trailing(allocator);
        REQUIRE(trailing.WriteBool(true));
        REQUIRE(trailing.WriteU32(static_cast<uint32>(WireType::Record)));
        REQUIRE(trailing.WriteRaw(own.View()));
        REQUIRE(trailing.WriteBool(false));
        CHECK(load("point", optionalWire, trailing.View()) == ErrorCode::InvalidArgument);

        auto undeclared =
            OptionalPayload(allocator, true, WireType::I64,
                            [](ArchiveWriter& out) noexcept { return out.WriteI64(0).IsOk(); });
        CHECK(load("mode", optionalWire, undeclared.View()) == ErrorCode::InvalidArgument);
        auto wide =
            OptionalPayload(allocator, true, WireType::I64,
                            [](ArchiveWriter& out) noexcept { return out.WriteI64(200).IsOk(); });
        CHECK(load("small", optionalWire, wide.View()) == ErrorCode::Overflow);
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 2단계(ApplyFields)가 unset 자리에 새 값을 만들 때 쓰는 객체 allocator(ConstructionContext::
// ObjectAllocator)의 할당이 실패하면 그 Stage는 OutOfMemory이고 트랜잭션 rollback 뒤 객체가 남지
// 않는다(Optional<String>·Optional<값 struct>의 String 멤버). 실패시킬 할당 순번을 성공 실행에서 센
// 수만큼 모두 돌린다(결정적 — 시간·스레드에 기대지 않는다).
TEST(OptionalGraph_ApplyAllocationFailureRollsBack) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    Array<uint8> bytes(allocator);
    {
        ObjectRegistry source(allocator, types, {0x4f5047, 5});
        RootRef<OptionalHolder> holder = New(source);
        With(source, holder.Handle(), [&](OptionalHolder& value) noexcept {
            value.text.Emplace(String(source.ObjectAllocator()));
            REQUIRE(value.text->TryAssign("allocated text"));
            OptionalPoint& point = value.point.Emplace(source.ObjectAllocator());
            REQUIRE(point.label.TryAssign("allocated label"));
        });

        auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
        REQUIRE(saved);
        bytes = Move(saved.Value());
        holder.Reset();
        REQUIRE(source.Shutdown(source.OwnerAccess()));
    }

    // 객체 allocator만 실패시킨다(Stage의 임시 할당은 따로 둔 allocator다).
    auto apply = [&](test::NFailAllocator& objectAllocator, uint64 failure,
                     uint64* attempts) noexcept -> ErrorCode {
        ObjectRegistry objects(objectAllocator, types, {0x4f5047, 6});
        ErrorCode result = ErrorCode::None;
        {
            ObjectGraphStage stage(allocator, objects, types, objects.OwnerAccess());
            ConstructionContext transaction(objects, objects.OwnerAccess());
            ObjectGraphInput input;
            input.bytes = bytes.View();
            // 준비 단계 실패는 시험 실패로 표시한다(REQUIRE는 값을 돌려주는 람다에서 쓸 수 없다).
            const bool prepared =
                stage.Validate(input).IsOk() && stage.CreateObjects(transaction).IsOk();
            CHECK(prepared);
            if (!prepared) {
                transaction.Rollback();
                result = ErrorCode::DependencyFailed;
            } else {
                objectAllocator.FailOnAttempt(failure);
                auto applied = stage.ApplyFields(transaction);
                if (attempts) {
                    *attempts = objectAllocator.Attempts();
                }

                objectAllocator.DisableFailure();
                result = applied ? ErrorCode::None : applied.Error();
                if (applied) {
                    CHECK(transaction.Commit());
                } else {
                    transaction.Rollback();
                }
            }
        }

        if (result != ErrorCode::None) {
            CHECK(objects.Stats().liveObjects == 0 && objects.Stats().reservedObjects == 0);
        }

        CHECK(objects.Collect(objects.OwnerAccess()));
        CHECK(objects.Shutdown(objects.OwnerAccess()));
        return result;
    };

    test::NFailAllocator objectAllocator(allocator);
    uint64 attempts = 0;
    REQUIRE(apply(objectAllocator, 0, &attempts) == ErrorCode::None);
    // 적어도 Optional<String>의 새 String과 Optional<OptionalPoint>의 label 할당이 있다.
    CHECK(attempts >= 2);
    for (uint64 index = 1; index <= attempts; ++index) {
        CHECK(apply(objectAllocator, index, nullptr) == ErrorCode::OutOfMemory);
    }

    CHECK(types.Shutdown());
}
