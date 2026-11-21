#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/StringView.h>
#include <Platform/Memory.h>
#include <SparksCompiler/Source.h>

// typed IR이다(Docs/Design/SparksBytecode.ko.md 2·3절, LC4a). 함수 하나는 기본 블록 배열이고,
// 명령은 bank가 정해진 가상 레지스터 위의 3-주소 형식이다(비SSA — 지역 이름 하나가 가상 레지스터
// 묶음 하나). Sparks 값 하나는 원시 slot 묶음(연속 가상 레지스터)으로 내린다. 호출·생성·발행·문자열
// 잇기의 인자 목록은 함수의 피연산자 표(operands)에 둔다.

namespace rocks::sparks::detail {

// 가상 레지스터가 속한 bank다.
enum class IrBank : uint8 {
    Scalar, // 태그 없는 64비트
    Ref,    // 객체 handle + generation
    Buf     // 참조계수 COW 버퍼
};

// Scalar slot의 해석이다(덤프·검사·bytecode 명령 선택에 쓴다).
enum class IrScalar : uint8 {
    None, // Ref·Buf
    Bool,
    I8,
    I16,
    I32,
    I64,
    U8,
    U16,
    U32,
    U64,
    F32,
    F64,
    Enum,    // case 번호
    Name,    // 사전 이름 id(Tag·Row·Class)
    Flag,    // `T?`의 있음 표지·Result의 오류 코드
    AssetLo, // Soft<T>의 AssetId 아래 64비트
    AssetHi  // 위 64비트
};

inline constexpr uint32 NoIrRegister = NumericLimits<uint32>::Max();
inline constexpr uint32 NoIrBlock = NumericLimits<uint32>::Max();

// 가상 레지스터 하나다.
struct IrRegister {
    IrBank bank = IrBank::Scalar;
    IrScalar scalar = IrScalar::None;
    uint32 type = 0;  // 이 레지스터가 속한 Sparks 값의 타입(묶음 전체의 타입)
    uint16 slot = 0;  // 묶음 안 번호
    uint16 slots = 1; // 묶음 크기
    StringView name;  // 지역 이름(임시 값은 빈 글자)
};

// 명령 종류다. 이름은 덤프에 그대로 쓴다(IrOpName). "목록"은 operands[b, b + c)이다.
enum class IrOp : uint8 {
    Move,         // a := b (slot 하나)
    LoadInt,      // a := 정수 상수(b 아래 32비트, extra 위 32비트)
    LoadFloat,    // a := 실수 상수(double 비트 — b 아래 32비트, extra 위 32비트)
    LoadString,   // a := 문자열 상수(strings[b]). sub IrStringPiece면 escape를 풀지 않은 원문 조각
    LoadNull,     // a := null(Ref·Buf)
    Unary,        // a := op(b) — sub = IrArith
    Binary,       // a := b op c — sub = IrArith
    Compare,      // a := 묶음 b cmp 묶음 c(slot 수 extra) — sub = IrCompare
    Convert,      // a := conv(b) — 목적 해석은 a. sub IrConvertCast면 `as?`(대상 타입 심볼 extra)
    Jump,         // goto block a
    Branch,       // if b then block a else block c
    Switch,       // b의 값으로 분기(표 cases[a, a + c)), 어느 것도 아니면 block extra
    Return,       // 반환(값 묶음 b — 없으면 NoIrRegister)
    IsSome,       // a := b가 null·없음이 아님
    GetField,     // a := 필드 extra(수신자 b — NoIrRegister면 문맥 이름)
    SetField,     // 필드 extra(수신자 a) := b
    GetElement,   // a := b[c] (컨테이너 원소·Map 값)
    SetElement,   // a[b] := c
    Length,       // a := 컨테이너 b의 원소 수
    GetEntry,     // a := 컨테이너 b의 c번째 항목(sub IrEntryKey면 key, IrEntryValue면 값)
    Call,         // a := 함수 extra(목록 — 수신자가 있으면 첫 인자). sub IrCallRpc면 RPC 송신
    Emit,         // 이벤트 extra를 대상 a(this면 NoIrRegister)에 발행(목록이 인자)
    Await,        // 정지: 대기 b(sub IrAwaitEvent면 발행자 b·이벤트 extra), 재개하면 a := 결과,
                  // block c로
    Try,          // Result b가 성공이면 a := 값, block extra로. 실패면 block c로(끝 명령)
    New,          // a := 생성 extra(목록 — sub로 해석: IrNewStruct는 (필드, 값) 쌍, IrNewLiteral은
                  // (이름 글자 번호, 값) 쌍, IrNewCollection은 원소·(key, 값), IrNewResult*는 값)
    Concat,       // a := 문자열 조각 목록을 잇는다
    ReturnError,  // Result b의 오류를 이 함수의 Result로 반환(끝 명령)
    EndExecution, // Result가 아닌 함수·handler의 `try` 실패: 현재 실행을 끝내고 b의 오류를 기록(끝
                  // 명령)
    Release,      // 지역 유지 자원 a를 해제
    Fault         // 실행 중 fault(종류 extra) — 도달하면 안 되는 경로
};

// 명령의 sub 값이다(op마다 뜻이 다르다).
inline constexpr uint8 IrCallDirect = 0;
inline constexpr uint8 IrCallRpc = 1;
inline constexpr uint8 IrCallMutating = 2; // 수신자 묶음(첫 인자)을 호출이 바꾼다(in/out)
inline constexpr uint8 IrNewStruct = 0;
inline constexpr uint8 IrNewLiteral = 1;
inline constexpr uint8 IrNewCollection = 2;
inline constexpr uint8 IrNewResultOk = 3;
inline constexpr uint8 IrNewResultError = 4;
inline constexpr uint8 IrConvertValue = 0;
inline constexpr uint8 IrConvertCast = 1;
inline constexpr uint8 IrAwaitValue = 0;
inline constexpr uint8 IrAwaitEvent = 1;
inline constexpr uint8 IrStringConstant = 0;
inline constexpr uint8 IrStringPiece = 1;
inline constexpr uint8 IrEntryKey = 0;
inline constexpr uint8 IrEntryValue = 1;

// Fault 종류다.
inline constexpr uint32 IrFaultUnreachable = 1;
inline constexpr uint32 IrFaultTryInHook = 2; // Awake·Enable·Start 안의 `try` 실패(SD43)
// 경로 쓰기로 읽은 원소가 없다(IndexOutOfRange — SL §4 "복합 대입과 경로 쓰기는 키가 없으면
// fault").
inline constexpr uint32 IrFaultMissingElement = 3;

// 단항·이항 산술 종류다(정수 overflow·실수 유한성 검사는 명령의 scalar 해석으로 정한다).
enum class IrArith : uint8 {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Neg,
    Not,
    Compose // quat·mat4 곱(묶음 전체 — 성분별이 아니다)
};

enum class IrCompare : uint8 {
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual
};

// 명령 하나다. 피연산자의 뜻은 IrOp 설명을 따른다.
struct IrInstruction {
    IrOp op = IrOp::Move;
    uint8 sub = 0;
    uint32 a = NoIrRegister;
    uint32 b = NoIrRegister;
    uint32 c = NoIrRegister;
    uint32 extra = 0;
    // Call의 명시한 타입 인자 목록(operands[types]가 개수, 뒤로 타입 번호), 없으면 NoIrRegister.
    uint32 types = NoIrRegister;
    SourceSpan span; // 낸 식·문장의 원문 범위
};

// 기본 블록이다: 명령 [first, first + count). 마지막 명령이 끝 명령(Jump·Branch·Switch·Return·
// Await·Try 아님·Fault).
struct IrBlock {
    uint32 first = 0;
    uint32 count = 0;
};

// switch 표의 한 줄이다(값 → 블록).
struct IrCase {
    int64 value = 0;
    uint32 block = NoIrBlock;
};

// 함수·handler 하나의 IR이다.
struct IrFunction {
    explicit IrFunction(Allocator& allocator) noexcept
        : registers(allocator), instructions(allocator), blocks(allocator), cases(allocator),
          parameters(allocator), operands(allocator), strings(allocator) {
    }

    uint32 module = 0;
    uint32 node = 0;            // Function·Handler 노드
    uint32 symbol = 0;          // 함수 심볼(handler는 NoSymbol)
    uint32 owner = 0;           // 담은 class·struct 심볼(최상위 함수는 NoSymbol)
    uint32 self = NoIrRegister; // this 묶음(class 본문)
    Array<IrRegister> registers;
    Array<IrInstruction> instructions;
    Array<IrBlock> blocks;
    Array<IrCase> cases;
    // 매개변수 묶음의 첫 레지스터(this 제외, 선언 순)다.
    Array<uint32> parameters;
    // 호출·생성·발행·잇기의 인자 목록(레지스터 번호, New의 쌍은 심볼·글자 번호도 담는다)이다.
    Array<uint32> operands;
    // 문자열 상수(원문을 빌린다 — 컴파일이 살아 있는 동안 유효)다.
    Array<StringView> strings;
};

const char* IrOpName(IrOp op) noexcept;

// 실수 상수의 64비트 표현이다.
inline uint64 FloatBits(double value) noexcept {
    return platform::BitCast<uint64>(value);
}

} // namespace rocks::sparks::detail
