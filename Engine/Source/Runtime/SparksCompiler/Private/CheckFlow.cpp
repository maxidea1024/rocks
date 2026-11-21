#include "CompilationChecker.h"
#include "TypeChecker.h"

// 흐름 분석이다(Docs/Design/SparksLanguage.ko.md 3.11절): await 뒤의 객체 지역 이름(S0412 — 타입
// 검사 전에 읽기마다 정한다), 읽지 않는 지역 이름·인자(S0710·S0712)와 그 뒤 읽지 않는 대입 값
// (S0711 — 타입 검사 뒤), 쓰지 않는 import(S0714). LC2d 단계(Flow)에서만 돈다.
//
// 함수 본문을 단계(FlowStep)의 그래프로 바꾸고 worklist로 고정점을 구한다(중첩 반복을 다시 도는
// 비용이 중첩 깊이에 지수로 늘지 않게). 문장 재귀는 구문 트리의 깊이 상한(S0215) 안이고 식은
// 반복으로 훑는다.

namespace rocks::sparks::detail {

namespace {

constexpr uint32 NoFlowStep = NumericLimits<uint32>::Max();
constexpr uint32 FlowWordBits = 64;

uint32 FlowWordCount(uint32 bits) noexcept {
    return (bits + FlowWordBits - 1) / FlowWordBits;
}

bool FlowBit(const uint64* words, uint32 index) noexcept {
    return (words[index / FlowWordBits] >> (index % FlowWordBits) & 1u) != 0;
}

void SetFlowBit(uint64* words, uint32 index) noexcept {
    words[index / FlowWordBits] |= uint64(1) << (index % FlowWordBits);
}

void ClearFlowBit(uint64* words, uint32 index) noexcept {
    words[index / FlowWordBits] &= ~(uint64(1) << (index % FlowWordBits));
}

} // namespace

class FlowAnalyzer final {
public:
    enum class Mode : uint8 {
        Stale, // await 뒤 상태(비트 = 지역 번호)
        Stores // 닿는 대입 값(비트 = 대입 값 번호)
    };

    FlowAnalyzer(TypeChecker& checker, const CheckContext& context, uint32 function,
                 Mode mode) noexcept
        : checker_(checker), c_(checker.compilation_), tree_(c_.trees_[context.module]),
          context_(context), function_(function), mode_(mode), locals_(checker.allocator_),
          localIndex_(checker.allocator_), localRead_(checker.allocator_),
          steps_(checker.allocator_), edgeFrom_(checker.allocator_), edgeTo_(checker.allocator_),
          firstEdge_(checker.allocator_), targets_(checker.allocator_),
          storeLocal_(checker.allocator_), storeUsed_(checker.allocator_),
          firstStore_(checker.allocator_), storesByLocal_(checker.allocator_),
          states_(checker.allocator_), reached_(checker.allocator_) {
    }

    // 지역 이름을 모으고 그래프를 만들어 고정점을 구한 뒤 읽기마다 판정한다. Stale은 "기다린 뒤"
    // 상태로 도달하는 지역 이름 읽기를 TypeChecker::staleRead_에 표시한다. 판정할 것이
    // 없거나(Stale: await 없음, Stores: 해석하지 못한 이름) 할당에 실패하면 false다(오류는
    // binder에).
    bool Run() noexcept;
    // Stores: S0710·S0711·S0712를 낸다.
    void ReportUnused() noexcept;

private:
    enum class StepKind : uint8 {
        Join,   // 합류(아무것도 하지 않는다)
        Unit,   // 식 하나를 계산한다(읽기, await)
        Read,   // 식별자 하나를 읽는다(emit 대상 이름)
        Define, // 지역 이름에 새 값(선언·`if (let)`·반복 변수)
        Write   // 지역 이름에 쓴다(대입)
    };

    struct Step {
        StepKind kind = StepKind::Join;
        bool await = false;          // Unit: 식에 await가 있다
        bool whole = false;          // Write: `=`로 이름 전체에 쓴다
        uint32 node = NoNode;        // Unit: 식, Read: 식별자, Write: 대입 문장
        uint32 exclude = NoNode;     // Unit: 읽기가 아닌 뿌리 식별자(값 타입 경로의 쓰기)
        uint32 local = NoLocalIndex; // Define·Write: 지역 번호
        uint32 store = NoLocalIndex; // Write: 대입 값 번호(S0711 대상이 아니면 NoLocalIndex)
    };

    struct Loop {
        uint32 head = NoFlowStep;
        Array<uint32> breaks;
        explicit Loop(Allocator& allocator) noexcept : breaks(allocator) {
        }
    };

    bool Failed() const noexcept {
        return checker_.binder_.error_ != ErrorCode::None;
    }

    void Fail(ErrorCode error) noexcept {
        checker_.binder_.error_ = error;
    }

    // 지역 번호(이 함수의 인자·지역 이름이 아니면 NoLocalIndex)다.
    uint32 LocalOf(uint32 symbol) const noexcept;
    uint32 LocalOfNode(uint32 nameNode) const noexcept;
    bool CollectLocals() noexcept;
    bool AddLocal(uint32 nameNode) noexcept;

    uint32 AddStep(const Step& step, uint32 predecessor) noexcept;
    void AddEdge(uint32 from, uint32 to) noexcept;
    uint32 AddUnit(uint32 expression, uint32 current, uint32 exclude = NoNode) noexcept;
    uint32 AddDefine(uint32 nameNode, uint32 current) noexcept;
    uint32 Join(uint32 left, uint32 right) noexcept;
    uint32 Build(uint32 node, uint32 current, Loop* loop) noexcept;
    uint32 BuildAssignment(uint32 node, uint32 current) noexcept;
    bool ContainsAwait(uint32 expression) noexcept;
    // 괄호를 벗긴 식이 리터럴 `true`인지다.
    bool IsLiteralTrue(uint32 expression) const noexcept;
    bool IsVarLocal(uint32 local) const noexcept;

