#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/StringBuilder.h>
#include <SparksVm/Bytecode.h>
#include <SparksVm/VmBuffer.h>

// VM과 엔진(host) 사이의 경계다(Docs/Design/SparksVm.ko.md 6절, LC5d). 모듈은 실행 전에
// LinkModule로 host에 묶고(못 묶는 참조가 있으면 로드 거부), 실행 중의 객체 필드·native 함수·문맥
// 이름·리터럴은 VmHost를 부른다.
//
// 사용 예:
//   VmLinkedModule linked(allocator);
//   StringBuilder<256> reason(allocator);
//   if (auto linkedOk = LinkModule(module, &host, linked, reason); !linkedOk) {
//       // reason: "ref 3 (native AudioSystem.StartLoop): ..."
//   }
//   VmExecution execution(allocator);
//   auto called = execution.Call(linked, function, scalars, refs, buffers);

namespace rocks::sparks {

class VmExecution;
enum class VmFaultKind : uint8;

// host 묶기 요청 하나다(참조 표 항목 하나 — 6.1절).
struct VmBindRequest {
    BytecodeRefKind kind = BytecodeRefKind::NativeFunction;
    uint32 ref = 0;            // 참조 번호(진단용)
    StringView name;           // 표시 이름(`AudioSystem.StartLoop`·`World`·`Tag`)
    StringView descriptor;     // NativeFunction·Field의 피연산자 서술자(6a절 문법, 없으면 빈 글자)
    StringView literal;        // Literal의 글자(`"Game.Door"`의 내용)
    StringView namedArguments; // Literal의 이름 지정 인자 표(`이름=bank글자;…`, 없으면 빈 글자)
    StringView typeArgument;   // 타입 인자를 단 호출의 첫 타입 인자 이름(`Find<Hp>` → `Hp`)
};

// host가 돌려준 묶음이다. id는 host가 정하는 번호이고 VM은 해석하지 않는다. bank·slots는
// Field·Context·Literal 값의 배치다(Context는 Ref 1 slot이어야 한다 — 링크가 본다).
struct VmHostBinding {
    uint64 id = 0;
    uint8 bank = 0;
    uint8 slots = 0;
    // 엔진 struct 필드(StructField)의 struct payload 안 절대 slot 위치(bank 묶음 Scalar·Ref·Buf 순
    // — TYPE 필드와 같은 규칙, 6.5절).
    uint16 offset = 0;
    // 엔진 struct 타입(Type)의 bank별 slot 수(0이면 struct가 아니다 — 객체·enum 타입).
    uint16 structSlots[BytecodeBankCount] = {};
};

class VmLinkedModule;

// 가상 호출의 대상이다(6.5절): Sparks 함수(링크된 모듈과 함수 번호) 또는 native(묶음 번호).
struct VmVirtualTarget {
    const VmLinkedModule* module = nullptr; // Sparks 함수면 그 모듈
    uint32 function = NoBytecodeIndex;
    bool native = false;
    uint64 binding = 0; // native면 CallNative에 줄 묶음 번호
};

// host 호출 하나의 창과 도구다. 창은 bank별 연속 slot이다: CALLN·NEWLIT은 이 frame의 할당 수부터
// frame 끝까지(인자는 수신자 먼저 bank별로 앞에서부터, 결과는 결과 bank의 0번부터), GETF는 결과
// 레지스터 묶음, SETF는 값 레지스터 묶음(읽기만), GETCTX는 결과 Ref 하나다.
class VmHostCall final {
public:
    // 묶음 번호(VmHostBinding::id)다.
    uint64 Binding() const noexcept {
        return binding_;
    }

    Span<uint64> Scalars() const noexcept {
        return scalars_;
    }

    Span<uint64> Refs() const noexcept {
        return refs_;
    }

    // Buf 창이다. 쓰기는 PutBuffer로 한다(참조계수를 맞춘다).
    Span<VmBuffer* const> Buffers() const noexcept {
        return Span<VmBuffer* const>(buffers_.Data(), buffers_.Size());
    }

    // Buf 창 at에 value를 둔다(참조 하나를 넘겨받는다 — 이전 값은 놓는다). at이 창 밖이면 value를
    // 놓고 false다(host는 그때 ContractViolation을 돌려준다).
    bool PutBuffer(uint32 at, VmBuffer* value) noexcept;

    // 버퍼를 만들고 놓는 실행 문맥이다(NewString·NewContainer·Retain·Release).
    VmExecution& Execution() const noexcept {
        return *execution_;
    }

