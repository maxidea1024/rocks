#pragma once

#include <Platform/DirectoryWatch.h>

// 디렉터리 변경 이벤트의 공급원이다(Docs/Design/AssetSystem.ko.md 7절 갱신, 7.1절 9 — A2a).
// AssetIndex의 감시 갱신은 이 인터페이스로 이벤트를 받는다. 실제 OS 감시는 PlatformBase
// DirectoryWatcher를 감싼 WatcherChangeSource이고, 시험은 이벤트를 직접 주입하는 구현으로 순서와
// 종류(추가·삭제·수정·이름 변경·Overflow·RootRemoved)를 결정적으로 만든다.
//
// 사용 예:
//   WatcherChangeSource source;
//   if (!source.Open(contentDirectory)) { ... }
//   source.Poll(visitor, context);   // 쌓인 이벤트를 넘긴다(기다리지 않는다)
namespace rocks {

class DirectoryChangeSource {
public:
    // 쌓인 이벤트를 visitor에 넘긴다(기다리지 않는다). 오류는 platform::DirectoryWatcher::Poll과
    // 같다.
    virtual platform::FileStatus Poll(platform::DirectoryChangeVisitor visitor,
                                      void* context) noexcept = 0;

    // 이벤트가 올 때까지 최대 timeoutMilliseconds 기다린다. 왔으면 true다.
    virtual bool Wait(uint32 timeoutMilliseconds) noexcept = 0;

protected:
    ~DirectoryChangeSource() = default;
};

// platform::DirectoryWatcher를 감싼 OS 감시다.
class WatcherChangeSource final : public DirectoryChangeSource {
public:
    // path(디렉터리, 하위 트리 포함)를 감시한다. 오류는 platform::DirectoryWatcher::Open과 같다.
    platform::FileStatus Open(const char* path, usize pathBytes) noexcept {
        return watcher_.Open(path, pathBytes);
    }

    void Close() noexcept {
        watcher_.Close();
    }

    platform::FileStatus Poll(platform::DirectoryChangeVisitor visitor,
                              void* context) noexcept override {
        return watcher_.Poll(visitor, context);
    }

    bool Wait(uint32 timeoutMilliseconds) noexcept override {
        return watcher_.Wait(timeoutMilliseconds);
    }

private:
    platform::DirectoryWatcher watcher_;
};

} // namespace rocks
