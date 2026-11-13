#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>

// MetaCompiler 생성 코드(B5c)로 등록한 리플렉션 배열 필드의 그래프 저장·복원 시험이다(Docs/Design/
// ReflectionFoundations.ko.md 5.6절). ArrayGraphTests가 손으로 쓴 특수화로 검증한 모양을 생성
// 특수화와 모듈 집계 등록으로 다시 확인한다: 생성 스키마(원소 형식·maxElements·FieldKind)와 함수표,
// 생성 형질(ContainsReferenceArray 포함), 모든 컨테이너의 왕복·재저장 byte 동일, 생성 Trace가
// ObjectRefArray·StructRefArray 원소와 인라인 struct 안 컨테이너, 상속한 배열 필드를 방문한다(배열
// 원소로만 닿는 객체가 수집에서 산다), Required 참조 배열의 null 원소, MaxElements 지정과 경계,
// 원소 한도의 생성자 전 실패.
using namespace rocks;
using reflection_fixture::ArrayBundle;
using reflection_fixture::ArrayChild;
using reflection_fixture::ArrayOwner;
using reflection_fixture::ArraySlot;
using reflection_fixture::ArrayTag;
using reflection_fixture::Channels;
using reflection_fixture::TextureGroup;

// 생성 형질과 깊이(모델 값 — 생성 코드가 멤버 타입·원소 struct의 값과 static_assert로 대조한다).
static_assert(!GeneratedType<ArrayTag>::ContainsStrongReference);
static_assert(!GeneratedType<ArrayTag>::ContainsReferenceArray);
static_assert(GeneratedType<ArraySlot>::ContainsStrongReference);
static_assert(!GeneratedType<ArraySlot>::ContainsReferenceArray);
static_assert(GeneratedType<ArrayBundle>::ContainsStrongReference);
static_assert(GeneratedType<ArrayBundle>::ContainsReferenceArray);
static_assert(GeneratedType<ArrayTag>::Depth == 1);
static_assert(GeneratedType<ArraySlot>::Depth == 1);
static_assert(GeneratedType<ArrayBundle>::Depth == 2);

namespace {

constexpr uint32 Required = static_cast<uint32>(FieldFlags::Required);

// 이름으로 스키마 필드를 찾는다(없으면 nullptr).
const FieldSchema* FindField(const TypeSchema& schema, StringView name,
                             usize* index = nullptr) noexcept {
    for (usize at = 0; at < schema.fields.Size(); ++at) {
        if (StringView(schema.fields[at].name.Data(), schema.fields[at].name.Size()) == name) {
            if (index) {
                *index = at;
            }

            return &schema.fields[at];
        }
    }

    return nullptr;
}

// 진단을 기록한다(경로는 앞 두 단계까지 복사한다).
class Diagnostics final : public FieldCodecHooks {
public:
    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (count < 8) {
            Entry& entry = entries[count++];
            entry.kind = diagnostic.kind;
            entry.value = diagnostic.value;
            entry.depth = diagnostic.path.Size();
            for (usize step = 0; step < diagnostic.path.Size() && step < 2; ++step) {
                entry.path[step] = diagnostic.path[step];
            }
        }

        return Result<void>::Success();
    }

    struct Entry {
        FieldDiagnosticKind kind = FieldDiagnosticKind::UnknownField;
        int64 value = 0;
        usize depth = 0;
        PathStep path[2] = {};
    };

    Entry entries[8];
    usize count = 0;
};

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

Result<void> Push(Array<String>& target, const char* text) noexcept {
    auto added = target.TryEmplaceBack(target.GetAllocator());
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    return added.Value()->TryAssign(text);
}

// 시험 digest다(byte i = 3i).
Sha256Digest TestDigest() noexcept {
    Sha256Digest digest;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] = static_cast<uint8>(index * 3);
    }

    return digest;
}

