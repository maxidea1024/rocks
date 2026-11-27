#include <Core/Move.h>
#include <MaterialSchema/MaterialGraphSchema.h>
#include <MaterialSchema/MaterialNodeCatalog.h>
#include <Meta/TypeSchema.h>

// 머티리얼 노드 정의의 등록과 조회다(Docs/Design/MaterialSystem 5.2·19.7절).
namespace rocks::material {

namespace {

char LowerAscii(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

// NamePool 비교 규칙(ASCII 대소문자 무시)과 같은 비교다.
bool SameKey(StringView left, StringView right) noexcept {
    if (left.ByteCount() != right.ByteCount()) {
        return false;
    }

    for (usize index = 0; index < left.ByteCount(); ++index) {
        if (LowerAscii(left.Data()[index]) != LowerAscii(right.Data()[index])) {
            return false;
        }
    }

    return true;
}

bool SameText(Span<const char> left, StringView right) noexcept {
    if (left.Size() != right.ByteCount()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right.Data()[index]) {
            return false;
        }
    }

    return true;
}

bool SchemaHasField(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (SameText(field.name, name)) {
            return true;
        }
    }

    return false;
}

// 새 노드 문자열 기본값의 대상 필드인가: 최상위 String·Name 필드다.
bool IsTextField(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (SameText(field.name, name)) {
            return field.value.kind == ValueKind::Utf8String || field.value.kind == ValueKind::Name;
        }
    }

    return false;
}

bool HasPin(Span<const MaterialPinDefinition> pins, StringView key,
            nodegraph::PinDirection direction) noexcept {
    for (const MaterialPinDefinition& pin : pins) {
        if (pin.direction == direction && SameKey(pin.key, key)) {
            return true;
        }
    }

    return false;
}

