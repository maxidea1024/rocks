#pragma once

#include <AssetDatabase/AssetImporter.h>
#include <AssetRuntime/MemoryBulkSource.h>

// 임포트 서비스의 bulk 옮기기다(Docs/Design/MeshAnimationAssets 5.5.3절·6.7.3절·7.6.2절 — 메시·
// 클립 임포트가 원본을 메모리 bulk에 쓴 뒤 출력 패키지 bulk로 넘긴다).
namespace rocks {

// bulk의 ref가 가리키는 byte를 복사해 context.AddBulk(output, …)로 넘기고 ref를 출력 bulk ref로
// 바꾼다. 복사 배열은 allocator로 만든다.
//
// 사용 예(원본을 메모리 bulk에 쓴 뒤):
//   MemoryBulkSource bulk(allocator);
//   (void)mesh.Store(bulk, names, data);
//   if (auto moved = TransferImportBulk(context, output, bulk, data.topology, allocator); !moved)
//   { ... }
//
// 오류: bulk를 못 읽음(ReadBulk의 오류), 할당 실패, AddBulk의 오류. 실패하면 ref는 그대로다.
ROCKS_ASSETDATABASE_API Result<void> TransferImportBulk(AssetImportContext& context,
                                                        ObjectHandle output, MemoryBulkSource& bulk,
                                                        BulkDataRef& ref,
                                                        Allocator& allocator) noexcept;

} // namespace rocks
