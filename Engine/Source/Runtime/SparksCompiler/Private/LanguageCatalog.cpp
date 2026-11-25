#include <SparksCompiler/LanguageCatalog.h>

#include "LanguageNames.h"

// 언어 단어 목록 중 hook·Unity 이름·언어 이름 판정이다(Docs/Design/SparksTools.ko.md 11.6절).
// attribute 목록은 CheckContexts.cpp, 키워드 목록은 Lexer.cpp에 있다(검사가 쓰는 표 옆).

namespace rocks::sparks {

namespace {

using P = HookParameters;

// §5.1의 hook이다(`Validate`는 full — 프로필은 S1113이 본다).
constexpr LanguageHook Hooks[] = {
    {"Awake", P::None, false},
    {"Enable", P::None, false},
    {"Start", P::None, false},
    {"Tick", P::Tick, false},
    {"FixedTick", P::Tick, false},
    {"LateTick", P::Tick, false},
    {"Disable", P::None, false},
    {"Destroy", P::None, false},
    {"TriggerEnter", P::Trigger, false},
    {"TriggerExit", P::Trigger, false},
    {"CollisionEnter", P::Collision, false},
    {"CollisionExit", P::Collision, false},
    {"Validate", P::None, true},
};

constexpr UnityAlias Aliases[] = {
    {"Update", "Tick", true},
    {"FixedUpdate", "FixedTick", true},
    {"LateUpdate", "LateTick", true},
    {"OnEnable", "Enable", true},
    {"OnDisable", "Disable", true},
    {"OnDestroy", "Destroy", true},
    {"OnTriggerEnter", "TriggerEnter", true},
    {"OnTriggerExit", "TriggerExit", true},
    {"OnCollisionEnter", "CollisionEnter", true},
    {"OnCollisionExit", "CollisionExit", true},
    {"OnValidate", "Validate", true},
    {"GetComponent", "Find", false},
};

} // namespace

Span<const LanguageHook> LanguageHooks() noexcept {
    return Span<const LanguageHook>(Hooks, sizeof(Hooks) / sizeof(Hooks[0]));
}

StringView HookParameterText(HookParameters parameters) noexcept {
    switch (parameters) {
    case HookParameters::Tick:
        return "(dt: float)";
    case HookParameters::Trigger:
        return "(other: Actor, trigger: Collider)";
    case HookParameters::Collision:
        return "(other: Actor, hit: Hit)";
    case HookParameters::None:
        break;
    }

    return "()";
}

Span<const UnityAlias> UnityAliases() noexcept {
    return Span<const UnityAlias>(Aliases, sizeof(Aliases) / sizeof(Aliases[0]));
}

bool IsUsableLanguageName(const Symbol& symbol) noexcept {
    if (symbol.kind != SymbolKind::BuiltinType && symbol.kind != SymbolKind::PreludeType &&
        symbol.kind != SymbolKind::PreludeValue) {
        return true;
    }

    const Span<const detail::LanguageName> names = detail::LanguageNames();
    return symbol.index < names.Size() && names[symbol.index].name == symbol.name &&
           names[symbol.index].kind != detail::LanguageNameKind::ReservedType;
}

} // namespace rocks::sparks
