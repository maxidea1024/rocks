#include "CompilationChecker.h"
#include "TypeChecker.h"

// struct 값의 순환(S0426)과 struct 기본값이다(Docs/Design/SparksLanguage 3.10절 "값 struct의
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
    bool all = true;
    // 기반 struct의 필드도 값의 일부다(SL 10.2절 struct `base` — EI1a). 기반 순환은 이름 해석이
    // 끊었다.
    for (uint32 owner = symbol, step = 0; all && IsEngineStruct(owner) && step < c.symbols_.Size();
         owner = c.symbols_[owner].base, ++step) {
        const Symbol& entry = c.symbols_[owner];
        const BindingSchema& schema = c.schemas_[entry.index].schema;
        const SchemaType& source = schema.Types()[entry.item];
        for (uint32 at = source.firstField; all && at < source.firstField + source.fieldCount;
             ++at) {
            const uint32 field = binder_.FindName(SpaceKey(NameSpaceKind::Members, owner),
                                                  schema.Text(schema.Fields()[at].name));
            const uint32 type = field == NoSymbol ? NoType : c.symbols_[field].type;
            if (type == NoType || IsOptional(type)) {
                continue;
            }

            const uint32 target = SymbolOf(type);
            all = IsEngineStruct(target) ? EngineStructHasDefault(target) : HasDefault(type);
        }
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

void TypeChecker::CheckDefaultCycles() noexcept {
    // 간선: struct S의 필드 초기값 안의 struct T 생성 식(`T(...)` — 배열·Map 리터럴 안 포함). S와
    // T가 같은 강한 연결 묶음이면(자기 간선 포함) 그 생성 식에 S0429다. 다만 묶음의 간선이 모두
    // 필드 초기값 전체인 생성(값 필드를 채우는 것)이면 값 순환이라 S0426이 이미 알리므로 내지
    // 않는다(연쇄 방지). 묶음은 반복형 Kosaraju로 찾는다(재귀 없음).
    Compilation& c = compilation_;
    const uint32 symbolCount = static_cast<uint32>(c.symbols_.Size());
    Array<uint32> local(allocator_);
    Array<uint32> structs(allocator_);
    if (auto resized = local.TryResize(symbolCount); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32 id = 0; id < symbolCount; ++id) {
        local[id] = NoLocalIndex;
        if (c.symbols_[id].kind == SymbolKind::Struct && c.symbols_[id].node != NoNode &&
            c.symbols_[id].module < c.modules_.Size()) {
            local[id] = static_cast<uint32>(structs.Size());
            if (!structs.TryPushBack(id)) {
                binder_.error_ = ErrorCode::OutOfMemory;
                return;
            }
        }
    }

    const uint32 count = static_cast<uint32>(structs.Size());
    Array<uint32> firstEdge(allocator_);
    Array<StructEdge> edges(allocator_); // target, field = 생성 식 노드
    Array<uint8> direct(allocator_);     // 그 생성 식이 필드 초기값 전체(괄호 안 포함)인지
    Array<uint32> pending(allocator_);
    for (uint32 index = 0; index < count && binder_.error_ == ErrorCode::None; ++index) {
        if (!firstEdge.TryPushBack(static_cast<uint32>(edges.Size()))) {
            binder_.error_ = ErrorCode::OutOfMemory;
            return;
        }

        const Symbol& entry = c.symbols_[structs[index]];
        const SyntaxTree& tree = c.trees_[entry.module];
        const uint32 members = tree.Child(entry.node, 3);
        if (members == NoNode || tree.Node(members).kind != SyntaxKind::List) {
            continue;
        }

        for (const uint32 member : tree.Children(members)) {
            const uint32 initializer =
                tree.Node(member).kind == SyntaxKind::Field ? tree.Child(member, 4) : NoNode;
            if (initializer == NoNode) {
                continue;
            }

            pending.Clear();
            if (!pending.TryPushBack(initializer)) {
                binder_.error_ = ErrorCode::OutOfMemory;
                return;
            }

            while (!pending.IsEmpty()) {
                const uint32 node = pending[pending.Size() - 1];
                static_cast<void>(pending.TryResize(pending.Size() - 1));
                if (tree.Node(node).kind == SyntaxKind::Call) {
                    const uint32 callee = c.NodeSymbol(entry.module, tree.Child(node, 0));
                    uint32 whole = initializer;
                    while (whole != NoNode && tree.Node(whole).kind == SyntaxKind::Parenthesized) {
                        whole = tree.Child(whole, 0);
                    }

                    if (callee != NoSymbol && callee < symbolCount &&
                        local[callee] != NoLocalIndex &&
                        (!edges.TryPushBack(StructEdge{local[callee], node}) ||
                         !direct.TryPushBack(node == whole ? 1 : 0))) {
                        binder_.error_ = ErrorCode::OutOfMemory;
                        return;
                    }
                }

                for (const uint32 child : tree.Children(node)) {
                    if (child != NoNode && !pending.TryPushBack(child)) {
                        binder_.error_ = ErrorCode::OutOfMemory;
                        return;
                    }
                }
            }
        }
    }

    if (!firstEdge.TryPushBack(static_cast<uint32>(edges.Size()))) {
        binder_.error_ = ErrorCode::OutOfMemory;
        return;
    }

    if (edges.IsEmpty()) {
        return;
    }

    // 역 간선(CSR).
    Array<uint32> reverseFirst(allocator_);
    Array<uint32> reverse(allocator_);
    if (!reverseFirst.TryResize(count + 1) || !reverse.TryResize(edges.Size())) {
        binder_.error_ = ErrorCode::OutOfMemory;
        return;
    }

    for (uint32 at = 0; at <= count; ++at) {
        reverseFirst[at] = 0;
    }

    for (const StructEdge& edge : edges) {
        ++reverseFirst[edge.target + 1];
    }

    for (uint32 at = 0; at < count; ++at) {
        reverseFirst[at + 1] += reverseFirst[at];
    }

    {
        Array<uint32> cursor(allocator_);
        if (!cursor.TryResize(count)) {
            binder_.error_ = ErrorCode::OutOfMemory;
            return;
        }

        for (uint32 at = 0; at < count; ++at) {
            cursor[at] = reverseFirst[at];
        }

        for (uint32 source = 0; source < count; ++source) {
            for (uint32 at = firstEdge[source]; at < firstEdge[source + 1]; ++at) {
                reverse[cursor[edges[at].target]++] = source;
            }
        }
    }

    // 1) 끝난 순서(반복 DFS — 프레임은 (노드, 다음 간선)).
    Array<uint32> finished(allocator_);
    Array<uint8> seen(allocator_);
    Array<TarjanFrame> frames(allocator_);
    if (!seen.TryResize(count)) {
        binder_.error_ = ErrorCode::OutOfMemory;
        return;
    }

    for (uint32 at = 0; at < count; ++at) {
        seen[at] = 0;
    }

    for (uint32 root = 0; root < count; ++root) {
        if (seen[root] != 0) {
            continue;
        }

        seen[root] = 1;
        if (!frames.TryPushBack(TarjanFrame{root, firstEdge[root]})) {
            binder_.error_ = ErrorCode::OutOfMemory;
            return;
        }

        while (!frames.IsEmpty()) {
            TarjanFrame& frame = frames[frames.Size() - 1];
            if (frame.edge < firstEdge[frame.node + 1]) {
                const uint32 next = edges[frame.edge++].target;
                if (seen[next] == 0) {
                    seen[next] = 1;
                    if (!frames.TryPushBack(TarjanFrame{next, firstEdge[next]})) {
                        binder_.error_ = ErrorCode::OutOfMemory;
                        return;
                    }
                }

                continue;
            }

            if (!finished.TryPushBack(frame.node)) {
                binder_.error_ = ErrorCode::OutOfMemory;
                return;
            }

            static_cast<void>(frames.TryResize(frames.Size() - 1));
        }
    }

    // 2) 역 그래프를 끝난 순서의 거꾸로 돌며 묶음 번호를 준다.
    Array<uint32> component(allocator_);
    if (!component.TryResize(count)) {
        binder_.error_ = ErrorCode::OutOfMemory;
        return;
    }

    for (uint32 at = 0; at < count; ++at) {
        component[at] = NoLocalIndex;
    }

    uint32 components = 0;
    for (usize order = finished.Size(); order > 0; --order) {
        const uint32 root = finished[order - 1];
        if (component[root] != NoLocalIndex) {
            continue;
        }

        pending.Clear();
        component[root] = components;
        if (!pending.TryPushBack(root)) {
            binder_.error_ = ErrorCode::OutOfMemory;
            return;
        }

        while (!pending.IsEmpty()) {
            const uint32 node = pending[pending.Size() - 1];
            static_cast<void>(pending.TryResize(pending.Size() - 1));
            for (uint32 at = reverseFirst[node]; at < reverseFirst[node + 1]; ++at) {
                if (component[reverse[at]] == NoLocalIndex) {
                    component[reverse[at]] = components;
                    if (!pending.TryPushBack(reverse[at])) {
                        binder_.error_ = ErrorCode::OutOfMemory;
                        return;
                    }
                }
            }
        }

        ++components;
    }

    // 3) 값 필드 생성만이 아닌(컨테이너 안 등) 간선이 있는 묶음을 고른다.
    Array<uint8> reported(allocator_);
    if (!reported.TryResize(components)) {
        binder_.error_ = ErrorCode::OutOfMemory;
        return;
    }

    for (uint32 at = 0; at < components; ++at) {
        reported[at] = 0;
    }

    for (uint32 source = 0; source < count; ++source) {
        for (uint32 at = firstEdge[source]; at < firstEdge[source + 1]; ++at) {
            if (component[edges[at].target] == component[source] && direct[at] == 0) {
                reported[component[source]] = 1;
            }
        }
    }

    // 4) 그 묶음 안의 생성 간선마다 S0429(선언 순).
    for (uint32 source = 0; source < count && binder_.error_ == ErrorCode::None; ++source) {
        const Symbol& owner = c.symbols_[structs[source]];
        for (uint32 at = firstEdge[source]; at < firstEdge[source + 1]; ++at) {
            const StructEdge& edge = edges[at];
            if (component[edge.target] != component[source] || reported[component[source]] == 0) {
                continue;
            }

            CheckContext context;
            context.module = owner.module;
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("The default value of struct '{}' constructs '{}', which leads "
                                 "back to '{}' without end.",
                                 owner.name, c.symbols_[structs[edge.target]].name, owner.name);
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::StructDefaultCycle, edge.field, message.View(),
                       "Field initializers run every time the struct is created, so a struct "
                       "whose initializers create it again has no finite default.",
                       "Start the field empty (for example '[]') and fill it in a function.");
            }
        }
    }
}

} // namespace rocks::sparks::detail
