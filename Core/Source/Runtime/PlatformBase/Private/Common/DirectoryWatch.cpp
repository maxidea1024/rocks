#include "DirectoryWatchInternal.h"
#include "FileInternal.h"

#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include <new>

namespace rocks::platform {
namespace detail {

namespace {

constexpr usize RecordAlignment = 8;

constexpr usize AlignUp(usize value) noexcept {
    return (value + RecordAlignment - 1u) & ~(RecordAlignment - 1u);
}

} // namespace

WatchEventQueue::~WatchEventQueue() noexcept {
    AlignedFree(data_);
}

bool WatchEventQueue::Reserve(usize bytes) noexcept {
    // 앞쪽에서 이미 넘긴 레코드 공간을 먼저 회수한다.
    if (head_ != 0) {
        MemoryMove(data_, data_ + head_, used_ - head_);
        used_ -= head_;
        head_ = 0;
    }
    if (bytes <= capacity_ - used_) {
        return true;
    }
    usize next = capacity_ == 0 ? 4096u : capacity_;
    while (next - used_ < bytes) {
        if (next > MaxBytes) {
            return false;
        }
        next *= 2u;
    }
    auto* grown = static_cast<uint8*>(AlignedAllocate(next, RecordAlignment));
    if (grown == nullptr) {
        return false;
    }
    MemoryCopy(grown, data_, used_);
    AlignedFree(data_);
    data_ = grown;
    capacity_ = next;
    return true;
}

void WatchEventQueue::CollapseToOverflow() noexcept {
    // 루트 전체 재검색은 앞서 쌓인 이벤트를 모두 포함한다. 할당이 필요 없는 플래그로 둔다.
    head_ = 0;
    used_ = 0;
    rootOverflow_ = true;
}

bool WatchEventQueue::Push(DirectoryChangeKind kind, DirectoryEntryKind entry, const char* path,
                           usize pathBytes, const char* oldPath, usize oldPathBytes) noexcept {
    // 루트 소실은 언제나 마지막 이벤트다(그 뒤로 감시가 없다). 할당 없이 플래그로 둔다.
    if (kind == DirectoryChangeKind::RootRemoved) {
        rootRemoved_ = true;
        return true;
    }
    if (kind == DirectoryChangeKind::Overflow && pathBytes == 0) {
        CollapseToOverflow();
        return true;
    }
    // 루트 전체 재검색이 걸려 있으면 그 뒤의 이벤트는 그 재검색에 포함된다.
    if (rootOverflow_) {
        return true;
    }
    if (kind == DirectoryChangeKind::Overflow) {
        oldPathBytes = 0;
    }
    const usize bytes = AlignUp(sizeof(Record) + pathBytes + 1u + oldPathBytes + 1u);
    if (pathBytes > MaxBytes || oldPathBytes > MaxBytes || used_ - head_ + bytes > MaxBytes ||
        !Reserve(bytes)) {
        CollapseToOverflow();
        return false;
    }
    Record record{static_cast<uint8>(kind),
                  static_cast<uint8>(entry),
                  0,
                  static_cast<uint32>(pathBytes),
                  static_cast<uint32>(oldPathBytes),
                  static_cast<uint32>(bytes)};
    uint8* at = data_ + used_;
    MemoryCopy(at, &record, sizeof(record));
    char* text = reinterpret_cast<char*>(at + sizeof(record));
    MemoryCopy(text, path, pathBytes);
    text[pathBytes] = '\0';
    MemoryCopy(text + pathBytes + 1u, oldPath, oldPathBytes);
    text[pathBytes + 1u + oldPathBytes] = '\0';
    used_ += bytes;
    return true;
}

void WatchEventQueue::TakeAll(WatchEventQueue& source) noexcept {
    DirectoryChange change;
    while (source.Front(change)) {
        Push(change.kind, change.entry, change.path, change.pathBytes, change.oldPath,
             change.oldPathBytes);
        source.Pop();
    }
    source.Clear();
}

bool WatchEventQueue::Front(DirectoryChange& change) const noexcept {
    change = DirectoryChange{};
    if (rootOverflow_) {
        change.kind = DirectoryChangeKind::Overflow;
        return true;
    }
    if (head_ != used_) {
        Record record;
        MemoryCopy(&record, data_ + head_, sizeof(record));
        const char* text = reinterpret_cast<const char*>(data_ + head_ + sizeof(record));
        change.kind = static_cast<DirectoryChangeKind>(record.kind);
        change.entry = static_cast<DirectoryEntryKind>(record.entry);
        change.path = text;
        change.pathBytes = record.pathBytes;
        change.oldPath = text + record.pathBytes + 1u;
        change.oldPathBytes = record.oldPathBytes;
        return true;
    }
    if (rootRemoved_) {
        change.kind = DirectoryChangeKind::RootRemoved;
        change.entry = DirectoryEntryKind::Directory;
        return true;
    }
    return false;
}

void WatchEventQueue::Pop() noexcept {
    if (rootOverflow_) {
        rootOverflow_ = false;
        return;
    }
    if (head_ != used_) {
        Record record;
        MemoryCopy(&record, data_ + head_, sizeof(record));
        head_ += record.recordBytes;
        if (head_ == used_) {
            head_ = 0;
            used_ = 0;
        }
        return;
    }
    rootRemoved_ = false;
}

void WatchEventQueue::Clear() noexcept {
    head_ = 0;
    used_ = 0;
    rootOverflow_ = false;
    rootRemoved_ = false;
}

bool JoinRelativePath(const char* base, usize baseBytes, const char* name, usize nameBytes,
                      char* out, usize capacity, usize& outBytes) noexcept {
    const usize separator = baseBytes != 0 && nameBytes != 0 ? 1u : 0u;
    const usize total = baseBytes + separator + nameBytes;
    if (total + 1u > capacity) {
        return false;
    }
    MemoryCopy(out, base, baseBytes);
    if (separator != 0) {
        out[baseBytes] = '/';
    }
    MemoryCopy(out + baseBytes + separator, name, nameBytes);
    out[total] = '\0';
    outBytes = total;
    return true;
}

} // namespace detail

struct DirectoryWatcher::State {
    detail::WatchBackend* backend = nullptr;
    detail::WatchEventQueue pending;
    uint64 owner = 0;
    bool busy = false;
    // visitor 안에서 Close를 불렀다. Poll이 끝난 뒤 닫는다.
    bool closeRequested = false;
};

namespace {

// 같은 감시기의 다른 스레드 호출과 visitor 재진입을 막는다. owner를 먼저 비교해 다른 스레드가
// busy를 읽는 경합을 피한다.
class CallGuard {
public:
    CallGuard(bool& busy, uint64 owner) noexcept : busy_(busy) {
        allowed_ = CurrentThreadId() == owner && !busy;
        if (allowed_) {
            busy_ = true;
        }
    }

