#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldCodec.h>
#include <Object/GeneratedPathAccess.h>
#include <Object/PropertyPath.h>
#include <TestSupport/Test.h>

// 경로 연산(B6a)의 원자성 시험이다(Docs/Design/ReflectionFoundations.ko.md 7.1절 "원자성·수명·
// 스레드"): 할당 콜백의 재진입(같은 원소 삭제 후 같은 크기 재삽입(ABA)·배열 축소·Registry 컨테이너
// 변경·소유자 파괴), Array<E> 삽입의 버퍼 교체 순서(옛 버퍼 해제 콜백이 게시된 배열만 본다), 읽기
// 중 재진입 쓰기(현행 ReadGeneratedField 결함 회귀 포함), 최상위 생성 String·Soft 쓰기의 할당 중
// 고정, 모든 할당 실패 지점의 내용 불변, 연산 중 Unregister. GeneratedType 특수화는 B6b 생성 코드와
// 같은 모양으로 손으로 썼다.
using namespace rocks;

namespace path_reentry {

struct Box;

// 참조 없는 struct다(Array<Label> 원소·struct 필드). icon은 Soft 멤버다.
struct Label {
    explicit Label(Allocator& allocator) noexcept : name(allocator), icon(allocator) {
    }

    String name;
    SoftObjectRef<Box> icon;
};

// 참조 struct다(StructRefArray 원소).
struct Entry {
    explicit Entry(Allocator& allocator) noexcept : name(allocator) {
    }

    ObjectRef<Box> target;
    String name;
};

struct Box final : Object {
    explicit Box(Allocator& allocator) noexcept
        : title(allocator), caption(allocator), icon(allocator), label(allocator), names(allocator),
          labels(allocator), entries(allocator), softs(allocator), refs(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept;

    String title;
    String caption;
    SoftObjectRef<Box> icon;
    Label label;
    Array<String> names;
    Array<Label> labels;
    StructRefArray<Entry> entries;
    Array<SoftObjectRef<Box>> softs;
    ObjectRefArray<Box> refs;
};

constexpr TypeId BoxId{0x505242, 1};
constexpr TypeId LabelId{0x505253, 1};
constexpr TypeId EntryId{0x505253, 2};
constexpr Guid NameId{0x50524d, 1};
constexpr Guid TargetId{0x50524d, 2};
constexpr Guid EntryNameId{0x50524d, 3};
constexpr Guid LabelIconId{0x50524d, 4};
constexpr Guid TitleId{0x505246, 1};
constexpr Guid LabelFieldId{0x505246, 2};
constexpr Guid NamesId{0x505246, 3};
constexpr Guid LabelsId{0x505246, 4};
constexpr Guid EntriesId{0x505246, 5};
constexpr Guid CaptionId{0x505246, 6};
constexpr Guid IconId{0x505246, 7};
constexpr Guid SoftsId{0x505246, 8};
constexpr Guid RefsId{0x505246, 9};

Span<const char> Text(const char* text) noexcept {
    usize bytes = 0;
    while (text[bytes] != '\0') {
        ++bytes;
    }

    return Span<const char>(text, bytes);
}

FieldSchema Scalar(Guid id, const char* name, ValueKind kind, TypeId type = {},
                   uint32 flags = 0) noexcept {
    FieldSchema field;
    field.id = id;
    field.kind = kind == ValueKind::ObjectReference ? FieldKind::ObjectReference : FieldKind::Value;
    field.flags = flags;
    field.name = Text(name);
    field.value = ValueType{kind, type};
    return field;
}

FieldSchema ArrayOf(Guid id, const char* name, ValueKind element, TypeId type = {}) noexcept {
    FieldSchema field;
    field.id = id;
    field.kind =
        element == ValueKind::ObjectReference ? FieldKind::ObjectReference : FieldKind::Value;
    field.name = Text(name);
    field.value = ValueType{ValueKind::Array, {}};
    field.element = ValueType{element, type};
    field.maxElements = DefaultMaxElements;
    return field;
}

template <auto Member, ValueKind Target>
RuntimeStructFieldOps MemberOps(Guid id) noexcept {
    return detail::StructFieldOps<Member>(id, &detail::PathStructMember<Member, Target>);
}

// 단일 값 클래스 필드(생성 코드 모양 — read·write·writeStaged, path 없음).
template <auto Member>
RuntimeFieldOps ValueField(Guid id) noexcept {
    using O = typename detail::StructMemberPointer<decltype(Member)>::Owner;
    RuntimeFieldOps ops;
    ops.fieldId = id;
    ops.read = +[](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
                   FieldValue& out) noexcept {
        return detail::ReadGeneratedField<O>(registry, access, object, Member, out);
    };
    ops.write = +[](ObjectRegistry& registry, const ObjectAccessContext& access,
                    ObjectHandle object, const FieldValue& value) noexcept {
        return detail::WriteGeneratedField<O>(registry, access, object, Member, value);
    };
    ops.writeStaged = +[](ConstructionContext& construction, ObjectHandle object,
                          const FieldValue& value) noexcept {
        return detail::WriteGeneratedStagedField<O>(construction, object, Member, value);
    };
    return ops;
}

// struct·배열 클래스 필드(B6b 생성 코드 모양 — codec, valueNativeType, path 람다).
template <auto Member, ValueKind Target>
RuntimeFieldOps PathField(Guid id, const void* valueNative = nullptr) noexcept {
    using O = typename detail::StructMemberPointer<decltype(Member)>::Owner;
    RuntimeFieldOps ops;
    ops.fieldId = id;
    ops.encode =
        +[](const RuntimeFieldOps&, ObjectRegistry& registry, const ObjectAccessContext& access,
            ObjectHandle object, ArchiveWriter& out, FieldEncodeContext& context) noexcept {
            return detail::EncodeGeneratedField<O>(registry, access, object, Member, out, context);
        };
    ops.decode = +[](const RuntimeFieldOps&, ConstructionContext* construction, ObjectHandle object,
                     const FieldView& field, FieldDecodeContext& context) noexcept {
        return detail::DecodeGeneratedField<O>(construction, object, Member, field, context);
    };
    ops.valueNativeType = valueNative;
    ops.path = +[](const RuntimeFieldOps&, detail::PathCursor& cursor) noexcept -> Result<void> {
        return detail::PathClassField<O, Target>(cursor, Member);
    };
    return ops;
}

} // namespace path_reentry

namespace rocks {

template <>
struct GeneratedType<path_reentry::Label> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return path_reentry::LabelId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_reentry;
        static const FieldSchema fields[] = {
            Scalar(NameId, "name", ValueKind::Utf8String),
            Scalar(LabelIconId, "icon", ValueKind::SoftObjectReference, BoxId)};
        static const TypeSchema schema{Id(), 1, fields, {"Label", 5}};
        return schema;
    }

    static void Trace(const path_reentry::Label&, TraceVisitor&) noexcept {
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace path_reentry;
        static const RuntimeStructFieldOps fields[] = {
            MemberOps<&Label::name, ValueKind::Utf8String>(NameId),
            MemberOps<&Label::icon, ValueKind::SoftObjectReference>(LabelIconId)};
        RuntimeStructOps ops = MakeRuntimeStructOps<Label>();
        ops.fields = fields;
        return ops;
    }
};

