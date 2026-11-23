#include "CompilationChecker.h"
#include "TypeChecker.h"

// 필드·const 초기값을 이름 의존 순서대로 미리 정하고(재귀 깊이를 쓰지 않게) 남은 연쇄의 깊이를
// 막는다(Docs/Design/SparksLanguage 3.10절 "초기값 깊이", S0427). 필드·const의 타입·값은
// 초기값을 검사해 정하므로 초기값이 가리키는 다른 필드·const를 먼저 정해야 한다. 이름(`X`, 별칭의
// `Alias.X`)으로 가리키는 의존은 반복형 깊이 우선 탐색의 후위 순서로 미리 정하므로, 그 뒤의 검사는
// 이미 정한 값을 읽고 깊이를 쓰지 않는다.

namespace rocks::sparks::detail {

namespace {

// 깊이 우선 탐색의 한 프레임: 초기값 항목과 다음에 볼 간선이다.
struct InitializerFrame {
    uint32 item = NoLocalIndex;
    uint32 edge = 0;
};

} // namespace

bool TypeChecker::EnterInitializer(uint32 module, uint32 initializer) noexcept {
    if (initializerDepth_ < MaxInitializerDepth) {
        ++initializerDepth_;
        return true;
    }

    CheckContext context;
    context.module = module;
    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
    message.AppendFormat("This initializer depends on a chain of more than {} initializers.",
                         MaxInitializerDepth);
    if (binder_.Succeeded(message)) {
        Report(context, DiagnosticCode::InitializerTooDeep, initializer, message.View(),
               "Field and const types and values come from their initializers; the chain is "
               "limited so that the compiler stack cannot overflow.",
               "Split the chain with a typed const or a local variable.");
    }

    return false;
}

void TypeChecker::PrepareInitializers() noexcept {
    Compilation& c = compilation_;
    const uint32 symbolCount = static_cast<uint32>(c.symbols_.Size());
    Array<uint32> items(allocator_);
    Array<uint32> local(allocator_);
    if (auto resized = local.TryResize(symbolCount); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32 id = 0; id < symbolCount; ++id) {
        local[id] = NoLocalIndex;
        const Symbol& symbol = c.symbols_[id];
        if ((symbol.kind != SymbolKind::Const && symbol.kind != SymbolKind::Field) ||
            symbol.node == NoNode || symbol.module >= c.nodeBase_.Size() ||
            c.nodeBase_[symbol.module] == NoNode ||
            c.trees_[symbol.module].Child(symbol.node, 4) == NoNode) {
            continue;
        }

        local[id] = static_cast<uint32>(items.Size());
        if (auto pushed = items.TryPushBack(id); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }
    }

    // 간선: 초기값 안의 Identifier가 가리키는 필드·const와 별칭 멤버(`Alias.X`)의 const·필드.
    const uint32 count = static_cast<uint32>(items.Size());
    Array<uint32> firstEdge(allocator_);
    Array<uint32> edges(allocator_);
    Array<uint32> pending(allocator_);
    for (uint32 index = 0; index < count && binder_.error_ == ErrorCode::None; ++index) {
        if (auto pushed = firstEdge.TryPushBack(static_cast<uint32>(edges.Size())); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }

        const Symbol& symbol = c.symbols_[items[index]];
        const SyntaxTree& tree = c.trees_[symbol.module];
        pending.Clear();
        if (auto pushed = pending.TryPushBack(tree.Child(symbol.node, 4)); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }

        while (!pending.IsEmpty()) {
            const uint32 node = pending.TryPopBack().Value(); // 비어 있지 않다
            const SyntaxNode& entry = tree.Node(node);
            uint32 target = NoSymbol;
            if (entry.kind == SyntaxKind::Identifier) {
                target = c.NodeSymbol(symbol.module, node);
            } else if (entry.kind == SyntaxKind::Member && tree.Child(node, 0) != NoNode &&
                       tree.Node(tree.Child(node, 0)).kind == SyntaxKind::Identifier) {
                const uint32 alias = c.NodeSymbol(symbol.module, tree.Child(node, 0));
                if (alias != NoSymbol && c.symbols_[alias].kind == SymbolKind::Alias) {
                    target = binder_.FindName(
                        SpaceKey(NameSpaceKind::ModuleTop, c.symbols_[alias].index),
                        TokenText(symbol.module, node));
                }
            }

            if (target != NoSymbol && local[target] != NoLocalIndex) {
                if (auto pushed = edges.TryPushBack(local[target]); !pushed) {
                    binder_.error_ = pushed.Error();
                    return;
                }
            }

            for (uint32 child = 0; child < entry.childCount; ++child) {
                const uint32 next = tree.Child(node, child);
                if (next == NoNode) {
                    continue;
                }

                if (auto pushed = pending.TryPushBack(next); !pushed) {
                    binder_.error_ = pushed.Error();
                    return;
                }
            }
        }
    }

    if (auto pushed = firstEdge.TryPushBack(static_cast<uint32>(edges.Size())); !pushed) {
        binder_.error_ = pushed.Error();
        return;
    }

    // 반복형 깊이 우선 탐색의 후위 순서로 정한다(의존이 먼저). 순환은 걷는 중인 항목에서 끊는다
    // (const 순환 진단은 LC2d — 그 const는 값 없이 Invalid가 된다).
    Array<uint8> state(allocator_); // 0 미방문, 1 걷는 중, 2 끝
    Array<InitializerFrame> frames(allocator_);
    if (auto resized = state.TryResize(count); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32 index = 0; index < count; ++index) {
        state[index] = 0;
    }

    for (uint32 root = 0; root < count && binder_.error_ == ErrorCode::None; ++root) {
        if (state[root] != 0) {
            continue;
        }

        state[root] = 1;
        if (auto pushed = frames.TryPushBack(InitializerFrame{root, firstEdge[root]}); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }

        while (!frames.IsEmpty() && binder_.error_ == ErrorCode::None) {
            InitializerFrame& frame = frames[frames.Size() - 1];
            if (frame.edge < firstEdge[frame.item + 1]) {
                const uint32 target = edges[frame.edge++];
                if (state[target] == 0) {
                    state[target] = 1;
                    if (auto pushed =
                            frames.TryPushBack(InitializerFrame{target, firstEdge[target]});
                        !pushed) {
                        binder_.error_ = pushed.Error();
                        return;
                    }
                }

                continue;
            }

            const uint32 item = frame.item;
            static_cast<void>(frames.TryPopBack()); // 비어 있지 않다
            state[item] = 2;
            const uint32 symbol = items[item];
            if (c.symbols_[symbol].kind == SymbolKind::Const) {
                static_cast<void>(ConstOf(symbol));
            } else {
                static_cast<void>(FieldType(symbol));
            }
        }
    }
}

} // namespace rocks::sparks::detail
