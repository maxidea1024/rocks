#pragma once

#include <Core/Containers/SlotArray.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <GraphicsTypes/PixelFormat.h>
#include <GraphicsTypes/Sampler.h>
#include <GraphicsTypes/VertexFormat.h>

// 그래픽 장치 계약의 값 타입(handle·장치·자원 설정)이다. backend와 무관하며 API 객체를 담지
// 않는다. 픽셀 형식·샘플러는 GraphicsTypes 모듈에 있다.
namespace rocks::graphics {

// 그래픽 backend다. D3D12·OpenGL은 구현 모듈(D3D12Graphics·OpenGLGraphics)이 있고, Vulkan·Metal은
// 자리만 둔다(구현 모듈이 아직 없어 선택하면 Unsupported).
enum class Backend : uint8 {
    D3D12,
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

// 장치 생성 설정이다.
struct DeviceDesc {
    // API 검증 계층(D3D12 debug layer, GL debug context)을 켠다. 없으면 켜지 않고 계속한다.
    bool enableValidation = false;
    // D3D12: 하드웨어 대신 소프트웨어 장치(WARP)를 쓴다. 헤드리스 CI 검증용이다. OpenGL은 무시한다.
    bool preferSoftware = false;
    // 업로드 ring 크기다(0이면 DefaultUploadRingBytes). 생성 초기 데이터·내용 갱신이 이 ring을
    // 거친다.
    uint64 uploadRingBytes = 0;
    // 스트리밍 staging pool 예산이다(0이면 DefaultStagingBudgetBytes).
    uint64 stagingBudgetBytes = 0;
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

// 버퍼 용도다. 정점 버퍼는 SetVertexBuffers, index 버퍼는 SetIndexBuffer로 묶는다.
enum class BufferUsage : uint8 {
    Vertex,
    Index
};

struct BufferDesc {
    usize size = 0;
    BufferUsage usage = BufferUsage::Vertex;
};

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
    // 셰이더에서 샘플링한다(초기 픽셀 필요).
    Sampled,
    // 렌더 타깃으로 그리고 ReadPixels로 읽거나 샘플링한다(초기 픽셀 없음).
    RenderTarget
};

// 텍스처 설정이다.
//
// 초기 픽셀 배치(Sampled): mip 0부터 mipLevels개를 빈틈 없이 잇는다. 각 mip은 MipExtent(width,
// level) × MipExtent(height, level) 픽셀, 첫 행이 위, 행 사이 padding이 없다(MipChainBytes 크기).
struct TextureDesc {
    uint32 width = 0;
    uint32 height = 0;
    Format format = Format::RGBA8UnormSrgb;
    TextureUsage usage = TextureUsage::Sampled;
    // 1–FullMipCount(width, height)다. RenderTarget은 1이어야 한다.
    uint32 mipLevels = 1;
    // 이 텍스처를 묶을 때 쓰는 샘플러다.
    SamplerDesc sampler;
};

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

// 정점 속성 하나다. location(0–15)은 HLSL 시맨틱 ATTRIBUTE{location}, GLSL layout(location)이다.
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
// GLSL에서 main이다.
struct ShaderSource {
    StringView hlsl;
    StringView glsl;
};

// 그리기 파이프라인 설정이다. 모든 파이프라인은 같은 고정 자원 배치를 쓴다: 그리기 상수
// (DrawConstants, HLSL b0 / GLSL uniform uTransform·uColor)와 텍스처 하나(HLSL t0·s0 /
// GLSL binding 0 uTexture). 샘플러는 묶은 텍스처의 TextureDesc::sampler를 따른다. 깊이·컬링은
// 쓰지 않는다.
//
// 셰이더 입력 대조(RenderBase 10.4절): 정점 셰이더가 읽는 모든 location이 layout에 있어야 하고
// 형식의 셰이더 타입 계열(VertexFormatShaderType)이 셰이더 입력 타입과 같아야 한다. 셰이더가 읽지
// 않는 속성은 허용한다. 입력은 D3D12가 컴파일한 VS 입력 signature(시맨틱 ATTRIBUTEn), OpenGL이 링크
// 뒤 active attribute에서 얻는다. 위반은 InvalidArgument이며 LastError에 location·형식·셰이더
// 타입을 적는다. 정점 입력 시맨틱은 ATTRIBUTEn만 쓴다(SV_ 시스템 값 제외).
struct PipelineDesc {
    ShaderSource vertex;
    ShaderSource pixel;
    VertexLayout layout;
    Format colorFormat = Format::RGBA8UnormSrgb;
    // 소스 알파로 섞는다(premultiplied가 아닌 일반 알파).
    bool alphaBlend = false;
};

// 그리기마다 넘기는 상수다. transform은 열 우선(column-major) 4x4 행렬(Rocks Mat4 저장 순서
// 그대로), color는 선형 RGBA다. 셰이더는 mul(transform, position)으로 쓴다.
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
    // 스레드의 다음 제출이 실행한다(GX8). OpenGL은 context가 소유 스레드에 묶여 false다.
    bool concurrentResourceCreation = false;
};

} // namespace rocks::graphics
