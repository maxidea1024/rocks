#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Span.h>

// 번역 입력의 색 커브 atlas snapshot이다(Docs/Design/MaterialSystem 19.4.3절, 6.10절 MS2c5 —
// CurveAtlasRowParameter). atlas 객체(ColorCurveAtlas) 대신 행마다 커브 AssetId만 본다(행 번호를
// 찾는 데만 쓴다 — 커브 값은 atlas 텍스처가 굽는다). 쿠킹·에디터가 에셋에서 채운다.
//
// 사용 예:
//   const Guid rows[] = {first.guid, Guid{}, second.guid};
//   MaterialCurveAtlasSnapshot snapshot{rows};
//
// 수명: 가리키는 목록은 번역이 끝날 때까지 살아 있고 바뀌지 않아야 한다.
namespace rocks::material {

struct MaterialCurveAtlasSnapshot {
    // 행마다 커브 AssetId다(ColorCurveAtlas::curves 순서, null 원소는 Guid{}).
    Span<const Guid> curves;
};

} // namespace rocks::material
