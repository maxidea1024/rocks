#pragma once

#include <Material/MaterialBase.h>

// 그래프를 가진 머티리얼 에셋이다(Docs/Design/MaterialSystem 2절·4.2절, 단계 MS3a). 지금은 골격만
// 있다. 그래프·기본 속성·사용 대상 필드는 MS3b가 더한다.
namespace rocks {

// UE UMaterial 대응이다.
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class Material final : public MaterialBase {
    ROCKS_GENERATED_BODY()
};

} // namespace rocks
