#include "LocalizationInternal.h"

namespace rocks::detail {

Result<void> RetainText(TextNode* node) noexcept {
    platform::LockGuard lock(node->gate);
    if (node->references == ~uint64(0)) {
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
        ConstructAt<detail::TextNode>(memory.Value(), allocator, kind));
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
    return Invariant(allocator, StringView(buffer + offset, sizeof(buffer) - offset));
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

} // namespace rocks
