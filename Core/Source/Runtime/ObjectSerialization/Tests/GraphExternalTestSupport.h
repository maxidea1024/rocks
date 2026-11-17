#pragma once

#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldCodec.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include "GraphTestSupport.h"

// graph v2 외부 참조 시험(GraphExternalTests.cpp·GraphExternalLoadTests.cpp)의 공용 타입·도우미다
// (Docs/Design/ObjectFoundations.ko.md 5.3·5.4절). Item은 Mark의 native 파생이고 단일 참조(Required
// link·EditorOnly editor)·참조 배열(links)·참조 struct 필드(hold.target)를 생성 코드와 같은 모양의
// 함수표로 가진다. 각 시험이 자기 TypeRegistry에 등록한다.
namespace external_graph_fixture {

struct Item;

// 참조 struct다(struct 필드 — 중첩 Record 안의 외부 참조 위치).
struct Hold {
    rocks::ObjectRef<Item> target;
    rocks::uint32 tag = 0;
};

inline constexpr rocks::TypeId MarkType{0x4745585454ULL, 1};
inline constexpr rocks::TypeId ItemType{0x4745585454ULL, 2};
inline constexpr rocks::TypeId OtherType{0x4745585454ULL, 3};
inline constexpr rocks::TypeId HoldType{0x4745585454ULL, 4};
inline constexpr rocks::Guid NumberId{0x4745585446ULL, 1};
inline constexpr rocks::Guid LinkId{0x4745585446ULL, 2};
inline constexpr rocks::Guid EditorId{0x4745585446ULL, 3};
inline constexpr rocks::Guid LinksId{0x4745585446ULL, 4};
inline constexpr rocks::Guid HoldId{0x4745585446ULL, 5};
inline constexpr rocks::Guid HoldTargetId{0x474558544dULL, 1};
inline constexpr rocks::Guid HoldTagId{0x474558544dULL, 2};
inline constexpr rocks::uint32 Required = static_cast<rocks::uint32>(rocks::FieldFlags::Required);
inline constexpr rocks::uint32 EditorOnly =
    static_cast<rocks::uint32>(rocks::FieldFlags::EditorOnly);

// Item의 native 기반이다(필드 없음 — 2b의 "선언 타입은 맞지만 기대 형식은 아님" 시험 대상).
struct Mark : rocks::Object {
    explicit Mark(rocks::Allocator&) noexcept {
    }
};

struct Item final : Mark {
    explicit Item(rocks::Allocator& allocator) noexcept : Mark(allocator), links(allocator) {
        ++Constructed;
    }

    void Trace(rocks::TraceVisitor& visitor) const noexcept;

    static inline rocks::uint32 Constructed = 0;
    rocks::uint32 number = 0;
    rocks::ObjectRef<Item> link;
    rocks::ObjectRef<Item> editor;
    rocks::ObjectRefArray<Item> links;
    Hold hold;
};

// Item과 관계없는 타입이다(선언 타입 불일치 시험).
struct Other final : rocks::Object {
    explicit Other(rocks::Allocator&) noexcept {
    }
};

} // namespace external_graph_fixture

namespace rocks {

template <>
struct GeneratedType<external_graph_fixture::Hold> {
    static constexpr bool ContainsStrongReference = true;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return external_graph_fixture::HoldType;
    }

    static void Trace(const external_graph_fixture::Hold& value, TraceVisitor& visitor) noexcept {
        visitor.Visit(value.target);
    }
};

} // namespace rocks

inline void external_graph_fixture::Item::Trace(rocks::TraceVisitor& visitor) const noexcept {
    visitor.Visit(link);
    visitor.Visit(editor);
    links.Trace(visitor);
    rocks::GeneratedType<Hold>::Trace(hold, visitor);
}

