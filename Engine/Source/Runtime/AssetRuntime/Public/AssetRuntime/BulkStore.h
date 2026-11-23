#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <AssetRuntime/BulkData.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>

// 패키지 bulk 저장소 추상이다(Docs/Design/AssetSystem 6.1절, 7.1절 15 — A2c1). 런타임
// 패키지는 로드 때 bulk를 모두 풀어 Package가 갖고(저장소 없음), 에디터 패키지는 저장소
// (FilePackageBulkStore)가 지연 읽기·저장하지 않은 bulk·저장 뒤 다시 잇기를 맡는다(UE
// FEditorBulkData 대응).
//
// bulkId는 패키지 저장소의 지역 핸들이다: 객체의 BulkDataRef는 저장해도 바뀌지 않고(저장소가
// "메모리 id → entry" 표를 다시 잇는다), 내용 비교는 BulkDataRef::SameContent로 한다. 패키지·
// 세션 사이에서 bulkId를 비교하지 않는다.
namespace rocks {

// 저장하지 않은 bulk의 id 표지다(최상위 bit — 메모리에만 있다, 파일 id는 1–0x7FFFFFFF). 패키지
// 읽기는 이 bit가 선 BulkDataRef를 CorruptPackage로 거부한다.
inline constexpr uint32 UnsavedBulkIdBit = 0x80000000u;

// Package의 bulk 접근 추상이다.
class PackageBulkStore {
public:
    virtual ~PackageBulkStore() = default;

    // ref의 raw byte다. span은 이 저장소가 살아 있는 동안 유효하다(다시 잇기·Detach도 옮기지
    // 않는다). 오류: 빈 ref InvalidArgument, 없는 id NotFound, rawBytes·sha256이 entry와 다르면
    // TypeMismatch, 지연 읽기에서 발견한 손상은 CorruptPackage / InvalidArgument(detail = 파일
    // bulkId), 파일이 로드 뒤 바뀌었으면(헤더·섹션 표 지문이 다름) ExternalModification /
    // StaleHandle, 파일 IO 오류는 진단 None과 그 오류, 할당 실패. 지문을 바꾸지 않는 제자리
    // 덮어쓰기(Bulk 내용만)는 entry CRC가 CorruptPackage로 잡는다. 캐시된 핸들이 남아 있는 동안
    // 이름 교체로 바뀐 파일은 이전 내용을 계속 읽는다(그 내용은 로드 지문과 맞다).
    virtual Result<Span<const uint8>> Read(const BulkDataRef& ref,
                                           AssetDiagnostic* failure = nullptr) noexcept = 0;
};

} // namespace rocks