template <>
struct GeneratedType<path_reentry::Entry> {
    static constexpr bool ContainsStrongReference = true;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return path_reentry::EntryId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_reentry;
        static const FieldSchema fields[] = {
            Scalar(TargetId, "target", ValueKind::ObjectReference, BoxId),
            Scalar(EntryNameId, "name", ValueKind::Utf8String)};
        static const TypeSchema schema{Id(), 1, fields, {"Entry", 5}};
        return schema;
    }

    static void Trace(const path_reentry::Entry& value, TraceVisitor& visitor) noexcept {
        visitor.Visit(value.target);
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace path_reentry;
        static const RuntimeStructFieldOps fields[] = {
            MemberOps<&Entry::target, ValueKind::ObjectReference>(TargetId),
            MemberOps<&Entry::name, ValueKind::Utf8String>(EntryNameId)};
        RuntimeStructOps ops = MakeRuntimeStructOps<Entry>();
        ops.trace = [](const void* value, TraceVisitor& visitor) noexcept {
            GeneratedType<path_reentry::Entry>::Trace(
                *static_cast<const path_reentry::Entry*>(value), visitor);
        };
        ops.fields = fields;
        return ops;
    }
};

template <>
struct GeneratedType<path_reentry::Box> {
    static constexpr TypeId Id() noexcept {
        return path_reentry::BoxId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_reentry;
        static const FieldSchema fields[] = {
            Scalar(TitleId, "title", ValueKind::Utf8String),
            Scalar(LabelFieldId, "label", ValueKind::Struct, LabelId),
            ArrayOf(NamesId, "names", ValueKind::Utf8String),
            ArrayOf(LabelsId, "labels", ValueKind::Struct, LabelId),
            ArrayOf(EntriesId, "entries", ValueKind::Struct, EntryId),
            Scalar(CaptionId, "caption", ValueKind::Utf8String, {},
                   static_cast<uint32>(FieldFlags::ReadOnly)),
            Scalar(IconId, "icon", ValueKind::SoftObjectReference, BoxId),
            ArrayOf(SoftsId, "softs", ValueKind::SoftObjectReference, BoxId),
            ArrayOf(RefsId, "refs", ValueKind::ObjectReference, BoxId)};
        static const TypeSchema schema{Id(), 1, fields, {"Box", 3}};
        return schema;
    }

    static RuntimeTypeOps Ops() noexcept {
        using namespace path_reentry;
        static const RuntimeFieldOps fields[] = {
            ValueField<&Box::title>(TitleId),
            PathField<&Box::label, ValueKind::Struct>(LabelFieldId, NativeTypeKey<Label>()),
            PathField<&Box::names, ValueKind::Utf8String>(NamesId),
            PathField<&Box::labels, ValueKind::Struct>(LabelsId, NativeTypeKey<Label>()),
            PathField<&Box::entries, ValueKind::Struct>(EntriesId, NativeTypeKey<Entry>()),
            ValueField<&Box::caption>(CaptionId),
            ValueField<&Box::icon>(IconId),
            PathField<&Box::softs, ValueKind::SoftObjectReference>(SoftsId),
            PathField<&Box::refs, ValueKind::ObjectReference>(RefsId)};
        auto ops = MakeRuntimeTypeOps<Box>();
        ops.fields = fields;
        return ops;
    }
};

} // namespace rocks

void path_reentry::Box::Trace(TraceVisitor& visitor) const noexcept {
    detail::TraceArrayField(entries, visitor);
    detail::TraceArrayField(refs, visitor);
}

namespace {

using namespace path_reentry;

// 할당을 세고, 지정한 번째 할당을 실패시키고, 지정한 번째 할당 직전에 hook을 한 번 실행하고,
// 지켜보는 주소의 반환 직전에 hook을 한 번 실행한다(allocator 콜백의 재진입).
class HookAllocator final : public Allocator {
public:
    explicit HookAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        ++allocations;
        if (failAt != 0 && allocations == failAt) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        if (hook != nullptr && allocations == hookAt) {
            auto run = hook;
            hook = nullptr;
            run(hookContext);
        }

        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (releaseHook != nullptr && address == watched) {
            auto run = releaseHook;
            releaseHook = nullptr;
            run(releaseContext);
        }

        return backing_.Deallocate(address);
    }

    // 지금부터 after번째 할당 직전에 run을 한 번 실행한다.
    void ArmAllocation(void (*run)(void*) noexcept, void* context, uint64 after = 1) noexcept {
        hook = run;
        hookContext = context;
        hookAt = allocations + after;
    }

    // address의 반환 직전에 run을 한 번 실행한다.
    void ArmRelease(const void* address, void (*run)(void*) noexcept, void* context) noexcept {
        watched = address;
        releaseHook = run;
        releaseContext = context;
    }

    // 지금부터 n번째 할당을 실패시킨다(0이면 끈다).
    void FailAfter(uint64 n) noexcept {
        failAt = n == 0 ? 0 : allocations + n;
    }

    uint64 allocations = 0;

private:
    Allocator& backing_;
    uint64 failAt = 0;
    uint64 hookAt = 0;
    void (*hook)(void*) noexcept = nullptr;
    void* hookContext = nullptr;
    const void* watched = nullptr;
    void (*releaseHook)(void*) noexcept = nullptr;
    void* releaseContext = nullptr;
};

// 다른 allocator 주소로 같은 backing에 넘긴다(옛 값을 객체와 다른 allocator로 받는 경로 — 할당 수는
// backing의 HookAllocator가 센다).
class ForwardAllocator final : public Allocator {
public:
    explicit ForwardAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

private:
    Allocator& backing_;
};

template <usize N>
PropertyPath PathOf(const PathStep (&steps)[N]) noexcept {
    return PropertyPath{Span<const PathStep>(steps, N)};
}

PathStep At(Guid id, uint32 index) noexcept {
    return PathStep{id, index, true};
}

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

FieldValue TextValue(Allocator& allocator, const char* text) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Utf8String;
    if (!result.string.TryAssign(text)) {
        platform::Fatal("fixture text allocation failed");
    }

    return result;
}

FieldValue SoftValue(Allocator& allocator, Guid asset, const char* path) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::SoftObjectReference;
    result.guid = asset;
    if (!result.string.TryAssign(path)) {
        platform::Fatal("fixture soft path allocation failed");
    }

    return result;
}

// 한 시험의 Registry 묶음이다. TypeRegistry는 일반 allocator, ObjectRegistry(객체와 멤버 버퍼)는
// HookAllocator를 쓴다.
struct World {
    explicit World(Allocator& allocator) noexcept
        : hooked(allocator), types(allocator), objects(hooked, types, {0x5052, 1}) {
    }

    bool Register() noexcept {
        auto label =
            types.RegisterStruct(GeneratedType<Label>::Schema(), GeneratedType<Label>::Ops());
        auto entry =
            types.RegisterStruct(GeneratedType<Entry>::Schema(), GeneratedType<Entry>::Ops());
        auto box = types.Register(GeneratedType<Box>::Schema(), GeneratedType<Box>::Ops());
        if (!label || !entry || !box) {
            return false;
        }

        labelToken = label.Value();
        boxToken = box.Value();
        return true;
    }

    HookAllocator hooked;
    TypeRegistry types;
    ObjectRegistry objects;
    TypeRegistrationToken labelToken;
    TypeRegistrationToken boxToken;
};

Box* Get(Borrow<Box>& borrow) noexcept {
    auto pointer = borrow.Get();
    if (!pointer) {
        platform::Fatal("fixture borrow failed");
    }

    return pointer.Value();
}

