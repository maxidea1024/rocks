#include "DynamicLibraryInternal.h"
#include "FileInternal.h"

#include <Base/PlacementNew.h>
#include <Platform/Atomics.h>
#include <Platform/Memory.h>

#include <string.h>

namespace rocks::platform {

// references는 소유자(DynamicLibrary) 몫 1과 살아 있는 lease 수의 합이다. 0이 되는 쪽(Close,
// 소유자 소멸, 마지막 lease 해제)이 라이브러리를 내리고 상태를 해제한다. 소유자가 있는 동안에는 1
// 이상이므로 Close는 1 → 0 교환이 성공할 때만 내린다(lease가 동시에 생기거나 풀려도 한쪽만 이긴다).
struct DynamicLibrary::State {
    uptr handle = 0;
    usize references = 1;
};

namespace {

void Unload(DynamicLibrary::State* state) noexcept {
    (void)detail::DynamicLibraryClose(state->handle);
    state->~State();
    AlignedFree(state);
}

// 참조 하나를 놓는다. 마지막이면 내린다. 해제 순서(acq_rel): 이 참조로 한 읽기가 내리기 전에
// 끝났음을 마지막 해제자가 본다.
void Release(DynamicLibrary::State* state) noexcept {
    if (state != nullptr &&
        AtomicFetchSub<MemoryOrder::AcquireRelease>(&state->references, usize{1}) == 1u) {
        Unload(state);
    }
}

FileStatus Adopt(DynamicLibrary::State*& state, uptr handle) noexcept {
    void* memory = AlignedAllocate(sizeof(DynamicLibrary::State), alignof(DynamicLibrary::State));
    if (memory == nullptr) {
        (void)detail::DynamicLibraryClose(handle);
        return {FileCode::OutOfMemory, 0};
    }

    state = new (memory) DynamicLibrary::State{};
    state->handle = handle;
    return {};
}

// 호출 시작에 문구 칸을 비운다(앞 실패의 문구가 남지 않게).
void ClearError(DynamicLibraryError* error) noexcept {
    if (error != nullptr) {
        error->text[0] = '\0';
        error->truncated = false;
    }
}

bool IsSeparator(char value) noexcept {
    return value == '/' || value == '\\';
}

// OpenSystem 이름은 파일 이름 하나여야 한다. 경로 구분자·드라이브(':')·"."·".."가 있으면 다른
// 디렉터리를 가리킬 수 있으므로 거부한다.
bool ValidSystemName(const char* name, usize bytes) noexcept {
    if (!detail::ValidFilePath(name, bytes) || bytes > DynamicLibrary::MaxSystemNameBytes) {
        return false;
    }

    for (usize i = 0; i < bytes; ++i) {
        if (IsSeparator(name[i]) || name[i] == ':') {
            return false;
        }
    }

    const bool dot = bytes == 1 && name[0] == '.';
    const bool dotDot = bytes == 2 && name[0] == '.' && name[1] == '.';
    return !dot && !dotDot;
}

} // namespace

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        State* previous = state_;
        state_ = other.state_;
        other.state_ = nullptr;
        Release(previous);
    }

    return *this;
}

DynamicLibrary::~DynamicLibrary() noexcept {
    // lease가 남아 있으면 소유자 몫만 놓는다. 마지막 lease가 내린다.
    Release(state_);
}

FileStatus DynamicLibrary::Open(const char* absolutePath, usize pathBytes,
                                DynamicLibraryError* error) noexcept {
    ClearError(error);
    if (state_ != nullptr) {
        return {FileCode::AlreadyOpen, 0};
    }

    if (!detail::ValidFilePath(absolutePath, pathBytes)) {
        return {FileCode::InvalidArgument, 0};
    }

    uptr handle = 0;
    FileStatus status = detail::DynamicLibraryOpenPath(absolutePath, pathBytes, handle, error);
    if (!status) {
        return status;
    }

    return Adopt(state_, handle);
}

FileStatus DynamicLibrary::Open(const char* absolutePath) noexcept {
    if (absolutePath == nullptr) {
        return {FileCode::InvalidArgument, 0};
    }

    return Open(absolutePath, strlen(absolutePath));
}

FileStatus DynamicLibrary::OpenSystem(const char* name, usize nameBytes,
                                      DynamicLibraryError* error) noexcept {
    ClearError(error);
    if (state_ != nullptr) {
        return {FileCode::AlreadyOpen, 0};
    }

    if (!ValidSystemName(name, nameBytes)) {
        return {FileCode::InvalidArgument, 0};
    }

    uptr handle = 0;
    FileStatus status = detail::DynamicLibraryOpenSystem(name, nameBytes, handle, error);
    if (!status) {
        return status;
    }

    return Adopt(state_, handle);
}

