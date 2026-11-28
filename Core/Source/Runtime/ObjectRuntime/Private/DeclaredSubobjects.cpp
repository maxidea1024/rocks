#include <Base/TypeTraits.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/SubobjectDefaultsContext.h>
#include <Object/TypeRegistry.h>

// 선언 subobject의 생성·배선·기본값 덮기다(Docs/Design/Prefab 10.2·10.3절 — UE
// CreateDefaultSubobject 대응). ObjectRuntime은 소유 타입을 모르므로 생성 연산표의 필드
// 접근자(RuntimeSubobjectOps·subobject*Access)로 필드 주소를 얻는다. ObjectRef·WeakObjectRef·
// ObjectRefArray는 standard-layout이고 첫 멤버(handle·배열 저장소)와 주소가 같으므로 그 주소를
// 첫 멤버 주소로 바꿔 쓴다(포인터 상호 변환 — 세 래퍼는 T와 무관한 단일 멤버라 <Object>로 검사한
// 배치가 모든 T에 성립한다).
namespace rocks {

static_assert(IsStandardLayoutV<ObjectRef<Object>> &&
                  sizeof(ObjectRef<Object>) == sizeof(ObjectHandle),
              "ObjectRef must be a standard-layout wrapper of one ObjectHandle");
static_assert(IsStandardLayoutV<WeakObjectRef<Object>> &&
                  sizeof(WeakObjectRef<Object>) == sizeof(ObjectHandle),
              "WeakObjectRef must be a standard-layout wrapper of one ObjectHandle");
static_assert(IsStandardLayoutV<ObjectRefArray<Object>> &&
                  sizeof(ObjectRefArray<Object>) == sizeof(detail::ObjectRefArrayStorage),
              "ObjectRefArray must be a standard-layout wrapper of its storage");

namespace {

// 소유 타입 base 사슬의 최대 깊이다(접근자 탐색·InitSubobjectDefaults 사슬을 모으는 고정 배열 크기
// — 리플렉션 클래스 사슬은 이보다 훨씬 얕다). 넘으면 CapacityExceeded다.
constexpr usize MaxSubobjectOwnerDepth = 64;

using FieldAccess = void* (*)(Object&) noexcept;

// lease 타입부터 base 사슬을 걸어 access(ops)가 처음으로 null이 아닌 접근자를 찾는다(지정자를
// 선언한 클래스만 접근자를 싣는다). 없으면 null 성공이고, 사슬이 상한보다 깊으면
// CapacityExceeded다.
Result<FieldAccess> FindAccess(TypeRegistry& types, const TypeLease& lease,
                               FieldAccess (*access)(const RuntimeTypeOps&) noexcept) noexcept {
    if (FieldAccess found = access(lease.Ops())) {
        return Result<FieldAccess>::Success(found);
    }

    TypeId base = lease.Schema().baseType;
    for (usize depth = 0; base != TypeId{}; ++depth) {
        if (depth == MaxSubobjectOwnerDepth) {
            return Result<FieldAccess>::Failure(ErrorCode::CapacityExceeded);
        }

        auto next = types.Acquire(base);
        if (!next) {
            return Result<FieldAccess>::Failure(next.Error());
        }

        if (FieldAccess found = access(next.Value().Ops())) {
            return Result<FieldAccess>::Success(found);
        }

        base = next.Value().Schema().baseType;
    }

    return Result<FieldAccess>::Success(nullptr);
}

const FieldSchema* FindField(const TypeSchema& schema, Guid id) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (field.id == id) {
            return &field;
        }
    }

    return nullptr;
}

// 필드 schema의 실효 도구 이름(Name= 또는 C++ 이름)이다 — 선언 subobject의 객체 이름이다.
StringView SubobjectName(const TypeSchema& schema, Guid id) noexcept {
    if (const FieldSchema* field = FindField(schema, id)) {
        const Span<const char> name = EffectiveToolName(*field);
        return StringView(name.Data(), name.Size());
    }

    return {};
}

} // namespace

SubobjectDefaultsContext::SubobjectDefaultsContext(Object& owner, const TypeLease& ownerType,
                                                   Span<Object* const> subobjects) noexcept
    : owner_(owner), ownerType_(ownerType), subobjects_(subobjects) {
}

