#pragma once

#include <Core/Containers/SlotArray.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Graphics/DeviceCaps.h>
#include <Graphics/PipelineState.h>
#include <GraphicsTypes/ChannelSwizzle.h>
#include <GraphicsTypes/PixelFormat.h>
#include <GraphicsTypes/Sampler.h>
#include <GraphicsTypes/TextureDimension.h>
#include <GraphicsTypes/VertexFormat.h>

// 그래픽 장치 계약의 값 타입(handle·장치·자원 설정)이다. backend와 무관하며 API 객체를 담지
// 않는다. 픽셀 형식·샘플러는 GraphicsTypes 모듈에 있다.
namespace rocks::graphics {

// 그래픽 backend다. D3D12·D3D11·OpenGL·Vulkan은 구현 모듈(D3D12Graphics·D3D11Graphics·
// OpenGLGraphics·VulkanGraphics)이 있고, Metal은 자리만 둔다(구현 모듈이 아직 없어 선택하면
// Unsupported).
enum class Backend : uint8 {
    D3D12,
    D3D11,
    OpenGL,
    Vulkan,
    Metal
};

// "D3D12" 같은 이름이다. 범위 밖이면 "Unknown"이다.
const char* BackendName(Backend backend) noexcept;

// 자원 handle이다. 발급한 장치 안에서만 의미가 있으며 파괴 후에는 stale이 된다(slot+generation).
struct BufferHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const BufferHandle&) const noexcept = default;
};

struct TextureHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const TextureHandle&) const noexcept = default;
};

struct PipelineHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const PipelineHandle&) const noexcept = default;
};

struct SwapChainHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const SwapChainHandle&) const noexcept = default;
};

class ShaderSourceCompiler;

// 장치 생성 설정이다.
struct DeviceDesc {
    // API 검증 계층(D3D12·D3D11 debug layer, Vulkan VK_LAYER_KHRONOS_validation, GL debug
    // context)을 켠다. 없으면 켜지 않고 계속한다.
    bool enableValidation = false;
    // 하드웨어 대신 소프트웨어 장치를 쓴다(D3D12·D3D11 WARP, Vulkan CPU 장치 — lavapipe·SwiftShader
    // 같은 소프트웨어 ICD). 헤드리스 CI 검증용이다. Vulkan은 CPU 장치가 없으면 Unsupported다.
    // OpenGL은 무시한다.
    bool preferSoftware = false;
    // 업로드 ring 크기다(0이면 DefaultUploadRingBytes). 생성 초기 데이터·내용 갱신이 이 ring을
    // 거친다.
    uint64 uploadRingBytes = 0;
    // 스트리밍 staging pool 예산이다(0이면 DefaultStagingBudgetBytes).
    uint64 stagingBudgetBytes = 0;
    // 실행 중 컴파일러가 없는 backend(Vulkan)가 PipelineDesc의 HLSL 원문을 backend 코드로 바꿀 때
    // 부르는 개발용 컴파일 훅이다(Graphics/ShaderSourceCompiler.h). 쿠킹된 셰이더(ShaderSystem SH2)
    // 전까지만 쓰며 Release 실행 파일에는 넣지 않는다. null이면 그런 backend의 파이프라인 생성은
    // Unsupported다. D3D12·D3D11·OpenGL은 무시한다. 장치보다 오래 살아야 한다.
    ShaderSourceCompiler* shaderCompiler = nullptr;
};

// 장치 생성 실패의 원인 기록이다(Docs/Design/LoggingSystem.ko.md 13절). 호출자 스택에 두고 생성
// 함수에 넘긴다. 힙을 쓰지 않으며 문자열은 영어 UTF-8이고 넘치면 자른다(truncated).
//
// 사용 예:
//   DeviceCreateReport report;
//   auto created = CreateD3D11Device(allocator, desc, &report);
//   if (!created) {
//       // report.stage = "D3D11CreateDevice", report.nativeCode = HRESULT, report.detail = 설명
//   }
struct DeviceCreateReport {
    // 생성 함수가 돌려준 오류와 같다(성공이면 None).
    ErrorCode code = ErrorCode::None;
    // 원본 API 오류다(HRESULT, GL error). 없으면 0이다.
    int64 nativeCode = 0;
    // 실패한 단계 이름(API 호출 이름 등)이다.
    char stage[48]{};
    // 사람이 읽는 설명이다(후보별 시도 결과 등).
    char detail[512]{};
    // stage·detail을 자른 적이 있는지다.
    bool truncated = false;
};

// 창에 그리기 위한 OS 표면이다. Windows: window = HWND, instance = HINSTANCE.
struct NativeSurface {
    void* window = nullptr;
    void* instance = nullptr;
};

struct SwapChainDesc {
    NativeSurface surface;
    uint32 width = 0;
    uint32 height = 0;
    // 수직 동기화로 present한다.
    bool vsync = true;
};

// 버퍼 용도다(RenderBase 4.4절 GX2, UE EBufferUsageFlags 대응). 여러 비트를 OR로 묶는다. 정점
// 버퍼는 SetVertexBuffers, index 버퍼는 SetIndexBuffer로 묶고, 셰이더 읽기·쓰기는 버퍼 view
// (CreateBufferView)로 묶는다. 조합 규칙은 ValidateBufferDesc다.
enum class BufferUsage : uint16 {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    // 상수 버퍼다. 다른 용도와 묶지 않고 크기는 16 byte 배수다(D3D11 bind 규칙). 묶기는 GX6이다.
    Constant = 1u << 2,
    // 간접 그리기·dispatch 인자(uint32 배열)다. 명령은 GX4다.
    Indirect = 1u << 3,
    // 셰이더 읽기 view(SRV)를 만들 수 있다.
    ShaderRead = 1u << 4,
    // 셰이더 쓰기 view(UAV)를 만들 수 있다.
    ShaderWrite = 1u << 5,
    // 구조화 버퍼다(원소 크기 BufferDesc::stride). 구조화 view만 만들 수 있다.
    Structured = 1u << 6,
    // byte address(raw) view를 만들 수 있다(UE ByteAddressBuffer). typed view도 만들 수 있다.
    Raw = 1u << 7
};

constexpr BufferUsage operator|(BufferUsage left, BufferUsage right) noexcept {
    return static_cast<BufferUsage>(static_cast<uint16>(left) | static_cast<uint16>(right));
}

constexpr BufferUsage operator&(BufferUsage left, BufferUsage right) noexcept {
    return static_cast<BufferUsage>(static_cast<uint16>(left) & static_cast<uint16>(right));
}

