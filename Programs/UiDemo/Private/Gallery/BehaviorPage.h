#pragma once

#include "BindingSample.h"
#include "GalleryGrid.h"

#include <Core/Memory/UniquePtr.h>

// 공통 기능 탭이다(Docs/Design/UiToolkit 16.9.2절). 모든 Element가 가진 기능을 한 번씩 보인다: 렌더
// 변환(회전·배율·기울기, 변환 뒤에도 누를 수 있음), 불투명도, 자르기, 가시성 5종, 전경색 상속, 커서
// 11종, 속성 바인딩. 초점 이동·접근성 트리·타이머는 Application을 트리에서 찾는 단계(UW4a2)에서
// 더한다.
namespace rocks::ui::gallery {

struct BehaviorPage {
    SharedPtr<Element> root;
    UniquePtr<GalleryGrid> grid;
    // 바인딩 견본의 모델이다(슬라이더가 쓰고, 상자 색·진행 막대가 읽는다).
    SharedPtr<BindingSample> model;
};

Result<BehaviorPage> BuildBehaviorPage(GalleryContext& context) noexcept;

} // namespace rocks::ui::gallery
