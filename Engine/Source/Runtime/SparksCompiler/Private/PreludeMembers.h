#pragma once

#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>

// 언어 소유 멤버 표다(Docs/Design/SparksLanguage.ko.md 3.8절 "부록 B 시그니처", SD67). 컴파일러는
// 이 목록을 그대로 가지며 타입 검사(LC2c)가 멤버 접근과 호출을 이 표로 해석한다.
//
// 시그니처 표기: `(이름: 타입, ...): 반환`(반환이 없으면 void). 타입은 3.6절 `Type`과 같은
// 문자열이며 다음 자리표시를 쓴다: `$N` 모든 숫자 타입, `$S` 부호 있는 정수·실수 타입, `$I` 정수
// 타입, `$F` float 또는 벡터, `$V` 벡터(vec2·vec3·vec4 — 벡터 공통 멤버에서는 주인 타입 자신).
// Math의 자리표시는 오버로딩이다(인자를 모두 같은 타입으로 맞춘다, 3.1절). `T`·`K`·`V`는 주인
// 제네릭 타입의 타입 인자 (Array<T>, Map<K, V> 등), 함수 자기 타입 매개변수는 `<T>(...)`처럼 앞에
// 쓴다. 반환 앞의 `await`는 대기를 돌려준다는 뜻이다(값의 타입은 `await` 뒤). `(...)`는 대기 1개
// 이상을 받는 All·Race다.

namespace rocks::sparks::detail {

// 멤버의 형태다.
enum class PreludeMemberForm : uint8 {
    Field,    // `var` — 읽고 쓰는 필드
    Property, // `let` — 읽기 전용 속성
    Method,   // 값으로 부르는 함수
    Static,   // 타입 이름으로 부르는 함수(`Tag.Find`)
    Function, // 이름 공간·최상위의 함수(`Math.Min`, `Delay`)
    Constant, // 이름 공간의 상수(`Math.Pi`)
    Event,    // 기다리고 `on`으로 받는 이벤트(`InputAction.Triggered`)
    Case      // 언어 enum의 case(`Clock.Game`)
};

// 멤버가 있는 조건이다(원소 타입에 따라 — 부록 B의 [==]·[<]·[값]·[객체]).
enum class PreludeCondition : uint8 {
    Always,
    Equatable,     // 원소가 `==`를 쓸 수 있을 때
    Ordered,       // 원소가 `<`를 쓸 수 있을 때
    ValueElement,  // 원소가 값 타입일 때
    ObjectElement, // 원소가 객체 타입일 때
    NotVoid        // `Result<void>`가 아닐 때
};

struct PreludeMember {
    // 주인: 언어 이름 표의 타입·이름 공간(`vec3`, `Array`, `Math`), 벡터 공통은
    // `$V`(vec2·vec3·vec4에 각각 넣는다), 문맥 이름 Time의 타입은 `Time`, 최상위 prelude 함수는 빈
    // 문자열.
    StringView owner;
    StringView name;
    PreludeMemberForm form;
    // 함수는 시그니처, 필드·속성·상수는 타입, 이벤트는 빈 문자열이다.
    StringView signature;
    PreludeCondition condition = PreludeCondition::Always;
};

Span<const PreludeMember> PreludeMembers() noexcept;

// 벡터 공통 멤버의 주인 표시다.
inline constexpr StringView VectorFamilyOwner = "$V";

// 문맥 이름 `Time`의 타입 이름이다(사용자에게 보이는 타입 이름이 아니다 — 부록 B `Time`).
inline constexpr StringView TimeTypeName = "Time";

} // namespace rocks::sparks::detail
