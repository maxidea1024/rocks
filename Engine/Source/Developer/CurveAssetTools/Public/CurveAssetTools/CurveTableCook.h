#pragma once

#include <AssetCooker/CookProcessor.h>

// 커브 표 쿠킹 처리기다(Docs/Design/CurveAssets 5.3절 — CA4, UE UCurveTable::Serialize의 쿠킹 중복
// 키 제거 — CurveTable.cpp:215-232). 쓰기용 사본에서:
// 1. 모드가 쓰지 않는 맵을 비운다(비어 있지 않았으면 경고 CurveTableUnusedRows, setting = 버린 행
// 수).
// 2. 행마다 중복 키를 허용오차 0으로 지운다(키 3개 이상일 때만 — UE RemoveRedundantKeys의 조기
// 반환).
//    Tangent 행은 이어서 자동 접선을 다시 계산한다(UE RemoveRedundantKeysInternal 끝의
//    AutoSetTangents — Rocks TryRemoveRedundantKeys는 부르지 않는다, CurveData D10).
// 에디터 객체는 바꾸지 않는다: 사본을 이동으로 넣어 쓰고, 성공·실패 모두 원래 맵을
// 되돌린다(이동이라 되돌리기가 할당하지 않는다). 너겟은 쓰지 않는다(빌드 비용이 작다). 동작이
// 바뀌면 버전 Guid를 바꾼다.
//
// 사용 예(호스트 — 명령 동안 처리기를 쥔다):
//   CurveTableCookProcessor curveTableCook;
//   (void)RegisterCurveTableCookProcessor(&curveTableCook, table);
namespace rocks {

inline constexpr NuggetProcessorId CurveTableCookProcessorId{
    Guid{0x524f434b43555243ull, 0x0000000000000001ull},
    Guid{0x524f434b43555256ull, 0x0000000000000001ull}};

// TypeIdOf<CurveTableAsset>()에 등록한다. 한 스레드에서 쓴다(쿠킹 실행기와 같다). Cook 오류:
// CurveTableAsset이 아님 TypeMismatch, 대표 에셋 없음·풀기 오류 그 오류, 쓰기 오류, 할당 실패
// OutOfMemory(진단 None).
class ROCKS_CURVEASSETTOOLS_API CurveTableCookProcessor final : public CookProcessor {
public:
    NuggetProcessorId Identity() const noexcept override {
        return CurveTableCookProcessorId;
    }

    Result<void> Cook(CookContext& context) const noexcept override;
};

// 쿠킹 처리기 등록이다(state = CurveTableCookProcessor*, 명령 동안 산다). 오류: null state
// InvalidArgument, CookProcessorTable::Register의 오류.
ROCKS_CURVEASSETTOOLS_API Result<void>
RegisterCurveTableCookProcessor(void* state, CookProcessorTable& table) noexcept;

} // namespace rocks
