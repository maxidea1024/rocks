#include <ShaderCompiler/ShaderSourceCache.h>

#include "ShaderPreprocessorInternal.h"
#include <Base/PlacementNew.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Text/String.h>
#include <Platform/Sync.h>

// 셰이더 원문 캐시(Docs/Design/ShaderSystem 16.6절 SSD98)다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace source_cache_detail {

// 항목 하나의 저장소다. 넣은 뒤에는 바뀌지 않는다(잠금 없이 읽는다).
struct Entry {
    explicit Entry(Allocator& allocator) noexcept : path(allocator), arena(allocator) {
    }

    String path;
    preprocess_detail::TextArena arena;
    ShaderSourceCacheEntry value;
};

} // namespace source_cache_detail

struct ShaderSourceCache::State {
    explicit State(Allocator& allocator) noexcept
        : allocator(allocator), entries(allocator), index(allocator) {
    }

    ~State() noexcept {
        for (source_cache_detail::Entry* entry : entries) {
            entry->~Entry();
            (void)allocator.Deallocate(entry);
        }
    }

    Allocator& allocator;
    platform::Mutex mutex;
    Array<source_cache_detail::Entry*> entries;
    // 항목 경로(항목 저장소를 빌린 view) → entries 색인이다.
    HashMap<StringView, uint32> index;
};

ShaderSourceCache::ShaderSourceCache(const ShaderSourceTree& tree, Allocator& allocator,
                                     usize fileBytes) noexcept
    : tree_(tree), allocator_(allocator), fileBytes_(fileBytes) {
    auto memory = allocator_.Allocate(sizeof(State), alignof(State));
    if (memory) {
        state_ = ::new (memory.Value()) State(allocator_);
    }
}

ShaderSourceCache::~ShaderSourceCache() noexcept {
    if (state_ != nullptr) {
        state_->~State();
        (void)allocator_.Deallocate(state_);
    }
}

usize ShaderSourceCache::EntryCount() const noexcept {
    if (state_ == nullptr) {
        return 0;
    }

    platform::LockGuard lock(state_->mutex);
    return state_->entries.Size();
}

const ShaderSourceCacheEntry* ShaderSourceCache::Get(StringView virtualPath) noexcept {
    using source_cache_detail::Entry;
    if (state_ == nullptr) {
        return nullptr;
    }

    {
        platform::LockGuard lock(state_->mutex);
        if (const uint32* found = state_->index.Find(virtualPath)) {
            return &state_->entries[*found]->value;
        }
    }

    // 잠금 밖에서 읽고 정규화한다(파일 읽기가 다른 경로의 조회를 막지 않는다).
    auto memory = allocator_.Allocate(sizeof(Entry), alignof(Entry));
    if (!memory) {
        return nullptr;
    }

    Entry* entry = ::new (memory.Value()) Entry(allocator_);
    auto release = [&]() noexcept {
        entry->~Entry();
        (void)allocator_.Deallocate(entry);
    };

    if (!entry->path.TryAssign(virtualPath)) {
        release();
        return nullptr;
    }

    Array<uint8> bytes(allocator_);
    entry->value.loadStatus = tree_.Load(entry->path.View(), bytes, fileBytes_);
    if (entry->value.loadStatus) {
        entry->value.sha256 = Sha256::Hash(bytes.View());
        entry->value.normalizeStatus = preprocess_detail::NormalizeSource(
            bytes.View(), entry->path.View(), entry->arena, entry->value.text);
    }

    platform::LockGuard lock(state_->mutex);
    if (const uint32* found = state_->index.Find(virtualPath)) {
        // 다른 스레드가 먼저 넣었다(같은 파일이라 결과가 같다 — 먼저 넣은 쪽을 쓴다).
        release();
        return &state_->entries[*found]->value;
    }

    const uint32 slot = static_cast<uint32>(state_->entries.Size());
    if (!state_->entries.TryPushBack(entry)) {
        release();
        return nullptr;
    }

    if (!state_->index.Insert(entry->path.View(), slot)) {
        (void)state_->entries.TryRemoveAt(slot);
        release();
        return nullptr;
    }

    return &entry->value;
}

} // namespace rocks::shader
