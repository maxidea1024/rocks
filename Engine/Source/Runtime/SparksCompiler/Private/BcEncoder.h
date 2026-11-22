#pragma once

#include <Core/Containers/HashMap.h>
#include <SparksVm/Bytecode.h>

#include "Ir.h"
#include "TypeChecker.h"

// typed IR → bytecode다(Docs/Design/SparksBytecode.ko.md 4절, LC4b). 함수마다 가상 레지스터의 생존
// 구간을 구하고(블록 단위 생존 분석) bank별 선형 스캔으로 실제 레지스터에 놓은 뒤, 블록을 배치해
// 32비트 명령으로 쓴다. 상수·문자열·참조 표는 모듈 하나에 하나다(중복 제거, 처음 나온 순).

namespace rocks::sparks::detail {

// bytecode 모듈 머리의 compiler version이다(형식·코드 생성이 바뀌면 올린다).
inline constexpr StringView SparksCompilerVersion = "sparks 0.4.0";

class BytecodeEncoder final {
public:
    BytecodeEncoder(TypeChecker& checker, uint32 module, BytecodeModule& out) noexcept;

    // 같은 모듈 함수 심볼을 함수 표 번호에 묶는다(그 함수를 부르는 CALL의 참조).
    void Declare(uint32 symbol, uint32 index) noexcept;

    // 함수 하나를 out.functions[index]로 쓴다. name은 표시 이름, nameNode는 S1150의 위치다. 함수가
    // bank 레지스터 256개·명령 32,767개를 넘으면 S1150을 내고 false(할당 실패도 false — 오류는
    // binder에).
    bool Encode(const IrFunction& function, uint32 index, StringView name,
                uint32 nameNode) noexcept;

    // CLASS 구역(형식 버전 3 — SparksRuntime 2.1절)을 쓴다. bodies는 함수 표 순의 (owner << 32) |
    // 본문 노드다(handler의 함수 번호). 할당 실패면 false(오류는 binder에).
    bool EncodeClasses(Span<const uint64> bodies) noexcept;

    // 문자열 표에 넣고 번호를 돌려준다(모듈 머리의 이름).
    uint32 String(StringView text) noexcept {
        return AddString(text);
    }

private:
    // ---- 모듈 표 ----
    uint32 AddScalar(uint64 bits) noexcept;
    uint32 AddString(StringView text) noexcept;
    // 리터럴 원문(따옴표 포함) 또는 보간 조각(piece)의 escape를 풀어 문자열 표에 넣는다.
    uint32 AddDecodedString(StringView text, bool piece) noexcept;
    // 참조를 더한다(같은 것은 한 번). arguments가 있으면(언어 함수 서술자) 그것이 같은 참조의
    // 열쇠다.
    uint32 AddRef(BytecodeRefKind kind, uint32 name, uint32 detail,
                  uint32 arguments = NoBytecodeIndex) noexcept;
    // CLASS 필드의 타입 표기(형식 v5 — SparksReload K4)를 out에 붙인다.
    void AppendFieldType(StringBuilderBase& out, uint32 type) noexcept;
    // CLASS 필드의 형태 표기(형식 v6 — SparksReload 5절)를 out에 붙인다. open은 이 형태에서 이미
    // 펼친 struct·enum 심볼(두 번째부터 이름만).
    void AppendFieldShape(StringBuilderBase& out, uint32 type, Array<uint32>& open) noexcept;
    // 언어 함수 호출의 피연산자 서술자(SparksVm 6a절)를 out에 붙인다.
    void AppendDescriptor(StringBuilderBase& out, uint32 type) noexcept;
    // native 함수·객체 필드 참조의 서술자 문자열 번호다(SparksVm 6.1절).
    uint32 HostDescriptor(BytecodeRefKind kind, uint32 symbol) noexcept;
    // 다른 모듈의 Sparks 선언이면 name에 `<import 경로>:`를 붙이고 true.
    bool Qualify(StringBuilderBase& name, uint32 symbol) noexcept;
    // types는 호출의 타입 인자 목록(IrInstruction::types), 없으면 NoIrRegister.
    uint32 SymbolRef(BytecodeRefKind kind, uint32 symbol, uint32 types = NoIrRegister,
                     uint32 arguments = NoBytecodeIndex) noexcept;
    uint32 TypeRef(uint32 type) noexcept;
    // Sparks struct·enum 심볼의 TYPE 표 번호다(처음이면 배치를 더한다 — SparksBytecode 5절). 그
    // 밖은 NoBytecodeIndex.
    uint32 TypeTable(uint32 symbol) noexcept;
    uint32 CallRef(uint32 callee, uint8 sub, uint32 types, Opcode& op) noexcept;
    // 16비트 표 번호를 검사한다(넘으면 S1150).
    uint32 Bx(uint32 index) noexcept;

