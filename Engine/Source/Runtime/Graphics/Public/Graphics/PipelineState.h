#pragma once

#include <Base/Types.h>
#include <GraphicsTypes/CompareFunction.h>

// 파이프라인 고정 기능 상태(RenderBase 4.4절 GX3, Renderer RQ5)와 attachment·간접 인자(GX4, RQ3)의
// 값 타입이다. 비교 함수(CompareFunction)는 깊이 비교 샘플러와 함께 쓰므로 GraphicsTypes에 있다.
// backend와 무관하며 각 backend가 자기 API 상태로 옮긴다. 이름은 Rocks 이름이고 UE
// 대응은 RenderBase 2절 대응표에 있다.
namespace rocks::graphics {

// 동시에 묶는 색 attachment(MRT) 수의 상한이다(D3D 8, Renderer RQ3). 장치 값은
// DeviceLimits::colorAttachments다.
inline constexpr uint32 MaxColorTargets = 8;

// 기본도형 종류다.
enum class PrimitiveTopology : uint8 {
    TriangleList,
    TriangleStrip,
    LineList,
    LineStrip,
    PointList
};

// 스텐실 값 연산이다. Saturate는 범위 끝에서 멈추고 Wrap은 돈다.
enum class StencilOperation : uint8 {
    Keep,
    Zero,
    Replace,
    IncrementSaturate,
    DecrementSaturate,
    Invert,
    IncrementWrap,
    DecrementWrap
};

// 앞면·뒷면 하나의 스텐실 시험이다. 기준 값은 CommandContext::SetStencilReference다.
struct StencilFace {
    CompareFunction compare = CompareFunction::Always;
    StencilOperation fail = StencilOperation::Keep;
    StencilOperation depthFail = StencilOperation::Keep;
    StencilOperation pass = StencilOperation::Keep;
};

// 깊이·스텐실 시험 상태다. 깊이 attachment가 없는 파이프라인은 둘 다 꺼야 한다.
struct DepthStencilState {
    bool depthTest = false;
    bool depthWrite = false;
    CompareFunction depthCompare = CompareFunction::GreaterEqual;
    bool stencilTest = false;
    uint8 stencilReadMask = 0xFF;
    uint8 stencilWriteMask = 0xFF;
    StencilFace front;
    StencilFace back;
};

// 블렌드 계수다(상수 계수·dual source는 넣지 않는다).
enum class BlendFactor : uint8 {
    Zero,
    One,
    SourceColor,
    InverseSourceColor,
    SourceAlpha,
    InverseSourceAlpha,
    DestinationColor,
    InverseDestinationColor,
    DestinationAlpha,
    InverseDestinationAlpha,
    SourceAlphaSaturate
};

// 블렌드 식이다. Min·Max는 계수를 쓰지 않는다.
enum class BlendOperation : uint8 {
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max
};

// 색 쓰기 채널이다. 여러 비트를 OR로 묶는다.
enum class ColorWriteMask : uint8 {
    None = 0,
    Red = 1u << 0,
    Green = 1u << 1,
    Blue = 1u << 2,
    Alpha = 1u << 3,
    All = 0xF
};

constexpr ColorWriteMask operator|(ColorWriteMask left, ColorWriteMask right) noexcept {
    return static_cast<ColorWriteMask>(static_cast<uint8>(left) | static_cast<uint8>(right));
}

constexpr ColorWriteMask operator&(ColorWriteMask left, ColorWriteMask right) noexcept {
    return static_cast<ColorWriteMask>(static_cast<uint8>(left) & static_cast<uint8>(right));
}

// 색 attachment 하나의 블렌드·쓰기 mask다. enable이 거짓이면 소스 색을 그대로 쓴다.
struct ColorTargetBlend {
    bool enable = false;
    BlendFactor sourceColor = BlendFactor::One;
    BlendFactor destinationColor = BlendFactor::Zero;
    BlendOperation colorOperation = BlendOperation::Add;
    BlendFactor sourceAlpha = BlendFactor::One;
    BlendFactor destinationAlpha = BlendFactor::Zero;
    BlendOperation alphaOperation = BlendOperation::Add;
    ColorWriteMask writeMask = ColorWriteMask::All;
};

// 소스 알파로 섞는 일반 알파 블렌드다(premultiplied 아님, 알파는 1 − 소스 알파로 섞는다).
constexpr ColorTargetBlend AlphaBlend() noexcept {
    ColorTargetBlend blend;
    blend.enable = true;
    blend.sourceColor = BlendFactor::SourceAlpha;
    blend.destinationColor = BlendFactor::InverseSourceAlpha;
    blend.sourceAlpha = BlendFactor::One;
    blend.destinationAlpha = BlendFactor::InverseSourceAlpha;
    return blend;
}

// 삼각형 컬링이다(UE ERasterizerCullMode 대응). 앞면은 표시된 화면에서 반시계로 감긴 삼각형이다
// (UE와 같다, ShaderSystem 7.2절 8항). 창·RenderTarget 어느 대상에 그려도 같은 삼각형이 같은 면으로
// 판정되며, backend는 이 규약을 자기 API의 앞면·컬 설정으로 옮긴다(D3D12 FrontCounterClockwise,
// OpenGL glFrontFace·glCullFace).
enum class CullMode : uint8 {
    // 컬링하지 않는다(양면).
    None,
    // 뒤면(화면에서 시계 방향)을 버린다. 일반 메시 그리기의 기본이다(UE CM_CW).
    Back,
    // 앞면(화면에서 반시계)을 버린다. 음수 행렬식 반전에 쓴다(UE CM_CCW).
    Front
};

// 래스터 상태다.
// - depthBias·depthBiasSlope·depthBiasClamp: 깊이 bias(그림자 깊이). depthBias는 깊이 형식의 최소
//   해상도 단위 상수, depthBiasSlope는 기울기 배수, depthBiasClamp는 bias 상한(0이면 없음)이다.
//   backend 식(D3D DepthBias·SlopeScaledDepthBias·DepthBiasClamp, Vulkan
//   depthBiasConstantFactor·SlopeFactor·Clamp, GL glPolygonOffset(slope, constant))을 그대로 쓰므로
//   같은 값의 결과가 backend마다 정확히 같지는 않다. depthBiasClamp가 0이 아니면 장치 능력
//   DeviceFeatures::depthBiasClamp가 필요하다.
// - depthClamp: 깊이 clip 대신 clamp한다. DeviceFeatures::depthClamp가 필요하다.
struct RasterState {
    // 버릴 면이다. 기본은 None이다(UE TStaticRasterizerState 기본 CM_None과 같고, 감김 순서를
    // 정하지 않은 화면 채우기·시험 그리기가 그대로 보인다). 메시 그리기는 Back을 쓰고, 물체·view
    // 행렬식이 음수면 Front 파이프라인으로 바꾼다(ShaderSystem 7.2절 8항).
    CullMode cullMode = CullMode::None;
    int32 depthBias = 0;
    float depthBiasSlope = 0.0f;
    float depthBiasClamp = 0.0f;
    bool depthClamp = false;
};

// attachment 시작 동작이다. DontCare는 이전 내용이 정해지지 않는다.
enum class LoadAction : uint8 {
    Load,
    Clear,
    DontCare
};

// attachment 끝 동작이다. DontCare는 패스 뒤 내용이 정해지지 않는다.
enum class StoreAction : uint8 {
    Store,
    DontCare
};

// 간접 그리기 인자 하나다(D3D·Vulkan·GL이 같은 배치). firstInstance는
// DeviceFeatures::indirectFirstInstance가 거짓인 장치에서 0이어야 한다(GPU가 쓴 값은 검사할 수
// 없다).
struct DrawIndirectArguments {
    uint32 vertexCount = 0;
    uint32 instanceCount = 0;
    uint32 firstVertex = 0;
    uint32 firstInstance = 0;
};

struct DrawIndexedIndirectArguments {
    uint32 indexCount = 0;
    uint32 instanceCount = 0;
    uint32 firstIndex = 0;
    int32 baseVertex = 0;
    uint32 firstInstance = 0;
};

struct DispatchIndirectArguments {
    uint32 groupCountX = 0;
    uint32 groupCountY = 0;
    uint32 groupCountZ = 0;
};

static_assert(sizeof(DrawIndirectArguments) == 16 && sizeof(DrawIndexedIndirectArguments) == 20 &&
                  sizeof(DispatchIndirectArguments) == 12,
              "indirect argument layouts must match the API structures");

} // namespace rocks::graphics