    // 할당자 실패를 알린다(실행은 Call의 OutOfMemory로 끝난다). 반환 값의 fault보다 앞선다.
    void Fail(ErrorCode error) noexcept {
        error_ = error;
    }

    ErrorCode Error() const noexcept {
        return error_;
    }

private:
    friend class VmExecution;

    uint64 binding_ = 0;
    Span<uint64> scalars_;
    Span<uint64> refs_;
    Span<VmBuffer*> buffers_;
    VmExecution* execution_ = nullptr;
    ErrorCode error_ = ErrorCode::None;
};

// 엔진 쪽 구현이다(SparksTestHost의 가짜 host, 엔진의 ScriptRuntime — EI2). 모든 함수는 스크립트
// 입력으로 Fatal하지 않는다: 계약 위반은 ContractViolation, 할당 실패는 call.Fail(SL 8.4절).
class VmHost {
public:
    virtual ~VmHost() noexcept = default;

    // 참조 하나를 묶는다. 못 묶으면 오류와 사유(reason — 영어 한 줄)다. 서술자가 host 시그니처와
    // 다르면 거부한다(schema와 모듈이 어긋남).
    virtual Result<VmHostBinding> Bind(const VmBindRequest& request,
                                       StringBuilderBase& reason) noexcept = 0;
    // handle(0이 아님)이 살아 있는 객체인지다(generation 대조 — 죽은 handle은 null로 읽힌다).
    virtual bool IsAlive(uint64 handle) noexcept = 0;
    // 문맥 객체 handle을 call.Refs()[0]에 둔다.
    virtual VmFaultKind GetContext(VmHostCall& call) noexcept = 0;
    // 살아 있는 object의 필드를 창에 둔다(Buf는 PutBuffer로).
    virtual VmFaultKind GetField(VmHostCall& call, uint64 object) noexcept = 0;
    // 살아 있는 object의 필드를 창의 값으로 바꾼다(Buf를 보관하면 Retain).
    virtual VmFaultKind SetField(VmHostCall& call, uint64 object) noexcept = 0;
    // native 함수를 부른다.
    virtual VmFaultKind CallNative(VmHostCall& call) noexcept = 0;
    // 리터럴 값을 결과 bank 창 0번에 만든다(이름 지정 인자는 창에 있다).
    virtual VmFaultKind MakeLiteral(VmHostCall& call) noexcept = 0;

    // ---- LC5d2(6.5절) — 기본 구현은 계약 위반·거짓·0 값이다. ----
    // CALLV의 살아 있는 수신자 receiver(창 Ref 0번)와 묶음(call.Binding())의 대상을 target에 둔다.
    virtual VmFaultKind ResolveVirtual(VmHostCall& call, uint64 receiver,
                                       VmVirtualTarget& target) noexcept;
    // 살아 있는 handle이 묶은 타입(Type 묶음 번호)인지다(CAST — `as?`).
    virtual bool IsA(uint64 handle, uint64 type) noexcept;
    // 새 엔진 struct의 schema 기본값을 채운다(call.Scalars()·Refs()가 그 struct의 Scalar·Ref 구역,
    // Buf 필드는 null로 남는다 — 엔진 struct의 Buf 필드 기본값은 연동 EI5).
    virtual VmFaultKind InitializeStruct(VmHostCall& call) noexcept;
    // Name·Tag 값(이름 번호 — 0은 없음)의 표기를 붙인다. 모르는 값은 false(ContractViolation).
    virtual bool AppendName(uint64 value, StringBuilderBase& out) noexcept;
    // 엔진 enum(Type 묶음 type)의 case 이름을 붙인다. 모르는 값은 false(ContractViolation).
    virtual bool AppendEnumName(uint64 type, uint64 value, StringBuilderBase& out) noexcept;
};

// 링크된 모듈이다: 모듈과 host, 참조 번호별 묶음. 모듈과 host는 이 객체보다 오래 살아야 한다.
class VmLinkedModule final {
public:
    explicit VmLinkedModule(Allocator& allocator) noexcept
        : hosts_(allocator), languageIndex_(allocator), languages_(allocator), imports_(allocator),
          shapes_(allocator), localFields_(allocator) {
    }

    VmLinkedModule(const VmLinkedModule&) = delete;
    VmLinkedModule& operator=(const VmLinkedModule&) = delete;

    const BytecodeModule& Module() const noexcept {
        return *module_;
    }

    VmHost* Host() const noexcept {
        return host_;
    }

