#pragma once

#include <UiFramework/Docking/DockManager.h>
#include <UiRender/DrawBatchRenderer.h>

namespace rocks::ui {

class CommandList;
int VerifyWorkspaceInput(Allocator& allocator, const SharedPtr<DockManager>& root,
                         const SharedPtr<CommandList>& commands) noexcept;

Result<void> CaptureWorkspace(Allocator& allocator, graphics::Device& device,
                              graphics::PipelineHandle pipeline, SlotArray<BrushTexture>& textures,
                              const SharedPtr<DockManager>& root, uint32 width,
                              uint32 height) noexcept;

} // namespace rocks::ui