// usage에 flags 가운데 하나라도 있는지다.
constexpr bool HasAnyBufferUsage(BufferUsage usage, BufferUsage flags) noexcept {
    return (usage & flags) != BufferUsage::None;
}

// 구조화 버퍼 원소의 최대 byte다(D3D11 StructureByteStride 상한 2048).
inline constexpr uint32 MaxStructuredStride = 2048;
// raw view 시작 byte 정렬이다(UE RHIResources.cpp:238과 같은 공통 규칙).
inline constexpr uint64 RawViewOffsetAlignment = 16;

// 버퍼 설정이다.
//
// 사용 예(광원 데이터 typed buffer, PS가 Buffer<float4>로 읽는다):
//   BufferDesc desc;
//   desc.size = lightCount * 16;
//   desc.usage = BufferUsage::ShaderRead | BufferUsage::ShaderWrite;
//   auto lights = device.CreateBuffer(desc, bytes);
//
// 사용 예(구조화 버퍼, 원소 32 byte):
//   desc.usage = BufferUsage::ShaderRead | BufferUsage::Structured;
//   desc.stride = 32;
struct BufferDesc {
    uint64 size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    // 구조화 버퍼 원소 byte다. Structured일 때만 0이 아니다.
    uint32 stride = 0;
};

// 장치와 무관한 버퍼 설정 규칙이다(backend가 생성 전에 부른다). 위반은 InvalidArgument다:
// - size 0, usage None 또는 모르는 비트
// - Constant를 다른 용도와 묶음, Constant 크기가 16 byte 배수가 아님
// - Structured와 Raw를 함께 씀
// - Structured인데 stride가 0·4 byte 배수 아님·MaxStructuredStride 초과·size가 stride 배수 아님,
//   ShaderRead·ShaderWrite가 없음, Vertex·Index·Indirect와 묶음(D3D11 구조화 버퍼 제약)
// - Structured가 아닌데 stride가 0이 아님
// - Raw인데 ShaderRead·ShaderWrite가 없거나 size가 4 byte 배수가 아님
// - Indirect인데 size가 4 byte 배수가 아님
// 장치 한도(DeviceLimits::bufferBytes)는 backend가 따로 보고 넘으면 Unsupported다.
Result<void> ValidateBufferDesc(const BufferDesc& desc) noexcept;

// 버퍼 view 종류다(UE EBufferType 대응).
enum class BufferViewKind : uint8 {
    // 형식 지정 원소(HLSL Buffer<T>·RWBuffer<T>, GLSL ES samplerBuffer·imageBuffer)
    Typed,
    // 구조화 원소(StructuredBuffer<T>, GLSL ES std430 storage block)
    Structured,
    // 32-bit 단위 byte address(ByteAddressBuffer, GLSL ES uint 배열 storage block)
    Raw
};

// view 접근이다. Read는 SRV, ReadWrite는 UAV다.
enum class ViewAccess : uint8 {
    Read,
    ReadWrite
};

// 버퍼 view handle이다(Device::CreateBufferView). Retire로 놓는다.
struct BufferViewHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const BufferViewHandle&) const noexcept = default;
};

// 버퍼 view 설정이다. 범위는 원소 단위이며 원소 byte는 종류가 정한다(Typed: 형식 블록 byte,
// Structured: 버퍼 stride, Raw: 4).
//
// 사용 예(RGBA32Float typed SRV, 버퍼 전체):
//   BufferViewDesc view;
//   view.kind = BufferViewKind::Typed;
//   view.format = PixelFormat::RGBA32Float;
//   auto srv = device.CreateBufferView(lights.Value(), view);
//   commands.SetBufferView(0, srv.Value());
struct BufferViewDesc {
    BufferViewKind kind = BufferViewKind::Typed;
    ViewAccess access = ViewAccess::Read;
    // Typed만 쓴다. 나머지 종류는 Unknown이어야 한다.
    PixelFormat format = PixelFormat::Unknown;
    // 시작 byte다. 원소 byte 배수여야 하고 Raw는 RawViewOffsetAlignment 배수다. 장치 정렬
    // (DeviceLimits::texelBufferOffsetAlignment·storageBufferOffsetAlignment)도 따른다.
    uint64 offset = 0;
    // 원소 수다. 0이면 버퍼 끝까지이며 그때 offset은 0이어야 한다(UE와 같은 규칙).
    uint64 elementCount = 0;
};

// 규칙을 통과한 view 범위다.
struct ResolvedBufferView {
    uint64 offset = 0;
    uint64 elementCount = 0;
    uint32 elementBytes = 0;

    uint64 Bytes() const noexcept {
        return elementCount * elementBytes;
    }
};

// 장치와 무관한 view 규칙이다(backend가 생성 전에 부른다). 위반은 InvalidArgument다:
// - 모르는 kind·access
// - Read인데 버퍼에 ShaderRead가 없음, ReadWrite인데 ShaderWrite가 없음
// - Typed: 구조화 버퍼, 형식이 무효·비압축 단일 plane 색 형식이 아님
// - Structured: 구조화 버퍼가 아님, 형식 지정
// - Raw: Raw 버퍼가 아님, 형식 지정, offset이 16 byte 배수가 아님
// - offset이 원소 byte 배수가 아니거나 버퍼 밖, elementCount 0인데 offset이 0이 아님, 범위가 버퍼
//   끝을 넘음
Result<ResolvedBufferView> ResolveBufferView(const BufferDesc& buffer,
                                             const BufferViewDesc& view) noexcept;

// 장치 한도로 view를 검사한다(backend가 ResolveBufferView 다음에 부른다). formatCaps는 view 형식의
// 이 장치 능력이다(Typed만 본다). 실패: 시작 byte가 장치 정렬(texelBufferOffsetAlignment·
// storageBufferOffsetAlignment) 배수가 아니면 InvalidArgument, Typed 형식에 TexelBufferRead(Read)·
// TexelBufferWrite(ReadWrite) 능력이 없거나 원소 수가 texelBufferElements를 넘거나 구조화·raw
// 범위가 storageBufferBytes를 넘으면 Unsupported(다른 형식·범위로 바꾸지 않는다). Vulkan backend는
// 여기에 더해 버퍼의 모든 texel 용도 능력을 요구한다(Device::CreateBufferView 주석).
Result<void> CheckBufferViewLimits(const DeviceLimits& limits, PixelFormatCaps formatCaps,
                                   const BufferViewDesc& view,
                                   const ResolvedBufferView& resolved) noexcept;

// 그리기에 묶는 버퍼 view slot 수다(GX6 파라미터 바인딩 전의 개발 경로 고정 배치, PipelineDesc
// 주석). slot n은 HLSL register t(n + 1), GLSL ES typed는 텍스처 유닛 PS n + 1·VS n + 17
// (samplerBuffer), 구조화·raw는 storage block binding n이다. 단계당 SRV 합계는 텍스처 t0와 합쳐
// 5다(형식 정의 최소 보장 16 안).
inline constexpr uint32 MaxDrawBufferViews = 4;

