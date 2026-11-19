#include "ConfigState.h"
#include "ConfigText.h"

#include <Platform/Clock.h>
#include <Platform/Memory.h>

// 설정 snapshot의 조회·만들기·게시다(Docs/Design/CVarSystem.ko.md 7.2·10절 — CVR2).
namespace rocks::config {

namespace {

// byte 순서 비교(사전식)다(이름 블록의 정렬 순서와 같다).
using detail::NamedBlock;

} // namespace

ConfigSnapshot::ConfigSnapshot(Allocator& allocator) noexcept
    : chunks_(allocator), bootChunks_(allocator) {
}

ConfigSnapshot::~ConfigSnapshot() = default;

usize ConfigSnapshot::Count() const noexcept {
    return named_.Get() != nullptr ? named_->entries.Size() : 0;
}

StringView ConfigSnapshot::NameAt(usize index) const noexcept {
    if (index >= Count()) {
        platform::Fatal("ConfigSnapshot::NameAt index out of range");
    }

    return named_->entries[index]->name.View();
}

const FieldValue& ConfigSnapshot::ValueAt(usize index) const noexcept {
    if (index >= Count()) {
        platform::Fatal("ConfigSnapshot::ValueAt index out of range");
    }

    return named_->entries[index]->value;
}

const FieldValue* ConfigSnapshot::Find(StringView name) const noexcept {
    usize low = 0;
    usize high = Count();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (detail::LessBytes(named_->entries[middle]->name.View(), name)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low < Count() && named_->entries[low]->name.View() == name ? &named_->entries[low]->value
                                                                      : nullptr;
}

namespace detail {

Result<SharedPtr<ConfigSnapshot>> SnapshotAccess::Make(Allocator& allocator) noexcept {
    return SharedPtr<ConfigSnapshot>::TryMake(allocator, allocator);
}

namespace {

// 설정 값(value — 콘솔 설정 종류)을 칸에 쓴다. String·Name은 새 텍스트 블록을 만든다.
Result<void> WriteSlot(Allocator& allocator, const FieldValue& value, CVarSlot& slot,
                       SharedPtr<const CVarText>& text) noexcept {
    switch (value.kind) {
    case ValueKind::Bool:
        slot.bits = value.boolean ? 1 : 0;
        text = SharedPtr<const CVarText>();
        return Result<void>::Success();
    case ValueKind::Float32:
        slot.bits = platform::BitCast<uint64>(static_cast<double>(value.float32));
        text = SharedPtr<const CVarText>();
        return Result<void>::Success();
    case ValueKind::Float64:
        slot.bits = platform::BitCast<uint64>(value.float64);
        text = SharedPtr<const CVarText>();
        return Result<void>::Success();
    case ValueKind::Utf8String:
    case ValueKind::Name: {
        auto made = SharedPtr<CVarText>::TryMake(allocator, allocator);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        if (value.kind == ValueKind::Utf8String) {
            if (auto assigned = made.Value()->text.TryAssign(value.string.View()); !assigned) {
                return assigned;
            }
        } else {
            made.Value()->name = value.name;
        }

        slot.bits = 0;
        text = Move(made.Value());
        return Result<void>::Success();
    }
    default:
        slot.bits = static_cast<uint64>(value.signedInteger);
        text = SharedPtr<const CVarText>();
        return Result<void>::Success();
    }
}

} // namespace

namespace {

// 조각 표 table(이전 snapshot의 것을 복사해 둔 표)에서 writes의 칸이 든 조각만 복사본으로 바꿔
// 쓴다. 실패하면 table은 일부만 바뀌었을 수 있다(호출자가 snapshot째 버린다).
Result<void> WriteChunks(ConfigState& state, Array<SharedPtr<const CVarChunk>>& table,
                         Span<const SlotWrite> writes) noexcept {
    Array<SharedPtr<CVarChunk>> copies(state.allocator);
    for (const SlotWrite& write : writes) {
        const usize chunk = write.index / ChunkSlotCount;
        if (chunk >= table.Size()) {
            if (auto grown = table.TryResize(chunk + 1); !grown) {
                return grown;
            }
        }

        if (chunk >= copies.Size()) {
            if (auto grown = copies.TryResize(chunk + 1); !grown) {
                return grown;
            }
        }

        if (copies[chunk].Get() == nullptr) {
            auto copy = SharedPtr<CVarChunk>::TryMake(state.allocator);
            if (!copy) {
                return Result<void>::Failure(copy.Error());
            }

            if (table[chunk].Get() != nullptr) {
                *copy.Value() = *table[chunk];
            }

            copies[chunk] = Move(copy.Value());
        }

        const usize slotIndex = write.index % ChunkSlotCount;
        CVarSlot& slot = copies[chunk]->slots[slotIndex];
        // 세대 0은 "칸 없음"이므로 감길 때 건너뛴다.
        const uint32 generation = slot.generation + 1 != 0 ? slot.generation + 1 : 1;
        if (auto written =
                WriteSlot(state.allocator, *write.value, slot, copies[chunk]->texts[slotIndex]);
            !written) {
            return written;
        }

        slot.generation = generation;
    }

    for (usize chunk = 0; chunk < copies.Size(); ++chunk) {
        if (copies[chunk].Get() != nullptr) {
            table[chunk] = SharedPtr<const CVarChunk>(Move(copies[chunk]));
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> PublishSnapshot(ConfigState& state, const SharedPtr<const NamedBlock>* named,
                             Span<const SlotWrite> writes,
                             Span<const SlotWrite> bootWrites) noexcept {
    const ConfigSnapshot* previous = state.snapshot.Get();
    auto made = SnapshotAccess::Make(state.allocator);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    ConfigSnapshot& snapshot = *made.Value();
    // 조각 표·부팅 블록은 이전 snapshot의 것을 공유하고, 쓰는 칸이 든 조각만 복사본으로 바꾼다.
    Array<SharedPtr<const CVarChunk>>& chunks = SnapshotAccess::Chunks(snapshot);
    Array<SharedPtr<const CVarChunk>>& bootChunks = SnapshotAccess::BootChunks(snapshot);
    if (previous != nullptr) {
        if (auto copied = chunks.TryAppend(SnapshotAccess::Chunks(*previous).View()); !copied) {
            return copied;
        }

        if (auto copied = bootChunks.TryAppend(SnapshotAccess::BootChunks(*previous).View());
            !copied) {
            return copied;
        }
    }

    if (auto written = WriteChunks(state, chunks, writes); !written) {
        return written;
    }

    if (auto written = WriteChunks(state, bootChunks, bootWrites); !written) {
        return written;
    }

    // 콘솔 이름 색인은 등록이 바뀌었으면 새로 만들고 아니면 이전 것을 공유한다(10.1절).
    SharedPtr<const ConsoleNameIndex> names;
    if (state.namesChanged || previous == nullptr) {
        auto built = BuildConsoleNameIndex(state);
        if (!built) {
            return Result<void>::Failure(built.Error());
        }

        names = Move(built.Value());
    } else {
        names = SnapshotAccess::Names(*previous);
    }

    // 여기부터는 실패하지 않는다.
    SnapshotAccess::Names(snapshot) = Move(names);
    state.namesChanged = false;
    SharedPtr<const NamedBlock> block;
    if (named != nullptr) {
        block = *named;
    } else if (previous != nullptr) {
        block = SnapshotAccess::Named(*previous);
    }

    const uint64 generation = previous != nullptr ? previous->Generation() + 1 : 1;
    SnapshotAccess::Assign(snapshot, generation, Move(block), state.channel);
    SnapshotAccess::Seal(snapshot, state.startupComplete);
    SharedPtr<const ConfigSnapshot> published(Move(made.Value()));
    {
        platform::LockGuard lock(state.channel->mutex);
        state.channel->current = published;
    }

    // 소유 스레드(게임 lane)를 이 문맥에 다시 묶는다(명시 묶음이 살아 있으면 그대로 둔다 — 10.1절).
    state.snapshot = Move(published);
    state.snapshotGeneration = generation;
    RebindGameLane(state);
    return Result<void>::Success();
}

} // namespace detail

} // namespace rocks::config
