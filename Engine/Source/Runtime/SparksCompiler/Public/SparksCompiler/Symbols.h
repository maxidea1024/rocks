#pragma once

#include <Base/NumericLimits.h>
#include <Core/Text/StringView.h>

// Sparks 이름 해석의 심볼과 타입이다(Docs/Design/SparksLanguage.ko.md 3.9절, LC2b). Compilation이
// 만들고 가진다(Compilation::Symbols·Types).
//
// 심볼은 이름 하나가 가리키는 대상 하나다: 언어 소유 이름(내장 타입·prelude), 문맥 이름, RPC 접두,
// 엔진 schema 타입·멤버, 사용자 선언(최상위·멤버·enum case·import 별칭·매개변수·지역 이름).
// 타입은 심볼과 타입 인자·`?`로 정해지며 같은 타입은 같은 번호다(intern).

namespace rocks::sparks {

// 심볼이 없음을 나타낸다.
inline constexpr uint32 NoSymbol = NumericLimits<uint32>::Max();

// 해석하지 못한 타입이다(타입 표의 0번). 이 타입이 낀 검사는 진단을 더 내지 않는다.
inline constexpr uint32 NoType = 0;

// 시그니처가 없음을 나타낸다.
inline constexpr uint32 NoSignature = NumericLimits<uint32>::Max();

enum class SymbolKind : uint8 {
    BuiltinType,         // `int` 등(index = 언어 이름 표 번호)
    PreludeType,         // `Array`·`Text`·`Tag`·`Clock` 등(index = 언어 이름 표 번호)
    PreludeValue,        // `Log`·`Math`·`Action` 등(index = 언어 이름 표 번호)
    ContextName,         // `World`·`Session`·`Input`·`Time`·`Caller`
    RpcPrefix,           // `Server`·`AllClients`·`OwnerClient`
    EngineType,          // schema 타입(index = schema 번호, item = 타입 번호)
    EngineField,         // schema 필드(owner = 타입 심볼, item = 필드 번호)
    EngineFunction,      // schema 함수(owner = 타입 심볼, item = 함수 번호)
    EngineEvent,         // schema 이벤트(owner = 타입 심볼, item = 이벤트 번호)
    EngineTypeParameter, // schema 함수의 타입 매개변수(owner = 함수 심볼)
    Alias,               // import 별칭(index = 대상 모듈)
    Class,
    Struct,
    Enum,
    Interface,
    Const,
    Function,        // 최상위 fn
    Field,           // class·struct 필드(owner = 타입 심볼)
    Method,          // class·struct 함수(owner = 타입 심볼)
    Event,           // class 이벤트(owner = 타입 심볼)
    EnumCase,        // owner = enum 심볼
    InterfaceMethod, // owner = interface 심볼
    Parameter,       // 함수·handler·이벤트·interface 메서드 매개변수(owner = 그 선언의 심볼)
    Local,           // 지역 변수·`if (let)` binder·`for` binder
    LanguageField,     // 부록 B 필드·속성·상수(owner = 주인 타입·이름 공간, index = 멤버 표 번호)
    LanguageFunction,  // 부록 B 함수(owner = 주인, 최상위 prelude 함수는 그 PreludeValue 심볼 자신)
    LanguageEvent,     // 부록 B 이벤트(`InputAction.Triggered`)
    LanguageTypeParameter // 부록 B의 타입 인자·자리표시(index = LanguageTypeParameterKind)
};

// 심볼 flag다.
struct SymbolFlags {
    static constexpr uint32 Open = 1u << 0;        // `open class`·`open fn`
    static constexpr uint32 Override = 1u << 1;    // `override fn`
    static constexpr uint32 Async = 1u << 2;       // `async fn`, schema `async`
    static constexpr uint32 Let = 1u << 3;         // `let` 필드·지역
    static constexpr uint32 Overridable = 1u << 4; // schema `overridable`
    static constexpr uint32 Static = 1u << 5;      // schema `static`
    static constexpr uint32 Erroneous =
        1u << 6; // 선언 이름에 진단을 냈다(import가 다시 알리지 않는다)
    static constexpr uint32 Incomplete = 1u << 7; // class 이름 공간이 불완전하다(3.9절 연쇄 방지)
    static constexpr uint32 Lifecycle = 1u << 8;  // 수명 클래스(기반 체인에 수명 엔진 타입)
    static constexpr uint32 ReadOnly = 1u << 9;    // 읽기 전용 속성(부록 B `let`, schema `access: read`)
    static constexpr uint32 Variadic = 1u << 10;   // 대기 1개 이상을 받는다(All·Race)
};

// 언어 타입 매개변수·자리표시의 종류다(LanguageTypeParameter 심볼의 index).
enum class LanguageTypeParameterKind : uint32 {
    Numeric,       // `$N` 모든 숫자 타입
    Signed,        // `$S` 부호 있는 정수·실수 타입
    Integer,       // `$I` 정수 타입
    FloatOrVector, // `$F` float 또는 벡터
    Vector,        // `$V` vec2·vec3·vec4
    Generic        // 주인 제네릭 타입·함수의 타입 인자(`T`·`K`·`V`, item = 위치)
};

// 심볼 하나다. 쓰지 않는 칸은 기본값이다.
struct Symbol {
    SymbolKind kind = SymbolKind::BuiltinType;
    uint32 flags = 0;
    // 이름이다. 원문·schema·정적 표의 글자를 가리키며 Compilation(과 SourceSet)이 사는 동안
    // 유효하다.
    StringView name;
    uint32 module = NumericLimits<uint32>::Max();   // 사용자 선언의 모듈(없으면 최댓값)
    uint32 node = NumericLimits<uint32>::Max();     // 선언 노드(Class·Field·Parameter 등)
    uint32 nameNode = NumericLimits<uint32>::Max(); // 이름 노드(진단·덤프 위치)
    uint32 owner = NoSymbol;
    uint32 index = 0;
    uint32 item = 0;
    // class·엔진 타입의 첫 기반 심볼이다(없으면 NoSymbol).
    uint32 base = NoSymbol;
    // class의 해석한 기반 전체(첫 기반 포함)가 Compilation::SymbolBases의 [firstBase,
    // +baseCount)다.
    uint32 firstBase = 0;
    uint32 baseCount = 0;
    // 선언 타입이다: 필드·const·매개변수·타입을 쓴 지역 이름, 엔진 필드. 없거나 해석 실패는 NoType.
    uint32 type = NoType;
    // 함수(최상위 fn·Method·InterfaceMethod·EngineFunction)의 시그니처 번호다.
    uint32 signature = NoSignature;
    // 제네릭 prelude 타입의 타입 인자 수다.
    uint32 arity = 0;
};

// 타입 하나다(타입 표의 원소). symbol은 그 타입을 정한 심볼(내장 타입은 정규 이름의 심볼)이다.
struct TypeEntry {
    uint32 symbol = NoSymbol;
    uint32 firstArgument = 0; // Compilation::TypeArguments의 구간
    uint32 argumentCount = 0;
    bool optional = false;
};

// 함수 시그니처다. 매개변수 타입은 Compilation::SignatureParameters의 구간이다.
struct Signature {
    uint32 returnType = NoType; // 반환 타입이 없으면 `void`
    uint32 firstParameter = 0;
    uint32 parameterCount = 0;
    bool isAsync = false;
};

} // namespace rocks::sparks