// 파이프라인이 읽는 버퍼 view slot 배치다. backend가 셰이더 리플렉션(D3D reflection, SPIR-V
// 리플렉션, GL program introspection)으로 채우고, 그리기 때 묶인 view가 배치를 채우는지 본다
// (DrawBufferSlotsSatisfied).
struct DrawBufferSlots {
    // slot n을 셰이더가 읽으면 비트 n이다.
    uint32 usedMask = 0;
    BufferViewKind kinds[MaxDrawBufferViews] = {};
    // Typed slot이 돌려주는 값 계열이다(view 형식의 PixelFormatInfo::shaderType과 같아야 한다).
    PixelShaderType typedShaderTypes[MaxDrawBufferViews] = {};
    // Structured slot의 셰이더 선언 원소 byte다(리플렉션이 알려 주지 않으면 0, 그때는 대조하지
    // 않는다).
    uint32 structureStrides[MaxDrawBufferViews] = {};
};

// 리플렉션한 slot 하나를 배치에 더한다(backend 공용). 같은 slot을 다른 단계가 이미 쓰면 종류·값
// 계열·구조화 stride(둘 다 알 때)가 같아야 하고, 다르면 거짓이다(backend는 InvalidArgument로
// 거부한다). stride는 Structured slot의 셰이더 선언 원소 byte이고 모르면 0이다.
bool AddDrawBufferSlot(DrawBufferSlots& slots, uint32 slot, BufferViewKind kind,
                       PixelShaderType type, uint32 stride = 0) noexcept;

// 그리기에 묶인 view 하나의 요약이다(backend가 view 기록에서 만든다). bound가 거짓이면 묶인 것이
// 없다. stride는 Structured view의 원소 byte(BufferDesc::stride)다.
struct BoundBufferViewInfo {
    bool bound = false;
    BufferViewKind kind = BufferViewKind::Typed;
    PixelShaderType shaderType = PixelShaderType::None;
    uint32 stride = 0;
};

// slots가 읽는 모든 slot에 같은 종류(Typed는 같은 값 계열, Structured는 셰이더가 선언한 stride를
// 알 때 같은 원소 byte)의 view가 묶였는지다. 아니면 backend는 그 그리기를 무시하고 IgnoredDraws를
// 올린다.
bool DrawBufferSlotsSatisfied(const DrawBufferSlots& slots,
                              const BoundBufferViewInfo (&bound)[MaxDrawBufferViews]) noexcept;

// index 형식이다(RenderBase 10.4절).
enum class IndexFormat : uint8 {
    UInt16,
    UInt32
};

// index 하나의 byte 크기다(2·4).
constexpr uint32 IndexFormatSize(IndexFormat format) noexcept {
    return format == IndexFormat::UInt16 ? 2u : 4u;
}

enum class TextureUsage : uint8 {
    // 셰이더에서 샘플링한다(초기 픽셀 필요). 형식 능력 Sampled가 필요하고 mip이 2개 이상이면
    // Mipmaps도 필요하다.
    Sampled,
    // 렌더 타깃으로 그리고 ReadPixels로 읽거나 샘플링한다(초기 픽셀 없음, mip 1). 형식 능력
    // RenderTarget·Sampled가 필요하다.
    RenderTarget,
    // 깊이·스텐실 타깃이다(초기 픽셀 없음, mip 1). 형식 능력 DepthStencil이 필요하고, 형식이
    // Sampled면 깊이 plane을 샘플할 수 있다. 깊이 그리기 경로는 GX2·GX3에서 연다(PixelFormat.ko.md
    // 개정 3).
    DepthStencil
};

// 텍스처 설정이다(AssetSystem A0-G 차원·swizzle·샘플러).
//
// 차원(dimension)과 크기:
// - Texture2D: width × height. depth·arraySize는 1이다.
// - Texture2DArray: width × height × arraySize 층.
// - Texture3D: width × height × depth. mip마다 세 축이 모두 절반으로 준다.
// - TextureCube: 한 변 width(= height)의 6면(CubeFace 순서, UE·D3D 규약).
// - TextureCubeArray: cube arraySize개(면 6 × arraySize). 장치 기능 DeviceFeatures::
//   textureCubeArray가 필요하다.
// subresource(전이·상태 추적 단위)는 mip × slice다. slice는 배열 층·cube 면(cube array는 cube
// c의 면 f가 c × 6 + f)이고 3D는 slice가 하나(깊이 면은 subresource가 아니다)다
// (TextureSliceCount).
//
// 초기 픽셀 배치(Sampled): mip 0부터 mipLevels개를 빈틈 없이 잇는다(mip 우선). mip 하나는 그 mip의
// 2D 이미지 TextureMipImageCount개(배열 층·cube 면 순서, 3D는 깊이 면 z = 0부터)를 잇고, 이미지
// 하나는 MipExtent(width, level) × MipExtent(height, level) 픽셀(블록 형식은 블록 행 단위), 첫 행이
// 위, 행 사이 padding이 없다. 다중 plane 형식은 이미지 안에서 plane 0 다음 plane 1이다. 전체 크기는
// TextureDataBytes, mip 하나는 TextureMipBytes다. UpdateTexture·UploadTextureFromStaging·
// ReadTexture도 mip 하나 전체(모든 slice)를 같은 배치로 다룬다. 장치가 대응하지 않는 형식·차원은
// 생성이 Unsupported다(PixelFormat.ko.md 8.3절).
//
// 사용 예(6면 cube, mip 하나, 면마다 64×64 RGBA8):
//   TextureDesc desc;
//   desc.dimension = TextureDimension::TextureCube;
//   desc.width = desc.height = 64;
//   desc.format = PixelFormat::RGBA8Unorm;
//   auto cube = device.CreateTexture(desc, faces);   // faces: 6 × 64 × 64 × 4 byte, +X 면부터
struct TextureDesc {
    TextureDimension dimension = TextureDimension::Texture2D;
    uint32 width = 0;
    uint32 height = 0;
    // Texture3D의 깊이다. 다른 차원은 1이다.
    uint32 depth = 1;
    // Texture2DArray의 층 수, TextureCubeArray의 cube 수다. 다른 차원은 1이다.
    uint32 arraySize = 1;
    PixelFormat format = PixelFormat::RGBA8UnormSrgb;
    TextureUsage usage = TextureUsage::Sampled;
    // 1–TextureFullMipCount(desc)다. RenderTarget·DepthStencil은 1이어야 한다.
    uint32 mipLevels = 1;
    // 이 텍스처를 묶을 때(SetTexture) 셰이더가 읽는 성분 대응이다(view swizzle). 항등이 아니면
    // D3D11은 Unsupported다(view swizzle이 없다). attachment·복사·ReadTexture에는 적용하지 않는다.
    ChannelSwizzle swizzle;
    // 이 텍스처를 묶을 때 쓰는 기본 샘플러다(결정 R12: 바인딩 때 다른 샘플러를 고르는 것은 GX6).
    SamplerDesc sampler;
};

