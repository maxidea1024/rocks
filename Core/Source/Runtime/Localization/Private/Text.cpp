#include "LocalizationInternal.h"
#include <Core/ScopeExit.h>

namespace rocks::detail {

Result<void> RetainText(TextNode* node) noexcept {
    platform::LockGuard lock(node->gate);
    if (node->references == NumericLimits<uint64>::Max()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }
    ++node->references;
    return Result<void>::Success();
}

namespace {

void DropToQueue(TextNode* node, TextNode*& pending) noexcept {
    if (!node) {
        return;
    }
    platform::LockGuard lock(node->gate);
    if (node->references == 0) {
        platform::Fatal("Text reference underflow");
    }
    if (--node->references == 0) {
        node->cleanupNext = pending;
        pending = node;
    }
}

} // namespace

void ReleaseText(TextNode* node) noexcept {
    TextNode* pending = nullptr;
    DropToQueue(node, pending);
    while (pending) {
        TextNode* current = pending;
        pending = current->cleanupNext;
        DropToQueue(current->pattern, pending);
        for (usize index = 0; index < current->arguments.Size(); ++index) {
            DropToQueue(current->arguments[index].value, pending);
        }
        // 자식 계수를 반환한 후 현재 노드의 이름/원문을 해제한다. 어떤 node gate도 잡고 있지 않다.
        // shared child는 마지막 parent만 큐에 넣으므로 DAG에서도 한 번만 파괴된다.
        Allocator* allocator = current->allocator;
        DestroyAt(current);
        if (!allocator->Deallocate(current)) {
            platform::Fatal("Text allocation ownership mismatch");
        }
    }
}

bool IsArgumentName(StringView name) noexcept {
    if (name.IsEmpty() || name.ByteCount() > 64) {
        return false;
    }
    for (usize index = 0; index < name.ByteCount(); ++index) {
        const char value = name.Data()[index];
        const bool letter = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
        const bool digit = value >= '0' && value <= '9';
        if (!(letter || value == '_' || (index != 0 && digit))) {
            return false;
        }
    }
    return true;
}

} // namespace rocks::detail

