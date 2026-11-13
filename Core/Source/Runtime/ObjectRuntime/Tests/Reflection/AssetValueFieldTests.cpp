#include <ReflectionTestFixtures/ReflectionFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Platform/Fatal.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// B3a 값 종류(AssetId·Sha256Digest·SoftObjectRef)의 필드 접근 시험이다(Docs/Design/
// ReflectionFoundations.ko.md 3.1–3.3·3.5–3.6절): 수동 등록 규칙, AssetId 문자열 표기,
// SoftObjectRef의 이동·복사 의미, 소프트 참조 쓰기의 staged·미등록 callback 검증, OOM·논리 파괴
// 원자성, 읽기 전용과 값 검사의 순서.
using namespace rocks;
using reflection_fixture::AssetFields;
using reflection_fixture::Node;

namespace {

usize SchemaIndex(const TypeSchema& schema, StringView name) noexcept {
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        const FieldSchema& field = schema.fields[index];
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return index;
        }
    }

    platform::Fatal("fixture field missing");
}

const RuntimeFieldOps& FieldOps(Span<const RuntimeFieldOps> fields, StringView name) noexcept {
    const Guid id = GeneratedType<AssetFields>::Schema()
                        .fields[SchemaIndex(GeneratedType<AssetFields>::Schema(), name)]
                        .id;
    for (const RuntimeFieldOps& field : fields) {
        if (field.fieldId == id) {
            return field;
        }
    }

    platform::Fatal("fixture field ops missing");
}

constexpr AssetId OldAsset{Guid{0x0a0a0a0a0a0a0a0aull, 0x0b0b0b0b0b0b0b0bull}};
constexpr AssetId NewAsset{Guid{0x0c0c0c0c0c0c0c0cull, 0x0d0d0d0d0d0d0d0dull}};
constexpr AssetId SampleAsset{Guid{0x0123456789abcdefull, 0xfedcba9876543210ull}};

// 4 KiB 상한을 1 byte 넘는 경로다.
StringView LongPath() noexcept {
    static char path[SoftObjectRef<>::MaxObjectPathBytes + 1];
    for (char& c : path) {
        c = 'a';
    }

    return StringView(path, sizeof(path));
}

void SetSoft(FieldValue& value, AssetId asset, StringView path) noexcept {
    value.kind = ValueKind::SoftObjectReference;
    value.guid = asset.value;
    if (!value.string.TryAssign(path)) {
        platform::Fatal("soft value path");
    }
}

bool SoftEquals(const SoftObjectRef<Node>& ref, AssetId asset, StringView path) noexcept {
    return ref.asset == asset && ref.objectPath.View() == path;
}

// 등록된 icon 필드를 읽어 (asset, path)와 같은지 본다.
bool IconIs(ObjectRegistry& objects, const TypeLease& lease, ObjectHandle handle, AssetId asset,
            StringView path, Allocator& allocator) noexcept {
    FieldValue read(allocator);
    return FieldOps(lease.Ops().fields, "icon")
               .read(objects, objects.OwnerAccess(), handle, read) &&
           read.kind == ValueKind::SoftObjectReference && read.guid == asset.value &&
           read.string.View() == path;
}

bool DestroyedIconIs(AssetId asset, StringView path) noexcept {
    return AssetFields::destroyedIcon == asset &&
           StringView(AssetFields::destroyedIconPath, AssetFields::destroyedIconPathBytes) == path;
}

// 할당 또는 해제 callback에서 대상 객체를 논리 파괴한다(ReflectionTests의 String 시험과 같은 방식).
class DestroyCallbackAllocator final : public Allocator {
public:
    explicit DestroyCallbackAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (onAllocate) {
            onAllocate = false;
            destruction = registry->RequestDestroy(registry->OwnerAccess(), target).Error();
        }

        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (onFree) {
            onFree = false;
            destruction = registry->RequestDestroy(registry->OwnerAccess(), target).Error();
        }

        return backing_.Deallocate(address);
    }

    ObjectRegistry* registry = nullptr;
    ObjectHandle target{};
    ErrorCode destruction = ErrorCode::DependencyFailed;
    bool onAllocate = false;
    bool onFree = false;

private:
    Allocator& backing_;
};

using Mutation = void (*)(FieldSchema&) noexcept;

} // namespace