// 텍스처의 subresource slice 수다(2D·3D 1, 2DArray arraySize, Cube 6, CubeArray 6 × arraySize).
// 무효 차원은 0이다.
uint32 TextureSliceCount(const TextureDesc& desc) noexcept;

// desc 크기의 전체 mip 수다(3D는 깊이도 본다).
uint32 TextureFullMipCount(const TextureDesc& desc) noexcept;

// mip level의 2D 이미지 수다(slice 수 × 3D면 그 mip의 깊이).
uint32 TextureMipImageCount(const TextureDesc& desc, uint32 level) noexcept;

// mip level 하나의 데이터 byte다(모든 이미지, 다중 plane은 plane 합). 형식·크기가 잘못되면 0이다.
uint64 TextureMipBytes(const TextureDesc& desc, uint32 level) noexcept;

// mip level 하나의 plane 하나 데이터 byte다(ReadTexture 출력 크기).
uint64 TextureMipPlaneBytes(const TextureDesc& desc, uint32 level, uint32 plane) noexcept;

// 초기 픽셀 전체 byte다(mip 0부터 mipLevels개의 TextureMipBytes 합). 잘못된 desc는 0이다.
uint64 TextureDataBytes(const TextureDesc& desc) noexcept;

// 장치와 무관한 텍스처 설정 규칙이다(backend가 생성 전에 부른다).
// InvalidArgument:
// - 모르는 차원·용도·형식, 0 크기, 무효 샘플러·swizzle
// - 3D가 아닌데 depth ≠ 1, 배열 차원이 아닌데 arraySize ≠ 1, depth·arraySize 0
// - Cube·CubeArray인데 width ≠ height
// - mipLevels가 0이거나 TextureFullMipCount 초과, RenderTarget·DepthStencil인데 mipLevels ≠ 1
// - DepthStencil인데 깊이 형식이 아님
// Unsupported(이 단계의 계약 밖):
// - RenderTarget·DepthStencil인데 Texture2D가 아님(slice attachment는 렌더 세션 GX2 몫)
// - 다중 plane 형식(깊이·스텐실 D24S8·D32S8, 비디오)인데 Texture2D가 아님
Result<void> ValidateTextureDesc(const TextureDesc& desc) noexcept;

// 장치 한도로 텍스처를 검사한다(ValidateTextureDesc 다음). 넘으면 Unsupported다: 한 변이
// textureSize2D(Cube·CubeArray는 textureSizeCube, 3D는 세 축 모두 textureSize3D)를 넘음, slice
// 수가 textureArrayLayers를 넘음, textureCubeArray 기능 없이 CubeArray.
Result<void> CheckTextureLimits(const DeviceCaps& caps, const TextureDesc& desc) noexcept;

// 텍스처 view 설정이다(Device::CreateTextureView). 원 텍스처의 자원을 공유하고 형식·plane·성분
// 대응만 다르게 읽는다(PixelFormat.ko.md 9·10·11절). 차원·mip·slice는 원 텍스처 전체다(mip·slice
// 범위 view는 렌더 세션 GX2 몫).
// - format: Unknown이면 그 plane의 기본 형식이다. 색·압축 형식은 자기 형식이나 sRGB 짝,
//   깊이·스텐실과 비디오는 plane 형식(PixelFormatInfo::planes)만 받는다.
// - plane: 단일 plane 형식은 0, 깊이·스텐실은 0(깊이)·1(스텐실, uint으로 읽고 값은 G 채널),
//   비디오는 0(휘도 Y)·1(색차 CbCr)이다.
// - swizzle: view 형식의 성분 기준 대응이다(원 텍스처의 TextureDesc::swizzle은 이어받지 않는다).
//   스텐실 plane의 "값은 G" 규칙 위에 합성된다. 항등이 아니면 D3D11은 Unsupported다.
//
// 사용 예(NV12의 색차 plane을 RG8로 샘플):
//   auto uv = device.CreateTextureView(video, {PixelFormat::Unknown, 1, {}});
//   commands.SetTexture(uv.Value());
struct TextureViewDesc {
    PixelFormat format = PixelFormat::Unknown;
    uint32 plane = 0;
    ChannelSwizzle swizzle;
};

// texture 형식에 desc가 맞는지 확인하고 view가 읽는 픽셀 형식(plane 형식 또는 sRGB 짝)을 돌려준다.
// 장치와 무관한 규칙이며 backend가 생성 전에 부른다. 실패: 무효 형식·plane 범위 밖·plane에 맞지
// 않는 형식·무효 swizzle InvalidArgument.
Result<PixelFormat> ResolveTextureViewFormat(PixelFormat texture,
                                             const TextureViewDesc& desc) noexcept;

// desc의 텍스처를 만들 때 형식에 필요한 능력이다(TextureUsage 주석, 3D는 Texture3D, Cube·
// CubeArray는 TextureCube를 더한다).
PixelFormatCaps RequiredPixelFormatCaps(const TextureDesc& desc) noexcept;

// 정점 배치 한계다(RenderBase 10.4절). 장치 최소 보장 쪽 값이다: 속성 16(Vulkan
// maxVertexInputAttributes·GL_MAX_VERTEX_ATTRIBS 최소), stream 8(Metal 정점 buffer 상한), offset ≤
// 2047· stride ≤ 2048(Vulkan maxVertexInputAttributeOffset·maxVertexInputBindingStride, GL
// MAX_VERTEX_ATTRIB_RELATIVE_OFFSET·MAX_VERTEX_ATTRIB_STRIDE 최소), 둘 다 4 byte 배수(Metal
// stride).
inline constexpr uint32 MaxVertexAttributes = 16;
inline constexpr uint32 MaxVertexStreams = 8;
inline constexpr uint32 MaxVertexAttributeOffset = 2047;
inline constexpr uint32 MaxVertexStride = 2048;
inline constexpr uint32 VertexLayoutAlignment = 4;

