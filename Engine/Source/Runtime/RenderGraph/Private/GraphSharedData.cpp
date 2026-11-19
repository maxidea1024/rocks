#include "RenderGraphInternal.h"

#include <Platform/Atomics.h>

namespace rocks::render {

using detail::GraphState;
using detail::RecordGraphError;

namespace detail {

uint32 AllocateGraphSharedDataIndex() noexcept {
    static uint32 counter = 0;
    return platform::AtomicFetchAdd(&counter, 1u);
}

} // namespace detail

namespace {

// 공유 값 목록·view 표가 처음 받는 자리 수다(그 뒤로는 두 배씩 늘린다).
constexpr usize InitialCapacity = 4;

// arena에서 count개 자리를 받는다(실패하면 null, 오류는 error).
void* AllocateArray(FrameArena& arena, usize count, usize bytes, usize alignment,
                    ErrorCode& error) noexcept {
    usize total = 0;
    if (!TryMultiply(count, bytes, total)) {
        error = ErrorCode::Overflow;
        return nullptr;
    }

    auto memory = arena.TryAllocate(total, alignment);
    if (!memory) {
        error = memory.Error();
        return nullptr;
    }

    return memory.Value();
}

} // namespace

// --- GraphSharedData ---

void* GraphSharedData::Allocate(uint32 index, const char* name, usize bytes, usize alignment,
                                void (*destroy)(void*) noexcept) noexcept {
    GraphState* state = graph_.state_;
    if (state == nullptr) {
        return nullptr;
    }

    if (Lookup(index) != nullptr) {
        RecordGraphError(*state, ErrorCode::InvalidArgument, true,
                         "graph shared data '{}' is created twice", name);
        return nullptr;
    }

    FrameArena& arena = graph_.Arena();
    ErrorCode error = ErrorCode::None;
    if (size_ == capacity_) {
        const usize capacity = capacity_ == 0 ? InitialCapacity : capacity_ * 2;
        auto* grown = static_cast<Entry*>(
            AllocateArray(arena, capacity, sizeof(Entry), alignof(Entry), error));
        if (grown == nullptr) {
            RecordGraphError(*state, error, false,
                             "graph shared data '{}' could not be created ({})", name,
                             ErrorCodeName(error));
            return nullptr;
        }

        if (size_ != 0) {
            platform::MemoryCopy(grown, entries_, size_ * sizeof(Entry));
        }

        entries_ = grown;
        capacity_ = capacity;
    }

    void* value = AllocateArray(arena, 1, bytes, alignment, error);
    if (value == nullptr) {
        RecordGraphError(*state, error, false, "graph shared data '{}' could not be created ({})",
                         name, ErrorCodeName(error));
        return nullptr;
    }

    Entry& entry = entries_[size_++];
    entry.index = index;
    entry.value = value;
    entry.destroy = destroy;
    return value;
}

void* GraphSharedData::Lookup(uint32 index) const noexcept {
    for (usize entry = 0; entry < size_; ++entry) {
        if (entries_[entry].index == index) {
            return entries_[entry].value;
        }
    }

    return nullptr;
}

void* GraphSharedData::Required(uint32 index, const char* name) noexcept {
    void* value = Lookup(index);
    if (value == nullptr && graph_.state_ != nullptr) {
        RecordGraphError(*graph_.state_, ErrorCode::InvalidArgument, true,
                         "graph shared data '{}' is required before it was created", name);
    }

    return value;
}

void GraphSharedData::DestroyAll() noexcept {
    while (size_ > 0) {
        Entry& entry = entries_[--size_];
        entry.destroy(entry.value);
    }
}

// --- GraphViewTableStorage ---

void* GraphViewTableStorage::Insert(GraphViewKey key, ErrorCode& error) noexcept {
    GraphState* state = graph_.state_;
    if (state == nullptr) {
        error = ErrorCode::OutOfMemory;
        return nullptr;
    }

    for (usize index = 0; index < size_; ++index) {
        if (keys_[index] == key) {
            error = RecordGraphError(*state, ErrorCode::InvalidArgument, true,
                                     "'{}' already has view set {} view {}", name_, key.viewSet,
                                     key.view);
            return nullptr;
        }
    }

    if (size_ == capacity_) {
        FrameArena& arena = graph_.Arena();
        const usize capacity = capacity_ == 0 ? InitialCapacity : capacity_ * 2;
        auto* keys = static_cast<GraphViewKey*>(
            AllocateArray(arena, capacity, sizeof(GraphViewKey), alignof(GraphViewKey), error));
        auto* values =
            keys != nullptr
                ? static_cast<uint8*>(AllocateArray(arena, capacity, bytes_, alignment_, error))
                : nullptr;
        if (values == nullptr) {
            RecordGraphError(*state, error, false, "'{}' could not grow ({})", name_,
                             ErrorCodeName(error));
            return nullptr;
        }

        if (size_ != 0) {
            platform::MemoryCopy(keys, keys_, size_ * sizeof(GraphViewKey));
            platform::MemoryCopy(values, values_, size_ * bytes_);
        }

        keys_ = keys;
        values_ = values;
        capacity_ = capacity;
    }

    keys_[size_] = key;
    return values_ + size_++ * bytes_;
}

const void* GraphViewTableStorage::Lookup(GraphViewKey key, bool required) const noexcept {
    for (usize index = 0; index < size_; ++index) {
        if (keys_[index] == key) {
            return values_ + index * bytes_;
        }
    }

    if (required && graph_.state_ != nullptr) {
        RecordGraphError(*graph_.state_, ErrorCode::InvalidArgument, true,
                         "'{}' has no entry for view set {} view {}", name_, key.viewSet, key.view);
    }

    return nullptr;
}

} // namespace rocks::render