namespace rocks {
namespace {

Result<detail::TextNode*> NewNode(Allocator& allocator, detail::TextKind kind) noexcept {
    auto memory = allocator.Allocate(sizeof(detail::TextNode), alignof(detail::TextNode));
    if (!memory) {
        return Result<detail::TextNode*>::Failure(memory.Error());
    }
    if (!memory.Value()) {
        platform::Fatal("Allocator returned null Text node");
    }
    return Result<detail::TextNode*>::Success(
        InitializeAt<detail::TextNode>(memory.Value(), allocator, kind));
}

Result<void> CheckSource(StringView source, TextCreateLimits limits) noexcept {
    if (limits.maxSourceBytes > 16 * 1024 * 1024 || limits.maxArguments > 64) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (source.ByteCount() > limits.maxSourceBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }
    return source.ValidateUtf8();
}

} // namespace

Text::Text(Text&& other) noexcept : node_(other.node_) {
    other.node_ = nullptr;
}

Text& Text::operator=(Text&& other) noexcept {
    if (this != &other) {
        detail::TextNode* previous = node_;
        node_ = other.node_;
        other.node_ = nullptr;
        detail::ReleaseText(previous);
    }
    return *this;
}

Text::~Text() noexcept {
    Reset();
}

void Text::Reset() noexcept {
    detail::TextNode* previous = node_;
    node_ = nullptr;
    detail::ReleaseText(previous);
}

bool Text::IsValid() const noexcept {
    return node_ != nullptr;
}

Result<Text> Text::Clone() const noexcept {
    if (!node_) {
        return Result<Text>::Failure(ErrorCode::InvalidArgument);
    }
    auto retained = detail::RetainText(node_);
    if (!retained) {
        return Result<Text>::Failure(retained.Error());
    }
    return Result<Text>::Success(Text(node_));
}

Result<Text> Text::Invariant(Allocator& allocator, StringView source,
                             TextCreateLimits limits) noexcept {
    auto valid = CheckSource(source, limits);
    if (!valid) {
        return Result<Text>::Failure(valid.Error());
    }
    auto allocation = NewNode(allocator, detail::TextKind::Invariant);
    if (!allocation) {
        return Result<Text>::Failure(allocation.Error());
    }
    Text owned(allocation.Value());
    auto copied = owned.node_->source.TryAssign(source);
    if (!copied) {
        return Result<Text>::Failure(copied.Error());
    }
    return Result<Text>::Success(Move(owned));
}

Result<Text> Text::Localized(Allocator& allocator, TextId id, uint64 sourceRevision,
                             StringView fallback, TextCreateLimits limits) noexcept {
    if (id == TextId{} || sourceRevision == 0) {
        return Result<Text>::Failure(ErrorCode::InvalidArgument);
    }
    auto created = Invariant(allocator, fallback, limits);
    if (!created) {
        return created;
    }
    // 아직 외부에 게시하지 않은 단독 소유 노드의 초기화만 변경한다.
    created.Value().node_->kind = detail::TextKind::Localized;
    created.Value().node_->id = id;
    created.Value().node_->sourceRevision = sourceRevision;
    return created;
}

Result<Text> Text::Integer(Allocator& allocator, int64 value) noexcept {
    char buffer[21];
    usize offset = sizeof(buffer);
    // 최소 음수에 직접 부호를 반전하지 않아 signed overflow를 피한다.
    uint64 magnitude =
        value < 0 ? static_cast<uint64>(-(value + 1)) + 1 : static_cast<uint64>(value);
    do {
        buffer[--offset] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (value < 0) {
        buffer[--offset] = '-';
    }
    auto created = Invariant(allocator, StringView(buffer + offset, sizeof(buffer) - offset));
    if (!created) {
        return created;
    }

    // 아직 외부에 게시하지 않은 단독 소유 노드다. 해석은 Invariant와 같고 저장할 수 있는 형태만
    // 아니다(TryGetPersistentForm).
    created.Value().node_->kind = detail::TextKind::Integer;
    return created;
}

Result<Text> Text::Format(Allocator& allocator, const Text& pattern,
                          Span<const TextArgument> arguments, TextCreateLimits limits) noexcept {
    auto validLimits = CheckSource({}, limits);
    if (!validLimits) {
        return Result<Text>::Failure(validLimits.Error());
    }
    if (!pattern.node_) {
        return Result<Text>::Failure(ErrorCode::InvalidArgument);
    }
    if (arguments.Size() > limits.maxArguments) {
        return Result<Text>::Failure(ErrorCode::CapacityExceeded);
    }
    for (usize index = 0; index < arguments.Size(); ++index) {
        if (!detail::IsArgumentName(arguments[index].name) || !arguments[index].value ||
            !arguments[index].value->node_) {
            return Result<Text>::Failure(ErrorCode::InvalidArgument);
        }
        for (usize prior = 0; prior < index; ++prior) {
            if (arguments[prior].name == arguments[index].name) {
                return Result<Text>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }
    auto allocation = NewNode(allocator, detail::TextKind::Format);
    if (!allocation) {
        return Result<Text>::Failure(allocation.Error());
    }
    Text owned(allocation.Value());
    auto retained = detail::RetainText(pattern.node_);
    if (!retained) {
        return Result<Text>::Failure(retained.Error());
    }
    owned.node_->pattern = pattern.node_;
    auto reserved = owned.node_->arguments.TryReserve(arguments.Size());
    if (!reserved) {
        return Result<Text>::Failure(reserved.Error());
    }
    for (usize index = 0; index < arguments.Size(); ++index) {
        auto added = owned.node_->arguments.TryEmplaceBack(allocator);
        if (!added) {
            return Result<Text>::Failure(added.Error());
        }
        auto copied = added.Value()->name.TryAssign(arguments[index].name);
        if (!copied) {
            return Result<Text>::Failure(copied.Error());
        }
        auto held = detail::RetainText(arguments[index].value->node_);
        if (!held) {
            return Result<Text>::Failure(held.Error());
        }
        added.Value()->value = arguments[index].value->node_;
    }
    return Result<Text>::Success(Move(owned));
}

Result<void> Text::TryGetPersistentForm(TextPersistentView& out) const noexcept {
    if (!node_) {
        out = TextPersistentView{};
        return Result<void>::Success();
    }

    // 게시된 노드는 불변이므로 잠그지 않고 읽는다.
    switch (node_->kind) {
    case detail::TextKind::Invariant:
        out = TextPersistentView{TextPersistentForm::Invariant, node_->source.View(), TextId{}, 0};
        return Result<void>::Success();
    case detail::TextKind::Localized:
        out = TextPersistentView{TextPersistentForm::Localized, node_->source.View(), node_->id,
                                 node_->sourceRevision};
        return Result<void>::Success();
    case detail::TextKind::Format:
    case detail::TextKind::Integer:
        break;
    }

    return Result<void>::Failure(ErrorCode::Unsupported);
}

namespace {

// DeepClone의 예산이다(노드 방문 + 복사 byte — ResolveText의 work와 같은 단위).
struct DeepCloneBudget {
    uint32 maxDepth = 0;
    uint64 remaining = 0;

    bool Spend(uint64 amount) noexcept {
        if (amount > remaining) {
            return false;
        }

        remaining -= amount;
        return true;
    }
};

// node의 깊은 사본을 allocator로 만든다(돌려준 노드의 reference 하나를 호출자가 소유한다). 깊이는
// maxDepth(64 이하)로 막히므로 재귀 깊이도 그만큼이다. 실패하면 만든 노드를 모두 반환한다.
Result<detail::TextNode*> CloneNode(Allocator& allocator, const detail::TextNode& node,
                                    uint32 depth, DeepCloneBudget& budget) noexcept {
    if (depth > budget.maxDepth || !budget.Spend(1) ||
        !budget.Spend(node.source.View().ByteCount())) {
        return Result<detail::TextNode*>::Failure(ErrorCode::CapacityExceeded);
    }

    auto allocation = NewNode(allocator, node.kind);
    if (!allocation) {
        return allocation;
    }

    detail::TextNode* copy = allocation.Value();
    auto release = MakeScopeExit([&copy]() noexcept { detail::ReleaseText(copy); });
    copy->id = node.id;
    copy->sourceRevision = node.sourceRevision;
    if (auto copied = copy->source.TryAssign(node.source.View()); !copied) {
        return Result<detail::TextNode*>::Failure(copied.Error());
    }

    if (node.pattern) {
        auto pattern = CloneNode(allocator, *node.pattern, depth + 1, budget);
        if (!pattern) {
            return pattern;
        }

        copy->pattern = pattern.Value();
    }

    if (auto reserved = copy->arguments.TryReserve(node.arguments.Size()); !reserved) {
        return Result<detail::TextNode*>::Failure(reserved.Error());
    }

    for (usize index = 0; index < node.arguments.Size(); ++index) {
        const detail::TextArgumentData& argument = node.arguments[index];
        if (!budget.Spend(argument.name.View().ByteCount())) {
            return Result<detail::TextNode*>::Failure(ErrorCode::CapacityExceeded);
        }

        auto added = copy->arguments.TryEmplaceBack(allocator);
        if (!added) {
            return Result<detail::TextNode*>::Failure(added.Error());
        }

        if (auto named = added.Value()->name.TryAssign(argument.name.View()); !named) {
            return Result<detail::TextNode*>::Failure(named.Error());
        }

        auto value = CloneNode(allocator, *argument.value, depth + 1, budget);
        if (!value) {
            return value;
        }

        added.Value()->value = value.Value();
    }

    detail::TextNode* result = copy;
    copy = nullptr;
    return Result<detail::TextNode*>::Success(result);
}

} // namespace

Result<Text> Text::DeepClone(Allocator& allocator, TextResolveLimits limits) const noexcept {
    if (limits.maxDepth > 64) {
        return Result<Text>::Failure(ErrorCode::InvalidArgument);
    }

    if (!node_) {
        return Result<Text>::Success(Text());
    }

    DeepCloneBudget budget{limits.maxDepth, limits.maxWork};
    auto cloned = CloneNode(allocator, *node_, 1, budget);
    if (!cloned) {
        return Result<Text>::Failure(cloned.Error());
    }

    return Result<Text>::Success(Text(cloned.Value()));
}

} // namespace rocks
