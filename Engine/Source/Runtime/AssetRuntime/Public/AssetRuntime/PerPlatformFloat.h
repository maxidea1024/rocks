#pragma once

#include <Base/Compiler.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// 대상 프로파일별 float 값이다(Docs/Design/MeshAnimationAssets 5.5.1절 2,
// Docs/Design/ReflectionExtensions 7.2절, UE FPerPlatformFloat — CoreUObject
// PerPlatformProperties.h). 에셋 설정 하나가 TargetProfile마다 다른 값을 가질 때 쓴다 (메시 LOD
// 화면 크기 등). 쿠킹은 대상 프로파일 값 하나로 줄여 쓴다(UE FPerPlatformProperty가 쿠킹 때 대상
// 플랫폼 값만 쓰는 것과 같다).
namespace rocks {

// perPlatform의 키는 TargetProfile id다(UE는 플랫폼 그룹 이름도 받지만 Rocks에는 플랫폼 그룹이
// 없다).
//
// 사용 예:
//   PerPlatformFloat screenSize(allocator);
//   screenSize.defaultValue = 1.0f;
//   const Name android = names.TryIntern("Android").Value();   // NamePool names
//   (void)screenSize.perPlatform.Insert(android, 0.5f);
//   const float value = screenSize.Resolve(android);           // 0.5
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct ROCKS_ASSETRUNTIME_API PerPlatformFloat {
    ROCKS_GENERATED_BODY()

    explicit PerPlatformFloat(Allocator& allocator) noexcept
#if ROCKS_WITH_EDITOR_DATA
        : perPlatform(allocator)
#endif
    {
        (void)allocator;
    }

    ROCKS_PROPERTY() float defaultValue = 0.0f;
#if ROCKS_WITH_EDITOR_DATA
    // 프로파일별 덮어쓰기다(EditorOnly — 쿠킹 패키지에는 defaultValue만 남는다, UE PerPlatform 맵과
    // 같다).
    ROCKS_PROPERTY(EditorOnly) OrderedMap<Name, float> perPlatform;
#endif

    // profile의 값이다(덮어쓰기 표에 없거나 None이면 기본값 — 에디터 데이터가 없는 빌드는 늘
    // 기본값이다: 쿠킹이 대상 프로파일 값을 defaultValue에 남긴다).
    float Resolve(Name profile) const noexcept;
};

} // namespace rocks
