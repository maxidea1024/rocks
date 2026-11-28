#include <ReflectionTestFixtures/NotifyFixture.generated.h>
#include <Base/Compiler.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/FieldNotifications.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/PropertyPath.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 쓰기 경로의 setter 경유 시험이다(Docs/Design/ChangeNotification.ko.md CN2b — CN-D14): 등록된
// write와 경로 쓰기(최상위 값·struct 멤버·Map 키·Optional)가 생성 setter를 지나(Apply·같은 값
// 생략·통지 한 번), ReadOnly 필드도 setter가 있으면 쓰이고, 값 검증 실패·encode 고정 Busy는 필드를
// 바꾸지 않으며, 다른 실행 scope 안에서도 호출자의 접근 문맥으로 쓰고, 등록이 참조 필드·path 없는
// setter를 거부한다.
using namespace rocks;
using reflection_fixture::NotifyCached;
using reflection_fixture::NotifyHolder;
using reflection_fixture::NotifyKind;
using reflection_fixture::NotifyModel;
using reflection_fixture::NotifyPoint;
using reflection_fixture::NotifySlot;
using reflection_fixture::NotifyStyle;
using reflection_fixture::NotifyWidget;
using reflection_fixture::NotifyWidgetChild;

namespace field_notification_setter_write_tests_file {

namespace {

constexpr uint32 MaxRecorded = 32;

Guid IdOf(TypeRegistry& types, TypeId type, const char* name) noexcept {
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
};

enum class Kind : uint8 {
    Write,
    InsertKey,
    Insert,
    Remove,
    Clear
};

// InsertKey는 Set 원소용이다(단일 값 Map은 PathWrite로 넣는다 — 이 fixture에는 Set이 없어 쓰지
// 않는다).

struct Fixture {
    explicit Fixture(Allocator& allocator_, RegistryOptions options = {}) noexcept
        : allocator(allocator_), types(allocator_),
          registry(allocator_, types, {0x4e535757, 1}, Move(options)) {
    }

    bool Start() noexcept {
        if (!GeneratedType<reflection_fixture::ValidRange>::Register(types) ||
            !GeneratedType<NotifyStyle>::Register(types) ||
            !GeneratedType<NotifyCached>::Register(types) ||
            !GeneratedType<NotifyWidget>::Register(types) ||
            !GeneratedType<NotifyWidgetChild>::Register(types)) {
            return false;
        }

        const TypeId widget = TypeIdOf<NotifyWidget>();
        label = IdOf(types, widget, "label");
        style = IdOf(types, widget, "style");
        scores = IdOf(types, widget, "scores");
        nickname = IdOf(types, widget, "nickname");
        locked = IdOf(types, widget, "locked");
        quiet = IdOf(types, widget, "quiet");
        span = IdOf(types, widget, "span");
        grid = IdOf(types, widget, "grid");
        stack = IdOf(types, widget, "stack");
        tags = IdOf(types, widget, "tags");
        return label != Guid{} && locked != Guid{};
    }

    template <class T>
    RootRef<T> Make() noexcept {
        auto created = registry.NewObject<T>(access, TypeIdOf<T>(), {}, {});
        return created ? Move(created.Value()) : RootRef<T>();
    }

    void Watch(ObjectHandle object, Recorder& recorder) noexcept {
        auto handle = registry.TrySubscribeAll(
            access, object,
            FieldChangeDelegate::CreateLambda(
                [&recorder](ObjectHandle, Guid id) noexcept { recorder.Add(id); }));
        CHECK(handle);
    }

    // 등록된 write(FieldWriteOperation)를 바로 부른다.
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

