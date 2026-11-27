#pragma once

#include <Base/Compiler.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// 대상 프로파일별 bool 값이다(Docs/Design/MeshAnimationAssets 6.7.1절 1, UE FPerPlatformBool —
// CoreUObject PerPlatformProperties.h). 규칙은 PerPlatformFloat과 같다(스킨 가중치 기본 프로필 등).
namespace rocks {

// perPlatform의 키는 TargetProfile id다.
//
// 사용 예:
//   PerPlatformBool profile(allocator);
//   (void)profile.perPlatform.Insert(names.TryIntern("Android").Value(), true);
//   const bool value = profile.Resolve(names.TryIntern("Windows").Value());   // false(기본값)
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct ROCKS_ASSETRUNTIME_API PerPlatformBool {
    ROCKS_GENERATED_BODY()

    explicit PerPlatformBool(Allocator& allocator) noexcept
#if ROCKS_WITH_EDITOR_DATA
        : perPlatform(allocator)
#endif
    {
        (void)allocator;
    }

    ROCKS_PROPERTY() bool defaultValue = false;
#if ROCKS_WITH_EDITOR_DATA
    // 프로파일별 덮어쓰기다(EditorOnly — 쿠킹 패키지에는 defaultValue만 남는다, UE PerPlatform 맵과
    // 같다).
    ROCKS_PROPERTY(EditorOnly) OrderedMap<Name, bool> perPlatform;
#endif

    // profile의 값이다(덮어쓰기 표에 없거나 None이면 기본값 — 에디터 데이터가 없는 빌드는 늘
    // 기본값이다: 쿠킹이 대상 프로파일 값을 defaultValue에 남긴다).
    bool Resolve(Name profile) const noexcept;
};

} // namespace rocks
