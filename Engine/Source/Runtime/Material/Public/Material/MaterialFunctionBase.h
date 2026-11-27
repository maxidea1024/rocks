#pragma once

#include <AssetRuntime/Package.h>

// 머티리얼 함수 에셋의 공통 기반이다(Docs/Design/MaterialSystem 4.6.1절, UE
// UMaterialFunctionInterface). 레이어·혼합 함수와 함수 인스턴스 부모는 이 타입으로 가리켜서 함수와
// 함수 인스턴스를 모두 받는다.
namespace rocks {

// 함수 참조는 EditorOnly ObjectRef<MaterialFunctionBase>다(쿠킹 런타임에 함수는 남지 않는다 — Build
// 의존은 RegisterMaterialAssetHooks의 hook이 선언한다).
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_MATERIAL_API MaterialFunctionBase : public Asset {
    ROCKS_GENERATED_BODY()
};

} // namespace rocks