Box* Pointer(ObjectRegistry& objects, ObjectHandle owner) noexcept {
    auto borrowed = objects.Resolve<Box>(objects.OwnerAccess(), owner);
    if (!borrowed) {
        platform::Fatal("fixture borrow failed");
    }

    return Get(borrowed.Value());
}

// 재진입 탐침의 상태다(hook이 같은 owner에 경로·Registry 연산을 시도하고 결과를 남긴다).
struct Probe {
    World* world = nullptr;
    ObjectHandle owner;
    Box* box = nullptr;
    Allocator* allocator = nullptr;
    ErrorCode removed = ErrorCode::DependencyFailed;
    ErrorCode inserted = ErrorCode::DependencyFailed;
    ErrorCode cleared = ErrorCode::DependencyFailed;
    ErrorCode written = ErrorCode::DependencyFailed;
    ErrorCode container = ErrorCode::DependencyFailed;
    ErrorCode reference = ErrorCode::DependencyFailed;
    ErrorCode referenceInserted = ErrorCode::DependencyFailed;
    ErrorCode referenceSet = ErrorCode::DependencyFailed;
    ErrorCode destroyed = ErrorCode::DependencyFailed;
    ErrorCode unregistered = ErrorCode::DependencyFailed;
    ErrorCode unregisteredOwner = ErrorCode::DependencyFailed;
    ErrorCode generated = ErrorCode::DependencyFailed;
    ErrorCode generatedSoft = ErrorCode::DependencyFailed;
    usize length = 0;
    FieldValue* read = nullptr;
    ErrorCode readResult = ErrorCode::DependencyFailed;
    PropertyPath readPath{};
    uint32 index = 0;
};

Probe MakeProbe(World& world, ObjectHandle owner, Box* box, Allocator* allocator,
                uint32 index = 0) noexcept {
    Probe probe;
    probe.world = &world;
    probe.owner = owner;
    probe.box = box;
    probe.allocator = allocator;
    probe.index = index;
    return probe;
}

// 같은 배열의 index 원소 삭제 후 같은 크기 원소 재삽입(ABA)·비우기(축소)·Registry 컨테이너 변경
// (StructRefArray·ObjectRefArray)·참조 쓰기를 시도한다 — 고정 중이므로 모두 Busy여야 한다.
void ReenterSameArray(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    ObjectRegistry& objects = probe.world->objects;
    const auto access = objects.OwnerAccess();
    const PathStep at[] = {At(NamesId, probe.index)};
    probe.removed = ErrorOf(PathRemoveElement(objects, access, probe.owner, PathOf(at)));
    FieldValue same = TextValue(*probe.allocator, "beta");
    probe.inserted = ErrorOf(PathInsertElement(objects, access, probe.owner, PathOf(at), &same));
    const PathStep names[] = {{NamesId}};
    probe.cleared = ErrorOf(PathClearArray(objects, access, probe.owner, PathOf(names)));
    const PathStep name[] = {{LabelFieldId}, {NameId}};
    probe.written = ErrorOf(PathWrite(objects, access, probe.owner, PathOf(name), same));
    auto added = objects.AddElement(access, probe.owner, probe.box->entries);
    probe.container = added ? ErrorCode::None : added.Error();
    probe.reference =
        probe.box->entries.IsEmpty()
            ? ErrorCode::None
            : ErrorOf(objects.SetElementReference(access, probe.owner, probe.box->entries, 0,
                                                  probe.box->entries[0].target, {}));
    probe.referenceInserted =
        ErrorOf(objects.InsertReferenceAt(access, probe.owner, probe.box->refs, 0, {}));
    probe.referenceSet =
        ErrorOf(objects.SetReferenceAt(access, probe.owner, probe.box->refs, 0, {}));
}

void DestroyOwner(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    probe.destroyed = ErrorOf(
        probe.world->objects.RequestDestroy(probe.world->objects.OwnerAccess(), probe.owner));
}

// 해제 콜백 중에 같은 배열을 경로로 읽는다(게시된 새 상태여야 한다).
void ReadDuringRelease(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    ObjectRegistry& objects = probe.world->objects;
    const auto access = objects.OwnerAccess();
    const PathStep names[] = {{NamesId}};
    auto length = PathArrayLength(objects, access, probe.owner, PathOf(names));
    probe.length = length ? length.Value() : ~usize(0);
    const PathStep at[] = {At(NamesId, probe.index)};
    probe.readResult = ErrorOf(PathRead(objects, access, probe.owner, PathOf(at), *probe.read));
}

// 해제 콜백 중에 probe.readPath를 읽는다.
void ReadPathDuringRelease(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    ObjectRegistry& objects = probe.world->objects;
    probe.readResult =
        ErrorOf(PathRead(objects, objects.OwnerAccess(), probe.owner, probe.readPath, *probe.read));
}

// 읽기의 out 할당 중 같은 필드를 쓴다(경로 쓰기와 생성 WriteGeneratedField).
void WriteDuringRead(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    ObjectRegistry& objects = probe.world->objects;
    const auto access = objects.OwnerAccess();
    FieldValue replacement = TextValue(*probe.allocator, "replaced while being read");
    const PathStep name[] = {{LabelFieldId}, {NameId}};
    probe.written = ErrorOf(PathWrite(objects, access, probe.owner, PathOf(name), replacement));
    probe.generated = ErrorOf(
        detail::WriteGeneratedField<Box>(objects, access, probe.owner, &Box::title, replacement));
}

// 최상위 생성 쓰기의 새 값 할당 중 같은 필드(title·icon)를 생성 write로 다시 쓴다.
void WriteTopDuringWrite(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    ObjectRegistry& objects = probe.world->objects;
    const auto access = objects.OwnerAccess();
    FieldValue text = TextValue(*probe.allocator, "reentrant title that should be rejected");
    probe.generated =
        ErrorOf(detail::WriteGeneratedField<Box>(objects, access, probe.owner, &Box::title, text));
    FieldValue soft = SoftValue(*probe.allocator, Guid{7, 7}, "Reentrant.Icon");
    probe.generatedSoft =
        ErrorOf(detail::WriteGeneratedField<Box>(objects, access, probe.owner, &Box::icon, soft));
}

void UnregisterDuringWrite(void* raw) noexcept {
    auto& probe = *static_cast<Probe*>(raw);
    probe.unregistered = ErrorOf(probe.world->types.Unregister(probe.world->labelToken));
    probe.unregisteredOwner = ErrorOf(probe.world->types.Unregister(probe.world->boxToken));
}

constexpr const char* LongText = "a value longer than the small string capacity";

} // namespace