// 정점 속성 하나다. location(0–15)은 HLSL 시맨틱 ATTRIBUTE{location}, GLSL ES layout(location)이다.
// stream(0–7)의 정점에서 offset byte 위치에 format 형식으로 놓인다.
struct VertexAttribute {
    uint32 location = 0;
    VertexFormat format = VertexFormat::Float3;
    uint32 stream = 0;
    uint32 offset = 0;
};

// stream이 정점마다 진행하는지 인스턴스마다 진행하는지다. 인스턴스 stream은 인스턴스마다 한 번(간격
// 1) 진행한다.
enum class VertexStepRate : uint8 {
    Vertex,
    Instance
};

// 정점 stream 하나다. streams 배열의 index가 stream 번호다.
struct VertexStream {
    uint32 stride = 0;
    VertexStepRate stepRate = VertexStepRate::Vertex;
};

// 정점 배치다(UE vertex declaration 대응). 속성이 없으면 정점 입력이 없는 배치다(SV_VertexID로
// 위치를 만드는 그리기).
//
// 사용 예(위치 stream 0, 인스턴스 색 stream 1):
//   const VertexAttribute attributes[] = {
//       {0, VertexFormat::Float3, 0, 0},
//       {1, VertexFormat::UByte4Unorm, 1, 0}};
//   const VertexStream streams[] = {{12, VertexStepRate::Vertex}, {4, VertexStepRate::Instance}};
//   desc.layout = {attributes, streams};
struct VertexLayout {
    Span<const VertexAttribute> attributes;
    Span<const VertexStream> streams;
};

// 배치 자체를 검사한다(파이프라인 생성이 부른다). 위반은 InvalidArgument다: 속성 수 > 16, stream 수
// > 8, location ≥ 16 또는 중복, 모르는 형식, 속성의 stream이 streams 밖, offset > 2047·stride >
// 2048 ·4 byte 배수 아님, offset + 형식 크기 > stride, 속성이 하나도 없는 stream.
Result<void> ValidateVertexLayout(const VertexLayout& layout) noexcept;

// 정점 버퍼 묶기 하나다(SetVertexBuffers).
struct VertexBufferBinding {
    BufferHandle buffer;
    uint64 offset = 0;
};

// backend별 셰이더 원문이다. 장치는 자기 backend 원문만 쓴다. 진입점 이름은 HLSL에서 VSMain·PSMain,
// GLSL에서 main이다. OpenGL backend의 glsl은 GLSL ES 3.20(`#version 320 es`) 원문이다(OpenGL ES
// 3.2 장치, OpenGLGraphics/OpenGLDevice.h). SH2에서 쿠킹된 코드로 바뀐다.
struct ShaderSource {
    StringView hlsl;
    StringView glsl;
};

// 그리기 파이프라인 설정이다. 모든 파이프라인은 같은 고정 자원 배치를 쓴다(파라미터 바인딩 GX6 전의
// 개발 경로):
// - 그리기 상수(DrawConstants, HLSL b0 / GLSL ES 루트 상수 에뮬레이션 배열 uniform highp vec4
//   vu_h[5]·pu_h[5])
// - PS 텍스처 하나(HLSL t0·s0 / GLSL ES layout(binding = 0) sampler2D ps0). 셰이더는 묶을 텍스처의
//   차원에 맞는 타입(Texture2D·Texture2DArray·Texture3D·TextureCube·TextureCubeArray / sampler2D·
//   sampler2DArray·sampler3D·samplerCube·samplerCubeArray)을 선언한다. 샘플러는 묶은 텍스처의
//   TextureDesc::sampler를 따르며, 비교 샘플러(SamplerDesc::comparison)면 HLSL은
//   SamplerComparisonState s0, GLSL ES는 sampler*Shadow ps0으로 선언한다.
// - VS·PS가 읽는 버퍼 view slot 0–3(SetBufferView). HLSL은 register t1–t4에 Buffer<T>·
//   StructuredBuffer<T>·ByteAddressBuffer를 선언한다. GLSL ES는 typed를 samplerBuffer·
//   isamplerBuffer·usamplerBuffer로 PS는 layout(binding = 1 + slot), VS는 layout(binding = 17 +
//   slot)(VS 텍스처 유닛은 16부터, ShaderSystem 7.2절 4항)에, 구조화·raw를 layout(std430,
//   binding = slot) readonly buffer로 선언하고 raw block 이름은 type_ByteAddressBuffer로
//   시작한다(SPIRV-Cross 출력 이름). 생성은 셰이더 리플렉션으로 slot 배치(DrawBufferSlots)를
//   얻는다.
// 배치 밖 자원(다른 register·상수 버퍼, VS·PS의 UAV)을 선언한 셰이더는 InvalidArgument다(UAV는
// compute만, ShaderSystem 7.2절 4항). OpenGL ES의 VS·PS storage block은 장치 한도
// (DeviceLimits::vertex·pixel.storageBuffers, ES 3.2 최소 보장 0)를 넘으면 링크가 실패한다
// (DependencyFailed). 깊이는 쓰지 않는다.
//
// 셰이더 입력 대조(RenderBase 10.4절): 정점 셰이더가 읽는 모든 location이 layout에 있어야 하고
// 형식의 셰이더 타입 계열(VertexFormatShaderType)이 셰이더 입력 타입과 같아야 한다. 셰이더가 읽지
// 않는 속성은 허용한다. 입력은 D3D12가 컴파일한 VS 입력 signature(시맨틱 ATTRIBUTEn), OpenGL이 링크
// 뒤 active attribute에서 얻는다. 위반은 InvalidArgument이며 LastError에 location·형식·셰이더
// 타입을 적는다. 정점 입력 시맨틱은 ATTRIBUTEn만 쓴다(SV_ 시스템 값 제외).
//
// 출력·고정 기능 상태(GX3, Renderer RQ5): 색 attachment 형식 colorFormats[0 .. colorTargetCount),
// 깊이 attachment 형식 depthFormat(없으면 Unknown), 타깃별 블렌드·쓰기 mask, alpha-to-coverage,
// 깊이·스텐실·래스터 상태, 기본도형 종류. 그릴 패스의 attachment 형식과 같아야 한다(다르면 검증
// 계층 오류이며 장치는 검사하지 않는다). 색 attachment가 없는 파이프라인(깊이만)은
// colorTargetCount 0이다. 규칙 위반은 InvalidArgument(ValidatePipelineDesc), 장치 능력 밖(형식의
// RenderTarget·Blendable·DepthStencil, colorAttachments 수, depthClamp·depthBiasClamp 기능)은
// Unsupported다.
//
// 사용 예(GBuffer 3장 + reverse-Z 깊이):
//   PipelineDesc desc;
//   desc.colorTargetCount = 3;
//   desc.colorFormats[0] = PixelFormat::RGBA8Unorm; ...
//   desc.depthFormat = PixelFormat::D32Float;
//   desc.depthStencil.depthTest = true;
//   desc.depthStencil.depthWrite = true;   // depthCompare 기본 GreaterEqual
struct PipelineDesc {
    ShaderSource vertex;
    ShaderSource pixel;
    VertexLayout layout;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    uint32 colorTargetCount = 1;
    PixelFormat colorFormats[MaxColorTargets] = {PixelFormat::RGBA8UnormSrgb};
    ColorTargetBlend blend[MaxColorTargets];
    bool alphaToCoverage = false;
    PixelFormat depthFormat = PixelFormat::Unknown;
    DepthStencilState depthStencil;
    RasterState raster;
};

