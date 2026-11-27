#pragma once

#include <UiCore/Rendering/UiDrawBuffer.h>

namespace rocks::ui {

// 장면 source가 이번 프레임의 장면 pass에 packet을 게시하는 통로다(UE FSceneViewport::Draw가 넣는
// render command 대응, Docs/Design/UiToolkit 18.3절). UiDrawBuffer::AddScenePass가 만들고, 다른
// pass를 쌓기 전까지만 쓸 수 있다. 게시한 packet은 Render lane에서 게시 순서대로 기록된다.
//
// 사용 예(ViewportSource::Extract 안, 게임 lane):
//   struct ScenePacket { Mat4 viewProjection; Vec3 color; };   // 게임 주소를 담지 않는다
//   static void RecordScene(UiScenePassContext& context, ScenePacket& packet) noexcept;
//   return extract.Post(ScenePacket{camera.view * camera.projection, color}, &RecordScene);
class ROCKS_UICORE_API UiSceneExtract {
public:
    UiSceneExtract(UiDrawBuffer& buffer, usize pass) noexcept : buffer_(&buffer), pass_(pass) {
    }

    // packet을 게시한다. 실패: 이미 다른 pass를 쌓음 InvalidArgument, 할당 실패는 그 오류.
    template <UiSceneTransferable T>
    Result<void> Post(T&& packet, UiScenePassFunction<T> record) noexcept {
        auto made = UiScenePacket::TryMake(buffer_->GetAllocator(), Move(packet), record);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        return buffer_->PostPacket(pass_, Move(made.Value()));
    }

private:
    UiDrawBuffer* buffer_;
    usize pass_;
};

} // namespace rocks::ui