TEST(PropertyPathReentry_AllocationCallbacksCannotChangeTheTarget) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(world.Register());
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto box = objects.NewObject<Box>(access, BoxId);
        REQUIRE(box);
        const ObjectHandle owner = box.Value().Handle();
        Box* pointer = nullptr;
        {
            auto borrowed = objects.Resolve<Box>(access, owner);
            REQUIRE(borrowed);
            pointer = Get(borrowed.Value());
        }

        const PathStep entry0[] = {At(EntriesId, 0)};
        REQUIRE(PathInsertElement(objects, access, owner, PathOf(entry0), nullptr));
        const PathStep name0[] = {At(NamesId, 0)};
        const PathStep name1[] = {At(NamesId, 1)};
        FieldValue alpha = TextValue(allocator, "alpha");
        FieldValue beta = TextValue(allocator, "beta");
        REQUIRE(PathInsertElement(objects, access, owner, PathOf(name0), &alpha));
        REQUIRE(PathInsertElement(objects, access, owner, PathOf(name1), &beta));

        // 원소 String 쓰기의 새 값 할당 중: 같은 원소 삭제 → 같은 크기 재삽입(ABA)·비우기·컨테이너
        // 변경·참조 쓰기는 모두 Busy이고 바깥 쓰기는 그대로 게시된다.
        Probe probe;
        probe.world = &world;
        probe.owner = owner;
        probe.box = pointer;
        probe.allocator = &allocator;
        probe.index = 1;
        FieldValue replacement = TextValue(allocator, LongText);
        world.hooked.ArmAllocation(&ReenterSameArray, &probe);
        REQUIRE(PathWrite(objects, access, owner, PathOf(name1), replacement));
        CHECK(probe.removed == ErrorCode::Busy && probe.inserted == ErrorCode::Busy);
        CHECK(probe.cleared == ErrorCode::Busy && probe.written == ErrorCode::Busy);
        CHECK(probe.container == ErrorCode::Busy && probe.reference == ErrorCode::Busy);
        CHECK(probe.referenceInserted == ErrorCode::Busy && probe.referenceSet == ErrorCode::Busy);
        REQUIRE(pointer->names.Size() == 2 && pointer->entries.Size() == 1);
        CHECK(pointer->refs.IsEmpty());
        CHECK(pointer->names[0].View() == StringView("alpha"));
        CHECK(pointer->names[1].View() == StringView(LongText));

        // Array<E> 삽입: 원소 할당 중(용량 안)과 새 버퍼 할당 중(용량 밖) 모두 같다.
        probe = Probe{};
        probe.world = &world;
        probe.owner = owner;
        probe.box = pointer;
        probe.allocator = &allocator;
        probe.index = 0;
        world.hooked.ArmAllocation(&ReenterSameArray, &probe);
        const PathStep name2[] = {At(NamesId, 2)};
        REQUIRE(PathInsertElement(objects, access, owner, PathOf(name2), &replacement));
        CHECK(probe.removed == ErrorCode::Busy && probe.inserted == ErrorCode::Busy &&
              probe.cleared == ErrorCode::Busy && probe.container == ErrorCode::Busy &&
              probe.referenceInserted == ErrorCode::Busy);
        REQUIRE(pointer->names.Size() == 3);
        FieldValue fourth = TextValue(allocator, "x");
        const PathStep name3[] = {At(NamesId, 3)};
        REQUIRE(PathInsertElement(objects, access, owner, PathOf(name3), &fourth));
        REQUIRE(pointer->names.Size() == pointer->names.Capacity());
        probe = Probe{};
        probe.world = &world;
        probe.owner = owner;
        probe.box = pointer;
        probe.allocator = &allocator;
        // 첫 할당은 원소 문자열, 두 번째가 새 버퍼다.
        world.hooked.ArmAllocation(&ReenterSameArray, &probe, 2);
        REQUIRE(PathInsertElement(objects, access, owner, PathOf(name0), &fourth));
        CHECK(probe.removed == ErrorCode::Busy && probe.inserted == ErrorCode::Busy &&
              probe.cleared == ErrorCode::Busy && probe.container == ErrorCode::Busy);
        REQUIRE(pointer->names.Size() == 5);
        CHECK(pointer->names[0].View() == StringView("x") &&
              pointer->names[1].View() == StringView("alpha"));

        // 소유자 파괴(고정은 막지 않는다): 할당 뒤 게시 직전 재확인이 Destroyed로 막는다(미게시).
        probe = Probe{};
        probe.world = &world;
        probe.owner = owner;
        const usize capacity = pointer->names.Capacity();
        world.hooked.ArmAllocation(&DestroyOwner, &probe);
        FieldValue doomed = TextValue(allocator, "doomed but long enough to allocate");
        const PathStep labelName[] = {{LabelFieldId}, {NameId}};
        CHECK(ErrorOf(PathWrite(objects, access, owner, PathOf(labelName), doomed)) ==
              ErrorCode::Destroyed);
        CHECK(probe.destroyed == ErrorCode::None);
        CHECK(pointer->label.name.IsEmpty());
        CHECK(pointer->names.Size() == 5 && pointer->names.Capacity() == capacity);
        box.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPathReentry_OwnerDestroyedDuringArrayGrowthLeavesArrayUnchanged) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(world.Register());
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto box = objects.NewObject<Box>(access, BoxId);
        REQUIRE(box);
        const ObjectHandle owner = box.Value().Handle();
        Box* pointer = nullptr;
        {
            auto borrowed = objects.Resolve<Box>(access, owner);
            REQUIRE(borrowed);
            pointer = Get(borrowed.Value());
        }

        for (uint32 index = 0; index < 4; ++index) {
            const PathStep at[] = {At(LabelsId, index)};
            REQUIRE(PathInsertElement(objects, access, owner, PathOf(at), nullptr));
        }

        const String* data = &pointer->labels[0].name;
        Probe probe;
        probe.world = &world;
        probe.owner = owner;
        world.hooked.ArmAllocation(&DestroyOwner, &probe);
        const PathStep at[] = {At(LabelsId, 2)};
        CHECK(ErrorOf(PathInsertElement(objects, access, owner, PathOf(at), nullptr)) ==
              ErrorCode::Destroyed);
        CHECK(probe.destroyed == ErrorCode::None);
        CHECK(pointer->labels.Size() == 4 && &pointer->labels[0].name == data);
        box.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPathReentry_OldBufferReleaseSeesThePublishedArray) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(world.Register());
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto box = objects.NewObject<Box>(access, BoxId);
        REQUIRE(box);
        const ObjectHandle owner = box.Value().Handle();
        Box* pointer = nullptr;
        {
            auto borrowed = objects.Resolve<Box>(access, owner);
            REQUIRE(borrowed);
            pointer = Get(borrowed.Value());
        }

        const char* values[] = {"zero", "one", "two", "three"};
        for (uint32 index = 0; index < 4; ++index) {
            const PathStep at[] = {At(NamesId, index)};
            FieldValue value = TextValue(allocator, values[index]);
            REQUIRE(PathInsertElement(objects, access, owner, PathOf(at), &value));
        }

        REQUIRE(pointer->names.Size() == pointer->names.Capacity());
        // 삽입이 새 버퍼를 게시한 뒤 옛 버퍼를 반환한다: 그 해제 콜백은 새 길이·새 원소를 본다.
        FieldValue read(allocator);
        Probe probe;
        probe.world = &world;
        probe.owner = owner;
        probe.read = &read;
        probe.index = 4;
        world.hooked.ArmRelease(pointer->names.Data(), &ReadDuringRelease, &probe);
        FieldValue tail = TextValue(allocator, "four");
        const PathStep at4[] = {At(NamesId, 4)};
        REQUIRE(PathInsertElement(objects, access, owner, PathOf(at4), &tail));
        CHECK(probe.length == 5 && probe.readResult == ErrorCode::None);
        CHECK(read.string.View() == StringView("four"));

        // 삭제는 원소를 빼서 게시한 뒤 소멸한다: 원소 버퍼의 해제 콜백은 줄어든 배열을 본다.
        FieldValue removedValue = TextValue(allocator, LongText);
        const PathStep at0[] = {At(NamesId, 0)};
        REQUIRE(PathWrite(objects, access, owner, PathOf(at0), removedValue));
        probe = Probe{};
        probe.world = &world;
        probe.owner = owner;
        probe.read = &read;
        probe.index = 0;
        world.hooked.ArmRelease(pointer->names[0].Data(), &ReadDuringRelease, &probe);
        REQUIRE(PathRemoveElement(objects, access, owner, PathOf(at0)));
        CHECK(probe.length == 4 && probe.readResult == ErrorCode::None);
        CHECK(read.string.View() == StringView("one"));

        // 비우기는 버퍼째 떼어 게시한 뒤 반환한다(용량까지): 해제 콜백은 빈 배열을 본다.
        probe = Probe{};
        probe.world = &world;
        probe.owner = owner;
        probe.read = &read;
        probe.index = 0;
        world.hooked.ArmRelease(pointer->names.Data(), &ReadDuringRelease, &probe);
        const PathStep names[] = {{NamesId}};
        REQUIRE(PathClearArray(objects, access, owner, PathOf(names)));
        CHECK(probe.length == 0 && probe.readResult == ErrorCode::InvalidArgument);
        CHECK(pointer->names.Capacity() == 0);
        box.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPathReentry_WritesDuringReadAreBusy) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(world.Register());
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto box = objects.NewObject<Box>(access, BoxId);
        REQUIRE(box);
        const ObjectHandle owner = box.Value().Handle();
        const PathStep labelName[] = {{LabelFieldId}, {NameId}};
        const PathStep title[] = {{TitleId}};
        FieldValue original = TextValue(allocator, "original value that needs a heap buffer");
        REQUIRE(PathWrite(objects, access, owner, PathOf(labelName), original));
        REQUIRE(PathWrite(objects, access, owner, PathOf(title), original));

        // 경로 읽기: out 할당 콜백의 같은 필드 쓰기는 Busy이고 읽은 값은 원본이다.
        HookAllocator outAllocator(allocator);
        Probe probe;
        probe.world = &world;
        probe.owner = owner;
        probe.allocator = &allocator;
        FieldValue read(outAllocator);
        outAllocator.ArmAllocation(&WriteDuringRead, &probe);
        REQUIRE(PathRead(objects, access, owner, PathOf(labelName), read));
        CHECK(probe.written == ErrorCode::Busy && probe.generated == ErrorCode::Busy);
        CHECK(read.string.View() == original.string.View());

        // 현행 결함 회귀: 생성 ReadGeneratedField(등록된 read)도 복사를 고정 아래 한다 — 할당
        // 콜백이 같은 필드를 WriteGeneratedField로 바꾸면(해제된 버퍼 읽기 대신) Busy다.
        probe = Probe{};
        probe.world = &world;
        probe.owner = owner;
        probe.allocator = &allocator;
        auto lease = world.types.Acquire(BoxId);
        REQUIRE(lease);
        FieldValue direct(outAllocator);
        outAllocator.ArmAllocation(&WriteDuringRead, &probe);
        REQUIRE(lease.Value().Ops().fields[0].read(objects, access, owner, direct));
        CHECK(probe.generated == ErrorCode::Busy);
        CHECK(direct.string.View() == original.string.View());
        probe = Probe{};
        probe.world = &world;
        probe.owner = owner;
        probe.allocator = &allocator;
        FieldValue topLevel(outAllocator);
        outAllocator.ArmAllocation(&WriteDuringRead, &probe);
        REQUIRE(PathRead(objects, access, owner, PathOf(title), topLevel));
        CHECK(probe.generated == ErrorCode::Busy);
        CHECK(topLevel.string.View() == original.string.View());
        // 읽기가 끝나면 다시 쓸 수 있다.
        FieldValue next = TextValue(allocator, "next");
        CHECK(PathWrite(objects, access, owner, PathOf(title), next));
        lease.Value().Reset();
        box.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPathReentry_LeasesHeldDuringOperationKeepUnregisterBusy) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(world.Register());
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto box = objects.NewObject<Box>(access, BoxId);
        REQUIRE(box);
        const ObjectHandle owner = box.Value().Handle();
        Probe probe;
        probe.world = &world;
        world.hooked.ArmAllocation(&UnregisterDuringWrite, &probe);
        const PathStep labelName[] = {{LabelFieldId}, {NameId}};
        FieldValue value = TextValue(allocator, LongText);
        REQUIRE(PathWrite(objects, access, owner, PathOf(labelName), value));
        CHECK(probe.unregistered == ErrorCode::Busy && probe.unregisteredOwner == ErrorCode::Busy);
        box.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(world.types.Unregister(world.boxToken));
        CHECK(world.types.Unregister(world.labelToken));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPathReentry_TopLevelGeneratedWritesPinWhileAllocating) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(world.Register());
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto box = objects.NewObject<Box>(access, BoxId);
        REQUIRE(box);
        const ObjectHandle owner = box.Value().Handle();
        Box* pointer = Pointer(objects, owner);
        auto lease = world.types.Acquire(BoxId);
        REQUIRE(lease);
        const RuntimeFieldOps& title = lease.Value().Ops().fields[0];
        REQUIRE(title.fieldId == TitleId);

        // 등록된 생성 String write: 새 값 할당 중 같은 필드의 재진입 생성 write는
        // Busy이고(덮어쓰기 유실 없음) 바깥 쓰기가 게시된다.
        Probe probe = MakeProbe(world, owner, pointer, &allocator);
        FieldValue outer = TextValue(allocator, LongText);
        world.hooked.ArmAllocation(&WriteTopDuringWrite, &probe);
        REQUIRE(title.write(objects, access, owner, outer));
        CHECK(probe.generated == ErrorCode::Busy && probe.generatedSoft == ErrorCode::Busy);
        CHECK(pointer->title.View() == StringView(LongText) && pointer->icon.IsNull());
        CHECK(objects.Stats().encodePins == 0);

        // Soft도 같다(경로 쓰기 — previous는 바깥 쓰기 직전 값).
        probe = MakeProbe(world, owner, pointer, &allocator);
        FieldValue soft = SoftValue(allocator, Guid{1, 2}, "Outer.Icon.Path.Long.Enough");
        FieldValue previous(allocator);
        const PathStep iconPath[] = {{IconId}};
        world.hooked.ArmAllocation(&WriteTopDuringWrite, &probe);
        REQUIRE(PathWrite(objects, access, owner, PathOf(iconPath), soft, &previous));
        CHECK(probe.generated == ErrorCode::Busy && probe.generatedSoft == ErrorCode::Busy);
        CHECK(previous.kind == ValueKind::SoftObjectReference && previous.guid == Guid{} &&
              previous.string.IsEmpty());
        CHECK(pointer->icon.asset.value == (Guid{1, 2}) &&
              pointer->icon.objectPath.View() == StringView("Outer.Icon.Path.Long.Enough"));
        CHECK(pointer->title.View() == StringView(LongText));
        CHECK(objects.Stats().encodePins == 0);

        // 생성 write의 옛 버퍼 해제 콜백은 고정이 풀린 뒤라 새 값을 읽고 다시 쓸 수 있다.
        FieldValue read(allocator);
        probe = MakeProbe(world, owner, pointer, &allocator);
        probe.read = &read;
        const PathStep titlePath[] = {{TitleId}};
        probe.readPath = PathOf(titlePath);
        world.hooked.ArmRelease(pointer->title.Data(), &ReadPathDuringRelease, &probe);
        FieldValue next = TextValue(allocator, "next title value with a heap buffer");
        REQUIRE(title.write(objects, access, owner, next));
        CHECK(probe.readResult == ErrorCode::None && read.string.View() == next.string.View());
        lease.Value().Reset();
        box.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPathReentry_OwnerDestroyedBeforePublishIsNotPublished) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(world.Register());
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto reading = objects.NewObject<Box>(access, BoxId);
        auto removing = objects.NewObject<Box>(access, BoxId);
        auto inserting = objects.NewObject<Box>(access, BoxId);
        REQUIRE(reading && removing && inserting);

        // 읽기: out 할당 콜백의 소유자 파괴 → 게시 직전 재확인이 Destroyed, out은 그대로다.
        const ObjectHandle readOwner = reading.Value().Handle();
        const PathStep labelName[] = {{LabelFieldId}, {NameId}};
        FieldValue name = TextValue(allocator, LongText);
        REQUIRE(PathWrite(objects, access, readOwner, PathOf(labelName), name));
        HookAllocator outAllocator(allocator);
        FieldValue out = TextValue(outAllocator, "keep");
        Probe probe = MakeProbe(world, readOwner, nullptr, &allocator);
        outAllocator.ArmAllocation(&DestroyOwner, &probe);
        PropertyPathFailure failure;
        CHECK(ErrorOf(PathRead(objects, access, readOwner, PathOf(labelName), out, &failure)) ==
              ErrorCode::Destroyed);
        CHECK(probe.destroyed == ErrorCode::None && failure.step == 1);
        CHECK(out.string.View() == StringView("keep") && objects.Stats().encodePins == 0);

        // 다른 allocator로의 removed 복사(CaptureText) 중 소유자 파괴 → 원소를 빼지 않는다.
        const ObjectHandle removeOwner = removing.Value().Handle();
        Box* removePointer = Pointer(objects, removeOwner);
        const PathStep name0[] = {At(NamesId, 0)};
        const PathStep name1[] = {At(NamesId, 1)};
        REQUIRE(PathInsertElement(objects, access, removeOwner, PathOf(name0), &name));
        FieldValue tail = TextValue(allocator, "tail");
        REQUIRE(PathInsertElement(objects, access, removeOwner, PathOf(name1), &tail));
        HookAllocator removedAllocator(allocator);
        FieldValue removed = TextValue(removedAllocator, "keep");
        probe = MakeProbe(world, removeOwner, nullptr, &allocator);
        removedAllocator.ArmAllocation(&DestroyOwner, &probe);
        CHECK(ErrorOf(PathRemoveElement(objects, access, removeOwner, PathOf(name0), &removed)) ==
              ErrorCode::Destroyed);
        CHECK(probe.destroyed == ErrorCode::None);
        CHECK(removed.string.View() == StringView("keep") && objects.Stats().encodePins == 0);
        CHECK(removePointer->names.Size() == 2 &&
              removePointer->names[0].View() == StringView(LongText));

        // String 원소의 용량 안 삽입: 원소 할당 중 소유자 파괴 → 삽입하지 않는다.
        const ObjectHandle insertOwner = inserting.Value().Handle();
        Box* insertPointer = Pointer(objects, insertOwner);
        FieldValue first = TextValue(allocator, "first");
        REQUIRE(PathInsertElement(objects, access, insertOwner, PathOf(name0), &first));
        REQUIRE(insertPointer->names.Size() < insertPointer->names.Capacity());
        probe = MakeProbe(world, insertOwner, nullptr, &allocator);
        world.hooked.ArmAllocation(&DestroyOwner, &probe);
        CHECK(ErrorOf(PathInsertElement(objects, access, insertOwner, PathOf(name0), &name)) ==
              ErrorCode::Destroyed);
        CHECK(probe.destroyed == ErrorCode::None && objects.Stats().encodePins == 0);
        CHECK(insertPointer->names.Size() == 1 &&
              insertPointer->names[0].View() == StringView("first"));
        reading.Value().Reset();
        removing.Value().Reset();
        inserting.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPathReentry_WriteReleasesOldBufferAfterPublishing) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(world.Register());
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto box = objects.NewObject<Box>(access, BoxId);
        REQUIRE(box);
        const ObjectHandle owner = box.Value().Handle();
        Box* pointer = Pointer(objects, owner);
        const PathStep name0[] = {At(NamesId, 0)};
        const PathStep labelName[] = {{LabelFieldId}, {NameId}};
        FieldValue old = TextValue(allocator, LongText);
        REQUIRE(PathInsertElement(objects, access, owner, PathOf(name0), &old));
        REQUIRE(PathWrite(objects, access, owner, PathOf(labelName), old));
        // 원소·멤버 String 쓰기의 옛 버퍼 해제 콜백은 게시된 새 값을 본다(고정도 풀렸다).
        const PropertyPath paths[] = {PathOf(name0), PathOf(labelName)};
        const void* buffers[] = {pointer->names[0].Data(), pointer->label.name.Data()};
        for (usize index = 0; index < 2; ++index) {
            FieldValue read(allocator);
            Probe probe = MakeProbe(world, owner, pointer, &allocator);
            probe.read = &read;
            probe.readPath = paths[index];
            world.hooked.ArmRelease(buffers[index], &ReadPathDuringRelease, &probe);
            FieldValue next = TextValue(allocator, "the new value with a heap buffer");
            REQUIRE(PathWrite(objects, access, owner, paths[index], next));
            CHECK(probe.readResult == ErrorCode::None);
            CHECK(read.string.View() == next.string.View());
        }

        box.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPathReentry_TopLevelPreviousValidatesBeforeReading) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(world.Register());
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto box = objects.NewObject<Box>(access, BoxId);
        REQUIRE(box);
        const ObjectHandle owner = box.Value().Handle();
        // ReadOnly 최상위 String에 previous + 할당 실패: 쓰기 검증(ReadOnly)이 previous
        // read·할당보다 먼저라 OutOfMemory가 아니라 Unsupported(ReadOnly)다. previous는
        // 그대로다.
        ForwardAllocator other(world.hooked);
        FieldValue previous = TextValue(other, "keep");
        FieldValue value = TextValue(allocator, LongText);
        const PathStep caption[] = {{CaptionId}};
        PropertyPathFailure failure;
        world.hooked.FailAfter(1);
        CHECK(ErrorOf(PathWrite(objects, access, owner, PathOf(caption), value, &previous,
                                &failure)) == ErrorCode::Unsupported);
        world.hooked.FailAfter(0);
        CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 0);
        CHECK(previous.string.View() == StringView("keep"));
        // 고정 중이면 previous read 전에 Busy다(할당이 없다).
        auto pin = objects.PinForEncode(access, owner);
        REQUIRE(pin);
        const uint64 before = world.hooked.allocations;
        const PathStep title[] = {{TitleId}};
        CHECK(ErrorOf(PathWrite(objects, access, owner, PathOf(title), value, &previous,
                                &failure)) == ErrorCode::Busy);
        CHECK(world.hooked.allocations == before && failure.reason == PropertyPathError::None);
        CHECK(previous.string.View() == StringView("keep"));
        pin.Value().Reset();
        CHECK(objects.Stats().encodePins == 0);
        box.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

