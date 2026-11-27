#include <ReflectionTestFixtures/NotifyFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/FieldNotifications.h>
#include <Object/ObjectRegistry.h>
#include <Object/PropertyPath.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 쓰기 경로의 변경 통지 시험이다(Docs/Design/ChangeNotification.ko.md CN1b — CN-D10·CN-D15): 등록된
// write의 통지와 같은 값 생략(비트 같음·문자열·weak 참조), 강한 참조·컨테이너 Registry API의 통지
// (handle이 바뀐 참조 쓰기만, 통지하지 않는 필드 제외), 경로 쓰기의 최상위 필드 통지와 중복 합치기,
// 통지 묶음(합치기·중첩·파괴된 객체·문맥), NotifyIfTarget, 등록의 storage 필수 규칙.
using namespace rocks;
using reflection_fixture::NotifyHolder;
using reflection_fixture::NotifyHolderChild;
using reflection_fixture::NotifyKind;
using reflection_fixture::NotifyModel;
using reflection_fixture::NotifyPoint;
using reflection_fixture::NotifySlot;

namespace field_notification_write_tests_file {

namespace {

constexpr uint32 MaxRecorded = 64;

Guid FieldIdOf(TypeRegistry& types, TypeId type, const char* name) noexcept {
    auto lease = types.Acquire(type);
    if (!lease) {
        return Guid{};
    }

    for (const FieldSchema& field : lease.Value().Schema().fields) {
        if (StringView(field.name.Data(), field.name.Size()) == StringView(name)) {
            return field.id;
        }
    }

    return Guid{};
}

template <class T>
ErrorCode ErrorOf(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 받은 통지(대상 ID)를 순서대로 모은다.
struct Recorder {
    Guid ids[MaxRecorded]{};
    uint32 count = 0;

    void Add(Guid id) noexcept {
        REQUIRE(count < MaxRecorded);
        ids[count++] = id;
    }

    uint32 CountOf(Guid id) const noexcept {
        uint32 found = 0;
        for (uint32 index = 0; index < count; ++index) {
            found += ids[index] == id ? 1 : 0;
        }

        return found;
    }

    void Clear() noexcept {
        count = 0;
    }
};

struct Fixture {
    explicit Fixture(Allocator& allocator_) noexcept
        : allocator(allocator_), types(allocator_), registry(allocator_, types, {0x4e575254, 1}) {
    }

    bool Start() noexcept {
        if (!GeneratedType<NotifyModel>::Register(types) ||
            !GeneratedType<NotifyKind>::Register(types) ||
            !GeneratedType<NotifyPoint>::Register(types) ||
            !GeneratedType<NotifySlot>::Register(types) ||
            !GeneratedType<NotifyHolder>::Register(types) ||
            !GeneratedType<NotifyHolderChild>::Register(types)) {
            return false;
        }

        health = FieldIdOf(types, TypeIdOf<NotifyModel>(), "health");
        label = FieldIdOf(types, TypeIdOf<NotifyModel>(), "label");
        plain = FieldIdOf(types, TypeIdOf<NotifyModel>(), "plain");
        partner = FieldIdOf(types, TypeIdOf<NotifyModel>(), "partner");
        target = FieldIdOf(types, TypeIdOf<NotifyHolder>(), "target");
        members = FieldIdOf(types, TypeIdOf<NotifyHolder>(), "members");
        quiet = FieldIdOf(types, TypeIdOf<NotifyHolder>(), "quiet");
        slots = FieldIdOf(types, TypeIdOf<NotifyHolder>(), "slots");
        byKey = FieldIdOf(types, TypeIdOf<NotifyHolder>(), "byKey");
        byKind = FieldIdOf(types, TypeIdOf<NotifyHolder>(), "byKind");
        point = FieldIdOf(types, TypeIdOf<NotifyHolder>(), "point");
        numbers = FieldIdOf(types, TypeIdOf<NotifyHolder>(), "numbers");
        ratio = FieldIdOf(types, TypeIdOf<NotifyHolder>(), "ratio");
        return health != Guid{} && target != Guid{} && ratio != Guid{};
    }

    template <class T>
    RootRef<T> Make() noexcept {
        auto created = registry.NewObject<T>(access, TypeIdOf<T>(), {}, {});
        return created ? Move(created.Value()) : RootRef<T>();
    }

    // object의 모든 통지 대상을 recorder에 모은다.
    FieldNotifyHandle Watch(ObjectHandle object, Recorder& recorder) noexcept {
        auto handle = registry.TrySubscribeAll(
            access, object,
            FieldChangeDelegate::CreateLambda(
                [&recorder](ObjectHandle, Guid id) noexcept { recorder.Add(id); }));
        CHECK(handle);
        return handle ? handle.Value() : FieldNotifyHandle{};
    }

    // 경로 텍스트로 쓴다(게시된 객체).
    Result<void> Write(ObjectHandle object, TypeId type, const char* text,
                       const FieldValue& value) noexcept {
        auto lease = types.Acquire(type);
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        PropertyPathBuffer path;
        if (auto parsed = ParsePropertyPath(lease.Value(), StringView(text), path); !parsed) {
            return parsed;
        }

        return PathWrite(registry, access, object, path.View(), value);
    }

    Result<void> Insert(ObjectHandle object, TypeId type, const char* text,
                        const FieldValue* value) noexcept {
        auto lease = types.Acquire(type);
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        PropertyPathBuffer path;
        if (auto parsed = ParsePropertyPath(lease.Value(), StringView(text), path); !parsed) {
            return parsed;
        }

        return PathInsertElement(registry, access, object, path.View(), value);
    }

    // 등록된 write(FieldWriteOperation)를 묶음 없이 바로 부른다.
    Result<void> WriteDirect(ObjectHandle object, TypeId type, Guid field,
                             const FieldValue& value) noexcept {
        auto lease = types.Acquire(type);
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        const auto fields = lease.Value().Schema().fields;
        for (usize index = 0; index < fields.Size(); ++index) {
            const RuntimeFieldOps* ops = lease.Value().FieldOps(index);
            if (fields[index].id == field && ops && ops->write) {
                return ops->write(registry, access, object, value);
            }
        }

        return Result<void>::Failure(ErrorCode::NotFound);
    }

    FieldValue Int(int64 number) noexcept {
        FieldValue value(allocator);
        value.kind = ValueKind::Int32;
        value.signedInteger = number;
        return value;
    }

    FieldValue Float(float number) noexcept {
        FieldValue value(allocator);
        value.kind = ValueKind::Float32;
        value.float32 = number;
        return value;
    }

    FieldValue Reference(ValueKind kind, ObjectHandle object) noexcept {
        FieldValue value(allocator);
        value.kind = kind;
        value.reference = object;
        return value;
    }

    Allocator& allocator;
    TypeRegistry types;
    ObjectRegistry registry;
    ObjectAccessContext access = registry.OwnerAccess();
    Guid health{};
    Guid label{};
    Guid plain{};
    Guid partner{};
    Guid target{};
    Guid members{};
    Guid quiet{};
    Guid slots{};
    Guid byKey{};
    Guid byKind{};
    Guid point{};
    Guid numbers{};
    Guid ratio{};
};

} // namespace

// 등록된 write(최상위 단일 값 — 경로 쓰기의 최상위 값도 같은 지점)는 바뀐 쓰기만 알린다: 같은 정수·
// 문자열·weak 참조는 알리지 않고, 실수는 비트로 비교한다(0.0과 -0.0은 다르고 같은 NaN은 같다).
// 통지하지 않는 필드의 쓰기는 알리지 않는다. 구독자가 없어도 쓰기는 그대로다.
TEST(FieldNotificationWrite_RegisteredWriteNotifiesOnlyChanges) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto model = fixture.Make<NotifyModel>();
        auto other = fixture.Make<NotifyModel>();
        auto holder = fixture.Make<NotifyHolder>();
        REQUIRE(!model.Handle().IsNull());
        REQUIRE(!holder.Handle().IsNull());
        const TypeId modelType = TypeIdOf<NotifyModel>();
        const TypeId holderType = TypeIdOf<NotifyHolder>();