Result<void> ValidatePins(Span<const MaterialPinDefinition> pins) noexcept {
    for (usize index = 0; index < pins.Size(); ++index) {
        const MaterialPinDefinition& pin = pins[index];
        if (pin.key.IsEmpty() || static_cast<uint32>(pin.type) >= MaterialPinTypeCount ||
            (static_cast<uint8>(pin.flags) & ~MaterialPinFlagMask) != 0 ||
            pin.defaultValue.componentCount > MaxMaterialPinDefaultComponents) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (pin.direction != nodegraph::PinDirection::Input &&
            pin.direction != nodegraph::PinDirection::Output) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (pin.direction == nodegraph::PinDirection::Output &&
            (pin.defaultValue.componentCount != 0 || pin.flags != MaterialPinFlags::None ||
             pin.type == MaterialPinType::TextureAny)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize other = 0; other < index; ++other) {
            if (SameKey(pins[other].key, pin.key)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateDefinition(const MaterialNodeDefinition& definition) noexcept {
    if (definition.type.IsEmpty() ||
        (static_cast<uint8>(definition.flags) & ~MaterialNodeFlagMask) != 0 ||
        definition.stages == 0 || (definition.stages & ~AllMaterialStages) != 0 ||
        (definition.graphKinds & ~AllMaterialGraphKinds) != 0 ||
        (definition.features & ~AllMaterialContextFeatures) != 0 ||
        static_cast<uint32>(definition.prerequisite) >= MaterialPrerequisiteCount) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool missing = definition.status == MaterialNodeStatus::PrerequisiteMissing;
    const bool implemented = definition.status == MaterialNodeStatus::Implemented;
    if (!missing && !implemented) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (missing == (definition.prerequisite == MaterialPrerequisite::None)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!definition.nonSemanticFields.IsEmpty()) {
        if (definition.propertiesSchema == nullptr) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const TypeSchema& schema = definition.propertiesSchema();
        for (StringView field : definition.nonSemanticFields) {
            if (!SchemaHasField(schema, field)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    if (!definition.textDefaults.IsEmpty()) {
        if (definition.propertiesSchema == nullptr) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 필드마다 기본값 하나다(같은 필드가 두 번이면 어느 값이 이기는지 정해지지 않는다).
        const TypeSchema& schema = definition.propertiesSchema();
        const Span<const MaterialPropertyTextDefault> entries = definition.textDefaults;
        for (usize index = 0; index < entries.Size(); ++index) {
            if (!IsTextField(schema, entries[index].field)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            for (usize earlier = 0; earlier < index; ++earlier) {
                if (entries[earlier].field == entries[index].field) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }
            }
        }
    }

    for (const MaterialPassThrough& pair : definition.passThrough) {
        if (!HasPin(definition.pins, pair.input, nodegraph::PinDirection::Input) ||
            !HasPin(definition.pins, pair.output, nodegraph::PinDirection::Output)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    return ValidatePins(definition.pins);
}

nodegraph::PinValuePolicy ValuePolicyOf(const MaterialPinDefinition& pin) noexcept {
    return HasPinFlag(pin.flags, MaterialPinFlags::HideValue) ? nodegraph::PinValuePolicy::Hidden
                                                              : nodegraph::PinValuePolicy::Editable;
}

// 정의 하나의 NodeGraph 서술 배열(핀·통과 쌍)을 이름을 intern해 채운다.
Result<void> BuildDescriptorArrays(NamePool& names, const MaterialNodeDefinition& definition,
                                   Array<nodegraph::PinDescriptor>& pins,
                                   Array<nodegraph::PassThroughPair>& passThrough) noexcept {
    pins.Clear();
    passThrough.Clear();
    if (auto reserved = pins.TryReserve(definition.pins.Size()); !reserved) {
        return reserved;
    }

    if (auto reserved = passThrough.TryReserve(definition.passThrough.Size()); !reserved) {
        return reserved;
    }

    for (const MaterialPinDefinition& pin : definition.pins) {
        auto key = names.TryIntern(pin.key);
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        nodegraph::PinDescriptor descriptor =
            pin.direction == nodegraph::PinDirection::Input
                ? nodegraph::InputPin(key.Value(), ToPinTypeId(pin.type), pin.displayName)
                : nodegraph::OutputPin(key.Value(), ToPinTypeId(pin.type), pin.displayName);
        descriptor.advanced = HasPinFlag(pin.flags, MaterialPinFlags::Advanced);
        descriptor.valuePolicy = ValuePolicyOf(pin);
        // 용량을 미리 잡았으므로 실패하지 않는다.
        (void)pins.TryPushBack(descriptor);
    }

    for (const MaterialPassThrough& pair : definition.passThrough) {
        auto input = names.TryIntern(pair.input);
        auto output = names.TryIntern(pair.output);
        if (!input || !output) {
            return Result<void>::Failure(!input ? input.Error() : output.Error());
        }

        (void)passThrough.TryPushBack(nodegraph::PassThroughPair{input.Value(), output.Value()});
    }

    return Result<void>::Success();
}

} // namespace

MaterialNodeCatalog::MaterialNodeCatalog(Allocator& allocator) noexcept
    : allocator_(allocator), entries_(allocator), groups_(allocator), lookup_(allocator) {
}

void MaterialNodeCatalog::RebuildLookup() noexcept {
    lookup_.Clear();
    lookupComplete_ = true;
    for (const Entry& entry : entries_) {
        if (auto added = lookup_.FindOrAdd(entry.type, entry.definition); !added) {
            lookupComplete_ = false;
            return;
        }
    }
}

Result<MaterialNodeGroupId> MaterialNodeCatalog::Register(
    nodegraph::NodeTypeRegistry& registry, const MaterialGraphSchema& schema,
    Span<const MaterialNodeDefinition> definitions, ModuleLease lease) noexcept {
    const Span<const MaterialNodeDefinition> one[] = {definitions};
    return RegisterGroups(registry, schema, one, Move(lease));
}

Result<MaterialNodeGroupId> MaterialNodeCatalog::RegisterGroups(
    nodegraph::NodeTypeRegistry& registry, const MaterialGraphSchema& schema,
    Span<const Span<const MaterialNodeDefinition>> groups, ModuleLease lease) noexcept {
    using Outcome = Result<MaterialNodeGroupId>;
    if (!schema.Ready() || registry.FindSchema(schema.Domain()) != &schema) {
        return Outcome::Failure(ErrorCode::InvalidArgument);
    }

    for (usize groupIndex = 0; groupIndex < groups.Size(); ++groupIndex) {
        const Span<const MaterialNodeDefinition> definitions = groups[groupIndex];
        for (usize index = 0; index < definitions.Size(); ++index) {
            if (auto valid = ValidateDefinition(definitions[index]); !valid) {
                return Outcome::Failure(valid.Error());
            }

            for (usize otherGroup = 0; otherGroup <= groupIndex; ++otherGroup) {
                const Span<const MaterialNodeDefinition> others = groups[otherGroup];
                const usize end = otherGroup == groupIndex ? index : others.Size();
                for (usize other = 0; other < end; ++other) {
                    if (SameKey(others[other].type, definitions[index].type)) {
                        return Outcome::Failure(ErrorCode::InvalidArgument);
                    }
                }
            }
        }
    }

    if (auto reserved = groups_.TryReserve(groups_.Size() + 1); !reserved) {
        return Outcome::Failure(reserved.Error());
    }

    const MaterialNodeGroupId group{nextGroup_};
    const usize firstEntry = entries_.Size();
    const MaterialGraphKindMask kind = ToMask(schema.Kind());
    NamePool& names = registry.Names();
    Array<nodegraph::PinDescriptor> pins(allocator_);
    Array<nodegraph::PassThroughPair> passThrough(allocator_);
    Result<void> status = Result<void>::Success();

    for (const Span<const MaterialNodeDefinition> definitions : groups) {
        for (const MaterialNodeDefinition& definition : definitions) {
            if ((definition.graphKinds & kind) == 0) {
                continue;
            }

            auto type = names.TryIntern(definition.type);
            if (!type) {
                status = Result<void>::Failure(type.Error());
                break;
            }

            // 같은 종류 이름은 모든 도메인에서 같은 정의여야 한다(Find가 도메인 없이 찾는다).
            const MaterialNodeDefinition* existing = Find(type.Value());
            if (existing != nullptr && existing != &definition) {
                status = Result<void>::Failure(ErrorCode::AlreadyExists);
                break;
            }

            if (auto built = BuildDescriptorArrays(names, definition, pins, passThrough); !built) {
                status = Move(built);
                break;
            }

            nodegraph::NodeTypeDescriptor descriptor;
            descriptor.type = type.Value();
            descriptor.schemaVersion = definition.schemaVersion;
            if (definition.propertiesSchema != nullptr) {
                descriptor.propertiesStruct = definition.propertiesSchema().id;
            }

            descriptor.staticPins = pins.View();
            descriptor.dynamicPins = definition.dynamicPins;
            descriptor.passThrough = passThrough.View();
            descriptor.menu = definition.menu;
            descriptor.capabilities = definition.capabilities;
            descriptor.migrate = definition.migrate;

            // 등록표가 핀·통과 쌍을 소유 복사하므로 배열을 다음 정의에 다시 쓴다.
            auto registered = registry.RegisterNodeType(schema.Domain(), descriptor, lease);
            if (!registered) {
                status = Result<void>::Failure(registered.Error());
                break;
            }

            Entry entry;
            entry.type = type.Value();
            entry.definition = &definition;
            entry.group = group;
            entry.registration = registered.Value();
            if (auto pushed = entries_.TryPushBack(entry); !pushed) {
                (void)registry.UnregisterNodeType(registered.Value());
                status = Move(pushed);
                break;
            }
        }

        if (!status) {
            break;
        }
    }

    if (!status) {
        // 이 묶음에서 등록한 것을 역순으로 되돌린다.
        while (entries_.Size() > firstEntry) {
            (void)registry.UnregisterNodeType(entries_[entries_.Size() - 1].registration);
            (void)entries_.TryPopBack();
        }

        RebuildLookup();
        return Outcome::Failure(status.Error());
    }

    // 용량을 미리 잡았으므로 실패하지 않는다.
    (void)groups_.TryPushBack(group.value);
    ++nextGroup_;
    RebuildLookup();
    return Outcome::Success(group);
}

Result<void> MaterialNodeCatalog::Unregister(nodegraph::NodeTypeRegistry& registry,
                                             MaterialNodeGroupId id) noexcept {
    usize groupIndex = groups_.Size();
    for (usize index = 0; index < groups_.Size(); ++index) {
        if (groups_[index] == id.value) {
            groupIndex = index;
            break;
        }
    }

    if (groupIndex == groups_.Size()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    for (usize index = entries_.Size(); index > 0; --index) {
        const usize at = index - 1;
        if (entries_[at].group.value != id.value) {
            continue;
        }

        // registry에서 이미 빠진 종류(NotFound)는 빠진 것으로 본다.
        auto removed = registry.UnregisterNodeType(entries_[at].registration);
        if (!removed && removed.Error() != ErrorCode::NotFound) {
            RebuildLookup();
            return removed;
        }

        (void)entries_.TryRemoveAt(at);
    }

    (void)groups_.TryRemoveAt(groupIndex);
    RebuildLookup();
    return Result<void>::Success();
}

const MaterialNodeDefinition* MaterialNodeCatalog::Find(Name type) const noexcept {
    if (const MaterialNodeDefinition* const* found = lookup_.Find(type); found != nullptr) {
        return *found;
    }

    if (lookupComplete_) {
        return nullptr;
    }

    for (const Entry& entry : entries_) {
        if (entry.type == type) {
            return entry.definition;
        }
    }

    return nullptr;
}

} // namespace rocks::material
