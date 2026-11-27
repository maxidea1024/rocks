#pragma once

#include <RenderBase/RenderHandle.h>
#include <UiCore/DrawBatch.h>
#include <UiCore/Rendering/UiDrawTypes.h>
#include <UiCore/Rendering/UiSceneRecord.h>

namespace rocks::ui {

// 명령 하나가 읽는 텍스처다. 게임 lane이 복사할 때 UiTextureId를 렌더 handle로 바꾼 값이다.
struct UiFrameTexture {
    render::RenderHandle handle{};
    UiTextureSampling sampling = UiTextureSampling::Image;
};

// 프레임 arena에 복사한 그리기 묶음 하나다(UiCore DrawBatch의 복사본). textures는 commands와 같은
// 길이이고 resource가 없는 명령은 빈 handle이다.
struct UiFrameBatch {
    Span<const DrawVertex> vertices;
    Span<const uint32> indices;
    Span<const DrawCommand> commands;
    Span<const UiFrameTexture> textures;
};

// 대상 앞의 표면·장면 pass 하나다(UiSurfacePass의 복사본). scenes는 프레임 arena로 옮긴 장면
// packet이다.
struct UiFramePass {
    UiSurfacePassKind kind = UiSurfacePassKind::Retained;
    render::RenderHandle target{};
    uint32 width = 0;
    uint32 height = 0;
    LinearColor clear{0, 0, 0, 0};
    UiFrameBatch content;
    Span<UiSceneRecord> scenes;
};

// 그리기 대상 하나다. window가 비었으면 renderTarget(오프스크린)에 그린다.
struct UiFrameTarget {
    UiWindowSurfaceId window{};
    render::RenderHandle renderTarget{};
    uint32 width = 0;
    uint32 height = 0;
    LinearColor clear{0, 0, 0, 1};
    UiFrameBatch batch;
    Span<UiFramePass> passes;
};

} // namespace rocks::ui
