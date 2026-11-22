#include "BcEncoder.h"
#include "IrBuilder.h"

// CLASS 구역(형식 버전 3 — Docs/Design/SparksRuntime.ko.md 2.1절)을 쓴다: 모듈의 class마다 기반,
// 자기 필드 배치(bank 구역 안 위치), hook(이름 하나짜리 `on X`), handler(`on 경로.이벤트` — 재진입
// 정책·await 여부), 이벤트(인자 서술자). 기반 클래스를 먼저 쓴다(verifier: 기반은 앞선 클래스).

namespace rocks::sparks::detail {

namespace {

struct ClassHookName {
    StringView name;
    BytecodeHookKind kind;
};

constexpr ClassHookName ClassHookNames[] = {
    {"Awake", BytecodeHookKind::Awake},
    {"Enable", BytecodeHookKind::Enable},
    {"Start", BytecodeHookKind::Start},
    {"Tick", BytecodeHookKind::Tick},
    {"FixedTick", BytecodeHookKind::FixedTick},
    {"LateTick", BytecodeHookKind::LateTick},
    {"Disable", BytecodeHookKind::Disable},
    {"Destroy", BytecodeHookKind::Destroy},
    {"TriggerEnter", BytecodeHookKind::TriggerEnter},
    {"TriggerExit", BytecodeHookKind::TriggerExit},
    {"CollisionEnter", BytecodeHookKind::CollisionEnter},
    {"CollisionExit", BytecodeHookKind::CollisionExit},
    {"Validate", BytecodeHookKind::Validate},
};

} // namespace

bool BytecodeEncoder::EncodeClasses(Span<const uint64> bodies) noexcept {
    const SyntaxTree& tree = c_.trees_[module_];
    const uint32 root = tree.Root();
    const uint32 declarations = root == NoNode ? NoNode : tree.Child(root, 1);
    if (declarations == NoNode) {
        return checker_.binder_.error_ == ErrorCode::None;
    }

    // 선언 순 class 심볼과 그 노드다.
    Array<uint64> classes(checker_.allocator_); // (symbol << 32) | node
    for (const uint32 declaration : tree.Children(declarations)) {
        if (tree.Node(declaration).kind != SyntaxKind::Class) {
            continue;
        }

        const uint32 symbol = c_.NodeSymbol(module_, tree.Child(declaration, 2));
        if (symbol != NoSymbol && !classes.TryPushBack((uint64(symbol) << 32) | declaration)) {
            Fail(ErrorCode::OutOfMemory);
            return false;
        }
    }

    Array<uint32> emitted(checker_.allocator_); // classes 순번 → CLASS 표 번호(아직이면 NoIndex)
    if (!emitted.TryResize(classes.Size())) {
        Fail(ErrorCode::OutOfMemory);
        return false;
    }

    for (uint32& index : emitted) {
        index = NoBytecodeIndex;
    }

    // position의 class를 쓴다(같은 모듈 기반 먼저 — 순환은 검사기가 막았다).
    auto emit = [&](auto& self, usize position) noexcept -> uint32 {
        if (emitted[position] != NoBytecodeIndex) {
            return emitted[position];
        }

        const uint32 symbol = static_cast<uint32>(classes[position] >> 32);
        const uint32 node = static_cast<uint32>(classes[position]);
        const Symbol& entry = c_.symbols_[symbol];
        BytecodeClass stored;
        stored.name = AddString(entry.name);
        if (entry.base != NoSymbol) {
            const Symbol& base = c_.symbols_[entry.base];
            usize found = classes.Size();
            for (usize at = 0; at < classes.Size(); ++at) {
                found = static_cast<uint32>(classes[at] >> 32) == entry.base ? at : found;
            }

            if (base.kind == SymbolKind::Class && base.module == module_ &&
                found < classes.Size()) {
                stored.baseClass = self(self, found);
            } else {
                StringBuilder<128> name(checker_.allocator_);
                Qualify(name, entry.base);
                name.Append(base.name);
                if (name.HasFailed()) {
                    Fail(name.Status().Error());
                    return NoBytecodeIndex;
                }

                stored.baseName = AddString(name.View());
            }
        }

        // 자기 필드: 선언 순, bank마다 이어 붙인 위치.
        const uint32 members = tree.Child(node, 4);
        stored.firstField = static_cast<uint32>(out_.classFields.Size());
        stored.firstEvent = static_cast<uint32>(out_.events.Size());
        for (const uint32 member :
             members == NoNode ? Span<const uint32>() : tree.Children(members)) {
            const SyntaxKind kind = tree.Node(member).kind;
            if (kind != SyntaxKind::Field && kind != SyntaxKind::Event) {
                continue;
            }

            const uint32 memberSymbol = c_.NodeSymbol(module_, tree.Child(member, 2));
            if (memberSymbol == NoSymbol) {
                continue;
            }

            if (kind == SyntaxKind::Field) {
                const IrLayout layout =
                    IrBuilder::Layout(checker_, checker_.FieldType(memberSymbol));
                const uint32 bank = static_cast<uint32>(layout.bank);
                BytecodeTypeField field;
                field.name = AddString(c_.symbols_[memberSymbol].name);
                field.bank = static_cast<uint8>(bank);
                field.slots = static_cast<uint8>(layout.slots);
                field.offset = stored.slots[bank];
                stored.slots[bank] = static_cast<uint16>(stored.slots[bank] + layout.slots);
                if (!out_.classFields.TryPushBack(field)) {
                    Fail(ErrorCode::OutOfMemory);
                    return NoBytecodeIndex;
                }

                ++stored.fieldCount;
                continue;
            }

            // 이벤트: 인자 서술자(매개변수 심볼 — 선언 순).
            StringBuilder<128> arguments(checker_.allocator_);
            bool first = true;
            for (uint32 at = 0; at < c_.symbols_.Size(); ++at) {
                const Symbol& parameter = c_.symbols_[at];
                if (parameter.kind == SymbolKind::Parameter && parameter.owner == memberSymbol) {
                    arguments.Append(first ? "" : ",");
                    first = false;
                    AppendDescriptor(arguments, parameter.type);
                }
            }

            if (arguments.HasFailed()) {
                Fail(arguments.Status().Error());
                return NoBytecodeIndex;
            }

            BytecodeEvent event;
            event.name = AddString(c_.symbols_[memberSymbol].name);
            event.arguments = first ? NoBytecodeIndex : AddString(arguments.View());
            if (!out_.events.TryPushBack(event)) {
                Fail(ErrorCode::OutOfMemory);
                return NoBytecodeIndex;
            }

            ++stored.eventCount;
        }

        // 초기화 함수: bodies의 class 노드 항목(함수 번호 = bodies 순번).
        for (usize index = 0; index < bodies.Size(); ++index) {
            if (static_cast<uint32>(bodies[index]) == node &&
                static_cast<uint32>(bodies[index] >> 32) == symbol) {
                stored.initializer = static_cast<uint32>(index);
            }
        }

        // hook·handler: 이 class의 handler 본문(함수 번호 = bodies 순번).
        stored.firstHook = static_cast<uint32>(out_.hooks.Size());
        stored.firstHandler = static_cast<uint32>(out_.handlers.Size());
        for (usize index = 0; index < bodies.Size(); ++index) {
            const uint32 body = static_cast<uint32>(bodies[index]);
            if (static_cast<uint32>(bodies[index] >> 32) != symbol ||
                tree.Node(body).kind != SyntaxKind::Handler) {
                continue;
            }

            const uint32 path = tree.Child(body, 2);
            const Span<const uint32> names =
                path == NoNode ? Span<const uint32>() : tree.Children(path);
            if (names.Size() == 1) {
                const StringView name = checker_.TokenText(module_, names[0]);
                bool hook = false;
                for (const ClassHookName& entry : ClassHookNames) {
                    if (entry.name == name) {
                        BytecodeHook hookEntry;
                        hookEntry.kind = entry.kind;
                        hookEntry.function = static_cast<uint32>(index);
                        hook = true;
                        if (!out_.hooks.TryPushBack(hookEntry)) {
                            Fail(ErrorCode::OutOfMemory);
                            return NoBytecodeIndex;
                        }
                    }
                }

                if (hook) {
                    ++stored.hookCount;
                    continue;
                }
            }

            // `on 경로.이벤트`: 마지막 이름이 이벤트, 앞이 경로(`this`는 그대로).
            StringBuilder<128> joined(checker_.allocator_);
            for (usize at = 0; at + 1 < names.Size(); ++at) {
                joined.Append(at == 0 ? "" : ".").Append(checker_.TokenText(module_, names[at]));
            }

            if (joined.HasFailed()) {
                Fail(joined.Status().Error());
                return NoBytecodeIndex;
            }

            BytecodeHandler handler;
            handler.path = AddString(joined.View());
            handler.event =
                AddString(names.IsEmpty() ? StringView()
                                          : checker_.TokenText(module_, names[names.Size() - 1]));
            handler.function = static_cast<uint32>(index);
            handler.awaits = checker_.ContainsAwait(module_, tree.Child(body, 4)) ? 1 : 0;
            handler.reentry = checker_.HasAttribute(module_, body, "skipWhileBusy")
                                  ? BytecodeReentry::SkipWhileBusy
                              : checker_.HasAttribute(module_, body, "restart")
                                  ? BytecodeReentry::Restart
                                  : BytecodeReentry::None;
            if (!out_.handlers.TryPushBack(handler)) {
                Fail(ErrorCode::OutOfMemory);
                return NoBytecodeIndex;
            }

            ++stored.handlerCount;
        }

        if (checker_.binder_.error_ != ErrorCode::None) {
            return NoBytecodeIndex;
        }

        const uint32 index = static_cast<uint32>(out_.classes.Size());
        if (!out_.classes.TryPushBack(stored)) {
            Fail(ErrorCode::OutOfMemory);
            return NoBytecodeIndex;
        }

        emitted[position] = index;
        return index;
    };
    for (usize position = 0;
         position < classes.Size() && checker_.binder_.error_ == ErrorCode::None; ++position) {
        emit(emit, position);
    }

    return checker_.binder_.error_ == ErrorCode::None;
}

} // namespace rocks::sparks::detail
