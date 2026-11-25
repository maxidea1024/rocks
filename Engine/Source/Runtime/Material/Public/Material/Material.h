#pragma once

#include <AssetRuntime/Package.h>
#include <Object/ModuleRegistration.h>

// 머티리얼 에셋 클래스다(Docs/Design/MaterialSystem 2절·4.2절, 단계 MS3a). 지금은 골격만
// 있다: 메시 머티리얼 슬롯(ObjectRef<MaterialBase> — MeshAnimationAssets 4.3절)이 참조할 타입이
// 머티리얼 schema·컴파일러보다 먼저 필요해서다(사용자 결정 2026-11-23). 그래프·기본 속성·사용
// 플래그 같은 4.2절 필드와 MaterialInstance·MaterialFunction·ParameterCollection은 MS3·MS6이
// 더한다.
namespace rocks {

// 그리기에 쓸 수 있는 머티리얼 에셋의 공통 기반이다(추상, UE UMaterialInterface). 메시 슬롯은 이
// 타입을 Hard로 참조한다(비면 엔진 기본 머티리얼).
//
// 사용 예(슬롯 필드):
//   ROCKS_PROPERTY() ObjectRef<MaterialBase> material;
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS(Abstract)
class MaterialBase : public Asset {
    ROCKS_GENERATED_BODY()
};

// 그래프를 가진 머티리얼 에셋이다(UE UMaterial). 그래프·기본 속성은 MS3에서 더한다.
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class Material final : public MaterialBase {
    ROCKS_GENERATED_BODY()
};

// Material 리플렉션 타입(MaterialBase·Material)을 등록한다(생성 집계 함수
// generated::Material::RegisterModuleTypes를 부른다 — 의존 모듈 타입도 함께). 도구·시험은 직접,
// 엔진은 머티리얼 에셋 모듈이 부른다. 오류: 등록 오류(이미 등록·할당 실패 등).
ROCKS_MATERIAL_API Result<void> RegisterMaterialTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
