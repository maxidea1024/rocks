#pragma once

#include <Base/Guid.h>
#include <MaterialCompiler/MaterialColorCurveSnapshot.h>
#include <MaterialCompiler/MaterialGraphSnapshot.h>

// 번역 입력의 원본 공급 interface다(Docs/Design/MaterialSystem 19.4.2절, 6.12절 MS2e2). 함수
// 호출·레이어는 FindFunction, MaterialSample은 FindMaterial, ColorRamp는 FindColorCurve로 그 에셋의
// snapshot을 받는다. 함수
// 인스턴스는 구현이 기반 함수 snapshot으로 푼다(override 적용은 MS6). 시험은 메모리 provider로
// 한다.
//
// 사용 예:
//   class MemoryProvider final : public MaterialSourceProvider {
//       const MaterialGraphSnapshot* FindFunction(Guid asset) const noexcept override { ... }
//       const MaterialGraphSnapshot* FindMaterial(Guid asset) const noexcept override { ... }
//   };
//   input.sources = &provider;
//
// 수명: 돌려준 snapshot은 번역이 끝날 때까지 살아 있어야 한다. 스레드: 번역 하나가 한 스레드에서
// 부른다.
namespace rocks::material {

class MaterialSourceProvider {
public:
    virtual ~MaterialSourceProvider() = default;

    // 함수(또는 함수 인스턴스의 기반 함수) snapshot이다. 없으면 nullptr이다.
    virtual const MaterialGraphSnapshot* FindFunction(Guid asset) const noexcept = 0;

    // 머티리얼 snapshot이다. 없으면 nullptr이다.
    virtual const MaterialGraphSnapshot* FindMaterial(Guid asset) const noexcept = 0;

    // 색 커브 snapshot이다(6.13절 MS2f4 — ColorRamp). 없으면 nullptr이다(기본 — 커브를 공급하지
    // 않는 provider).
    virtual const MaterialColorCurveSnapshot* FindColorCurve(Guid asset) const noexcept {
        (void)asset;
        return nullptr;
    }
};

} // namespace rocks::material
