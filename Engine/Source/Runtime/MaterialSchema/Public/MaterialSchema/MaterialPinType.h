#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>
#include <NodeGraph/GraphSchema.h>

// 머티리얼 그래프의 핀 타입 표다(Docs/Design/MaterialSystem 5.3절). 값은 NodeGraph PinTypeId(도메인
// 핀 타입 표의 색인)다. 핀은 문서에 저장하지 않으므로(노드 그래프 GD3) 값은 저장 형식이 아니지만,
// 진단·dump가 이름으로 보이므로 순서를 바꾸지 않고 끝에만 더한다.
//
// 연결 규칙(MaterialGraphSchema::CanConnect)은 타입 군으로 판정한다: 수 군(Numeric·Float1–4·
// Int1–4·UInt1–4·Bool·Double1–4·행렬)끼리는 잇고 폭·변환 오류는 번역의 V1이 낸다(5.3절 암묵
// 변환). 자원·표면·속성 군은 같은 타입끼리만 잇는다. TextureAny는 텍스처 타입 전부를 받는다. Any
// 핀은 무엇과도 잇고 타입은 번역이 연결된 값으로 정한다.
//
// 사용 예:
//   const nodegraph::PinTypeTable& table = MaterialPinTypes();
//   StringView name = MaterialPinTypeName(MaterialPinType::Float3); // "Float3"
namespace rocks::material {

enum class MaterialPinType : uint32 {
    // 폭을 번역 때 정하는 수 값(UE MCT_Float·MCT_Numeric 대응). 수학 노드 입력 대부분이다.
    Numeric = 0,
    Float1 = 1,
    Float2 = 2,
    Float3 = 3,
    Float4 = 4,
    Int1 = 5,
    Int2 = 6,
    Int3 = 7,
    Int4 = 8,
    UInt1 = 9,
    UInt2 = 10,
    UInt3 = 11,
    UInt4 = 12,
    Bool = 13,
    Float3x3 = 14,
    Float4x4 = 15,
    // 큰 좌표 값(UE LWC, 셰이더 안에서는 타일 + 오프셋 — 5.3절). 절대 world 위치는 Double3(cm)다.
    Double1 = 16,
    Double2 = 17,
    Double3 = 18,
    Double4 = 19,
    Texture2D = 20,
    Texture2DArray = 21,
    TextureCube = 22,
    TextureCubeArray = 23,
    Texture3D = 24,
    // 위 다섯 텍스처 타입을 받는 입력(UE MCT_Texture). 출력 핀에는 쓰지 않는다.
    TextureAny = 25,
    // 아래 여섯은 자원 종류가 아직 없다(19.4.1절 MPQ). 핀은 정의를 위해 지금 둔다.
    TextureExternal = 26,
    TextureVirtual = 27,
    SparseVolumeTexture = 28,
    TextureCollection = 29,
    TextureMeshPaint = 30,
    TextureMaterialCache = 31,
    // 번역 때 정해지는 값(5.3절).
    StaticBool = 32,
    StaticInt = 33,
    // BSDF 트리(10절, UE MCT_Substrate).
    Surface = 34,
    // 속성 집합(UE MCT_MaterialAttributes).
    MaterialAttributes = 35,
    // shading model 번호(UE MCT_ShadingModel).
    ShadingModel = 36,
    // 번역 때 정해지는 열거 값(5.3절 — preset 선택 등).
    StaticEnum = 37,
    // 무엇이든 받는 통과 핀(UE MCT_Unknown — static switch·reroute·조건 스위치의 값 입력과 출력).
    // 연결은 막지 않고 번역이 연결된 값의 타입으로 정한다(5.3절).
    Any = 38
};

// 표의 타입 수다(마지막 값 + 1).
inline constexpr uint32 MaterialPinTypeCount = 39;

// 타입 군이다(연결 판정).
enum class MaterialPinTypeClass : uint8 {
    Numeric,
    Texture,
    TextureAny,
    Static,
    Surface,
    Attributes,
    ShadingModel,
    // 자원 종류가 아직 없는 타입(같은 타입끼리만).
    Pending,
    // 통과 핀(무엇과도 잇는다).
    Any
};

// 도메인 핀 타입 표다(MaterialGraphSchema::PinTypes가 돌려준다). 수명: 정적이다.
ROCKS_MATERIALSCHEMA_API const nodegraph::PinTypeTable& MaterialPinTypes() noexcept;

// 영어 이름이다. 범위 밖이면 "Invalid"다.
ROCKS_MATERIALSCHEMA_API StringView MaterialPinTypeName(MaterialPinType type) noexcept;

// 타입 군이다. 범위 밖이면 Pending이다.
ROCKS_MATERIALSCHEMA_API MaterialPinTypeClass
ClassifyMaterialPinType(MaterialPinType type) noexcept;

// NodeGraph PinTypeId로 바꾼다.
constexpr nodegraph::PinTypeId ToPinTypeId(MaterialPinType type) noexcept {
    return static_cast<nodegraph::PinTypeId>(type);
}

} // namespace rocks::material
