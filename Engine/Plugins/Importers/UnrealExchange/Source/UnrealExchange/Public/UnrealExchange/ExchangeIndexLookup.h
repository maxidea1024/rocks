#pragma once

#include <AssetDatabase/AssetIndex.h>
#include <UnrealExchange/ExchangeImportOrder.h>

// AssetIndex 태그로 UE objectPath의 기존 Rocks 에셋을 찾는 조회다(Docs/Design/UnrealExchange
// 9.2절, 10.5절 4번). 임포트가 남긴 태그 UnrealExchange.ObjectPath(지금 UE 경로)·FormerPaths(묶음
// redirect로 알게 된 옛 경로)를 본다. 처음 조회할 때 두 태그의 (키, 값) 정렬 표를 한 번 만들고
// 조회마다 이진 검색한다(묶음 에셋마다 색인 전체를 훑지 않는다).
//
// 사용 예:
//   ExchangeIndexLookup lookup(session.service->Index(), allocator);
//   ExchangeOrderOptions options;
//   options.existing = &lookup;
//   auto planned = PlanExchangeImport(bundle, options, plan, diagnostics);

namespace rocks {

class ROCKS_UNREALEXCHANGE_API ExchangeIndexLookup final : public ExchangeExistingAssets {
public:
    // index는 이 조회보다 오래 살아야 하고 조회하는 동안 바뀌지 않아야 한다(돌려준 path는 그 색인
    // 항목의 문자열이고 정렬 표가 항목을 가리킨다). allocator는 정렬 표와 조회 하나의 임시 목록에
    // 쓴다.
    ExchangeIndexLookup(const AssetIndex& index, Allocator& allocator) noexcept
        : index_(&index), allocator_(&allocator), rows_(allocator) {
    }

    // 두 단계로 찾는다: ObjectPath 태그 = objectPath, 없으면 ObjectPath 태그 ∈ formerPaths 또는
    // FormerPaths 태그 ∈ {objectPath, formerPaths}(former 표시). 처음 찾은 단계의 서로 다른 색인
    // 항목이 하나이고 그 id가 색인에서 유일하면 Found(id·지금 경로), 둘 이상이거나 같은 id의 다른
    // 파일이 있으면 Ambiguous다. 오류: 할당 실패(정렬 표 만들기 포함 — 다음 조회가 다시 만든다).
    Result<ExchangeExistingMatch> Find(StringView objectPath,
                                       Span<const StringView> formerPaths) const noexcept override;

    // 논리 경로를 쓰는 색인 항목이 있으면 참이다(같은 경로 키 둘도 참). 오류: 경로 규칙 위반
    // InvalidArgument·InvalidPath, 할당 실패.
    Result<bool> PathTaken(StringView logicalPath) const noexcept override;

private:
    // 정렬 표 한 줄이다: 태그 종류(0 = ObjectPath, 1 = FormerPaths), 값, 색인 항목.
    struct TagRow {
        uint8 kind = 0;
        StringView value;
        const AssetIndexEntry* entry = nullptr;
    };

    Result<void> BuildRows() const noexcept;
    Result<void> AppendMatches(uint8 kind, StringView value,
                               Array<const AssetIndexEntry*>& entries) const noexcept;

    const AssetIndex* index_;
    Allocator* allocator_;
    mutable Array<TagRow> rows_;
    mutable bool built_ = false;
};

} // namespace rocks