// 생성 스키마를 복사해 필드 하나만 바꿔 등록한다. 바꾸지 않은 복사본은 등록되므로 거부 이유는 바꾼
// 곳이다.
TEST(AssetValueFields_ManualRegistrationEnforcesValueTypeRules) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    const TypeSchema& generated = GeneratedType<AssetFields>::Schema();
    REQUIRE(generated.fields.Size() == 4);
    const usize source = SchemaIndex(generated, "source");
    const usize hash = SchemaIndex(generated, "hash");
    const usize icon = SchemaIndex(generated, "icon");
    const ValueKind beyondLast =
        static_cast<ValueKind>(static_cast<uint8>(ValueKind::Digest256) + 1);
    struct Case {
        usize field;
        Mutation mutate;
    };

    const Case cases[] = {
        // 소프트 참조는 GC 간선이 아니다.
        {icon, [](FieldSchema& field) noexcept { field.kind = FieldKind::ObjectReference; }},
        // value.type은 참조·소프트 참조에만 쓴다.
        {source, [](FieldSchema& field) noexcept { field.value.type = TypeIdOf<Node>(); }},
        {hash, [](FieldSchema& field) noexcept { field.value.type = Guid{1, 1}; }},
        // Required는 대상 타입이 있는 참조에만 쓴다.
        {source,
         [](FieldSchema& field) noexcept {
             field.flags |= static_cast<uint32>(FieldFlags::Required);
         }},
        {hash,
         [](FieldSchema& field) noexcept {
             field.flags |= static_cast<uint32>(FieldFlags::Required);
         }},
        // 배열 자리(element·maxElements)는 B5 전까지 비어 있어야 한다.
        {source, [](FieldSchema& field) noexcept { field.element.kind = ValueKind::Bool; }},
        {icon, [](FieldSchema& field) noexcept { field.element.type = Guid{1, 1}; }},
        {source, [](FieldSchema& field) noexcept { field.maxElements = 1; }},
        // Enum·Struct·Array는 B3b–B5 전까지 받지 않는다.
        {source, [](FieldSchema& field) noexcept { field.value.kind = ValueKind::Enum; }},
        {source, [](FieldSchema& field) noexcept { field.value.kind = ValueKind::Struct; }},
        {source, [](FieldSchema& field) noexcept { field.value.kind = ValueKind::Array; }}};
    for (const Case& example : cases) {
        FieldSchema fields[4];
        for (usize index = 0; index < 4; ++index) {
            fields[index] = generated.fields[index];
        }

        example.mutate(fields[example.field]);
        TypeSchema schema = generated;
        schema.fields = Span<const FieldSchema>(fields, 4);
        CHECK(types.Register(schema, GeneratedType<AssetFields>::Ops()).Error() ==
              ErrorCode::InvalidArgument);
    }

    FieldSchema outOfRange[4];
    for (usize index = 0; index < 4; ++index) {
        outOfRange[index] = generated.fields[index];
    }

    outOfRange[source].value.kind = beyondLast;
    TypeSchema schema = generated;
    schema.fields = Span<const FieldSchema>(outOfRange, 4);
    CHECK(types.Register(schema, GeneratedType<AssetFields>::Ops()).Error() ==
          ErrorCode::InvalidArgument);

    // Required 소프트 참조의 기대 타입(Node)은 등록되지 않아도 된다(대상은 쓰기에서 검사하지
    // 않는다).
    auto registered = GeneratedType<AssetFields>::Register(types);
    REQUIRE(registered);
    REQUIRE(types.Unregister(registered.Value()));
    CHECK(types.Shutdown());
}

TEST(AssetValueFields_AssetIdTextIsCanonicalLowercase) {
    SystemAllocator allocator;
    StringBuilder<64> text(allocator);
    REQUIRE(text.TryAppendFormat("{}", SampleAsset));
    CHECK(text.View() == StringView("01234567-89ab-cdef-fedc-ba9876543210"));
    CHECK(AssetId::TryParse(text.View()).Value() == SampleAsset);
    CHECK(AssetId::TryParse("00000000-0000-0000-0000-000000000000").Value().IsNull());

    // 대문자·32자·중괄호·잘못된 구분자는 정규 표기가 아니다.
    const char* invalid[] = {
        "01234567-89AB-CDEF-FEDC-BA9876543210", "0123456789abcdeffedcba9876543210",
        "{01234567-89ab-cdef-fedc-ba9876543210}", "01234567_89ab-cdef-fedc-ba9876543210", ""};
    for (const char* candidate : invalid) {
        CHECK(AssetId::TryParse(candidate).Error() == ErrorCode::InvalidArgument);
    }

    auto created = AssetId::TryNew();
    REQUIRE(created);
    CHECK(!created.Value().IsNull());
}

