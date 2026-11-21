#pragma once

#include <Core/Math/RandomStream.h>
#include <Core/Text/StringView.h>
#include <SparksVm/Vm.h>

// 언어 함수(부록 B — `CALLL`)의 서술자·묶기·호출 문맥이다(Docs/Design/SparksVm.ko.md 6a절, LC5c).
// 구현은 영역별 파일(VmLibraryMath·VmLibraryText·VmLibraryCollections)에 있고 VmLibrary.cpp가 이름
// 표로 묶는다.

namespace rocks::sparks::detail {

// 서술자의 타입 글자다(SparksVm 6a절).
enum class VmTypeCode : uint8 {
    Void,
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
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Rotator,
    Color,
    Transform,
    Mat4,
    String,
    Object,
    Wait,
    Struct,
    Array,
    Map,
    Set,
    Result,
    Optional,
    Raw // z<bank><slots> — 배치만 아는 타입
};

// 서술자 타입 하나(트리 노드)다. child는 원소·key·값·안쪽 타입의 노드 번호다.
struct VmTypeNode {
    VmTypeCode code = VmTypeCode::Void;
    uint8 bank = 0;
    uint8 slots = 0;
    uint8 child[2] = {};
};

inline constexpr uint32 VmMaxSignatureNodes = 48;
inline constexpr uint32 VmMaxOperands = 8;

// 묶은 서술자다: 피연산자(수신자 먼저)마다 노드와 그 bank 창 위치, 결과 노드.
struct VmSignature {
    VmTypeNode nodes[VmMaxSignatureNodes] = {};
    uint32 nodeCount = 0;
    uint8 operands[VmMaxOperands] = {};
    uint16 offsets[VmMaxOperands] = {};
    uint32 operandCount = 0;
    uint8 result = 0;
};

// 서술자 글자를 읽는다. 틀리면 false.
bool VmParseSignature(StringView text, VmSignature& out) noexcept;

// 노드의 Scalar 종류인지(정수·실수·bool·enum·이름)다.
bool VmIsSigned(VmTypeCode code) noexcept;
bool VmIsUnsigned(VmTypeCode code) noexcept;
bool VmIsFloat(VmTypeCode code) noexcept;

class VmLibrary;

// 구현 하나다. 반환: fault 종류(None이면 성공). 할당자 실패는 context.error.
struct VmLibraryContext;
using VmLibraryFunction = VmFaultKind (*)(VmLibraryContext& context) noexcept;

struct VmLibraryEntry {
    StringView name;
    VmLibraryFunction function;
};

// 영역별 구현 표다(이름은 참조 이름 `<주인>.<멤버>` — 벡터 공통 메서드는 `$V.<멤버>`).
Span<const VmLibraryEntry> VmMathEntries() noexcept;
Span<const VmLibraryEntry> VmTextEntries() noexcept;
Span<const VmLibraryEntry> VmCollectionEntries() noexcept;

// 호출 하나의 문맥이다: 창(bank별 시작), 서술자, 예산. 결과는 결과 bank의 창 0번부터 둔다(값을
// 바꾸는 메서드의 수신자는 그 bank 창 0번에 그대로 남는다 — 결과와 bank가 다르다).
struct VmLibraryContext {
    VmExecution* execution = nullptr;
    const VmSignature* signature = nullptr;
    uint64* s = nullptr;
    uint64* r = nullptr;
    VmBuffer** b = nullptr;
    uint64 budget = 0;
    ErrorCode error = ErrorCode::None;

    const VmTypeNode& Node(uint32 index) const noexcept {
        return signature->nodes[index];
    }

    const VmTypeNode& Operand(uint32 index) const noexcept {
        return signature->nodes[signature->operands[index]];
    }

    const VmTypeNode& ResultNode() const noexcept {
        return signature->nodes[signature->result];
    }

    // 피연산자 index의 Scalar slot·Ref·Buf 자리다.
    uint64* Scalars(uint32 index) const noexcept {
        return s + signature->offsets[index];
    }

    uint64 Ref(uint32 index) const noexcept {
        return r[signature->offsets[index]];
    }

    VmBuffer*& Buffer(uint32 index) const noexcept {
        return b[signature->offsets[index]];
    }

    double Real(uint32 index) const noexcept;
    int64 Integer(uint32 index) const noexcept;
};

// 실행 문맥의 내부(버퍼·예산·Random·로그)에 닿는 도구다(VmExecution의 friend).
class VmLibrary final {
public:
    // name의 구현이다(없으면 nullptr).
    static VmLibraryFunction Find(StringView name) noexcept;

    // 새 버퍼(원소 elements개 × elementBytes byte, 내용은 쓰지 않는다)를 만든다. fault·error는
    // context가 받는다(nullptr).
    static VmBuffer* Allocate(VmLibraryContext& context, VmBufferKind kind, uint64 elements,
                              uint64 elementBytes, VmFaultKind& fault) noexcept;
    static VmBuffer* NewString(VmLibraryContext& context, StringView text,
                               VmFaultKind& fault) noexcept;
    static void Release(VmLibraryContext& context, VmBuffer* buffer) noexcept;
    static void Retain(VmBuffer* buffer) noexcept {
        if (buffer != nullptr) {
            ++buffer->references;
        }
    }

    // Buf 창 at에 value를 둔다(참조는 넘겨받는다 — 이전 값을 놓는다).
    static void Put(VmLibraryContext& context, uint32 at, VmBuffer* value) noexcept;

    // 컨테이너 slot을 이 실행만 쥐고 원소 needed개 자리가 있게 한다(COW·증가 — 빈 컨테이너는
    // marker의 배치를 적는다).
    static VmFaultKind Reserve(VmLibraryContext& context, VmBuffer*& slot, uint64 needed,
                               uint32 marker) noexcept;
    static void FreeShell(VmLibraryContext& context, VmBuffer* buffer) noexcept;

    static RandomStream& Random(VmLibraryContext& context) noexcept;
    // 로그 기록을 더한다(할당 실패는 기록을 버린다 — 로그는 결과에 영향을 주지 않는다).
    static void Log(VmLibraryContext& context, VmLogLevel level, StringView message) noexcept;
};

} // namespace rocks::sparks::detail
