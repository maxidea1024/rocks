#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <SparksCompiler/Compilation.h>
#include <SparksVm/Vm.h>
#include <SparksVm/VmHost.h>

// SparksTestHost의 가짜 엔진 host다(Docs/Design/SparksVm.ko.md 6.3절, LC5d1). Compilation의
// binding schema로 native 함수·필드·문맥·리터럴을 묶고, 객체는 번호(`#1`…)·타입 이름·generation·
// 필드 값 표로 흉내 낸다. native 호출은 일반 동작(객체 결과는 새 객체, 그 밖은 0 값)과 기록을
// 남긴다. 상태는 `.run` 사례 하나에 하나다.

namespace rocks::sparks::conformance::detail {

// 묶음 하나(VmHostBinding::id = 번호)다.
struct FakeBinding {
    explicit FakeBinding(Allocator& allocator) noexcept
        : name(allocator), descriptor(allocator), resultType(allocator) {
    }

    BytecodeRefKind kind = BytecodeRefKind::NativeFunction;
    String name;                // 표시 이름(타입 인자 제외)
    String descriptor;          // 피연산자 서술자(native·필드)
    String resultType;          // 객체 결과·필드·문맥·리터럴의 타입 이름(새 객체를 만들 때)
    bool createsObject = false; // 결과·필드가 객체면 새 객체를 만든다(non-null 필드는 처음 읽을 때)
    bool optional = false;      // 결과·필드가 `T?`
    bool destroys = false;      // `RequestDestroy` — 수신자를 죽인다
    uint8 bank = 0;
    uint8 slots = 0;
    uint64 literal = 0; // 리터럴 값(이름 표 번호 + 1)
};

// 객체 필드 값 하나다.
struct FakeFieldValue {
    uint64 binding = 0;
    uint64 slots[32] = {};
    VmBuffer* buffer = nullptr;
};

struct FakeObject {
    String type;
    uint32 generation = 1;
    bool alive = true;
    Array<FakeFieldValue> fields;
};

class FakeHost final : public VmHost {
public:
    FakeHost(Allocator& allocator, const Compilation& compilation) noexcept;

    Result<VmHostBinding> Bind(const VmBindRequest& request,
                               StringBuilderBase& reason) noexcept override;
    bool IsAlive(uint64 handle) noexcept override;
    VmFaultKind GetContext(VmHostCall& call) noexcept override;
    VmFaultKind GetField(VmHostCall& call, uint64 object) noexcept override;
    VmFaultKind SetField(VmHostCall& call, uint64 object) noexcept override;
    VmFaultKind CallNative(VmHostCall& call) noexcept override;
    VmFaultKind MakeLiteral(VmHostCall& call) noexcept override;

    // `.run`의 객체 인자 `#number`(타입 type)의 handle이다(없으면 만든다 — 번호는 처음 쓴 순서와
    // 무관하게 그 번호다). 실패(번호 0·할당)는 0.
    uint64 ObjectArgument(uint32 number, StringView type) noexcept;
    // handle 표기: `#n`, 죽은 handle은 `dead#n`, 0은 `none`.
    void FormatHandle(StringBuilderBase& out, uint64 handle) const noexcept;
    // 마지막 ClearTrace 뒤의 native 호출 기록이다(`<이름>(<인자>) -> <결과>`).
    Span<const String> Trace() const noexcept {
        return trace_.View();
    }

    void ClearTrace() noexcept {
        trace_.Clear();
    }

    // 필드에 보관한 버퍼 수다(실행 문맥 ledger에 살아 있는 것으로 남는다 — V12).
    uint64 HeldBuffers() const noexcept;
    // 보관한 버퍼를 모두 놓는다(사례 끝).
    void ReleaseAll(VmExecution& execution) noexcept;
    // 할당 실패를 겪었는지다(실행기는 OutOfMemory로 돌려준다).
    bool RanOutOfMemory() const noexcept {
        return outOfMemory_;
    }

private:
    FakeObject* Find(uint64 handle) noexcept;
    uint64 NewObject(StringView type) noexcept;
    uint64 HandleOf(uint32 index) const noexcept;
    FakeFieldValue* FieldOf(FakeObject& object, uint64 binding) noexcept;
    void Record(StringView text) noexcept;

    Allocator* allocator_;
    const Compilation& compilation_;
    Array<FakeBinding> bindings_;
    Array<FakeObject> objects_; // 번호 n은 n − 1번
    Array<String> names_;       // 리터럴 이름 표
    Array<String> trace_;
    bool outOfMemory_ = false;
};

} // namespace rocks::sparks::conformance::detail
