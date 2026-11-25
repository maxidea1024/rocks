#pragma once

#include <Core/Containers/OrderedMap.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// 대상 프로파일별 int32 값이다(Docs/Design/MeshAnimationAssets 5.5.1절 2,
// Docs/Design/ReflectionExtensions 7.2절, UE FPerPlatformInt — CoreUObject
// PerPlatformProperties.h). 규칙은 PerPlatformFloat과 같다(최소 LOD·스트리밍 LOD 수 등).
namespace rocks {

// perPlatform의 키는 TargetProfile id다.
//
// 사용 예:
//   PerPlatformInt32 minLod(allocator);
//   (void)minLod.perPlatform.Insert(names.TryIntern("Android").Value(), 1);
//   const int32 value = minLod.Resolve(names.TryIntern("Windows").Value());   // 0(기본값)
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct ROCKS_ASSETRUNTIME_API PerPlatformInt32 {
    ROCKS_GENERATED_BODY()

    explicit PerPlatformInt32(Allocator& allocator) noexcept : perPlatform(allocator) {
    }

    ROCKS_PROPERTY() int32 defaultValue = 0;
    // 프로파일별 덮어쓰기다(EditorOnly — 쿠킹 패키지에는 defaultValue만 남는다, UE PerPlatform 맵과
    // 같다).
    ROCKS_PROPERTY(EditorOnly) OrderedMap<Name, int32> perPlatform;

    // profile의 값이다(덮어쓰기 표에 없거나 None이면 기본값).
    int32 Resolve(Name profile) const noexcept;
};

} // namespace rocks
