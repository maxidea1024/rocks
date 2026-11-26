#include <UnrealExchange/ExchangeIndexLookup.h>

#include <AssetDatabase/LogicalPath.h>
#include <Core/Algorithms.h>
#include <UnrealExchange/UnrealExchangeFormat.h>

// AssetIndex 태그 기반 기존 에셋 조회다(Docs/Design/UnrealExchange 9.2절, 10.5절 4번).

namespace rocks {

namespace {

// 정렬 표의 태그 종류다.
constexpr uint8 ObjectPathTagKind = 0;
constexpr uint8 FormerPathsTagKind = 1;

ExchangeExistingMatch MatchOf(Span<const AssetIndexEntry* const> entries, bool former) noexcept {
    ExchangeExistingMatch match;
    if (entries.IsEmpty()) {
        return match;
    }

    // 같은 id의 다른 파일이 있으면(태그가 한쪽에만 있어도) 어느 파일인지 정할 수 없다.
    if (entries.Size() > 1 || entries[0]->duplicateId) {
        match.state = ExchangeExistingState::Ambiguous;
        return match;
    }

    match.state = ExchangeExistingState::Found;
    match.id = entries[0]->id;
    match.path = entries[0]->path.View();
    match.former = former;
    return match;
}

} // namespace

Result<void> ExchangeIndexLookup::BuildRows() const noexcept {
    if (built_) {
        return Result<void>::Success();
    }

    for (usize index = 0; index < index_->Count(); ++index) {
        const AssetIndexEntry& entry = index_->At(index);
        for (const PackageTag& tag : entry.tags) {
            const bool current = tag.key.View() == UnrealExchangeObjectPathTag;
            if (!current && tag.key.View() != UnrealExchangeFormerPathsTag) {
                continue;
            }

            const TagRow row{current ? ObjectPathTagKind : FormerPathsTagKind, tag.value.View(),
                             &entry};
            if (auto pushed = rows_.TryPushBack(row); !pushed) {
                rows_.Clear();
                return pushed;
            }
        }
    }

    algo::Sort(rows_.View(), [](const TagRow& left, const TagRow& right) noexcept {
        if (left.kind != right.kind) {
            return left.kind < right.kind;
        }

        const int compared = StringView::CompareBytes(left.value, right.value);
        return compared != 0 ? compared < 0 : left.entry < right.entry;
    });
    built_ = true;
    return Result<void>::Success();
}

Result<void>
ExchangeIndexLookup::AppendMatches(uint8 kind, StringView value,
                                   Array<const AssetIndexEntry*>& entries) const noexcept {
    struct Key {
        uint8 kind;
        StringView value;
    };
    const Key key{kind, value};
    usize at =
        algo::LowerBound(rows_.View(), key, [](const TagRow& row, const Key& wanted) noexcept {
            return row.kind != wanted.kind ? row.kind < wanted.kind
                                           : StringView::CompareBytes(row.value, wanted.value) < 0;
        });
    for (; at < rows_.Size() && rows_[at].kind == kind && rows_[at].value == value; ++at) {
        bool seen = false;
        for (const AssetIndexEntry* entry : entries) {
            seen = seen || entry == rows_[at].entry;
        }

        if (seen) {
            continue;
        }

        if (auto pushed = entries.TryPushBack(rows_[at].entry); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

Result<ExchangeExistingMatch>
ExchangeIndexLookup::Find(StringView objectPath,
                          Span<const StringView> formerPaths) const noexcept {
    using MatchResult = Result<ExchangeExistingMatch>;
    if (auto built = BuildRows(); !built) {
        return MatchResult::Failure(built.Error());
    }

    Array<const AssetIndexEntry*> entries(*allocator_);
    if (auto added = AppendMatches(ObjectPathTagKind, objectPath, entries); !added) {
        return MatchResult::Failure(added.Error());
    }

    if (!entries.IsEmpty()) {
        return MatchResult::Success(MatchOf(entries.View(), false));
    }

    // 둘째 단계: 옛 경로로 남은 ObjectPath 태그(UE에서 이름을 바꾼 뒤 처음 다시 임포트)와 옛 경로
    // 목록 태그.
    for (const StringView former : formerPaths) {
        if (auto added = AppendMatches(ObjectPathTagKind, former, entries); !added) {
            return MatchResult::Failure(added.Error());
        }
    }

    if (auto added = AppendMatches(FormerPathsTagKind, objectPath, entries); !added) {
        return MatchResult::Failure(added.Error());
    }

    for (const StringView former : formerPaths) {
        if (auto added = AppendMatches(FormerPathsTagKind, former, entries); !added) {
            return MatchResult::Failure(added.Error());
        }
    }

    return MatchResult::Success(MatchOf(entries.View(), true));
}

Result<bool> ExchangeIndexLookup::PathTaken(StringView logicalPath) const noexcept {
    auto parsed = ParseLogicalPath(*allocator_, logicalPath);
    if (!parsed) {
        return Result<bool>::Failure(parsed.Error());
    }

    // FindByPath는 조회만 한다(할당 없음): 있으면 성공, 같은 경로 키 둘이면 AlreadyExists(역시
    // 점유), 없으면 NotFound다.
    auto found = index_->FindByPath(parsed.Value());
    return Result<bool>::Success(found.IsOk() || found.Error() != ErrorCode::NotFound);
}

} // namespace rocks