// 장치와 무관한 파이프라인 상태 규칙이다(backend가 생성 전에 부른다). 위반은 InvalidArgument다:
// colorTargetCount가 MaxColorTargets 초과, 무효한 형식 번호, 색 attachment도 깊이도 없음, 깊이
// attachment 없이 깊이·스텐실 시험, 스텐실 plane 없는 깊이 형식에 스텐실 시험, 모르는 enum 값
// (기본도형·비교·스텐실 연산·블렌드 계수·식·쓰기 mask·컬). 형식이 렌더 타깃·깊이 타깃이 될 수
// 있는지는 장치 능력이라 backend가 Unsupported로 공개한다.
Result<void> ValidatePipelineDesc(const PipelineDesc& desc) noexcept;

// 파이프라인이 장치 능력(DeviceCaps) 안인지다(backend가 ValidatePipelineDesc 다음에 부른다). 넘으면
// Unsupported다: colorTargetCount > DeviceLimits::colorAttachments, 기능이 없는데 depthClamp·0이
// 아닌 depthBiasClamp·alphaToCoverage를 켬, independentBlend 없이 타깃마다 다른 블렌드. 다른 값으로
// 바꿔 만들지 않는다.
Result<void> CheckPipelineFeatures(const DeviceCaps& caps, const PipelineDesc& desc) noexcept;

// 파이프라인이 깊이나 스텐실 값을 쓰는지다(깊이 쓰기, 또는 Keep이 아닌 스텐실 연산과 0이 아닌 쓰기
// mask). 읽기 전용 깊이 attachment(DepthAttachment::readOnly)에서는 그런 그리기를 무시한다.
bool PipelineWritesDepthStencil(const PipelineDesc& desc) noexcept;

// compute 파이프라인 설정이다. 진입점은 HLSL CSMain, GLSL ES는 main(#version 320 es compute 셰이더,
// layout(local_size_*))이다. 자원 배치(GX6 전 개발 경로 고정 배치):
// - 그리기 상수 DrawConstants(HLSL b0 / GLSL ES uniform highp vec4 cu_h[5])
// - 읽기 버퍼 view slot 0–3(SetBufferView): HLSL t1–t4, GLSL ES typed는 텍스처 유닛 1 + slot
//   (samplerBuffer), 구조화·raw는 storage block binding slot
// - 쓰기 버퍼 view slot 0–3(SetWritableBufferView): HLSL u0–u3(RWBuffer<T>·RWStructuredBuffer<T>·
//   RWByteAddressBuffer), GLSL ES typed는 image unit slot(layout(binding = slot, <형식>)
//   imageBuffer), 구조화·raw는 storage block binding 4 + slot
// 배치 밖 자원은 InvalidArgument다.
struct ComputePipelineDesc {
    ShaderSource shader;
};

// 그리기마다 넘기는 상수다. color는 선형 RGBA다.
//
// transform은 행 벡터 규약(p' = p · M)의 4x4 행렬을 행 우선(row-major)으로 담는다: Core Mat4의 저장
// 순서(m[row * 4 + column], 이동은 m[12..14]) 그대로이며 올릴 때 전치하지 않는다. 셰이더는
// mul(float4(position, 1), transform)으로 쓰고, 장치가 행렬을 행 우선으로 담는다(UE와 같다,
// ShaderSystem 7.2절 6항): D3D12는 HLSL을 D3DCOMPILE_PACK_MATRIX_ROW_MAJOR(FXC /Zpr)로 컴파일하고,
// OpenGL ES는 80 byte를 vec4 5개(vu_h·pu_h)로 그대로 올리며 GLSL ES 원문이
// mat4(vu_h[0], vu_h[1], vu_h[2], vu_h[3]) * v로 쓴다(열 우선으로 읽은 Mᵀ와 v의 곱 = v·M,
// 쿠킹된 ESSL과 같다).
struct DrawConstants {
    float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float color[4] = {1, 1, 1, 1};
};

struct ClearColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// 패스의 색 attachment 하나다(RenderTarget 텍스처, mip 0). 정수 형식의 clear는 값을 그 형식으로
// 바꿔 쓴다.
struct ColorAttachment {
    TextureHandle texture;
    LoadAction load = LoadAction::Clear;
    StoreAction store = StoreAction::Store;
    ClearColor clear;
};

// 패스의 깊이·스텐실 attachment다(DepthStencil 텍스처). readOnly면 깊이·스텐실을 읽기 전용으로
// 붙인다(접근 DepthRead, 같은 패스에서 SRV로 함께 읽기는 RQ6의 GX5 단계). 그때 깊이·스텐실을 쓰는
// 파이프라인의 그리기는 무시된다(IgnoredDraws). clearDepth 기본 0은 UE reverse-Z의 먼 평면이다.
struct DepthAttachment {
    TextureHandle texture;
    LoadAction depthLoad = LoadAction::Clear;
    StoreAction depthStore = StoreAction::Store;
    float clearDepth = 0.0f;
    LoadAction stencilLoad = LoadAction::Clear;
    StoreAction stencilStore = StoreAction::Store;
    uint8 clearStencil = 0;
    bool readOnly = false;
};

// 패스 시작 설정이다(Device::BeginPass, GX4). 색 attachment는 앞에서부터 colorCount개를 쓴다. 모든
// attachment의 크기가 같아야 한다. 색도 깊이도 없는 패스는 compute·복사 전용이고 그 안의 그리기는
// 무시된다.
//
// 사용 예(MRT 2장 + 깊이):
//   PassDesc pass;
//   pass.colorCount = 2;
//   pass.colors[0] = {albedo};
//   pass.colors[1] = {normal, LoadAction::Clear, StoreAction::Store, {0.5f, 0.5f, 1, 1}};
//   pass.depth.texture = depth;
//   auto commands = device.BeginPass(pass);
struct PassDesc {
    ColorAttachment colors[MaxColorTargets];
    uint32 colorCount = 0;
    DepthAttachment depth;
};

