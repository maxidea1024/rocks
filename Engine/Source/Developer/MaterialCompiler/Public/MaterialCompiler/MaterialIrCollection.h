#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>

// 머티리얼 IR 모듈이 참조하는 파라미터 컬렉션 하나다(Docs/Design/MaterialSystem 6.10절 MS2c2 —
// IrModule::Collections()가 asset id 순으로 담는다, 생성 원문 MaterialCollection<k>).
//
// 사용 예:
//   for (const IrCollection& collection : module.Collections()) {
//       Bind(collection.asset, collection.vectors);
//   }
namespace rocks::material {

// 모듈이 참조하는 파라미터 컬렉션 하나다(6.10절 MS2c2).
struct IrCollection {
    Guid asset;
    // 컬렉션 layout의 float4 수다(9.7절).
    uint32 vectors = 0;
};

} // namespace rocks::material
