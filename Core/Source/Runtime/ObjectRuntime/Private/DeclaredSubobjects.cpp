#include <Base/TypeTraits.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/SubobjectDefaultsContext.h>
#include <Object/TypeRegistry.h>

// 선언 subobject의 생성·배선·기본값 덮기다(Docs/Design/Prefab 10.2·10.3절 — UE
// CreateDefaultSubobject 대응). ObjectRuntime은 소유 타입을 모르므로 생성 연산표의 필드
// 접근자(RuntimeSubobjectOps·subobject*Access)로 필드 주소를 얻는다. ObjectRef·WeakObjectRef·
// ObjectRefArray는 standard-layout이고 첫 멤버(handle·배열 저장소)와 주소가 같으므로 그 주소를
// 첫 멤버 주소로 바꿔 쓴다(포인터 상호 변환).
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

// 소유 타입 base 사슬의 최대 깊이다(InitSubobjectDefaults를 가장 먼 base부터 부를 때 사슬을
// 모으는 고정 배열 크기 — 리플렉션 클래스 사슬은 이보다 훨씬 얕다).
constexpr usize MaxSubobjectOwnerDepth = 64;

using FieldAccess = void* (*)(Object&) noexcept;

// lease 타입부터 base 사슬을 걸어 access(ops)가 처음으로 null이 아닌 접근자를 찾는다. 없으면
// null이다(지정자를 선언한 클래스만 접근자를 싣는다).
FieldAccess FindAccess(TypeRegistry& types, const TypeLease& lease,
                       FieldAccess (*access)(const RuntimeTypeOps&) noexcept) noexcept {
    if (FieldAccess found = access(lease.Ops())) {
        return found;
    }

    TypeId base = lease.Schema().baseType;
    for (usize depth = 0; base != TypeId{} && depth < MaxSubobjectOwnerDepth; ++depth) {
        auto next = types.Acquire(base);
        if (!next) {
            return nullptr;
        }

        if (FieldAccess found = access(next.Value().Ops())) {
            return found;
        }

        base = next.Value().Schema().baseType;
    }

    return nullptr;
}

// 필드 schema의 실효 도구 이름(Name= 또는 C++ 이름)이다 — 선언 subobject의 객체 이름이다.
StringView SubobjectName(const TypeSchema& schema, Guid id) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (field.id == id) {
            const Span<const char> name = field.toolName.Size() != 0 ? field.toolName : field.name;
            return StringView(name.Data(), name.Size());
        }
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

    auto ownerType = AcquireType(owner);
    if (!ownerType) {
        return Result<usize>::Failure(ownerType.Error());
    }

    const TypeLease& lease = ownerType.Value();
    const TypeSchema& schema = lease.Schema();
    const Span<const SubobjectSchema> table = schema.subobjects;
    const usize count = table.Size();
    if (count == 0) {
        return Result<usize>::Success(0);
    }

    // 표가 있는데 결과 칸이 모자라거나 소유 목록 접근자가 없으면 호출·등록 계약 위반이다.
    TypeRegistry& types = registry_.Types();
    const FieldAccess listAccess = FindAccess(
        types, lease, [](const RuntimeTypeOps& ops) noexcept { return ops.subobjectListAccess; });
    const FieldAccess rootAccess = FindAccess(
        types, lease, [](const RuntimeTypeOps& ops) noexcept { return ops.subobjectRootAccess; });
    bool hasRoot = false;
    for (const SubobjectSchema& entry : table) {
        hasRoot = hasRoot || (entry.flags & static_cast<uint32>(SubobjectFlags::Root)) != 0;
    }

    if (out.Size() < count || listAccess == nullptr || (hasRoot && rootAccess == nullptr)) {
        (void)Fail(ErrorCode::InvalidArgument);
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    auto ownerObject = Get<Object>(owner);
    if (!ownerObject) {
        (void)Fail(ownerObject.Error());
        return Result<usize>::Failure(ownerObject.Error());
    }

    // 소유 목록은 소유 객체를 막 만든 직후라 비어 있어야 한다(선언 subobject가 목록 앞에 온다).
    auto& list = *static_cast<detail::ObjectRefArrayStorage*>(listAccess(*ownerObject.Value()));
    if (list.size != 0) {
        (void)Fail(ErrorCode::InvalidArgument);
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    // 1. 표 순서로 만든다(Outer = 소유 객체, 이름 = 소유 필드 도구 이름). 생성 실패는 트랜잭션을
    //    오류로 고정한다(Create 규칙).
    Object* objects[MaxClassSubobjects];
    for (usize index = 0; index < count; ++index) {
        auto created =
            CreateImpl(table[index].type, nullptr, owner, SubobjectName(schema, table[index].id));
        if (!created) {
            return Result<usize>::Failure(created.Error());
        }

        out[index] = created.Value();
        if (auto marked =
                registry_.SetPendingOrigin(out[index], transaction_, ObjectOrigin::ClassSubobject);
            !marked) {
            (void)Fail(marked.Error());
            return Result<usize>::Failure(marked.Error());
        }

        auto object = Get<Object>(out[index]);
        if (!object) {
            (void)Fail(object.Error());
            return Result<usize>::Failure(object.Error());
        }

        objects[index] = object.Value();
    }

    // 2. 배선: 소유 필드, 소유 목록(표 순서), Root 필드, 부착 부모.
    const auto failWith = [this](ErrorCode error) noexcept {
        (void)Fail(error);
        return Result<usize>::Failure(error);
    };
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

    for (usize index = 0; index < count; ++index) {
        const SubobjectSchema& entry = table[index];
        if ((entry.flags & static_cast<uint32>(SubobjectFlags::Root)) != 0) {
            auto* root = static_cast<ObjectHandle*>(rootAccess(*ownerObject.Value()));
            if (auto set =
                    registry_.SetEdge(access_, owner, *root, out[index], transaction_, nullptr);
                !set) {
                return failWith(set.Error());
            }
        }

        if (entry.attachParent >= 0) {
            auto subobjectType = AcquireType(out[index]);
            if (!subobjectType) {
                return failWith(subobjectType.Error());
            }

            const FieldAccess attachAccess =
                FindAccess(types, subobjectType.Value(), [](const RuntimeTypeOps& ops) noexcept {
                    return ops.subobjectAttachAccess;
                });
            if (attachAccess == nullptr) {
                return failWith(ErrorCode::InvalidArgument);
            }

            // weak 참조는 GC 간선이 아니므로 staged 객체에 바로 쓴다(같은 트랜잭션의 대상).
            *static_cast<ObjectHandle*>(attachAccess(*objects[index])) =
                out[static_cast<usize>(entry.attachParent)];
        }
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

} // namespace rocks
