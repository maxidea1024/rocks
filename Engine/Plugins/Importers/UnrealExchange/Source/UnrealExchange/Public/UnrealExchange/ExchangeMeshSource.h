#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <Core/Containers/Span.h>
#include <Core/Text/Json.h>
#include <Core/Text/NamePool.h>
#include <MeshSource/MeshSource.h>
#include <UnrealExchange/ExchangeBundle.h>

// 교환 MeshSource 기록 적재다(Docs/Design/UnrealExchange 5.5절 1번): 기록 하나(5.4절 2번)를 MA1
// 저장 형태(MeshSourceData + 메모리 bulk)로 옮겨 MeshSource::Load로 만든다. 정적 메시
// 임포트(UX3b)와 스킨 메시 임포트(UX7b)가 LOD 기록마다 부른다.
//
// 사용 예(A2f 임포터 — blobs[i]는 asset.blobs[i]의 검사한 byte):
//   MeshSource mesh(allocator);
//   uint64 normalized = 0;
//   ExchangeMeshFailure failure;
//   if (auto loaded = LoadExchangeMeshSource(asset, blobs, lod.Find("prepared"), names, mesh,
//                                            normalized, failure);
//       !loaded) {
//       return context.Fail(failure.code, loaded.Error(), lodIndex);
//   }
namespace rocks {

// 적재 실패의 세부다.
struct ExchangeMeshFailure {
    // CorruptBundle(기록 형식·blob 참조·Name 값 배치), InvalidSource(호출 계약 위반 — blobs 수가
    // asset.blobs와 다름), MeshSource::Load의 진단 코드(CorruptPackage는 교환 쪽이라
    // CorruptBundle로 바꾼다). 할당·이름 pool 오류는 None이다.
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    // 진단 detail이다(LoadExchangeMeshSource는 0, 정적 메시 변환은 실패 위치 —
    // ExchangeStaticMesh.h).
    uint64 detail = 0;
    // MeshSource::Load의 세부 진단이다(Load 전 거부면 비어 있다).
    MeshDiagnostic mesh;
};

// record(MeshSource 기록 객체)로 out을 만든다(5.5절 1번):
// - MeshSourceData 요소 수는 counts, 위상 bulk는 위상 blob 일곱을 MeshAnimationAssets 9.1절 순서로
//   이어 붙인 것, 속성 레코드는 기록 순서이고 값 bulk는 blob byte 그대로다(배치가 같다 — 3.4.1절).
// - 기록의 속성은 원문 이름으로 9.2절 순서(요소, 이름 byte)의 엄격 증가여야 한다.
// - 속성 이름과 Name 값을 NFC로 바꾼다(MeshSource는 NFC만 받는다). 이름이 바뀌어 9.2절 순서가
//   달라지면 다시 정렬한다. normalized는 바뀐 이름(속성 이름 + Name 값) 수다(0이 아니면 호출자가
//   경고 UnrealNameNormalized를 낸다).
// - bulk는 메모리 bulk 원본(MemoryBulkSource)으로 주고 MeshSource::Load(data, bulk, names)를
//   부른다. 그래서 배치 판정과 ValidateMeshSource(본 수 모름)가 패키지 로드와 같다.
//
// 인자: blobs는 asset.blobs와 같은 순서·수의 byte다(VerifyExchangeBlob을 통과한 것 — 이 함수는
// 다시 검사하지 않는다). names는 출력이 쓸 Registry의 Name pool이다.
//
// 오류: blobs 수가 다름 InvalidArgument(InvalidSource), 기록 형식(객체·키·counts·위상 blob 수 7과
// 배치·원소 수·요소·값 타입 이름·채널·blob 참조가 asset.blobs에 없음·배치와 값 타입 불일치·속성
// 순서·Name 값 배치)
// CorruptData(CorruptBundle), 잘못된 UTF-8 이름 CorruptData(CorruptBundle), MeshSource::Load
// 실패는 그 오류와 진단, 이름 pool·할당 오류는 그 오류(진단 None). 실패하면 out은 그대로다.
ROCKS_UNREALEXCHANGE_API Result<void> LoadExchangeMeshSource(const ExchangeAsset& asset,
                                                             Span<const Span<const uint8>> blobs,
                                                             JsonValue record, NamePool& names,
                                                             MeshSource& out, uint64& normalized,
                                                             ExchangeMeshFailure& failure) noexcept;

} // namespace rocks