    ~CallGuard() noexcept {
        if (allowed_) {
            busy_ = false;
        }
    }

    bool Allowed() const noexcept {
        return allowed_;
    }

private:
    bool& busy_;
    bool allowed_ = false;
};

} // namespace

DirectoryWatcher::~DirectoryWatcher() noexcept {
    if (state_ != nullptr && state_->busy) {
        Fatal("DirectoryWatcher destroyed inside its own Poll visitor");
    }
    Close();
}

FileStatus DirectoryWatcher::Open(const char* path) noexcept {
    return Open(path, path != nullptr ? platform::CStringLength(path) : 0);
}

FileStatus DirectoryWatcher::Open(const char* path, usize pathBytes) noexcept {
    if (state_ != nullptr) {
        return {FileCode::AlreadyOpen, 0};
    }
    if (path == nullptr || pathBytes == 0 || !detail::ValidFilePath(path, pathBytes)) {
        return {FileCode::InvalidArgument, 0};
    }
    void* memory = AlignedAllocate(sizeof(State), alignof(State));
    if (memory == nullptr) {
        return {FileCode::OutOfMemory, 0};
    }
    auto* state = ::new (memory) State();
    state->owner = CurrentThreadId();
    const FileStatus opened = detail::WatchOpen(path, pathBytes, state->backend);
    if (!opened) {
        state->~State();
        AlignedFree(memory);
        return opened;
    }
    state_ = state;
    return {};
}

FileStatus DirectoryWatcher::Poll(DirectoryChangeVisitor visitor, void* context) noexcept {
    if (state_ == nullptr) {
        return {FileCode::NotOpen, 0};
    }
    if (visitor == nullptr) {
        return {FileCode::InvalidArgument, 0};
    }
    FileStatus collected{};
    {
        CallGuard guard(state_->busy, state_->owner);
        if (!guard.Allowed()) {
            return {FileCode::Busy, 0};
        }
        collected = detail::WatchCollect(*state_->backend, state_->pending);
        DirectoryChange change;
        while (!state_->closeRequested && state_->pending.Front(change)) {
            if (!visitor(change, context)) {
                break;
            }
            state_->pending.Pop();
        }
    }
    if (state_->closeRequested) {
        Close();
    }
    return collected;
}

bool DirectoryWatcher::Wait(uint32 timeoutMilliseconds) noexcept {
    if (state_ == nullptr) {
        return false;
    }
    CallGuard guard(state_->busy, state_->owner);
    if (!guard.Allowed()) {
        return false;
    }
    if (!state_->pending.IsEmpty()) {
        return true;
    }
    return detail::WatchWait(*state_->backend, timeoutMilliseconds);
}

void DirectoryWatcher::Close() noexcept {
    if (state_ == nullptr) {
        return;
    }
    if (CurrentThreadId() != state_->owner) {
        Fatal("DirectoryWatcher closed from a thread other than its owner");
    }
    if (state_->busy) {
        // visitor 안이다. Poll이 visitor 호출을 끝낸 뒤 닫는다.
        state_->closeRequested = true;
        return;
    }
    detail::WatchClose(state_->backend);
    State* state = state_;
    state_ = nullptr;
    state->~State();
    AlignedFree(state);
}

} // namespace rocks::platform
