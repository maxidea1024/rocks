#include <Core/Move.h>
#include <SparksVm/Opcode.h>
#include <SparksVm/VmSession.h>

// hot reload의 구현이다(Docs/Design/SparksReload.ko.md — LC7a·LC7b): 새 revision 계획·적용(필드
// migration·구독 연결 재구성 — R2), 이전 revision 코드(Keep 실행)의 필드 위치 번역, revision 참조
// 판정.

namespace rocks::sparks {

namespace {

// 옛 필드의 운명이다(계획의 fate_ — SparksReload 5절).
constexpr uint8 VmFieldRemoved = 0; // 어느 새 필드의 원천도 아니다
constexpr uint8 VmFieldMoved = 1;   // 타입·형태·배치가 같은 새 필드로 옮긴다
constexpr uint8 VmFieldLost = 2;    // 새 필드의 원천이지만 형식이 바뀌었다(새 기본값)

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

// 두 handler가 같은 함수 이름·경로·이벤트인지다(재진입 정책은 전달 때 새 기록을 읽는다 — 5절).
bool VmSameHandler(const BytecodeModule& left, const BytecodeHandler& a,
                   const BytecodeModule& right, const BytecodeHandler& b) noexcept {
    return left.String(left.functions[a.function].name) ==
               right.String(right.functions[b.function].name) &&
           VmText(left, a.path) == VmText(right, b.path) &&
           VmText(left, a.event) == VmText(right, b.event);
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

// 쓰기 bit 묶음의 index번 bit다(범위 밖은 false).
bool VmBitOf(const Array<uint64>& bits, usize index) noexcept {
    return index / VmWrittenWordBits < bits.Size() &&
           ((bits[index / VmWrittenWordBits] >> (index % VmWrittenWordBits)) & 1) != 0;
}

void VmSetBit(Array<uint64>& bits, usize index) noexcept {
    if (index / VmWrittenWordBits < bits.Size()) {
        bits[index / VmWrittenWordBits] |= uint64(1) << (index % VmWrittenWordBits);
    }
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
    plan.sources_.Clear();
    plan.fate_.Clear();
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

    // 아직 참조되는 이전 revision 객체로 되돌리면 이전 revision 기록이 순환한다(LC7a 리뷰 3).
    PruneRetired();
    bool retiredFresh = false;
    for (const Retired& entry : retired_) {
        retiredFresh = retiredFresh || entry.link == &fresh || entry.successor == &fresh;
    }

    if (retiredFresh || Uses(&fresh)) {
        return VmReject(reason, "the new revision is still referenced by the session");
    }

    const BytecodeModule& before = old.Module();
    const BytecodeModule& after = fresh.Module();
    if (VmText(before, before.path) != VmText(after, after.path)) {
        return VmReject(reason, "the new revision has another module path");
    }

    if (!plan.classMap_.TryResize(before.classes.Size()) ||
        !plan.fieldMap_.TryResize(before.classFields.Size()) ||
        !plan.handlerMap_.TryResize(before.handlers.Size()) ||
        !plan.changed_.TryResize(before.functions.Size()) ||
        !plan.sources_.TryResize(after.classFields.Size()) ||
        !plan.fate_.TryResize(before.classFields.Size())) {
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

    for (uint32& value : plan.sources_) {
        value = NoBytecodeIndex;
    }

    for (uint8& value : plan.fate_) {
        value = VmFieldRemoved;
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
        // 필드 대응(K4·5절): 새 필드마다 같은 이름의 옛 필드를 먼저, 그다음 아직 원천이 아닌 옛
        // 필드 중 `@formerly` 옛 이름과 같은 것. 타입·형태 표기와 bank·slot이 같으면 옮김, 다르면
        // 손실이다. 어느 새 필드의 원천도 아닌 옛 필드는 삭제다.
        const BytecodeClass& newEntry = after.classes[newClass];
        const uint32 oldEnd = oldEntry.firstField + oldEntry.fieldCount;
        const uint32 newEnd = newEntry.firstField + newEntry.fieldCount;
        for (uint32 field = newEntry.firstField; field < newEnd; ++field) {
            for (uint32 candidate = oldEntry.firstField; candidate < oldEnd; ++candidate) {
                if (before.String(before.classFields[candidate].name) ==
                    after.String(after.classFields[field].name)) {
                    plan.sources_[field] = candidate;
                    break;
                }
            }
        }

        auto sourced = [&](uint32 candidate) noexcept {
            for (uint32 field = newEntry.firstField; field < newEnd; ++field) {
                if (plan.sources_[field] == candidate) {
                    return true;
                }
            }

            return false;
        };
        for (uint32 field = newEntry.firstField; field < newEnd; ++field) {
            const uint32 former = after.classFields[field].formerName;
            if (plan.sources_[field] != NoBytecodeIndex || former == NoBytecodeIndex) {
                continue;
            }

            for (uint32 candidate = oldEntry.firstField; candidate < oldEnd; ++candidate) {
                if (!sourced(candidate) &&
                    before.String(before.classFields[candidate].name) == after.String(former)) {
                    plan.sources_[field] = candidate;
                    break;
                }
            }
        }

        for (uint32 field = newEntry.firstField; field < newEnd; ++field) {
            const uint32 source = plan.sources_[field];
            if (source == NoBytecodeIndex) {
                continue;
            }

            const BytecodeTypeField& was = before.classFields[source];
            const BytecodeTypeField& now = after.classFields[field];
            const bool same = VmText(after, now.type) == VmText(before, was.type) &&
                              VmText(after, now.shape) == VmText(before, was.shape) &&
                              now.bank == was.bank && now.slots == was.slots;
            plan.fate_[source] = same ? VmFieldMoved : VmFieldLost;
            plan.fieldMap_[source] = same ? field : NoBytecodeIndex;
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
    plan.serial_ = nextSerial_;
    return Result<void>::Success();
}

bool VmSession::Uses(const VmLinkedModule* link) const noexcept {
    for (const VmLinkedModule* existing : modules_) {
        if (existing == link) {
            return true;
        }
    }

    for (const Instance& instance : instances_) {
        if (instance.attached && (instance.module == link || instance.tickStopModule == link)) {
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

bool VmSession::TranslateStep(const VmLinkedModule* successor, Span<const uint32> classMap,
                              Span<const uint32> fieldMap, const VmLinkedModule*& module,
                              uint32& classIndex, uint32 bank, uint32 slots,
                              uint32& offset) const noexcept {
    const BytecodeModule& before = module->Module();
    if (classIndex >= classMap.Size() || classMap[classIndex] == NoBytecodeIndex) {
        return false;
    }

    const BytecodeClass& owner = before.classes[classIndex];
    for (uint32 field = owner.firstField; field < owner.firstField + owner.fieldCount; ++field) {
        const BytecodeTypeField& was = before.classFields[field];
        if (was.bank != bank || was.offset != offset) {
            continue;
        }

        const uint32 mapped = field < fieldMap.Size() ? fieldMap[field] : NoBytecodeIndex;
        if (mapped == NoBytecodeIndex) {
            return false;
        }

        const BytecodeTypeField& now = successor->Module().classFields[mapped];
        if (now.bank != bank || now.slots != slots) {
            return false;
        }

        module = successor;
        classIndex = classMap[classIndex];
        offset = now.offset;
        return true;
    }

    return false;
}

bool VmSession::TranslateField(const VmLinkedModule*& module, uint32& classIndex, uint32 bank,
                               uint32 slots, uint32& offset) const noexcept {
    for (const Retired& entry : retired_) {
        if (entry.link == module) {
            return TranslateStep(entry.successor, entry.classMap.View(), entry.fieldMap.View(),
                                 module, classIndex, bank, slots, offset);
        }
    }

    return false;
}

bool VmSession::FieldsReach(const VmLinkedModule& link, uint32 function,
                            const VmReloadPlan& plan) const noexcept {
    // 필드 명령(GETF·SETF)의 Field 참조가 가리키는 로컬 필드마다 이전 revision 기록과 이번 대응(old
    // → fresh)으로 새 revision까지 옮겨지는지 본다(SparksReload 5절 — 옮기지 못하면 그 실행은
    // 취소).
    const BytecodeModule& code = link.Module();
    const BytecodeFunction& entry = code.functions[function];
    const uint32 end = entry.firstWord + entry.wordCount;
    for (uint32 at = entry.firstWord; at < end;) {
        const Opcode op = DecodeOpcode(code.words[at]);
        const bool extra = InfoOf(op).extra;
        const uint32 ref = extra && at + 1 < end ? DecodeAx(code.words[at + 1]) : NoBytecodeIndex;
        at += extra ? 2 : 1;
        if (op < Opcode::GetFS || op > Opcode::SetFB || ref >= link.localFields_.Size() ||
            link.localFields_[ref].classIndex == NoBytecodeIndex) {
            continue;
        }

        const VmLinkedModule::LocalField& local = link.localFields_[ref];
        const VmLinkedModule* module = &link;
        uint32 classIndex = local.classIndex;
        uint32 offset = local.offset;
        for (uint32 hop = 0; module != plan.fresh_; ++hop) {
            bool moved = false;
            if (hop <= MaxRevisions && module == plan.old_) {
                moved = TranslateStep(plan.fresh_, plan.classMap_.View(), plan.fieldMap_.View(),
                                      module, classIndex, local.bank, local.slots, offset);
            } else if (hop <= MaxRevisions) {
                moved = TranslateField(module, classIndex, local.bank, local.slots, offset);
            }

            if (!moved) {
                return false;
            }
        }
    }

    return true;
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
    // 조각 밖 경계에는 쓰기 기록이 없다(저장소 위치를 바꾸므로 남아 있으면 적용하지 않는다).
    if (closed_ || execution_.running_ || old == nullptr || fresh == nullptr ||
        !writes_.IsEmpty()) {
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

    // 계획 뒤에 instance가 생겼으면 낡은 계획이다(판정은 계획 때의 instance로 했다 — 필드·handler
    // 변경을 허용한 클래스에 새 instance가 있을 수 있다, LC7a 리뷰 2). 아무것도 바꾸지 않는다.
    if (plan.serial_ != nextSerial_) {
        return Outcome::Failure(ErrorCode::InvalidArgument);
    }

    PruneRetired();
    const BytecodeModule& before = old->Module();
    const BytecodeModule& after = fresh->Module();
    // 할당은 바꾸기 전에 모두 마친다(SparksReload K3 — 실패면 session 그대로).
    struct Cancel {
        uint32 suspended = NoBytecodeIndex;
        uint64 wait = 0;
    };

    struct Restart {
        uint32 suspended = NoBytecodeIndex; // 후보 루트 실행(취소됐을 때만 다시 부른다)
        uint64 wait = 0;
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
    // 설정과 무관한 취소(SL 12.4 — 5절): 삭제된 handler로 시작된 실행, 새 revision으로 옮기지
    // 못하는 (손실·삭제) 필드를 필드 명령으로 참조하는 함수를 old 또는 이전 revision frame에 가진
    // 실행.
    auto forcedCancels = [&](const Suspended& entry) noexcept {
        if (entry.handlerModule == old && entry.handler < plan.handlerMap_.Size() &&
            plan.handlerMap_[entry.handler] == NoBytecodeIndex) {
            return true;
        }

        for (const VmExecution::Frame& frame : entry.frames) {
            bool revision = frame.link == old;
            for (uint32 at = 0; at < olderCount; ++at) {
                revision = revision || frame.link == older[at];
            }

            if (revision && !FieldsReach(*frame.link, frame.function, plan)) {
                return true;
            }
        }

        return false;
    };
    auto mustCancel = [&](const Suspended& entry) noexcept {
        return policyCancels(entry) || forcedCancels(entry);
    };

    // 큐 항목(key 글자)도 old를 붙잡는다(전달되면 놓는다 — 만료 취소의 대상은 아니다).
    bool oldStays = false;
    for (usize at = queueHead_; at < queue_.Size(); ++at) {
        oldStays = oldStays || queue_[at].module == old;
    }

    for (const Suspended& entry : suspended_) {
        if (!entry.live || mustCancel(entry)) {
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
        if (!entry.live || (!mustCancel(entry) && !expired(entry))) {
            continue;
        }

        Cancel cancel;
        cancel.suspended = at;
        cancel.wait = entry.wait;
        static_cast<void>(cancels.TryPushBack(cancel)); // 예약함
    }

    // 맨 아래 frame이 Start hook인 루트 실행은 모두 후보다: 취소된 뒤(직접이든 하위 실행 취소가
    // 이어서든 — LC7a 리뷰 1) 그 instance의 Start를 지금 revision의 같은 클래스 Start로 다시
    // 부른다.
    for (uint32 at = 0; at < suspended_.Size(); ++at) {
        const Suspended& entry = suspended_[at];
        if (!entry.live || entry.frames.IsEmpty() || entry.childWait != 0 ||
            entry.instance >= instances_.Size() || !instances_[entry.instance].attached) {
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
            restart.suspended = at;
            restart.wait = entry.wait;
            restart.instance = entry.instance;
            restart.serial = instances_[entry.instance].serial;
            restart.module = link;
            restart.function = function;
            static_cast<void>(restarts.TryPushBack(restart)); // 예약함
        }
    }

    // ---- R2 준비(5절): 기본값 레코드, instance마다 새 저장소, 연결 표 사본 ----
    // 새 클래스 하나의 기본값(자기 구역만 — 필드 초기값은 상수식이라 instance마다 같다).
    struct Defaults {
        explicit Defaults(Allocator& allocator) noexcept
            : scalars(allocator), refs(allocator), buffers(allocator) {
        }

        uint32 classIndex = NoBytecodeIndex;
        Array<uint64> scalars;
        Array<uint64> refs;
        Array<VmBuffer*> buffers;
    };

    // 영향 instance 하나다. 임시 반영 뒤 배열은 옛 저장소를 쥔다(되돌리면 다시 새 저장소).
    struct Moving {
        explicit Moving(Allocator& allocator) noexcept
            : scalars(allocator), refs(allocator), buffers(allocator), written(allocator) {
        }

        uint32 instance = NoBytecodeIndex;
        const VmLinkedModule* module = nullptr; // 반영할 module·class(교체로 옛 값과 맞바꾼다)
        uint32 classIndex = NoBytecodeIndex;
        uint32 depth = 0;
        const VmLinkedModule* modules[MaxBaseDepth] = {};
        uint32 classes[MaxBaseDepth] = {};
        uint32 refStart[MaxBaseDepth] = {}; // 옛 저장소에서 사슬 원소의 Ref 구역 시작
        Array<uint64> scalars;
        Array<uint64> refs;
        Array<VmBuffer*> buffers;
        Array<uint64> written;
    };

    Array<Defaults> defaults(*allocator_);
    Array<Moving> moving(*allocator_);
    Array<Connection> savedConnections(*allocator_);
    // 준비·되돌리기의 정리: 새 저장소와 기본값 레코드가 쥔 Buf 참조를 놓는다.
    auto discard = [&]() noexcept {
        for (Defaults& entry : defaults) {
            for (VmBuffer*& buffer : entry.buffers) {
                execution_.Release(buffer);
                buffer = nullptr;
            }
        }

        for (Moving& entry : moving) {
            for (VmBuffer*& buffer : entry.buffers) {
                execution_.Release(buffer);
                buffer = nullptr;
            }
        }
    };
    if (!savedConnections.TryAppend(connections_.View())) {
        return Outcome::Failure(ErrorCode::OutOfMemory);
    }

    // 새 클래스의 기본값 레코드를 만든다(새 `$init`을 그 클래스 임시 저장소로 — 5절). 실패는
    // 할당자 오류, fault는 기본값 레코드 재합성 실패라 InvalidArgument다.
    auto defaultsOf = [&](uint32 classIndex, uint64 handle) noexcept -> Result<const Defaults*> {
        for (const Defaults& entry : defaults) {
            if (entry.classIndex == classIndex) {
                return Result<const Defaults*>::Success(&entry);
            }
        }

        const BytecodeClass& entry = after.classes[classIndex];
        Defaults made(*allocator_);
        made.classIndex = classIndex;
        if (!made.scalars.TryResize(entry.slots[0]) || !made.refs.TryResize(entry.slots[1]) ||
            !made.buffers.TryResize(entry.slots[2]) || !defaults.TryPushBack(Move(made))) {
            return Result<const Defaults*>::Failure(ErrorCode::OutOfMemory);
        }

        Defaults& stored = defaults[defaults.Size() - 1];
        for (uint64& value : stored.scalars) {
            value = 0;
        }

        for (uint64& value : stored.refs) {
            value = 0;
        }

        for (VmBuffer*& value : stored.buffers) {
            value = nullptr;
        }

        if (entry.initializer == NoBytecodeIndex) {
            return Result<const Defaults*>::Success(&stored);
        }

        staging_.module = fresh;
        staging_.classIndex = classIndex;
        staging_.scalars = stored.scalars.Data();
        staging_.refs = stored.refs.Data();
        staging_.buffers = stored.buffers.Data();
        for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
            staging_.sizes[bank] = entry.slots[bank];
        }

        const uint64 self[] = {handle};
        auto called = execution_.Call(*fresh, entry.initializer, {}, Span<const uint64>(self, 1),
                                      {}, FragmentLimits(false));
        staging_ = Staging();
        if (!called) {
            return Result<const Defaults*>::Failure(called.Error());
        }

        const VmOutcome outcome = called.Value().outcome;
        execution_.ReleaseReturned();
        if (outcome != VmOutcome::Returned) {
            return Result<const Defaults*>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<const Defaults*>::Success(&stored);
    };
    uint32 moved = 0;
    uint32 defaulted = 0;
    uint32 lost = 0;
    uint32 removed = 0;
    for (uint32 instance = 0; instance < instances_.Size(); ++instance) {
        const Instance& entry = instances_[instance];
        if (!entry.attached) {
            continue;
        }

        const VmLinkedModule* chainModules[MaxBaseDepth] = {};
        uint32 chainClasses[MaxBaseDepth] = {};
        const uint32 depth = Chain(instance, chainModules, chainClasses);
        bool touched = false;
        for (uint32 at = 0; at < depth; ++at) {
            touched = touched || chainModules[at] == old;
        }

        if (!touched) {
            continue;
        }

        // 새 사슬(old 클래스 → 대응 새 클래스)의 구역 시작: 기반부터 bank별로 쌓는다.
        uint32 oldStart[MaxBaseDepth][BytecodeBankCount] = {};
        uint32 newStart[MaxBaseDepth][BytecodeBankCount] = {};
        uint32 oldTotal[BytecodeBankCount] = {};
        uint32 newTotal[BytecodeBankCount] = {};
        for (uint32 at = depth; at > 0; --at) {
            const uint32 index = at - 1;
            const BytecodeClass& was = chainModules[index]->Module().classes[chainClasses[index]];
            const bool mapped = chainModules[index] == old;
            const uint32 mappedClass =
                mapped ? plan.classMap_[chainClasses[index]] : NoBytecodeIndex;
            if (mapped && mappedClass == NoBytecodeIndex) {
                discard();
                return Outcome::Failure(ErrorCode::InvalidArgument); // 계획이 거부했어야 한다
            }

            const BytecodeClass& now = mapped ? after.classes[mappedClass] : was;
            for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                oldStart[index][bank] = oldTotal[bank];
                newStart[index][bank] = newTotal[bank];
                oldTotal[bank] += was.slots[bank];
                newTotal[bank] += now.slots[bank];
            }
        }

        if (oldTotal[0] != entry.scalars.Size() || oldTotal[1] != entry.refs.Size() ||
            oldTotal[2] != entry.buffers.Size()) {
            discard();
            return Outcome::Failure(ErrorCode::InvalidArgument);
        }

        // 새 클래스의 기본값 레코드(사슬에서 처음 만나는 instance의 handle로 `$init`).
        const Defaults* chainDefaults[MaxBaseDepth] = {};
        for (uint32 at = 0; at < depth; ++at) {
            if (chainModules[at] != old) {
                continue;
            }

            auto made = defaultsOf(plan.classMap_[chainClasses[at]], entry.handle);
            if (!made) {
                discard();
                return Outcome::Failure(made.Error());
            }

            chainDefaults[at] = made.Value();
        }

        // defaults가 자라 앞서 받은 포인터가 옮겨졌을 수 있다: 번호로 다시 찾는다.
        for (uint32 at = 0; at < depth; ++at) {
            for (const Defaults& candidate : defaults) {
                if (chainModules[at] == old &&
                    candidate.classIndex == plan.classMap_[chainClasses[at]]) {
                    chainDefaults[at] = &candidate;
                }
            }
        }

        const usize totalSlots = usize(newTotal[0]) + newTotal[1] + newTotal[2];
        Moving made(*allocator_);
        made.instance = instance;
        made.module = entry.module == old ? fresh : entry.module;
        made.classIndex = entry.module == old ? plan.classMap_[entry.classIndex] : entry.classIndex;
        made.depth = depth;
        if (!made.scalars.TryResize(newTotal[0]) || !made.refs.TryResize(newTotal[1]) ||
            !made.buffers.TryResize(newTotal[2]) ||
            !made.written.TryResize((totalSlots + VmWrittenWordBits - 1) / VmWrittenWordBits) ||
            !moving.TryPushBack(Move(made))) {
            discard();
            return Outcome::Failure(ErrorCode::OutOfMemory);
        }

        // ---- 채우기(실패 없음): Buf는 새 저장소가 자기 참조를 쥔다 ----
        Moving& target = moving[moving.Size() - 1];
        for (uint64& value : target.scalars) {
            value = 0;
        }

        for (uint64& value : target.refs) {
            value = 0;
        }

        for (VmBuffer*& value : target.buffers) {
            value = nullptr;
        }

        for (uint64& value : target.written) {
            value = 0;
        }

        const usize oldBase[BytecodeBankCount] = {0, oldTotal[0], usize(oldTotal[0]) + oldTotal[1]};
        const usize newBase[BytecodeBankCount] = {0, newTotal[0], usize(newTotal[0]) + newTotal[1]};
        auto copySlot = [&](uint32 bank, usize from, usize to) noexcept {
            if (bank == 2) {
                VmBuffer* buffer = entry.buffers[from];
                execution_.Retain(buffer);
                target.buffers[to] = buffer;
            } else {
                (bank == 0 ? target.scalars : target.refs)[to] =
                    (bank == 0 ? entry.scalars : entry.refs)[from];
            }

            if (VmBitOf(entry.written, oldBase[bank] + from)) {
                VmSetBit(target.written, newBase[bank] + to);
            }
        };
        auto defaultSlot = [&](const Defaults& source, uint32 bank, usize from, usize to) noexcept {
            if (bank == 2) {
                VmBuffer* buffer = source.buffers[from];
                execution_.Retain(buffer);
                target.buffers[to] = buffer;
            } else {
                (bank == 0 ? target.scalars : target.refs)[to] =
                    (bank == 0 ? source.scalars : source.refs)[from];
            }
        };
        for (uint32 at = 0; at < depth; ++at) {
            target.modules[at] = chainModules[at];
            target.classes[at] = chainClasses[at];
            target.refStart[at] = oldStart[at][1];
            const BytecodeClass& was = chainModules[at]->Module().classes[chainClasses[at]];
            if (chainModules[at] != old) {
                // old 밖 구역(다른 모듈 기반·파생)은 그대로 옮긴다(위치만 밀린다).
                for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                    for (uint32 index = 0; index < was.slots[bank]; ++index) {
                        copySlot(bank, usize(oldStart[at][bank]) + index,
                                 usize(newStart[at][bank]) + index);
                    }
                }

                continue;
            }

            const BytecodeClass& now = after.classes[plan.classMap_[chainClasses[at]]];
            for (uint32 field = now.firstField; field < now.firstField + now.fieldCount; ++field) {
                const BytecodeTypeField& declared = after.classFields[field];
                const uint32 source = plan.sources_[field];
                const usize to = usize(newStart[at][declared.bank]) + declared.offset;
                if (source != NoBytecodeIndex && plan.fieldMap_[source] == field) {
                    // 옮김: 유지 자원은 그대로, 쓴 필드는 옛 값, 쓰지 않은 필드는 새 기본값(K5).
                    const BytecodeTypeField& previous = before.classFields[source];
                    const usize from = usize(oldStart[at][previous.bank]) + previous.offset;
                    if ((declared.flags & BytecodeFieldHeld) != 0 ||
                        VmBitOf(entry.written, oldBase[previous.bank] + from)) {
                        for (uint32 index = 0; index < declared.slots; ++index) {
                            copySlot(declared.bank, from + index, to + index);
                        }

                        ++moved;
                        continue;
                    }

                    ++defaulted;
                } else if (source != NoBytecodeIndex) {
                    ++lost; // 형식이 바뀌었다(무손실 변환은 EI — 새 기본값)
                } else {
                    ++defaulted; // 새 필드
                }

                for (uint32 index = 0; index < declared.slots; ++index) {
                    defaultSlot(*chainDefaults[at], declared.bank, declared.offset + index,
                                to + index);
                }
            }

            for (uint32 field = was.firstField; field < was.firstField + was.fieldCount; ++field) {
                removed += plan.fate_[field] == VmFieldRemoved ? 1 : 0;
            }
        }
    }

    // ---- 임시 반영: 저장소·module·class 맞바꾸기, session 모듈, 연결 교체 ----
    auto swapStorage = [&](Moving& entry) noexcept {
        Instance& instance = instances_[entry.instance];
        Array<uint64> scalars = Move(instance.scalars);
        instance.scalars = Move(entry.scalars);
        entry.scalars = Move(scalars);
        Array<uint64> refs = Move(instance.refs);
        instance.refs = Move(entry.refs);
        entry.refs = Move(refs);
        Array<VmBuffer*> buffers = Move(instance.buffers);
        instance.buffers = Move(entry.buffers);
        entry.buffers = Move(buffers);
        Array<uint64> written = Move(instance.written);
        instance.written = Move(entry.written);
        entry.written = Move(written);
        const VmLinkedModule* module = instance.module;
        instance.module = entry.module;
        entry.module = module;
        const uint32 classIndex = instance.classIndex;
        instance.classIndex = entry.classIndex;
        entry.classIndex = classIndex;
    };
    for (Moving& entry : moving) {
        swapStorage(entry);
    }

    modules_[slot] = fresh;
    // 연결의 module·handler·key 교체(순서 유지 — K8), 삭제된 handler의 연결은 지운다.
    usize kept = 0;
    for (usize at = 0; at < connections_.Size(); ++at) {
        Connection connection = connections_[at];
        if (connection.module == old) {
            const uint32 handler = plan.handlerMap_[connection.handler];
            if (handler == NoBytecodeIndex) {
                continue;
            }

            connection.module = fresh;
            connection.handler = handler;
            connection.key = HandlerKey(*fresh, handler);
        }

        connections_[kept++] = connection;
    }

    static_cast<void>(connections_.TryResize(kept));
    // ---- 연결 다시 풀기: 남는 발행자는 순서 유지, 새 `on`·새 발행자는 끝(K8) ----
    const uint64 dropped = droppedConnections_;
    Result<void> connected = Result<void>::Success();
    for (usize at = 0; at < moving.Size() && connected; ++at) {
        const Instance& instance = instances_[moving[at].instance];
        if (instance.enabled && !instance.isolated) {
            connected = Connect(moving[at].instance, nullptr, NoBytecodeIndex);
        }
    }

    if (!connected) {
        // 되돌리기: 연결 표(capacity는 사본 이상)·거부 수·session 모듈·저장소를 원래대로.
        connections_.Clear();
        static_cast<void>(connections_.TryAppend(savedConnections.View()));
        droppedConnections_ = dropped;
        modules_[slot] = old;
        for (Moving& entry : moving) {
            swapStorage(entry);
        }

        discard();
        return Outcome::Failure(connected.Error());
    }

    // ---- 확정(실패 없음) ----
    VmReloadReport report;
    report.classes = plan.classes_;
    report.instances = static_cast<uint32>(moving.Size());
    report.moved = moved;
    report.defaulted = defaulted;
    report.lost = lost;
    report.removed = removed;
    // 옮기지 않은 유지 자원 필드(손실·삭제)를 선언 역순(파생 → 기반, 뒤 필드부터)으로 멈추고 옛
    // 저장소의 Buf를 놓는다(옮긴 값은 새 저장소가 자기 참조를 쥐었다).
    for (Moving& entry : moving) {
        for (uint32 at = 0; at < entry.depth; ++at) {
            if (entry.modules[at] != old) {
                continue;
            }

            const BytecodeClass& was = before.classes[entry.classes[at]];
            for (uint32 field = was.firstField + was.fieldCount; field > was.firstField; --field) {
                const BytecodeTypeField& declared = before.classFields[field - 1];
                const usize where = usize(entry.refStart[at]) + declared.offset;
                if ((declared.flags & BytecodeFieldHeld) == 0 ||
                    plan.fate_[field - 1] == VmFieldMoved || where >= entry.refs.Size() ||
                    entry.refs[where] == 0) {
                    continue;
                }

                if (host_ != nullptr) {
                    host_->ReleaseHeld(entry.refs[where]);
                }

                entry.refs[where] = 0;
            }
        }
    }

    discard();
    // Tick 중지 해제(그 모듈의 새 revision — SL 8.3절).
    for (Instance& entry : instances_) {
        if (entry.attached && entry.ticksStopped && entry.tickStopModule == old) {
            entry.ticksStopped = false;
            entry.tickStopModule = nullptr;
        }
    }

    // 취소(취소가 이어 취소한 실행은 건너뛴다). 보고는 이어진 취소까지 센다. 취소는 되돌릴 수
    // 없어 연결 뒤에 한다(instance 저장소를 읽지 않는다 — frame 레지스터·대기만 놓는다).
    const uint32 liveBefore = SuspendedCount();
    for (const Cancel& cancel : cancels) {
        if (suspended_[cancel.suspended].live && suspended_[cancel.suspended].wait == cancel.wait) {
            CancelSuspended(cancel.suspended);
        }
    }

    report.cancelled = liveBefore - SuspendedCount();
    usize restartCount = 0;
    for (const Restart& restart : restarts) {
        const Suspended& entry = suspended_[restart.suspended];
        if (!entry.live || entry.wait != restart.wait) {
            restarts[restartCount++] = restart;
        }
    }

    static_cast<void>(restarts.TryResize(restartCount));
    // 한 handler에만 갈 큐 항목, 정지 실행의 시작 handler(재진입 판정)를 새 번호로.
    for (usize at = queueHead_; at < queue_.Size(); ++at) {
        Item& item = queue_[at];
        if (item.targetModule == old) {
            const uint32 handler = plan.handlerMap_[item.targetHandler];
            if (handler == NoBytecodeIndex) {
                // 대상 handler가 없어졌다: 항목을 버린다(모두에게 전달되지 않게 — LC7a 리뷰 4).
                ReleaseItem(item);
                item.publisher = 0;
                continue;
            }

            item.targetHandler = handler;
            item.targetModule = fresh;
            item.key = HandlerKey(*fresh, handler); // old의 key 글자를 가리키지 않게
        }
    }

    for (Suspended& entry : suspended_) {
        if (entry.live && entry.handlerModule == old) {
            entry.handler = plan.handlerMap_[entry.handler];
            entry.handlerModule = entry.handler == NoBytecodeIndex ? nullptr : fresh;
        }
    }

    // 이전 revision 기록(Keep 실행의 필드 번역).
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
    // 취소된 Start 다시 부르기(instance 생성 순 — 적용 기록 `  hook` 줄).
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

        // 할당 실패는 남은 Start를 부르지 않고 보고에 남긴다(reload 자체는 적용됐다).
        if (auto ran = ApplyHook(restart.instance, BytecodeHookKind::Start, restart.module,
                                 restart.function);
            !ran) {
            report.restartError = ran.Error();
            break;
        }

        ++report.restarted;
    }

    return Outcome::Success(report);
}

} // namespace rocks::sparks
