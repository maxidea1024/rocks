#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Graphics/Device.h>
#include <Platform/Atomics.h>
#include <RenderBase/RenderHandle.h>
#include <RenderBase/ShaderSet.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderParameterTypes.h>

// 내장 자원 팩이다(Docs/Design/RenderBase 6.3절·11.1절, 11.4절 5·6항, 계획 9.6절 Builtin Resource
// Pack). 렌더 lane 시작 단계(RenderLaneProcessor::OnLaneStart)에서 만들고, 반환 뒤 RenderLane이
// 제출 fence를 기다려 준비 완료(BuiltinReady)를 게시한다. 하나라도 실패하면 만든 것을 모두
// Retire하고 실패를 돌려준다 — 준비 완료가 게시되지 않고 시작이 실패한다(UE 전역 자원과 달리 정적
// 초기화와 준비 gate 없는 경로에 기대지 않는다).
//
// 1차 내용(11.4절 5항): 1×1 텍스처(2D white·black·transparent·flat normal, 차원별 대체 2D 배열·3D·
// cube·cube array·uint 2D·깊이), typed 버퍼(0·1·uint 0·identity affine), 기본 메시 cube·plane(엔진
// 기본 정점 입력 타입 BuiltinVertexInput), fallback 셰이더(엔진 셰이더 집합의 BuiltinMeshVertex·
// BuiltinMeshPixel·FullscreenTriangleVertex·CopyTexturePixel).
//
// 사용 예(렌더 lane 처리기):
//   Result<void> OnLaneStart(render::RenderLaneContext& context) noexcept override {
//       return builtins_.Create(context.Device(), engineShaders_);
//   }
//   void OnLaneStop(render::RenderLaneContext& context) noexcept override {
//       builtins_.Retire(context.Device());
//   }
//   // 그리기: 준비 전·실패한 텍스처 handle은 typed 대체로 그린다(6.2절).
//   auto fallback = builtins_.FallbackTexture(shader::ShaderResourceShape::Texture2D,
//                                             shader::ShaderComponentType::Float);
//   auto texture = builtins_.ResolveTexture(handles, item.texture, fallback.Value());
//
// 스레드: Create·Retire는 장치 소유 스레드(렌더 lane)에서 부른다. 만든 뒤의 조회(Texture·Buffer·
// Mesh·FallbackTexture·ResolveTexture)는 어느 스레드에서나 부를 수 있다(대체 계수는 원자).
namespace rocks::render {

// 팩 텍스처다(1×1, 선형, 샘플러 point·wrap).
enum class BuiltinTexture : uint8 {
    // RGBA8Unorm (1,1,1,1)
    White2D,
    // RGBA8Unorm (0,0,0,1)
    Black2D,
    // RGBA8Unorm (0,0,0,0)
    Transparent2D,
    // RGBA8Unorm (128,128,255,255) — unorm n·0.5+0.5 인코딩의 tangent 공간 (0,0,1)
    FlatNormal2D,
    // 층 1, (1,1,1,1)
    White2DArray,
    // 1×1×1, (1,1,1,1)
    White3D,
    // 면 6, (0,0,0,1)
    BlackCube,
    // cube 1, (0,0,0,0) — 장치에 DeviceFeatures::textureCubeArray가 있을 때만(없으면 null)
    BlackCubeArray,
    // R32Uint 0
    ZeroUint2D,
    // D32Float 0(reversed-Z 원평면) — 깊이 SRV 대체
    FarDepth2D
};

inline constexpr uint32 BuiltinTextureCount = 10;

// 팩 버퍼다(typed SRV view 함께).
enum class BuiltinBuffer : uint8 {
    // RGBA32Float 원소 1, (0,0,0,0)
    ZeroFloat4,
    // RGBA32Float 원소 1, (1,1,1,1)
    OneFloat4,
    // R32Uint 원소 1, 0
    ZeroUint,
    // RGBA32Float 원소 3 — 행 벡터 4×3 affine 행렬 하나의 열 셋 (1,0,0,0)·(0,1,0,0)·(0,0,1,0)
    // (결정 R18의 본 행렬 배치)
    IdentityAffine
};

inline constexpr uint32 BuiltinBufferCount = 4;

// 팩 메시다(BuiltinVertexInput 배치, UInt16 index, 감김은 바깥에서 보아 화면 반시계).
enum class BuiltinMesh : uint8 {
    // 한 변 100 cm, 원점 중심, 면마다 정점 4(UE /Engine/BasicShapes/Cube와 같은 크기)
    Cube,
    // 100 × 100 cm, XY 평면, 법선 +Z(UE /Engine/BasicShapes/Plane과 같은 크기·축)
    Plane
};

inline constexpr uint32 BuiltinMeshCount = 2;

// 엔진 기본 정점 입력 타입의 정점 하나다(10.6절 3항 — stride 36 byte).
struct BuiltinVertex {
    float position[3] = {};
    float texCoord[2] = {};
    float color[4] = {};
};

static_assert(sizeof(BuiltinVertex) == 36, "BuiltinVertex must match BuiltinVertexLayout");

// BuiltinVertexInput의 정점 배치다(stream 0 stride 36: ATTRIBUTE0 Float3 0, ATTRIBUTE1 Float2 12,
// ATTRIBUTE2 Float4 20). 가리키는 배열은 정적 수명이다.
ROCKS_RENDERBASE_API graphics::VertexLayout BuiltinVertexLayout() noexcept;

// 기본 메시의 CPU 데이터다(정적 수명).
struct BuiltinMeshData {
    Span<const BuiltinVertex> vertices;
    Span<const uint16> indices;
};

ROCKS_RENDERBASE_API BuiltinMeshData GetBuiltinMeshData(BuiltinMesh mesh) noexcept;

// 장치에 만든 기본 메시다.
struct BuiltinMeshBuffers {
    graphics::BufferHandle vertices;
    graphics::BufferHandle indices;
    graphics::IndexFormat indexFormat = graphics::IndexFormat::UInt16;
    uint32 vertexCount = 0;
    uint32 indexCount = 0;
};

// fallback 메시 셰이더의 그리기별 상수다(80 byte, 생성 HLSL
// /Engine/Generated/RenderBase/BuiltinMeshConstants.rsh — 셰이더는 Draw.<멤버>,
// ShaderSystem 16.2절). 변환은 행 벡터 p·M(행 우선)이다.
ROCKS_SHADER_PARAMETERS(Name = "Draw", RootConstants)
struct alignas(16) BuiltinMeshConstants {
    shader::ShaderFloat4x4 localToClip;
    shader::ShaderFloat4 color = {1.0f, 1.0f, 1.0f, 1.0f};
};

// fallback 셰이더 항목(엔진 셰이더 집합의 항목 번호)이다.
struct BuiltinShaderEntries {
    uint32 meshVertex = NoShaderSetEntry;
    uint32 meshPixel = NoShaderSetEntry;
    uint32 fullscreenVertex = NoShaderSetEntry;
    uint32 copyPixel = NoShaderSetEntry;
};

class ROCKS_RENDERBASE_API BuiltinResources final {
public:
    BuiltinResources() noexcept = default;
    // 장치를 모르므로 Retire하지 않는다. 주인이 종료 단계에서 Retire한다(남은 자원은 장치 closure
    // 확인이 잡는다).
    ~BuiltinResources() noexcept = default;
    BuiltinResources(const BuiltinResources&) = delete;
    BuiltinResources& operator=(const BuiltinResources&) = delete;