// 실패 스윕의 대상 내용 사본이다(최상위 String·Soft, 멤버 String·Soft, 원소 String·Soft,
// Array<S> 원소, StructRefArray 원소의 이름·참조, ObjectRefArray 원소).
struct Snapshot {
    explicit Snapshot(Allocator& allocator) noexcept
        : text(allocator), handles(allocator), guids(allocator) {
    }

    String text;
    Array<ObjectHandle> handles;
    Array<Guid> guids;
};

bool AppendText(String& text, StringView value) noexcept {
    return text.TryAppend(value) && text.TryAppend("|");
}

void Capture(const Box& box, Snapshot& snapshot) noexcept {
    String& text = snapshot.text;
    bool ok = AppendText(text, box.title.View()) && AppendText(text, box.caption.View()) &&
              AppendText(text, box.icon.objectPath.View()) &&
              AppendText(text, box.label.name.View()) &&
              AppendText(text, box.label.icon.objectPath.View()) &&
              snapshot.guids.TryPushBack(box.icon.asset.value) &&
              snapshot.guids.TryPushBack(box.label.icon.asset.value);
    for (usize index = 0; ok && index < box.names.Size(); ++index) {
        ok = AppendText(text, box.names[index].View());
    }

    ok = ok && AppendText(text, "labels");
    for (usize index = 0; ok && index < box.labels.Size(); ++index) {
        ok = AppendText(text, box.labels[index].name.View()) &&
             AppendText(text, box.labels[index].icon.objectPath.View());
    }

    ok = ok && AppendText(text, "entries");
    for (usize index = 0; ok && index < box.entries.Size(); ++index) {
        ok = AppendText(text, box.entries[index].name.View()) &&
             snapshot.handles.TryPushBack(box.entries[index].target.Handle());
    }

    ok = ok && AppendText(text, "softs");
    for (usize index = 0; ok && index < box.softs.Size(); ++index) {
        ok = AppendText(text, box.softs[index].objectPath.View()) &&
             snapshot.guids.TryPushBack(box.softs[index].asset.value);
    }

    ok = ok && AppendText(text, "refs");
    for (usize index = 0; ok && index < box.refs.Size(); ++index) {
        ok = static_cast<bool>(snapshot.handles.TryPushBack(box.refs[index]));
    }

    if (!ok) {
        platform::Fatal("snapshot allocation failed");
    }
}