// 값 배열(참조 없는 부분)을 채운다. names는 nameCount개다(MaxElements 시험이 4를 넘긴다).
Result<void> FillValues(ArrayChild& value, usize nameCount) noexcept {
    bool ok = value.flags.TryPushBack(true) && value.flags.TryPushBack(false) &&
              value.longs.TryPushBack(~uint64{0}) && value.longs.TryPushBack(uint64{0}) &&
              value.doubles.TryPushBack(-0.5) && value.guids.TryPushBack(Guid{1, 2}) &&
              value.guids.TryPushBack(Guid{}) && value.assets.TryPushBack(AssetId{Guid{9, 9}}) &&
              value.digests.TryPushBack(TestDigest()) && value.ints.TryPushBack(int32{-7}) &&
              value.ints.TryPushBack(int32{0}) && value.ints.TryPushBack(int32{2147483647}) &&
              value.groups.TryPushBack(TextureGroup::EightBitData) &&
              value.groups.TryPushBack(TextureGroup::World) &&
              value.channels.TryPushBack(static_cast<Channels>(5)) &&
              value.channels.TryPushBack(Channels::Alpha) &&
              value.points.TryPushBack(Vec3(1.5f, -2.0f, 0.25f)) &&
              value.extra.TryPushBack(uint8{0}) && value.extra.TryPushBack(uint8{255});
    if (!ok) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    for (usize index = 0; index < nameCount; ++index) {
        if (auto pushed = Push(value.names, index == 1 ? "배열" : ""); !pushed) {
            return pushed;
        }
    }

    // 소프트 참조: null과 경로 있는 값.
    for (usize index = 0; index < 2; ++index) {
        auto added = value.softs.TryEmplaceBack(value.softs.GetAllocator());
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        if (index == 1) {
            if (auto set = added.Value()->TryAssign(AssetId{Guid{5, 5}}, "Mesh.Socket"); !set) {
                return set;
            }
        }
    }

    // 대상 Object인 소프트 참조(에셋만 있는 값).
    auto anySoft = value.anySofts.TryEmplaceBack(value.anySofts.GetAllocator());
    if (!anySoft) {
        return Result<void>::Failure(anySoft.Error());
    }

    anySoft.Value()->asset = AssetId{Guid{6, 6}};

    // 참조 없는 struct 원소(원소 struct 안 배열 포함).
    for (usize index = 0; index < 2; ++index) {
        auto added = value.tags.TryEmplaceBack(value.tags.GetAllocator());
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        ArrayTag& tag = *added.Value();
        tag.id = static_cast<uint32>(index + 1);
        if (index == 0 && (!tag.label.TryAssign("첫째") || !tag.codes.TryPushBack(uint16{1}) ||
                           !tag.codes.TryPushBack(uint16{65535}))) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    return Result<void>::Success();
}

// 그래프 모양 선택이다. requiredNull은 A.required를 [null]로, nameCount는 A.names 원소 수다.
struct GraphOptions {
    bool requiredNull = false;
    usize nameCount = 2;
};

// A(ArrayChild — 배열 필드를 상속한다)와 배열 원소로만 닿는 B·C·D·E·F(ArrayOwner)를 만든다. A.links
// = [B, null, A], A.required = [C](requiredNull이면 [null]), A.slots = [{D, "s0", [0.5]}, {null}],
// A.bundle = {refs [null, E], slots [{F, "b0", [1, 2]}]}. A의 root를 돌려준다.
Result<RootRef<Object>> MakeGraph(ObjectRegistry& objects,
                                  const GraphOptions& options = {}) noexcept {
    ConstructionContext transaction(objects, objects.OwnerAccess());
    auto a = transaction.Create<ArrayChild>(TypeIdOf<ArrayChild>());
    ObjectHandle others[5];
    for (ObjectHandle& other : others) {
        auto created = transaction.Create<ArrayOwner>(TypeIdOf<ArrayOwner>());
        if (!created) {
            return Result<RootRef<Object>>::Failure(created.Error());
        }

        other = created.Value();
    }

    if (!a) {
        return Result<RootRef<Object>>::Failure(a.Error());
    }

    auto root = transaction.Get<ArrayChild>(a.Value());
    if (!root) {
        return Result<RootRef<Object>>::Failure(root.Error());
    }

    ArrayChild& value = *root.Value();
    if (auto filled = FillValues(value, options.nameCount); !filled) {
        return Result<RootRef<Object>>::Failure(filled.Error());
    }

    const ObjectHandle links[] = {others[0], ObjectHandle{}, a.Value()};
    const ObjectHandle required[] = {options.requiredNull ? ObjectHandle{} : others[1]};
    const ObjectHandle bundleRefs[] = {ObjectHandle{}, others[3]};
    bool ok = transaction.AssignReferences(a.Value(), value.links, links) &&
              transaction.AssignReferences(a.Value(), value.required, required) &&
              transaction.AssignReferences(a.Value(), value.bundle.refs, bundleRefs) &&
              transaction.AssignElements(a.Value(), value.slots, 2) &&
              transaction.AssignElements(a.Value(), value.bundle.slots, 1);
    if (!ok) {
        return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
    }

    ok = value.slots[0].name.TryAssign("s0") && value.slots[0].weights.TryPushBack(0.5f) &&
         value.bundle.slots[0].name.TryAssign("b0") &&
         value.bundle.slots[0].weights.TryPushBack(1.0f) &&
         value.bundle.slots[0].weights.TryPushBack(2.0f) &&
         transaction.SetElementReference(a.Value(), value.slots, 0, value.slots[0].material,
                                         others[2]) &&
         transaction.SetElementReference(a.Value(), value.bundle.slots, 0,
                                         value.bundle.slots[0].material, others[4]);
    if (!ok) {
        return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
    }

    return transaction.Commit();
}

Result<Array<uint8>> SaveGraph(Allocator& allocator, ObjectRegistry& objects,
                               const GraphOptions& graph = {},
                               ObjectGraphSaveOptions options = {}) noexcept {
    auto root = MakeGraph(objects, graph);
    if (!root) {
        return Result<Array<uint8>>::Failure(root.Error());
    }

    return SaveObjectGraph(allocator, objects, objects.OwnerAccess(), root.Value().Handle(),
                           options);
}

// 로드한 A의 값이 MakeGraph가 넣은 그대로인지 본다(self = A). 배열 원소로만 닿는 객체의 handle을
// reached에 돌려준다(B·C·D·E·F 순 — 서로 다르고 A가 아니다).
bool CheckFilled(const ArrayChild& value, ObjectHandle self, ObjectHandle (&reached)[5]) noexcept {
    const Sha256Digest digest = TestDigest();
    bool ok = value.flags.Size() == 2 && value.flags[0] && !value.flags[1] &&
              value.longs.Size() == 2 && value.longs[0] == ~uint64{0} && value.longs[1] == 0 &&
              value.doubles.Size() == 1 && value.doubles[0] == -0.5 && value.guids.Size() == 2 &&
              value.guids[0] == (Guid{1, 2}) && value.guids[1] == Guid{} &&
              value.assets.Size() == 1 && value.assets[0] == AssetId{Guid{9, 9}} &&
              value.digests.Size() == 1 && value.anySofts.Size() == 1 &&
              value.anySofts[0].asset == AssetId{Guid{6, 6}} &&
              value.anySofts[0].objectPath.ByteCount() == 0;
    for (usize index = 0; ok && index < Sha256Digest::Bytes; ++index) {
        ok = value.digests[0].bytes[index] == digest.bytes[index];
    }

    ok = ok && value.ints.Size() == 3 && value.ints[0] == -7 && value.ints[2] == 2147483647 &&
         value.names.Size() == 2 && value.names[0].View() == StringView("") &&
         value.names[1].View() == StringView("배열") && value.groups.Size() == 2 &&
         value.groups[0] == TextureGroup::EightBitData && value.channels.Size() == 2 &&
         static_cast<uint32>(value.channels[0]) == 5 && value.channels[1] == Channels::Alpha &&
         value.softs.Size() == 2 && value.softs[0].IsNull() &&
         value.softs[1].asset == AssetId{Guid{5, 5}} &&
         value.softs[1].objectPath.View() == StringView("Mesh.Socket") &&
         value.points.Size() == 1 && value.points[0].x == 1.5f && value.points[0].y == -2.0f &&
         value.points[0].z == 0.25f && value.extra.Size() == 2 && value.extra[1] == 255;
    ok = ok && value.tags.Size() == 2 && value.tags[0].id == 1 &&
         value.tags[0].label.View() == StringView("첫째") && value.tags[0].codes.Size() == 2 &&
         value.tags[0].codes[1] == 65535 && value.tags[1].id == 2 && value.tags[1].codes.IsEmpty();
    ok = ok && value.links.Size() == 3 && value.links[1] == ObjectHandle{} &&
         value.links[2] == self && value.required.Size() == 1 && value.slots.Size() == 2 &&
         value.slots[0].name.View() == StringView("s0") && value.slots[0].weights.Size() == 1 &&
         value.slots[0].weights[0] == 0.5f && value.slots[1].material.Handle() == ObjectHandle{} &&
         value.bundle.refs.Size() == 2 && value.bundle.refs[0] == ObjectHandle{} &&
         value.bundle.slots.Size() == 1 && value.bundle.slots[0].name.View() == StringView("b0") &&
         value.bundle.slots[0].weights.Size() == 2 && value.bundle.slots[0].weights[1] == 2.0f;
    if (!ok) {
        return false;
    }

    reached[0] = value.links[0];
    reached[1] = value.required[0];
    reached[2] = value.slots[0].material.Handle();
    reached[3] = value.bundle.refs[1];
    reached[4] = value.bundle.slots[0].material.Handle();
    for (usize index = 0; index < 5; ++index) {
        if (reached[index] == ObjectHandle{} || reached[index] == self) {
            return false;
        }

        for (usize other = 0; other < index; ++other) {
            if (reached[other] == reached[index]) {
                return false;
            }
        }
    }

    return true;
}

} // namespace

