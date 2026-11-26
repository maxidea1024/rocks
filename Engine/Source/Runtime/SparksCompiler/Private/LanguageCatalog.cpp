#include <SparksCompiler/LanguageCatalog.h>

#include <Core/Text/EditDistance.h>

#include "LanguageNames.h"

// 언어 단어 목록 중 hook·Unity 이름·언어 이름 판정과 hook·attribute 이름 제안이다
// (Docs/Design/SparksTools.ko.md 11.6절·11.7절).
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

// 이름 제안의 편집 거리 상한이다.
constexpr usize MaxSuggestionDistance = 2;

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

StringView SimilarHook(StringView name) noexcept {
    StringView best;
    usize bestDistance = MaxSuggestionDistance + 1;
    for (const LanguageHook& hook : Hooks) {
        const usize distance = EditDistance(name, hook.name, MaxSuggestionDistance,
                                            EditDistanceCase::AsciiInsensitive);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = hook.name;
        }
    }

    return best;
}

StringView SimilarAttribute(StringView name) noexcept {
    StringView best;
    usize bestDistance = MaxSuggestionDistance + 1;
    for (uint32 at = 0; at < LanguageAttributeCount(); ++at) {
        const StringView candidate = LanguageAttributeAt(at).name;
        const usize distance = EditDistance(name, candidate, MaxSuggestionDistance,
                                            EditDistanceCase::AsciiInsensitive);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }

    return best;
}

uint32 LanguageOwnedNameCount() noexcept {
    return static_cast<uint32>(detail::LanguageNames().Size());
}

StringView LanguageOwnedNameAt(uint32 index) noexcept {
    const Span<const detail::LanguageName> names = detail::LanguageNames();
    return index < names.Size() ? names[index].name : StringView();
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
