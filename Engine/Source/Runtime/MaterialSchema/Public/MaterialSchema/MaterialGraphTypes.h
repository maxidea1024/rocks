#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>
#include <NodeGraph/GraphDocument.h>

// 머티리얼 그래프 도메인의 이름·사유 코드·연결 규칙이다(Docs/Design/MaterialSystem 5.2·5.3·5.6절).
namespace rocks::material {

// 도메인 이름이다.
inline constexpr StringView MaterialDomainName = "Material";
inline constexpr StringView MaterialFunctionDomainName = "MaterialFunction";

// 출력 노드 종류 이름이다(5.6절).
inline constexpr StringView MaterialOutputNodeTypeName = "Material.Output";

// 연결 거부 사유 코드다(ConnectVerdict::reasonCode).
enum class MaterialConnectReason : uint32 {
    None = 0,
    TypeMismatch = 1,
    Cycle = 2,
    KindMismatch = 3,
    // 판정 입력이 비어 있다(그래프·핀 서술 없음).
    InvalidQuery = 4,
    // 순환 판정을 끝내지 못했다(할당 실패). 잇지 않는다.
    CycleCheckFailed = 5
};

// 두 핀 타입을 이을 수 있는가(순환 검사 제외). from은 출력 핀, to는 입력 핀 타입이다.
// UnresolvedPinType(wildcard)은 어느 쪽이든 받는다. 연결 때는 타입 군만 보고 폭·변환·static 오류는
// 번역의 V1·V7이 낸다(5.4절, UE CanConnectMaterialValueTypes와 같다).
ROCKS_MATERIALSCHEMA_API bool CanConnectMaterialPinTypes(nodegraph::PinTypeId from,
                                                         nodegraph::PinTypeId to) noexcept;

} // namespace rocks::material
