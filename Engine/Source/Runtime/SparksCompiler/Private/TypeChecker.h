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
        Enum,
        Invalid // 값을 정하려다 진단이 난 상수(const 초기값 오류 등) — 더 알리지 않는다
    };

    Kind kind = Kind::None;
    bool negative = false; // Integer: 값은 negative ? -magnitude : magnitude
    uint64 magnitude = 0;
    double real = 0.0;
    bool boolean = false;
    uint32 symbol = NoSymbol; // Enum: case 심볼
    StringView text;          // String: 리터럴 원문(따옴표·`@` 포함 — 값은 DecodeStringLiteral)
};

// 기대 타입이 앞선 오류로 정해지지 않은 자리의 표지다(Check·Coerce의 expected·type 인자로만 쓴다).
// 기대 타입이 없는 자리(NoType)와 달리 `null`·`[]`·Map 리터럴·`Asset`·`Row`·`CVar(...)`에 S0417을
// 내지 않는다(3.10절 연쇄 방지). 타입 표에 없는 값이므로 타입 번호로 쓰지 않는다.
inline constexpr uint32 UnresolvedType = NumericLimits<uint32>::Max() - 2;

// 지역 번호 표(심볼 → 지역 번호)의 "없음" 표지다(struct 그래프·초기값 의존 그래프).
inline constexpr uint32 NoLocalIndex = NumericLimits<uint32>::Max();

