#pragma once

#include <Base/Compiler.h>
#include <CurveAssets/ColorCurveAsset.h>
#include <CurveAtlas/CurveAtlasConstants.h>
#include <Object/ObjectRefArray.h>
#include <Texture/Texture.h>

// 색 커브 atlas다(Docs/Design/CurveAssets 4장, UE UCurveLinearColorAtlas 대응).
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
namespace rocks {

// 색 커브 여러 개를 한 줄씩 구운 2D 텍스처다. 행 i는 curves[i]를 너비 W = max(width, 2) 칸에서
// 시간 x × (1 / (W − 1))로 표본한 색이다(null 커브는 흰색, 커브가 없으면 흰 한 줄). 원본(source —
// RGBA16F)은 편집 도구(CurveAtlasTools의 원본 재구성 hook)가 asset-set·asset-new·패키지 열기 때
// 굽고 저장한다. 쿠킹은 보통 Texture2D 경로다. 머티리얼은 atlas와 행 번호(RowOf)로 색을 읽는다
// (UE 식 V = (행 + 0.5) / 높이 — 머티리얼 노드 몫).
//
// 기본 텍스처 설정은 UE 생성자 값이다: mip 없음, Bilinear, sRGB 아님, 주소 Clamp·Clamp, HDR 압축.
//
// 사용 예:
//   const usize row = atlas->RowOf(curveHandle);
//   if (row != IndexNone) { /* V = (row + 0.5) / 높이 */ }
ROCKS_CLASS()
class ROCKS_CURVEATLAS_API ColorCurveAtlas final : public Texture2D {
    ROCKS_GENERATED_BODY()

public:
    explicit ColorCurveAtlas(Allocator& allocator) noexcept;

    // curve의 행 번호다(UE GetCurveIndex — 같은 원소가 여럿이면 첫 원소). null handle이나 목록에
    // 없는 커브는 IndexNone이다.
    usize RowOf(ObjectHandle curve) const noexcept;

    // 굽는 너비 max(width, MinColorCurveAtlasWidth)다(UE UpdateTextures).
    uint32 EffectiveWidth() const noexcept;

    // 너비다(UE TextureSize). 굽기와 정규화가 최소 2로 올린다.
    ROCKS_PROPERTY() uint32 width = DefaultColorCurveAtlasWidth;
    // 행마다 구울 커브다(UE GradientCurves — 런타임 필드, null 원소는 흰 행).
    ROCKS_PROPERTY() ObjectRefArray<ColorCurveAsset> curves;
#if ROCKS_WITH_EDITOR_DATA
    // 켜면 커브 조정 대신 원값으로 굽고, 텍스처 조정값을 항등값으로 지킨다(UE
    // bDisableAllAdjustments — 음수 값을 보존할 때).
    ROCKS_PROPERTY(EditorOnly) bool disableAdjustments = false;
#endif
};

} // namespace rocks
