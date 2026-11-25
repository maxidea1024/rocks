#pragma once

#include <AssetRuntime/Package.h>

// 그리기에 쓸 수 있는 머티리얼 에셋의 공통 기반이다(Docs/Design/MaterialSystem 2절·4.2절, 단계
// MS3a). 지금은 골격만 있다: 메시 머티리얼 슬롯(ObjectRef<MaterialBase> —
// MeshAnimationAssets 4.3절)이 참조할 타입이 머티리얼 schema·컴파일러보다 먼저 필요해서다(사용자
// 결정 2026-11-23). 필드는 MS3b가 더한다.
namespace rocks {

// 추상 기반이다(UE UMaterialInterface). 메시 슬롯은 이 타입을 Hard로 참조한다(비면 엔진 기본
// 머티리얼).
//
// 사용 예(슬롯 필드):
//   ROCKS_PROPERTY() ObjectRef<MaterialBase> material;
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS(Abstract)
class MaterialBase : public Asset {
    ROCKS_GENERATED_BODY()
};

} // namespace rocks
