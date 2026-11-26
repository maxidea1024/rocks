#pragma once

#include <AnimationTools/AnimationClipCook.h>
#include <AssetCooker/CookProcessor.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <AudioTools/SoundClipCook.h>
#include <Core/Containers/Array.h>
#include <MeshTools/SkeletalMeshCook.h>
#include <MeshTools/StaticMeshCook.h>
#include <TextureTools/TextureCook.h>

// 제품 명령 실행 파일의 에셋 처리기·타입 hook 묶음이다(Docs/Design/MeshAnimationAssets 13.1절 1).
// AssetCommandContext의 등록 칸은 함수 하나 + state라 제품이 가진 처리기·hook을 이 묶음 하나로
// 건다.
//
// 사용 예(CommandletHost):
//   ProductCookProcessors cook;
//   assetContext.cookProcessors = {&RegisterProductCookProcessors, &cook};
//   assetContext.typeHooks = {&RegisterProductTypeHooks, nullptr};
namespace rocks {

// 제품 쿠킹 처리기다(명령 동안 산다).
struct ProductCookProcessors {
    TextureCookProcessor texture;
    StaticMeshCookProcessor staticMesh;
    SkeletalMeshCookProcessor skeletalMesh;
    AnimationClipCookProcessor animationClip;
    SoundClipCookProcessor soundClip;
};

// state(ProductCookProcessors*)의 처리기 다섯을 table에 차례로 등록한다. 오류: null state
// InvalidArgument, 처음 실패한 등록의 오류.
Result<void> RegisterProductCookProcessors(void* state, CookProcessorTable& table) noexcept;

// 텍스처 hook(설정 정규화·원본 재구성)과 애니메이션 hook(Build 의존)을 table에 등록한다(state는
// 쓰지 않는다). 오류: 처음 실패한 등록의 오류.
Result<void> RegisterProductTypeHooks(void* state, AssetTypeHookTable& table,
                                      Array<AssetTypeHookRegistration>& tokens) noexcept;

} // namespace rocks
