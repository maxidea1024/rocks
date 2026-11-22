#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <SparksVm/Vm.h>
#include <SparksVm/VmHost.h>

// Sparks 실행 단위(session)다(Docs/Design/SparksRuntime.ko.md 2절·2.2절, LC6a). instance(host
// 객체에 붙은 Sparks 클래스 instance)의 필드 저장소와 실행 문맥 하나를 가진다. Sparks 클래스 필드의
// GETF·SETF는 session의 instance 저장소를 직접 쓴다(필드 로컬 묶음 — R2).
//
// 사용 예:
//   VmSession session(allocator, &host);
//   session.AddModule(linked);
//   auto instance = session.CreateInstance(handle, linked, classIndex, init);
//   auto enabled = session.RunHook(instance.Value(), BytecodeHookKind::Enable, {}, {});
//   session.Detach(instance.Value());

namespace rocks::sparks {

class VmSession final {
public:
    VmSession(Allocator& allocator, VmHost* host) noexcept;
    // 남은 instance를 모두 Detach한다(CloseSession — 실행 문맥 ledger가 0이 된다).
    ~VmSession() noexcept;
    VmSession(const VmSession&) = delete;
    VmSession& operator=(const VmSession&) = delete;

    // session 모듈 목록에 더한다(다른 모듈 기반 클래스를 찾는 범위). linked는 링크에 성공했고
    // session의 host로 링크했어야 하며 session보다 오래 산다. 실패: 그 밖이면 InvalidArgument,
    // 할당 실패는 OutOfMemory.
    Result<void> AddModule(const VmLinkedModule& linked) noexcept;

    // handle에 module의 classIndex 클래스 instance를 붙이고 기반 먼저 `$init`을 부른다. 반환은
    // instance 번호, init은 초기화 실행 결과(fault면 instance를 만들지 않고 NoBytecodeIndex를
    // 돌려준다). 실패: 모듈이 session에 없거나 클래스 번호가 범위 밖이거나 handle에 이미 instance가
    // 있거나 handle이 살아 있지 않으면 InvalidArgument, 할당 실패는 OutOfMemory.
    Result<uint32> CreateInstance(uint64 handle, const VmLinkedModule& module, uint32 classIndex,
                                  VmCallResult& init) noexcept;

    // instance의 kind hook을 기반 사슬 순(기반 먼저)으로 부른다. scalars·refs는 this 뒤 인자(Tick의
    // dt 등)다. 첫 fault에서 멈추고 그 결과, 아니면 마지막 결과(hook이 없으면 Returned·0 steps)다.
    Result<VmCallResult> RunHook(uint32 instance, BytecodeHookKind kind, Span<const uint64> scalars,
                                 Span<const uint64> refs) noexcept;

    // instance를 this로 module의 function을 부른다(refs는 this 뒤 Ref 인자).
    Result<VmCallResult> Call(uint32 instance, const VmLinkedModule& module, uint32 function,
                              Span<const uint64> scalars, Span<const uint64> refs,
                              Span<VmBuffer* const> buffers) noexcept;

    // 필드 버퍼를 놓고 instance를 표에서 뺀다(번호는 다시 쓰지 않는다).
    void Detach(uint32 instance) noexcept;

    // 붙은 instance를 모두 Detach한다(CloseSession).
    void DetachAll() noexcept;

    // 붙은 instance 수다.
    uint32 AttachedCount() const noexcept;

    // handle에 붙은 instance 번호다(없으면 NoBytecodeIndex).
    uint32 Find(uint64 handle) const noexcept;

    VmExecution& Execution() noexcept {
        return execution_;
    }

    // instance의 필드 저장소(기반 사슬 전체 — bank별)다(시험·덤프용).
    struct FieldView {
        Span<const uint64> scalars;
        Span<const uint64> refs;
        Span<VmBuffer* const> buffers;
    };

    FieldView Fields(uint32 instance) const noexcept;
    // instance의 클래스다.
    const VmLinkedModule* ModuleOf(uint32 instance) const noexcept;
    uint32 ClassOf(uint32 instance) const noexcept;
    // 클래스 구역의 bank별 시작(기반 사슬의 slot 합)이다. 기반을 찾지 못하면 false.
    bool RegionStart(const VmLinkedModule& module, uint32 classIndex,
                     uint32 (&start)[BytecodeBankCount]) const noexcept;

private:
    friend class VmExecution;

    struct Instance {
        explicit Instance(Allocator& allocator) noexcept
            : scalars(allocator), refs(allocator), buffers(allocator) {
        }

        uint64 handle = 0;
        const VmLinkedModule* module = nullptr;
        uint32 classIndex = NoBytecodeIndex;
        bool attached = false;
        Array<uint64> scalars;
        Array<uint64> refs;
        Array<VmBuffer*> buffers;
    };

    // 기반 클래스(같은 모듈, 또는 `<경로>:Class` — session 모듈)를 찾는다. 엔진 기반·없음은 false.
    bool BaseOf(const VmLinkedModule& module, uint32 classIndex, const VmLinkedModule*& baseModule,
                uint32& baseClass) const noexcept;
    // GETF·SETF의 로컬 필드 자리다(instance가 아니면 false).
    bool FieldSlots(uint64 handle, const VmLinkedModule& module, uint32 classIndex, uint32 bank,
                    uint32 offset, uint32 slots, uint64*& values, VmBuffer**& buffers) noexcept;
    void ReleaseFields(Instance& instance) noexcept;

    Allocator* allocator_;
    VmHost* host_;
    VmExecution execution_;
    Array<const VmLinkedModule*> modules_;
    Array<Instance> instances_;
    HashMap<uint64, uint32> byHandle_;
};

} // namespace rocks::sparks
