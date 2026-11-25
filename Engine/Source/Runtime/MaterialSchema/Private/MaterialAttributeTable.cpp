#include <Core/Move.h>
#include <MaterialSchema/MaterialAttributeTable.h>

// 머티리얼 속성 표다(Docs/Design/MaterialSystem 19.12절).
namespace rocks::material {

namespace {

bool IsSingleStage(MaterialStageMask stage) noexcept {
    return stage == MaterialVertexStage || stage == MaterialPixelStage;
}

Result<void> ValidateAttribute(const MaterialAttributeDefinition& definition) noexcept {
    const bool valid = !(definition.id == Guid{}) && !definition.name.IsEmpty() &&
                       static_cast<uint32>(definition.type) < MaterialPinTypeCount &&
                       definition.defaultValue.componentCount <= MaxMaterialPinDefaultComponents &&
                       IsSingleStage(definition.frequency);
    return valid ? Result<void>::Success() : Result<void>::Failure(ErrorCode::InvalidArgument);
}

} // namespace

MaterialAttributeTable::MaterialAttributeTable(Allocator& allocator) noexcept
    : entries_(allocator), groups_(allocator) {
}

Result<MaterialAttributeGroupId>
MaterialAttributeTable::Register(Span<const MaterialAttributeDefinition> definitions,
                                 ModuleLease lease) noexcept {
    using Outcome = Result<MaterialAttributeGroupId>;
    for (usize index = 0; index < definitions.Size(); ++index) {
        if (auto valid = ValidateAttribute(definitions[index]); !valid) {
            return Outcome::Failure(valid.Error());
        }

        for (usize other = 0; other < index; ++other) {
            if (definitions[other].id == definitions[index].id) {
                return Outcome::Failure(ErrorCode::InvalidArgument);
            }
        }

        if (Find(definitions[index].id) != nullptr) {
            return Outcome::Failure(ErrorCode::AlreadyExists);
        }
    }

    if (auto reserved = entries_.TryReserve(entries_.Size() + definitions.Size()); !reserved) {
        return Outcome::Failure(reserved.Error());
    }

    if (auto reserved = groups_.TryReserve(groups_.Size() + 1); !reserved) {
        return Outcome::Failure(reserved.Error());
    }

    const MaterialAttributeGroupId group{nextGroup_++};
    for (const MaterialAttributeDefinition& definition : definitions) {
        // 용량을 미리 잡았으므로 실패하지 않는다.
        (void)entries_.TryPushBack(Entry{&definition, group});
    }

    (void)groups_.TryPushBack(Group{group, Move(lease)});
    return Outcome::Success(group);
}

Result<void> MaterialAttributeTable::Unregister(MaterialAttributeGroupId id) noexcept {
    usize groupIndex = groups_.Size();
    for (usize index = 0; index < groups_.Size(); ++index) {
        if (groups_[index].id.value == id.value) {
            groupIndex = index;
            break;
        }
    }

    if (groupIndex == groups_.Size()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    for (usize index = entries_.Size(); index > 0; --index) {
        if (entries_[index - 1].group.value == id.value) {
            (void)entries_.TryRemoveAt(index - 1);
        }
    }

    // 정의를 표에서 뺀 뒤 lease를 놓는다.
    (void)groups_.TryRemoveAt(groupIndex);
    return Result<void>::Success();
}

const MaterialAttributeDefinition* MaterialAttributeTable::Find(Guid id) const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.definition->id == id) {
            return entry.definition;
        }
    }

    return nullptr;
}

} // namespace rocks::material