        // 구독자가 없으면 통지 없이 쓴다.
        REQUIRE(fixture.Write(model.Handle(), modelType, "health", fixture.Int(3)));
        Recorder recorder;
        (void)fixture.Watch(model.Handle(), recorder);
        REQUIRE(fixture.Write(model.Handle(), modelType, "health", fixture.Int(3)));
        CHECK(recorder.count == 0);
        REQUIRE(fixture.Write(model.Handle(), modelType, "health", fixture.Int(4)));
        CHECK(recorder.count == 1);
        CHECK(recorder.CountOf(fixture.health) == 1);

        REQUIRE(fixture.Write(model.Handle(), modelType, "plain", fixture.Int(9)));
        CHECK(recorder.count == 1);

        FieldValue text(allocator);
        text.kind = ValueKind::Utf8String;
        REQUIRE(text.string.TryAssign(StringView("north")));
        REQUIRE(fixture.Write(model.Handle(), modelType, "label", text));
        REQUIRE(fixture.Write(model.Handle(), modelType, "label", text));
        CHECK(recorder.CountOf(fixture.label) == 1);

        const FieldValue weak = fixture.Reference(ValueKind::WeakObjectReference, other.Handle());
        REQUIRE(fixture.Write(model.Handle(), modelType, "partner", weak));
        REQUIRE(fixture.Write(model.Handle(), modelType, "partner", weak));
        CHECK(recorder.CountOf(fixture.partner) == 1);
        REQUIRE(fixture.Write(model.Handle(), modelType, "partner",
                              fixture.Reference(ValueKind::WeakObjectReference, ObjectHandle{})));
        CHECK(recorder.CountOf(fixture.partner) == 2);

