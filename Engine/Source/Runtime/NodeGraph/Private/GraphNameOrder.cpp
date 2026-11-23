#include "GraphNameRanks.h"

#include <Core/Algorithms.h>
#include <NodeGraph/GraphNameOrder.h>

// Name·Guid 정규 순서와 내부 순위표다(Docs/Design/NodeGraph 6.1·6.3절).
namespace rocks::nodegraph {

namespace {

constexpr StringView NameOrderNoneText = "none";

uint8 NameOrderLower(char value) noexcept {
    const uint8 byte = static_cast<uint8>(value);
    if (byte >= 'A' && byte <= 'Z') {
        return static_cast<uint8>(byte - 'A' + 'a');
    }

    return byte;
}

// Name 같음 규칙의 None인가: 빈 base(번호가 있어도)이거나 번호 없는 "None"(ASCII 대소문자 무시).
bool NameOrderIsNone(const NameTextParts& parts) noexcept {
    if (parts.base.IsEmpty()) {
        return true;
    }

    if (parts.number != 0 || parts.base.ByteCount() != NameOrderNoneText.ByteCount()) {
        return false;
    }

    for (usize index = 0; index < parts.base.ByteCount(); ++index) {
        if (NameOrderLower(parts.base.Data()[index]) !=
            static_cast<uint8>(NameOrderNoneText.Data()[index])) {
            return false;
        }
    }

    return true;
}

} // namespace

int32 CompareNameText(StringView left, StringView right) noexcept {
    const NameTextParts leftParts = SplitNameText(left);
    const NameTextParts rightParts = SplitNameText(right);
    const bool leftNone = NameOrderIsNone(leftParts);
    const bool rightNone = NameOrderIsNone(rightParts);
    if (leftNone || rightNone) {
        if (leftNone == rightNone) {
            return 0;
        }

        return leftNone ? -1 : 1;
    }

    const usize leftBytes = leftParts.base.ByteCount();
    const usize rightBytes = rightParts.base.ByteCount();
    const usize common = leftBytes < rightBytes ? leftBytes : rightBytes;
    for (usize index = 0; index < common; ++index) {
        const uint8 leftByte = NameOrderLower(leftParts.base.Data()[index]);
        const uint8 rightByte = NameOrderLower(rightParts.base.Data()[index]);
        if (leftByte != rightByte) {
            return leftByte < rightByte ? -1 : 1;
        }
    }

    if (leftBytes != rightBytes) {
        return leftBytes < rightBytes ? -1 : 1;
    }

    if (leftParts.number != rightParts.number) {
        return leftParts.number < rightParts.number ? -1 : 1;
    }

    return 0;
}

Result<int32> CompareNames(NamePool& names, Name left, Name right) noexcept {
    if (!left.IsValid() || !right.IsValid()) {
        return Result<int32>::Failure(ErrorCode::InvalidArgument);
    }

    if (left == right) {
        return Result<int32>::Success(0);
    }

    auto leftText = names.Acquire(left);
    if (!leftText) {
        return Result<int32>::Failure(leftText.Error());
    }

    auto rightText = names.Acquire(right);
    if (!rightText) {
        return Result<int32>::Failure(rightText.Error());
    }

    return Result<int32>::Success(
        CompareNameText(leftText.Value().View(), rightText.Value().View()));
}

namespace detail {

GraphNameRanks::GraphNameRanks(Allocator& allocator, NamePool& names, bool skipInvalid) noexcept
    : names_(names), entries_(allocator), leases_(allocator), skipInvalid_(skipInvalid) {
}

GraphNameRanks::TokenKey GraphNameRanks::KeyOf(Name name) noexcept {
    // 같은 pool 안에서 Name 같음(operator==)과 일치하는 토큰 순서다: Normal은 (id, number), 그
    // 밖(None 계열)은 number만 본다.
    TokenKey key;
    if (name.kind == NameKind::Normal) {
        key.normal = 1;
        key.id = name.id;
    }

    key.number = name.number;
    return key;
}

int32 GraphNameRanks::CompareKeys(const TokenKey& left, const TokenKey& right) noexcept {
    if (left.normal != right.normal) {
        return left.normal < right.normal ? -1 : 1;
    }

    if (left.id != right.id) {
        return left.id < right.id ? -1 : 1;
    }

    if (left.number != right.number) {
        return left.number < right.number ? -1 : 1;
    }

    return 0;
}

Result<void> GraphNameRanks::Add(Name name) noexcept {
    if (!name.IsValid()) {
        return skipInvalid_ ? Result<void>::Success()
                            : Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Entry entry;
    entry.key = KeyOf(name);
    entry.name = name;
    return entries_.TryPushBack(entry);
}

Result<void> GraphNameRanks::AddDocument(const GraphDocument& document) noexcept {
    if (auto added = Add(document.domain); !added) {
        return added;
    }

    for (const Graph& graph : document.graphs) {
        for (const GraphNode& node : graph.nodes) {
            if (auto added = Add(node.type); !added) {
                return added;
            }

            for (const PinValueEntry& entry : node.inputDefaults) {
                if (auto added = Add(entry.pin); !added) {
                    return added;
                }
            }

            for (const RetainedPin& pin : node.retainedPins) {
                if (auto added = Add(pin.key); !added) {
                    return added;
                }
            }
        }

        for (const GraphEdge& edge : graph.edges) {
            if (auto added = Add(edge.fromPin); !added) {
                return added;
            }

            if (auto added = Add(edge.toPin); !added) {
                return added;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> GraphNameRanks::Build() noexcept {
    if (built_) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    algo::Sort(entries_.View(), [](const Entry& left, const Entry& right) noexcept {
        return CompareKeys(left.key, right.key) < 0;
    });

    // 같은 토큰을 하나로 줄인다.
    usize unique = 0;
    for (usize index = 0; index < entries_.Size(); ++index) {
        if (unique > 0 && CompareKeys(entries_[unique - 1].key, entries_[index].key) == 0) {
            continue;
        }

        entries_[unique] = entries_[index];
        ++unique;
    }

    if (auto resized = entries_.TryResize(unique); !resized) {
        return resized;
    }

    if (auto reserved = leases_.TryReserve(unique); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < unique; ++index) {
        auto lease = names_.Acquire(entries_[index].name);
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        entries_[index].lease = static_cast<uint32>(leases_.Size());
        if (auto pushed = leases_.TryPushBack(Move(lease.Value())); !pushed) {
            return pushed;
        }
    }

    // 원문 순서로 순위를 매긴다. 순서 배열은 entries_의 색인이다.
    Array<uint32> order(entries_.GetAllocator());
    if (auto reserved = order.TryReserve(unique); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < unique; ++index) {
        if (auto pushed = order.TryPushBack(static_cast<uint32>(index)); !pushed) {
            return pushed;
        }
    }

    algo::Sort(order.View(), [this](uint32 left, uint32 right) noexcept {
        return CompareNameText(leases_[entries_[left].lease].View(),
                               leases_[entries_[right].lease].View()) < 0;
    });

    uint32 rank = 0;
    for (usize index = 0; index < order.Size(); ++index) {
        if (index > 0 && CompareNameText(leases_[entries_[order[index - 1]].lease].View(),
                                         leases_[entries_[order[index]].lease].View()) != 0) {
            ++rank;
        }

        entries_[order[index]].rank = rank;
    }

    built_ = true;
    return Result<void>::Success();
}

usize GraphNameRanks::Find(Name name) const noexcept {
    if (!built_ || !name.IsValid()) {
        return IndexNone;
    }

    const TokenKey key = KeyOf(name);
    usize low = 0;
    usize high = entries_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const int32 compared = CompareKeys(entries_[middle].key, key);
        if (compared == 0) {
            return middle;
        }

        if (compared < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return IndexNone;
}

uint32 GraphNameRanks::Rank(Name name) const noexcept {
    const usize index = Find(name);
    return index == IndexNone ? UnrankedName : entries_[index].rank;
}

StringView GraphNameRanks::Text(Name name) const noexcept {
    const usize index = Find(name);
    return index == IndexNone ? StringView() : leases_[entries_[index].lease].View();
}

Result<uint32> CanonicalNameParts(StringView text, Array<char>& lowered) noexcept {
    lowered.Clear();
    const NameTextParts parts = SplitNameText(text);
    if (NameOrderIsNone(parts)) {
        return Result<uint32>::Success(0);
    }

    if (auto reserved = lowered.TryReserve(parts.base.ByteCount()); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    for (usize index = 0; index < parts.base.ByteCount(); ++index) {
        if (auto pushed =
                lowered.TryPushBack(static_cast<char>(NameOrderLower(parts.base.Data()[index])));
            !pushed) {
            return Result<uint32>::Failure(pushed.Error());
        }
    }

    return Result<uint32>::Success(parts.number);
}

} // namespace detail

} // namespace rocks::nodegraph