Object* SubobjectDefaultsContext::Find(const void* field) const noexcept {
    const Span<const RuntimeSubobjectOps> entries = ownerType_.Ops().subobjects;
    for (usize index = 0; index < entries.Size() && index < subobjects_.Size(); ++index) {
        if (entries[index].field(owner_) == field) {
            return subobjects_[index];
        }
    }

    return nullptr;
}

Result<usize> ConstructionContext::CreateDeclaredSubobjects(ObjectHandle owner,
                                                            Span<ObjectHandle> out) noexcept {
    if (auto state = CheckOpen(); !state) {
        return Result<usize>::Failure(state.Error());
    }

    // 모든 실패는 트랜잭션을 그 오류로 고정한다(Commit이 실패하고 Rollback이 전부 되돌린다).
    const auto failWith = [this](ErrorCode error) noexcept {
        (void)Fail(error);
        return Result<usize>::Failure(error);
    };

    // owner는 이 트랜잭션의 공개 전 객체여야 한다(Alive·다른 트랜잭션 객체 거부 — 표가 없어도 먼저
    // 본다).
    auto ownerObject = Get<Object>(owner);
    if (!ownerObject) {
        return failWith(ownerObject.Error());
    }

    auto ownerType = AcquireType(owner);
    if (!ownerType) {
        return failWith(ownerType.Error());
    }

    const TypeLease& lease = ownerType.Value();
    const TypeSchema& schema = lease.Schema();
    const Span<const SubobjectSchema> table = schema.subobjects;
    const usize count = table.Size();
    if (count == 0) {
        return Result<usize>::Success(0);
    }

    TypeRegistry& types = registry_.Types();
    auto listAccess = FindAccess(
        types, lease, [](const RuntimeTypeOps& ops) noexcept { return ops.subobjectListAccess; });
    auto rootAccess = FindAccess(
        types, lease, [](const RuntimeTypeOps& ops) noexcept { return ops.subobjectRootAccess; });
    if (!listAccess || !rootAccess) {
        return failWith(!listAccess ? listAccess.Error() : rootAccess.Error());
    }

    bool hasRoot = false;
    for (const SubobjectSchema& entry : table) {
        hasRoot = hasRoot || (entry.flags & static_cast<uint32>(SubobjectFlags::Root)) != 0;
    }

    // 결과 칸이 모자라거나 등록된 지정자 접근자가 없으면 호출·등록 계약 위반이다.
    if (out.Size() < count || listAccess.Value() == nullptr ||
        (hasRoot && rootAccess.Value() == nullptr)) {
        return failWith(ErrorCode::InvalidArgument);
    }

    // 소유 목록은 소유 객체를 막 만든 직후라 비어 있어야 한다(선언 subobject가 목록 앞에 온다 — 두
    // 번 부르면 여기서 거부된다).
    auto& list =
        *static_cast<detail::ObjectRefArrayStorage*>(listAccess.Value()(*ownerObject.Value()));
    if (list.size != 0) {
        return failWith(ErrorCode::InvalidArgument);
    }

    // 1. 표 순서로 만든다(Outer = 소유 객체, 이름 = 소유 필드 도구 이름, 생성 출처 ClassSubobject).
    //    생성 실패는 CreateImpl이 트랜잭션을 오류로 고정한다.
    Object* objects[MaxClassSubobjects];
    for (usize index = 0; index < count; ++index) {
        auto created =
            CreateImpl(table[index].type, nullptr, owner, SubobjectName(schema, table[index].id),
                       ObjectOrigin::ClassSubobject);
        if (!created) {
            return Result<usize>::Failure(created.Error());
        }

        out[index] = created.Value();
        auto object = Get<Object>(out[index]);
        if (!object) {
            return failWith(object.Error());
        }

        objects[index] = object.Value();
    }

    // 2. 배선: 소유 필드, 소유 목록(표 순서), Root 필드, 부착 부모. 대상은 이 트랜잭션의 객체이고
    //    표 타입이 필드 타입이라(등록 검사) 타입 대조를 생략한다(nativeType null).
    if (auto writable = CheckStagedWrite(); !writable) {
        return failWith(writable.Error());
    }

    const Span<const RuntimeSubobjectOps> ops = lease.Ops().subobjects;
    for (usize index = 0; index < count; ++index) {
        auto* field = static_cast<ObjectHandle*>(ops[index].field(*ownerObject.Value()));
        if (auto set = registry_.SetEdge(access_, owner, *field, out[index], transaction_, nullptr);
            !set) {
            return failWith(set.Error());
        }
    }

    if (auto assigned = registry_.AssignArrayEdges(access_, owner, list,
                                                   Span<const ObjectHandle>(out.Data(), count),
                                                   transaction_, nullptr);
        !assigned) {
        return failWith(assigned.Error());
    }

    // 부착 부모: 같은 subobject 타입은 접근자·부착 필드 기대 타입을 한 번만 찾는다.
    TypeId cachedType{};
    FieldAccess cachedAccess = nullptr;
    TypeId cachedExpected{};
    for (usize index = 0; index < count; ++index) {
        const SubobjectSchema& entry = table[index];
        if ((entry.flags & static_cast<uint32>(SubobjectFlags::Root)) != 0) {
            auto* root = static_cast<ObjectHandle*>(rootAccess.Value()(*ownerObject.Value()));
            if (auto set =
                    registry_.SetEdge(access_, owner, *root, out[index], transaction_, nullptr);
                !set) {
                return failWith(set.Error());
            }
        }

        if (entry.attachParent < 0) {
            continue;
        }

        if (entry.type != cachedType) {
            auto subobjectType = types.Acquire(entry.type);
            if (!subobjectType) {
                return failWith(subobjectType.Error());
            }

            auto attach =
                FindAccess(types, subobjectType.Value(), [](const RuntimeTypeOps& ops) noexcept {
                    return ops.subobjectAttachAccess;
                });
            const FieldSchema* attachField =
                FindField(subobjectType.Value().Schema(),
                          subobjectType.Value().Schema().subobjectAttachField);
            if (!attach || attach.Value() == nullptr || attachField == nullptr) {
                return failWith(!attach ? attach.Error() : ErrorCode::InvalidArgument);
            }

            cachedType = entry.type;
            cachedAccess = attach.Value();
            cachedExpected = attachField->value.type;
        }

        // 부착 부모 타입은 부착 필드(WeakObjectRef<T>)의 T 또는 그 파생이어야 한다(MetaCompiler도
        // 검사한다 — 손 등록·생성 불일치 방어).
        const usize parent = static_cast<usize>(entry.attachParent);
        if (cachedExpected != TypeId{}) {
            auto parentType = types.Acquire(table[parent].type);
            if (!parentType || !parentType.Value().IsA(cachedExpected)) {
                return failWith(!parentType ? parentType.Error() : ErrorCode::TypeMismatch);
            }
        }

        // weak 참조는 GC 간선이 아니므로 staged 객체에 바로 쓴다(일반 weak 필드의 staged 쓰기와
        // 같다 — 장부 없음).
        *static_cast<ObjectHandle*>(cachedAccess(*objects[index])) = out[parent];
    }

    // 3. 기본값 덮기: base 사슬의 hook을 가장 먼 base부터 부른다.
    TypeLease chain[MaxSubobjectOwnerDepth];
    usize depth = 0;
    TypeId current = schema.id;
    while (current != TypeId{}) {
        if (depth == MaxSubobjectOwnerDepth) {
            return failWith(ErrorCode::CapacityExceeded);
        }

        auto acquired = types.Acquire(current);
        if (!acquired) {
            return failWith(acquired.Error());
        }

        current = acquired.Value().Schema().baseType;
        chain[depth++] = Move(acquired.Value());
    }

    SubobjectDefaultsContext context(*ownerObject.Value(), lease,
                                     Span<Object* const>(objects, count));
    for (usize index = depth; index > 0; --index) {
        if (auto hook = chain[index - 1].Ops().initSubobjectDefaults) {
            if (auto applied = hook(context); !applied) {
                return failWith(applied.Error());
            }
        }
    }

    return Result<usize>::Success(count);
}