        Recorder floats;
        (void)fixture.Watch(holder.Handle(), floats);
        REQUIRE(fixture.Write(holder.Handle(), holderType, "ratio", fixture.Float(0.0f)));
        CHECK(floats.count == 0);
        REQUIRE(fixture.Write(holder.Handle(), holderType, "ratio", fixture.Float(-0.0f)));
        CHECK(floats.CountOf(fixture.ratio) == 1);
        const float nan = platform::BitCast<float>(uint32{0x7fc00001});
        REQUIRE(fixture.Write(holder.Handle(), holderType, "ratio", fixture.Float(nan)));
        REQUIRE(fixture.Write(holder.Handle(), holderType, "ratio", fixture.Float(nan)));
        CHECK(floats.CountOf(fixture.ratio) == 2);
    }
}

// 묶음 없이 부른 등록된 write도 한 번만 알린다: 강한 참조 필드는 SetReference 한 지점만(같은
// handle은 알리지 않음), 값 필드는 FieldWriteOperation 한 지점만.
TEST(FieldNotificationWrite_DirectWriteNotifiesOnce) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto holder = fixture.Make<NotifyHolder>();
        auto first = fixture.Make<NotifyModel>();
        const ObjectHandle owner = holder.Handle();
        const TypeId type = TypeIdOf<NotifyHolder>();
        Recorder recorder;
        (void)fixture.Watch(owner, recorder);

        const FieldValue strong = fixture.Reference(ValueKind::ObjectReference, first.Handle());
        REQUIRE(fixture.WriteDirect(owner, type, fixture.target, strong));
        CHECK(recorder.CountOf(fixture.target) == 1);
        REQUIRE(fixture.WriteDirect(owner, type, fixture.target, strong));
        CHECK(recorder.CountOf(fixture.target) == 1);
        REQUIRE(fixture.WriteDirect(owner, type, fixture.ratio, fixture.Float(1.5f)));
        CHECK(recorder.CountOf(fixture.ratio) == 1);
        CHECK(recorder.count == 2);
    }
}

