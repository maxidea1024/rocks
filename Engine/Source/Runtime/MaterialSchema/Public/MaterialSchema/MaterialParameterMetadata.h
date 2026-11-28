#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Text/StringView.h>
#include <MaterialSchema/MaterialParameterType.h>

// 파라미터 노드 하나의 값·표시 정보다(Docs/Design/MaterialSystem 12.8절 MS3d1 — UE
// FMaterialParameterMetadata, RE/Public/Materials/MaterialParameters.h:449). 노드 정의의
// ParameterValue hook이 채운다. 문자열(name·group)은 hook에 준 reader 버퍼를 빌리므로 그 reader가
// 살아 있는 동안만 유효하다. 번역(lowering 기본값)과 쿠킹(파라미터 표)이 같은 hook을 쓴다.
//
// 사용 예:
//   MaterialParameterMetadata metadata;
//   if (auto read = definition.parameterValue(reader, metadata); read && read.Value()) {
//       Use(metadata.parameterId, metadata.defaults);
//   }
namespace rocks::material {

// 수 기본값 성분 수다(Vector·DoubleVector 넷).
inline constexpr usize MaterialParameterComponents = 4;

// custom primitive data를 쓰지 않음을 뜻하는 primitiveDataIndex 값이다(UE INDEX_NONE).
inline constexpr int32 NoPrimitiveDataIndex = -1;

struct MaterialParameterMetadata {
    MaterialParameterType type = MaterialParameterType::Scalar;
    Guid parameterId;
    StringView name;
    StringView group;
    int32 sortPriority = 0;
    // custom primitive data 번호다(UE PrimitiveDataIndex — 쓰지 않으면 NoPrimitiveDataIndex).
    int32 primitiveDataIndex = NoPrimitiveDataIndex;
    // 수 기본값이다(성분 수만큼 — float도 double로, static 값은 0·1).
    double defaults[MaterialParameterComponents] = {0.0, 0.0, 0.0, 0.0};
    // 에셋 기본값이다(텍스처·텍스처 컬렉션·font·RVT·SVT — 없으면 null).
    Guid texture;
    // 커브 atlas 행 파라미터의 커브·atlas다(UE bUsedAsAtlasPosition·ScalarCurve·ScalarAtlas —
    // 행 번호 기본값은 ResolveParameterDefaults가 atlas에서 계산한다).
    Guid atlasCurve;
    Guid atlas;
    bool usedAsAtlasPosition = false;
};

} // namespace rocks::material
