#pragma once

#include <Base/Compiler.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Vector3.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>

// 메시 소켓이다(Docs/Design/MeshAnimationAssets 4.5절·5.5.1절 4, UE UStaticMeshSocket —
// StaticMeshSocket.h). UE는 UObject지만 소켓마다 하위 객체 정체성이 필요한 기능이 없어 값 struct
// 배열로 둔다(결정 MA-D6).
namespace rocks {

// 메시 공간 상대 변환을 가진 이름 붙은 부착점이다. 좌표·단위는 UE와 같다(cm, Rotator는 도).
// 미리보기 메시(UE PreviewStaticMesh)는 에디터 UI 범위라 넣지 않는다.
//
// 사용 예:
//   MeshSocket socket(allocator);
//   socket.name = names.TryIntern("Muzzle").Value();
//   socket.relativeLocation = Vec3d(50.0, 0.0, 10.0);
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct MeshSocket {
    ROCKS_GENERATED_BODY()

    explicit MeshSocket(Allocator& allocator) noexcept : tag(allocator) {
    }

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() Vec3d relativeLocation {};
    ROCKS_PROPERTY() Rotator<double> relativeRotation {};
    ROCKS_PROPERTY() Vec3d relativeScale = Vec3d(1.0, 1.0, 1.0);
    ROCKS_PROPERTY() String tag;
#if ROCKS_WITH_EDITOR_DATA
    // 임포트가 만든 소켓인지다(UE bSocketCreatedAtImport — 재임포트는 임포트 소켓만 바꾼다).
    ROCKS_PROPERTY(EditorOnly) bool createdAtImport = false;
#endif
};

} // namespace rocks
