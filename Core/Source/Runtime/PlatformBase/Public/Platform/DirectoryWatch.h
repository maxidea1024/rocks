#pragma once

#include <Platform/File.h>

namespace rocks::platform {

// 디렉터리 감시 이벤트의 종류다.
enum class DirectoryChangeKind : uint8 {
    // 항목(파일·디렉터리)이 생겼다. 다른 곳에서 감시 루트 안으로 옮겨 온 항목도 여기에 해당한다.
    Added,
    // 항목이 없어졌다. 감시 루트 밖으로 옮겨 간 항목도 여기에 해당한다.
    Removed,
    // 파일 내용·크기·쓰기 시각 등이 바뀌었다. 한 번의 저장이 여러 Modified로 올 수 있다.
    Modified,
    // 감시 루트 안에서 이름이 바뀌었다(path = 새 경로, oldPath = 이전 경로). 플랫폼이 두 이름을
    // 짝지어 알려 주지 못하면 Removed(이전) + Added(새)로 온다.
    Renamed,
    // 이벤트 일부를 잃었다(OS 버퍼 넘침·커널 삭제·watch 한도·이름 해석 실패). path 아래 하위 트리를
    // 다시 검색해야 한다(path가 비면 감시 루트 전체).
    Overflow,
    // 감시 루트 자체가 지워지거나 옮겨졌다. 이후 이벤트는 오지 않으며 Close 후 다시 Open해야 한다.
    RootRemoved
};

// 항목 종류다. 플랫폼이 알려 주지 않거나(Windows Removed) 이미 사라져 알 수 없으면 Unknown이다.
enum class DirectoryEntryKind : uint8 {
    Unknown,
    File,
    Directory
};

// Poll이 넘기는 이벤트 하나다. 경로는 감시 루트 기준 상대 경로(UTF-8, 구분자 '/', 앞뒤 '/' 없음,
// NUL 종료)이며 visitor 호출 동안만 유효하다. 이름은 OS가 돌려준 byte 그대로다(Unicode 정규화를
// 하지 않는다. macOS는 NFD 이름을 돌려줄 수 있으므로 비교하는 쪽이 정규화한다).
struct DirectoryChange {
    DirectoryChangeKind kind = DirectoryChangeKind::Modified;
    DirectoryEntryKind entry = DirectoryEntryKind::Unknown;
    const char* path = "";
    usize pathBytes = 0;
    // Renamed의 이전 경로다. 그 밖에는 빈 문자열이다.
    const char* oldPath = "";
    usize oldPathBytes = 0;
};

// false를 반환하면 이번 Poll의 나머지 이벤트를 다음 Poll로 미룬다(버리지 않는다).
using DirectoryChangeVisitor = bool (*)(const DirectoryChange& change, void* context) noexcept;

// 디렉터리 하나를 하위 디렉터리까지 감시한다(UE IDirectoryWatcher 대응, 에셋 원본 변경 감지·자동
// 재임포트용).
//
// 사용 예:
//   platform::DirectoryWatcher watcher;
//   if (auto opened = watcher.Open("C:/Game/SourceArt"); !opened) { ... }
//   for (;;) {
//       if (watcher.Wait(500)) {
//           auto polled = watcher.Poll(
//               [](const platform::DirectoryChange& change, void*) noexcept {
//                   // change.kind, change.path 처리
//                   return true;
//               },
//               nullptr);
//       }
//   }
//   watcher.Close();
//
// 이식 계약(플랫폼 사이에서 보장하는 것과 보장하지 않는 것):
// - 보장: 변경 뒤 언젠가 그 경로에 대한 이벤트가 온다. 이벤트를 놓쳤으면 Overflow가 온다. 이벤트는
//   OS가 보고한 순서대로 넘긴다. 열기 이후의 변경만 보고한다(기존 항목 목록은 보내지 않는다).
// - 보장하지 않음: 이벤트 개수(합쳐지거나 중복될 수 있다), Renamed 짝(Removed+Added로 올 수 있다),
//   항목 종류(Unknown일 수 있다), 디렉터리 안 항목별 이벤트(디렉터리를 통째로 옮기면 그 디렉터리에
//   대한 이벤트 하나만 올 수 있다). 따라서 이벤트는 "다시 확인할 경로" 힌트로 다루고, 실제 상태는
//   Stat·EnumerateDirectory로 확인한다.
// - link는 따라가지 않는다. 감시 루트가 link면 OS가 해석한 대상을 감시한다.
//
// 플랫폼 구현:
// - Windows: ReadDirectoryChangesExW(overlapped, 하위 트리, 확장 정보로 항목 종류). 버퍼 넘침은
//   Overflow다. 8.3 짧은 이름은 긴 이름으로 바꾸고, 바꿀 수 없으면 부모 디렉터리의 Overflow로
//   보낸다.
// - macOS: FSEvents(파일 단위 이벤트, 자체 dispatch queue). 합쳐진 flag는 lstat으로 현재 상태를
//   확인해 Added·Removed·Modified로 정한다. Renamed 짝을 짓지 않는다.
// - Android(Linux): inotify. 하위 디렉터리마다 watch를 두고 새 디렉터리는 생기는 즉시 watch를
//   추가하며 그 사이에 생긴 자식은 Added로 보낸다. MOVED_FROM/MOVED_TO cookie로 Renamed 짝을
//   짓는다. IN_Q_OVERFLOW·watch 한도 초과는 Overflow다.
//
// 스레드: 한 감시기의 Open·Poll·Wait·Close는 같은 스레드에서 호출한다(다른 스레드의 동시 호출은
// Busy). visitor 안에서 같은 감시기를 호출하면 Busy다. 객체 파괴 전 Close를 부르지 않았으면
// 소멸자가 닫는다.
class DirectoryWatcher final {
public:
    DirectoryWatcher() noexcept = default;
    ~DirectoryWatcher() noexcept;
    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;

    // path(UTF-8, 길이 지정)를 하위 디렉터리까지 감시한다. 이미 열려 있으면 AlreadyOpen, 없으면
    // NotFound, 디렉터리가 아니면 WrongType, 이 플랫폼에 backend가 없으면 Unsupported, 메모리
    // 부족은 OutOfMemory, 그 밖은 OS 오류다. 실패하면 닫힌 상태로 남는다.
    FileStatus Open(const char* path, usize pathBytes) noexcept;

    // NUL 종료 path를 쓰는 편의 함수다.
    FileStatus Open(const char* path) noexcept;

    // 지금까지 쌓인 이벤트를 visitor로 넘긴다. 기다리지 않는다(없으면 바로 Success). 열려 있지
    // 않으면 NotOpen, OS 오류는 그 오류이며 이때 감시는 계속 열려 있지만 Overflow를 먼저 보낸 뒤
    // 복구를 시도한다.
    FileStatus Poll(DirectoryChangeVisitor visitor, void* context) noexcept;

    // 이벤트가 생기거나 timeoutMilliseconds가 지날 때까지 기다린다. 이벤트가 있으면 true다(Poll을
    // 부를 것). 열려 있지 않으면 바로 false다.
    bool Wait(uint32 timeoutMilliseconds) noexcept;

    // 감시를 끝낸다. 여러 번 호출해도 안전하다. 아직 넘기지 않은 이벤트는 버린다.
    void Close() noexcept;

    bool IsOpen() const noexcept {
        return state_ != nullptr;
    }

private:
    struct State;
    State* state_ = nullptr;
};

} // namespace rocks::platform
