#pragma once

#include "NameBinder.h"

// 타입 검사(LC2c)다(Docs/Design/SparksLanguage.ko.md 3.10절, S0326·S04xx·S05xx·S0109). 이름 해석
// (NameBinder)의 이름 공간과 심볼을 그대로 쓰며 NameBinder::Run의 마지막에 돈다. 파일:
//   TypeRelations.cpp    숫자 분류·대입 가능·파생·`==`·`<`·기본값·제네릭 치환·타입 문장
//   CheckExpressions.cpp 식·연산자·상수·기대 타입 맞추기
//   CheckCalls.cpp       멤버 접근·호출·생성·변환·검증 리터럴
//   CheckStatements.cpp  선언 초기값·본문·문장·반환·switch

namespace rocks::sparks::detail {

// 숫자 타입의 분류다.
enum class NumericKind : uint8 {
    None,
    Signed,
    Unsigned,
    Float
};

struct NumericInfo {
    NumericKind kind = NumericKind::None;
    uint32 bits = 0;
};

// 컴파일 때 아는 값이다(상수식·switch case·const).
struct ConstantValue {
    enum class Kind : uint8 {
        None,
        Integer,
        Float,
        Bool,
        String,
        Enum
    };

    Kind kind = Kind::None;
    bool negative = false; // Integer: 값은 negative ? -magnitude : magnitude
    uint64 magnitude = 0;
    double real = 0.0;
    bool boolean = false;
    uint32 symbol = NoSymbol; // Enum: case 심볼
    StringView text;          // String: 원문 토큰(같은 리터럴 비교용)
};

// 식 하나의 결과다.
struct ExpressionValue {
    enum class Kind : uint8 {
        Error,        // 해석하지 못했다(진단은 이미 있다 — 더 내지 않는다)
        Typed,        // 그 타입의 값
        UntypedInt,   // 타입 없는 정수 상수
        UntypedFloat, // 타입 없는 실수 상수
        Null,         // `null`
        Void,         // 결과 없음
        Wait,         // 대기(type은 await 결과)
        TypeName,     // 식에 쓴 타입 이름(symbol)
        Namespace,    // prelude 이름 공간·import 별칭(symbol)
        Function,     // 부를 함수(symbol, receiver)
        Event,        // 이벤트(symbol, receiver) — await하면 void
        Literal       // 검증 리터럴 이름(`Action`·`Asset`·`Loc`·`CVar` — symbol)
    };

    Kind kind = Kind::Error;
    uint32 type = NoType;
    uint32 symbol = NoSymbol;
    uint32 receiver = NoType; // Function·Event: 수신자 타입(인스턴스 멤버·제네릭 치환)
    bool instance = false;    // Function: 값으로 불렀다(static·이름 공간 함수가 아니다)
    bool chained = false;     // `?.`로 얻었다(호출 결과도 `?`가 된다)
    ConstantValue constant;
};

// 본문 하나의 검사 자리다.
struct CheckContext {
    uint32 module = NoModule;
    uint32 owner = NoSymbol;     // class·struct 심볼
    uint32 thisType = NoType;    // class 본문이면 그 타입(아니면 `this` 없음)
    uint32 returnType = NoType;  // 함수 반환 타입(void 포함, 모르면 NoType)
    bool returnKnown = false;    // 반환 규칙을 검사하는지(시그니처를 해석했다)
    bool resultFunction = false; // 반환 타입이 Result<T>
    uint32 resultValue = NoType; // Result<T>의 T(Result<void>면 void 타입)
};

class TypeChecker final {
public:
    explicit TypeChecker(NameBinder& binder) noexcept;

    void Run() noexcept;

private:
    // 노드 대표 토큰의 글자다(연산자·멤버 이름·리터럴).
    StringView TokenText(uint32 module, uint32 node) const noexcept;

    // ---- 타입 관계(TypeRelations.cpp) ----
    NumericInfo Numeric(uint32 type) const noexcept;
    bool IsOptional(uint32 type) const noexcept;
    uint32 NonOptional(uint32 type) noexcept;
    uint32 MakeOptional(uint32 type) noexcept;
    uint32 SymbolOf(uint32 type) const noexcept;
    uint32 Argument(uint32 type, uint32 index) const noexcept;
    uint32 TypeOfSymbol(uint32 symbol) noexcept;
    bool IsReference(uint32 type) const noexcept;
    bool IsAssetType(uint32 type) const noexcept;
    bool IsEnumType(uint32 type) const noexcept;
    bool IsVector(uint32 type) const noexcept;
    bool IsLanguage(uint32 type, StringView name) const noexcept;
    bool IsDerived(uint32 type, uint32 base) const noexcept;
    bool IsAssignable(uint32 from, uint32 to) noexcept;
    bool IsEquatable(uint32 type) const noexcept;
    bool IsOrdered(uint32 type) const noexcept;
    bool HasDefault(uint32 type) const noexcept;
    bool IsInterpolatable(uint32 type) const noexcept;
    // 제네릭 치환: 주인 타입 인자(receiver)·함수 타입 인자·Math 자리표시(chosen)를 바꾼다.
    uint32 Substitute(uint32 type, uint32 receiver, Span<const uint32> functionArguments,
                      uint32 chosen) noexcept;
    bool ContainsPlaceholder(uint32 type) const noexcept;
    bool PlaceholderAccepts(uint32 placeholder, uint32 type) const noexcept;