    // 팩을 만든다(11.4절 5항). engineShaders는 Ready인 엔진 셰이더 집합이며 팩보다 오래 산다. 실패:
    // 이미 만듦 Busy(장치 세대마다 한 번 — 재생성은 새 팩), 집합이 Ready가 아님 NotReady, fallback
    // 셰이더 항목이 없음 NotFound, 장치 자원·셰이더 생성 실패는 그 오류. 실패하면 만든 것을 모두
    // Retire하고 LastFailure에 영어 이유(자원 이름)를 둔다.
    Result<void> Create(graphics::Device& device, ShaderSet& engineShaders) noexcept;

    // 모든 자원을 장치에 Retire한다(종료 4단계 — 마지막 사용 fence 뒤 해제). 만들지 않았으면 아무
    // 것도 하지 않는다. 다시 Create할 수 있다.
    void Retire(graphics::Device& device) noexcept;

    bool IsReady() const noexcept {
        return ready_;
    }

    // 자원이다(준비 전이나 능력 없음이면 null).
    graphics::TextureHandle Texture(BuiltinTexture texture) const noexcept;
    graphics::BufferHandle Buffer(BuiltinBuffer buffer) const noexcept;
    graphics::BufferViewHandle BufferView(BuiltinBuffer buffer) const noexcept;
    const BuiltinMeshBuffers& Mesh(BuiltinMesh mesh) const noexcept;