bool Same(const Snapshot& left, const Snapshot& right) noexcept {
    if (left.text.View() != right.text.View() || left.handles.Size() != right.handles.Size() ||
        left.guids.Size() != right.guids.Size()) {
        return false;
    }

    for (usize index = 0; index < left.handles.Size(); ++index) {
        if (!(left.handles[index] == right.handles[index])) {
            return false;
        }
    }

    for (usize index = 0; index < left.guids.Size(); ++index) {
        if (!(left.guids[index] == right.guids[index])) {
            return false;
        }
    }

    return true;
}

enum class SweepOperation : uint8 {
    WriteMember,
    WriteSoftMember,
    WriteElementWithPrevious,
    WriteSoftElementWithPrevious,
    InsertInCapacity,
    InsertGrowing,
    InsertStruct,
    InsertRegistryElement,
    InsertReference,
    WriteRegistryElementMember,
    RemoveWithRemoved,
    RemoveSoftWithRemoved,
    RemoveSameAllocator,
    WriteTop,
    WriteTopWithPrevious,
    WriteTopSoftWithPrevious,
    ReadTop,
    ReadMember,
    Format,
    Count
};

// 할당하지 않는 연산이다(실패 지점이 없어야 한다 — 같은 allocator의 removed는 옛 버퍼를
// 옮긴다).
bool AllocationFree(SweepOperation operation) noexcept {
    return operation == SweepOperation::RemoveSameAllocator;
}

