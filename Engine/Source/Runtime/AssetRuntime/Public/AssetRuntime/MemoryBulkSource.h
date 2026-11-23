#pragma once

#include <AssetRuntime/PackageWriter.h>
#include <Core/Containers/Array.h>

namespace rocks {

// 저장 전 bulk raw byte를 메모리에 모아 두는 bulk 원본이다(Docs/Design/AssetSystem 5.5절,
// MeshAnimationAssets.ko.md 3.7절 2). 원본 데이터(메시 속성·텍스처 원본 등)를 에셋 필드의
// BulkDataRef로 만들 때 쓴다. Add가 byte를 넘겨받아
// 임시 bulkId(1부터 차례로)·크기·SHA-256을 담은 ref를 돌려주고, WritePackage가 그 ref로 ReadBulk를
// 불러 byte를 얻는다(writer가 크기·SHA-256을 다시 대조하고 패키지 안 번호로 다시 매긴다).
//
// 사용 예:
//   MemoryBulkSource bulk(allocator);
//   auto ref = bulk.Add(Move(bytes));               // asset.pixels = ref.Value();
//   PackageWriteOptions options = ...;
//   options.bulkSource = &bulk;
//   WritePackage(allocator, registry, access, package, options, sink);
//
// 수명: ReadBulk가 준 span은 이 객체가 살아 있고 Clear되지 않는 동안 유효하다(Add는 이미 준 span을
// 무효화하지 않는다 — entry마다 따로 할당한다). 이 객체가 준 ref는 이 객체에만 쓸 수 있다(다른
// source에서는 번호가 다른 byte를 가리킨다).
// 스레드: 동기화하지 않는다. 한 스레드(저장하는 쪽)가 쓴다.
class ROCKS_ASSETRUNTIME_API MemoryBulkSource final : public PackageBulkSource {
public:
    explicit MemoryBulkSource(Allocator& allocator) noexcept
        : allocator_(&allocator), entries_(allocator) {
    }

    MemoryBulkSource(const MemoryBulkSource&) = delete;
    MemoryBulkSource& operator=(const MemoryBulkSource&) = delete;

    // bytes를 넘겨받아 새 entry로 두고 그 ref(bulkId = 지금까지 entry 수 + 1)를 돌려준다. 0 byte도
    // entry다(빈 ref가 아니다).
    //
    // 오류: entry 수가 uint32 bulkId로 셀 수 있는 범위를 넘으면 CapacityExceeded, 할당 실패는
    // allocator 오류다. 실패하면 bytes는 호출자에게 남고(이동하지 않는다) 이 객체는 그대로다.
    Result<BulkDataRef> Add(Array<uint8>&& bytes) noexcept;

    // bytes를 복사해 새 entry로 둔다(Add(Array&&)와 같은 규칙).
    Result<BulkDataRef> AddCopy(Span<const uint8> bytes) noexcept;

    // ref의 bulkId에 해당하는 entry의 byte다. 크기·SHA-256 대조는 호출자(writer)가 한다.
    //
    // 오류: 빈 ref·없는 bulkId는 NotFound다.
    Result<Span<const uint8>> ReadBulk(const BulkDataRef& ref) noexcept override;

    // 지금 entry 수다.
    uint32 Count() const noexcept {
        return static_cast<uint32>(entries_.Size());
    }

    // 모든 entry를 버린다(준 ref·span이 모두 무효가 된다).
    void Clear() noexcept {
        entries_.Clear();
    }

private:
    Allocator* allocator_;
    Array<Array<uint8>> entries_;
};

} // namespace rocks