// viewport다(픽셀 단위, 원점은 대상 왼쪽 위). 깊이 범위 기본은 [0, 1]이다.
struct Viewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;
};

// scissor 사각형이다(픽셀, 원점은 대상 왼쪽 위, [x, x + width) × [y, y + height)).
struct ScissorRect {
    uint32 x = 0;
    uint32 y = 0;
    uint32 width = 0;
    uint32 height = 0;
};

// 제출 queue 종류다(계획 11.2절 QueueId). 1차 범위는 graphics queue 하나다(RenderBase 1절 렌더
// 그래프 결정). compute·copy queue는 그 단계에서 더한다.
enum class QueueKind : uint8 {
    Graphics
};

// GPU 제출 하나의 완료 지점이다(계획 11.2절 GpuFencePoint). 장치 incarnation(deviceEpoch)·queue·
// fence 값의 튜플이며, 같은 epoch·queue 안에서 value가 클수록 나중 제출이다. 뒤 제출의 완료는 앞
// 제출의 완료를 뜻한다.
//
// - deviceEpoch는 장치를 만들 때마다 프로세스 안에서 새로 받는 번호다(NewDeviceEpoch). 장치 손실 뒤
//   새로 만든 장치는 다른 epoch이므로 구 장치의 지점을 새 장치에 물으면 WrongContext다.
// - value 0(IsNull)은 "제출 없음"이며 늘 끝난 것으로 본다.
//
// 사용 예:
//   auto submitted = device.EndFrame();
//   if (submitted) {
//       slot.completion = submitted.Value();
//   }
//   ...
//   auto waited = device.WaitFence(slot.completion);   // 다른 스레드에서도 된다
struct GpuFencePoint {
    uint64 deviceEpoch = 0;
    QueueKind queue = QueueKind::Graphics;
    uint64 value = 0;

    bool IsNull() const noexcept {
        return value == 0;
    }

    bool operator==(const GpuFencePoint&) const noexcept = default;
};

// fence 완료 통지 함수다(Device::NotifyFence). result는 완료면 None, 장치 손실이면 Destroyed, 완료
// 전에 장치가 닫히면 ShuttingDown이다. 장치의 관찰 스레드나 NotifyFence를 부른 스레드에서 불리므로
// 짧게 끝내고 장치 API(스레드 안전 fence API 제외)를 부르지 않는다.
using FenceCallback = void (*)(ErrorCode result, void* context) noexcept;

// 새 장치 incarnation 번호다(1부터, 프로세스 안에서 다시 쓰지 않는다). backend가 장치를 만들 때
// 부른다. 어느 스레드에서나 부를 수 있다.
uint64 NewDeviceEpoch() noexcept;

// 스트리밍 staging handle이다(Device::AcquireStaging). 발급한 장치 안에서만 의미가 있다.
struct StagingHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const StagingHandle&) const noexcept = default;
};

// 받은 staging이다. data는 size byte의 CPU 가시 메모리이며 Release까지 주소가 바뀌지 않는다(IO
// 목적지로 바로 쓸 수 있다).
struct StagingBuffer {
    StagingHandle handle;
    uint8* data = nullptr;
    uint64 size = 0;
};

// staging pool 진단이다.
struct StagingStats {
    // 호출자가 가진 수, Release했지만 업로드 fence를 기다리는 수, 재사용을 기다리는 빈 수다.
    uint64 acquired = 0;
    uint64 waiting = 0;
    uint64 free = 0;
    uint64 createdBytes = 0;
    uint64 created = 0;
    uint64 reused = 0;
};

// 비동기 readback handle이다(Device::RequestReadback).
struct ReadbackHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const ReadbackHandle&) const noexcept = default;
};

// readback 요청 결과다. completion이 끝나면 TryReadback이 bytes byte를 낸다.
struct ReadbackRequest {
    ReadbackHandle handle;
    GpuFencePoint completion;
    uint64 bytes = 0;
};

// 업로드 경로 진단이다(RenderBase GX8).
struct UploadStats {
    uint64 ringCapacity = 0;
    uint64 ringUsed = 0;
    // ring이 0으로 감은 횟수다.
    uint64 ringWraps = 0;
    // ring에 자리가 없어 전용 staging으로 올린 수다.
    uint64 dedicatedUploads = 0;
    // 아직 제출하지 않은 업로드 기록이 있는지다.
    bool pending = false;
};

// 업로드 ring 기본 크기(4 MiB)와 스트리밍 staging 예산 기본값(256 MiB)이다.
inline constexpr uint64 DefaultUploadRingBytes = uint64{4} << 20;
inline constexpr uint64 DefaultStagingBudgetBytes = uint64{256} << 20;

// --- GPU 계측(RenderBase 13.2절, GX9) ---

// timestamp를 담는 프레임 slot 수와 slot(제출 하나)당 timestamp 한도다. slot이 모두 회수 전이면
// CommandContext::WriteTimestamp가 기다리지 않고 Busy다.
inline constexpr uint32 TimestampFrameSlots = 4;
inline constexpr uint32 MaxTimestampsPerFrame = 256;

// timestamp 하나를 기록할 자리다(TimestampRing::Allocate). query는 결과를 맞춰 볼 번호(1부터
// 증가), index는 backend query pool 칸이다.
struct TimestampWrite {
    uint64 query = 0;
    uint32 index = 0;
};

// backend query pool의 연속 구간이다(제출 전 resolve용).
struct TimestampRange {
    uint32 first = 0;
    uint32 count = 0;
};

// backend가 resolve한 결과를 읽은 상태다.
enum class TimestampReadState : uint8 {
    // 값이 준비되었다.
    Ready,
    // GPU는 끝났지만 결과가 아직 보이지 않는다(다음 회수에서 다시 묻는다).
    NotReady,
    // 측정 구간이 끊겼다(전력 상태 변화·GPU 재설정 등). 값은 무효다.
    Disjoint
};

// GPU 완료 뒤 회수한 timestamp 결과 하나다(Device::CollectTimestamps).
struct TimestampSample {
    // CommandContext::WriteTimestamp가 돌려준 번호다.
    uint64 query = 0;
    // GPU tick이다. 초당 tick 수는 DeviceInfo::timestampFrequency다. disjoint면 0이다.
    uint64 ticks = 0;
    // true면 값이 무효다(구간 끊김 또는 제출 실패). 이 값으로 구간을 재지 않는다.
    bool disjoint = false;
};

