#pragma once

#include <UiCore/DrawBatch.h>
#include <UiCore/Rendering/UiRenderer.h>

namespace rocks::ui {

// 렌더 타깃 내용을 RGBA(위에서 아래 순서, 4 byte/픽셀)로 읽는다(읽기 요청 → lane 표지 대기 →
// 결과 — Docs/Design/UiToolkit 18.5절). 그 타깃을 그린 DrawWindows 뒤에 부른다.
Result<void> CaptureTarget(UiRenderer& renderer, UiTextureId target, Array<uint8>& rgba) noexcept;

// buffer에 쌓인 표면 pass를 닫아 batch를 target(오프스크린)에 그린 뒤 RGBA로 읽는다. buffer는
// 이 호출 안에서 그려지고 비워진다.
Result<void> DrawAndCapture(UiRenderer& renderer, UiDrawBuffer& buffer, UiTextureId target,
                            uint32 width, uint32 height, LinearColor clear, const DrawBatch& batch,
                            Array<uint8>& rgba) noexcept;

} // namespace rocks::ui
