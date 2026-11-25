#pragma once

#include <Core/Result.h>
#include <UiCore/ResolvedVectorImage.h>
#include <UiCore/VectorImageKey.h>

namespace rocks::ui {

// 벡터 이미지 래스터 화소 크기의 한 변 상한이다. PaintList는 이 값으로 자르고, resolver는 넘으면
// InvalidArgument다. OpenGL ES 3.0이 보장하는 최소 텍스처 크기(GL_MAX_TEXTURE_SIZE 2048)에 맞춘다 —
// 큰 그림은 이 크기의 단독 텍스처가 된다.
inline constexpr uint32 MaxVectorImagePixels = 2048;

// 벡터 brush를 그리기 목록에 기록할 때 화소 크기의 래스터 자리로 바꾸는 쪽이다(UE
// FSlateVectorGraphicsCache — Docs/Design/UiToolkit 17.2절). UiRender VectorImageAtlas가 구현한다.
// PaintList가 들고(Application이 넣는다) 그리는 스레드에서 부른다.
class ROCKS_UICORE_API VectorImageResolver {
public:
    virtual ~VectorImageResolver() noexcept = default;

    // key 그림을 width × height 화소로 래스터화한 자리다. 같은 키·크기는 같은 자리를 돌려준다.
    // 모르는 키는 NotFound, 크기 0은 InvalidArgument, 래스터·atlas 실패는 그 오류다.
    virtual Result<ResolvedVectorImage> Resolve(VectorImageKey key, uint32 width,
                                                uint32 height) noexcept = 0;

    // 돌려준 자리가 무효가 될 때(atlas를 비울 때) 올라가는 번호다. 바뀌면 기록해 둔 그리기 목록을
    // 다시 만들어야 한다(Application이 cache root를 다시 만든다).
    virtual uint64 Revision() const noexcept = 0;
};

} // namespace rocks::ui
