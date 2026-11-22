#pragma once

#include <Base/Types.h>

// Sparks bytecode의 opcode와 명령 단어다(Docs/Design/SparksBytecode.ko.md 4.3절, LC4b). 명령은
// 32비트 고정이고 모양은 셋이다: ABC(`op:8 | A:8 | B:8 | C:8`), ABx(`op:8 | A:8 | Bx:16`),
// AsBx(`op:8 | A:8 | sBx:16` — 부호 있는 분기 거리, 다음 명령 기준). 피연산자가 더 필요한 명령은
// 바로 뒤에 EXTRA 단어
// (`Extra:8 | Ax:24`) 하나를 붙인다. opcode 번호는 bytecode 형식 버전(BytecodeFormatVersion)에
// 묶인다 — 번호를 바꾸면 버전을 올린다.
//
// 사용 예:
//   const uint32 word = EncodeABC(Opcode::AddI64, 4, 0, 3);
//   const OpcodeInfo& info = InfoOf(DecodeOpcode(word)); // info.name == "ADD.I64"

namespace rocks::sparks {

// `.sparkc`·bytecode 형식 버전이다(opcode 번호·명령 모양·표 배치가 바뀌면 올린다).
inline constexpr uint32 BytecodeFormatVersion = 3; // 2: TYPE 구역(LC5b3), 3: CLASS 구역(LC6a)

// bank 하나의 레지스터 수 상한이다(8비트 피연산자 — D3).
inline constexpr uint32 MaxBankRegisters = 256;

// 함수 하나의 명령 단어 수 상한이다(16비트 분기 거리 — D3).
inline constexpr uint32 MaxFunctionWords = 32767;

// ABx의 Bx·EXTRA의 Ax 상한이다.
inline constexpr uint32 MaxBx = 0xFFFF;
inline constexpr uint32 MaxAx = 0xFFFFFF;

// LOADI가 상수 표 없이 싣는 정수 범위다.
inline constexpr int32 MinImmediate = -32768;
inline constexpr int32 MaxImmediate = 32767;

enum class Opcode : uint8 {
    // 이동·상수
    MovS,
    MovR,
    MovB,
    LoadI,
    LoadK,
    LoadStr,
    LoadNullR,
    LoadNullB,
    // 정수 산술(타입별 — 범위 밖은 IntegerOverflow, 0 나눗셈은 DivideByZero, DIV·MOD는 내림 방식)
    AddI8,
    AddI16,
    AddI32,
    AddI64,
    AddU8,
    AddU16,
    AddU32,
    AddU64,
    SubI8,
    SubI16,
    SubI32,
    SubI64,
    SubU8,
    SubU16,
    SubU32,
    SubU64,
    MulI8,
    MulI16,
    MulI32,
    MulI64,
    MulU8,
    MulU16,
    MulU32,
    MulU64,
    DivI8,
    DivI16,
    DivI32,
    DivI64,
    DivU8,
    DivU16,
    DivU32,
    DivU64,
    ModI8,
    ModI16,
    ModI32,
    ModI64,
    ModU8,
    ModU16,
    ModU32,
    ModU64,
    NegI8,
    NegI16,
    NegI32,
    NegI64,
    // 실수 산술(결과가 유한하지 않으면 NonFinite)
    AddF32,
    SubF32,
    MulF32,
    DivF32,
    NegF32,
    AddF64,
    SubF64,
    MulF64,
    DivF64,
    NegF64,
    // 수학 타입 합성(UE와 같은 순서 — quat은 FQuat::operator*, mat4는 FMatrix::operator*). A·B·C는
    // 묶음의 첫 레지스터(quat 4·mat4 16 slot)
    MulQ,
    MulM,
    // 논리·비교
    Not,
    EqI,
    NeI,
    LtI,
    LeI,
    LtU,
    LeU,
    EqF,
    NeF,
    LtF,
    LeF,
    EqStr,
    NeStr,
    LtStr, // UTF-8 byte 순
    LeStr,
    EqR,
    NeR,
    EqB,
    NeB,
    EqN,  // + EXTRA(slot 수)
    NeN,  // + EXTRA(slot 수)
    EqNF, // 실수 묶음(성분 F32·F64와 있음 표지) — 성분은 실수 비교(-0.0 == 0.0, NaN != NaN)
    NeNF,
    // 변환
    Conv,  // C = (원래 종류 << 4) | 목적 종류(ScalarKind)
    ToStr, // + EXTRA(표시 종류 << 16 | enum 타입 참조)
    Cast,  // + EXTRA(타입 참조)
    // 제어
    Jmp,
    Brt,
    Brf,
    Switch,
    Ret,
    RetErrS,  // Scalar Result A..A+B의 오류를 이 함수의 Result로 반환
    RetErrB,  // Buf Result A의 오류를 이 함수의 Result로 반환
    EndExecS, // Result가 아닌 함수·handler의 `try` 실패: 현재 실행을 끝내고 Scalar Result A..A+B의
              // 오류를 기록
    EndExecB, // 같은 것의 Buf Result A
    // optional
    IsSomeR,
    IsSomeB,
    // 필드(+ EXTRA 필드 참조)
    GetFS,
    GetFR,
    GetFB,
    SetFS,
    SetFR,
    SetFB,
    GetSFS,
    GetSFR,
    GetSFB,
    SetSFS,
    SetSFR,
    SetSFB,
    GetCtx,
    // 컨테이너(+ EXTRA 컨테이너 표지)
    GetE,
    SetE,
    Len,
    GetKey,
    GetVal,
    // 호출·이벤트(창 base는 함수 표의 bank별 할당 수)
    Call,
    CallV,
    CallN,
    CallL,
    CallRpc,
    Emit,
    // 대기·Result
    Await,   // + EXTRA(결과 bank << 8 | slot 수)
    AwaitEv, // + EXTRA(이벤트 참조)
    TryS,    // A ← Result B..B+C의 값, 오류면 EXTRA(부호 있는 거리)로 분기
    TryB,    // A ← Buf Result B의 값(C = 값 bank << 5 | slot 수), 오류면 EXTRA 거리로 분기
    // 생성
    NewS,
    NewC,
    NewA,
    NewSet,
    NewM,
    Append, // + EXTRA(컨테이너 표지)
    Put,    // + EXTRA(컨테이너 표지)
    NewLit, // + EXTRA(리터럴 참조)
    Ok,     // + EXTRA(값 bank << 8 | slot 수)
    Err,
    // 문자열·자원·fault
    Concat,
    Release,
    Fault,
    Extra,
    Count
};

// 명령의 피연산자 모양이다(덤프·verifier가 쓴다).
enum class OperandForm : uint8 {
    None, // 피연산자 없음
    A,    // A
    AB,   // A, B
    ABC,  // A, B, C
    ABx,  // A, Bx
    AsBx, // A, sBx(분기)
    SBx,  // sBx(분기 — JMP)
    AImm, // A, sBx(부호 있는 즉시값 — LOADI)
    Bx,   // Bx(표 번호 — 호출·발행)
    Ax    // Ax(24비트 — FAULT·EXTRA)
};

// 명령이 쓰고 읽는 레지스터의 bank다(verifier·덤프의 레지스터 글자).
enum class OperandBank : uint8 {
    None,
    Scalar,
    Ref,
    Buf
};

struct OpcodeInfo {
    Opcode op;
    const char* name;
    OperandForm form;
    bool extra;    // 뒤에 EXTRA 단어가 붙는다
    OperandBank a; // A 레지스터의 bank(레지스터가 아니면 None)
    OperandBank b; // B 레지스터의 bank
    OperandBank c; // C 레지스터의 bank
};

// opcode의 정보다. op는 Opcode::Count 앞이어야 한다(범위 밖은 호출자 버그 — Fatal).
const OpcodeInfo& InfoOf(Opcode op) noexcept;

// 명령 단어 만들기·읽기다.
constexpr uint32 EncodeABC(Opcode op, uint32 a, uint32 b, uint32 c) noexcept {
    return uint32(op) | ((a & 0xFF) << 8) | ((b & 0xFF) << 16) | ((c & 0xFF) << 24);
}

constexpr uint32 EncodeABx(Opcode op, uint32 a, uint32 bx) noexcept {
    return uint32(op) | ((a & 0xFF) << 8) | ((bx & MaxBx) << 16);
}

constexpr uint32 EncodeAsBx(Opcode op, uint32 a, int32 sbx) noexcept {
    return uint32(op) | ((a & 0xFF) << 8) | ((uint32(sbx) & MaxBx) << 16);
}

constexpr uint32 EncodeAx(Opcode op, uint32 ax) noexcept {
    return uint32(op) | ((ax & MaxAx) << 8);
}

constexpr Opcode DecodeOpcode(uint32 word) noexcept {
    return static_cast<Opcode>(word & 0xFF);
}

constexpr uint32 DecodeA(uint32 word) noexcept {
    return (word >> 8) & 0xFF;
}

constexpr uint32 DecodeB(uint32 word) noexcept {
    return (word >> 16) & 0xFF;
}

constexpr uint32 DecodeC(uint32 word) noexcept {
    return (word >> 24) & 0xFF;
}

constexpr uint32 DecodeBx(uint32 word) noexcept {
    return (word >> 16) & MaxBx;
}

constexpr int32 DecodeSBx(uint32 word) noexcept {
    return static_cast<int32>(static_cast<int16>(static_cast<uint16>(word >> 16)));
}

constexpr uint32 DecodeAx(uint32 word) noexcept {
    return (word >> 8) & MaxAx;
}

// EXTRA의 Ax를 부호 있는 24비트 거리로 읽는다(TRY의 실패 분기).
constexpr int32 DecodeSAx(uint32 word) noexcept {
    const uint32 ax = DecodeAx(word);
    return (ax & 0x800000) != 0 ? static_cast<int32>(ax | 0xFF000000u) : static_cast<int32>(ax);
}

// EXTRA 컨테이너 표지(GETE·SETE·GETKEY·GETVAL·APPEND·PUT)다: 값 bank·slot 수와 key bank·slot 수.
constexpr uint32 EncodeContainerMarker(uint32 valueBank, uint32 valueSlots, uint32 keyBank,
                                       uint32 keySlots) noexcept {
    return (valueSlots & 0x1F) | ((valueBank & 0x3) << 5) | ((keySlots & 0x1F) << 8) |
           ((keyBank & 0x3) << 13);
}

// TOSTR의 표시 종류다: 0–15는 ScalarKind, 그 위는 수학 타입(성분 double 묶음).
enum class DisplayKind : uint8 {
    Vec2 = 16,
    Vec3,
    Vec4,
    Quat,
    Rotator,
    Color,
    Transform,
    Mat4
};

// Scalar 값의 해석이다(CONV의 C·TOSTR 표시 종류·덤프). 번호는 형식에 묶인다.
enum class ScalarKind : uint8 {
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
    Enum,
    Name,
    Flag,
    AssetLo,
    AssetHi
};

const char* ScalarKindName(ScalarKind kind) noexcept;

} // namespace rocks::sparks
