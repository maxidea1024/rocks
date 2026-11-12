#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Span.h>
#include <Core/Text/String.h>
#include <Platform/Sync.h>

namespace rocks {

using TextId = Guid;

namespace detail {
struct CatalogData;
class TextResolver;
} // namespace detail

// Create 호출 동안만 빌리는 입력이다. snapshot은 모든 번역 byte를 독립 소유한다.
struct CatalogEntry {
    TextId id;
    uint64 sourceRevision = 0;
    StringView translation;
};

struct CatalogLimits {
    usize maxEntries = 4096;
    usize maxTotalBytes = 4 * 1024 * 1024;
};

// 불변 번역 데이터의 이동 전용 공유 소유권이다. Clone은 명시적으로 보존 횟수를 늘린다.
// 서로 다른 래퍼의 Clone/Reset/읽기는 동시 사용 가능하다. 동일 래퍼의 변경은 동기화한다.
// allocator는 마지막 snapshot 해제까지 살아 있고 해당 스레드의 Allocate/Deallocate를 지원해야 한다.
// allocator 콜백에서 현재 호출의 입력 snapshot 래퍼를 Reset/move하는 재진입은 금지한다.
// culture와 revision은 자동 정규화하지 않는 opaque identity다. 전역 locale을 사용하지 않는다.
class CatalogSnapshot {
public:
    CatalogSnapshot() noexcept = default;
    CatalogSnapshot(CatalogSnapshot&&) noexcept;
    CatalogSnapshot& operator=(CatalogSnapshot&&) noexcept;
    CatalogSnapshot(const CatalogSnapshot&) = delete;
    CatalogSnapshot& operator=(const CatalogSnapshot&) = delete;
    ~CatalogSnapshot() noexcept;

    // 빈/잘못된 UTF-8 culture, revision=0, entry id/revision=0, 중복 id는 거부한다.
    // 입력 수명은 이 호출까지면 충분하다. 실패는 게시된 snapshot이나 입력을 변경하지 않는다.
    // 초기 구현의 hard ceiling은 entries 4096, 전체 번역+문화 byte 16 MiB다.
    static Result<CatalogSnapshot> Create(Allocator&, StringView culture, uint64 revision,
                                          Span<const CatalogEntry>, CatalogLimits = {}) noexcept;
    Result<CatalogSnapshot> Clone() const noexcept;
    void Reset() noexcept;
    bool IsValid() const noexcept;
    // 반환 뷰는 같은 데이터를 보존하는 snapshot이 살아 있는 동안만 유효하다.
    // invalid snapshot은 빈 culture와 revision=0을 반환한다.
    StringView Culture() const noexcept;
    uint64 Revision() const noexcept;
    usize EntryCount() const noexcept;

private:
    explicit CatalogSnapshot(detail::CatalogData* data) noexcept : data_(data) {
    }

    detail::CatalogData* data_ = nullptr;
    friend class detail::TextResolver;
};

// 현재 snapshot 한 개를 게시한다. Publish/Acquire/Close는 서로 동시 호출할 수 있다.
// 교체 전 snapshot은 기존 reader가 계속 보존하며 store가 소멸한 뒤에도 유효하다.
// 소멸은 모든 store API 호출 종료 뒤 수행한다. allocator 콜백은 store gate 밖에서 실행한다.
// Publish 순서는 caller가 결정한다. culture/revision의 최신성이나 비동기 요청 순서를 추측하지
// 않는다.
class CatalogStore {
public:
    CatalogStore() noexcept = default;
    ~CatalogStore() noexcept;
    CatalogStore(const CatalogStore&) = delete;
    CatalogStore& operator=(const CatalogStore&) = delete;

    // invalid 입력은 InvalidArgument다. 실패하면 현재 게시값은 유지한다.
    Result<void> Publish(const CatalogSnapshot&) noexcept;
    // 아직 게시하지 않았다면 NotFound, Close 이후에는 ShuttingDown이다.
    Result<CatalogSnapshot> Acquire() const noexcept;
    // 새 Publish/Acquire만 닫는다. 이미 획득한 snapshot을 무효화하지 않는다. 반복 호출 가능하다.
    void Close() noexcept;

private:
    mutable platform::Mutex gate_;
    CatalogSnapshot current_;
    bool closed_ = false;
};

} // namespace rocks
