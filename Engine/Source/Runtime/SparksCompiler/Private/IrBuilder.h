#pragma once

#include "Ir.h"
#include "TypeChecker.h"

// 검사한 구문 트리 → typed IR 내림이다(Docs/Design/SparksBytecode.ko.md 3절, LC4a). 함수 본문
// 하나를 IrFunction 하나로 내린다. 오류 없는 컴파일에서만 돈다(타입·심볼이 모두 정해져 있다).

namespace rocks::sparks::detail {

// Sparks 값 하나의 slot 배치다(2절 표).
// 값 하나의 slot 수 상한이다: 가장 큰 값은 `Result<mat4>?`(mat4 16 + 오류 코드 1 + 있음 표지 1)다
// (`Result<T?>`·`T??`는 타입 오류다).
inline constexpr uint32 MaxIrSlots = 18;

struct IrLayout {
    IrBank bank = IrBank::Scalar;
    uint16 slots = 0; // 0이면 값이 없다(void)
    IrScalar scalars[MaxIrSlots] = {};
};

class IrBuilder final {
public:
    // function.module·node·symbol·owner를 채운 함수를 받는다.
    IrBuilder(TypeChecker& checker, IrFunction& function) noexcept;

    // function.node의 본문을 내린다. 할당 실패면 false(오류는 binder에).
    bool Build() noexcept;

    // 타입의 slot 배치다.
    static IrLayout Layout(TypeChecker& checker, uint32 type) noexcept;

    // 내린 함수의 구조 불변식을 본다(끝 명령·분기 대상·레지스터 범위·묶음 크기). false는 내림의
    // 결함이다.
    bool Validate() const noexcept;

private:
    bool Failed() const noexcept;
    void Fail(ErrorCode error) noexcept;

    // ---- 블록·명령·값(IrBuilder.cpp) ----
    uint32 NewBlock() noexcept;
    void StartBlock(uint32 block) noexcept;
    // 현재 블록이 끝 명령으로 닫혔는지다(닫혔으면 이어지는 명령은 도달하지 않는 새 블록에 둔다).
    bool Terminated() const noexcept;
    void Add(const IrInstruction& instruction) noexcept;
    void Add(IrOp op, uint32 a, uint32 b, uint32 c, uint32 extra, uint32 node,
             uint8 sub = 0) noexcept;
    // 타입 하나의 값 묶음을 새 가상 레지스터에 잡고 첫 번호를 돌려준다(void면 NoIrRegister).
    uint32 NewValue(uint32 type, StringView name = StringView()) noexcept;
    // 대기 handle(Ref, 타입 NoType — 덤프는 `wait`)을 잡는다.
    uint32 NewWait() noexcept;
    uint32 SlotCount(uint32 first) const noexcept;
    // 묶음 to ← from(slot마다 Move).
    void MoveValue(uint32 to, uint32 from, uint32 node) noexcept;
    // 묶음을 없음(Scalar는 모두 0, Ref·Buf는 null)으로 둔다.
    void LoadNone(uint32 value, uint32 node) noexcept;
    // struct 생성에서 빠진 필드의 값이다(SparksBytecode 5절 TYPE 구역): 초기값(그 필드의 모듈 구문
    // 트리로 내리고 원문 위치는 생성 자리 node)이나 0 값. 0 slot·null로 충분하면 NoIrRegister.
    uint32 LowerFieldDefault(uint32 field, uint32 node) noexcept;
    // 타입의 0 값(SL §3.2)이다: 문자열 `""`, 빈 컨테이너, struct 기본값, quat·mat4·Transform
    // 단위값. 0 slot·null로 충분한 타입(숫자·bool·enum·`T?`·그 밖의 수학 타입)은 NoIrRegister.
    uint32 LowerZero(uint32 type, uint32 node) noexcept;
    // 지역 이름(인자·지역·binding)의 첫 레지스터다.
    uint32 LocalRegister(uint32 symbol) const noexcept;
    void BindLocal(uint32 symbol, uint32 first) noexcept;
    uint32 AddOperands(Span<const uint32> values) noexcept;
    uint32 AddString(StringView text) noexcept;