    // fallback 셰이더 항목이다(준비 전이면 모두 NoShaderSetEntry).
    const BuiltinShaderEntries& ShaderEntries() const noexcept {
        return entries_;
    }

    // 셰이더 자원 모양(값 1 v3)과 원소 계열의 typed 대체 텍스처다(11.4절 6항): float 2D White2D,
    // 2D 배열 White2DArray, 3D White3D, cube BlackCube, cube array BlackCubeArray, uint 2D
    // ZeroUint2D. 실패: 준비 전 NotReady, 맞는 대체가 없는 조합(MSAA·buffer·1D 모양, uint·int의 2D
    // 밖, cube array 능력 없음) Unsupported — 2D white를 다른 모양 slot에 넣지 않는다(BR02).
    Result<graphics::TextureHandle>
    FallbackTexture(shader::ShaderResourceShape shape,
                    shader::ShaderComponentType component) const noexcept;

    // 렌더 handle의 장치 텍스처를 그리기용으로 푼다(6.2절): Ready면 그 텍스처, Pending·Failed면
    // fallback을 돌려주고 FallbackUses를 올린다. 실패: Retired·모르는 handle StaleHandle(대체하지
    // 않는다), 텍스처가 아닌 handle TypeMismatch, fallback이 null InvalidArgument.
    Result<graphics::TextureHandle> ResolveTexture(const RenderHandleTable& handles,
                                                   RenderHandle handle,
                                                   graphics::TextureHandle fallback) noexcept;

    // ResolveTexture가 대체를 돌려준 누적 수다(진단 계수).
    uint64 FallbackUses() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&fallbackUses_);
    }

    // 마지막 Create 실패의 영어 이유다(정적 수명, 없으면 빈 문자열).
    const char* LastFailure() const noexcept {
        return failure_;
    }

private:
    Result<void> Fail(graphics::Device& device, ErrorCode code, const char* reason) noexcept;
    Result<void> CreateTextures(graphics::Device& device) noexcept;
    Result<void> CreateBuffers(graphics::Device& device) noexcept;
    Result<void> CreateMeshes(graphics::Device& device) noexcept;
    Result<void> PrepareShaders(ShaderSet& engineShaders) noexcept;

    graphics::TextureHandle textures_[BuiltinTextureCount];
    graphics::BufferHandle buffers_[BuiltinBufferCount];
    graphics::BufferViewHandle views_[BuiltinBufferCount];
    BuiltinMeshBuffers meshes_[BuiltinMeshCount];
    BuiltinShaderEntries entries_;
    // 원자로 읽고 쓴다(platform::AtomicFetchAdd).
    uint64 fallbackUses_ = 0;
    const char* failure_ = "";
    bool created_ = false;
    bool ready_ = false;
};

} // namespace rocks::render