// 모듈 집계 등록이 배열 필드의 생성 스키마(value {Array, 0}·원소 형식·원소 대상·maxElements·
// FieldKind)와 함수표(read·write·writeStaged 없음, codec, 원소 Enum·Struct의 native 키)를 싣고,
// 원소 struct가 먼저 등록되어 계산 형질이 생성 값과 같다. 상속한 배열 필드도 파생 클래스 스키마에
// 같은 maxElements로 들어간다.
TEST(GeneratedArrayGraph_ModuleRegistrationDescribesArrayFields) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            auto bundle = types.AcquireValueType(TypeIdOf<ArrayBundle>(), TypeKind::Struct);
            auto slot = types.AcquireValueType(TypeIdOf<ArraySlot>(), TypeKind::Struct);
            auto tag = types.AcquireValueType(TypeIdOf<ArrayTag>(), TypeKind::Struct);
            REQUIRE(bundle && slot && tag);
            CHECK(bundle.Value().Struct().ContainsReferenceArray());
            CHECK(bundle.Value().Struct().ContainsStrongReference());
            CHECK(bundle.Value().Struct().Depth() == 2);
            CHECK(bundle.Value().StructOps().trace != nullptr);
            CHECK(slot.Value().Struct().ContainsStrongReference());
            CHECK(!slot.Value().Struct().ContainsReferenceArray());
            CHECK(!tag.Value().Struct().ContainsStrongReference());
            CHECK(tag.Value().StructOps().trace == nullptr);

            auto child = types.Acquire(TypeIdOf<ArrayChild>());
            REQUIRE(child);
            const TypeSchema& schema = child.Value().Schema();
            struct Expected {
                const char* name;
                ValueKind element;
                TypeId target;
                uint32 maxElements;
                FieldKind kind;
            };

            const Expected expected[] = {
                {"flags", ValueKind::Bool, {}, DefaultMaxElements, FieldKind::Value},
                {"longs", ValueKind::UInt64, {}, DefaultMaxElements, FieldKind::Value},
                {"doubles", ValueKind::Float64, {}, DefaultMaxElements, FieldKind::Value},
                {"guids", ValueKind::Guid, {}, DefaultMaxElements, FieldKind::Value},
                {"assets", ValueKind::AssetId, {}, DefaultMaxElements, FieldKind::Value},
                {"digests", ValueKind::Digest256, {}, DefaultMaxElements, FieldKind::Value},
                {"anySofts",
                 ValueKind::SoftObjectReference,
                 {},
                 DefaultMaxElements,
                 FieldKind::Value},
                {"ints", ValueKind::Int32, {}, DefaultMaxElements, FieldKind::Value},
                {"names", ValueKind::Utf8String, {}, 4, FieldKind::Value},
                {"groups", ValueKind::Enum, TypeIdOf<TextureGroup>(), DefaultMaxElements,
                 FieldKind::Value},
                {"channels", ValueKind::Enum, TypeIdOf<Channels>(), DefaultMaxElements,
                 FieldKind::Value},
                {"softs", ValueKind::SoftObjectReference, TypeIdOf<ArrayOwner>(),
                 DefaultMaxElements, FieldKind::Value},
                {"points", ValueKind::Struct, TypeIdOf<Vec3>(), DefaultMaxElements,
                 FieldKind::Value},
                {"tags", ValueKind::Struct, TypeIdOf<ArrayTag>(), DefaultMaxElements,
                 FieldKind::Value},
                {"links", ValueKind::ObjectReference, TypeIdOf<ArrayOwner>(), DefaultMaxElements,
                 FieldKind::ObjectReference},
                {"required",
                 ValueKind::ObjectReference,
                 {},
                 DefaultMaxElements,
                 FieldKind::ObjectReference},
                {"slots", ValueKind::Struct, TypeIdOf<ArraySlot>(), DefaultMaxElements,
                 FieldKind::Value},
                {"extra", ValueKind::UInt8, {}, DefaultMaxElements, FieldKind::Value}};
            for (const Expected& entry : expected) {
                usize index = 0;
                const FieldSchema* field = FindField(schema, StringView(entry.name), &index);
                REQUIRE(field);
                CHECK(field->value.kind == ValueKind::Array);
                CHECK(field->value.type == TypeId{});
                CHECK(field->element.kind == entry.element);
                CHECK(field->element.type == entry.target);
                CHECK(field->maxElements == entry.maxElements);
                CHECK(field->kind == entry.kind);
                CHECK(((field->flags & Required) != 0) == (StringView(entry.name) == "required"));
                const RuntimeFieldOps* found = nullptr;
                for (const RuntimeFieldOps& candidate : child.Value().Ops().fields) {
                    found = candidate.fieldId == field->id ? &candidate : found;
                }

                REQUIRE(found);
                CHECK(found->read == nullptr);
                CHECK(!found->write);
                CHECK(found->writeStaged == nullptr);
                CHECK(found->encode != nullptr && found->decode != nullptr);
                if (entry.element == ValueKind::Struct) {
                    CHECK(!child.Value().FieldStruct(index).IsEmpty());
                    CHECK(found->valueNativeType ==
                          child.Value().FieldStruct(index).StructOps().nativeType);
                } else if (entry.element == ValueKind::Enum) {
                    CHECK(child.Value().FieldEnum(index) != nullptr);
                    CHECK(found->valueNativeType == (entry.target == TypeIdOf<TextureGroup>()
                                                         ? NativeTypeKey<TextureGroup>()
                                                         : NativeTypeKey<Channels>()));
                } else {
                    CHECK(found->valueNativeType == nullptr);
                }
            }

            // 인라인 struct 필드는 배열이 아니다.
            const FieldSchema* bundleField = FindField(schema, StringView("bundle"));
            REQUIRE(bundleField);
            CHECK(bundleField->value.kind == ValueKind::Struct);
            CHECK(bundleField->element.kind == ValueKind::Opaque && bundleField->maxElements == 0);

            // struct 멤버 배열의 스키마: 인라인 struct의
            // ObjectRefArray(FieldKind::ObjectReference·원소 대상)와 StructRefArray, 원소 struct 안
            // 배열.
            const FieldSchema* refs = FindField(bundle.Value().Schema(), StringView("refs"));
            const FieldSchema* slots = FindField(bundle.Value().Schema(), StringView("slots"));
            const FieldSchema* weights = FindField(slot.Value().Schema(), StringView("weights"));
            const FieldSchema* codes = FindField(tag.Value().Schema(), StringView("codes"));
            REQUIRE(refs && slots && weights && codes);
            CHECK(refs->kind == FieldKind::ObjectReference && refs->value.kind == ValueKind::Array);
            CHECK(refs->element.kind == ValueKind::ObjectReference &&
                  refs->element.type == TypeIdOf<ArrayOwner>());
            CHECK(slots->kind == FieldKind::Value && slots->element.kind == ValueKind::Struct &&
                  slots->element.type == TypeIdOf<ArraySlot>());
            CHECK(weights->value.kind == ValueKind::Array &&
                  weights->element.kind == ValueKind::Float32 &&
                  weights->maxElements == DefaultMaxElements);
            CHECK(codes->value.kind == ValueKind::Array &&
                  codes->element.kind == ValueKind::UInt16 &&
                  codes->maxElements == DefaultMaxElements);
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// 모든 컨테이너·원소 종류가 그대로 돌아오고 다른 Registry에서 다시 저장한 byte가 같다. 생성 Trace가
// 상속한 ObjectRefArray(links·required), StructRefArray 원소 안 참조(slots[0].material), 인라인
// struct 안 ObjectRefArray(bundle.refs)와 StructRefArray 원소 안 참조(bundle.slots[0].material)를
// 방문해 그것들로만 닿는 다섯 객체가 수집에서 살고, root를 놓으면 모두 회수된다.
TEST(GeneratedArrayGraph_RoundTripResavesIdenticallyAndTracesElements) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            ObjectRegistry source(allocator, types, {0x4741, 1});
            ObjectRegistry target(allocator, types, {0x4741, 2});
            {
                auto bytes = SaveGraph(allocator, source);
                REQUIRE(bytes);
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              bytes.Value().View());
                REQUIRE(loaded);
                const ObjectHandle self = loaded.Value().Handle();
                ObjectHandle reached[5] = {};
                {
                    auto borrowed = target.Resolve<ArrayChild>(target.OwnerAccess(), self);
                    REQUIRE(borrowed);
                    CHECK(CheckFilled(*borrowed.Value().Get().Value(), self, reached));
                }

                REQUIRE(target.Collect(target.OwnerAccess()));
                CHECK(target.Stats().liveObjects == 6);
                for (ObjectHandle handle : reached) {
                    CHECK(static_cast<bool>(
                        target.Resolve<ArrayOwner>(target.OwnerAccess(), handle)));
                }

                auto resaved = SaveObjectGraph(allocator, target, target.OwnerAccess(), self);
                REQUIRE(resaved);
                CHECK(SameBytes(bytes.Value().View(), resaved.Value().View()));
                loaded.Value().Reset();
                REQUIRE(target.Collect(target.OwnerAccess()));
                CHECK(target.Stats().liveObjects == 0);
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// Required 참조 배열(ObjectRefArray<>)의 null 원소는 rejectMissingRequired 저장에서 경로
// [required[0]]의 RequiredMissing 뒤 InvalidArgument이고 로드는 진단만 낸다. MaxElements = 4인
// names는 정확히 4개면 저장하고, 넘기면 저장에서 ArrayTooLong 뒤 CapacityExceeded다.
TEST(GeneratedArrayGraph_RequiredNullElementAndMaxElements) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        Guid requiredId{};
        Guid namesId{};
        {
            auto child = types.Acquire(TypeIdOf<ArrayChild>());
            REQUIRE(child);
            const FieldSchema* required = FindField(child.Value().Schema(), StringView("required"));
            const FieldSchema* names = FindField(child.Value().Schema(), StringView("names"));
            REQUIRE(required && names);
            requiredId = required->id;
            namesId = names->id;
        }

        {
            ObjectRegistry source(allocator, types, {0x4741, 3});
            ObjectRegistry target(allocator, types, {0x4741, 4});
            {
                GraphOptions nullRequired;
                nullRequired.requiredNull = true;
                Diagnostics saveHooks;
                ObjectGraphSaveOptions strict;
                strict.policy.rejectMissingRequired = true;
                strict.hooks = &saveHooks;
                CHECK(SaveGraph(allocator, source, nullRequired, strict).Error() ==
                      ErrorCode::InvalidArgument);
                REQUIRE(saveHooks.count == 1);
                CHECK(saveHooks.entries[0].kind == FieldDiagnosticKind::RequiredMissing);
                CHECK(saveHooks.entries[0].depth == 1);
                CHECK(saveHooks.entries[0].path[0].fieldId == requiredId);
                CHECK(saveHooks.entries[0].path[0].indexed &&
                      saveHooks.entries[0].path[0].index == 0);

                auto bytes = SaveGraph(allocator, source, nullRequired);
                REQUIRE(bytes);
                Diagnostics loadHooks;
                ObjectGraphLoadOptions load;
                load.hooks = &loadHooks;
                CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                      bytes.Value().View(), load));
                REQUIRE(loadHooks.count == 1);
                CHECK(loadHooks.entries[0].kind == FieldDiagnosticKind::RequiredMissing);
                CHECK(loadHooks.entries[0].path[0].fieldId == requiredId);

                // 정확히 상한(4개)은 저장한다.
                GraphOptions fullNames;
                fullNames.nameCount = 4;
                CHECK(static_cast<bool>(SaveGraph(allocator, source, fullNames)));

                GraphOptions longNames;
                longNames.nameCount = 5;
                Diagnostics lengthHooks;
                ObjectGraphSaveOptions reported;
                reported.hooks = &lengthHooks;
                CHECK(SaveGraph(allocator, source, longNames, reported).Error() ==
                      ErrorCode::CapacityExceeded);
                REQUIRE(lengthHooks.count == 1);
                CHECK(lengthHooks.entries[0].kind == FieldDiagnosticKind::ArrayTooLong);
                CHECK(lengthHooks.entries[0].value == 5);
                CHECK(lengthHooks.entries[0].path[0].fieldId == namesId);
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// 생성 codec도 그래프 원소 한도(maxTotalElements)를 객체 생성자 전에 검사한다: 작은 한도의 로드는
// CapacityExceeded이고 ArrayOwner 생성자가 한 번도 불리지 않는다. 기본 한도로는 같은 byte가 읽힌다.
TEST(GeneratedArrayGraph_ElementLimitFailsBeforeConstructors) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            ObjectRegistry source(allocator, types, {0x4741, 5});
            ObjectRegistry target(allocator, types, {0x4741, 6});
            {
                auto bytes = SaveGraph(allocator, source);
                REQUIRE(bytes);
                const uint32 constructed = ArrayOwner::constructed;
                ObjectGraphLoadOptions few;
                few.limits.maxTotalElements = 8;
                CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                      bytes.Value().View(), few)
                          .Error() == ErrorCode::CapacityExceeded);
                CHECK(ArrayOwner::constructed == constructed);
                CHECK(target.Stats().liveObjects == 0);
                CHECK(static_cast<bool>(LoadObjectGraph(
                    allocator, target, types, target.OwnerAccess(), bytes.Value().View())));
                CHECK(ArrayOwner::constructed == constructed + 6);
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}