    Result<void> Path(Kind kind, ObjectHandle object, const char* text, const FieldValue* value,
                      FieldValue* previous = nullptr,
                      PropertyPathFailure* failure = nullptr) noexcept {
        auto lease = types.Acquire(TypeIdOf<NotifyWidget>());
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        PropertyPathBuffer path;
        if (auto parsed = ParsePropertyPath(lease.Value(), StringView(text), path); !parsed) {
            return parsed;
        }

        switch (kind) {
        case Kind::Write:
            return PathWrite(registry, access, object, path.View(), *value, previous, failure);
        case Kind::Insert:
            return PathInsertElement(registry, access, object, path.View(), value, failure);
        case Kind::Remove:
            return PathRemoveElement(registry, access, object, path.View(), nullptr, failure);
        case Kind::Clear:
            return PathClear(registry, access, object, path.View(), failure);
        case Kind::InsertKey:
            break;
        }

        auto inserted = PathInsertKey(registry, access, object, path.View(), failure);
        return inserted ? Result<void>::Success() : Result<void>::Failure(inserted.Error());
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

    FieldValue Str(const char* text) noexcept {
        FieldValue value(allocator);
        value.kind = ValueKind::Utf8String;
        CHECK(value.string.TryAssign(StringView(text)));
        return value;
    }

    Allocator& allocator;
    TypeRegistry types;
    ObjectRegistry registry;
    ObjectAccessContext access = registry.OwnerAccess();
    Guid label{};
    Guid style{};
    Guid scores{};
    Guid nickname{};
    Guid locked{};
    Guid quiet{};
    Guid span{};
    Guid grid{};
    Guid stack{};
    Guid tags{};
};

NotifyWidget* Live(Fixture& fixture, ObjectHandle object,
                   Optional<Borrow<NotifyWidget>>& holder) noexcept {
    auto borrowed = fixture.registry.Resolve<NotifyWidget>(fixture.access, object);
    if (!borrowed) {
        return nullptr;
    }

    holder.Emplace(Move(borrowed.Value()));
    auto got = holder.Value().Get();
    return got ? got.Value() : nullptr;
}

} // namespace

// 등록된 write(FieldWriteOperation)는 setter를 지난다: 바뀐 값은 Apply와 통지 한 번, 같은 값은 둘
// 다 없다. 값 종류가 틀리면 TypeMismatch이고 필드는 그대로다. ReadOnly 필드도 setter가 있으면
// 쓰인다. 통지 없는 필드의 setter는 값만 바꾼다.
TEST(FieldNotificationSetterWrite_RegisteredWriteGoesThroughSetter) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto root = fixture.Make<NotifyWidget>();
        REQUIRE(!root.Handle().IsNull());
        const ObjectHandle object = root.Handle();
        Recorder recorder;
        fixture.Watch(object, recorder);
        const TypeId type = TypeIdOf<NotifyWidget>();