    // 다른 모듈 함수 참조를 모두 풀었는지다(LinkImports — 6.5절). 풀기 전에는 Call이 거부한다.
    bool ImportsLinked() const noexcept {
        return importsLinked_;
    }

    // LinkModule이 성공했는지다(실패한·링크하지 않은 객체는 Call이 거부한다 — LC5d1 리뷰).
    bool Linked() const noexcept {
        return linked_;
    }

    // ref의 host 묶음이다(host가 묶지 않는 참조는 0).
    const VmHostBinding& HostBinding(uint32 ref) const noexcept {
        return hosts_[ref];
    }

private:
    friend class VmExecution;
    friend class VmSession;
    friend Result<void> LinkModule(const BytecodeModule& module, VmHost* host, VmLinkedModule& out,
                                   StringBuilderBase& reason) noexcept;
    friend Result<void> LinkImports(Span<VmLinkedModule* const> modules,
                                    StringBuilderBase& reason) noexcept;

    // 다른 모듈 함수 참조의 대상이다(참조 번호별 — ImportedFunction이 아니면 비었다). generation은
    // 푼 때의 대상 링크 세대다: 대상을 다시 링크하면 세대가 바뀌어 CALL이 ContractViolation이다.
    struct Import {
        const VmLinkedModule* module = nullptr;
        uint32 function = NoBytecodeIndex;
        uint64 generation = 0;
    };

    // 같은 모듈 Sparks 클래스 필드 참조의 로컬 묶음이다(SparksRuntime 2.2절 — host에 묻지 않는다).
    struct LocalField {
        uint32 classIndex = NoBytecodeIndex; // NoIndex면 로컬 필드가 아니다
        uint8 bank = 0;
        uint8 slots = 0;
        uint32 offset = 0; // 클래스 구역 안 위치
    };

    // ImportedFunction·VirtualFunction 참조 서술자의 함수 모양(bank별 매개변수 slot 수와 반환 —
    // 6.5절 모양 대조)이다.
    struct Shape {
        uint16 parameters[BytecodeBankCount] = {};
        uint8 returnBank = 0;
        uint8 returnSlots = 0;
        bool valid = false;
    };

    const BytecodeModule* module_ = nullptr;
    VmHost* host_ = nullptr;
    Array<VmHostBinding> hosts_;
    // 언어 함수 묶음(참조 번호 → languages_의 묶음 번호, 언어 함수가 아니면 NoBytecodeIndex). 묶음
    // 모양은 VM 안의 것이라 단어 배열로 둔다.
    Array<uint32> languageIndex_;
    Array<uint64> languages_;
    Array<Import> imports_;
    Array<Shape> shapes_;
    Array<LocalField> localFields_;
    bool importsLinked_ = false;
    bool linked_ = false;
    uint64 generation_ = 0; // LinkModule마다 바뀐다(이 객체를 가리키는 import의 무효화)
};

// verifier를 통과한 module의 참조를 모두 묶는다(6.1절). host가 nullptr이면 host가 묶어야 하는
// 참조(native·필드·문맥·리터럴)가 있을 때 거부한다. out은 이전 내용을 버린다. 실패: 못 묶은 참조가
// 있거나 필드 명령의 bank·slot 수가 묶음과 다르면 InvalidArgument(reason에 `ref <번호> (<종류>
// <이름>): <사유>` 또는 `function <이름> word <주소>: <사유>`), 할당 실패는 OutOfMemory.
Result<void> LinkModule(const BytecodeModule& module, VmHost* host, VmLinkedModule& out,
                        StringBuilderBase& reason) noexcept;

// 링크된 modules의 다른 모듈 함수 참조(`<import 경로>:Owner.Name`)를 modules 안의 함수로 푼다(6.5절
// —// 모든 module은 링크에 성공했고 같은 host여야 한다. 대상 모듈을 다시 링크하면 그 import는
// 무효가 되어 부르면 ContractViolation이다 — 다시 LinkImports한다. 모듈들은 실행 동안 살아 있어야
// 한다. — 순환 import도 된다). 대상의 함수 표 모양(bank별 매개변수 수·반환 bank·slot 수)이 참조
// 서술자와 같아야 한다. 실패: 대상 모듈·함수가 없거나 모양이 다르면 InvalidArgument(reason에 `ref
// <번호> (import <이름>): <사유>`), 할당 실패는 OutOfMemory. 모든 module의 ImportsLinked가 참이
// 된다.
Result<void> LinkImports(Span<VmLinkedModule* const> modules, StringBuilderBase& reason) noexcept;

} // namespace rocks::sparks
