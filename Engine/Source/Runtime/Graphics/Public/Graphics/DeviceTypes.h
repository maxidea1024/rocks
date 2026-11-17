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

// 그래픽 backend다. D3D12·D3D11·OpenGL은 구현 모듈(D3D12Graphics·D3D11Graphics·OpenGLGraphics)이
// 있고, Vulkan·Metal은 자리만 둔다(구현 모듈이 아직 없어 선택하면 Unsupported).
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

// 장치 생성 설정이다.
struct DeviceDesc {
    // API 검증 계층(D3D12·D3D11 debug layer, GL debug context)을 켠다. 없으면 켜지 않고 계속한다.
    bool enableValidation = false;
    // D3D12·D3D11: 하드웨어 대신 소프트웨어 장치(WARP)를 쓴다. 헤드리스 CI 검증용이다. OpenGL은
    // 무시한다.
    bool preferSoftware = false;
    // 업로드 ring 크기다(0이면 DefaultUploadRingBytes). 생성 초기 데이터·내용 갱신이 이 ring을
    // 거친다.
    uint64 uploadRingBytes = 0;
    // 스트리밍 staging pool 예산이다(0이면 DefaultStagingBudgetBytes).
    uint64 stagingBudgetBytes = 0;
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
// level) × MipExtent(height, level) 픽셀(블록 형식은 블록 행 단위), 첫 행이 위, 행 사이 padding이
// 없다(MipChainBytes 크기). 장치가 대응하지 않는 형식은 생성이 Unsupported다(PixelFormat.ko.md
// 8.3절).
struct TextureDesc {
    uint32 width = 0;
    uint32 height = 0;
    PixelFormat format = PixelFormat::RGBA8UnormSrgb;
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

// 그리기 파이프라인 설정이다. 모든 파이프라인은 같은 고정 자원 배치를 쓴다: 그리기 상수
// (DrawConstants, HLSL b0 / GLSL ES 루트 상수 에뮬레이션 배열 uniform highp vec4 vu_h[5]·pu_h[5])와
// 텍스처 하나(HLSL t0·s0 / GLSL ES layout(binding = 0) sampler2D ps0). 샘플러는 묶은 텍스처의
// TextureDesc::sampler를 따른다. 깊이는 쓰지 않는다.
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
    PixelFormat colorFormat = PixelFormat::RGBA8UnormSrgb;
    // 소스 알파로 섞는다(premultiplied가 아닌 일반 알파).
    bool alphaBlend = false;
    // 버릴 면이다. 기본은 None이다(UE TStaticRasterizerState 기본 CM_None과 같고, 감김 순서를
    // 정하지 않은 화면 채우기·시험 그리기가 그대로 보인다). 메시 그리기는 Back을 쓰고, 물체·view
    // 행렬식이 음수면 Front 파이프라인으로 바꾼다(ShaderSystem 7.2절 8항).
    CullMode cullMode = CullMode::None;
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
    // GPU가 쉴 때 넣은 timestamp의 제출 전 CPU 시각과 완료를 본 CPU 시각 차의 절반이다.
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
    // OpenGL은 context가 소유 스레드에 묶여 false다.
    bool concurrentResourceCreation = false;
    // GPU timestamp를 기록할 수 있는지와 초당 tick 수다(GX9). false면 WriteTimestamp가
    // Unsupported이며 GPU 시간은 "없음"이다(추정값을 만들지 않는다). D3D12는 graphics queue
    // timestamp, OpenGL ES는 GL_EXT_disjoint_timer_query(Windows 미리보기의 데스크톱 core context는
    // GL 3.3 timer query)이고 확장이 없거나 counter bit가 0이면 false다.
    bool timestamps = false;
    uint64 timestampFrequency = 0;
    // timestamp 구간 끊김(disjoint)을 알아챌 수 있는지다. false면 TimestampSample::disjoint는 제출
    // 실패 때만 true다. D3D12는 queue timestamp가 끊기지 않아 해당 없음(false), OpenGL은
    // GL_EXT_disjoint_timer_query가 있을 때만 true, D3D11은 프레임마다 TIMESTAMP_DISJOINT로 감싸
    // true다(주파수가 공개값과 달라져도 disjoint로 낸다).
    bool timestampDisjointDetection = false;
    // GPU·CPU 시각 쌍(CalibrateClocks)을 얻을 수 있는지다. D3D12는 queue clock calibration,
    // OpenGL은 GL_TIMESTAMP 조회와 CPU 시각을 잇달아 읽는다(조회 지연만큼 오차). D3D11은 제출한 GPU
    // 작업이 끝나기를 기다린 뒤 timestamp 하나를 CPU 시각 둘로 감싼다(호출이 비싸다).
    bool clockCalibration = false;
    // debug marker(PushMarker·PopMarker)가 캡처 도구에 기록되는지다. false면 두 호출은 아무것도
    // 하지 않는다. D3D12는 command list 이벤트(PIX), D3D11은 ID3DUserDefinedAnnotation, OpenGL은
    // debug group이다.
    bool debugMarkers = false;
};

} // namespace rocks::graphics
