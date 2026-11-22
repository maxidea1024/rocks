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

// 구현 하나다. pattern은 받는 서술자 모양이다(서술자 문법 + 자리표시: `I` 정수, `F` 실수, `N` 숫자,
// `V` 벡터, `O` 정렬 가능한 값(정수·실수·문자열·enum), `T`·`U` 아무 타입 — 같은 글자는 같은 타입).
// 링크가 이름과 모양으로 구현을 고른다(SparksVm 6.1절 — 모양이 다르면 거부).
struct VmLibraryEntry {
    StringView name;
    StringView pattern;
    VmLibraryFunction function;
};

// 값 묶음 하나의 slot 상한이다(서술자 — 컨테이너 표지의 5비트 slot 필드 최댓값과 같다: 32 이상은
// 표지에서 잘려 0 stride가 된다, LC5d1 리뷰 B1).
inline constexpr uint32 VmMaxValueSlots = 31;

// 실행 하나의 `Log.*` 글자 상한이다(메모리 예산 밖의 host 메모리를 무한히 쓰지 않게).
inline constexpr usize VmMaxLogBytes = 1024 * 1024;

// 서술자 descriptor가 pattern 모양인지다.
bool VmMatchPattern(StringView pattern, StringView descriptor) noexcept;

// 컨테이너 key 찾기(원소 번호, 없으면 uint32 최댓값)와 해시 색인 재구성이다(VmContainers.cpp —
// 원소를 지우거나 옮긴 뒤 부른다).
uint32 VmContainerFind(VmBuffer* buffer, const uint64* key) noexcept;
void VmContainerReindex(VmBuffer* buffer) noexcept;
// 끝에 넣은 원소 element 하나를 색인에 더한다(용량은 Reserve가 맞췄다).
void VmContainerIndexInsert(VmBuffer* buffer, uint32 element) noexcept;
// 값을 바꾸는 언어 메서드인지다(CALL.MUT — 수신자를 창 0번에 돌려받는다, 링크의 창 검사).
bool VmLanguageMutates(StringView name) noexcept;

// 영역별 구현 표다(이름은 참조 이름 `<주인>.<멤버>` — 벡터 공통 메서드는 `$V.<멤버>`).
Span<const VmLibraryEntry> VmMathEntries() noexcept;
Span<const VmLibraryEntry> VmTextEntries() noexcept;
Span<const VmLibraryEntry> VmCollectionEntries() noexcept;
Span<const VmLibraryEntry> VmWaitEntries() noexcept;

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

// 링크한 언어 함수 하나의 묶음이다(VmLinkedModule이 단어 배열로 보관한다 — SparksVm 6.1절).
struct VmLanguageBinding {
    VmLibraryFunction function = nullptr;
    VmSignature signature;
};

inline constexpr usize VmLanguageWords =
    (sizeof(VmLanguageBinding) + sizeof(uint64) - 1) / sizeof(uint64);

// 실행 문맥의 내부(버퍼·예산·Random·로그)에 닿는 도구다(VmExecution의 friend).
class VmLibrary final {
public:
    // name·서술자의 구현이다(없거나 모양이 맞지 않으면 nullptr).
    static VmLibraryFunction Find(StringView name, StringView descriptor) noexcept;
    // 실행 의미가 LC6(대기)·host(시간·에셋·Tag…)에 있는 언어 함수 이름인지다(링크는 하되 부르면
    // ContractViolation — V5).
    static bool Deferred(StringView name) noexcept;

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
    // session 대기를 만든다(SparksRuntime 2.4절 — kind는 VmSession::WaitKind 순번). session 밖이면
    // 0.
    static uint64 NewWait(VmLibraryContext& context, uint8 kind, uint8 clock, double seconds,
                          Span<const uint64> children) noexcept;
    // handle이 살아 있는 객체인지다(host — 6.2절).
    static bool Alive(VmLibraryContext& context, uint64 handle) noexcept;
    // 실행 문맥의 할당자다(임시 서식 builder용).
    static Allocator& AllocatorOf(VmLibraryContext& context) noexcept;
    // 로그 기록을 더한다(할당 실패는 기록을 버린다 — 로그는 결과에 영향을 주지 않는다).
    static void Log(VmLibraryContext& context, VmLogLevel level, StringView message) noexcept;
};

} // namespace rocks::sparks::detail
