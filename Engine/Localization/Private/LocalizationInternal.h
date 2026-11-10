#pragma once

#include <Localization/Text.h>
#include <Core/Containers/Array.h>

namespace rocks::detail {

enum class TextKind : uint8 {
    Invariant,
    Localized,
    Format
};

struct TextArgumentData {
    explicit TextArgumentData(Allocator& allocator) noexcept : name(allocator) {
    }

    TextArgumentData(TextArgumentData&&) noexcept = default;
    TextArgumentData& operator=(TextArgumentData&&) noexcept = default;
    String name;
    TextNode* value = nullptr;
};

// raw child pointer는 이 노드가 각각 한 reference를 소유한다. 소멸자는 child를 재귀 해제하지
// 않는다. references=0이 된 노드만 cleanupNext에 연결해 할당 없는 반복 해제에 사용한다.
struct TextNode {
    TextNode(Allocator& owner, TextKind value) noexcept
        : allocator(&owner), kind(value), source(owner), arguments(owner) {
    }

    Allocator* allocator;
    platform::Mutex gate;
    uint64 references = 1;
    TextNode* cleanupNext = nullptr;
    TextKind kind;
    String source;
    TextId id;
    uint64 sourceRevision = 0;
    TextNode* pattern = nullptr;
    Array<TextArgumentData> arguments;
};

struct CatalogOwnedEntry {
    explicit CatalogOwnedEntry(Allocator& allocator) noexcept : translation(allocator) {
    }

    CatalogOwnedEntry(CatalogOwnedEntry&&) noexcept = default;
    CatalogOwnedEntry& operator=(CatalogOwnedEntry&&) noexcept = default;
    TextId id;
    uint64 sourceRevision = 0;
    String translation;
};

struct CatalogData {
    explicit CatalogData(Allocator& owner) noexcept
        : allocator(&owner), culture(owner), entries(owner) {
    }

    Allocator* allocator;
    platform::Mutex gate;
    uint64 references = 1;
    String culture;
    uint64 revision = 0;
    Array<CatalogOwnedEntry> entries;
};

Result<void> RetainText(TextNode*) noexcept;
void ReleaseText(TextNode*) noexcept;
Result<void> RetainCatalog(CatalogData*) noexcept;
void ReleaseCatalog(CatalogData*) noexcept;
bool IsArgumentName(StringView) noexcept;

} // namespace rocks::detail
