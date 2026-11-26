#pragma once

#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/Symbols.h>

// 컴파일러가 아는 언어 단어 목록이다(Docs/Design/SparksTools.ko.md 11.6절 — 언어 서버 자동완성).
// 검사가 쓰는 표를 그대로 내보낸다: hook은 이 목록이 정본이고(CheckEvents가 이 목록으로 검사한다),
// attribute는 attribute 규칙 표(CheckContexts), 키워드는 lexer 키워드 표를 읽는다. 사본 표를 두지
// 않는다(T13 — 도구와 검사가 같은 목록을 본다).
//
// 사용 예:
//   for (const LanguageHook& hook : LanguageHooks()) {
//       if (!hook.fullOnly || profile == Profile::Full) {
//           Offer(hook.name, HookParameterText(hook.parameters));
//       }
//   }
//
// 수명: 글자는 정적 자료다(프로그램 내내 유효). 스레드: 어디서 불러도 된다.

namespace rocks::sparks {

// hook의 인자 모양이다(Docs/Design/SparksLanguage 5.1절).
enum class HookParameters : uint8 {
    None,      // ()
    Tick,      // (dt: float)
    Trigger,   // (other: Actor, trigger: Collider)
    Collision, // (other: Actor, hit: Hit)
};

// 수명 hook 하나다. fullOnly는 core 패키지에서 쓸 수 없음(S1113 — `Validate`)이다.
struct LanguageHook {
    StringView name;
    HookParameters parameters;
    bool fullOnly;
};

// attribute·키워드 하나다. fullOnly는 core 패키지에서 쓸 수 없음(1.3절 full 목록 — S1113)이다.
struct LanguageWord {
    StringView name;
    bool fullOnly;
};

// Unity 이름 하나와 대응하는 Rocks 이름이다(SL 13절). hook이면 수명 hook 이름, 아니면 엔진 함수
// 이름(`GetComponent` → `Find`)이다.
struct UnityAlias {
    StringView unity;
    StringView rocks;
    bool hook;
};

// core 패키지에서 쓸 수 없는 prelude 타입 이름이다(`Class<T>` — S1113).
inline constexpr StringView FullOnlyPreludeTypeName = "Class";

// 수명 hook 전부다(5.1절 순서).
ROCKS_SPARKSCOMPILER_API Span<const LanguageHook> LanguageHooks() noexcept;

// 인자 모양의 원문 표기다(`(dt: float)`, 인자 없음은 `()`).
ROCKS_SPARKSCOMPILER_API StringView HookParameterText(HookParameters parameters) noexcept;

// attribute 전부다(attribute 규칙 표 순서). index가 범위 밖이면 빈 이름이다.
ROCKS_SPARKSCOMPILER_API uint32 LanguageAttributeCount() noexcept;
ROCKS_SPARKSCOMPILER_API LanguageWord LanguageAttributeAt(uint32 index) noexcept;

// 키워드 전부다(lexer 표 순서). index가 범위 밖이면 빈 이름이다.
ROCKS_SPARKSCOMPILER_API uint32 LanguageKeywordCount() noexcept;
ROCKS_SPARKSCOMPILER_API LanguageWord LanguageKeywordAt(uint32 index) noexcept;

// name과 비슷한 hook·attribute 이름이다(ASCII 대소문자 무시 편집 거리 2 이하 중 가장 가까운 것,
// 같으면 목록 순서의 앞 — S0803·S0620의 수정 예와 언어 서버 빠른 수정이 같은 제안을 쓴다). 없으면
// 빈 글자다.
ROCKS_SPARKSCOMPILER_API StringView SimilarHook(StringView name) noexcept;
ROCKS_SPARKSCOMPILER_API StringView SimilarAttribute(StringView name) noexcept;

// Unity 이름 대응 전부다.
ROCKS_SPARKSCOMPILER_API Span<const UnityAlias> UnityAliases() noexcept;

// 언어 이름 심볼(BuiltinType·PreludeType·PreludeValue)이 원문에 쓸 수 있는 이름인지다: 이름만
// 예약한 타입(`WorldPosition` — S0318)과 언어 이름 표 밖의 타입(문맥 이름 `Time`의 타입)은
// 거짓이다. 다른 종류의 심볼은 참이다.
ROCKS_SPARKSCOMPILER_API bool IsUsableLanguageName(const Symbol& symbol) noexcept;

} // namespace rocks::sparks
