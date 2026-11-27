#pragma once

#include <Core/Result.h>
#include <CurveAssets/CurveAssetConstants.h>
#include <CurveAssets/FloatCurveAsset.h>
#include <Curves/TangentCurve.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectRefArray.h>

// 커브 에셋 slot의 평탄화다(Docs/Design/CurveAssets 6.1절 — CA5, InputActions 2.9절 IA9). 값
// struct는 참조를 둘 수 없으므로 소유 에셋이 ObjectRefArray<FloatCurveAsset>를 갖고 값 struct는 그
// 번호(slot)를 갖는다. 사본을 만드는 곳이 이 도우미로 에셋 커브를 인라인 칸에 복사하고 slot을
// 비운다.
namespace rocks {

class ObjectRegistry;

// slot이 NoCurveAssetSlot이면 아무 일도 하지 않는다. 아니면 assets[slot]의 FloatCurveAsset::curve를
// curve에 깊이 복사하고 slot을 NoCurveAssetSlot으로 바꾼다. registry·access는 assets를 가진 소유
// 객체의 Registry와 접근 문맥이다. 오류: slot이 범위 밖이거나 그 원소가 null이면 InvalidArgument,
// 풀리지 않으면 Resolve의 오류(파괴 요청 Destroyed, 다른 스레드 WrongThread), 할당 실패
// OutOfMemory. 실패하면 slot·curve는 그대로다.
//
// 사용 예(모디파이어 사본 — 소유 InputAction의 curveAssets):
//   if (auto resolved = TryResolveCurveSlot(registry, access, action.curveAssets,
//                                           copy.responseXAsset, copy.responseX);
//       !resolved) {
//       return resolved;
//   }
ROCKS_CURVEASSETS_API Result<void>
TryResolveCurveSlot(ObjectRegistry& registry, const ObjectAccessContext& access,
                    const ObjectRefArray<FloatCurveAsset>& assets, uint32& slot,
                    TangentCurve& curve) noexcept;

} // namespace rocks