    // ---- 상수·식(IrLower.cpp) ----
    // 정해진 숫자·bool 상수식(리터럴, 숫자 const, 단항, + - * / %)의 값이다. 아니면 false.
    bool EvaluateConstant(uint32 node, ConstantValue& value) noexcept;
    bool EvaluateBinaryConstant(uint32 node, ConstantValue& value) noexcept;
    // 접힌 값이 없는 const(수학 값·Tag·언어 함수 결과)는 초기값 식을 쓴 자리에서 내린다.
    uint32 LowerConstInitializer(uint32 constSymbol, uint32 useNode) noexcept;
    uint32 LowerConst(uint32 symbol, uint32 type, uint32 node) noexcept;
    uint32 LoadConstant(const ConstantValue& value, uint32 type, uint32 node) noexcept;
    uint32 LoadConstantOrString(const ConstantValue& value, uint32 type, uint32 node) noexcept;
    uint64 EnumIndex(uint32 caseSymbol) const noexcept;
    // 식의 기록된 타입, 없으면(타입 없는 상수) expected다.
    uint32 TypeOf(uint32 node, uint32 expected) const noexcept;
    // 식 하나를 내리고 값 묶음의 첫 레지스터를 돌려준다(void면 NoIrRegister). expected는 자리의
    // 기대 타입(타입 없는 상수·null·빈 컬렉션의 타입)이다.
    uint32 Lower(uint32 node, uint32 expected) noexcept;
    // 값을 자리의 타입으로 맞춘다(T → T?, 파생 → 기반).
    uint32 Coerce(uint32 value, uint32 to, uint32 node) noexcept;
    // T?의 값 묶음에서 T를 꺼낸다.
    uint32 Unwrap(uint32 optional, uint32 type, uint32 node) noexcept;
    uint32 LowerIdentifier(uint32 node) noexcept;
    uint32 LowerBinary(uint32 node, uint32 expected) noexcept;
    uint32 LowerBinaryStep(uint32 node, uint32 left, uint32 expected) noexcept;
    uint32 LowerLogical(uint32 node) noexcept;
    uint32 LowerCoalesce(uint32 node, uint32 expected) noexcept;
    uint32 LowerUnary(uint32 node, uint32 expected) noexcept;
    // 조건 식을 분기로 내린다(참 → onTrue, 거짓 → onFalse).
    void LowerCondition(uint32 node, uint32 onTrue, uint32 onFalse) noexcept;

    // ---- 멤버·호출·생성(IrCalls.cpp) ----
    bool MathFieldSlots(uint32 type, StringView field, uint32& offset,
                        uint32& count) const noexcept;
    uint32 LowerMember(uint32 node) noexcept;
    uint32 ReadMember(uint32 object, uint32 objectType, uint32 member, uint32 type,
                      uint32 node) noexcept;
    uint32 LowerCall(uint32 node, uint32 expected) noexcept;
    // 함수 callee를 수신자(없으면 NoIrRegister)와 인자로 부른다. 결과는 type의 값 또는 대기 handle.
    uint32 EmitCall(uint32 node, uint32 callee, uint32 receiver, Span<const uint32> arguments,
                    uint32 type, uint8 sub) noexcept;
    uint32 LowerConstruct(uint32 node, uint32 typeSymbol, Span<const uint32> arguments,
                          uint32 type) noexcept;
    uint32 LowerLiteralCall(uint32 node, uint32 literal, Span<const uint32> arguments,
                            uint32 type) noexcept;
    uint32 LowerIndex(uint32 node) noexcept;
    uint32 LowerString(uint32 node) noexcept;
    uint32 LowerCollection(uint32 node, uint32 expected) noexcept;
    uint32 LowerCast(uint32 node) noexcept;
    uint32 LowerAwait(uint32 node, uint32 operandNode) noexcept;
    // `await a?.F()`: a가 있을 때만 부르고 기다린다(결과 `R?`).
    uint32 LowerChainedAwait(uint32 node, uint32 call) noexcept;
    // 대기 피연산자가 재개 때 돌려주는 값의 타입이다.
    uint32 AwaitResultType(uint32 operandNode) noexcept;
    // 본문이 `on Awake`·`on Enable`·`on Start` hook인지다(`try` 실패가 fault인 자리).
    bool InStartupHook() const noexcept;
    uint32 LowerTry(uint32 node) noexcept;