Result<ObjectHandle> ConstructionContext::DeclaredSubobject(ObjectHandle owner,
                                                            usize index) noexcept {
    auto ownerObject = Get<Object>(owner);
    if (!ownerObject) {
        return Result<ObjectHandle>::Failure(ownerObject.Error());
    }

    auto ownerType = AcquireType(owner);
    if (!ownerType) {
        return Result<ObjectHandle>::Failure(ownerType.Error());
    }

    const Span<const RuntimeSubobjectOps> ops = ownerType.Value().Ops().subobjects;
    if (index >= ops.Size()) {
        return Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<ObjectHandle>::Success(
        *static_cast<const ObjectHandle*>(ops[index].field(*ownerObject.Value())));
}

Result<void> ConstructionContext::RestoreDeclaredSubobjectList(ObjectHandle owner) noexcept {
    if (auto state = CheckOpen(); !state) {
        return state;
    }

    const auto failWith = [this](ErrorCode error) noexcept {
        (void)Fail(error);
        return Result<void>::Failure(error);
    };
    auto ownerObject = Get<Object>(owner);
    if (!ownerObject) {
        return failWith(ownerObject.Error());
    }

    auto ownerType = AcquireType(owner);
    if (!ownerType) {
        return failWith(ownerType.Error());
    }

    const TypeLease& lease = ownerType.Value();
    const TypeSchema& schema = lease.Schema();
    const Span<const RuntimeSubobjectOps> ops = lease.Ops().subobjects;
    if (ops.Size() == 0) {
        return Result<void>::Success();
    }

    auto listAccess =
        FindAccess(registry_.Types(), lease, [](const RuntimeTypeOps& typeOps) noexcept {
            return typeOps.subobjectListAccess;
        });
    if (!listAccess || listAccess.Value() == nullptr) {
        return failWith(!listAccess ? listAccess.Error() : ErrorCode::InvalidArgument);
    }

    if (auto writable = CheckStagedWrite(); !writable) {
        return failWith(writable.Error());
    }

    // 소유 필드는 2a가 만든 subobject(Outer = owner, 이름 = 필드 도구 이름)여야 한다. Record가
    // null을 썼으면(선언 subobject의 파괴는 저장하지 않는다 — UE default subobject처럼) 그
    // subobject로 되돌리고, 다른 객체를 가리키면 손상이다(CorruptData).
    auto& list =
        *static_cast<detail::ObjectRefArrayStorage*>(listAccess.Value()(*ownerObject.Value()));
    Array<ObjectHandle> rebuilt(ObjectAllocator());
    if (auto reserved = rebuilt.TryReserve(ops.Size() + list.size); !reserved) {
        return failWith(reserved.Error());
    }

    for (usize index = 0; index < ops.Size(); ++index) {
        auto child = FindChild(owner, SubobjectName(schema, ops[index].id));
        if (!child) {
            return failWith(ErrorCode::CorruptData);
        }

        auto* field = static_cast<ObjectHandle*>(ops[index].field(*ownerObject.Value()));
        if (field->IsNull()) {
            if (auto set =
                    registry_.SetEdge(access_, owner, *field, child.Value(), transaction_, nullptr);
                !set) {
                return failWith(set.Error());
            }
        } else if (*field != child.Value()) {
            return failWith(ErrorCode::CorruptData);
        }

        (void)rebuilt.TryPushBack(child.Value());
    }

    // 나머지 원소는 지금 순서로 한 번씩(저장된 선언 subobject 원소는 앞에 이미 있다).
    for (uint32 index = 0; index < list.size; ++index) {
        bool declared = false;
        for (usize scan = 0; scan < ops.Size(); ++scan) {
            declared = declared || rebuilt[scan] == list.data[index];
        }

        if (!declared) {
            (void)rebuilt.TryPushBack(list.data[index]);
        }
    }

    if (auto assigned =
            registry_.AssignArrayEdges(access_, owner, list, rebuilt.View(), transaction_, nullptr);
        !assigned) {
        return failWith(assigned.Error());
    }

    return Result<void>::Success();
}

} // namespace rocks
