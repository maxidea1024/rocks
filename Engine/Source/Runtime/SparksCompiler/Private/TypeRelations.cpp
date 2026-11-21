#include "LanguageNames.h"
#include "TypeChecker.h"

// 타입 관계다: 숫자 분류, 대입 가능(3.10절 "대입 가능"), 파생, `==`·`<`를 쓸 수 있는 타입(3.3절
// "비교"), 기본값이 있는 타입(3.2절), 보간할 수 있는 타입, 제네릭 치환.

namespace rocks::sparks::detail {

namespace {

struct NumericName {
    StringView name;
    NumericKind kind;
    uint32 bits;
};

constexpr NumericName NumericNames[] = {
    {"int8", NumericKind::Signed, 8},      {"int16", NumericKind::Signed, 16},
    {"int32", NumericKind::Signed, 32},    {"int", NumericKind::Signed, 64},
    {"uint8", NumericKind::Unsigned, 8},   {"uint16", NumericKind::Unsigned, 16},
    {"uint32", NumericKind::Unsigned, 32}, {"uint64", NumericKind::Unsigned, 64},
    {"float32", NumericKind::Float, 32},   {"float", NumericKind::Float, 64},
};

constexpr StringView VectorNames[] = {"vec2", "vec3", "vec4"};

} // namespace

TypeChecker::TypeChecker(NameBinder& binder) noexcept
    : binder_(binder), compilation_(binder.compilation_), allocator_(binder.allocator_),
      inference_(binder.allocator_), constants_(binder.allocator_),
      exhaustiveSwitches_(binder.allocator_), structDefault_(binder.allocator_),
      firstCase_(binder.allocator_), nextCase_(binder.allocator_), staleRead_(binder.allocator_) {
}

void TypeChecker::AppendDisplayType(StringBuilderBase& out, uint32 type) const noexcept {
    if (type == NoType || type >= compilation_.types_.Size()) {
        out.Append("?");
        return;
    }

    const TypeEntry& entry = compilation_.types_[type];
    const Symbol& symbol = compilation_.symbols_[entry.symbol];
    if (symbol.kind == SymbolKind::BuiltinType) {
        const StringView canonical = LanguageNames()[symbol.index].canonical;
        out.Append(canonical.IsEmpty() ? symbol.name : canonical);
    } else {
        out.Append(symbol.name);
    }

    if (entry.argumentCount != 0) {
        out.Append("<");
        for (uint32 index = 0; index < entry.argumentCount; ++index) {
            if (index != 0) {
                out.Append(", ");
            }

            AppendDisplayType(out, compilation_.typeArguments_[entry.firstArgument + index]);
        }

        out.Append(">");
    }

    if (entry.optional) {
        out.Append("?");
    }
}

uint32 TypeChecker::SymbolOf(uint32 type) const noexcept {
    return type == NoType || type >= compilation_.types_.Size() ? NoSymbol
                                                                : compilation_.types_[type].symbol;
}

uint32 TypeChecker::Argument(uint32 type, uint32 index) const noexcept {
    const TypeEntry& entry = compilation_.types_[type];
    return index < entry.argumentCount ? compilation_.typeArguments_[entry.firstArgument + index]
                                       : NoType;
}

bool TypeChecker::IsOptional(uint32 type) const noexcept {
    return type != NoType && compilation_.types_[type].optional;
}

uint32 TypeChecker::NonOptional(uint32 type) noexcept {
    if (!IsOptional(type)) {
        return type;
    }

    const TypeEntry entry = compilation_.types_[type];
    return binder_.InternType(
        entry.symbol,
        Span<const uint32>(compilation_.typeArguments_.Data() + entry.firstArgument,
                           entry.argumentCount),
        false);
}

uint32 TypeChecker::MakeOptional(uint32 type) noexcept {
    if (type == NoType || IsOptional(type)) {
        return type;
    }

    const TypeEntry entry = compilation_.types_[type];
    return binder_.InternType(
        entry.symbol,
        Span<const uint32>(compilation_.typeArguments_.Data() + entry.firstArgument,
                           entry.argumentCount),
        true);
}

uint32 TypeChecker::TypeOfSymbol(uint32 symbol) noexcept {
    return binder_.InternType(symbol, Span<const uint32>(), false);
}

NumericInfo TypeChecker::Numeric(uint32 type) const noexcept {
    const uint32 symbol = SymbolOf(type);
    if (symbol == NoSymbol || IsOptional(type)) {
        return NumericInfo{};
    }

    const Symbol& entry = compilation_.symbols_[symbol];
    if (entry.kind != SymbolKind::BuiltinType) {
        return NumericInfo{};
    }

    for (const NumericName& name : NumericNames) {
        if (name.name == entry.name) {
            return NumericInfo{name.kind, name.bits};
        }
    }

    return NumericInfo{};
}

bool TypeChecker::IsLanguage(uint32 type, StringView name) const noexcept {
    const uint32 symbol = SymbolOf(type);
    if (symbol == NoSymbol) {
        return false;
    }

    const Symbol& entry = compilation_.symbols_[symbol];
    return (entry.kind == SymbolKind::BuiltinType || entry.kind == SymbolKind::PreludeType) &&
           entry.name == name;
}

bool TypeChecker::IsVector(uint32 type) const noexcept {
    for (const StringView name : VectorNames) {
        if (IsLanguage(type, name) && !IsOptional(type)) {
            return true;
        }
    }

    return false;
}

bool TypeChecker::IsReference(uint32 type) const noexcept {
    const uint32 symbol = SymbolOf(type);
    if (symbol == NoSymbol) {
        return false;
    }

    const Symbol& entry = compilation_.symbols_[symbol];
    if (entry.kind == SymbolKind::Class || entry.kind == SymbolKind::Interface) {
        return true;
    }

    if (entry.kind != SymbolKind::EngineType) {
        return false;
    }

    const SchemaTypeKind kind = compilation_.schemas_[entry.index].schema.Types()[entry.item].kind;
    return kind == SchemaTypeKind::Object || kind == SchemaTypeKind::Asset ||
           kind == SchemaTypeKind::Interface;
}

bool TypeChecker::IsAssetType(uint32 type) const noexcept {
    uint32 at = SymbolOf(type);
    for (uint32 step = 0; at != NoSymbol && step < compilation_.symbols_.Size(); ++step) {
        const Symbol& entry = compilation_.symbols_[at];
        if (entry.kind == SymbolKind::EngineType) {
            return compilation_.schemas_[entry.index].schema.Types()[entry.item].kind ==
                   SchemaTypeKind::Asset;
        }

        if (entry.kind != SymbolKind::Class) {
            return false;
        }

        at = entry.base;
    }

    return false;
}

bool TypeChecker::IsEnumType(uint32 type) const noexcept {
    const uint32 symbol = SymbolOf(type);
    if (symbol == NoSymbol || IsOptional(type)) {
        return false;
    }

    const Symbol& entry = compilation_.symbols_[symbol];
    if (entry.kind == SymbolKind::Enum) {
        return true;
    }

    if (entry.kind == SymbolKind::EngineType) {
        return compilation_.schemas_[entry.index].schema.Types()[entry.item].kind ==
               SchemaTypeKind::Enum;
    }

    return entry.kind == SymbolKind::PreludeType && entry.name == "Clock";
}

// type이 base의 파생(자기 포함 — class 기반 체인·Sparks interface·schema `base`)인지다.
bool TypeChecker::IsDerived(uint32 type, uint32 base) const noexcept {
    const uint32 target = SymbolOf(base);
    uint32 at = SymbolOf(type);
    for (uint32 step = 0; at != NoSymbol && step < compilation_.symbols_.Size(); ++step) {
        if (at == target) {
            return true;
        }

        const Symbol& entry = compilation_.symbols_[at];
        if (entry.kind == SymbolKind::Class) {
            for (uint32 index = 1; index < entry.baseCount; ++index) {
                uint32 face = compilation_.symbolBases_[entry.firstBase + index];
                for (uint32 inner = 0; face != NoSymbol && inner < compilation_.symbols_.Size();
                     ++inner) {
                    if (face == target) {
                        return true;
                    }

                    face = compilation_.symbols_[face].base;
                }
            }
        }

        at = entry.base;
    }

    return false;
}

bool TypeChecker::IsAssignable(uint32 from, uint32 to) noexcept {
    if (from == to || from == NoType || to == NoType) {
        return true;
    }

    const bool targetOptional = IsOptional(to);
    if (IsOptional(from) && !targetOptional) {
        return false;
    }

    const uint32 source = NonOptional(from);
    const uint32 target = NonOptional(to);
    if (source == target) {
        return true;
    }

    // 참조 타입의 파생(제네릭은 무공변이므로 타입 인자가 없는 참조끼리만).
    return IsReference(source) && IsReference(target) &&
           compilation_.types_[source].argumentCount == 0 &&
           compilation_.types_[target].argumentCount == 0 && IsDerived(source, target);
}

bool TypeChecker::IsEquatable(uint32 type) const noexcept {
    const uint32 symbol = SymbolOf(type);
    if (symbol == NoSymbol) {
        return false;
    }

    const TypeEntry& entry = compilation_.types_[type];
    const Symbol& target = compilation_.symbols_[symbol];
    const NumericInfo numeric = [&]() noexcept {
        for (const NumericName& name : NumericNames) {
            if (target.kind == SymbolKind::BuiltinType && name.name == target.name) {
                return NumericInfo{name.kind, name.bits};
            }
        }

        return NumericInfo{};
    }();
    if (numeric.kind == NumericKind::Signed || numeric.kind == NumericKind::Unsigned) {
        return true;
    }

    if (target.kind == SymbolKind::BuiltinType) {
        return target.name == "bool" || target.name == "string";
    }

    if (target.kind == SymbolKind::PreludeType) {
        return target.name == "Tag" || target.name == "InputAction" || target.name == "Soft" ||
               target.name == "Row" || target.name == "Clock";
    }

    if (target.kind == SymbolKind::Enum) {
        return true;
    }

    if (target.kind == SymbolKind::EngineType) {
        const SchemaTypeKind kind =
            compilation_.schemas_[target.index].schema.Types()[target.item].kind;
        return kind == SchemaTypeKind::Enum || kind == SchemaTypeKind::Object ||
               kind == SchemaTypeKind::Interface;
    }

    static_cast<void>(entry);
    return target.kind == SymbolKind::Class || target.kind == SymbolKind::Interface;
}

bool TypeChecker::IsOrdered(uint32 type) const noexcept {
    if (IsOptional(type)) {
        return false;
    }

    return Numeric(type).kind != NumericKind::None || IsLanguage(type, "string") ||
           IsEnumType(type);
}

bool TypeChecker::HasDefault(uint32 type) const noexcept {
    if (type == NoType || IsOptional(type)) {
        return true;
    }

    const uint32 symbol = SymbolOf(type);
    const Symbol& entry = compilation_.symbols_[symbol];
    switch (entry.kind) {
    case SymbolKind::BuiltinType:
        return true;
    case SymbolKind::PreludeType:
        // Tag·InputAction·Soft·Row에는 기본값이 없다(3.2절 SD19).
        return !(entry.name == "Tag" || entry.name == "InputAction" || entry.name == "Soft" ||
                 entry.name == "Row" || entry.name == "Result" || entry.name == "LoadRequest" ||
                 entry.name == "CVarRef" || entry.name == "Class");
    case SymbolKind::Enum:
        return true;
    case SymbolKind::Struct:
        // 모든 필드에 초기값이 있거나 그 타입에 기본값이 있어야 한다(AnalyzeStructs가 정했다).
        return symbol >= structDefault_.Size() || structDefault_[symbol] != StructDefaultMissing;
    case SymbolKind::EngineType: {
        const SchemaTypeKind kind =
            compilation_.schemas_[entry.index].schema.Types()[entry.item].kind;
        if (kind == SchemaTypeKind::Struct) {
            return symbol >= structDefault_.Size() ||
                   structDefault_[symbol] != StructDefaultMissing;
        }

        return kind == SchemaTypeKind::Enum;
    }
    default:
        return false; // 객체·에셋·interface 참조
    }
}

bool TypeChecker::IsInterpolatable(uint32 type) const noexcept {
    if (type == NoType) {
        return true;
    }

    if (IsOptional(type)) {
        return false;
    }

    if (Numeric(type).kind != NumericKind::None || IsEnumType(type) || IsVector(type)) {
        return true;
    }

    constexpr StringView Names[] = {"bool",    "string", "Tag",      "quat",
                                    "Rotator", "Color",  "Transform"};
    for (const StringView name : Names) {
        if (IsLanguage(type, name)) {
            return true;
        }
    }

    return false;
}

bool TypeChecker::ContainsPlaceholder(uint32 type) const noexcept {
    if (type == NoType) {
        return false;
    }

    const TypeEntry& entry = compilation_.types_[type];
    const Symbol& symbol = compilation_.symbols_[entry.symbol];
    if (symbol.kind == SymbolKind::LanguageTypeParameter &&
        symbol.index != static_cast<uint32>(LanguageTypeParameterKind::Generic)) {
        return true;
    }

    for (uint32 index = 0; index < entry.argumentCount; ++index) {
        if (ContainsPlaceholder(compilation_.typeArguments_[entry.firstArgument + index])) {
            return true;
        }
    }

    return false;
}

bool TypeChecker::PlaceholderAccepts(uint32 placeholder, uint32 type) const noexcept {
    const Symbol& entry = compilation_.symbols_[placeholder];
    const NumericInfo numeric = Numeric(type);
    switch (static_cast<LanguageTypeParameterKind>(entry.index)) {
    case LanguageTypeParameterKind::Numeric:
        return numeric.kind != NumericKind::None;
    case LanguageTypeParameterKind::Signed:
        return numeric.kind == NumericKind::Signed || numeric.kind == NumericKind::Float;
    case LanguageTypeParameterKind::Integer:
        return numeric.kind == NumericKind::Signed || numeric.kind == NumericKind::Unsigned;
    case LanguageTypeParameterKind::FloatOrVector:
        return (numeric.kind == NumericKind::Float && numeric.bits == 64) || IsVector(type);
    case LanguageTypeParameterKind::Vector:
        return IsVector(type);
    default:
        return false;
    }
}

uint32 TypeChecker::Substitute(uint32 type, uint32 receiver, Span<const uint32> functionArguments,
                               uint32 chosen) noexcept {
    if (type == NoType) {
        return NoType;
    }

    const TypeEntry entry = compilation_.types_[type];
    const Symbol& symbol = compilation_.symbols_[entry.symbol];
    uint32 replacement = NoType;
    bool replaced = false;
    if (symbol.kind == SymbolKind::LanguageTypeParameter) {
        replaced = true;
        if (symbol.index != static_cast<uint32>(LanguageTypeParameterKind::Generic)) {
            replacement = chosen;
        } else if (receiver != NoType && SymbolOf(NonOptional(receiver)) == symbol.owner) {
            replacement = Argument(NonOptional(receiver), symbol.item);
        } else if (symbol.item < functionArguments.Size()) {
            replacement = functionArguments[symbol.item];
        }
    } else if (symbol.kind == SymbolKind::EngineTypeParameter) {
        replaced = true;
        const Symbol& function = compilation_.symbols_[symbol.owner];
        const SchemaFunction& source =
            compilation_.schemas_[function.index].schema.Functions()[function.item];
        const uint32 position = symbol.item - source.firstTypeParameter;
        replacement = position < functionArguments.Size() ? functionArguments[position] : NoType;
    }

    if (replaced) {
        return entry.optional ? MakeOptional(replacement) : replacement;
    }

    if (entry.argumentCount == 0) {
        return type;
    }

    Array<uint32> arguments(allocator_);
    for (uint32 index = 0; index < entry.argumentCount; ++index) {
        const uint32 argument = Substitute(compilation_.typeArguments_[entry.firstArgument + index],
                                           receiver, functionArguments, chosen);
        if (argument == NoType) {
            return NoType;
        }

        if (auto pushed = arguments.TryPushBack(argument); !pushed) {
            binder_.error_ = pushed.Error();
            return NoType;
        }
    }

    return binder_.InternType(entry.symbol, arguments.View(), entry.optional);
}

} // namespace rocks::sparks::detail
