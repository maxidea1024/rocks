#pragma once

#include <AssetDatabase/AssetClock.h>
#include <AssetDatabase/AssetIndex.h>
#include <Platform/DirectoryWatch.h>

// 색인 감시 갱신이다(Docs/Design/AssetSystem.ko.md 7절, 7.1절 16 — A2d3, UE AssetRegistry의
// DirectoryWatcher 연결 대응). Content의 변경 이벤트를 받아 바뀐 파일·하위 트리만 다시 읽는다.
// 감시가 바꾸는 것은 색인뿐이다 — 이미 열린 패키지의 외부 변경은 저장 1단계(지문)와 지연 읽기가
// 잡는다.
//
// 사용 예(에디터 tick):
//   PlatformAssetChangeSource source;
//   SystemAssetClock clock;
//   AssetIndexWatch watch(allocator, index, files, readOptions, source, clock);
//   if (auto started = watch.Start(&cache); !started) { ... }
//   // tick마다
//   if (auto pumped = watch.Pump(); !pumped) { /* 할당 실패 등 — 다음 Pump가 다시 한다 */ }
//
// 스레드: 색인과 같은 한 스레드에서 쓴다. 기다리지 않는다(sleep 없음 — 호출자가 Pump를 돈다).
namespace rocks {

// 감시 루트가 사라진 뒤 다시 열어 보는 간격이다(1초).
inline constexpr int64 AssetWatchRootRetryTicks = AssetClockTicksPerSecond;
// 쓰는 중 파일의 크기·시각이 같은 두 관찰 사이 최소 간격이다(500 ms — 7절 debounce와 같다).
inline constexpr int64 AssetWatchStableTicks = 500 * AssetClockTicksPerMillisecond;
// 쓰는 중 파일을 기다리는 상한이다(처음 본 뒤 10초 — 넘으면 다시 읽어 그 진단을 남긴다).
inline constexpr int64 AssetWatchWriteTimeoutTicks = 10 * AssetClockTicksPerSecond;

// 변경 이벤트의 원천이다(platform::DirectoryWatcher를 감싸는 것과 시험 가짜). 이벤트 계약은
// platform::DirectoryWatcher와 같다(경로는 Content 상대, 종류는 힌트, 놓치면 Overflow).
class AssetChangeSource {
public:
    // root(절대 경로)를 하위 디렉터리까지 감시하기 시작한다. 오류: 없음 NotFound, 그 밖의 원천
    // 오류(열리지 않은 채 남는다).
    virtual Result<void> Open(StringView root) noexcept = 0;
    // 쌓인 이벤트를 visitor로 넘긴다(기다리지 않는다). visitor가 거짓이면 그 이벤트부터 다음
    // Poll로 미룬다. 오류: 원천 오류(감시는 열린 채 남고 잃은 이벤트는 Overflow로 온다).
    virtual Result<void> Poll(platform::DirectoryChangeVisitor visitor, void* context) noexcept = 0;
    // 감시를 끝낸다(여러 번 불러도 된다 — 넘기지 않은 이벤트는 버린다).
    virtual void Close() noexcept = 0;

protected:
    ~AssetChangeSource() = default;
};

// platform::DirectoryWatcher로 만든 원천이다.
class PlatformAssetChangeSource final : public AssetChangeSource {
public:
    Result<void> Open(StringView root) noexcept override;
    Result<void> Poll(platform::DirectoryChangeVisitor visitor, void* context) noexcept override;
    void Close() noexcept override;

    // 이벤트가 생기거나 timeoutMilliseconds가 지날 때까지 기다린다(이벤트가 있으면 참). 감시를
    // 도는 스레드가 tick 사이에 쓴다.
    bool Wait(uint32 timeoutMilliseconds) noexcept;

private:
    platform::DirectoryWatcher watcher_;
};

class AssetIndexWatch {
public:
    // index·files·source·clock은 감시보다 오래 산다. options는 복사한다(customVersions 같은 span이
    // 가리키는 것은 호출자가 살려 둔다).
    AssetIndexWatch(Allocator& allocator, AssetIndex& index, platform::FileSystem& files,
                    const PackageReadOptions& options, AssetChangeSource& source,
                    AssetClock& clock) noexcept;
    ~AssetIndexWatch() noexcept;

    AssetIndexWatch(const AssetIndexWatch&) = delete;
    AssetIndexWatch& operator=(const AssetIndexWatch&) = delete;