// fresh box에 시작 내용을 만든다(이름 3개 — 용량 4, ObjectRefArray는 용량 4를 null로
// 채운다).
bool Prepare(World& world, ObjectHandle owner, ObjectHandle target, Allocator& allocator) noexcept {
    ObjectRegistry& objects = world.objects;
    const auto access = objects.OwnerAccess();
    const char* values[] = {"first element with a long body", "second", "third element body"};
    for (uint32 index = 0; index < 3; ++index) {
        const PathStep at[] = {At(NamesId, index)};
        FieldValue value = TextValue(allocator, values[index]);
        if (!PathInsertElement(objects, access, owner, PathOf(at), &value)) {
            return false;
        }
    }

    // Array<Label>은 용량 4를 채운다(기본 원소는 할당하지 않으므로 삽입이 새 버퍼를 할당하게).
    for (uint32 index = 0; index < 4; ++index) {
        const PathStep label[] = {At(LabelsId, index)};
        const PathStep labelName[] = {At(LabelsId, index), {NameId}};
        FieldValue name = TextValue(allocator, index == 0 ? "label zero" : "label other");
        if (!PathInsertElement(objects, access, owner, PathOf(label), nullptr) ||
            !PathWrite(objects, access, owner, PathOf(labelName), name)) {
            return false;
        }
    }

    for (uint32 index = 0; index < 2; ++index) {
        const PathStep soft[] = {At(SoftsId, index)};
        FieldValue softValue = SoftValue(allocator, Guid{5, index + 1u}, "Soft.Element.Path");
        if (!PathInsertElement(objects, access, owner, PathOf(soft), &softValue)) {
            return false;
        }
    }

    for (uint32 index = 0; index < 4; ++index) {
        const PathStep at[] = {At(RefsId, index)};
        FieldValue null(allocator);
        null.kind = ValueKind::ObjectReference;
        if (!PathInsertElement(objects, access, owner, PathOf(at), &null)) {
            return false;
        }
    }

    const PathStep title[] = {{TitleId}};
    const PathStep icon[] = {{IconId}};
    const PathStep labelName[] = {{LabelFieldId}, {NameId}};
    const PathStep labelIcon[] = {{LabelFieldId}, {LabelIconId}};
    const PathStep entry0[] = {At(EntriesId, 0)};
    const PathStep entryName[] = {At(EntriesId, 0), {EntryNameId}};
    const PathStep entryTarget[] = {At(EntriesId, 0), {TargetId}};
    FieldValue titleValue = TextValue(allocator, "title value with a heap buffer");
    FieldValue iconValue = SoftValue(allocator, Guid{3, 3}, "Top.Icon.Path");
    FieldValue label = TextValue(allocator, "label value with a heap buffer");
    FieldValue labelIconValue = SoftValue(allocator, Guid{4, 4}, "Label.Icon.Path");
    FieldValue entry = TextValue(allocator, "entry name with a heap buffer");
    FieldValue reference(allocator);
    reference.kind = ValueKind::ObjectReference;
    reference.reference = target;
    return PathWrite(objects, access, owner, PathOf(title), titleValue) &&
           PathWrite(objects, access, owner, PathOf(icon), iconValue) &&
           PathWrite(objects, access, owner, PathOf(labelName), label) &&
           PathWrite(objects, access, owner, PathOf(labelIcon), labelIconValue) &&
           PathInsertElement(objects, access, owner, PathOf(entry0), nullptr) &&
           PathWrite(objects, access, owner, PathOf(entryName), entry) &&
           PathWrite(objects, access, owner, PathOf(entryTarget), reference);
}

