#pragma once

#include "GalleryWindow.h"

// 갤러리 입력 self-test다(Docs/Design/UiToolkit 16.8절). 요소의 실제 배치 위치를 읽어 누른다:
// 탭 전환, 활성 체크 상자로 행 비활성(비활성 brush·알파), 단순 버튼 올림 전경색, 배치 탭 도킹
// 영역의 탭 끌어 놓기. 성공이면 0, 실패면 단계 번호(40번대)다.
namespace rocks::ui::gallery {

int VerifyGallery(Allocator& allocator, GalleryWindow& gallery,
                  const SharedPtr<DockManager>& area) noexcept;

} // namespace rocks::ui::gallery