    bool Solve() noexcept;
    void Transfer(const Step& step, uint64* words) const noexcept;
    // 식 하나의 지역 이름 읽기마다 read(식별자, 지역 번호)를 부른다(exclude 식별자는 뺀다).
    template <typename Read>
    void ForEachRead(uint32 expression, uint32 exclude, Read&& read) noexcept;
    void ApplyRead(uint32 step, uint32 identifier, uint32 local) noexcept;
    void UseStores(uint32 local, const uint64* words) noexcept;
    void ApplySteps() noexcept;
    bool HasFixedSignature() const noexcept;

    TypeChecker& checker_;
    Compilation& c_;
    const SyntaxTree& tree_;
    const CheckContext& context_;
    uint32 function_;
    Mode mode_;
    // 지역 번호 → 심볼, (심볼 - localBase_) → 지역 번호.
    Array<uint32> locals_;
    Array<uint32> localIndex_;
    uint32 localBase_ = 0;
    Array<uint8> localRead_;
    bool unresolved_ = false; // 이름을 해석하지 못한 식별자가 있다(3.11절 연쇄 방지)
    bool hasAwait_ = false;
    Array<Step> steps_;
    Array<uint32> edgeFrom_;
    Array<uint32> edgeTo_;
    Array<uint32> firstEdge_; // 단계 → targets_ 구간 시작(단계 수 + 1개)
    Array<uint32> targets_;
    // 대입 값 번호 → 지역 번호·읽었는지, 지역 번호 → storesByLocal_ 구간.
    Array<uint32> storeLocal_;
    Array<uint8> storeUsed_;
    Array<uint32> firstStore_;
    Array<uint32> storesByLocal_;
    uint32 words_ = 0;     // 상태 하나의 word 수
    Array<uint64> states_; // 단계마다 들어오는 상태(words_개씩)
    Array<uint8> reached_; // 단계에 닿는 경로가 있는지
};

uint32 FlowAnalyzer::LocalOf(uint32 symbol) const noexcept {
    if (symbol == NoSymbol || symbol < localBase_ || symbol - localBase_ >= localIndex_.Size()) {
        return NoLocalIndex;
    }

    return localIndex_[symbol - localBase_];
}

uint32 FlowAnalyzer::LocalOfNode(uint32 nameNode) const noexcept {
    if (nameNode == NoNode || tree_.Node(nameNode).kind != SyntaxKind::Name) {
        return NoLocalIndex;
    }

    return LocalOf(c_.NodeSymbol(context_.module, nameNode));
}

bool FlowAnalyzer::AddLocal(uint32 nameNode) noexcept {
    if (nameNode == NoNode || tree_.Node(nameNode).kind != SyntaxKind::Name) {
        return true; // `_`·구문 오류
    }

    const uint32 symbol = c_.NodeSymbol(context_.module, nameNode);
    if (symbol == NoSymbol) {
        return true;
    }

    const SymbolKind kind = c_.symbols_[symbol].kind;
    if (kind != SymbolKind::Local && kind != SymbolKind::Parameter) {
        return true;
    }

    if (auto pushed = locals_.TryPushBack(symbol); !pushed) {
        Fail(pushed.Error());
        return false;
    }

    return true;
}

bool FlowAnalyzer::CollectLocals() noexcept {
    Array<uint32> pending(checker_.allocator_);
    if (auto pushed = pending.TryPushBack(function_); !pushed) {
        Fail(pushed.Error());
        return false;
    }

    const uint32 base = c_.nodeBase_[context_.module];
    while (!pending.IsEmpty()) {
        const uint32 node = pending.TryPopBack().Value(); // 비어 있지 않다
        const SyntaxNode& entry = tree_.Node(node);
        bool ok = true;
        switch (entry.kind) {
        case SyntaxKind::Parameter:
        case SyntaxKind::LocalVariable:
        case SyntaxKind::IfLet:
            ok = AddLocal(tree_.Child(node, 0));
            break;
        case SyntaxKind::For:
            ok = AddLocal(tree_.Child(node, 0)) && AddLocal(tree_.Child(node, 1));
            break;
        case SyntaxKind::Identifier:
            // 해석하지 못한 이름(S0317 등)은 NoSymbol로 기록되어 있다.
            unresolved_ = unresolved_ || c_.nodeSymbols_[base + node] == NoSymbol;
            break;
        case SyntaxKind::Await:
            hasAwait_ = true;
            break;
        case SyntaxKind::Try:
            hasAwait_ = hasAwait_ || (entry.flags & SyntaxFlags::Await) != 0;
            break;
        default:
            break;
        }

        if (!ok) {
            return false;
        }

        for (const uint32 child : tree_.Children(node)) {
            if (child == NoNode) {
                continue;
            }

            if (auto pushed = pending.TryPushBack(child); !pushed) {
                Fail(pushed.Error());
                return false;
            }
        }
    }

    if (locals_.IsEmpty()) {
        return true;
    }

    uint32 low = locals_[0];
    uint32 high = locals_[0];
    for (const uint32 symbol : locals_) {
        low = symbol < low ? symbol : low;
        high = symbol > high ? symbol : high;
    }

    localBase_ = low;
    if (auto resized = localIndex_.TryResize(usize(high - low) + 1); !resized) {
        Fail(resized.Error());
        return false;
    }

    for (uint32& index : localIndex_) {
        index = NoLocalIndex;
    }

    for (uint32 index = 0; index < locals_.Size(); ++index) {
        localIndex_[locals_[index] - low] = index;
    }

    if (auto resized = localRead_.TryResize(locals_.Size()); !resized) {
        Fail(resized.Error());
        return false;
    }

    for (uint8& read : localRead_) {
        read = 0;
    }

    return true;
}

uint32 FlowAnalyzer::AddStep(const Step& step, uint32 predecessor) noexcept {
    if (Failed()) {
        return NoFlowStep;
    }

    const uint32 index = static_cast<uint32>(steps_.Size());
    if (auto pushed = steps_.TryPushBack(step); !pushed) {
        Fail(pushed.Error());
        return NoFlowStep;
    }

    AddEdge(predecessor, index);
    return index;
}

void FlowAnalyzer::AddEdge(uint32 from, uint32 to) noexcept {
    if (from == NoFlowStep || to == NoFlowStep || Failed()) {
        return;
    }

    if (auto pushed = edgeFrom_.TryPushBack(from); !pushed) {
        Fail(pushed.Error());
        return;
    }

    if (auto pushed = edgeTo_.TryPushBack(to); !pushed) {
        Fail(pushed.Error());
    }
}

bool FlowAnalyzer::ContainsAwait(uint32 expression) noexcept {
    Array<uint32> pending(checker_.allocator_);
    if (auto pushed = pending.TryPushBack(expression); !pushed) {
        Fail(pushed.Error());
        return false;
    }

    while (!pending.IsEmpty()) {
        const uint32 node = pending.TryPopBack().Value(); // 비어 있지 않다
        const SyntaxNode& entry = tree_.Node(node);
        if (entry.kind == SyntaxKind::Await ||
            (entry.kind == SyntaxKind::Try && (entry.flags & SyntaxFlags::Await) != 0)) {
            return true;
        }

        for (const uint32 child : tree_.Children(node)) {
            if (child == NoNode) {
                continue;
            }

            if (auto pushed = pending.TryPushBack(child); !pushed) {
                Fail(pushed.Error());
                return false;
            }
        }
    }

    return false;
}

bool FlowAnalyzer::IsLiteralTrue(uint32 expression) const noexcept {
    while (expression != NoNode && tree_.Node(expression).kind == SyntaxKind::Parenthesized) {
        expression = tree_.Child(expression, 0);
    }

    return expression != NoNode && tree_.Node(expression).kind == SyntaxKind::BoolLiteral &&
           checker_.TokenText(context_.module, expression) == "true";
}

uint32 FlowAnalyzer::AddUnit(uint32 expression, uint32 current, uint32 exclude) noexcept {
    if (expression == NoNode) {
        return current;
    }

    Step step;
    step.kind = StepKind::Unit;
    step.node = expression;
    step.exclude = exclude;
    step.await = mode_ == Mode::Stale && ContainsAwait(expression);
    return AddStep(step, current);
}

uint32 FlowAnalyzer::AddDefine(uint32 nameNode, uint32 current) noexcept {
    const uint32 local = LocalOfNode(nameNode);
    if (local == NoLocalIndex) {
        return current;
    }

    Step step;
    step.kind = StepKind::Define;
    step.local = local;
    return AddStep(step, current);
}

uint32 FlowAnalyzer::Join(uint32 left, uint32 right) noexcept {
    if (left == NoFlowStep) {
        return right;
    }

    if (right == NoFlowStep || right == left) {
        return left;
    }

    const uint32 join = AddStep(Step{}, left);
    AddEdge(right, join);
    return join;
}

bool FlowAnalyzer::IsVarLocal(uint32 local) const noexcept {
    const Symbol& entry = c_.symbols_[locals_[local]];
    return entry.kind == SymbolKind::Local && (entry.flags & SymbolFlags::Let) == 0 &&
           entry.node != NoNode && tree_.Node(entry.node).kind == SyntaxKind::LocalVariable;
}

uint32 FlowAnalyzer::BuildAssignment(uint32 node, uint32 current) noexcept {
    // 오른쪽 값을 먼저 계산하고 왼쪽 경로를 그 뒤에 읽는다(3.11절).
    const uint32 target = tree_.Child(node, 0);
    current = AddUnit(tree_.Child(node, 1), current);
    if (target == NoNode) {
        return current;
    }

    // 맨 바깥이 이름·멤버·원소가 아닌 대상(S0211 — `(x) = 1`)은 쓰기가 아니라 읽기다.
    const SyntaxKind targetKind = tree_.Node(target).kind;
    if (targetKind != SyntaxKind::Identifier && targetKind != SyntaxKind::Member &&
        targetKind != SyntaxKind::Index) {
        return AddUnit(target, current);
    }

    // 뿌리 이름과, 참조 타입 값을 지나는지(그러면 뿌리는 읽기다 — Stores만 타입을 안다).
    uint32 root = target;
    bool crosses = false;
    while (root != NoNode) {
        const SyntaxKind kind = tree_.Node(root).kind;
        if (kind == SyntaxKind::Member || kind == SyntaxKind::Index) {
            const uint32 object = tree_.Child(root, 0);
            if (mode_ == Mode::Stores) {
                const uint32 type = c_.ExpressionType(context_.module, object);
                crosses =
                    crosses || type == NoType || checker_.IsReference(checker_.NonOptional(type));
            }

            root = object;
        } else if (kind == SyntaxKind::Parenthesized) {
            root = tree_.Child(root, 0);
        } else {
            break;
        }
    }

    const uint32 local = root != NoNode && tree_.Node(root).kind == SyntaxKind::Identifier
                             ? LocalOf(c_.NodeSymbol(context_.module, root))
                             : NoLocalIndex;
    const bool direct = root == target;
    if (local == NoLocalIndex || (!direct && (crosses || mode_ == Mode::Stale))) {
        // 지역 이름에 쓰지 않는다(필드·객체 속성 쓰기) — 경로 전체가 읽기다.
        return AddUnit(target, current);
    }

    if (!direct) {
        current = AddUnit(target, current, root); // 번호 식 등(뿌리는 쓰기)
    }

    Step step;
    step.kind = StepKind::Write;
    step.node = node;
    step.local = local;
    step.whole = direct && checker_.binder_.NodeText(context_.module, node) == "=";
    if (mode_ == Mode::Stores && IsVarLocal(local)) {
        step.store = static_cast<uint32>(storeLocal_.Size());
        if (auto pushed = storeLocal_.TryPushBack(local); !pushed) {
            Fail(pushed.Error());
            return NoFlowStep;
        }
    }

    return AddStep(step, current);
}

uint32 FlowAnalyzer::Build(uint32 node, uint32 current, Loop* loop) noexcept {
    if (node == NoNode || Failed()) {
        return current;
    }

    const SyntaxNode& entry = tree_.Node(node);
    switch (entry.kind) {
    case SyntaxKind::Block:
        for (const uint32 statement : tree_.Children(node)) {
            current = Build(statement, current, loop);
        }

        return current;
    case SyntaxKind::If: {
        const uint32 test = tree_.Child(node, 0);
        uint32 head = current;
        uint32 thenStart = current;
        if (test != NoNode && tree_.Node(test).kind == SyntaxKind::IfLet) {
            head = AddUnit(tree_.Child(test, 1), current);
            thenStart = AddDefine(tree_.Child(test, 0), head);
        } else {
            head = AddUnit(test, current);
            thenStart = head;
        }

        const uint32 thenEnd = Build(tree_.Child(node, 1), thenStart, loop);
        const uint32 elseNode = tree_.Child(node, 2);
        const uint32 elseEnd = elseNode == NoNode ? head : Build(elseNode, head, loop);
        return Join(thenEnd, elseEnd);
    }
    case SyntaxKind::While: {
        const uint32 head = AddStep(Step{}, current);
        const uint32 condition = AddUnit(tree_.Child(node, 0), head);
        Loop inner(checker_.allocator_);
        inner.head = head;
        AddEdge(Build(tree_.Child(node, 1), condition, &inner), head);
        // `while (true)`는 break로만 끝난다(조건이 거짓인 경로가 없다 — 3.11절).
        const uint32 exit =
            AddStep(Step{}, IsLiteralTrue(tree_.Child(node, 0)) ? NoFlowStep : condition);
        for (const uint32 from : inner.breaks) {
            AddEdge(from, exit);
        }

        return exit;
    }
    case SyntaxKind::For: {
        current = AddUnit(tree_.Child(node, 2), current);
        current = AddUnit(tree_.Child(node, 3), current);
        const uint32 head = AddStep(Step{}, current);
        uint32 body = AddDefine(tree_.Child(node, 0), head);
        body = AddDefine(tree_.Child(node, 1), body);
        Loop inner(checker_.allocator_);
        inner.head = head;
        AddEdge(Build(tree_.Child(node, 4), body, &inner), head);
        const uint32 exit = AddStep(Step{}, head);
        for (const uint32 from : inner.breaks) {
            AddEdge(from, exit);
        }

        return exit;
    }
    case SyntaxKind::Switch: {
        // default가 없으면 어느 case도 실행하지 않고 지나가는 경로를 둔다(3.11절).
        const uint32 subject = AddUnit(tree_.Child(node, 0), current);
        const uint32 exit = AddStep(Step{}, NoFlowStep);
        const uint32 cases = tree_.Child(node, 1);
        if (cases != NoNode && tree_.Node(cases).kind == SyntaxKind::List) {
            for (const uint32 item : tree_.Children(cases)) {
                if (tree_.Node(item).kind != SyntaxKind::SwitchCase) {
                    continue;
                }

                uint32 start = subject;
                const uint32 values = tree_.Child(item, 0);
                if (values != NoNode && tree_.Node(values).kind == SyntaxKind::List) {
                    for (const uint32 value : tree_.Children(values)) {
                        start = AddUnit(value, start);
                    }
                }

                AddEdge(Build(tree_.Child(item, 1), start, loop), exit);
            }
        }

        const uint32 fallback = tree_.Child(node, 2);
        if (fallback != NoNode && tree_.Node(fallback).kind == SyntaxKind::SwitchDefault) {
            AddEdge(Build(tree_.Child(fallback, 0), subject, loop), exit);
        } else {
            AddEdge(subject, exit);
        }

        return exit;
    }
    case SyntaxKind::Return:
        static_cast<void>(AddUnit(tree_.Child(node, 0), current));
        return NoFlowStep;
    case SyntaxKind::Break:
        if (loop != nullptr && current != NoFlowStep) {
            if (auto pushed = loop->breaks.TryPushBack(current); !pushed) {
                Fail(pushed.Error());
            }
        }

        return NoFlowStep;
    case SyntaxKind::Continue:
        if (loop != nullptr) {
            AddEdge(current, loop->head);
        }

        return NoFlowStep;
    case SyntaxKind::LocalVariable: {
        const uint32 value = AddUnit(tree_.Child(node, 2), current);
        const uint32 exit = tree_.Child(node, 3);
        if (exit != NoNode && tree_.Node(exit).kind == SyntaxKind::ExitReturn) {
            static_cast<void>(AddUnit(tree_.Child(exit, 0), value)); // `?? return e`: 경로 끝
        }

        return AddDefine(tree_.Child(node, 0), value);
    }
    case SyntaxKind::Assignment:
        return BuildAssignment(node, current);
    case SyntaxKind::Emit: {
        // 대상 이름(`emit target.Interacted(...)`의 `target`)은 읽기다.
        const uint32 path = tree_.Child(node, 0);
        if (path != NoNode && tree_.Node(path).kind == SyntaxKind::EventPath &&
            tree_.Node(path).childCount > 1) {
            const uint32 first = tree_.Child(path, 0);
            if (tree_.Node(first).kind == SyntaxKind::Name &&
                LocalOf(c_.NodeSymbol(context_.module, first)) != NoLocalIndex) {
                Step step;
                step.kind = StepKind::Read;
                step.node = first;
                current = AddStep(step, current);
            }
        }

        const uint32 arguments = tree_.Child(node, 1);
        if (arguments != NoNode && tree_.Node(arguments).kind == SyntaxKind::List) {
            for (const uint32 argument : tree_.Children(arguments)) {
                current = AddUnit(argument, current);
            }
        }

        return current;
    }
    case SyntaxKind::ExpressionStatement:
    case SyntaxKind::DiscardAssignment:
        return AddUnit(tree_.Child(node, 0), current);
    default:
        return current; // 구문 오류
    }
}

void FlowAnalyzer::Transfer(const Step& step, uint64* words) const noexcept {
    switch (step.kind) {
    case StepKind::Unit:
        if (step.await) {
            // await를 지나면 모든 지역 이름이 "기다린 뒤" 상태다(아직 선언하지 않은 이름은 선언이
            // 풀므로 함께 둬도 된다).
            for (uint32 local = 0; local < locals_.Size(); ++local) {
                SetFlowBit(words, local);
            }
        }

        break;
    case StepKind::Define:
    case StepKind::Write:
        if (mode_ == Mode::Stale) {
            ClearFlowBit(words, step.local);
            break;
        }

        if (step.kind == StepKind::Write && !step.whole && step.store == NoLocalIndex) {
            break; // 쓸 수 없는 이름(S0701·S0702)의 부분 쓰기 — 앞 값은 그대로 닿는다
        }

        for (uint32 index = firstStore_[step.local]; index < firstStore_[step.local + 1]; ++index) {
            ClearFlowBit(words, storesByLocal_[index]);
        }

        if (step.store != NoLocalIndex) {
            SetFlowBit(words, step.store);
        }

        break;
    default:
        break;
    }
}

bool FlowAnalyzer::Solve() noexcept {
    const uint32 stepCount = static_cast<uint32>(steps_.Size());
    const uint32 bits = mode_ == Mode::Stale ? static_cast<uint32>(locals_.Size())
                                             : static_cast<uint32>(storeLocal_.Size());
    words_ = FlowWordCount(bits);

    // 간선을 단계별 후속 목록으로 모은다.
    if (auto resized = firstEdge_.TryResize(usize(stepCount) + 1); !resized) {
        Fail(resized.Error());
        return false;
    }

    for (uint32& first : firstEdge_) {
        first = 0;
    }

    for (const uint32 from : edgeFrom_) {
        ++firstEdge_[from + 1];
    }

    for (uint32 index = 0; index < stepCount; ++index) {
        firstEdge_[index + 1] += firstEdge_[index];
    }

    if (auto resized = targets_.TryResize(edgeTo_.Size()); !resized) {
        Fail(resized.Error());
        return false;
    }

    {
        Array<uint32> fill(checker_.allocator_);
        if (auto resized = fill.TryResize(stepCount); !resized) {
            Fail(resized.Error());
            return false;
        }

        for (uint32 index = 0; index < stepCount; ++index) {
            fill[index] = firstEdge_[index];
        }

        for (usize edge = 0; edge < edgeFrom_.Size(); ++edge) {
            targets_[fill[edgeFrom_[edge]]++] = edgeTo_[edge];
        }
    }

    // 지역 이름마다 그 이름의 대입 값 목록이다(Stores).
    if (auto resized = firstStore_.TryResize(locals_.Size() + 1); !resized) {
        Fail(resized.Error());
        return false;
    }

    for (uint32& first : firstStore_) {
        first = 0;
    }

    for (const uint32 local : storeLocal_) {
        ++firstStore_[local + 1];
    }

    for (usize index = 0; index < locals_.Size(); ++index) {
        firstStore_[index + 1] += firstStore_[index];
    }

    if (auto resized = storesByLocal_.TryResize(storeLocal_.Size()); !resized) {
        Fail(resized.Error());
        return false;
    }

    if (auto resized = storeUsed_.TryResize(storeLocal_.Size()); !resized) {
        Fail(resized.Error());
        return false;
    }

    {
        Array<uint32> fill(checker_.allocator_);
        if (auto resized = fill.TryResize(locals_.Size()); !resized) {
            Fail(resized.Error());
            return false;
        }

        for (usize index = 0; index < locals_.Size(); ++index) {
            fill[index] = firstStore_[index];
        }

        for (uint32 store = 0; store < storeLocal_.Size(); ++store) {
            storesByLocal_[fill[storeLocal_[store]]++] = store;
            storeUsed_[store] = 0;
        }
    }

    if (auto resized = states_.TryResize(usize(stepCount) * words_); !resized) {
        Fail(resized.Error());
        return false;
    }

    if (auto resized = reached_.TryResize(stepCount); !resized) {
        Fail(resized.Error());
        return false;
    }

    for (uint64& word : states_) {
        word = 0;
    }

    Array<uint8> queued(checker_.allocator_);
    Array<uint32> work(checker_.allocator_);
    Array<uint64> out(checker_.allocator_);
    if (auto resized = queued.TryResize(stepCount); !resized) {
        Fail(resized.Error());
        return false;
    }

    if (auto resized = out.TryResize(words_); !resized) {
        Fail(resized.Error());
        return false;
    }

    for (uint32 index = 0; index < stepCount; ++index) {
        reached_[index] = 0;
        queued[index] = 0;
    }

    // 0번 단계가 함수 입구다(아무 이름도 "기다린 뒤"가 아니고 닿는 대입 값이 없다).
    reached_[0] = 1;
    queued[0] = 1;
    if (auto pushed = work.TryPushBack(0); !pushed) {
        Fail(pushed.Error());
        return false;
    }

    while (!work.IsEmpty()) {
        const uint32 step = work.TryPopBack().Value(); // 비어 있지 않다
        queued[step] = 0;
        for (uint32 word = 0; word < words_; ++word) {
            out[word] = states_[usize(step) * words_ + word];
        }

        Transfer(steps_[step], out.Data());
        for (uint32 edge = firstEdge_[step]; edge < firstEdge_[step + 1]; ++edge) {
            const uint32 next = targets_[edge];
            bool changed = reached_[next] == 0;
            reached_[next] = 1;
            for (uint32 word = 0; word < words_; ++word) {
                uint64& into = states_[usize(next) * words_ + word];
                const uint64 merged = into | out[word];
                changed = changed || merged != into;
                into = merged;
            }

            if (changed && queued[next] == 0) {
                queued[next] = 1;
                if (auto pushed = work.TryPushBack(next); !pushed) {
                    Fail(pushed.Error());
                    return false;
                }
            }
        }
    }

    return true;
}

template <typename Read>
void FlowAnalyzer::ForEachRead(uint32 expression, uint32 exclude, Read&& read) noexcept {
    Array<uint32> pending(checker_.allocator_);
    if (auto pushed = pending.TryPushBack(expression); !pushed) {
        Fail(pushed.Error());
        return;
    }

    while (!pending.IsEmpty()) {
        const uint32 node = pending.TryPopBack().Value(); // 비어 있지 않다
        if (tree_.Node(node).kind == SyntaxKind::Identifier && node != exclude) {
            const uint32 local = LocalOf(c_.NodeSymbol(context_.module, node));
            if (local != NoLocalIndex) {
                read(node, local);
            }
        }

        for (const uint32 child : tree_.Children(node)) {
            if (child == NoNode) {
                continue;
            }

            if (auto pushed = pending.TryPushBack(child); !pushed) {
                Fail(pushed.Error());
                return;
            }
        }
    }
}

void FlowAnalyzer::UseStores(uint32 local, const uint64* words) noexcept {
    for (uint32 index = firstStore_[local]; index < firstStore_[local + 1]; ++index) {
        const uint32 store = storesByLocal_[index];
        if (FlowBit(words, store)) {
            storeUsed_[store] = 1;
        }
    }
}

void FlowAnalyzer::ApplyRead(uint32 step, uint32 identifier, uint32 local) noexcept {
    localRead_[local] = 1; // 닿지 않는 코드의 읽기도 읽기다(S0710)
    if (reached_[step] == 0) {
        return;
    }

    const uint64* words = states_.Data() + usize(step) * words_;
    if (mode_ == Mode::Stale) {
        if (FlowBit(words, local)) {
            checker_.staleRead_[identifier] = 1;
        }

        return;
    }

    UseStores(local, words);
}

void FlowAnalyzer::ApplySteps() noexcept {
    for (uint32 step = 0; step < steps_.Size() && !Failed(); ++step) {
        const Step& entry = steps_[step];
        switch (entry.kind) {
        case StepKind::Unit:
            ForEachRead(entry.node, entry.exclude, [&](uint32 identifier, uint32 local) noexcept {
                ApplyRead(step, identifier, local);
            });
            break;
        case StepKind::Read:
            ApplyRead(step, entry.node, LocalOf(c_.NodeSymbol(context_.module, entry.node)));
            break;
        case StepKind::Write:
            // 복합 대입·멤버·원소 쓰기는 앞 값을 바탕으로 한다(앞 값의 읽기 — S0711).
            if (mode_ == Mode::Stores && !entry.whole && reached_[step] != 0) {
                UseStores(entry.local, states_.Data() + usize(step) * words_);
            }

            break;
        default:
            break;
        }
    }
}

bool FlowAnalyzer::Run() noexcept {
    if (!CollectLocals()) {
        return false;
    }

    if (mode_ == Mode::Stale && (!hasAwait_ || locals_.IsEmpty())) {
        return false; // await가 없으면 "기다린 뒤"인 읽기가 없다
    }

    // 구문 오류 복구가 건너뛴 원문의 읽기는 보이지 않는다(3.11절 연쇄 방지).
    if (mode_ == Mode::Stores &&
        (unresolved_ || checker_.binder_.moduleStates_[context_.module].syntaxErrors)) {
        return false;
    }

    const SyntaxKind kind = tree_.Node(function_).kind;
    const uint32 body = tree_.Child(function_, kind == SyntaxKind::Handler ? 4 : 5);
    if (body == NoNode || tree_.Node(body).kind != SyntaxKind::Block) {
        return false;
    }

    const uint32 entry = AddStep(Step{}, NoFlowStep);
    static_cast<void>(Build(body, entry, nullptr));
    if (Failed() || !Solve()) {
        return false;
    }

    ApplySteps();
    return !Failed();
}

bool FlowAnalyzer::HasFixedSignature() const noexcept {
    const SyntaxNode& entry = tree_.Node(function_);
    if (entry.kind == SyntaxKind::Handler) {
        return true;
    }

    if ((entry.flags & (SyntaxFlags::Override | SyntaxFlags::Open)) != 0) {
        return true;
    }

    const uint32 symbol = c_.NodeSymbol(context_.module, tree_.Child(function_, 2));
    return symbol != NoSymbol && context_.owner != NoSymbol &&
           checker_.ImplementsInterfaceMethod(context_.owner, c_.symbols_[symbol].name);
}

void FlowAnalyzer::ReportUnused() noexcept {
    // S0710: 읽지 않는 지역 이름(인자는 아래 S0712).
    for (uint32 local = 0; local < locals_.Size(); ++local) {
        const Symbol& entry = c_.symbols_[locals_[local]];
        if (entry.kind != SymbolKind::Local || localRead_[local] != 0) {
            continue;
        }

        const SyntaxKind declaration =
            entry.node == NoNode ? SyntaxKind::Error : tree_.Node(entry.node).kind;
        StringBuilder<CompilationMessageInlineBytes> message(checker_.allocator_);
        message.AppendFormat("'{}' is never read.", entry.name);
        if (!checker_.binder_.Succeeded(message)) {
            return;
        }

        checker_.Report(context_, DiagnosticCode::UnusedLocal, entry.nameNode, message.View(),
                        "A name that is declared but never used is usually a mistake.",
                        declaration == SyntaxKind::For ? StringView("Name the loop variable '_'.")
                        : declaration == SyntaxKind::IfLet
                            ? StringView("Test it with 'value != null' instead.")
                            : StringView("Remove it, or use the value."));
    }

    // S0711: 그 뒤 어느 경로에서도 읽지 않는 대입 값(어디에서도 읽지 않는 이름은 S0710만, 닿지 않는
    // 대입은 알리지 않는다).
    for (uint32 index = 0; index < steps_.Size(); ++index) {
        const Step& step = steps_[index];
        if (step.kind != StepKind::Write || step.store == NoLocalIndex || reached_[index] == 0 ||
            storeUsed_[step.store] != 0 || localRead_[step.local] == 0) {
            continue;
        }

        const uint32 target = tree_.Child(step.node, 0);
        const bool copy = tree_.Node(target).kind != SyntaxKind::Identifier;
        const Symbol& entry = c_.symbols_[locals_[step.local]];
        StringBuilder<CompilationMessageInlineBytes> message(checker_.allocator_);
        if (copy) {
            message.AppendFormat("The change to '{}' is never read.", entry.name);
        } else {
            message.AppendFormat("The value written to '{}' is never read.", entry.name);
        }

        if (!checker_.binder_.Succeeded(message)) {
            return;
        }

        checker_.Report(context_, DiagnosticCode::UnreadStore, target, message.View(),
                        copy ? StringView("A local value is a copy: changing it does not change "
                                          "where it came from.")
                             : StringView("No path reads it before it is overwritten or the "
                                          "function ends."),
                        copy ? StringView("Write the copy back, as in 'Hinge.RelativeLocation = "
                                          "p;', or remove the change.")
                             : StringView("Remove the assignment, or use the value."));
    }

    // S0712: 읽지 않는 인자와 시그니처가 정해지지 않은 함수의 `_` 인자.
    const uint32 parameters = tree_.Child(function_, 3);
    if (parameters == NoNode || tree_.Node(parameters).kind != SyntaxKind::List) {
        return;
    }

    // 선언 이름에 진단이 있는 함수(S0313 등)는 시그니처를 정했는지 알 수 없다(3.11절 연쇄 방지).
    if (tree_.Node(function_).kind == SyntaxKind::Function) {
        const uint32 symbol = c_.NodeSymbol(context_.module, tree_.Child(function_, 2));
        if (symbol == NoSymbol || (c_.symbols_[symbol].flags & SymbolFlags::Erroneous) != 0) {
            return;
        }
    }

    const bool fixed = HasFixedSignature();
    for (const uint32 parameter : tree_.Children(parameters)) {
        if (tree_.Node(parameter).kind != SyntaxKind::Parameter) {
            continue;
        }

        const uint32 nameNode = tree_.Child(parameter, 0);
        if (nameNode == NoNode) {
            continue;
        }

        if (tree_.Node(nameNode).kind == SyntaxKind::Discard) {
            if (!fixed) {
                checker_.Report(context_, DiagnosticCode::UnusedParameter, nameNode,
                                "A parameter named '_' is only for a fixed signature.",
                                "'_' marks a parameter that an override, an interface method or "
                                "an event handler must have.",
                                "Remove the parameter.");
            }

            continue;
        }

        const uint32 local = LocalOfNode(nameNode);
        if (local == NoLocalIndex || localRead_[local] != 0) {
            continue;
        }

        StringBuilder<CompilationMessageInlineBytes> message(checker_.allocator_);
        message.AppendFormat("Parameter '{}' is never read.", c_.symbols_[locals_[local]].name);
        if (!checker_.binder_.Succeeded(message)) {
            return;
        }

        checker_.Report(context_, DiagnosticCode::UnusedParameter, nameNode, message.View(),
                        fixed ? StringView("The signature is fixed by an override, an interface "
                                           "method or an event.")
                              : StringView("A parameter that is never used is usually a mistake."),
                        fixed ? StringView("Name the parameter '_'.")
                              : StringView("Remove the parameter, or use it."));
    }
}

void TypeChecker::AnalyzeFlow(const CheckContext& context, uint32 function, bool stale) noexcept {
    FlowAnalyzer analyzer(*this, context, function,
                          stale ? FlowAnalyzer::Mode::Stale : FlowAnalyzer::Mode::Stores);
    if (!analyzer.Run()) {
        return;
    }

    if (!stale) {
        analyzer.ReportUnused();
    }
}

bool TypeChecker::ImplementsInterfaceMethod(uint32 owner, StringView name) const noexcept {
    const Compilation& c = compilation_;
    uint32 step = 0;
    for (uint32 type = owner; type != NoSymbol && step < c.symbols_.Size();
         type = c.symbols_[type].base, ++step) {
        const Symbol& entry = c.symbols_[type];
        if (entry.kind != SymbolKind::Class) {
            break;
        }

        for (uint32 index = 0; index < entry.baseCount; ++index) {
            const uint32 base = c.symbolBases_[entry.firstBase + index];
            const Symbol& baseEntry = c.symbols_[base];
            const bool engineInterface =
                baseEntry.kind == SymbolKind::EngineType &&
                c.schemas_[baseEntry.index].schema.Types()[baseEntry.item].kind ==
                    SchemaTypeKind::Interface;
            if ((baseEntry.kind == SymbolKind::Interface || engineInterface) &&
                binder_.FindMemberInChain(base, name) != NoSymbol) {
                return true;
            }
        }
    }

    return false;
}

void TypeChecker::CheckUnusedImports(uint32 module) noexcept {
    Compilation& c = compilation_;
    // 찾지 못한 이름이 그 import를 가리키려던 것일 수 있고, 구문 오류 복구가 건너뛴 원문의 사용은
    // 보이지 않는다(3.11절 연쇄 방지).
    if (binder_.unresolvedNames_[module] != 0 || binder_.moduleStates_[module].syntaxErrors) {
        return;
    }

    const SyntaxTree& tree = c.trees_[module];
    const auto& owner = c.modules_[module];
    CheckContext context;
    context.module = module;
    // attribute 인자는 LC3 전까지 결합하지 않으므로 그 안의 식별자 글자를 모아 이름으로 본다.
    Array<StringView> attributeNames(allocator_);
    Array<uint32> pending(allocator_);
    for (uint32 node = 0; node < tree.NodeCount(); ++node) {
        if (tree.Node(node).kind != SyntaxKind::Attribute || tree.Child(node, 0) == NoNode) {
            continue;
        }

        if (auto pushed = pending.TryPushBack(tree.Child(node, 0)); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }

        while (!pending.IsEmpty()) {
            const uint32 current = pending.TryPopBack().Value(); // 비어 있지 않다
            if (tree.Node(current).kind == SyntaxKind::Identifier) {
                if (auto pushed = attributeNames.TryPushBack(TokenText(module, current)); !pushed) {
                    binder_.error_ = pushed.Error();
                    return;
                }
            }

            for (const uint32 child : tree.Children(current)) {
                if (child == NoNode) {
                    continue;
                }

                if (auto pushed = pending.TryPushBack(child); !pushed) {
                    binder_.error_ = pushed.Error();
                    return;
                }
            }
        }
    }

    for (uint32 index = 0; index < owner.importCount; ++index) {
        const ImportResolution& import = c.imports_[owner.firstImport + index];
        if (import.module == NoModule || binder_.importUsed_[owner.firstImport + index] != 0) {
            continue;
        }

        if (c.nodeBase_[import.module] == NoNode) {
            continue; // 분석하지 않은 모듈(이름을 도입하지 않았다)
        }

        // attribute 인자 안의 식별자가 대상 모듈의 최상위 선언 이름이거나 이 import의 별칭이면 쓴
        // 것이다.
        const uint32 aliasNode = tree.Child(import.node, 1);
        const StringView alias =
            aliasNode == NoNode ? StringView() : binder_.NodeText(module, aliasNode);
        bool used = false;
        for (const StringView name : attributeNames) {
            used = used || (!alias.IsEmpty() && name == alias) ||
                   binder_.FindName(SpaceKey(NameSpaceKind::ModuleTop, import.module), name) !=
                       NoSymbol;
        }

        // 확장 이벤트(§5.2)는 이벤트 해석(LC3) 전까지 이름으로 본다: 이 모듈의 `on`·`emit` 경로 끝
        // 이름이 대상 모듈이 선언한 확장 이벤트 이름과 같으면 쓴 것이다. Rules 단계는 해석한
        // 이벤트로 이미 표시했다(3.12절).
        const SyntaxTree& targetTree = c.trees_[import.module];
        const uint32 declarations = targetTree.Child(targetTree.Root(), 1);
        for (const uint32 declaration : targetTree.Children(declarations)) {
            if (used || rules_ || targetTree.Node(declaration).kind != SyntaxKind::ExtensionEvent) {
                continue;
            }

            const uint32 eventName = targetTree.Child(declaration, 3);
            if (eventName == NoNode || targetTree.Node(eventName).kind != SyntaxKind::Name) {
                continue;
            }

            const StringView name = binder_.NodeText(import.module, eventName);
            for (uint32 node = 0; node < tree.NodeCount() && !used; ++node) {
                const SyntaxKind kind = tree.Node(node).kind;
                const uint32 path = kind == SyntaxKind::Handler ? tree.Child(node, 2)
                                    : kind == SyntaxKind::Emit  ? tree.Child(node, 0)
                                                                : NoNode;
                const uint32 count = path == NoNode ? 0 : tree.Node(path).childCount;
                if (count == 0 || tree.Node(path).kind != SyntaxKind::EventPath) {
                    continue;
                }

                const uint32 last = tree.Child(path, count - 1);
                used = tree.Node(last).kind == SyntaxKind::Name &&
                       binder_.NodeText(module, last) == name;
            }
        }

        if (used) {
            continue;
        }

        // 원문에 쓴 import 경로(`Lib.Spare`)로 알린다.
        const uint32 path = tree.Child(import.node, 0);
        const uint32 shown = path == NoNode ? import.node : path;
        const SourceSpan span = tree.Node(shown).span;
        const StringView written(c.sources_->Get(c.modules_[module].source).Text().Data() +
                                     span.begin,
                                 span.end - span.begin);
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("Module '{}' is imported but never used.", written);
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::UnusedImport, shown, message.View(),
                   "None of the declarations of the module are used here.", "Remove the import.");
        }
    }
}

} // namespace rocks::sparks::detail
