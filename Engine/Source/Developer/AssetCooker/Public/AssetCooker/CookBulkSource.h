#pragma once

#include <AssetRuntime/PackageWriter.h>
#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>

// 쿠킹 처리기의 연쇄 bulk 원본이다(Docs/Design/AssetSystem 12.14절 9번,
// Docs/Design/MeshAnimationAssets 5.5.2절 4). 처리기가 새로 만든 bulk(텍스처 mip·꼬리, 메시 LOD
// 덩어리)를 앞에 두고, 그 밖 ref는 에디터 패키지 bulk 원본으로 보낸다. 텍스처·메시 처리기가 함께
// 쓴다.
namespace rocks {

// 쿠킹이 새로 만든 bulk의 첫 id다(uint32 최댓값부터 내려간다 — 에디터 저장소의 저장하지 않은 bulk는
// 최상위 bit id를 1부터 올려 쓰므로 겹치지 않는다).
inline constexpr uint32 CookBulkIdFirst = NumericLimits<uint32>::Max();

// 사용 예(처리기 Cook — 쓰기 동안 byte를 살려 둔다):
//   CookChainedBulkSource bulk(allocator, context.bulkSource);
//   auto ref = bulk.Add(lodBytes.View());   // renderData.lods[i].data = ref.Value();
//   auto streamed = bulk.Add(payload.View(), PackageBulkPlacement::Sidecar);  // 스트리밍 bulk
//   CookContext write = context;
//   write.bulkSource = &bulk;
//
// 수명: Add가 받은 byte는 빌림이다(쓰기가 끝날 때까지 산다). next는 이 객체보다 오래 산다.
// 스레드: 한 스레드(쿠킹 처리기)가 쓴다.
class ROCKS_ASSETCOOKER_API CookChainedBulkSource final : public PackageBulkSource {
public:
    CookChainedBulkSource(Allocator& allocator, PackageBulkSource* next) noexcept
        : entries_(allocator), next_(next) {
    }

    // bytes를 새 entry로 두고 그 ref(id는 CookBulkIdFirst부터 내려감, 크기·SHA-256)를 돌려준다. 0
    // byte도 entry다. placement는 쓰기의 배치다(AssetSystem 20.5 1 — Sidecar면 sidecar 파일,
    // sidecar 출력이 없는 쿠킹·0 byte는 패키지 안). 오류: id가 저장하지 않은 bulk 범위로 내려옴
    // CapacityExceeded, 할당 실패.
    Result<BulkDataRef>
    Add(Span<const uint8> bytes,
        PackageBulkPlacement placement = PackageBulkPlacement::Package) noexcept;

    // 새 bulk는 이 객체가, 그 밖은 next가 준다(next가 없으면 NotFound).
    Result<Span<const uint8>> ReadBulk(const BulkDataRef& ref) noexcept override;

    // 새 bulk는 저장 byte가 없다(writer가 raw를 부호화한다 — NotFound). 그 밖은 next다.
    Result<PackageStoredBulk> ReadStoredBulk(const BulkDataRef& ref) noexcept override;

    // 새 bulk는 Add가 받은 배치, 그 밖은 next(없으면 Package)다.
    PackageBulkPlacement Placement(const BulkDataRef& ref) const noexcept override;

private:
    struct Entry {
        Span<const uint8> bytes;
        PackageBulkPlacement placement = PackageBulkPlacement::Package;
    };

    const Entry* Find(uint32 bulkId) const noexcept;

    Array<Entry> entries_;
    PackageBulkSource* next_;
};

} // namespace rocks