TEST(AssetValueFields_SoftObjectRefAssignValidatesAndKeepsValueOnFailure) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        SoftObjectRef<Node> ref(allocator);
        CHECK(ref.IsNull());
        REQUIRE(ref.TryAssign(OldAsset, "Body.Old"));
        CHECK(!ref.IsNull());

        const char invalidUtf8[] = {'\xff'};
        CHECK(ref.TryAssign(OldAsset, StringView(invalidUtf8, sizeof(invalidUtf8))).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(ref.TryAssign(AssetId{}, "Orphan").Error() == ErrorCode::InvalidArgument);
        CHECK(ref.TryAssign(NewAsset, LongPath()).Error() == ErrorCode::InvalidArgument);
        allocator.FailOnAttempt(1);
        CHECK(ref.TryAssign(NewAsset, "Body.New").Error() == ErrorCode::OutOfMemory);
        allocator.DisableFailure();
        CHECK(SoftEquals(ref, OldAsset, "Body.Old"));

        // 에셋만 있고 경로가 빈 값은 그 에셋의 대표 객체이며 null이 아니다.
        REQUIRE(ref.TryAssign(NewAsset, ""));
        CHECK(!ref.IsNull());
        REQUIRE(ref.TryAssign(AssetId{}, ""));
        CHECK(ref.IsNull());
    }

    CHECK(backing.Stats().liveAllocations == 0);
}

// 이동은 원본을 null로 만든다. 복사(TryAssign)는 받는 쪽 allocator를 유지하고, TryClone은 준
// allocator를 쓴다.
TEST(AssetValueFields_SoftObjectRefMoveAndCopyKeepAllocatorContracts) {
    SystemAllocator first;
    SystemAllocator second;
    {
        SoftObjectRef<Node> moved(first);
        REQUIRE(moved.TryAssign(OldAsset, "Body.Old"));
        SoftObjectRef<Node> constructed(Move(moved));
        CHECK(moved.IsNull());
        CHECK(SoftEquals(constructed, OldAsset, "Body.Old"));

        SoftObjectRef<Node> assigned(second);
        REQUIRE(assigned.TryAssign(NewAsset, "Body.New"));
        assigned = Move(constructed);
        CHECK(constructed.IsNull());
        CHECK(SoftEquals(assigned, OldAsset, "Body.Old"));
        CHECK(&assigned.objectPath.GetAllocator() == &first);
        SoftObjectRef<Node>& alias = assigned;
        assigned = Move(alias);
        CHECK(SoftEquals(assigned, OldAsset, "Body.Old"));

        SoftObjectRef<Node> copied(second);
        REQUIRE(copied.TryAssign(assigned));
        CHECK(SoftEquals(copied, OldAsset, "Body.Old"));
        CHECK(SoftEquals(assigned, OldAsset, "Body.Old"));
        CHECK(&copied.objectPath.GetAllocator() == &second);
        REQUIRE(copied.TryAssign(copied));
        CHECK(SoftEquals(copied, OldAsset, "Body.Old"));

        auto clone = assigned.TryClone(second);
        REQUIRE(clone);
        CHECK(SoftEquals(clone.Value(), OldAsset, "Body.Old"));
        CHECK(&clone.Value().objectPath.GetAllocator() == &second);
    }

    CHECK(first.Stats().liveAllocations == 0);
    CHECK(second.Stats().liveAllocations == 0);
}

// staged 쓰기와 등록 전 callback(바인딩 없는 write)도 thunk에서 같은 소프트 참조 규칙을 검사하고,
// 실패하면 값이 그대로다.
TEST(AssetValueFields_StagedAndUnboundSoftWritesRejectInvalidValues) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto registered = GeneratedType<AssetFields>::Register(types);
    REQUIRE(registered);
    {
        ObjectRegistry objects(allocator, types, {0x4156, 1});
        const auto access = objects.OwnerAccess();
        const RuntimeTypeOps unbound = GeneratedType<AssetFields>::Ops();
        {
            auto lease = types.Acquire(TypeIdOf<AssetFields>());
            REQUIRE(lease);
            const RuntimeFieldOps& icon = FieldOps(lease.Value().Ops().fields, "icon");
            ConstructionContext construction(objects, access);
            auto created = construction.Create<AssetFields>(TypeIdOf<AssetFields>());
            REQUIRE(created);
            FieldValue value(allocator);
            SetSoft(value, OldAsset, "Body.Old");
            REQUIRE(icon.writeStaged(construction, created.Value(), value));
            SetSoft(value, AssetId{}, "Orphan");
            CHECK(icon.writeStaged(construction, created.Value(), value).Error() ==
                  ErrorCode::InvalidArgument);
            SetSoft(value, NewAsset, LongPath());
            CHECK(icon.writeStaged(construction, created.Value(), value).Error() ==
                  ErrorCode::InvalidArgument);
            auto pending = construction.Get<AssetFields>(created.Value());
            REQUIRE(pending);
            CHECK(SoftEquals(pending.Value()->icon, OldAsset, "Body.Old"));
            auto committed = construction.Commit();
            REQUIRE(committed);

            const RuntimeFieldOps& raw = FieldOps(unbound.fields, "icon");
            SetSoft(value, AssetId{}, "Orphan");
            CHECK(raw.write(objects, access, committed.Value().Handle(), value).Error() ==
                  ErrorCode::InvalidArgument);
            SetSoft(value, NewAsset, LongPath());
            CHECK(raw.write(objects, access, committed.Value().Handle(), value).Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(IconIs(objects, lease.Value(), committed.Value().Handle(), OldAsset, "Body.Old",
                         allocator));
        }

        REQUIRE(objects.Shutdown(access));
    }

    REQUIRE(types.Unregister(registered.Value()));
    CHECK(types.Shutdown());
}