    // ---- 식(CheckExpressions.cpp) ----
    ExpressionValue Check(const CheckContext& context, uint32 node, uint32 expected) noexcept;
    ExpressionValue CheckIdentifier(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue ValueOfSymbol(const CheckContext& context, uint32 symbol, uint32 receiver,
                                  bool instance) noexcept;
    ExpressionValue CheckBinary(const CheckContext& context, uint32 node, uint32 expected) noexcept;
    ExpressionValue CheckUnary(const CheckContext& context, uint32 node, uint32 expected) noexcept;
    ExpressionValue CheckIndex(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CheckCollection(const CheckContext& context, uint32 node,
                                    uint32 expected) noexcept;
    ExpressionValue CheckString(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CheckCast(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CheckAwaitTry(const CheckContext& context, uint32 node) noexcept;
    // 값을 type 자리에 맞춘다(대입 가능·상수 범위). 맞으면 그 타입, 아니면 진단을 내고 NoType.
    uint32 Coerce(const CheckContext& context, const ExpressionValue& value, uint32 type,
                  uint32 node) noexcept;
    // 자리 타입이 없을 때의 타입(상수는 int·float). 값이 아니면 진단을 내고 NoType.
    uint32 Materialize(const CheckContext& context, const ExpressionValue& value,
                       uint32 node) noexcept;
    // 값 하나를 요구하는 자리에서 값이 아닌 것(S0405·S0422)을 알린다. 값이면 true.
    bool RequireValue(const CheckContext& context, const ExpressionValue& value,
                      uint32 node) noexcept;
    bool ConstantFits(const ConstantValue& constant, bool integer, uint32 type) const noexcept;
    void Record(const CheckContext& context, uint32 node, uint32 type) noexcept;
    void Report(const CheckContext& context, DiagnosticCode code, uint32 node,
                StringView message, StringView explanation, StringView fix) noexcept;
    void ReportAt(const CheckContext& context, DiagnosticCode code, SourceSpan span,
                  StringView message, StringView explanation, StringView fix) noexcept;
    void ReportMismatch(const CheckContext& context, uint32 node, uint32 from,
                        uint32 to) noexcept;

    // ---- 멤버·호출(CheckCalls.cpp) ----
    ExpressionValue CheckMember(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CheckCall(const CheckContext& context, uint32 node, uint32 expected) noexcept;
    ExpressionValue CallFunction(const CheckContext& context, uint32 node,
                                 const ExpressionValue& callee, uint32 typeArguments) noexcept;
    ExpressionValue Construct(const CheckContext& context, uint32 node, uint32 typeSymbol) noexcept;
    ExpressionValue Convert(const CheckContext& context, uint32 node, uint32 typeSymbol) noexcept;
    ExpressionValue CheckLiteral(const CheckContext& context, uint32 node, uint32 literal,
                                 uint32 expected) noexcept;
    // 타입의 멤버(자기·기반 체인, 언어 멤버)를 찾는다.
    uint32 FindMember(uint32 type, StringView name) const noexcept;
    bool MemberAvailable(uint32 member, uint32 receiver) const noexcept;

    // ---- 문장(CheckStatements.cpp) ----
    void CheckModule(uint32 module) noexcept;
    void CheckFunction(const CheckContext& outer, uint32 function, uint32 symbol) noexcept;
    void CheckStatement(const CheckContext& context, uint32 node) noexcept;
    void CheckSwitch(const CheckContext& context, uint32 node) noexcept;
    void CheckReturn(const CheckContext& context, uint32 node) noexcept;
    bool Terminates(uint32 module, uint32 node) const noexcept;
    uint32 FieldType(uint32 field) noexcept;
    ConstantValue ConstOf(uint32 symbol) noexcept;

    NameBinder& binder_;
    Compilation& compilation_;
    Allocator& allocator_;
    // 언어 타입 심볼(정규 이름)이다.
    uint32 boolType_ = NoType;
    uint32 intType_ = NoType;
    uint32 floatType_ = NoType;
    uint32 stringType_ = NoType;
    uint32 voidType_ = NoType;
    uint32 errorEnum_ = NoSymbol; // 엔진 `Error`
    // 필드·const 타입 추론의 진행 표시(심볼 번호 → 0 미정, 1 진행 중, 2 끝)다.
    Array<uint8> inference_;
    // const 심볼의 값이다(심볼 번호).
    Array<ConstantValue> constants_;
    // 모든 case를 다룬 enum switch((모듈 << 32) | 노드)다(끝나는 문장 판정).
    Array<uint64> exhaustiveSwitches_;
};

} // namespace rocks::sparks::detail