    // ---- 문장(IrStatements.cpp) ----
    void LowerStatement(uint32 node) noexcept;
    void LowerBlock(uint32 node) noexcept;
    void LowerLocal(uint32 node) noexcept;
    void LowerAssignment(uint32 node) noexcept;
    // 값 경로의 한 단계다(대입 대상·값을 바꾸는 메서드의 수신자). 객체·key는 한 번만 평가한다.
    struct PathStep {
        uint8 kind = 0;       // PathStepKind
        bool chained = false; // object가 앞 단계의 값이다(다시 쓰기가 앞 단계로 이어진다)
        uint32 node = NoNode;
        uint32 symbol = NoSymbol; // 지역·필드 심볼
        uint32 object = NoIrRegister;
        uint32 key = NoIrRegister;
        uint32 offset = 0; // 수학 타입 필드의 slot 구간
        uint32 count = 0;
    };

    // 경로 node를 왼쪽부터 평가해 단계를 path에 더하고 그 자리의 현재 값을 돌려준다(readLeaf가
    // false면 마지막 단계의 값은 읽지 않는다 — 단순 대입).
    uint32 EvaluatePath(uint32 node, Array<PathStep>& path, bool readLeaf) noexcept;
    // 경로의 끝에 value를 쓰고, 값 타입을 지난 단계는 바깥 값에 차례로 다시 쓴다.
    void StorePath(Span<const PathStep> path, uint32 value) noexcept;
    void LowerIf(uint32 node) noexcept;
    void LowerWhile(uint32 node) noexcept;
    void LowerFor(uint32 node) noexcept;
    void LowerSwitch(uint32 node) noexcept;
    // `return [값]`. node가 NoNode면 본문 끝의 암묵 반환이다.
    void LowerReturn(uint32 valueNode, uint32 node) noexcept;
    void LowerEmit(uint32 node) noexcept;
    void LowerJump(bool isBreak, uint32 node) noexcept;
    // 유지 자원 지역 중 held_[mark..)를 선언 역순으로 해제한다(블록·반복을 떠나는 경로).
    void ReleaseHeld(usize mark, uint32 node) noexcept;

    struct Loop {
        uint32 breakBlock = NoIrBlock;
        uint32 continueBlock = NoIrBlock;
        usize heldMark = 0; // 반복 본문 바깥의 유지 자원 수
    };

    TypeChecker& checker_;
    Compilation& c_;
    IrFunction& function_;
    // 내리는 식의 모듈·구문 트리다(다른 모듈 const의 초기값을 그 자리에서 내릴 때 잠시 바뀐다).
    const SyntaxTree* tree_;
    uint32 module_;
    // 0이 아니면 명령의 원문 위치를 이 범위로 둔다(다른 모듈 const를 쓴 자리의 위치).
    SourceSpan spanOverride_;
    bool overrideSpan_ = false;
    uint32 current_ = NoIrBlock;
    uint32 this_ = NoIrRegister;
    uint32 returnType_ = 0;
    // 지역 이름: (심볼 << 32) | 첫 레지스터. 블록을 떠나면 그 블록의 것을 뗀다(뒤에서부터 찾는다).
    Array<uint64> locals_;
    // 살아 있는 지역 유지 자원의 첫 레지스터(선언 순)다.
    Array<uint32> held_;
    Array<Loop> loops_;
};

} // namespace rocks::sparks::detail
