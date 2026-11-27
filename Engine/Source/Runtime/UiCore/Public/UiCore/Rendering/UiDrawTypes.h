#pragma once

#include <Core/Math/Color.h>
#include <UiCore/Rendering/UiTexture.h>
#include <UiCore/Rendering/UiWindowSurface.h>

namespace rocks::ui {

class DrawBatch;

enum class UiSurfacePassKind : uint8 {
    // 보관 표면 내용(content)을 대상 렌더 타깃에 그린다.
    Retained,
    // 장면 packet들(firstPacket부터 packetCount개)의 기록 함수가 대상에 그린다. packet이 없으면
    // clear 색으로 지우기만 한다.
    Scene
};

// 그리기 대상 하나 앞에 먼저 기록하는 표면·장면 pass다(Docs/Design/UiToolkit 18.3절). 한 대상의
// pass는 안쪽 표면부터 쌓인 순서대로 기록한다.
struct UiSurfacePass {
    UiSurfacePassKind kind = UiSurfacePassKind::Retained;
    UiTextureId target{};
    uint32 width = 0;
    uint32 height = 0;
    LinearColor clear{0, 0, 0, 0};
    const DrawBatch* content = nullptr;
    usize firstPacket = 0;
    usize packetCount = 0;
};

// 그리기 대상 하나다. 창 표면(window)이나 오프스크린 렌더 타깃(renderTarget) 중 하나를 가리킨다.
// passes는 버퍼의 pass 목록에서 firstPass부터 passCount개다.
struct UiDrawTarget {
    UiWindowSurfaceId window{};
    UiTextureId renderTarget{};
    uint32 width = 0;
    uint32 height = 0;
    LinearColor clear{0, 0, 0, 1};
    const DrawBatch* batch = nullptr;
    usize firstPass = 0;
    usize passCount = 0;
};

} // namespace rocks::ui
