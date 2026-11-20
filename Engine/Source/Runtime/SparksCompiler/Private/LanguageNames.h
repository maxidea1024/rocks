#pragma once

#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>

// 언어가 소유하는 이름 표다(Docs/Design/SparksLanguage.ko.md 3.7절 부록 A, 3.8절 부록 B, 3.9절
// "이름의 출처" SD62). binding schema reader(언어 소유 이름 금지)와 이름 해석이 같이 쓴다.

namespace rocks::sparks::detail {

// 언어 소유 이름의 종류다.
enum class LanguageNameKind : uint8 {
    BuiltinType,  // `int`, `vec3` ... 소문자 내장 타입
    PreludeType,  // `Array<T>`, `Text`, `Clock` ...
    PreludeValue, // `Log`, `Math`, `Delay` ...
    Literal,      // 검증 리터럴 중 타입이 아닌 것(`Action`, `Asset`, `Loc`, `CVar`)
    TypeLiteral,  // 타입이면서 검증 리터럴인 것(`Tag`, `Row<T>`)
    ReservedType  // 이름만 예약한 타입(`WorldPosition` — 쓰면 S0318)
};

struct LanguageName {
    StringView name;
    LanguageNameKind kind;
    uint8 arity; // 제네릭 타입 인자 수(아니면 0)
    // 같은 타입의 정규 이름이다(`int64` → `int`, `float64` → `float`). 비어 있으면 자기 이름이다.
    StringView canonical;
};

// 언어 소유 이름 전부다(내장 타입 → prelude 타입 → prelude 값 순, 순서는 이름 해석의 심볼 번호를
// 정한다 — 결정적이다).
Span<const LanguageName> LanguageNames() noexcept;

// 언어 소유 이름(내장 타입·prelude 타입·prelude 값·검증 리터럴)인지다. binding schema 타입 이름으로
// 쓸 수 없다.
bool IsLanguageOwnedName(StringView name) noexcept;

// 문맥 이름이다(3.4절 표).
inline constexpr StringView ContextNames[] = {"World", "Session", "Input", "Time", "Caller"};

// RPC 접두다(full, 쓰임 규칙은 LC3).
inline constexpr StringView RpcPrefixes[] = {"Server", "AllClients", "OwnerClient"};

// 사용자 선언 이름으로 쓸 수 없는 이름인지다(부록 A — 언어 소유 이름, 핵심 엔진 타입, schema가
// 제공하는 prelude 타입, 문맥 이름, RPC 접두, `Changed`). 철자가 정확히 같을 때만 참이다.
bool IsReservedName(StringView name) noexcept;

// 수명 클래스의 멤버 이름으로 쓸 수 없는 hook 계열 이름이면 대신 쓸 hook 이름(`Update` → `Tick`,
// `OnStart` → `Start`)을, 아니면 빈 문자열을 돌려준다.
StringView HookForMemberName(StringView name) noexcept;

} // namespace rocks::sparks::detail