// 파생 객체에서 상속한 Notify 컨테이너(ObjectRefArray)와 enum 키 Map을 Registry API로 바꿔도 그
// 필드를 알린다(storage thunk가 파생 객체에서 기반 멤버를 잰다).
TEST(FieldNotificationWrite_InheritedAndEnumKeyedContainersNotify) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto child = fixture.Make<NotifyHolderChild>();
        auto first = fixture.Make<NotifyModel>();
        const ObjectHandle owner = child.Handle();
        Recorder recorder;
        (void)fixture.Watch(owner, recorder);
        auto borrowed = fixture.registry.Resolve<NotifyHolderChild>(fixture.access, owner);
        REQUIRE(borrowed);
        NotifyHolderChild* object = borrowed.Value().Get().Value();
        REQUIRE(
            fixture.registry.AddReference(fixture.access, owner, object->members, first.Handle()));
        REQUIRE(fixture.registry.SetMapReference(fixture.access, owner, object->byKind,
                                                 NotifyKind::Second, first.Handle()));
        CHECK(recorder.CountOf(fixture.members) == 1);
        CHECK(recorder.CountOf(fixture.byKind) == 1);
        CHECK(recorder.count == 2);
    }
}

// Registry 참조·컨테이너 API는 게시된 객체의 Notify 필드를 알린다: 참조 쓰기는 handle이 바뀐 때만,
// 컨테이너 변경(ObjectRefArray·StructRefArray·ObjectRefMap)은 성공마다. 통지하지 않는 필드(quiet)와
// 실패한 변경은 알리지 않는다.
TEST(FieldNotificationWrite_ReferenceApisNotifyOwnerField) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto holder = fixture.Make<NotifyHolder>();
        auto first = fixture.Make<NotifyModel>();
        auto second = fixture.Make<NotifyModel>();
        const ObjectHandle owner = holder.Handle();
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        Recorder recorder;
        (void)fixture.Watch(owner, recorder);
        // 컨테이너 API는 호출자가 owner의 Borrow를 쥐어야 한다.
        auto objectBorrow = fixture.registry.Resolve<NotifyHolder>(fixture.access, owner);
        REQUIRE(objectBorrow);
        NotifyHolder* object = objectBorrow.Value().Get().Value();

        REQUIRE(registry.SetReference(access, owner, object->target, first.Handle()));
        REQUIRE(registry.SetReference(access, owner, object->target, first.Handle()));
        CHECK(recorder.CountOf(fixture.target) == 1);
        REQUIRE(registry.SetReference(access, owner, object->target, ObjectHandle{}));
        CHECK(recorder.CountOf(fixture.target) == 2);

        REQUIRE(registry.AddReference(access, owner, object->members, first.Handle()));
        REQUIRE(registry.InsertReferenceAt(access, owner, object->members, 0, second.Handle()));
        REQUIRE(registry.SetReferenceAt(access, owner, object->members, 1, second.Handle()));
        REQUIRE(registry.RemoveReferenceAt(access, owner, object->members, 0));
        REQUIRE(registry.RemoveReference(access, owner, object->members, second.Handle()));
        CHECK(ErrorOf(registry.RemoveReferenceAt(access, owner, object->members, 5)) !=
              ErrorCode::None);
        REQUIRE(registry.ClearReferences(access, owner, object->members));
        CHECK(recorder.CountOf(fixture.members) == 6);

        REQUIRE(registry.AddReference(access, owner, object->quiet, first.Handle()));
        REQUIRE(registry.ClearReferences(access, owner, object->quiet));
        CHECK(recorder.CountOf(fixture.quiet) == 0);

        auto added = registry.AddElement(access, owner, object->slots);
        REQUIRE(added);
        REQUIRE(registry.SetElementReference(access, owner, object->slots, added.Value(),
                                             object->slots[added.Value()].item, first.Handle()));
        REQUIRE(registry.SetElementReference(access, owner, object->slots, added.Value(),
                                             object->slots[added.Value()].item, first.Handle()));
        REQUIRE(registry.InsertElementAt(access, owner, object->slots, 0));
        REQUIRE(registry.RemoveElementAt(access, owner, object->slots, 0));
        REQUIRE(registry.ClearElements(access, owner, object->slots));
        CHECK(recorder.CountOf(fixture.slots) == 5);

        REQUIRE(registry.SetMapReference(access, owner, object->byKey, uint32{7}, first.Handle()));
        REQUIRE(registry.EraseMapReference(access, owner, object->byKey, uint32{7}));
        CHECK(ErrorOf(registry.EraseMapReference(access, owner, object->byKey, uint32{7})) !=
              ErrorCode::None);
        REQUIRE(registry.ClearMapReferences(access, owner, object->byKey));
        CHECK(recorder.CountOf(fixture.byKey) == 3);

        // 구독자가 없는 owner의 변경은 통지 판정 없이 성공한다.
        auto lone = fixture.Make<NotifyHolder>();
        // 컨테이너 API는 호출자가 owner의 Borrow를 쥐어야 한다.
        auto loneObjectBorrow =
            fixture.registry.Resolve<NotifyHolder>(fixture.access, lone.Handle());
        REQUIRE(loneObjectBorrow);
        NotifyHolder* loneObject = loneObjectBorrow.Value().Get().Value();
        REQUIRE(registry.AddReference(access, lone.Handle(), loneObject->members, first.Handle()));
        CHECK(recorder.count == 16);
    }
}

