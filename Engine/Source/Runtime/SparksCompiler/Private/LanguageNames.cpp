#include "LanguageNames.h"

#include <SparksCompiler/LanguageCatalog.h>

namespace rocks::sparks::detail {

namespace {

using K = LanguageNameKind;

constexpr LanguageName Names[] = {
    {"bool", K::BuiltinType, 0, ""},
    {"int", K::BuiltinType, 0, ""},
    {"float", K::BuiltinType, 0, ""},
    {"int8", K::BuiltinType, 0, ""},
    {"int16", K::BuiltinType, 0, ""},
    {"int32", K::BuiltinType, 0, ""},
    {"int64", K::BuiltinType, 0, "int"},
    {"uint8", K::BuiltinType, 0, ""},
    {"uint16", K::BuiltinType, 0, ""},
    {"uint32", K::BuiltinType, 0, ""},
    {"uint64", K::BuiltinType, 0, ""},
    {"float32", K::BuiltinType, 0, ""},
    {"float64", K::BuiltinType, 0, "float"},
    {"string", K::BuiltinType, 0, ""},
    {"vec2", K::BuiltinType, 0, ""},
    {"vec3", K::BuiltinType, 0, ""},
    {"vec4", K::BuiltinType, 0, ""},
    {"quat", K::BuiltinType, 0, ""},
    {"mat4", K::BuiltinType, 0, ""},
    {"void", K::BuiltinType, 0, ""},
    {"Array", K::PreludeType, 1, ""},
    {"Set", K::PreludeType, 1, ""},
    {"Map", K::PreludeType, 2, ""},
    {"Result", K::PreludeType, 1, ""},
    {"Soft", K::PreludeType, 1, ""},
    {"Row", K::TypeLiteral, 1, ""},
    {"Class", K::PreludeType, 1, ""},
    {"CVarRef", K::PreludeType, 1, ""},
    {"LoadRequest", K::PreludeType, 1, ""},
    {"Text", K::PreludeType, 0, ""},
    {"Tag", K::TypeLiteral, 0, ""},
    {"TagSet", K::PreludeType, 0, ""},
    {"InputAction", K::PreludeType, 0, ""},
    {"Rotator", K::PreludeType, 0, ""},
    {"Transform", K::PreludeType, 0, ""},
    {"Color", K::PreludeType, 0, ""},
    {"Clock", K::PreludeType, 0, ""},
    {"WorldPosition", K::ReservedType, 0, ""},
    {"Log", K::PreludeValue, 0, ""},
    {"Assert", K::PreludeValue, 0, ""},
    {"Math", K::PreludeValue, 0, ""},
    {"Random", K::PreludeValue, 0, ""},
    {"Format", K::PreludeValue, 0, ""},
    {"Parse", K::PreludeValue, 0, ""},
    {"Delay", K::PreludeValue, 0, ""},
    {"NextTick", K::PreludeValue, 0, ""},
    {"NextFixedTick", K::PreludeValue, 0, ""},
    {"All", K::PreludeValue, 0, ""},
    {"Race", K::PreludeValue, 0, ""},
    {"Assets", K::PreludeValue, 0, ""},
    {"CVars", K::PreludeValue, 0, ""},
    {"Settings", K::PreludeValue, 0, ""},
    {"Action", K::Literal, 0, ""},
    {"Asset", K::Literal, 0, ""},
    {"Loc", K::Literal, 0, ""},
    {"CVar", K::Literal, 0, ""},
};

// schema가 제공하는 예약 이름(핵심 엔진 타입과 엔진 쪽 prelude 타입, 부록 A)이다.
constexpr StringView EngineReservedNames[] = {
    "Actor",       "Pawn",         "Component",        "SceneComponent", "ScriptableComponent",
    "LevelScript", "WorldService", "WidgetController", "Widget",         "DataAsset",
    "Collider",    "World",        "Connection",       "PlaySession",    "Hit",
    "Error"};

constexpr StringView OnPrefix = "On";

} // namespace

Span<const LanguageName> LanguageNames() noexcept {
    return Span<const LanguageName>(Names, sizeof(Names) / sizeof(Names[0]));
}

bool IsLanguageOwnedName(StringView name) noexcept {
    for (const LanguageName& entry : Names) {
        if (entry.name == name) {
            return true;
        }
    }

    return false;
}

bool IsReservedName(StringView name) noexcept {
    if (IsLanguageOwnedName(name) || name == ChangedEventName) {
        return true;
    }

    for (const StringView reserved : EngineReservedNames) {
        if (reserved == name) {
            return true;
        }
    }

    for (const StringView reserved : ContextNames) {
        if (reserved == name) {
            return true;
        }
    }

    for (const StringView reserved : RpcPrefixes) {
        if (reserved == name) {
            return true;
        }
    }

    return false;
}

StringView HookForMemberName(StringView name) noexcept {
    for (const LanguageHook& entry : LanguageHooks()) {
        const StringView hook = entry.name;
        if (hook == name) {
            return hook;
        }

        if (name.ByteCount() == OnPrefix.ByteCount() + hook.ByteCount() &&
            StringView(name.Data(), OnPrefix.ByteCount()) == OnPrefix &&
            StringView(name.Data() + OnPrefix.ByteCount(), hook.ByteCount()) == hook) {
            return hook;
        }
    }

    for (const UnityAlias& unity : UnityAliases()) {
        if (unity.hook && unity.unity == name) {
            return unity.rocks;
        }
    }

    return StringView();
}

} // namespace rocks::sparks::detail