        REQUIRE(fixture.WriteDirect(object, type, fixture.label, fixture.Str("Hello")));
        REQUIRE(fixture.WriteDirect(object, type, fixture.label, fixture.Str("Hello")));
        CHECK(ErrorOf(fixture.WriteDirect(object, type, fixture.label, fixture.Int(3))) ==
              ErrorCode::TypeMismatch);
        REQUIRE(fixture.WriteDirect(object, type, fixture.locked, fixture.Int(7)));
        REQUIRE(fixture.WriteDirect(object, type, fixture.quiet, fixture.Int(2)));
        Optional<Borrow<NotifyWidget>> holder;
        NotifyWidget* widget = Live(fixture, object, holder);
        REQUIRE(widget);
        CHECK(widget->label.View() == "Hello" && widget->relayouts == 1);
        CHECK(widget->locked == 7 && widget->quiet == 2);
        CHECK(recorder.CountOf(fixture.label) == 1 && recorder.CountOf(fixture.locked) == 1);
        CHECK(recorder.count == 2);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 경로 쓰기도 setter를 지난다: 최상위 값(이전 값 출력 포함)·struct 멤버(같은 값이면 통지 없음 —
// setter 없는 필드의 하위 경로는 늘 알린다)·Map 키 넣기·Optional<String> 값. ReadOnly 필드도 경로로
// 쓰인다.
TEST(FieldNotificationSetterWrite_PathWritesGoThroughSetter) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto root = fixture.Make<NotifyWidget>();
        REQUIRE(!root.Handle().IsNull());
        const ObjectHandle object = root.Handle();
        Recorder recorder;
        fixture.Watch(object, recorder);

        const FieldValue name = fixture.Str("Title");
        FieldValue previous(allocator);
        REQUIRE(fixture.Path(Kind::Write, object, "label", &name, &previous));
        CHECK(previous.kind == ValueKind::Utf8String && previous.string.View().IsEmpty());
        const FieldValue size = fixture.Float(18.0f);
        REQUIRE(fixture.Path(Kind::Write, object, "style.size", &size));
        REQUIRE(fixture.Path(Kind::Write, object, "style.size", &size));
        const FieldValue score = fixture.Int(40);
        REQUIRE(fixture.Path(Kind::Write, object, "scores[4]", &score));
        const FieldValue nick = fixture.Str("Ace");
        REQUIRE(fixture.Path(Kind::Write, object, "nickname", &nick));
        const FieldValue seven = fixture.Int(7);
        REQUIRE(fixture.Path(Kind::Write, object, "locked", &seven));

        Optional<Borrow<NotifyWidget>> holder;
        NotifyWidget* widget = Live(fixture, object, holder);
        REQUIRE(widget);
        CHECK(widget->label.View() == "Title" && widget->relayouts == 1);
        CHECK(widget->style.size == 18.0f && widget->scores.Count() == 1);
        CHECK(widget->nickname.HasValue() && widget->locked == 7);
        CHECK(recorder.CountOf(fixture.label) == 1 && recorder.CountOf(fixture.style) == 1);
        CHECK(recorder.CountOf(fixture.scores) == 1 && recorder.CountOf(fixture.nickname) == 1);
        CHECK(recorder.CountOf(fixture.locked) == 1);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 실패한 쓰기는 필드를 바꾸지 않는다: 상자 위 값 검증 실패(값 종류)와 encode 고정 중 Busy(실패 단계
// 0). 엄격 모드에서도 경로 API는 호출자가 넘긴 접근 문맥으로 setter를 부른다.
TEST(FieldNotificationSetterWrite_FailuresAndAccessScope) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto root = fixture.Make<NotifyWidget>();
        REQUIRE(!root.Handle().IsNull());
        const ObjectHandle object = root.Handle();
        Recorder recorder;
        fixture.Watch(object, recorder);

        const FieldValue wrong = fixture.Str("x");
        PropertyPathFailure failure;
        CHECK(ErrorOf(fixture.Path(Kind::Write, object, "style.size", &wrong, nullptr, &failure)) ==
              ErrorCode::TypeMismatch);
        CHECK(failure.step == 1);
        {
            auto pin = fixture.registry.PinForEncode(fixture.access, object);
            REQUIRE(pin);
            const FieldValue size = fixture.Float(30.0f);
            failure = PropertyPathFailure{};
            CHECK(ErrorOf(fixture.Path(Kind::Write, object, "style.size", &size, nullptr,
                                       &failure)) == ErrorCode::Busy);
            CHECK(failure.step == 0);
        }

        Optional<Borrow<NotifyWidget>> holder;
        NotifyWidget* widget = Live(fixture, object, holder);
        REQUIRE(widget);
        CHECK(widget->style.size == 0.0f && recorder.CountOf(fixture.style) == 0);
    }

    // 엄격 모드(실행 scope 필수)에서도 scope 없이 접근 문맥을 넘긴 경로 쓰기가 setter의 접근 검사·
    // 통지를 그 문맥으로 한다.
    {
        RegistryOptions strict;
        strict.requireLifecycleScope = true;
        Fixture fixture(allocator, Move(strict));
        REQUIRE(fixture.Start());
        auto root = fixture.Make<NotifyWidget>();
        REQUIRE(!root.Handle().IsNull());
        Recorder recorder;
        fixture.Watch(root.Handle(), recorder);
        const FieldValue name = fixture.Str("Scoped");
        CHECK(fixture.Path(Kind::Write, root.Handle(), "label", &name));
        CHECK(recorder.CountOf(fixture.label) == 1);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 등록은 setter를 사본을 만들 수 있는 값 필드에만 받는다(CN-D14): 강한 참조·참조 배열·참조 struct
// 필드의 setter와 path 없는 setter는 InvalidArgument, 값 필드(path 있음)는 받는다.
TEST(FieldNotificationSetterWrite_RegistrationRejectsReferenceSetters) {
    SystemAllocator allocator;
    const TypeSchema& schema = GeneratedType<NotifyHolder>::Schema();
    const RuntimeTypeOps generated = GeneratedType<NotifyHolder>::Ops();
    const FieldSetterFunction dummy =
        +[](Object&, void*) noexcept -> Result<void> { return Result<void>::Success(); };
    struct Case {
        const char* name;
        bool dropPath;
        ErrorCode expected;
    };

    const Case cases[] = {{"target", false, ErrorCode::InvalidArgument},
                          {"members", false, ErrorCode::InvalidArgument},
                          {"point", false, ErrorCode::InvalidArgument},
                          {"ratio", true, ErrorCode::InvalidArgument},
                          {"ratio", false, ErrorCode::None}};
    for (const Case& item : cases) {
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
        for (const FieldSchema& schemaField : schema.fields) {
            if (StringView(schemaField.name.Data(), schemaField.name.Size()) !=
                StringView(item.name)) {
                continue;
            }

            for (RuntimeFieldOps& field : fields) {
                if (field.fieldId == schemaField.id) {
                    field.setter = dummy;
                    if (item.dropPath) {
                        field.path = nullptr;
                    }

                    found = true;
                }
            }
        }

        REQUIRE(found);
        RuntimeTypeOps ops = GeneratedType<NotifyHolder>::Ops();
        ops.fields = fields.View();
        CHECK(ErrorOf(types.Register(schema, Move(ops))) == item.expected);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 원소 연산과 상자 경계(CN-D14): 배열의 삽입·원소 쓰기·삭제·비우기, 인라인 저장소보다 큰 정적
// 배열(heap 상자)의 원소 쓰기, Set의 새 원소(통지)와 이미 있는 원소(setter·통지 없음)가 모두
// setter를 지난다. 검증 hook struct의 거부는 InvalidArgument(StructValidation)이고 필드는 그대로다.
TEST(FieldNotificationSetterWrite_ElementOperationsAndValidation) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto root = fixture.Make<NotifyWidget>();
        REQUIRE(!root.Handle().IsNull());
        const ObjectHandle object = root.Handle();
        Recorder recorder;
        fixture.Watch(object, recorder);

        const FieldValue one = fixture.Int(1);
        const FieldValue two = fixture.Int(2);
        REQUIRE(fixture.Path(Kind::Insert, object, "stack[0]", &one));
        REQUIRE(fixture.Path(Kind::Insert, object, "stack[1]", &two));
        REQUIRE(fixture.Path(Kind::Write, object, "stack[0]", &two));
        REQUIRE(fixture.Path(Kind::Remove, object, "stack[1]", nullptr));
        CHECK(recorder.CountOf(fixture.stack) == 4);
        {
            Optional<Borrow<NotifyWidget>> holder;
            NotifyWidget* widget = Live(fixture, object, holder);
            REQUIRE(widget);
            CHECK(widget->stack.Size() == 1 && widget->stack[0] == 2);
        }

        REQUIRE(fixture.Path(Kind::Clear, object, "stack", nullptr));
        CHECK(recorder.CountOf(fixture.stack) == 5);

        const FieldValue cell = fixture.Int(9);
        REQUIRE(fixture.Path(Kind::Write, object, "grid[20]", &cell));
        REQUIRE(fixture.Path(Kind::Write, object, "grid[20]", &cell));
        CHECK(recorder.CountOf(fixture.grid) == 1);

        REQUIRE(fixture.Path(Kind::InsertKey, object, "tags[3]", nullptr));
        REQUIRE(fixture.Path(Kind::InsertKey, object, "tags[3]", nullptr));
        CHECK(recorder.CountOf(fixture.tags) == 1);

        const FieldValue high = fixture.Int(5);
        REQUIRE(fixture.Path(Kind::Write, object, "span.high", &high));
        const FieldValue low = fixture.Int(9);
        PropertyPathFailure failure;
        CHECK(ErrorOf(fixture.Path(Kind::Write, object, "span.low", &low, nullptr, &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::StructValidation && failure.step == 0);

        Optional<Borrow<NotifyWidget>> holder;
        NotifyWidget* widget = Live(fixture, object, holder);
        REQUIRE(widget);
        CHECK(widget->stack.Size() == 0 && widget->grid[20] == 9 && widget->tags.Count() == 1);
        CHECK(widget->span.low == 0 && widget->span.high == 5);
        CHECK(recorder.CountOf(fixture.span) == 1);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 상속한 setter 필드를 파생 객체에 쓰면 기반의 setter가 불린다(파생 함수표의 SetterOf 칸). 통지
// 구독자는 쓰기 Borrow를 놓은 뒤 불려 같은 객체를 경로로 읽을 수 있다. 파괴 요청된 객체의 쓰기는
// 실패하고 필드는 그대로다.
TEST(FieldNotificationSetterWrite_InheritedSubscriberReadsAndDestroyed) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto child = fixture.Make<NotifyWidgetChild>();
        REQUIRE(!child.Handle().IsNull());
        const ObjectHandle object = child.Handle();
        PropertyPathBuffer labelPath;
        {
            auto lease = fixture.types.Acquire(TypeIdOf<NotifyWidgetChild>());
            REQUIRE(lease);
            REQUIRE(ParsePropertyPath(lease.Value(), "label", labelPath));
        }

        ErrorCode readInside = ErrorCode::Unsupported;
        auto subscribed = fixture.registry.TrySubscribe(
            fixture.access, object, {fixture.label},
            FieldChangeDelegate::CreateLambda([&](ObjectHandle, Guid) noexcept {
                FieldValue seen(allocator);
                readInside = ErrorOf(
                    PathRead(fixture.registry, fixture.access, object, labelPath.View(), seen));
            }));
        REQUIRE(subscribed);
        const FieldValue name = fixture.Str("Child");
        REQUIRE(fixture.Path(Kind::Write, object, "label", &name));
        CHECK(readInside == ErrorCode::None);
        {
            auto borrowed = fixture.registry.Resolve<NotifyWidgetChild>(fixture.access, object);
            REQUIRE(borrowed);
            NotifyWidgetChild* widget = borrowed.Value().Get().Value();
            CHECK(widget->label.View() == "Child" && widget->relayouts == 1);
            REQUIRE(widget->Destroy());
        }

        const FieldValue late = fixture.Str("Late");
        CHECK(!fixture.Path(Kind::Write, object, "label", &late));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace field_notification_setter_write_tests_file