// 경로 쓰기는 최상위 필드를 한 번 알린다: struct 멤버 값(point.x), struct 안 참조(point.pin —
// SetReference와 경로 진입점이 한 번으로 합쳐진다), 참조 배열 원소(members[0] — SetReferenceAt과
// 합쳐진다), 값 배열 삽입(numbers). 최상위 강한 참조(target)는 SetReference 한 지점이 알린다(같은
// handle이면 알리지 않는다).
TEST(FieldNotificationWrite_PathWritesNotifyTopFieldOnce) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto holder = fixture.Make<NotifyHolder>();
        auto first = fixture.Make<NotifyModel>();
        const ObjectHandle owner = holder.Handle();
        const TypeId type = TypeIdOf<NotifyHolder>();
        Recorder recorder;
        (void)fixture.Watch(owner, recorder);

        REQUIRE(fixture.Write(owner, type, "point.x", fixture.Int(5)));
        CHECK(recorder.CountOf(fixture.point) == 1);
        REQUIRE(fixture.Write(owner, type, "point.pin",
                              fixture.Reference(ValueKind::ObjectReference, first.Handle())));
        CHECK(recorder.CountOf(fixture.point) == 2);

        // 컨테이너 API는 호출자가 owner의 Borrow를 쥐어야 한다.
        auto objectBorrow = fixture.registry.Resolve<NotifyHolder>(fixture.access, owner);
        REQUIRE(objectBorrow);
        NotifyHolder* object = objectBorrow.Value().Get().Value();
        REQUIRE(
            fixture.registry.AddReference(fixture.access, owner, object->members, ObjectHandle{}));
        CHECK(recorder.CountOf(fixture.members) == 1);
        REQUIRE(fixture.Write(owner, type, "members[0]",
                              fixture.Reference(ValueKind::ObjectReference, first.Handle())));
        CHECK(recorder.CountOf(fixture.members) == 2);

        const FieldValue number = fixture.Int(11);
        REQUIRE(fixture.Insert(owner, type, "numbers[0]", &number));
        CHECK(recorder.CountOf(fixture.numbers) == 1);

        const FieldValue strong = fixture.Reference(ValueKind::ObjectReference, first.Handle());
        REQUIRE(fixture.Write(owner, type, "target", strong));
        REQUIRE(fixture.Write(owner, type, "target", strong));
        CHECK(recorder.CountOf(fixture.target) == 1);
        CHECK(recorder.count == 6);
    }
}