// 필드·const 초기값이 다른 초기값을 따라가는 깊이 상한이다(S0427).
inline constexpr uint32 MaxInitializerDepth = 64;

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

    // 모듈의 모든 타입 표기에 타입 인자 제약과 `void` 자리를 검사한다(3.10절 "타입 인자 제약").
    void CheckTypeRules(uint32 module) noexcept;

    // Sparks class나 엔진 object 타입인지다(`Class<T>`의 T, `?` 없음).
    bool IsClassType(uint32 type) const noexcept;

    // 타입(또는 그 기반 체인)의 멤버 이름 공간이 불완전한지다(3.9절 — 기반을 해석하지 못한 class).
    // 그런 타입의 멤버를 찾지 못해도 S0326을 내지 않는다(연쇄 방지).
    bool HasIncompleteMembers(uint32 type) const noexcept;

    // 정해진 상수 접기(3.10절): 두 값이 모두 상수면 결과 값을 result.constant에 둔다. 범위·유한성을
    // 어기면 S0503·S0505를 내고 false. operandType은 피연산자의 타입(비교면 비교하는 타입)이다.
    bool FoldBinary(const CheckContext& context, uint32 node, StringView op,
                    const ExpressionValue& left, const ExpressionValue& right, uint32 operandType,
                    ExpressionValue& result) noexcept;

    // 엔진 object·Sparks class·interface처럼 파괴될 수 있는 객체 타입인지다(에셋 제외, 부록 B
    // `[객체]`).
    bool IsObjectType(uint32 type) const noexcept;

    // 찾지 못한 멤버 이름과 가장 비슷한 멤버(편집 거리 2 이하, 같으면 byte 순)다. 없으면 빈 문자열.
    StringView SimilarMember(Span<const uint64> spaces, uint32 receiverType,
                             StringView name) const noexcept;

    // 식 안의 타입 인자가 함수가 아닌 것에 붙었으면 S0320을 내고 true다(3.10절 "호출").
    bool RejectTypeArguments(const CheckContext& context, uint32 typeArguments,
                             StringView name) noexcept;

    // S0425: 타입 이름으로 인스턴스 멤버(valueType NoType), 값으로 enum case·static 함수를 썼다.
    // 재귀 경로(CheckMember)의 frame을 작게 두려고 문장 만들기를 따로 둔다.
    void ReportStaticInstance(const CheckContext& context, uint32 node, StringView name,
                              uint32 valueType) noexcept;

    // 나누는 쪽이 값 0인 정수 상수(타입 없는 상수·`const`)면 S0504를 내고 true다(3.10절).
    bool ReportZeroDivisor(const CheckContext& context, const ExpressionValue& divisor,
                           uint32 node) noexcept;

    // 진단 문장의 타입 표기다(정규 이름 대신 선언 이름 — `Mode`, `Array<Box>?`).
    void AppendDisplayType(StringBuilderBase& out, uint32 type) const noexcept;

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

    // struct 기본값 판정 상태다(structDefault_ 값).
    static constexpr uint8 StructDefaultUnknown = 0;
    static constexpr uint8 StructDefaultComputing = 1;
    static constexpr uint8 StructDefaultPresent = 2;
    static constexpr uint8 StructDefaultMissing = 3;

    // 값 struct 순환(S0426)을 찾고 struct 기본값을 정한다(CheckStructs.cpp).
    // cycles=false: 표기한 필드만으로 기본값을 정한다(검사 전). cycles=true: 추론한 필드까지 넣어
    // 순환을 알린다(검사 뒤).
    void AnalyzeStructs(bool cycles) noexcept;
    // 필드·const 초기값을 이름 의존 순서대로 미리 정한다(재귀 깊이를 쓰지 않게 — S0427 앞 단계).
    void PrepareInitializers() noexcept;
    // 초기값 깊이를 하나 들어간다. 상한을 넘으면 S0427을 내고 false(그 초기값은 해석하지 못함).
    bool EnterInitializer(uint32 module, uint32 initializer) noexcept;
    bool IsEngineStruct(uint32 symbol) const noexcept;
    bool EngineStructHasDefault(uint32 symbol) noexcept;
    bool SparksStructHasDefault(uint32 symbol) const noexcept;
    bool IsInterpolatable(uint32 type) const noexcept;
    // 제네릭 치환: 주인 타입 인자(receiver)·함수 타입 인자·Math 자리표시(chosen)를 바꾼다.
    uint32 Substitute(uint32 type, uint32 receiver, Span<const uint32> functionArguments,
                      uint32 chosen) noexcept;
    bool ContainsPlaceholder(uint32 type) const noexcept;
    bool PlaceholderAccepts(uint32 placeholder, uint32 type) const noexcept;

    // ---- 식(CheckExpressions.cpp) ----
    ExpressionValue Check(const CheckContext& context, uint32 node, uint32 expected) noexcept;
    // 잎(리터럴·this)과 괄호다. 아래의 `...Value`·`Judge...`는 재귀가 끝난 뒤의 판정이고, 같은
    // 이름의 wrapper가 판정 값의 타입을 기록한다(재귀 경로의 frame을 작게 — Check 참고).
    ExpressionValue CheckLeaf(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CheckParenthesized(const CheckContext& context, uint32 node,
                                       uint32 expected) noexcept;
    ExpressionValue CheckIdentifier(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue ValueOfSymbol(const CheckContext& context, uint32 symbol, uint32 receiver,
                                  bool instance) noexcept;
    ExpressionValue CheckBinary(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CheckLogical(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CombineLogical(const CheckContext& context, uint32 node,
                                   const ExpressionValue& left,
                                   const ExpressionValue& right) noexcept;
    ExpressionValue CombineLogicalValue(const CheckContext& context, uint32 node,
                                        const ExpressionValue& left,
                                        const ExpressionValue& right) noexcept;
    ExpressionValue CheckCoalesce(const CheckContext& context, uint32 node,
                                  uint32 expected) noexcept;
    ExpressionValue CheckCoalesceValue(const CheckContext& context, uint32 node,
                                       uint32 expected) noexcept;
    ExpressionValue CombineBinary(const CheckContext& context, uint32 node,
                                  const ExpressionValue& left,
                                  const ExpressionValue& right) noexcept;
    ExpressionValue CombineBinaryValue(const CheckContext& context, uint32 node,
                                       const ExpressionValue& left,
                                       const ExpressionValue& right) noexcept;
    ExpressionValue CheckUnary(const CheckContext& context, uint32 node, uint32 expected) noexcept;
    ExpressionValue CheckUnaryValue(const CheckContext& context, uint32 node,
                                    uint32 expected) noexcept;
    ExpressionValue CheckIndex(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue IndexValue(const CheckContext& context, uint32 node,
                               const ExpressionValue& object) noexcept;
    ExpressionValue JudgeIndex(const CheckContext& context, uint32 node,
                               const ExpressionValue& object) noexcept;
    ExpressionValue CheckCollection(const CheckContext& context, uint32 node,
                                    uint32 expected) noexcept;
    ExpressionValue CheckCollectionValue(const CheckContext& context, uint32 node,
                                         uint32 expected) noexcept;
    ExpressionValue CheckString(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CheckCast(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CastOf(const CheckContext& context, uint32 node,
                           const ExpressionValue& operand) noexcept;
    ExpressionValue CastOfValue(const CheckContext& context, uint32 node,
                                const ExpressionValue& operand) noexcept;
    ExpressionValue CheckAwaitTry(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CheckAwaitTryValue(const CheckContext& context, uint32 node) noexcept;
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
    void Report(const CheckContext& context, DiagnosticCode code, uint32 node, StringView message,
                StringView explanation, StringView fix) noexcept;
    void ReportAt(const CheckContext& context, DiagnosticCode code, SourceSpan span,
                  StringView message, StringView explanation, StringView fix) noexcept;
    void ReportMismatch(const CheckContext& context, uint32 node, uint32 from, uint32 to) noexcept;

    // ---- 멤버·호출(CheckCalls.cpp) ----
    ExpressionValue CheckMember(const CheckContext& context, uint32 node) noexcept;
    // 받는 값을 검사한 뒤의 멤버 판정이다(재귀 경로 밖 — CheckBinary 참고).
    ExpressionValue MemberOf(const CheckContext& context, uint32 node,
                             const ExpressionValue& receiver) noexcept;
    ExpressionValue MemberOfValue(const CheckContext& context, uint32 node,
                                  const ExpressionValue& receiver) noexcept;
    ExpressionValue CheckCall(const CheckContext& context, uint32 node, uint32 expected) noexcept;
    // 부르는 값을 검사한 뒤의 호출 판정이다(재귀 경로 밖 — CheckBinary 참고).
    ExpressionValue CallValue(const CheckContext& context, uint32 node, uint32 expected,
                              const ExpressionValue& callee) noexcept;
    ExpressionValue JudgeCall(const CheckContext& context, uint32 node, uint32 expected,
                              const ExpressionValue& callee) noexcept;
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
    // switch의 대상·case 값(대상 타입을 돌려준다, case 상수는 seen에)과 빠진 case·default 판정이다.
    uint32 CheckSwitchValues(const CheckContext& context, uint32 node,
                             Array<ConstantValue>& seen) noexcept;
    void CheckSwitchCoverage(const CheckContext& context, uint32 node, uint32 type,
                             Span<const ConstantValue> seen) noexcept;
    // 문장 재귀(CheckStatement)의 frame을 작게 두려고 나눈 머리·단순 문장 검사다.
    void CheckCondition(const CheckContext& context, uint32 expression) noexcept;
    void CheckIfHead(const CheckContext& context, uint32 node) noexcept;
    void CheckForHead(const CheckContext& context, uint32 node) noexcept;
    void CheckSimpleStatement(const CheckContext& context, uint32 node) noexcept;
    void CheckReturn(const CheckContext& context, uint32 node) noexcept;
    // `x op= v`(x의 읽기 타입 readType)를 `x = x op v`의 연산 규칙으로 검사한다(3.10절 "연산자").
    void CheckCompoundAssignment(const CheckContext& context, uint32 node, StringView op,
                                 uint32 readType, const ExpressionValue& value,
                                 uint32 valueNode) noexcept;
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
    // struct 기본값 판정(심볼 번호 → StructDefault*)이다. AnalyzeStructs가 채운다.
    Array<uint8> structDefault_;
    // enum case 목록(선언 순): enum 심볼 → 첫 case, case → 다음 case(NoSymbol 끝)이다.
    Array<uint32> firstCase_;
    Array<uint32> nextCase_;
    // 지금 정하고 있는 초기값의 깊이다(S0427).
    uint32 initializerDepth_ = 0;
};

} // namespace rocks::sparks::detail