FileStatus DynamicLibrary::OpenSystem(const char* name) noexcept {
    if (name == nullptr) {
        return {FileCode::InvalidArgument, 0};
    }

    return OpenSystem(name, strlen(name));
}

FileStatus DynamicLibrary::Find(DynamicSymbol* symbols, usize count, DynamicLibraryLease& lease,
                                usize* missingIndex) const noexcept {
    if (missingIndex != nullptr) {
        *missingIndex = IndexNone;
    }

    if (symbols == nullptr || count == 0) {
        return {FileCode::InvalidArgument, 0};
    }

    for (usize i = 0; i < count; ++i) {
        if (symbols[i].name == nullptr || symbols[i].name[0] == '\0') {
            return {FileCode::InvalidArgument, 0};
        }
    }

    if (state_ == nullptr) {
        return {FileCode::NotOpen, 0};
    }

    // 먼저 모두 찾고 나서 lease를 늘린다. 실패하면 채운 주소를 모두 되돌려 반쯤 채운 표가 남지 않게
    // 한다.
    const auto rollback = [symbols, count]() noexcept {
        for (usize i = 0; i < count; ++i) {
            symbols[i].address = nullptr;
        }
    };

    for (usize i = 0; i < count; ++i) {
        symbols[i].address = detail::DynamicLibraryFindSymbol(state_->handle, symbols[i].name);
        if (symbols[i].address == nullptr) {
            rollback();
            if (missingIndex != nullptr) {
                *missingIndex = i;
            }

            return {FileCode::NotFound, 0};
        }
    }

    // 다른 스레드의 Find와 겹쳐도 상한을 넘지 않게 비교 교환으로 늘린다. 소유자 몫이 있으므로
    // references는 1 이상이다.
    usize current = AtomicLoad<MemoryOrder::Relaxed>(&state_->references);
    do {
        if (current == MaxSize) {
            rollback();
            return {FileCode::Overflow, 0};
        }
    } while (!AtomicCompareExchangeWeak<MemoryOrder::AcquireRelease, MemoryOrder::Relaxed>(
        &state_->references, current, current + 1u));

    DynamicLibraryLease acquired;
    acquired.state_ = state_;
    lease = static_cast<DynamicLibraryLease&&>(acquired);
    return {};
}

FileStatus DynamicLibrary::Find(const char* symbol, void*& address,
                                DynamicLibraryLease& lease) const noexcept {
    DynamicSymbol entry{symbol, nullptr};
    const FileStatus status = Find(&entry, 1, lease);
    address = entry.address;
    return status;
}

usize DynamicLibrary::LeaseCount() const noexcept {
    if (state_ == nullptr) {
        return 0;
    }

    return AtomicLoad<MemoryOrder::Acquire>(&state_->references) - 1u;
}

FileStatus DynamicLibrary::Close() noexcept {
    if (state_ == nullptr) {
        return {};
    }

    // lease가 없을 때(소유자 몫 1만 있을 때)만 0으로 바꾼다. 실패하면 lease가 남아 있다.
    usize expected = 1;
    if (!AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(
            &state_->references, expected, usize{0})) {
        return {FileCode::Busy, 0};
    }

    State* state = state_;
    state_ = nullptr;
    const FileStatus status = detail::DynamicLibraryClose(state->handle);
    state->~State();
    AlignedFree(state);
    return status;
}

void detail::CopyDynamicLibraryErrorText(const char* text, DynamicLibraryError* error) noexcept {
    if (error == nullptr) {
        return;
    }

    const usize length = text == nullptr ? 0 : strlen(text);
    usize count = length;
    constexpr usize capacity = DynamicLibraryError::MaxTextBytes - 1u;
    if (count > capacity) {
        // 잘린 자리가 UTF-8 문자 중간이면(다음 byte가 이어짐 byte 10xxxxxx) 그 문자 시작까지
        // 물린다.
        count = capacity;
        while (count > 0 && (static_cast<unsigned char>(text[count]) & 0xC0u) == 0x80u) {
            --count;
        }

        error->truncated = true;
    }

    if (count != 0) {
        MemoryCopy(error->text, text, count);
    }

    error->text[count] = '\0';
}

DynamicLibraryLease& DynamicLibraryLease::operator=(DynamicLibraryLease&& other) noexcept {
    if (this != &other) {
        DynamicLibrary::State* previous = state_;
        state_ = other.state_;
        other.state_ = nullptr;
        Release(previous);
    }

    return *this;
}

void DynamicLibraryLease::Reset() noexcept {
    DynamicLibrary::State* state = state_;
    state_ = nullptr;
    Release(state);
}

} // namespace rocks::platform