// 같은 순간의 GPU tick과 CPU 시각 한 쌍이다(Device::CalibrateClocks). CPU 시각은
// platform::MonotonicNanoseconds 시간축의 ns와 platform::MonotonicTicks 시간축의 tick(trace 시간축,
// Docs/Design/TraceSystem.ko.md 4.4절)으로 함께 준다.
struct ClockCalibration {
    uint64 gpuTicks = 0;
    uint64 cpuNanoseconds = 0;
    // GPU tick의 초당 수다(DeviceInfo::timestampFrequency와 같다).
    uint64 gpuFrequency = 0;
    // cpuNanoseconds와 같은 순간의 platform::MonotonicTicks 값이다(GPU 구간을 trace tick으로 바꿀
    // 때 쓴다. ns를 다시 tick으로 바꾸는 반올림을 피한다).
    uint64 cpuTicks = 0;
    // 두 시각이 같은 순간이라는 추정의 오차 반경(ns)이다. D3D12는 queue clock calibration이 한 번에
    // 읽은 쌍이라 CPU tick 하나의 길이, OpenGL은 GL_TIMESTAMP 조회 앞뒤 CPU 시각 차의 절반, D3D11은
    // GPU가 쉴 때 넣은 timestamp의 제출 전 CPU 시각과 완료를 본 CPU 시각 차의 절반, Vulkan은
    // vkGetCalibratedTimestamps가 준 최대 편차(maxDeviation)다.
    uint64 uncertaintyNanoseconds = 0;
};

// frequency(초당 tick) 기준 ticks를 ns로 바꾼다. 중간 곱이 넘치지 않으며 1초 미만 부분만 double로
// 계산한다. frequency가 0이면 0이다. backend가 OS 시계 tick(예: QPC)을 CPU 시간축으로 옮길 때도
// 쓴다.
uint64 TicksToNanoseconds(uint64 ticks, uint64 frequency) noexcept;

// GPU tick을 calibration 기준으로 CPU 시간축 ns로 옮긴다. ticks가 기준보다 앞이면 그만큼 앞선
// 시각이다(0 아래는 0). gpuFrequency가 0이면 cpuNanoseconds를 그대로 돌려준다. 두 시계의 drift는
// 보정하지 않으므로 오래된 calibration일수록 오차가 커진다(프레임마다 새로 얻기를 권한다).
//
// 사용 예:
//   auto pair = device.CalibrateClocks();
//   uint64 cpuTime = GpuTicksToCpuNanoseconds(pair.Value(), sample.ticks);
uint64 GpuTicksToCpuNanoseconds(const ClockCalibration& calibration, uint64 ticks) noexcept;

// debug marker 이름의 최대 byte 수다. 넘으면 UTF-8 글자 경계에서 자른다.
inline constexpr usize MaxDebugMarkerNameBytes = 255;

// name을 MaxDebugMarkerNameBytes 안으로 자른다(UTF-8 글자 중간에서 자르지 않는다). backend가
// marker를 기록하기 전에 쓴다. 잘못된 UTF-8은 검사하지 않는다.
StringView ClipDebugMarkerName(StringView name) noexcept;

// 장치 정보다. 문자열은 장치가 소유하며 장치 수명 동안 유효하다.
struct DeviceInfo {
    Backend backend = Backend::D3D12;
    StringView adapterName;
    // 이 장치의 incarnation 번호다(GpuFencePoint::deviceEpoch).
    uint64 deviceEpoch = 0;
    bool validationEnabled = false;
    bool software = false;
    // 자원·파이프라인 생성(Create*)을 소유 스레드가 아닌 스레드에서도 부를 수 있는지다(RenderBase
    // 5.4절 GX11). false면 소유 스레드 전용이고 다른 스레드의 생성 요청은 렌더 lane 작업으로
    // 넘긴다. D3D12는 true다: 생성·갱신·staging 업로드는 내부 잠금 아래 업로드 기록에 쌓이고 소유
    // 스레드의 다음 제출이 실행한다(GX8). D3D11도 true다(free-threaded 장치, 갱신은 업로드 기록).
    // Vulkan도 true다(D3D12와 같은 업로드 기록 구조). OpenGL은 context가 소유 스레드에 묶여
    // false다.
    bool concurrentResourceCreation = false;
    // GPU timestamp를 기록할 수 있는지와 초당 tick 수다(GX9). false면 WriteTimestamp가
    // Unsupported이며 GPU 시간은 "없음"이다(추정값을 만들지 않는다). D3D12는 graphics queue
    // timestamp, OpenGL ES는 GL_EXT_disjoint_timer_query(Windows 미리보기의 데스크톱 core context는
    // GL 3.3 timer query)이고 확장이 없거나 counter bit가 0이면 false다. Vulkan은 graphics queue
    // family의 timestampValidBits가 0이 아니고 timestampPeriod가 양수일 때다(주파수는 1e9 /
    // timestampPeriod를 정수 Hz로 반올림).
    bool timestamps = false;
    uint64 timestampFrequency = 0;
    // timestamp 구간 끊김(disjoint)을 알아챌 수 있는지다. false면 TimestampSample::disjoint는 제출
    // 실패 때만 true다. D3D12·Vulkan은 queue timestamp가 끊기지 않아 해당 없음(false), OpenGL은
    // GL_EXT_disjoint_timer_query가 있을 때만 true, D3D11은 프레임마다 TIMESTAMP_DISJOINT로 감싸
    // true다(주파수가 공개값과 달라져도 disjoint로 낸다).
    bool timestampDisjointDetection = false;
    // GPU·CPU 시각 쌍(CalibrateClocks)을 얻을 수 있는지다. D3D12는 queue clock calibration,
    // OpenGL은 GL_TIMESTAMP 조회와 CPU 시각을 잇달아 읽는다(조회 지연만큼 오차). D3D11은 제출한 GPU
    // 작업이 끝나기를 기다린 뒤 timestamp 하나를 CPU 시각 둘로 감싼다(호출이 비싸다). Vulkan은
    // calibrated timestamps 확장이 장치 domain과 PlatformBase 시계 domain(QPC·CLOCK_MONOTONIC)을 줄
    // 때만 참이다.
    bool clockCalibration = false;
    // debug marker(PushMarker·PopMarker)가 캡처 도구에 기록되는지다. false면 두 호출은 아무것도
    // 하지 않는다. D3D12는 command list 이벤트(PIX), D3D11은 ID3DUserDefinedAnnotation, Vulkan은
    // VK_EXT_debug_utils label, OpenGL은 debug group이다.
    bool debugMarkers = false;
    // API 판 표시다(진단·로그용). D3D는 장치 feature level(예: 0xc100 = 12_1), Vulkan은 장치
    // apiVersion(VK_MAKE_API_VERSION), OpenGL은 major × 100 + minor × 10(예: 320)이다.
    uint32 apiVersion = 0;
};

} // namespace rocks::graphics