namespace rocks::test::external {

using namespace external_graph_fixture;

inline FieldSchema Scalar(Guid id, const char* name, usize bytes, ValueKind kind, TypeId type = {},
                          uint32 flags = 0) noexcept {
    FieldSchema field;
    field.id = id;
    field.kind = kind == ValueKind::ObjectReference ? FieldKind::ObjectReference : FieldKind::Value;
    field.flags = flags;
    field.name = {name, bytes};
    field.value = ValueType{kind, type};
    return field;
}

// 생성 코드와 같은 모양의 클래스 필드 함수표다(codec만 — struct·배열 필드).
template <auto Member>
RuntimeFieldOps CodecField(Guid id, const void* valueNative = nullptr) noexcept {
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
    return ops;
}

// 단일 값 필드 함수표다(read·write·writeStaged와 생성 codec).
template <auto Member>
RuntimeFieldOps ScalarField(Guid id) noexcept {
    using O = typename detail::StructMemberPointer<decltype(Member)>::Owner;
    RuntimeFieldOps ops = CodecField<Member>(id);
    ops.read = +[](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
                   FieldValue& value) noexcept {
        return detail::ReadGeneratedField<O>(registry, access, object, Member, value);
    };
    ops.write = +[](ObjectRegistry& registry, const ObjectAccessContext& access,
                    ObjectHandle object, const FieldValue& value) noexcept {
        return detail::WriteGeneratedField<O>(registry, access, object, Member, value);
    };
    ops.writeStaged = +[](ConstructionContext& transaction, ObjectHandle object,
                          const FieldValue& value) noexcept {
        return detail::WriteGeneratedStagedField<O>(transaction, object, Member, value);
    };
    return ops;
}

// Hold(struct) → Mark → Item(Mark의 native 파생) → Other 순서로 등록한다.
inline bool RegisterTypes(TypeRegistry& types) noexcept {
    const FieldSchema holdFields[] = {
        Scalar(HoldTargetId, "target", 6, ValueKind::ObjectReference, ItemType),
        Scalar(HoldTagId, "tag", 3, ValueKind::UInt32)};
    const RuntimeStructFieldOps holdOps[] = {
        detail::StructFieldOps<Hold, &Hold::target>(HoldTargetId),
        detail::StructFieldOps<Hold, &Hold::tag>(HoldTagId)};
    RuntimeStructOps hold = MakeRuntimeStructOps<Hold>();
    hold.trace = [](const void* value, TraceVisitor& visitor) noexcept {
        GeneratedType<Hold>::Trace(*static_cast<const Hold*>(value), visitor);
    };
    hold.fields = holdOps;
    if (!types.RegisterStruct({HoldType, 1, holdFields, {"Hold", 4}}, Move(hold))) {
        return false;
    }

    auto mark = MakeRuntimeTypeOps<Mark>();
    mark.completeFieldLayout = true;
    if (!types.Register({MarkType, 1, {}, {"Mark", 4}}, Move(mark))) {
        return false;
    }

    const FieldSchema itemFields[] = {
        Scalar(NumberId, "number", 6, ValueKind::UInt32),
        Scalar(LinkId, "link", 4, ValueKind::ObjectReference, ItemType, Required),
        Scalar(EditorId, "editor", 6, ValueKind::ObjectReference, ItemType, EditorOnly),
        [] {
            FieldSchema field = Scalar(LinksId, "links", 5, ValueKind::Array);
            field.kind = FieldKind::ObjectReference;
            field.element = ValueType{ValueKind::ObjectReference, ItemType};
            field.maxElements = MaxArrayElements;
            return field;
        }(),
        Scalar(HoldId, "hold", 4, ValueKind::Struct, HoldType)};
    const RuntimeFieldOps itemOps[] = {
        ScalarField<&Item::number>(NumberId), ScalarField<&Item::link>(LinkId),
        ScalarField<&Item::editor>(EditorId), CodecField<&Item::links>(LinksId),
        CodecField<&Item::hold>(HoldId, NativeTypeKey<Hold>())};
    TypeSchema itemSchema{ItemType, 1, itemFields, {"Item", 4}};
    itemSchema.baseType = MarkType;
    auto item = MakeRuntimeTypeOps<Item>();
    item.fields = itemOps;
    item.nativeBaseType = NativeTypeKey<Mark>();
    item.completeFieldLayout = true;
    if (!types.Register(itemSchema, Move(item))) {
        return false;
    }

    auto other = MakeRuntimeTypeOps<Other>();
    other.completeFieldLayout = true;
    return types.Register({OtherType, 1, {}, {"Other", 5}}, Move(other)).IsOk();
}

// 객체를 만들고 roots에 RootRef를 쥔다(handle을 돌려준다).
inline Result<ObjectHandle> New(ObjectRegistry& registry, Array<RootRef<Object>>& roots,
                                TypeId type = ItemType, ObjectHandle outer = {},
                                StringView name = {}) noexcept {
    auto made = registry.NewObject<Object>(registry.OwnerAccess(), type, outer, name);
    if (!made) {
        return Result<ObjectHandle>::Failure(made.Error());
    }

    const ObjectHandle handle = made.Value().Handle();
    if (!roots.TryPushBack(Move(made.Value()))) {
        return Result<ObjectHandle>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<ObjectHandle>::Success(handle);
}

// 생존 Item 하나를 고친다(edit(registry, owner, item)이 참조·값을 쓴다).
template <class Edit>
Result<void> Change(ObjectRegistry& registry, ObjectHandle owner, Edit edit) noexcept {
    auto borrowed = registry.Resolve<Item>(registry.OwnerAccess(), owner);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return edit(*value.Value());
}

// owner의 참조를 정한다: link·editor·hold.target과 links 원소(뒤에 붙인다).
inline Result<void> SetLink(ObjectRegistry& registry, ObjectHandle owner,
                            ObjectHandle target) noexcept {
    return Change(registry, owner, [&](Item& item) noexcept {
        return registry.SetReference(registry.OwnerAccess(), owner, item.link, target);
    });
}

inline Result<void> SetEditor(ObjectRegistry& registry, ObjectHandle owner,
                              ObjectHandle target) noexcept {
    return Change(registry, owner, [&](Item& item) noexcept {
        return registry.SetReference(registry.OwnerAccess(), owner, item.editor, target);
    });
}

inline Result<void> SetHold(ObjectRegistry& registry, ObjectHandle owner,
                            ObjectHandle target) noexcept {
    return Change(registry, owner, [&](Item& item) noexcept {
        return registry.SetReference(registry.OwnerAccess(), owner, item.hold.target, target);
    });
}

inline Result<void> AddLink(ObjectRegistry& registry, ObjectHandle owner,
                            ObjectHandle target) noexcept {
    return Change(registry, owner, [&](Item& item) noexcept {
        return registry.AddReference(registry.OwnerAccess(), owner, item.links, target);
    });
}

inline Result<void> SetNumber(ObjectRegistry& registry, ObjectHandle owner,
                              uint32 number) noexcept {
    return Change(registry, owner, [&](Item& item) noexcept {
        item.number = number;
        return Result<void>::Success();
    });
}

// Item의 값이다(실패하면 0·null). links는 앞 네 원소다.
struct Seen {
    uint32 number = 0;
    ObjectHandle link{};
    ObjectHandle editor{};
    ObjectHandle hold{};
    usize linkCount = 0;
    ObjectHandle links[4] = {};
};

inline Seen Read(const Item& item) noexcept {
    Seen seen;
    seen.number = item.number;
    seen.link = item.link.Handle();
    seen.editor = item.editor.Handle();
    seen.hold = item.hold.target.Handle();
    seen.linkCount = item.links.Size();
    for (usize index = 0; index < seen.linkCount && index < 4; ++index) {
        seen.links[index] = item.links[index];
    }

    return seen;
}

inline Seen Look(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    auto borrowed = registry.Resolve<Item>(registry.OwnerAccess(), handle);
    if (!borrowed) {
        return {};
    }

    auto value = borrowed.Value().Get();
    return value ? Read(*value.Value()) : Seen{};
}

// 공개 전 Item의 값이다(같은 트랜잭션).
inline Seen LookStaged(ConstructionContext& transaction, ObjectHandle handle) noexcept {
    auto value = transaction.Get<Item>(handle);
    return value ? Read(*value.Value()) : Seen{};
}

inline ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 외부 참조 허락과 진단을 기록하는 hook이다. 대상별 결정(nulls·rejects)이 없으면 Import이고,
// failure가 있으면 그 오류를, undefined면 정의되지 않은 결과 값을 돌려준다. destroyIn이 있으면
// destroyTarget의 허락을 묻는 동안 그 대상을 파괴 요청한다. 앞 8개의 호출·진단을 기록하고 수는
// 모두 센다.
struct Admitter final : FieldCodecHooks {
    struct Call {
        ObjectHandle target{};
        TypeId targetType{};
        TypeId expectedType{};
        uint32 flags = 0;
        bool required = false;
        uint32 objectIndex = 0;
        usize depth = 0;
        Guid field{};
        bool indexed = false;
        uint32 index = 0;
        uint32 slot = 0;
    };

    struct Diagnostic {
        FieldDiagnosticKind kind = FieldDiagnosticKind::UnknownField;
        int64 value = 0;
        uint32 objectIndex = 0;
        Guid field{};
    };

    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (reports < 8) {
            Diagnostic& entry = diagnostics[reports];
            entry.kind = diagnostic.kind;
            entry.value = diagnostic.value;
            entry.objectIndex = diagnostic.objectIndex;
            entry.field = diagnostic.path.Size() != 0
                              ? diagnostic.path[diagnostic.path.Size() - 1].fieldId
                              : Guid{};
        }

        ++reports;
        return Result<void>::Success();
    }

    Result<ExternalAdmission>
    AdmitExternalReference(const ExternalReferenceInfo& info) noexcept override {
        if (calls < 8) {
            Call& entry = entries[calls];
            entry.target = info.target;
            entry.targetType = info.targetType;
            entry.expectedType = info.expectedType;
            entry.flags = info.effectiveFlags;
            entry.required = info.required;
            entry.objectIndex = info.objectIndex;
            entry.depth = info.path.Size();
            if (info.path.Size() != 0) {
                const PathStep& last = info.path[info.path.Size() - 1];
                entry.field = last.fieldId;
                entry.indexed = last.selector == PathSelector::Index;
                entry.index = last.index;
            }

            entry.slot = info.slot;
        }

        ++calls;
        // 계약 위반 모의: 허락을 묻는 동안 그 대상을 파괴 요청한다.
        if (destroyIn != nullptr && info.target == destroyTarget) {
            (void)destroyIn->RequestDestroy(destroyIn->OwnerAccess(), info.target);
        }

        if (failure != ErrorCode::None) {
            return Result<ExternalAdmission>::Failure(failure);
        }

        if (undefined) {
            return Result<ExternalAdmission>::Success(static_cast<ExternalAdmission>(7));
        }

        for (ObjectHandle target : nulls) {
            if (target == info.target) {
                return Result<ExternalAdmission>::Success(ExternalAdmission::Null);
            }
        }

        for (ObjectHandle target : rejects) {
            if (target == info.target) {
                return Result<ExternalAdmission>::Success(ExternalAdmission::Reject);
            }
        }

        return Result<ExternalAdmission>::Success(ExternalAdmission::Import);
    }

    Call entries[8] = {};
    usize calls = 0;
    Diagnostic diagnostics[8] = {};
    usize reports = 0;
    Span<const ObjectHandle> nulls{};
    Span<const ObjectHandle> rejects{};
    ErrorCode failure = ErrorCode::None;
    bool undefined = false;
    ObjectRegistry* destroyIn = nullptr;
    ObjectHandle destroyTarget{};
};

// Admit 정책·NodeTable 저장 옵션이다.
inline ObjectGraphSaveOptions AdmitOptions(Admitter* hooks) noexcept {
    ObjectGraphSaveOptions options;
    options.externals = GraphExternalPolicy::Admit;
    options.writeNodeTable = true;
    options.hooks = hooks;
    return options;
}

// count개 importTypes를 모두 type으로 채운다.
inline Array<TypeId> ImportTypes(Allocator& allocator, usize count,
                                 TypeId type = ItemType) noexcept {
    Array<TypeId> types(allocator);
    if (!types.TryResize(count)) {
        rocks::platform::Fatal("External test import types allocation failed");
    }

    for (usize index = 0; index < count; ++index) {
        types[index] = type;
    }

    return types;
}

} // namespace rocks::test::external