TEST(AssetValueFields_SoftWriteOomKeepsOldValue) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        TypeRegistry types(allocator);
        REQUIRE(GeneratedType<AssetFields>::Register(types));
        ObjectRegistry objects(allocator, types, {0x4156, 2});
        auto root = objects.NewObject<AssetFields>(objects.OwnerAccess(), TypeIdOf<AssetFields>());
        auto lease = types.Acquire(TypeIdOf<AssetFields>());
        REQUIRE(root && lease);
        const RuntimeFieldOps& icon = FieldOps(lease.Value().Ops().fields, "icon");
        FieldValue input(backing);
        SetSoft(input, OldAsset, "Body.Old");
        REQUIRE(icon.write(objects, objects.OwnerAccess(), root.Value().Handle(), input));
        SetSoft(input, NewAsset, "Body.New requiring an owned replacement");
        allocator.FailOnAttempt(1);
        CHECK(icon.write(objects, objects.OwnerAccess(), root.Value().Handle(), input).Error() ==
              ErrorCode::OutOfMemory);
        CHECK(allocator.Attempts() == 1);
        allocator.DisableFailure();
        // asset만 먼저 바뀌지 않는다.
        CHECK(IconIs(objects, lease.Value(), root.Value().Handle(), OldAsset, "Body.Old", backing));
    }

    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(AssetValueFields_SoftWriteLogicalDestroyBeforeCommitLeavesOldValue) {
    SystemAllocator backing;
    DestroyCallbackAllocator allocator(backing);
    {
        TypeRegistry types(allocator);
        REQUIRE(GeneratedType<AssetFields>::Register(types));
        ObjectRegistry objects(allocator, types, {0x4156, 3});
        allocator.registry = &objects;
        auto root = objects.NewObject<AssetFields>(objects.OwnerAccess(), TypeIdOf<AssetFields>());
        auto lease = types.Acquire(TypeIdOf<AssetFields>());
        REQUIRE(root && lease);
        allocator.target = root.Value().Handle();
        const RuntimeFieldOps& icon = FieldOps(lease.Value().Ops().fields, "icon");
        FieldValue input(backing);
        SetSoft(input, OldAsset, "Body.Old");
        REQUIRE(icon.write(objects, objects.OwnerAccess(), root.Value().Handle(), input));
        SetSoft(input, NewAsset, "Body.New");
        allocator.onAllocate = true;
        CHECK(icon.write(objects, objects.OwnerAccess(), root.Value().Handle(), input).Error() ==
              ErrorCode::Destroyed);
        CHECK(allocator.destruction == ErrorCode::None);
        root.Value().Reset();
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        CHECK(DestroyedIconIs(OldAsset, "Body.Old"));
    }

    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(AssetValueFields_SoftWriteLogicalDestroyAfterCommitKeepsSuccess) {
    SystemAllocator backing;
    DestroyCallbackAllocator allocator(backing);
    {
        TypeRegistry types(allocator);
        REQUIRE(GeneratedType<AssetFields>::Register(types));
        ObjectRegistry objects(allocator, types, {0x4156, 4});
        allocator.registry = &objects;
        auto root = objects.NewObject<AssetFields>(objects.OwnerAccess(), TypeIdOf<AssetFields>());
        auto lease = types.Acquire(TypeIdOf<AssetFields>());
        REQUIRE(root && lease);
        allocator.target = root.Value().Handle();
        const RuntimeFieldOps& icon = FieldOps(lease.Value().Ops().fields, "icon");
        FieldValue input(backing);
        SetSoft(input, OldAsset, "Body.Old");
        REQUIRE(icon.write(objects, objects.OwnerAccess(), root.Value().Handle(), input));
        SetSoft(input, NewAsset, "Body.New");
        allocator.onFree = true;
        REQUIRE(icon.write(objects, objects.OwnerAccess(), root.Value().Handle(), input));
        CHECK(allocator.destruction == ErrorCode::None);
        root.Value().Reset();
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        CHECK(DestroyedIconIs(NewAsset, "Body.New"));
    }

    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(AssetValueFields_SoftReadOomKeepsOutput) {
    SystemAllocator backing;
    {
        TypeRegistry types(backing);
        REQUIRE(GeneratedType<AssetFields>::Register(types));
        ObjectRegistry objects(backing, types, {0x4156, 5});
        auto root = objects.NewObject<AssetFields>(objects.OwnerAccess(), TypeIdOf<AssetFields>());
        auto lease = types.Acquire(TypeIdOf<AssetFields>());
        REQUIRE(root && lease);
        const RuntimeFieldOps& icon = FieldOps(lease.Value().Ops().fields, "icon");
        FieldValue input(backing);
        SetSoft(input, OldAsset, "Body.Old");
        REQUIRE(icon.write(objects, objects.OwnerAccess(), root.Value().Handle(), input));
        test::NFailAllocator failing(backing);
        FieldValue output(failing);
        output.kind = ValueKind::UInt64;
        output.unsignedInteger = 91;
        output.guid = NewAsset.value;
        REQUIRE(output.string.TryAssign("preserved"));
        failing.FailOnAttempt(1);
        CHECK(icon.read(objects, objects.OwnerAccess(), root.Value().Handle(), output).Error() ==
              ErrorCode::OutOfMemory);
        CHECK(output.kind == ValueKind::UInt64 && output.unsignedInteger == 91);
        CHECK(output.guid == NewAsset.value);
        CHECK(output.string.View() == StringView("preserved"));
    }

    CHECK(backing.Stats().liveAllocations == 0);
}

// 오류 순서: 전달 값 종류(TypeMismatch) → 읽기 전용(Unsupported) → 값 내용(InvalidArgument). staged
// 쓰기는 읽기 전용 정책을 받지 않지만 값 내용은 검사한다.
TEST(AssetValueFields_ReadOnlySoftReferencePolicyPrecedesValueChecks) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    const TypeSchema& generated = GeneratedType<AssetFields>::Schema();
    FieldSchema fields[4];
    for (usize index = 0; index < 4; ++index) {
        fields[index] = generated.fields[index];
    }

    fields[SchemaIndex(generated, "icon")].flags |= static_cast<uint32>(FieldFlags::ReadOnly);
    TypeSchema schema = generated;
    schema.fields = Span<const FieldSchema>(fields, 4);
    auto registered = types.Register(schema, GeneratedType<AssetFields>::Ops());
    REQUIRE(registered);
    {
        ObjectRegistry objects(allocator, types, {0x4156, 6});
        const auto access = objects.OwnerAccess();
        {
            auto lease = types.Acquire(TypeIdOf<AssetFields>());
            REQUIRE(lease);
            const RuntimeFieldOps& icon = FieldOps(lease.Value().Ops().fields, "icon");
            ConstructionContext construction(objects, access);
            auto created = construction.Create<AssetFields>(TypeIdOf<AssetFields>());
            REQUIRE(created);
            FieldValue value(allocator);
            SetSoft(value, AssetId{}, "Orphan");
            CHECK(icon.writeStaged(construction, created.Value(), value).Error() ==
                  ErrorCode::InvalidArgument);
            SetSoft(value, OldAsset, "Body.Old");
            REQUIRE(icon.writeStaged(construction, created.Value(), value));
            auto committed = construction.Commit();
            REQUIRE(committed);
            const ObjectHandle handle = committed.Value().Handle();

            SetSoft(value, NewAsset, "Body.New");
            CHECK(icon.write(objects, access, handle, value).Error() == ErrorCode::Unsupported);
            SetSoft(value, AssetId{}, "Orphan");
            CHECK(icon.write(objects, access, handle, value).Error() == ErrorCode::Unsupported);
            FieldValue wrongKind(allocator);
            wrongKind.kind = ValueKind::Utf8String;
            CHECK(icon.write(objects, access, handle, wrongKind).Error() ==
                  ErrorCode::TypeMismatch);
            CHECK(IconIs(objects, lease.Value(), handle, OldAsset, "Body.Old", allocator));
        }

        REQUIRE(objects.Shutdown(access));
    }

    REQUIRE(types.Unregister(registered.Value()));
    CHECK(types.Shutdown());
}
