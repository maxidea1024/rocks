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
    VmReloadModule pair;
    pair.old = &old;
    pair.fresh = &fresh;
    return PlanReload(Span<const VmReloadModule>(&pair, 1), executions, plan, reason);
}

Result<void> VmSession::PlanReload(Span<const VmReloadModule> modules,
                                   VmReloadExecutions executions, VmReloadPlan& plan,
                                   StringBuilderBase& reason) noexcept {
    plan.entries_.Clear();
    if (closed_ || execution_.running_) {
        return VmReject(reason, "the session is closed or running");
    }

    if (modules.IsEmpty()) {
        return VmReject(reason, "the reload set is empty");
    }

    // 아직 참조되는 이전 revision 객체로 되돌리면 이전 revision 기록이 순환한다(LC7a 리뷰 3).
    PruneRetired();
    auto oldOf = [&](const VmLinkedModule* link) noexcept {
        for (const VmReloadModule& pair : modules) {
            if (pair.old == link) {
                return true;
            }
        }

        return false;
    };
    for (usize at = 0; at < modules.Size(); ++at) {
        const VmLinkedModule* old = modules[at].old;
        const VmLinkedModule* fresh = modules[at].fresh;
        if (old == nullptr || fresh == nullptr) {
            return VmReject(reason, "a reload pair is missing a module");
        }

        bool oldKnown = false;
        bool freshKnown = false;
        for (const VmLinkedModule* existing : modules_) {
            oldKnown = oldKnown || existing == old;
            freshKnown = freshKnown || existing == fresh;
        }

        if (!oldKnown) {
            return VmReject(reason, "the old revision is not a session module");
        }

        if (freshKnown || fresh == old || oldOf(fresh)) {
            return VmReject(reason, "the new revision must be a new linked module");
        }

        for (usize before = 0; before < at; ++before) {
            if (modules[before].old == old || modules[before].fresh == fresh) {
                return VmReject(reason, "a module appears twice in the reload set");
            }
        }

        if (!fresh->Linked() || !fresh->ImportsLinked() || fresh->Host() != host_) {
            return VmReject(reason, "the new revision is not linked with the session host");
        }

        bool retiredFresh = false;
        for (const Retired& entry : retired_) {
            retiredFresh = retiredFresh || entry.link == fresh || entry.successor == fresh;
        }

        if (retiredFresh || Uses(fresh)) {
            return VmReject(reason, "the new revision is still referenced by the session");
        }

        const BytecodeModule& before = old->Module();
        const BytecodeModule& after = fresh->Module();
        if (VmText(before, before.path) != VmText(after, after.path)) {
            return VmReject(reason, "the new revision has another module path");
        }

        // 새 revision은 묶음의 다른 새 revision이나 묶음 밖 지금 모듈만 부른다(묶음은 함께
        // 바뀐다 — 6절 LC7c2b, LC7c 리뷰 3: 이전 revision·session 밖 모듈도 아니다).
        for (const VmLinkedModule::Import& import : fresh->imports_) {
            if (import.module == nullptr) {
                continue;
            }

            bool current = false;
            for (const VmLinkedModule* existing : modules_) {
                current = current || (existing == import.module && !oldOf(existing));
            }

            for (const VmReloadModule& other : modules) {
                current = current || other.fresh == import.module;
            }

            if (!current) {
                return VmReject(reason,
                                "the new revision of {} imports an old revision of the "
                                "reload set",
                                VmText(after, after.path));
            }
        }
    }

    // 묶음 밖 session 모듈이 묶음의 옛 revision을 import하면 그 모듈도 묶음에 넣어야 한다(옛
    // revision을 계속 부르게 된다).
    for (const VmLinkedModule* existing : modules_) {
        if (oldOf(existing)) {
            continue;
        }

        for (const VmLinkedModule::Import& import : existing->imports_) {
            if (import.module != nullptr && oldOf(import.module)) {
                const BytecodeModule& code = existing->Module();
                return VmReject(reason,
                                "module {} imports a module of the reload set (reload it too)",
                                VmText(code, code.path));
            }
        }
    }

    if (!plan.entries_.TryReserve(modules.Size())) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    for (const VmReloadModule& pair : modules) {
        VmReloadPlan::Entry entry(*plan.allocator_);
        entry.old = pair.old;
        entry.fresh = pair.fresh;
        if (auto planned = PlanModule(entry, reason); !planned) {
            plan.entries_.Clear();
            return planned;
        }

        static_cast<void>(plan.entries_.TryPushBack(Move(entry))); // 예약함
    }

    plan.executions_ = executions;
    plan.serial_ = nextSerial_;
    return Result<void>::Success();
}