    // ---- 할당 ----
    bool Allocate() noexcept;
    uint32 TupleFirst(uint32 vreg) const noexcept;
    uint32 Phys(uint32 vreg) const noexcept;
    uint32 BankOf(uint32 vreg) const noexcept;

    // ---- 인코딩 ----
    bool Fail(ErrorCode error) noexcept;
    void TooLarge() noexcept;
    void Word(uint32 word, SourceSpan span) noexcept;
    void Branch(Opcode op, uint32 a, uint32 block, SourceSpan span) noexcept;
    void JumpTo(uint32 block, uint32 layoutIndex, SourceSpan span) noexcept;
    void Move(uint32 bank, uint32 to, uint32 from, SourceSpan span) noexcept;
    // 인자 묶음들을 bank별 호출 창에 옮긴다. firstSlot[bank]는 창에서 그 bank가 시작하는 자리다.
    void ToWindow(Span<const uint32> values, SourceSpan span) noexcept;
    void FromWindow(uint32 value, uint32 offset, SourceSpan span) noexcept;
    void Instruction(const IrInstruction& i, uint32 layoutIndex) noexcept;
    void EncodeCall(const IrInstruction& i) noexcept;
    void EncodeNew(const IrInstruction& i) noexcept;
    void EncodeCompare(const IrInstruction& i) noexcept;
    void EncodeConvert(const IrInstruction& i) noexcept;
    void RecordAwait(uint32 address, uint32 position, uint32 result) noexcept;
    uint32 ContainerMarker(uint32 value, uint32 key) const noexcept;

    struct Fixup {
        uint32 word = 0;    // 고칠 단어(함수 안 주소)
        uint32 block = 0;   // 분기 대상 블록
        bool extra = false; // EXTRA의 24비트 거리(TRY), 아니면 sBx
    };

    TypeChecker& checker_;
    Compilation& c_;
    uint32 module_;
    BytecodeModule& out_;
    HashMap<uint64, uint32> scalarIndex_;
    HashMap<uint64, uint32> stringIndex_; // 내용 hash → 첫 번호(같은 hash는 stringNext_ 사슬)
    Array<uint32> stringNext_;
    HashMap<uint64, uint32> refIndex_;
    HashMap<uint64, uint32> functionIndex_;
    // TYPE 표(형식 버전 2): Sparks struct·enum 심볼 → TYPE 번호, struct 필드 심볼 → TYPE 필드 번호.
    HashMap<uint64, uint32> typeIndex_;
    HashMap<uint64, uint32> fieldIndex_;
    bool tooLarge_ = false;

    // 함수 하나의 상태.
    const IrFunction* f_ = nullptr;
    uint32 nameNode_ = 0;
    Array<uint32> order_;     // 배치 순서의 블록(도달하는 블록만)
    Array<uint32> layoutOf_;  // 블록 → 배치 번호(도달하지 않으면 NoIrBlock)
    Array<uint32> firstPos_;  // 블록 → 첫 명령의 선형 번호
    Array<uint32> start_;     // 묶음 첫 레지스터 → 생존 시작 위치(없으면 Max)
    Array<uint32> end_;       // 묶음 첫 레지스터 → 생존 끝 위치
    Array<uint32> physFirst_; // 묶음 첫 레지스터 → 실제 레지스터
    Array<uint32> stringOf_;  // 가상 레지스터 → LOADSTR한 문자열 번호(리터럴 생성의 이름)
    Array<uint32> blockAddress_;
    Array<Fixup> fixups_;
    Array<uint32> switchFixups_; // 모듈 switch 표 번호(이 함수의 것)
    uint32 firstWord_ = 0;
    uint32 registers_[BytecodeBankCount] = {};
    uint32 window_[BytecodeBankCount] = {}; // 가장 큰 호출 창
    uint32 cursor_[BytecodeBankCount] = {}; // ToWindow 진행
};

} // namespace rocks::sparks::detail