// 통지 묶음은 (객체, 대상)마다 한 번으로 모았다가 가장 바깥 묶음을 닫을 때 모은 순서대로 낸다. 안쪽
// 묶음을 닫는 것은 아무것도 하지 않고, 모은 뒤 파괴 요청된 객체는 부르지 않으며, 구독자가 없는
// 대상은 모으지 않는다. 다른 문맥은 열 수 없고, 열지 않은 묶음의 End는 무동작이다.
TEST(FieldNotificationWrite_NotifyGroupMergesNestedNotifications) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        auto model = fixture.Make<NotifyModel>();
        auto doomed = fixture.Make<NotifyModel>();
        Recorder recorder;
        Recorder doomedRecorder;
        (void)fixture.Watch(model.Handle(), recorder);
        (void)fixture.Watch(doomed.Handle(), doomedRecorder);

        NotifyGroup idle;
        idle.End();
        CHECK(!idle.IsActive());

        auto outer = registry.BeginNotifyGroup(access);
        REQUIRE(outer);
        REQUIRE(registry.Notify(access, model.Handle(), NotifyTarget{fixture.label}));
        REQUIRE(registry.Notify(access, model.Handle(), NotifyTarget{fixture.health}));
        {
            auto inner = registry.BeginNotifyGroup(access);
            REQUIRE(inner);
            REQUIRE(registry.Notify(access, model.Handle(), NotifyTarget{fixture.label}));
            REQUIRE(registry.Notify(access, doomed.Handle(), NotifyTarget{fixture.health}));
            inner.Value().End();
            CHECK(recorder.count == 0);
        }

        REQUIRE(registry.RequestDestroy(access, doomed.Handle()));
        CHECK(recorder.count == 0);
        outer.Value().End();
        REQUIRE(recorder.count == 2);
        CHECK(recorder.ids[0] == fixture.label);
        CHECK(recorder.ids[1] == fixture.health);
        CHECK(doomedRecorder.count == 0);
        outer.Value().End();
        CHECK(recorder.count == 2);

        // 묶음 밖에서는 바로 낸다.
        REQUIRE(registry.Notify(access, model.Handle(), NotifyTarget{fixture.health}));
        CHECK(recorder.count == 3);

        const ObjectAccessContext wrong{ContextId{0x4e575254, 2}};
        CHECK(ErrorOf(registry.BeginNotifyGroup(wrong)) == ErrorCode::WrongContext);

        // 묶음은 이동할 수 있고(옮긴 쪽이 닫는다), 닫는 동안 구독자가 연 새 묶음은 그 자리에서
        // 닫혀 낸다(가장 바깥 묶음이 끝난 뒤라 중첩이 아니다).
        recorder.Clear();
        Recorder chained;
        auto chain = registry.TrySubscribe(
            access, model.Handle(), NotifyTarget{fixture.label},
            FieldChangeDelegate::CreateLambda([&](ObjectHandle object, Guid) noexcept {
                if (chained.count != 0) {
                    return;
                }

                chained.Add(fixture.label);
                auto nested = registry.BeginNotifyGroup(access);
                REQUIRE(nested);
                REQUIRE(registry.Notify(access, object, NotifyTarget{fixture.health}));
                REQUIRE(registry.Notify(access, object, NotifyTarget{fixture.health}));
            }));
        REQUIRE(chain);
        auto opened = registry.BeginNotifyGroup(access);
        REQUIRE(opened);
        NotifyGroup moved = Move(opened.Value());
        CHECK(!opened.Value().IsActive());
        CHECK(moved.IsActive());
        REQUIRE(registry.Notify(access, model.Handle(), NotifyTarget{fixture.label}));
        opened.Value().End();
        CHECK(recorder.count == 0);
        moved.End();
        CHECK(chained.count == 1);
        REQUIRE(recorder.count == 2);
        // label의 대상 구독자(chain)가 전체 구독자보다 먼저 불리고, 그 안에서 닫힌 묶음이 health를
        // 낸다.
        CHECK(recorder.ids[0] == fixture.health);
        CHECK(recorder.ids[1] == fixture.label);
    }
}

