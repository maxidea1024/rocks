#pragma once

#include <Platform/DirectoryWatch.h>

// DirectoryWatcher 공용 상태와 플랫폼 hook이다. 공용 수명·재진입 검사·이벤트 전달은
// Common/DirectoryWatch.cpp가, OS 감시는 Private/<플랫폼>/DirectoryWatch.cpp가 맡는다.
namespace rocks::platform::detail {

// 아직 넘기지 않은 이벤트의 FIFO다. 이벤트는 가변 길이 레코드로 연속 버퍼에 쌓는다. 쌓인 크기가
// MaxBytes를 넘거나 할당에 실패하면 모두 버리고 루트 Overflow 하나로 접는다(메모리 상한). 루트
// Overflow 뒤의 이벤트는 그 재검색에 포함되어 버린다. RootRemoved는 항상 마지막에 남는다. 스레드
// 안전하지 않다(호출자가 직렬화한다).
class WatchEventQueue {
public:
    static constexpr usize MaxBytes = 4u * 1024u * 1024u;

    WatchEventQueue() noexcept = default;
    ~WatchEventQueue() noexcept;
    WatchEventQueue(const WatchEventQueue&) = delete;
    WatchEventQueue& operator=(const WatchEventQueue&) = delete;

    // 이벤트를 끝에 붙인다. 경로는 복사한다. 할당에 실패하거나 상한을 넘으면 Overflow 하나로 접고
    // false를 반환한다.
    bool Push(DirectoryChangeKind kind, DirectoryEntryKind entry, const char* path, usize pathBytes,
              const char* oldPath = "", usize oldPathBytes = 0) noexcept;

    // 다른 큐의 이벤트를 모두 끝으로 옮긴다(source는 빈다).
    void TakeAll(WatchEventQueue& source) noexcept;

    bool IsEmpty() const noexcept {
        return head_ == used_ && !rootOverflow_ && !rootRemoved_;
    }

    // 맨 앞 이벤트다. 비어 있으면 false다. change의 경로는 Pop·Push 전까지 유효하다.
    bool Front(DirectoryChange& change) const noexcept;
    void Pop() noexcept;
    void Clear() noexcept;

private:
    struct Record {
        uint8 kind;
        uint8 entry;
        uint16 reserved;
        uint32 pathBytes;
        uint32 oldPathBytes;
        uint32 recordBytes;
    };

    bool Reserve(usize bytes) noexcept;
    void CollapseToOverflow() noexcept;

    uint8* data_ = nullptr;
    usize capacity_ = 0;
    // [head_, used_)가 남은 레코드다.
    usize head_ = 0;
    usize used_ = 0;
    // 루트 전체 Overflow(맨 앞)와 루트 소실(맨 뒤)은 할당 없이 플래그로 둔다. 메모리가 부족해도
    // 사라지지 않는다.
    bool rootOverflow_ = false;
    bool rootRemoved_ = false;
};

// 플랫폼 backend의 불투명 상태다.
struct WatchBackend;

// ---- 플랫폼 hook ----
// path를 감시하기 시작한다. 성공하면 backend를 소유해 돌려준다. path는 검증된 UTF-8이다.
FileStatus WatchOpen(const char* path, usize pathBytes, WatchBackend*& backend) noexcept;
// OS에 쌓인 이벤트를 기다리지 않고 out에 옮긴다.
FileStatus WatchCollect(WatchBackend& backend, WatchEventQueue& out) noexcept;
// OS 이벤트가 생기거나 시간이 지날 때까지 기다린다. 이벤트가 있을 수 있으면 true다.
bool WatchWait(WatchBackend& backend, uint32 timeoutMilliseconds) noexcept;
// 감시를 끝내고 backend를 해제한다.
void WatchClose(WatchBackend* backend) noexcept;

// ---- 공용 도구 ----
// 경로 구성 요소를 '/'로 이어 out에 쓴다(NUL 종료). base가 비면 name만 쓴다. 공간이 부족하면
// false다.
bool JoinRelativePath(const char* base, usize baseBytes, const char* name, usize nameBytes,
                      char* out, usize capacity, usize& outBytes) noexcept;

} // namespace rocks::platform::detail
