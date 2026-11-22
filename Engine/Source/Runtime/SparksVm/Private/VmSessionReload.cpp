#include <Core/Move.h>
#include <SparksVm/VmSession.h>

// hot reload의 구현이다(Docs/Design/SparksReload.ko.md — LC7a): 새 revision 계획·적용, 이전
// revision 코드(Keep 실행)의 필드 위치 번역, revision 참조 판정.

namespace rocks::sparks {

namespace {

// 문자열 번호의 글자다(NoIndex면 빈 글자).
StringView VmText(const BytecodeModule& module, uint32 index) noexcept {
    return index == NoBytecodeIndex ? StringView() : module.String(index);
}

// 클래스의 기반 표기다(같은 모듈 기반은 그 클래스 이름, 다른 모듈·엔진 기반은 기록한 이름).
StringView VmBaseText(const BytecodeModule& module, uint32 classIndex) noexcept {
    const BytecodeClass& entry = module.classes[classIndex];
    if (entry.baseClass != NoBytecodeIndex) {
        return module.String(module.classes[entry.baseClass].name);
    }

    return VmText(module, entry.baseName);
}

uint32 VmFindClass(const BytecodeModule& module, StringView name) noexcept {
    for (uint32 index = 0; index < module.classes.Size(); ++index) {
        if (module.String(module.classes[index].name) == name) {
            return index;
        }
    }

    return NoBytecodeIndex;
}

uint32 VmFindFunction(const BytecodeModule& module, StringView name) noexcept {
    for (uint32 index = 0; index < module.functions.Size(); ++index) {
        if (module.String(module.functions[index].name) == name) {
            return index;
        }
    }

    return NoBytecodeIndex;
}

// 두 필드가 이름·타입 서술자·배치·표지까지 같은지다(R1 — 같은 배치).
bool VmSameField(const BytecodeModule& left, const BytecodeTypeField& a,
                 const BytecodeModule& right, const BytecodeTypeField& b) noexcept {
    return left.String(a.name) == right.String(b.name) &&
           VmText(left, a.type) == VmText(right, b.type) && a.bank == b.bank &&
           a.slots == b.slots && a.offset == b.offset && a.flags == b.flags;
}

// 두 handler가 같은 함수 이름·경로·이벤트·재진입 정책인지다.
bool VmSameHandler(const BytecodeModule& left, const BytecodeHandler& a,
                   const BytecodeModule& right, const BytecodeHandler& b) noexcept {
    return left.String(left.functions[a.function].name) ==
               right.String(right.functions[b.function].name) &&
           VmText(left, a.path) == VmText(right, b.path) &&
           VmText(left, a.event) == VmText(right, b.event) && a.reentry == b.reentry;
}

// 판정 실패: reason에 이유를 쓰고 InvalidArgument(할당 실패는 그 오류)다.
template <typename... Args>
Result<void> VmReject(StringBuilderBase& reason, StringView format, const Args&... args) noexcept {
    if (auto appended = reason.TryAppendFormat(format, args...); !appended) {
        return appended;
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

// 클래스의 kind hook 함수다(없으면 NoIndex).
uint32 VmHookOf(const BytecodeModule& module, uint32 classIndex, BytecodeHookKind kind) noexcept {
    const BytecodeClass& entry = module.classes[classIndex];
    for (uint32 hook = entry.firstHook; hook < entry.firstHook + entry.hookCount; ++hook) {
        if (module.hooks[hook].kind == kind) {
            return module.hooks[hook].function;
        }
    }

    return NoBytecodeIndex;
}

} // namespace

bool VmSession::HasInstancesOf(const VmLinkedModule& module, uint32 classIndex) const noexcept {
    for (uint32 instance = 0; instance < instances_.Size(); ++instance) {
        if (!instances_[instance].attached) {
            continue;
        }

        const VmLinkedModule* chainModules[MaxBaseDepth] = {};
        uint32 chainClasses[MaxBaseDepth] = {};
        const uint32 depth = Chain(instance, chainModules, chainClasses);
        for (uint32 at = 0; at < depth; ++at) {
            if (chainModules[at] == &module && chainClasses[at] == classIndex) {
                return true;
            }
        }
    }

    return false;
}

Result<void> VmSession::PlanReload(const VmLinkedModule& old, const VmLinkedModule& fresh,
                                   VmReloadExecutions executions, VmReloadPlan& plan,
                                   StringBuilderBase& reason) noexcept {
    plan.old_ = nullptr;
    plan.fresh_ = nullptr;
    plan.classes_ = 0;
    plan.classMap_.Clear();
    plan.fieldMap_.Clear();
    plan.handlerMap_.Clear();
    plan.changed_.Clear();
    if (closed_ || execution_.running_) {
        return VmReject(reason, "the session is closed or running");
    }

    bool oldKnown = false;
    bool freshKnown = false;
    for (const VmLinkedModule* existing : modules_) {
        oldKnown = oldKnown || existing == &old;
        freshKnown = freshKnown || existing == &fresh;
    }

    if (!oldKnown) {
        return VmReject(reason, "the old revision is not a session module");
    }

    if (freshKnown || &fresh == &old) {
        return VmReject(reason, "the new revision must be a new linked module");
    }

    if (!fresh.Linked() || !fresh.ImportsLinked() || fresh.Host() != host_) {
        return VmReject(reason, "the new revision is not linked with the session host");
    }

    const BytecodeModule& before = old.Module();
    const BytecodeModule& after = fresh.Module();
    if (VmText(before, before.path) != VmText(after, after.path)) {
        return VmReject(reason, "the new revision has another module path");
    }

    if (!plan.classMap_.TryResize(before.classes.Size()) ||
        !plan.fieldMap_.TryResize(before.classFields.Size()) ||
        !plan.handlerMap_.TryResize(before.handlers.Size()) ||
        !plan.changed_.TryResize(before.functions.Size())) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    for (uint32& value : plan.classMap_) {
        value = NoBytecodeIndex;
    }

    for (uint32& value : plan.fieldMap_) {
        value = NoBytecodeIndex;
    }

    for (uint32& value : plan.handlerMap_) {
        value = NoBytecodeIndex;
    }

    // 클래스 대응(이름 — SparksReload K2). instance가 있는 클래스의 삭제·기반 변경은 R3(instance
    // 재생성)가 필요하므로 판정 실패다.
    for (uint32 oldClass = 0; oldClass < before.classes.Size(); ++oldClass) {
        const BytecodeClass& oldEntry = before.classes[oldClass];
        const StringView name = before.String(oldEntry.name);
        const uint32 newClass = VmFindClass(after, name);
        const bool used = HasInstancesOf(old, oldClass);
        if (newClass == NoBytecodeIndex) {
            if (used) {
                return VmReject(reason,
                                "class {} was removed while it has instances (they need a "
                                "respawn)",
                                name);
            }

            continue;
        }

        if (VmBaseText(before, oldClass) != VmBaseText(after, newClass)) {
            if (used) {
                return VmReject(reason,
                                "the base of class {} changed while it has instances (they need "
                                "a respawn)",
                                name);
            }

            continue;
        }

        plan.classMap_[oldClass] = newClass;
        ++plan.classes_;
        // 필드 대응(K4): 같은 이름 또는 새 필드의 `@formerly`, 같은 타입 서술자·배치 크기.
        const BytecodeClass& newEntry = after.classes[newClass];
        for (uint32 field = oldEntry.firstField; field < oldEntry.firstField + oldEntry.fieldCount;
             ++field) {
            const BytecodeTypeField& was = before.classFields[field];
            for (uint32 candidate = newEntry.firstField;
                 candidate < newEntry.firstField + newEntry.fieldCount; ++candidate) {
                const BytecodeTypeField& now = after.classFields[candidate];
                const bool named = after.String(now.name) == before.String(was.name) ||
                                   VmText(after, now.formerName) == before.String(was.name);
                if (named && VmText(after, now.type) == VmText(before, was.type) &&
                    now.bank == was.bank && now.slots == was.slots) {
                    plan.fieldMap_[field] = candidate;
                    break;
                }
            }
        }

        // handler 대응(함수 이름·경로·이벤트).
        for (uint32 handler = oldEntry.firstHandler;
             handler < oldEntry.firstHandler + oldEntry.handlerCount; ++handler) {
            for (uint32 candidate = newEntry.firstHandler;
                 candidate < newEntry.firstHandler + newEntry.handlerCount; ++candidate) {
                if (VmSameHandler(before, before.handlers[handler], after,
                                  after.handlers[candidate])) {
                    plan.handlerMap_[handler] = candidate;
                    break;
                }
            }
        }

        if (!used) {
            continue;
        }

        // R1(LC7a): instance가 있는 클래스는 필드 배치와 handler 목록이 그대로여야 한다(필드
        // migration·handler 연결 재구성은 R2 — LC7b).
        bool sameFields = oldEntry.fieldCount == newEntry.fieldCount;
        for (uint32 at = 0; sameFields && at < oldEntry.fieldCount; ++at) {
            sameFields = VmSameField(before, before.classFields[oldEntry.firstField + at], after,
                                     after.classFields[newEntry.firstField + at]);
        }

        if (!sameFields) {
            return VmReject(reason,
                            "the fields of class {} changed (field migration is not supported "
                            "yet)",
                            name);
        }

        bool sameHandlers = oldEntry.handlerCount == newEntry.handlerCount;
        for (uint32 at = 0; sameHandlers && at < oldEntry.handlerCount; ++at) {
            sameHandlers =
                plan.handlerMap_[oldEntry.firstHandler + at] == newEntry.firstHandler + at;
        }

        if (!sameHandlers) {
            return VmReject(reason,
                            "the handlers of class {} changed (handler migration is not "
                            "supported yet)",
                            name);
        }
    }

    // 함수 대응(이름 — 본문 hash 비교, SparksBytecode 6.2절).
    for (uint32 function = 0; function < before.functions.Size(); ++function) {
        const uint32 match = VmFindFunction(after, before.String(before.functions[function].name));
        bool changed = match == NoBytecodeIndex;
        for (usize at = 0; !changed && at < BytecodeHashBytes; ++at) {
            changed =
                before.functions[function].bodyHash[at] != after.functions[match].bodyHash[at];
        }

        plan.changed_[function] = changed ? 1 : 0;
    }

    plan.old_ = &old;
    plan.fresh_ = &fresh;
    plan.executions_ = executions;
    return Result<void>::Success();
}

bool VmSession::Uses(const VmLinkedModule* link) const noexcept {
    for (const VmLinkedModule* existing : modules_) {
        if (existing == link) {
            return true;
        }
    }

    for (const Instance& instance : instances_) {
        if (instance.attached && instance.module == link) {
            return true;
        }
    }

    for (const Connection& connection : connections_) {
        if (connection.module == link) {
            return true;
        }
    }

    // 큐 항목의 key 글자는 발행한 모듈의 bytecode를 가리킨다.
    for (usize at = queueHead_; at < queue_.Size(); ++at) {
        if (queue_[at].module == link || queue_[at].targetModule == link) {
            return true;
        }
    }

    for (const Suspended& entry : suspended_) {
        if (!entry.live) {
            continue;
        }

        if (entry.handlerModule == link) {
            return true;
        }

        for (const VmExecution::Frame& frame : entry.frames) {
            if (frame.link == link) {
                return true;
            }
        }
    }

    return false;
}

void VmSession::PruneRetired() noexcept {
    // 직접 참조되는 revision과, 그 revision의 필드 번역이 지나가는 뒤 revision만 남긴다(포인터만
    // 비교한다 — 놓인 revision 객체를 읽지 않는다).
    for (Retired& entry : retired_) {
        entry.needed = Uses(entry.link);
    }

    for (bool grew = true; grew;) {
        grew = false;
        for (const Retired& entry : retired_) {
            if (!entry.needed) {
                continue;
            }

            for (Retired& next : retired_) {
                if (!next.needed && next.link == entry.successor) {
                    next.needed = true;
                    grew = true;
                }
            }
        }
    }

    for (usize at = retired_.Size(); at > 0; --at) {
        if (!retired_[at - 1].needed) {
            static_cast<void>(retired_.TryRemoveAt(at - 1));
        }
    }
}

bool VmSession::References(const VmLinkedModule& linked) noexcept {
    PruneRetired();
    if (Uses(&linked)) {
        return true;
    }

    for (const Retired& entry : retired_) {
        if (entry.link == &linked) {
            return true;
        }
    }

    return false;
}

bool VmSession::TranslateField(const VmLinkedModule*& module, uint32& classIndex, uint32 bank,
                               uint32 slots, uint32& offset) const noexcept {
    for (const Retired& entry : retired_) {
        if (entry.link != module) {
            continue;
        }

        const BytecodeModule& before = entry.link->Module();
        if (classIndex >= entry.classMap.Size() || entry.classMap[classIndex] == NoBytecodeIndex) {
            return false;
        }

        const BytecodeClass& owner = before.classes[classIndex];
        for (uint32 field = owner.firstField; field < owner.firstField + owner.fieldCount;
             ++field) {
            const BytecodeTypeField& was = before.classFields[field];
            if (was.bank != bank || was.offset != offset) {
                continue;
            }

            const uint32 mapped = entry.fieldMap[field];
            if (mapped == NoBytecodeIndex) {
                return false;
            }

            const BytecodeTypeField& now = entry.successor->Module().classFields[mapped];
            if (now.bank != bank || now.slots != slots) {
                return false;
            }

            module = entry.successor;
            classIndex = entry.classMap[classIndex];
            offset = now.offset;
            return true;
        }

        return false;
    }

    return false;
}

VmEventKey VmSession::HandlerKey(const VmLinkedModule& module, uint32 handler) const noexcept {
    const BytecodeModule& bytecode = module.Module();
    const BytecodeHandler& entry = bytecode.handlers[handler];
    const bool changed = entry.eventRef == NoBytecodeIndex && entry.pathCount > 0 &&
                         bytecode.String(entry.event) == "Changed";
    if (changed) {
        return module.keys_[bytecode.handlerPaths[entry.firstPath + entry.pathCount - 1]];
    }

    return entry.eventRef == NoBytecodeIndex ? VmEventKey() : module.keys_[entry.eventRef];
}

Result<VmReloadReport> VmSession::ApplyReload(VmReloadPlan& plan) noexcept {
    using Outcome = Result<VmReloadReport>;
    const VmLinkedModule* old = plan.old_;
    const VmLinkedModule* fresh = plan.fresh_;
    if (closed_ || execution_.running_ || old == nullptr || fresh == nullptr) {
        return Outcome::Failure(ErrorCode::InvalidArgument);
    }

    usize slot = modules_.Size();
    for (usize at = 0; at < modules_.Size(); ++at) {
        if (modules_[at] == fresh) {
            return Outcome::Failure(ErrorCode::InvalidArgument);
        }

        slot = modules_[at] == old ? at : slot;
    }

    if (slot == modules_.Size()) {
        return Outcome::Failure(ErrorCode::InvalidArgument);
    }

    // 계획 뒤에 생긴 instance도 대응이 있어야 한다(낡은 계획 — 아무것도 바꾸지 않는다).
    for (uint32 instance = 0; instance < instances_.Size(); ++instance) {
        if (!instances_[instance].attached) {
            continue;
        }

        const VmLinkedModule* chainModules[MaxBaseDepth] = {};
        uint32 chainClasses[MaxBaseDepth] = {};
        const uint32 depth = Chain(instance, chainModules, chainClasses);
        for (uint32 at = 0; at < depth; ++at) {
            if (chainModules[at] == old && plan.classMap_[chainClasses[at]] == NoBytecodeIndex) {
                return Outcome::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    PruneRetired();
    const BytecodeModule& before = old->Module();
    // 할당은 바꾸기 전에 모두 마친다(SparksReload K3 — 실패면 session 그대로).
    struct Cancel {
        uint32 suspended = NoBytecodeIndex;
        uint64 wait = 0;
    };

    struct Restart {
        uint32 instance = NoBytecodeIndex;
        uint64 serial = 0;
        const VmLinkedModule* module = nullptr;
        uint32 function = NoBytecodeIndex;
    };

    Array<Cancel> cancels(*allocator_);
    Array<Restart> restarts(*allocator_);
    Retired retiring(*allocator_);
    retiring.link = old;
    retiring.successor = fresh;
    if (!cancels.TryReserve(suspended_.Size()) || !restarts.TryReserve(suspended_.Size()) ||
        !retiring.classMap.TryAppend(plan.classMap_.View()) ||
        !retiring.fieldMap.TryAppend(plan.fieldMap_.View()) ||
        !retired_.TryReserve(retired_.Size() + 1)) {
        return Outcome::Failure(ErrorCode::OutOfMemory);
    }

    // revision 상한(SL §12.4): fresh, old(남는 실행이 있으면), old 앞의 옛 revision. 넘으면 가장
    // 오래된 revision을 frame에 가진 실행을 취소한다.
    const VmLinkedModule* older[MaxRevisions] = {};
    uint32 olderCount = 0;
    for (const VmLinkedModule* link = old; olderCount < MaxRevisions;) {
        const VmLinkedModule* previous = nullptr;
        for (const Retired& entry : retired_) {
            previous = entry.successor == link ? entry.link : previous;
        }

        if (previous == nullptr) {
            break;
        }

        older[olderCount++] = previous; // 새것 → 옛것 순
        link = previous;
    }

    // 실행별 처리(K6): Cancel이면 새 revision과 본문이 다른(삭제된) 함수를 frame에 가진 실행을
    // 취소한다 — old의 frame은 계획의 대응표로, 앞서 Keep으로 남은 더 오래된 revision의 frame은
    // 새 revision의 같은 이름 함수와 본문 hash를 직접 비교한다.
    const BytecodeModule& after = fresh->Module();
    auto policyCancels = [&](const Suspended& entry) noexcept {
        if (plan.executions_ != VmReloadExecutions::Cancel) {
            return false;
        }

        for (const VmExecution::Frame& frame : entry.frames) {
            if (frame.link == old) {
                if (frame.function < plan.changed_.Size() && plan.changed_[frame.function] != 0) {
                    return true;
                }

                continue;
            }

            for (uint32 at = 0; at < olderCount; ++at) {
                if (frame.link != older[at]) {
                    continue;
                }

                const BytecodeModule& code = frame.link->Module();
                const BytecodeFunction& was = code.functions[frame.function];
                const uint32 match = VmFindFunction(after, code.String(was.name));
                bool changed = match == NoBytecodeIndex;
                for (usize byte = 0; !changed && byte < BytecodeHashBytes; ++byte) {
                    changed = was.bodyHash[byte] != after.functions[match].bodyHash[byte];
                }

                if (changed) {
                    return true;
                }
            }
        }

        return false;
    };

    bool oldStays = false;
    for (const Suspended& entry : suspended_) {
        if (!entry.live || policyCancels(entry)) {
            continue;
        }

        for (const VmExecution::Frame& frame : entry.frames) {
            oldStays = oldStays || frame.link == old;
        }
    }

    // 정리 뒤 old 앞의 revision은 많아야 MaxRevisions - 1개라 넘치는 것은 그 앞쪽뿐이다.
    const uint32 live = 1 + (oldStays ? 1 : 0) + olderCount;
    const uint32 expire = live > MaxRevisions ? live - MaxRevisions : 0;
    auto expired = [&](const Suspended& entry) noexcept {
        for (const VmExecution::Frame& frame : entry.frames) {
            for (uint32 at = olderCount - expire; expire <= olderCount && at < olderCount; ++at) {
                if (frame.link == older[at]) {
                    return true;
                }
            }
        }

        return false;
    };
    for (uint32 at = 0; at < suspended_.Size(); ++at) {
        const Suspended& entry = suspended_[at];
        if (!entry.live || (!policyCancels(entry) && !expired(entry))) {
            continue;
        }

        Cancel cancel;
        cancel.suspended = at;
        cancel.wait = entry.wait;
        static_cast<void>(cancels.TryPushBack(cancel)); // 예약함
        // 맨 아래 frame이 Start hook인 루트 실행이면 적용 뒤 그 instance의 Start를 다시 부른다
        // (지금 revision의 같은 클래스 Start).
        if (entry.frames.IsEmpty() || entry.childWait != 0 || entry.instance >= instances_.Size() ||
            !instances_[entry.instance].attached) {
            continue;
        }

        const VmExecution::Frame& bottom = entry.frames[0];
        const BytecodeModule& code = bottom.link->Module();
        uint32 owner = NoBytecodeIndex;
        for (uint32 index = 0; index < code.classes.Size() && owner == NoBytecodeIndex; ++index) {
            owner =
                VmHookOf(code, index, BytecodeHookKind::Start) == bottom.function ? index : owner;
        }

        const VmLinkedModule* link = bottom.link;
        for (uint32 hop = 0; owner != NoBytecodeIndex && hop <= MaxRevisions; ++hop) {
            if (link == old) {
                owner = plan.classMap_[owner];
                link = fresh;
                continue;
            }

            const Retired* step = nullptr;
            for (const Retired& candidate : retired_) {
                step = candidate.link == link ? &candidate : step;
            }

            if (step == nullptr) {
                break;
            }

            owner = owner < step->classMap.Size() ? step->classMap[owner] : NoBytecodeIndex;
            link = step->successor;
        }

        const uint32 function = owner == NoBytecodeIndex
                                    ? NoBytecodeIndex
                                    : VmHookOf(link->Module(), owner, BytecodeHookKind::Start);
        if (function != NoBytecodeIndex) {
            Restart restart;
            restart.instance = entry.instance;
            restart.serial = instances_[entry.instance].serial;
            restart.module = link;
            restart.function = function;
            static_cast<void>(restarts.TryPushBack(restart)); // 예약함
        }
    }

    // ---- 여기부터 바꾼다(실패 없음) ----
    VmReloadReport report;
    report.classes = plan.classes_;
    // (1) 취소(취소가 이어 취소한 실행은 건너뛴다).
    for (const Cancel& cancel : cancels) {
        if (suspended_[cancel.suspended].live && suspended_[cancel.suspended].wait == cancel.wait) {
            CancelSuspended(cancel.suspended);
            ++report.cancelled;
        }
    }

    // (2) instance의 module·class 번호 교체(R1 — 같은 배치라 저장소는 그대로), Tick 중지 해제.
    for (uint32 instance = 0; instance < instances_.Size(); ++instance) {
        Instance& entry = instances_[instance];
        if (!entry.attached) {
            continue;
        }

        const VmLinkedModule* chainModules[MaxBaseDepth] = {};
        uint32 chainClasses[MaxBaseDepth] = {};
        const uint32 depth = Chain(instance, chainModules, chainClasses);
        bool touched = false;
        for (uint32 at = 0; at < depth; ++at) {
            if (chainModules[at] == old) {
                touched = true;
                report.moved += before.classes[chainClasses[at]].fieldCount;
            }
        }

        report.instances += touched ? 1 : 0;
        if (entry.module == old) {
            entry.module = fresh;
            entry.classIndex = plan.classMap_[entry.classIndex];
        }

        if (entry.ticksStopped && entry.tickStopModule == old) {
            entry.ticksStopped = false; // 그 모듈의 새 revision(SL 8.3절)
            entry.tickStopModule = nullptr;
        }
    }

    // (3) 연결의 module·handler·key 교체(순서 유지 — K8).
    usize kept = 0;
    for (usize at = 0; at < connections_.Size(); ++at) {
        Connection connection = connections_[at];
        if (connection.module == old) {
            const uint32 handler = plan.handlerMap_[connection.handler];
            if (handler == NoBytecodeIndex) {
                continue; // 삭제된 handler의 연결(R2)
            }

            connection.module = fresh;
            connection.handler = handler;
            connection.key = HandlerKey(*fresh, handler);
        }

        connections_[kept++] = connection;
    }

    static_cast<void>(connections_.TryResize(kept));
    // (4) 한 handler에만 갈 큐 항목, 정지 실행의 시작 handler(재진입 판정)를 새 번호로.
    for (usize at = queueHead_; at < queue_.Size(); ++at) {
        Item& item = queue_[at];
        if (item.targetModule == old) {
            item.targetHandler = plan.handlerMap_[item.targetHandler];
            item.targetModule = item.targetHandler == NoBytecodeIndex ? nullptr : fresh;
            item.target = item.targetHandler == NoBytecodeIndex ? NoBytecodeIndex : item.target;
        }
    }

    for (Suspended& entry : suspended_) {
        if (entry.live && entry.handlerModule == old) {
            entry.handler = plan.handlerMap_[entry.handler];
            entry.handlerModule = entry.handler == NoBytecodeIndex ? nullptr : fresh;
        }
    }

    // (5) session 모듈 목록 교체와 이전 revision 기록(Keep 실행의 필드 번역).
    modules_[slot] = fresh;
    static_cast<void>(retired_.TryPushBack(Move(retiring))); // 예약함
    for (const Suspended& entry : suspended_) {
        bool previous = false;
        for (const VmExecution::Frame& frame : entry.frames) {
            for (const Retired& retired : retired_) {
                previous = previous || (entry.live && frame.link == retired.link);
            }
        }

        report.previous += previous ? 1 : 0;
    }

    PruneRetired();
    plan.old_ = nullptr;
    plan.fresh_ = nullptr;
    // (6) 취소된 Start 다시 부르기(instance 생성 순 — 적용 기록 `  hook` 줄).
    for (usize at = 1; at < restarts.Size(); ++at) {
        for (usize back = at; back > 0 && restarts[back - 1].serial > restarts[back].serial;
             --back) {
            const Restart swap = restarts[back - 1];
            restarts[back - 1] = restarts[back];
            restarts[back] = swap;
        }
    }

    BeginEntry();
    struct Nested {
        VmSession& session;
        ~Nested() noexcept {
            session.nested_ = false;
            session.execution_.keepLogs_ = false;
        }
    } nested{*this};
    nested_ = true;
    execution_.keepLogs_ = true;
    for (const Restart& restart : restarts) {
        const Instance& entry = instances_[restart.instance];
        if (!entry.attached || entry.serial != restart.serial || entry.isolated ||
            entry.pendingDestroy) {
            continue;
        }

        // 할당 실패는 남은 Start를 부르지 않고 알린다(reload 자체는 적용됐다).
        if (auto ran = ApplyHook(restart.instance, BytecodeHookKind::Start, restart.module,
                                 restart.function);
            !ran) {
            return Outcome::Failure(ran.Error());
        }

        ++report.restarted;
    }

    return Outcome::Success(report);
}

} // namespace rocks::sparks