// NotifyIfTarget은 통지 대상이면 Notify와 같고 대상이 아닌 ID(통지하지 않는 필드·없는 ID)는
// 무동작이다. 회수된 handle은 StaleHandle이다.
TEST(FieldNotificationWrite_NotifyIfTargetSkipsNonTargets) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        auto model = fixture.Make<NotifyModel>();
        Recorder recorder;
        (void)fixture.Watch(model.Handle(), recorder);

        REQUIRE(registry.NotifyIfTarget(access, model.Handle(), fixture.plain));
        REQUIRE(registry.NotifyIfTarget(access, model.Handle(), Guid{1, 2}));
        CHECK(recorder.count == 0);
        REQUIRE(registry.NotifyIfTarget(access, model.Handle(), fixture.health));
        CHECK(recorder.CountOf(fixture.health) == 1);

        const ObjectHandle stale = model.Handle();
        model.Reset();
        REQUIRE(registry.RequestDestroy(access, stale));
        REQUIRE(registry.Collect(access));
        CHECK(ErrorOf(registry.NotifyIfTarget(access, stale, fixture.health)) ==
              ErrorCode::StaleHandle);
    }
}

// 참조·컨테이너를 담을 수 있는 Notify 필드(강한 참조·aggregate)는 함수표 storage가 있어야 한다
// (InvalidArgument). 단일 값 Notify 필드는 storage가 없어도 된다.
TEST(FieldNotificationWrite_RegistrationRequiresStorageForReferenceFields) {
    SystemAllocator allocator;
    const TypeSchema& schema = GeneratedType<NotifyHolder>::Schema();
    const RuntimeTypeOps generated = GeneratedType<NotifyHolder>::Ops();
    const char* required[] = {"target", "members", "slots", "byKey", "byKind", "point", "numbers"};
    for (const char* name : required) {
        TypeRegistry types(allocator);
        REQUIRE(GeneratedType<NotifyModel>::Register(types));
        REQUIRE(GeneratedType<NotifyKind>::Register(types));
        REQUIRE(GeneratedType<NotifyPoint>::Register(types));
        REQUIRE(GeneratedType<NotifySlot>::Register(types));
        Array<RuntimeFieldOps> fields(allocator);
        for (const RuntimeFieldOps& field : generated.fields) {
            REQUIRE(fields.TryPushBack(field));
        }

        bool found = false;
        for (usize index = 0; index < schema.fields.Size(); ++index) {
            if (StringView(schema.fields[index].name.Data(), schema.fields[index].name.Size()) !=
                StringView(name)) {
                continue;
            }

            for (RuntimeFieldOps& field : fields) {
                if (field.fieldId == schema.fields[index].id) {
                    CHECK(field.storage != nullptr);
                    field.storage = nullptr;
                    found = true;
                }
            }
        }

        REQUIRE(found);
        RuntimeTypeOps ops = GeneratedType<NotifyHolder>::Ops();
        ops.fields = fields.View();
        CHECK(ErrorOf(types.Register(schema, Move(ops))) == ErrorCode::InvalidArgument);
    }

    // 단일 값 Notify 필드(ratio)는 storage가 없어도 등록된다.
    {
        TypeRegistry types(allocator);
        REQUIRE(GeneratedType<NotifyModel>::Register(types));
        REQUIRE(GeneratedType<NotifyKind>::Register(types));
        REQUIRE(GeneratedType<NotifyPoint>::Register(types));
        REQUIRE(GeneratedType<NotifySlot>::Register(types));
        Array<RuntimeFieldOps> fields(allocator);
        const Guid ratio = [&]() noexcept {
            for (const FieldSchema& field : schema.fields) {
                if (StringView(field.name.Data(), field.name.Size()) == StringView("ratio")) {
                    return field.id;
                }
            }

            return Guid{};
        }();
        for (const RuntimeFieldOps& field : generated.fields) {
            REQUIRE(fields.TryPushBack(field));
            if (field.fieldId == ratio) {
                fields[fields.Size() - 1].storage = nullptr;
            }
        }

        RuntimeTypeOps ops = GeneratedType<NotifyHolder>::Ops();
        ops.fields = fields.View();
        CHECK(types.Register(schema, Move(ops)));
    }
}

} // namespace field_notification_write_tests_file
