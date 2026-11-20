#include "CompilationChecker.h"
#include "TypeChecker.h"

// struct 값의 순환(S0426)과 struct 기본값이다(Docs/Design/SparksLanguage.ko.md 3.10절 "값 struct의
// 순환", 3.2절 기본값). Sparks struct의 필드 그래프(필드 타입에서 `?`를 벗긴 것이 struct일 때의
// 간선)에서 강하게 연결된 묶음을 반복형 Tarjan으로 찾는다. 묶음은 끝(다른 묶음을 가리키지 않는
// 쪽)부터 나오므로 그 순서로 기본값을 정하면 가리키는 struct의 판정이 이미 있다. 재귀가 없어 struct
// 수가 많아도 stack을 쓰지 않는다.

namespace rocks::sparks::detail {

namespace {

// 필드 간선: 대상 struct(지역 번호)와 필드 노드다.
struct StructEdge {
    uint32 target = NoLocalIndex;
    uint32 field = NoNode;
};

// Tarjan 반복의 한 프레임: 지역 번호와 다음에 볼 간선이다.
struct TarjanFrame {
    uint32 node = NoLocalIndex;
    uint32 edge = 0;
};

} // namespace

bool TypeChecker::IsEngineStruct(uint32 symbol) const noexcept {
    if (symbol == NoSymbol) {
        return false;
    }

    const Symbol& entry = compilation_.symbols_[symbol];
    return entry.kind == SymbolKind::EngineType &&
           compilation_.schemas_[entry.index].schema.Types()[entry.item].kind ==
               SchemaTypeKind::Struct;
}

bool TypeChecker::EngineStructHasDefault(uint32 symbol) noexcept {
    Compilation& c = compilation_;
    if (structDefault_[symbol] != StructDefaultUnknown) {
        // 판정 중(schema struct의 값 순환)이면 연쇄를 막기 위해 기본값이 있는 것으로 본다.
        return structDefault_[symbol] != StructDefaultMissing;
    }

    structDefault_[symbol] = StructDefaultComputing;
    const Symbol& entry = c.symbols_[symbol];
    const BindingSchema& schema = c.schemas_[entry.index].schema;
    const SchemaType& source = schema.Types()[entry.item];
    bool all = true;
    for (uint32 at = source.firstField; all && at < source.firstField + source.fieldCount; ++at) {
        const uint32 field = binder_.FindName(SpaceKey(NameSpaceKind::Members, symbol),
                                              schema.Text(schema.Fields()[at].name));
        const uint32 type = field == NoSymbol ? NoType : c.symbols_[field].type;
        if (type == NoType || IsOptional(type)) {
            continue;
        }

        const uint32 target = SymbolOf(type);
        all = IsEngineStruct(target) ? EngineStructHasDefault(target) : HasDefault(type);
    }

    structDefault_[symbol] = all ? StructDefaultPresent : StructDefaultMissing;
    return all;
}

bool TypeChecker::SparksStructHasDefault(uint32 symbol) const noexcept {
    const Compilation& c = compilation_;
    const Symbol& entry = c.symbols_[symbol];
    const SyntaxTree& tree = c.trees_[entry.module];
    const uint32 members = tree.Child(entry.node, 3);
    if (members == NoNode || tree.Node(members).kind != SyntaxKind::List) {
        return true;
    }

    for (const uint32 member : tree.Children(members)) {
        if (tree.Node(member).kind != SyntaxKind::Field || tree.Child(member, 4) != NoNode) {
            continue;
        }

        const uint32 field = c.NodeSymbol(entry.module, tree.Child(member, 2));
        const uint32 type = field == NoSymbol ? NoType : c.symbols_[field].type;
        if (type != NoType && !HasDefault(type)) {
            return false;
        }
    }

    return true;
}

void TypeChecker::AnalyzeStructs(bool cycles) noexcept {
    Compilation& c = compilation_;
    const uint32 symbolCount = static_cast<uint32>(c.symbols_.Size());
    if (!cycles) {
        if (auto resized = structDefault_.TryResize(symbolCount); !resized) {
            binder_.error_ = resized.Error();
            return;
        }

        for (uint32 id = 0; id < symbolCount; ++id) {
            structDefault_[id] = StructDefaultUnknown;
        }

        // 엔진 struct의 기본값(schema 필드 기준)을 먼저 정한다.
        for (uint32 id = 0; id < symbolCount; ++id) {
            if (IsEngineStruct(id)) {
                static_cast<void>(EngineStructHasDefault(id));
            }
        }
    }

    // Sparks struct와 그 필드 간선(필드 선언 순서).
    Array<uint32> structs(allocator_);
    Array<uint32> local(allocator_);
    if (auto resized = local.TryResize(symbolCount); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32 id = 0; id < symbolCount; ++id) {
        local[id] = NoLocalIndex;
        if (c.symbols_[id].kind == SymbolKind::Struct && c.symbols_[id].node != NoNode) {
            local[id] = static_cast<uint32>(structs.Size());
            if (auto pushed = structs.TryPushBack(id); !pushed) {
                binder_.error_ = pushed.Error();
                return;
            }
        }
    }

    const uint32 count = static_cast<uint32>(structs.Size());
    Array<uint32> firstEdge(allocator_);
    Array<StructEdge> edges(allocator_);
    for (uint32 index = 0; index < count; ++index) {
        if (auto pushed = firstEdge.TryPushBack(static_cast<uint32>(edges.Size())); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }

        const Symbol& entry = c.symbols_[structs[index]];
        const SyntaxTree& tree = c.trees_[entry.module];
        const uint32 members = tree.Child(entry.node, 3);
        if (members == NoNode || tree.Node(members).kind != SyntaxKind::List) {
            continue;
        }

        for (const uint32 member : tree.Children(members)) {
            if (tree.Node(member).kind != SyntaxKind::Field) {
                continue;
            }

            // 기본값 단계는 표기한 필드만 본다(표기 없는 필드는 초기값이 있어 기본값과 무관하다).
            // 순환 단계는 검사가 추론한 타입까지 본다.
            if (!cycles && tree.Child(member, 3) == NoNode) {
                continue;
            }

            const uint32 field = c.NodeSymbol(entry.module, tree.Child(member, 2));
            const uint32 type = field == NoSymbol ? NoType : c.symbols_[field].type;
            const uint32 target = type == NoType ? NoSymbol : SymbolOf(NonOptional(type));
            if (target == NoSymbol || local[target] == NoLocalIndex) {
                continue;
            }

            if (auto pushed = edges.TryPushBack(StructEdge{local[target], member}); !pushed) {
                binder_.error_ = pushed.Error();
                return;
            }
        }
    }

    if (auto pushed = firstEdge.TryPushBack(static_cast<uint32>(edges.Size())); !pushed) {
        binder_.error_ = pushed.Error();
        return;
    }

    Array<uint32> order(allocator_);     // 방문 번호(NoLocalIndex = 아직)
    Array<uint32> low(allocator_);       // 스택 위에서 도달하는 가장 이른 방문 번호
    Array<uint32> component(allocator_); // 묶음 번호(NoLocalIndex = 아직)
    Array<uint32>* const perNode[] = {&order, &low, &component};
    for (Array<uint32>* list : perNode) {
        if (auto resized = list->TryResize(count); !resized) {
            binder_.error_ = resized.Error();
            return;
        }
    }

    for (uint32 index = 0; index < count; ++index) {
        order[index] = NoLocalIndex;
        component[index] = NoLocalIndex;
    }

    // (패키지 이름, 모듈 import 경로, 원문 위치) byte 순서로 앞인지다(S0323과 같은 순서).
    auto before = [&c, this](uint32 left, uint32 right) noexcept {
        const Symbol& a = c.symbols_[left];
        const Symbol& b = c.symbols_[right];
        const int package =
            StringView::CompareBytes(c.packages_[c.modules_[a.module].package].manifest.Name(),
                                     c.packages_[c.modules_[b.module].package].manifest.Name());
        if (package != 0) {
            return package < 0;
        }

        const int path = StringView::CompareBytes(c.Text(c.modules_[a.module].importPath),
                                                  c.Text(c.modules_[b.module].importPath));
        if (path != 0) {
            return path < 0;
        }

        return binder_.NodeSpan(a.module, a.nameNode).begin <
               binder_.NodeSpan(b.module, b.nameNode).begin;
    };

    Array<uint32> stack(allocator_);
    Array<uint32> members(allocator_);
    Array<TarjanFrame> frames(allocator_);
    uint32 visited = 0;
    uint32 components = 0;
    auto enter = [&](uint32 node) noexcept {
        order[node] = visited;
        low[node] = visited;
        ++visited;
        if (auto pushed = stack.TryPushBack(node); !pushed) {
            binder_.error_ = pushed.Error();
            return false;
        }

        if (auto pushed = frames.TryPushBack(TarjanFrame{node, firstEdge[node]}); !pushed) {
            binder_.error_ = pushed.Error();
            return false;
        }

        return true;
    };
    // 순환 하나에 S0426 하나: 순서로 처음인 struct의, 묶음 안을 가리키는 첫 필드 타입에.
    auto reportCycle = [&](uint32 componentId) noexcept {
        uint32 first = members[0];
        for (const uint32 member : members) {
            first = before(structs[member], structs[first]) ? member : first;
        }

        uint32 field = NoNode;
        for (uint32 at = firstEdge[first]; field == NoNode && at < firstEdge[first + 1]; ++at) {
            if (component[edges[at].target] == componentId) {
                field = edges[at].field;
            }
        }

        const Symbol& entry = c.symbols_[structs[first]];
        CheckContext context;
        context.module = entry.module;
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("Struct '{}' contains itself through its fields.", entry.name);
        StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
        fix.AppendFormat("Hold it in a container, as in 'Array<{}>', or make it a class.",
                         entry.name);
        // 위치는 필드의 타입 표기, 표기가 없으면(초기값에서 정한 타입) 필드 이름이다.
        const SyntaxTree& tree = c.trees_[entry.module];
        const uint32 at =
            tree.Child(field, 3) != NoNode ? tree.Child(field, 3) : tree.Child(field, 2);
        if (binder_.Succeeded(message) && binder_.Succeeded(fix)) {
            Report(context, DiagnosticCode::RecursiveStruct, at, message.View(),
                   "A struct holds its fields by value, so it cannot contain itself directly "
                   "or through other structs.",
                   fix.View());
        }
    };

    for (uint32 root = 0; root < count && binder_.error_ == ErrorCode::None; ++root) {
        if (order[root] != NoLocalIndex) {
            continue;
        }

        if (!enter(root)) {
            return;
        }

        while (!frames.IsEmpty()) {
            TarjanFrame& frame = frames[frames.Size() - 1];
            const uint32 node = frame.node;
            if (frame.edge < firstEdge[node + 1]) {
                const uint32 target = edges[frame.edge].target;
                ++frame.edge;
                if (order[target] == NoLocalIndex) {
                    if (!enter(target)) {
                        return;
                    }
                } else if (component[target] == NoLocalIndex && order[target] < low[node]) {
                    low[node] = order[target]; // 아직 묶음에 들지 않은(스택 위) 대상
                }

                continue;
            }

            static_cast<void>(frames.TryPopBack()); // 비어 있지 않다
            if (!frames.IsEmpty()) {
                const uint32 parent = frames[frames.Size() - 1].node;
                low[parent] = low[node] < low[parent] ? low[node] : low[parent];
            }

            if (low[node] != order[node]) {
                continue;
            }

            // node가 묶음의 뿌리다: 스택에서 node까지 꺼낸다.
            members.Clear();
            uint32 popped = NoLocalIndex;
            do {
                popped = stack.TryPopBack().Value(); // node가 스택에 있다
                component[popped] = components;
                if (auto pushed = members.TryPushBack(popped); !pushed) {
                    binder_.error_ = pushed.Error();
                    return;
                }
            } while (popped != node);

            bool cyclic = members.Size() > 1;
            for (uint32 at = firstEdge[node]; !cyclic && at < firstEdge[node + 1]; ++at) {
                cyclic = edges[at].target == node;
            }

            if (cyclic && cycles) {
                reportCycle(components);
            } else if (cycles) {
                // 순환이 아닌 묶음 — 기본값은 앞 단계에서 정했다.
            } else if (cyclic) {
                for (const uint32 member : members) {
                    structDefault_[structs[member]] = StructDefaultPresent; // 연쇄 방지
                }
            } else {
                structDefault_[structs[node]] = SparksStructHasDefault(structs[node])
                                                    ? StructDefaultPresent
                                                    : StructDefaultMissing;
            }

            ++components;
        }
    }
}

} // namespace rocks::sparks::detail
