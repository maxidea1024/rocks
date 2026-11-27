#include <MaterialSchema/MaterialAttributeTable.h>

#include "Translator.h"

// 속성 기록 저장이다(Docs/Design/MaterialSystem 6.12절 MS2e1).
namespace rocks::material::detail {

Result<IrValueId> Translator::AddAttributes(Span<const AttributeEntry> entries) noexcept {
    AttributeRange range;
    range.first = static_cast<uint32>(attributeEntries_.Size());
    for (const AttributeEntry& entry : entries) {
        if (entry.value == NoIrValue) {
            continue;
        }

        // 같은 Guid는 뒤의 것이 이긴다(Set이 덮는다).
        bool replaced = false;
        for (usize index = range.first; index < attributeEntries_.Size(); ++index) {
            if (attributeEntries_[index].id == entry.id) {
                attributeEntries_[index].value = entry.value;
                replaced = true;
            }
        }

        if (!replaced) {
            if (auto added = attributeEntries_.TryPushBack(entry); !added) {
                return Result<IrValueId>::Failure(added.Error());
            }
        }
    }

    range.count = static_cast<uint32>(attributeEntries_.Size()) - range.first;
    const uint32 index = static_cast<uint32>(attributeRanges_.Size());
    if (auto added = attributeRanges_.TryPushBack(range); !added) {
        return Result<IrValueId>::Failure(added.Error());
    }

    return builder_.AttributesRef(index);
}

Result<bool> Translator::CopyAttributes(IrValueId value,
                                        Array<AttributeEntry>& out) const noexcept {
    out.Clear();
    if (value == NoIrValue || value >= builder_.Count()) {
        return Result<bool>::Success(false);
    }

    const IrValue& reference = builder_.Value(value);
    if (reference.op != IrOp::AttributesRef || reference.immediate[0] >= attributeRanges_.Size()) {
        return Result<bool>::Success(false);
    }

    const AttributeRange range = attributeRanges_[static_cast<usize>(reference.immediate[0])];
    for (uint32 index = 0; index < range.count; ++index) {
        if (auto added = out.TryPushBack(attributeEntries_[range.first + index]); !added) {
            return Result<bool>::Failure(added.Error());
        }
    }

    return Result<bool>::Success(true);
}

const MaterialAttributeDefinition* Translator::FindAttribute(Guid id) const noexcept {
    for (const MaterialAttributeDefinition& definition : BuiltinMaterialAttributes()) {
        if (definition.id == id) {
            return &definition;
        }
    }

    return input_.attributes != nullptr ? input_.attributes->Find(id) : nullptr;
}

const MaterialAttributeDefinition* Translator::FindAttributeByName(StringView name) const noexcept {
    for (const MaterialAttributeDefinition& definition : BuiltinMaterialAttributes()) {
        if (definition.name == name) {
            return &definition;
        }
    }

    return nullptr;
}

} // namespace rocks::material::detail