Result<void> VmSession::PlanModule(VmReloadPlan::Entry& entry, StringBuilderBase& reason) noexcept {
    const BytecodeModule& before = entry.old->Module();
    const BytecodeModule& after = entry.fresh->Module();
    if (!entry.classMap.TryResize(before.classes.Size()) ||
        !entry.fieldMap.TryResize(before.classFields.Size()) ||
        !entry.handlerMap.TryResize(before.handlers.Size()) ||
        !entry.changed.TryResize(before.functions.Size()) ||
        !entry.sources.TryResize(after.classFields.Size()) ||
        !entry.fate.TryResize(before.classFields.Size())) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    for (uint32& value : entry.classMap) {
        value = NoBytecodeIndex;
    }

    for (uint32& value : entry.fieldMap) {
        value = NoBytecodeIndex;
    }

    for (uint32& value : entry.handlerMap) {
        value = NoBytecodeIndex;
    }

    for (uint32& value : entry.sources) {
        value = NoBytecodeIndex;
    }

    for (uint8& value : entry.fate) {
        value = VmFieldRemoved;
    }

    // 클래스 대응(이름 — SparksReload K2). instance가 있는 클래스의 삭제·기반 변경은 R3(instance
    // 재생성)가 필요하므로 판정 실패다.
    for (uint32 oldClass = 0; oldClass < before.classes.Size(); ++oldClass) {
        const BytecodeClass& oldEntry = before.classes[oldClass];
        const StringView name = before.String(oldEntry.name);
        const uint32 newClass = VmFindClass(after, name);
        const bool used = HasInstancesOf(*entry.old, oldClass);
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

        entry.classMap[oldClass] = newClass;
        ++entry.classes;
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
                    entry.sources[field] = candidate;
                    break;
                }
            }
        }

        auto sourced = [&](uint32 candidate) noexcept {
            for (uint32 field = newEntry.firstField; field < newEnd; ++field) {
                if (entry.sources[field] == candidate) {
                    return true;
                }
            }

            return false;
        };
        for (uint32 field = newEntry.firstField; field < newEnd; ++field) {
            const uint32 former = after.classFields[field].formerName;
            if (entry.sources[field] != NoBytecodeIndex || former == NoBytecodeIndex) {
                continue;
            }

            for (uint32 candidate = oldEntry.firstField; candidate < oldEnd; ++candidate) {
                if (!sourced(candidate) &&
                    before.String(before.classFields[candidate].name) == after.String(former)) {
                    entry.sources[field] = candidate;
                    break;
                }
            }
        }

        for (uint32 field = newEntry.firstField; field < newEnd; ++field) {
            const uint32 source = entry.sources[field];
            if (source == NoBytecodeIndex) {
                continue;
            }

            const BytecodeTypeField& was = before.classFields[source];
            const BytecodeTypeField& now = after.classFields[field];
            const bool same = VmText(after, now.type) == VmText(before, was.type) &&
                              VmText(after, now.shape) == VmText(before, was.shape) &&
                              now.bank == was.bank && now.slots == was.slots;
            entry.fate[source] = same ? VmFieldMoved : VmFieldLost;
            entry.fieldMap[source] = same ? field : NoBytecodeIndex;
        }

        // handler 대응(함수 이름·경로·이벤트).
        for (uint32 handler = oldEntry.firstHandler;
             handler < oldEntry.firstHandler + oldEntry.handlerCount; ++handler) {
            for (uint32 candidate = newEntry.firstHandler;
                 candidate < newEntry.firstHandler + newEntry.handlerCount; ++candidate) {
                if (VmSameHandler(before, before.handlers[handler], after,
                                  after.handlers[candidate])) {
                    entry.handlerMap[handler] = candidate;
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

        entry.changed[function] = changed ? 1 : 0;
    }

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
    // 직접 참조되거나 살아 있는 실행이 import로 닿는 revision과, 그 revision의 필드 번역이 지나가는
    // 뒤 revision만 남긴다(import는 정적으로 묶인다 — Keep 실행의 옛 코드가 부르는 옛 revision도
    // 산다, LC7c 리뷰 1). 포인터만 비교한다 — 놓인 revision 객체를 읽지 않는다.
    for (Retired& entry : retired_) {
        entry.needed = Uses(entry.link);
        for (const Suspended& execution : suspended_) {
            entry.needed =
                entry.needed || (execution.live && ExecutionReaches(execution, entry.link));
        }
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

bool VmSession::ExecutionReaches(const Suspended& execution,
                                 const VmLinkedModule* link) const noexcept {
    // frame 모듈에서 import(전이)를 따라 link에 닿는지다. 따라간 모듈이 상한을 넘으면 닿는다고
    // 본다(보수적 — revision을 더 오래 붙잡을 뿐이다).
    const VmLinkedModule* seen[MaxReachModules] = {};
    uint32 count = 0;
    for (const VmExecution::Frame& frame : execution.frames) {
        if (frame.link == link) {
            return true;
        }

        bool known = false;
        for (uint32 at = 0; at < count; ++at) {
            known = known || seen[at] == frame.link;
        }

        if (!known) {
            if (count == MaxReachModules) {
                return true;
            }

            seen[count++] = frame.link;
        }
    }

    for (uint32 at = 0; at < count; ++at) {
        for (const VmLinkedModule::Import& import : seen[at]->imports_) {
            if (import.module == nullptr) {
                continue;
            }

            if (import.module == link) {
                return true;
            }

            bool known = false;
            for (uint32 other = 0; other < count; ++other) {
                known = known || seen[other] == import.module;
            }

            if (!known) {
                if (count == MaxReachModules) {
                    return true;
                }

                seen[count++] = import.module;
            }
        }
    }

    return false;
}
bool VmSession::AppendGateState(Array<uint64>& out) const noexcept {
    auto bits = [](const void* pointer) noexcept {
        return uint64(reinterpret_cast<uptr>(pointer));
    };
    bool ok = out.TryPushBack(modules_.Size()) && out.TryPushBack(nextOrder_);
    for (const VmLinkedModule* module : modules_) {
        ok = ok && out.TryPushBack(bits(module));
    }

    for (const Connection& connection : connections_) {
        ok = ok && out.TryPushBack(connection.publisher) &&
             out.TryPushBack(connection.subscriber) && out.TryPushBack(bits(connection.module)) &&
             out.TryPushBack(connection.handler) &&
             out.TryPushBack(bits(connection.key.name.Data())) &&
             out.TryPushBack(connection.key.name.ByteCount()) &&
             out.TryPushBack(bits(connection.key.path.Data())) &&
             out.TryPushBack(connection.order) && out.TryPushBack(connection.active ? 1 : 0);
    }

    for (const Instance& instance : instances_) {
        if (!instance.attached) {
            continue;
        }

        ok = ok && out.TryAppend(instance.written.View()) &&
             out.TryPushBack(bits(instance.tickStopModule)) &&
             out.TryPushBack(instance.ticksStopped ? 1 : 0);
    }

    return ok;
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
    const BytecodeFunction& body = code.functions[function];
    const uint32 end = body.firstWord + body.wordCount;
    for (uint32 at = body.firstWord; at < end;) {
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
        // FieldSlots와 같은 상한(MaxRevisions 단계 — LC7b 리뷰 6). 끝은 쌍의 새 revision이거나
        // 묶음 밖 지금 모듈이다(이전 revision 기록·이번 대응으로 옮긴다).
        for (uint32 hop = 0;; ++hop) {
            const VmReloadPlan::Entry* pair = nullptr;
            bool fresh = false;
            for (const VmReloadPlan::Entry& entry : plan.entries_) {
                fresh = fresh || entry.fresh == module;
                pair = entry.old == module ? &entry : pair;
            }

            bool current = false;
            for (const VmLinkedModule* existing : modules_) {
                current = current || existing == module;
            }

            if (fresh || (current && pair == nullptr)) {
                break;
            }

            bool moved = false;
            if (hop < MaxRevisions && pair != nullptr) {
                moved = TranslateStep(pair->fresh, pair->classMap.View(), pair->fieldMap.View(),
                                      module, classIndex, local.bank, local.slots, offset);
            } else if (hop < MaxRevisions) {
                moved = TranslateField(module, classIndex, local.bank, local.slots, offset);
            }

            if (!moved) {
                return false;
            }
        }
    }

    return true;
}

bool VmSession::CallsFlagged(const VmLinkedModule& link, uint32 function,
                             Span<const VmLinkedModule* const> links, Span<const uint32> starts,
                             Span<const uint8> flags) const noexcept {
    // 같은 모듈 호출(CALL·CALLW의 Function 참조 — 같은 revision)과 import 호출(정적으로 묶인 대상
    // revision — LC7c 리뷰 2)이 표지된 함수를 부르는지다. 가상 호출은 host가 지금 revision으로
    // 푼다.
    auto flagged = [&](const VmLinkedModule* target, uint32 callee) noexcept {
        for (usize at = 0; at < links.Size(); ++at) {
            if (links[at] == target && callee < starts[at + 1] - starts[at]) {
                return flags[starts[at] + callee] != 0;
            }
        }

        return false;
    };
    const BytecodeModule& code = link.Module();
    const BytecodeFunction& entry = code.functions[function];
    const uint32 end = entry.firstWord + entry.wordCount;
    for (uint32 at = entry.firstWord; at < end;) {
        const uint32 word = code.words[at];
        const Opcode op = DecodeOpcode(word);
        at += InfoOf(op).extra ? 2 : 1;
        if (op != Opcode::Call && op != Opcode::CallW) {
            continue;
        }

        const uint32 ref = DecodeBx(word);
        if (ref >= code.refs.Size()) {
            continue;
        }

        if (code.refs[ref].kind == BytecodeRefKind::Function &&
            flagged(&link, code.refs[ref].detail)) {
            return true;
        }

        if (code.refs[ref].kind == BytecodeRefKind::ImportedFunction &&
            ref < link.imports_.Size() && link.imports_[ref].module != nullptr &&
            flagged(link.imports_[ref].module, link.imports_[ref].function)) {
            return true;
        }
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
    // G5 주입(시험 전용)은 이 적용 하나에만 걸린다.
    const bool injectRollback = injectRollback_;
    injectRollback_ = false;
    const usize pairs = plan.entries_.Size();
    // 조각 밖 경계에는 쓰기 기록이 없다(저장소 위치를 바꾸므로 남아 있으면 적용하지 않는다).
    if (closed_ || execution_.running_ || pairs == 0 || !writes_.IsEmpty()) {
        return Outcome::Failure(ErrorCode::InvalidArgument);
    }

    // 쌍 번호다(old가 묶음의 옛 revision이 아니면 pairs).
    auto pairOf = [&](const VmLinkedModule* link) noexcept {
        for (usize at = 0; at < pairs; ++at) {
            if (plan.entries_[at].old == link) {
                return at;
            }
        }

        return pairs;
    };
    Array<usize> slots(*allocator_);
    if (!slots.TryResize(pairs)) {
        return Outcome::Failure(ErrorCode::OutOfMemory);
    }

    for (usize pair = 0; pair < pairs; ++pair) {
        const VmReloadPlan::Entry& entry = plan.entries_[pair];
        slots[pair] = modules_.Size();
        for (usize at = 0; at < modules_.Size(); ++at) {
            if (modules_[at] == entry.fresh) {
                return Outcome::Failure(ErrorCode::InvalidArgument);
            }

            slots[pair] = modules_[at] == entry.old ? at : slots[pair];
        }

        if (slots[pair] == modules_.Size()) {
            return Outcome::Failure(ErrorCode::InvalidArgument);
        }
    }

    // 계획 뒤에 instance가 생겼으면 낡은 계획이다(판정은 계획 때의 instance로 했다 — 필드·handler
    // 변경을 허용한 클래스에 새 instance가 있을 수 있다, LC7a 리뷰 2). 아무것도 바꾸지 않는다.
    if (plan.serial_ != nextSerial_) {
        return Outcome::Failure(ErrorCode::InvalidArgument);
    }

    PruneRetired();
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
    Array<Retired> retiring(*allocator_);
    if (!cancels.TryReserve(suspended_.Size()) || !restarts.TryReserve(suspended_.Size()) ||
        !retiring.TryReserve(pairs) || !retired_.TryReserve(retired_.Size() + pairs)) {
        return Outcome::Failure(ErrorCode::OutOfMemory);
    }

    for (const VmReloadPlan::Entry& entry : plan.entries_) {
        Retired record(*allocator_);
        record.link = entry.old;
        record.successor = entry.fresh;
        if (!record.classMap.TryAppend(entry.classMap.View()) ||
            !record.fieldMap.TryAppend(entry.fieldMap.View())) {
            return Outcome::Failure(ErrorCode::OutOfMemory);
        }

        static_cast<void>(retiring.TryPushBack(Move(record))); // 예약함
    }

    // 쌍마다 old 앞의 이전 revision(새것 → 옛것 순 — revision 상한, SL §12.4).
    Array<const VmLinkedModule*> older(*allocator_);
    Array<uint32> olderCount(*allocator_);
    if (!older.TryResize(pairs * MaxRevisions) || !olderCount.TryResize(pairs)) {
        return Outcome::Failure(ErrorCode::OutOfMemory);
    }

    for (usize pair = 0; pair < pairs; ++pair) {
        olderCount[pair] = 0;
        for (const VmLinkedModule* link = plan.entries_[pair].old;
             olderCount[pair] < MaxRevisions;) {
            const VmLinkedModule* previous = nullptr;
            for (const Retired& entry : retired_) {
                previous = entry.successor == link ? entry.link : previous;
            }

            if (previous == nullptr) {
                break;
            }

            older[pair * MaxRevisions + olderCount[pair]++] = previous;
            link = previous;
        }
    }

    // 판정 표(LC7c 리뷰 1·2): 쌍의 old와 살아 있는 모든 이전 revision의 함수마다
    // - "바뀜"(Cancel 정책 — K6): 그 모듈 경로의 최신 revision(묶음이면 새 revision, 아니면 지금
    //   모듈)과 본문이 다르거나 없다,
    // - "필드 깨짐"(정책 무관 — 5절): 옮기지 못하는(손실·삭제) 필드를 필드 명령으로 참조한다.
    // 둘 다 같은 모듈 호출과 정적으로 묶인 import 호출의 폐포로 넓힌다(재개 뒤 부를 callee까지 —
    // 옛 코드가 낡은 함수를 부르거나 실행 중 fault 대신 취소).
    Array<const VmLinkedModule*> scanned(*allocator_);
    Array<uint32> scannedStart(*allocator_);
    if (!scannedStart.TryPushBack(0)) {
        return Outcome::Failure(ErrorCode::OutOfMemory);
    }

    auto scan = [&](const VmLinkedModule* link) noexcept -> bool {
        for (const VmLinkedModule* known : scanned) {
            if (known == link) {
                return true;
            }
        }

        const uint32 start = scannedStart[scannedStart.Size() - 1];
        return scanned.TryPushBack(link) &&
               scannedStart.TryPushBack(start +
                                        static_cast<uint32>(link->Module().functions.Size()));
    };
    for (const VmReloadPlan::Entry& entry : plan.entries_) {
        if (!scan(entry.old)) {
            return Outcome::Failure(ErrorCode::OutOfMemory);
        }
    }

    for (const Retired& entry : retired_) {
        if (!scan(entry.link)) {
            return Outcome::Failure(ErrorCode::OutOfMemory);
        }
    }

    Array<uint8> changedFlags(*allocator_);
    Array<uint8> broken(*allocator_);
    const uint32 scannedFunctions = scannedStart[scannedStart.Size() - 1];
    if (!changedFlags.TryResize(scannedFunctions) || !broken.TryResize(scannedFunctions)) {
        return Outcome::Failure(ErrorCode::OutOfMemory);
    }

    // 모듈 경로의 최신 revision이다(쌍이면 새 revision, 아니면 지금 모듈 — 없으면 nullptr).
    auto newest = [&](const VmLinkedModule* link) noexcept -> const VmLinkedModule* {
        const BytecodeModule& code = link->Module();
        const StringView path = VmText(code, code.path);
        for (const VmReloadPlan::Entry& entry : plan.entries_) {
            if (VmText(entry.fresh->Module(), entry.fresh->Module().path) == path) {
                return entry.fresh;
            }
        }

        for (const VmLinkedModule* existing : modules_) {
            if (VmText(existing->Module(), existing->Module().path) == path) {
                return existing;
            }
        }

        return nullptr;
    };
    for (usize at = 0; at < scanned.Size(); ++at) {
        const BytecodeModule& code = scanned[at]->Module();
        const VmLinkedModule* latest = newest(scanned[at]);
        for (uint32 function = scannedStart[at]; function < scannedStart[at + 1]; ++function) {
            const uint32 local = function - scannedStart[at];
            const BytecodeFunction& was = code.functions[local];
            const uint32 match = latest == nullptr
                                     ? NoBytecodeIndex
                                     : VmFindFunction(latest->Module(), code.String(was.name));
            bool changed = match == NoBytecodeIndex;
            for (usize byte = 0; !changed && byte < BytecodeHashBytes; ++byte) {
                changed = was.bodyHash[byte] != latest->Module().functions[match].bodyHash[byte];
            }

            changedFlags[function] = changed ? 1 : 0;
            broken[function] = FieldsReach(*scanned[at], local, plan) ? uint8(0) : uint8(1);
        }
    }

    // 호출 폐포: 모든 scanned revision을 한 번에(import가 revision 사이를 잇는다) 더 바뀌지 않을
    // 때까지.
    for (bool grew = true; grew;) {
        grew = false;
        for (usize at = 0; at < scanned.Size(); ++at) {
            for (uint32 function = scannedStart[at]; function < scannedStart[at + 1]; ++function) {
                const uint32 local = function - scannedStart[at];
                if (changedFlags[function] == 0 &&
                    CallsFlagged(*scanned[at], local, scanned.View(), scannedStart.View(),
                                 changedFlags.View())) {
                    changedFlags[function] = 1;
                    grew = true;
                }

                if (broken[function] == 0 && CallsFlagged(*scanned[at], local, scanned.View(),
                                                          scannedStart.View(), broken.View())) {
                    broken[function] = 1;
                    grew = true;
                }
            }
        }
    }

    // frame 함수의 표지다(scanned revision이 아니면 0 — 지금 모듈의 코드는 낡지 않았다).
    auto frameFlag = [&](const Array<uint8>& flags, const VmExecution::Frame& frame) noexcept {
        for (usize at = 0; at < scanned.Size(); ++at) {
            if (frame.link == scanned[at]) {
                return flags[scannedStart[at] + frame.function] != 0;
            }
        }

        return false;
    };
    auto policyCancels = [&](const Suspended& entry) noexcept {
        if (plan.executions_ != VmReloadExecutions::Cancel) {
            return false;
        }

        for (const VmExecution::Frame& frame : entry.frames) {
            if (frameFlag(changedFlags, frame)) {
                return true;
            }
        }

        return false;
    };
    // 설정과 무관한 취소(SL 12.4 — 5절): 삭제된 handler로 시작된 실행, "필드 깨짐" 함수를 frame에
    // 가진 실행.
    auto forcedCancels = [&](const Suspended& entry) noexcept {
        const usize handlerPair = pairOf(entry.handlerModule);
        if (handlerPair < pairs) {
            const Array<uint32>& handlers = plan.entries_[handlerPair].handlerMap;
            if (entry.handler < handlers.Size() && handlers[entry.handler] == NoBytecodeIndex) {
                return true;
            }
        }

        for (const VmExecution::Frame& frame : entry.frames) {
            if (frameFlag(broken, frame)) {
                return true;
            }
        }

        return false;
    };
    auto mustCancel = [&](const Suspended& entry) noexcept {
        return policyCancels(entry) || forcedCancels(entry);
    };

    // 쌍마다 취소 뒤에도 old를 붙잡는 것이 있는지다: 큐 항목의 key 글자(전달되면 놓는다 — 만료
    // 취소의 대상은 아니다), 남는 실행의 frame과 그 import 폐포(LC7c 리뷰 1).
    Array<uint8> stays(*allocator_);
    if (!stays.TryResize(pairs)) {
        return Outcome::Failure(ErrorCode::OutOfMemory);
    }

    for (usize pair = 0; pair < pairs; ++pair) {
        stays[pair] = 0;
        for (usize at = queueHead_; at < queue_.Size(); ++at) {
            stays[pair] |= queue_[at].module == plan.entries_[pair].old ? 1 : 0;
        }

        for (const Suspended& entry : suspended_) {
            if (entry.live && stays[pair] == 0 && !mustCancel(entry) &&
                ExecutionReaches(entry, plan.entries_[pair].old)) {
                stays[pair] = 1;
            }
        }
    }

    // 정리 뒤 old 앞의 revision은 많아야 MaxRevisions - 1개라 넘치는 것은 그 앞쪽뿐이다. 실행이
    // frame이나 import 폐포로 닿으면 만료 취소한다.
    auto expired = [&](const Suspended& entry) noexcept {
        for (usize pair = 0; pair < pairs; ++pair) {
            const uint32 live = 1 + stays[pair] + olderCount[pair];
            const uint32 expire = live > MaxRevisions ? live - MaxRevisions : 0;
            for (uint32 at = olderCount[pair] - expire;
                 expire <= olderCount[pair] && at < olderCount[pair]; ++at) {
                if (ExecutionReaches(entry, older[pair * MaxRevisions + at])) {
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
            const usize pair = pairOf(link);
            if (pair < pairs) {
                owner = plan.entries_[pair].classMap[owner];
                link = plan.entries_[pair].fresh;
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

        const VmLinkedModule* module = nullptr;
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
        usize pairs[MaxBaseDepth] = {};     // 사슬 원소의 쌍(바뀌지 않는 원소는 묶음 쌍 수)
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
    auto defaultsOf = [&](const VmLinkedModule* fresh, uint32 classIndex,
                          uint64 handle) noexcept -> Result<const Defaults*> {
        for (const Defaults& entry : defaults) {
            if (entry.module == fresh && entry.classIndex == classIndex) {
                return Result<const Defaults*>::Success(&entry);
            }
        }

        const BytecodeClass& entry = fresh->Module().classes[classIndex];
        Defaults made(*allocator_);
        made.module = fresh;
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
        usize chainPairs[MaxBaseDepth] = {};
        const uint32 depth = Chain(instance, chainModules, chainClasses);
        bool touched = false;
        for (uint32 at = 0; at < depth; ++at) {
            chainPairs[at] = pairOf(chainModules[at]);
            touched = touched || chainPairs[at] < pairs;
        }

        if (!touched) {
            continue;
        }

        // 새 사슬(묶음 클래스 → 대응 새 클래스)의 구역 시작: 기반부터 bank별로 쌓는다.
        uint32 oldStart[MaxBaseDepth][BytecodeBankCount] = {};
        uint32 newStart[MaxBaseDepth][BytecodeBankCount] = {};
        uint32 oldTotal[BytecodeBankCount] = {};
        uint32 newTotal[BytecodeBankCount] = {};
        for (uint32 at = depth; at > 0; --at) {
            const uint32 index = at - 1;
            const BytecodeClass& was = chainModules[index]->Module().classes[chainClasses[index]];
            const usize pair = chainPairs[index];
            const uint32 mappedClass =
                pair < pairs ? plan.entries_[pair].classMap[chainClasses[index]] : NoBytecodeIndex;
            if (pair < pairs && mappedClass == NoBytecodeIndex) {
                discard();
                return Outcome::Failure(ErrorCode::InvalidArgument); // 계획이 거부했어야 한다
            }

            const BytecodeClass& now =
                pair < pairs ? plan.entries_[pair].fresh->Module().classes[mappedClass] : was;
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
        for (uint32 at = 0; at < depth; ++at) {
            if (chainPairs[at] == pairs) {
                continue;
            }

            const VmReloadPlan::Entry& pair = plan.entries_[chainPairs[at]];
            auto made = defaultsOf(pair.fresh, pair.classMap[chainClasses[at]], entry.handle);
            if (!made) {
                discard();
                return Outcome::Failure(made.Error());
            }
        }

        // defaults가 자랐을 수 있어 포인터는 다 만든 뒤 찾는다.
        const Defaults* chainDefaults[MaxBaseDepth] = {};
        for (uint32 at = 0; at < depth; ++at) {
            for (const Defaults& candidate : defaults) {
                if (chainPairs[at] < pairs &&
                    candidate.module == plan.entries_[chainPairs[at]].fresh &&
                    candidate.classIndex ==
                        plan.entries_[chainPairs[at]].classMap[chainClasses[at]]) {
                    chainDefaults[at] = &candidate;
                }
            }
        }

        const usize totalSlots = usize(newTotal[0]) + newTotal[1] + newTotal[2];
        const usize ownPair = pairOf(entry.module);
        Moving made(*allocator_);
        made.instance = instance;
        made.module = ownPair < pairs ? plan.entries_[ownPair].fresh : entry.module;
        made.classIndex =
            ownPair < pairs ? plan.entries_[ownPair].classMap[entry.classIndex] : entry.classIndex;
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
            target.pairs[at] = chainPairs[at];
            target.refStart[at] = oldStart[at][1];
            const BytecodeClass& was = chainModules[at]->Module().classes[chainClasses[at]];
            if (chainPairs[at] == pairs) {
                // 묶음 밖 구역(다른 모듈 기반·파생)은 그대로 옮긴다(위치만 밀린다).
                for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                    for (uint32 index = 0; index < was.slots[bank]; ++index) {
                        copySlot(bank, usize(oldStart[at][bank]) + index,
                                 usize(newStart[at][bank]) + index);
                    }
                }

                continue;
            }

            const VmReloadPlan::Entry& pair = plan.entries_[chainPairs[at]];
            const BytecodeModule& before = pair.old->Module();
            const BytecodeModule& after = pair.fresh->Module();
            const BytecodeClass& now = after.classes[pair.classMap[chainClasses[at]]];
            for (uint32 field = now.firstField; field < now.firstField + now.fieldCount; ++field) {
                const BytecodeTypeField& declared = after.classFields[field];
                const uint32 source = pair.sources[field];
                const usize to = usize(newStart[at][declared.bank]) + declared.offset;
                if (source != NoBytecodeIndex && pair.fieldMap[source] == field) {
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
                removed += pair.fate[field] == VmFieldRemoved ? 1 : 0;
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

    for (usize pair = 0; pair < pairs; ++pair) {
        modules_[slots[pair]] = plan.entries_[pair].fresh;
    }

    // 연결의 module·handler·key 교체(순서 유지 — K8), 삭제된 handler의 연결은 지운다.
    usize kept = 0;
    for (usize at = 0; at < connections_.Size(); ++at) {
        Connection connection = connections_[at];
        const usize pair = pairOf(connection.module);
        if (pair < pairs) {
            const uint32 handler = plan.entries_[pair].handlerMap[connection.handler];
            if (handler == NoBytecodeIndex) {
                continue;
            }

            connection.module = plan.entries_[pair].fresh;
            connection.handler = handler;
            connection.key = HandlerKey(*plan.entries_[pair].fresh, handler);
        }

        connections_[kept++] = connection;
    }

    static_cast<void>(connections_.TryResize(kept));
    // ---- 연결 다시 풀기: 남는 발행자는 순서 유지, 새 `on`·새 발행자는 끝(K8) ----
    const uint64 dropped = droppedConnections_;
    const uint64 order = nextOrder_; // 되돌리기에 복원(LC7b 리뷰 3)
    Result<void> connected = Result<void>::Success();
    for (usize at = 0; at < moving.Size() && connected; ++at) {
        const Instance& instance = instances_[moving[at].instance];
        if (instance.enabled && !instance.isolated) {
            connected = Connect(moving[at].instance, nullptr, NoBytecodeIndex);
        }
    }

    // G5(SparksReload 6절): 연결 실패와 같은 되돌리기 경로를 밟는다.
    if (connected && injectRollback) {
        injectionFired_ = true;
        connected = Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!connected) {
        // 되돌리기: 연결 표(capacity는 사본 이상)·거부 수·session 모듈·저장소를 원래대로.
        connections_.Clear();
        static_cast<void>(connections_.TryAppend(savedConnections.View()));
        droppedConnections_ = dropped;
        nextOrder_ = order;
        for (usize pair = 0; pair < pairs; ++pair) {
            modules_[slots[pair]] = plan.entries_[pair].old;
        }

        for (Moving& entry : moving) {
            swapStorage(entry);
        }

        discard();
        return Outcome::Failure(connected.Error());
    }

    // ---- 확정(실패 없음) ----
    VmReloadReport report;
    report.modules = static_cast<uint32>(pairs);
    for (const VmReloadPlan::Entry& entry : plan.entries_) {
        report.classes += entry.classes;
    }

    report.instances = static_cast<uint32>(moving.Size());
    report.moved = moved;
    report.defaulted = defaulted;
    report.lost = lost;
    report.removed = removed;
    // 옮기지 않은 유지 자원 필드(손실·삭제)를 선언 역순(파생 → 기반, 뒤 필드부터)으로 멈추고 옛
    // 저장소의 Buf를 놓는다(옮긴 값은 새 저장소가 자기 참조를 쥐었다).
    for (Moving& entry : moving) {
        for (uint32 at = 0; at < entry.depth; ++at) {
            if (entry.pairs[at] == pairs) {
                continue;
            }

            const VmReloadPlan::Entry& pair = plan.entries_[entry.pairs[at]];
            const BytecodeModule& before = pair.old->Module();
            const BytecodeClass& was = before.classes[entry.classes[at]];
            for (uint32 field = was.firstField + was.fieldCount; field > was.firstField; --field) {
                const BytecodeTypeField& declared = before.classFields[field - 1];
                const usize where = usize(entry.refStart[at]) + declared.offset;
                if ((declared.flags & BytecodeFieldHeld) == 0 ||
                    pair.fate[field - 1] == VmFieldMoved || where >= entry.refs.Size() ||
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
        if (entry.attached && entry.ticksStopped && pairOf(entry.tickStopModule) < pairs) {
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
        const usize pair = pairOf(item.targetModule);
        if (pair < pairs) {
            const uint32 handler = plan.entries_[pair].handlerMap[item.targetHandler];
            if (handler == NoBytecodeIndex) {
                // 대상 handler가 없어졌다: 항목을 버린다(모두에게 전달되지 않게 — LC7a 리뷰 4).
                ReleaseItem(item);
                item.publisher = 0;
                continue;
            }

            item.targetHandler = handler;
            item.targetModule = plan.entries_[pair].fresh;
            item.key = HandlerKey(*plan.entries_[pair].fresh, handler); // old의 key 글자 대신
        }
    }

    for (Suspended& entry : suspended_) {
        const usize pair = entry.live ? pairOf(entry.handlerModule) : pairs;
        if (pair < pairs) {
            entry.handler = plan.entries_[pair].handlerMap[entry.handler];
            entry.handlerModule =
                entry.handler == NoBytecodeIndex ? nullptr : plan.entries_[pair].fresh;
        }
    }

    // 이전 revision 기록(Keep 실행의 필드 번역).
    for (Retired& record : retiring) {
        static_cast<void>(retired_.TryPushBack(Move(record))); // 예약함
    }

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
    plan.entries_.Clear();
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