struct SweepOutputs {
    FieldValue* other = nullptr;
    FieldValue* same = nullptr;
    String* formatted = nullptr;
};

Result<void> RunSweep(World& world, ObjectHandle owner, ObjectHandle target,
                      SweepOperation operation, Allocator& allocator,
                      SweepOutputs outputs) noexcept {
    ObjectRegistry& objects = world.objects;
    const auto access = objects.OwnerAccess();
    FieldValue value = TextValue(allocator, LongText);
    FieldValue soft = SoftValue(allocator, Guid{9, 9}, "A.Longer.Soft.Object.Path.Value");
    FieldValue reference(allocator);
    reference.kind = ValueKind::ObjectReference;
    reference.reference = target;
    const PathStep labelName[] = {{LabelFieldId}, {NameId}};
    const PathStep labelIcon[] = {{LabelFieldId}, {LabelIconId}};
    const PathStep name1[] = {At(NamesId, 1)};
    const PathStep name3[] = {At(NamesId, 3)};
    const PathStep name0[] = {At(NamesId, 0)};
    const PathStep soft0[] = {At(SoftsId, 0)};
    const PathStep label0[] = {At(LabelsId, 0)};
    const PathStep entry0[] = {At(EntriesId, 0)};
    const PathStep ref0[] = {At(RefsId, 0)};
    const PathStep entryName[] = {At(EntriesId, 0), {EntryNameId}};
    const PathStep title[] = {{TitleId}};
    const PathStep icon[] = {{IconId}};
    const PathStep formatPath[] = {At(LabelsId, 1), {LabelIconId}};
    FieldValue& other = *outputs.other;
    switch (operation) {
    case SweepOperation::WriteMember:
        return PathWrite(objects, access, owner, PathOf(labelName), value, &other);
    case SweepOperation::WriteSoftMember:
        return PathWrite(objects, access, owner, PathOf(labelIcon), soft, &other);
    case SweepOperation::WriteElementWithPrevious:
        return PathWrite(objects, access, owner, PathOf(name1), value, &other);
    case SweepOperation::WriteSoftElementWithPrevious:
        return PathWrite(objects, access, owner, PathOf(soft0), soft, &other);
    case SweepOperation::InsertInCapacity:
        return PathInsertElement(objects, access, owner, PathOf(name3), &value);
    case SweepOperation::InsertGrowing:
        // 시험이 용량 4를 미리 채웠다(그 할당은 스윕 전) — 새 버퍼가 필요하다.
        return PathInsertElement(objects, access, owner, PathOf(name0), &value);
    case SweepOperation::InsertStruct:
        return PathInsertElement(objects, access, owner, PathOf(label0), nullptr);
    case SweepOperation::InsertRegistryElement:
        return PathInsertElement(objects, access, owner, PathOf(entry0), nullptr);
    case SweepOperation::InsertReference:
        return PathInsertElement(objects, access, owner, PathOf(ref0), &reference);
    case SweepOperation::WriteRegistryElementMember:
        return PathWrite(objects, access, owner, PathOf(entryName), value, &other);
    case SweepOperation::RemoveWithRemoved:
        return PathRemoveElement(objects, access, owner, PathOf(name0), &other);
    case SweepOperation::RemoveSoftWithRemoved:
        return PathRemoveElement(objects, access, owner, PathOf(soft0), &other);
    case SweepOperation::RemoveSameAllocator:
        return PathRemoveElement(objects, access, owner, PathOf(name0), outputs.same);
    case SweepOperation::WriteTop:
        return PathWrite(objects, access, owner, PathOf(title), value);
    case SweepOperation::WriteTopWithPrevious:
        return PathWrite(objects, access, owner, PathOf(title), value, &other);
    case SweepOperation::WriteTopSoftWithPrevious:
        return PathWrite(objects, access, owner, PathOf(icon), soft, &other);
    case SweepOperation::ReadTop:
        return PathRead(objects, access, owner, PathOf(title), other);
    case SweepOperation::ReadMember:
        return PathRead(objects, access, owner, PathOf(labelName), other);
    case SweepOperation::Format: {
        auto lease = world.types.Acquire(BoxId);
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        return FormatPropertyPath(lease.Value(), PathOf(formatPath), *outputs.formatted);
    }
    default:
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
}

} // namespace

TEST(PropertyPathReentry_EveryAllocationFailureLeavesContentsUnchanged) {
    SystemAllocator allocator;
    for (uint32 raw = 0; raw < static_cast<uint32>(SweepOperation::Count); ++raw) {
        const auto operation = static_cast<SweepOperation>(raw);
        uint32 failures = 0;
        bool succeeded = false;
        for (uint64 failAt = 1; failAt < 64 && !succeeded; ++failAt) {
            {
                World world(allocator);
                REQUIRE(world.Register());
                ObjectRegistry& objects = world.objects;
                const auto access = objects.OwnerAccess();
                auto box = objects.NewObject<Box>(access, BoxId);
                auto target = objects.NewObject<Box>(access, BoxId);
                REQUIRE(box && target);
                const ObjectHandle owner = box.Value().Handle();
                REQUIRE(Prepare(world, owner, target.Value().Handle(), allocator));
                Box* pointer = Pointer(objects, owner);
                if (operation == SweepOperation::InsertGrowing) {
                    FieldValue filler = TextValue(allocator, "x");
                    const PathStep at[] = {At(NamesId, 3)};
                    REQUIRE(PathInsertElement(objects, access, owner, PathOf(at), &filler));
                    REQUIRE(pointer->names.Size() == pointer->names.Capacity());
                }

                // 옛 값·삭제 값·읽기 결과는 값이 담긴 채로 객체와 다른 allocator(같은 할당
                // 계수 — 복사 할당도 스윕한다)나 같은 allocator로 받는다. Format 결과도
                // 다른 allocator다.
                ForwardAllocator otherAllocator(world.hooked);
                FieldValue other = TextValue(otherAllocator, "untouched output");
                FieldValue same = TextValue(world.hooked, "untouched output");
                String formatted(otherAllocator);
                REQUIRE(formatted.TryAssign("keep"));
                Snapshot before(allocator);
                Capture(*pointer, before);
                world.hooked.FailAfter(failAt);
                auto result = RunSweep(world, owner, target.Value().Handle(), operation, allocator,
                                       SweepOutputs{&other, &same, &formatted});
                world.hooked.FailAfter(0);
                CHECK(objects.Stats().encodePins == 0);
                succeeded = static_cast<bool>(result);
                if (!succeeded) {
                    ++failures;
                    CHECK(result.Error() == ErrorCode::OutOfMemory);
                    Snapshot after(allocator);
                    Capture(*pointer, after);
                    CHECK(Same(before, after));
                    CHECK(other.kind == ValueKind::Utf8String &&
                          other.string.View() == StringView("untouched output"));
                    CHECK(same.string.View() == StringView("untouched output"));
                    CHECK(formatted.View() == StringView("keep"));
                }

                box.Value().Reset();
                target.Value().Reset();
                REQUIRE(objects.Collect(access));
                CHECK(objects.Stats().liveObjects == 0);
            }

            CHECK(allocator.Stats().liveAllocations == 0);
        }

        CHECK(succeeded);
        // 할당하는 연산은 적어도 한 번 실패 경로를 지났고, 할당하지 않는 연산은 실패 지점이
        // 없다.
        CHECK(AllocationFree(operation) ? failures == 0 : failures > 0);
    }
}