    // 원천을 Content에 열고 색인을 다시 검색한다(cache가 있으면 재사용 — 7.1절 16 A2d1). 연 뒤의
    // 변경은 이벤트로 쌓여 다음 Pump가 적용한다(검색 중 바뀐 파일도 다시 읽힌다). 오류: 이미
    // 시작했으면 InvalidArgument, 원천 열기 오류(Content가 없으면 NotFound), Rebuild 오류 — 원천은
    // 닫힌 채 남는다(Rebuild 오류면 색인은 Rebuild가 남긴 상태다).
    Result<void> Start(const AssetIndexCache* cache = nullptr) noexcept;

    // 쌓인 이벤트를 적용한다. 경로마다 다시 볼 곳: `.rasset`이면 그 파일(종류가 디렉터리면 빼고),
    // 디렉터리이거나 종류를 모르면 그 하위 트리, Renamed는 옛 이름과 새 이름 둘 다, Overflow는 그
    // 하위 트리(빈 경로면 전체), 원천 오류·이벤트 기록의 할당 실패도 전체다. 그 밖의 파일(임시
    // 파일 등)은 무시한다. 유효한 UTF-8이 아닌 경로는 그 앞의 유효한 디렉터리 하위 트리다.
    //
    // 쓰는 중 파일: 이벤트로 다시 읽은 파일이 CorruptPackage이거나 Busy면 색인을 바꾸지 않고
    // 기다린다. 크기·수정 시각이 같은 관찰이 AssetWatchStableTicks 이상 떨어져 두 번 나오거나,
    // 처음 본 뒤 AssetWatchWriteTimeoutTicks가 지나거나, Stat이 실패하면(없어짐 등) 다시 읽어 그
    // 결과를 남긴다. 새 이벤트는 다시 미룰 수 있다(시간 상한 전까지).
    //
    // 루트 소실(RootRemoved): 원천을 닫고 색인을 비워 RootLost 진단 하나를 두고(대기 목록도
    // 비운다), 이후 Pump가 AssetWatchRootRetryTicks 간격으로 다시 열어 보고 열리면 전체를 다시
    // 검색한다. 시계가 뒤로 가면 그 기준 시각을 지금으로 당긴다(기다림이 길어질 뿐 앞당기지
    // 않는다).
    //
    // 오류: 시작하지 않았으면 InvalidArgument, 다시 읽기 오류(할당 실패·Content 열거 실패),
    // 재열기의 원천 오류(NotFound 제외 — 그때도 다음 간격에 다시 연다) — 다시 읽기가 실패하면
    // 색인은 그대로이고 모은 일은 다음 Pump가 다시 한다.
    Result<void> Pump() noexcept;

    // 원천을 닫고 모은 일·대기 목록을 버린다(색인은 그대로). 시작하지 않았어도 된다.
    void Stop() noexcept;

    bool IsStarted() const noexcept {
        return started_;
    }

    // 루트를 잃고 다시 열기를 기다리는지다.
    bool IsRootLost() const noexcept {
        return rootLost_;
    }

    // 쓰는 중이라 기다리는 파일 수다.
    usize WaitingCount() const noexcept {
        return waiting_.Size();
    }

    // 다음 Pump가 다시 읽을 파일·하위 트리 수(전체 다시 검색이면 그것과 별도)다.
    usize PendingFiles() const noexcept {
        return files_.Size();
    }

    usize PendingSubtrees() const noexcept {
        return subtrees_.Size();
    }

    bool FullRescanPending() const noexcept {
        return fullRescan_;
    }

private:
    // 쓰는 중이라 기다리는 파일이다.
    struct Waiting {
        explicit Waiting(Allocator& allocator) noexcept : name(allocator) {
        }

        String name;
        int64 firstSeen = 0;
        int64 observedAt = 0;
        uint64 observedBytes = 0;
        int64 observedTicks = 0;
        bool observed = false;
    };

    static bool Visit(const platform::DirectoryChange& change, void* context) noexcept;
    void AddPath(StringView path, platform::DirectoryEntryKind entry) noexcept;
    void AddName(Array<String>& to, StringView name) noexcept;
    Result<void> Reopen(int64 now) noexcept;
    Result<void> LoseRoot(int64 now) noexcept;
    // 대기 파일 하나를 Stat해 관찰을 갱신하고 지금 다시 읽을지 정한다.
    Result<bool> Observe(Waiting& waiting, int64 now) noexcept;
    Result<void> Apply(int64 now) noexcept;
    void ClearWork() noexcept;

    Allocator* allocator_;
    AssetIndex* index_;
    platform::FileSystem* fileSystem_;
    PackageReadOptions options_;
    AssetChangeSource* source_;
    AssetClock* clock_;
    Array<String> files_;
    Array<String> subtrees_;
    Array<Waiting> waiting_;
    int64 lastReopen_ = 0;
    bool started_ = false;
    bool sourceOpen_ = false;
    bool rootLost_ = false;
    bool rootRemoved_ = false;
    bool fullRescan_ = false;
};

} // namespace rocks
