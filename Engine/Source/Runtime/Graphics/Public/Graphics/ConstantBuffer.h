#pragma once

#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Containers/SlotArray.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Graphics/ShaderCode.h>

// 상수 버퍼(셰이더 cbuffer)의 장치 무관 계약이다(Docs/Design/RenderBase 9.11절 GX6, UE
// FRHIUniformBuffer·EUniformBufferUsage 대응). 상수 버퍼는 셰이더 바인딩 layout(값 1)의 상수 버퍼
// slot에 묶는다. 그리기별 상수(루트 상수, PipelineDesc::drawConstantBytes)는 이 계약 밖이다.
namespace rocks::graphics {

// 상수 버퍼 handle의 수명이다(9.11절 1항). UE의 SingleDraw는 handle 없는 기록 명령
// CommandContext::SetConstantData다.
enum class ConstantBufferLifetime : uint8 {
    // 내용을 만들 때 정하고 바꾸지 않는다. 쓰는 쪽이 프레임 끝에 Retire한다(UE SingleFrame).
    SingleFrame,
    // Device::UpdateConstantBuffer로 바꿀 수 있고 여러 프레임 쓴다(UE MultiFrame).
    MultiFrame
};

// "SingleFrame"·"MultiFrame"이다. 범위 밖이면 "Unknown"이다.
ROCKS_GRAPHICS_API const char* ConstantBufferLifetimeName(ConstantBufferLifetime lifetime) noexcept;

// 상수 버퍼 handle이다(Device::CreateConstantBuffer). 발급한 장치 안에서만 의미가 있고 Retire 뒤
// stale이다.
struct ConstantBufferHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const ConstantBufferHandle&) const noexcept = default;
};

// 모든 backend가 상수 버퍼 자리를 잡는 단위(byte)다. D3D12 CBV 주소 규칙
// (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)이고 Vulkan minUniformBufferOffsetAlignment의
// 사양 최대값이다.
inline constexpr uint32 ConstantBufferAlignment = 256;
// 내용 크기 단위(vec4, ShaderSystem 7.2절 5항)다.
inline constexpr uint32 ConstantBufferGranularity = 16;
// 내용 최대 byte 수다(D3D 4096 vec4 = ShaderCore MaxShaderConstantBufferBytes).
inline constexpr uint32 MaxConstantBufferBytes = 65'536;
// 단계당 상수 버퍼 slot 수다(값 1 slot 0–13, 형식 정의 constantBuffers 최대인 D3D12 14).
inline constexpr uint32 MaxConstantBufferSlots = 14;
// 상수 버퍼를 묶는 단계 수(VS·PS·CS)다.
inline constexpr uint32 ConstantBufferStageCount = 3;
// D3D12·Vulkan handle 저장소 page 하나의 byte 수다(9.11절 2항).
inline constexpr uint64 ConstantPageBytes = 1ull << 20;
// 기록 명령(SetConstantData) ring의 기본 byte 수다(queue마다 하나).
inline constexpr uint64 DefaultConstantRingBytes = 4ull << 20;
// PS 진단 버퍼(ShaderSystem 7.2절 12항) 자리에 장치가 묶는 내부 버퍼의 byte 수다(9.11절 6항 —
// 읽기와 크기 결정은 엔진 셰이더 SL2).
inline constexpr uint32 PixelDebugBufferBytes = 4096;

// 내용 byte 수가 규칙(16 byte 배수, 16 – MaxConstantBufferBytes) 안인지다.
ROCKS_GRAPHICS_API bool IsValidConstantBufferBytes(usize bytes) noexcept;

// bytes를 ConstantBufferAlignment로 올린다.
constexpr uint64 AlignConstantBufferBytes(uint64 bytes) noexcept {
    return (bytes + ConstantBufferAlignment - 1) &
           ~static_cast<uint64>(ConstantBufferAlignment - 1);
}

// 상수 구조체를 CreateConstantBuffer·SetConstantData가 받는 byte view로 본다(결과 수명은
// constants와 같다). 크기 규칙과 trivially copyable을 컴파일 시간에 검사한다. 멤버 배치는 셰이더
// cbuffer와 같아야 한다(ShaderSystem 7.2절 5항 — 행렬은 행 우선, DrawConstantBytes 주석).
//
// 사용 예:
//   struct alignas(16) MaterialConstants { float color[4]; };   // cbuffer Material { float4 color;
//   } auto material = device.CreateConstantBuffer(ConstantBufferLifetime::SingleFrame,
//                                               ConstantBufferBytes(values));
template <typename T>
Span<const uint8> ConstantBufferBytes(const T& constants) noexcept {
    static_assert(IsTriviallyCopyableV<T>, "Constant buffer contents must be trivially copyable");
    static_assert(sizeof(T) % ConstantBufferGranularity == 0,
                  "Constant buffer contents must be a multiple of 16 bytes");
    static_assert(sizeof(T) <= MaxConstantBufferBytes,
                  "Constant buffer contents must not exceed 65536 bytes");
    return Span<const uint8>(reinterpret_cast<const uint8*>(&constants), sizeof(T));
}

// 단계의 묶음 표 색인(Vertex 0·Pixel 1·Compute 2)이다.
constexpr uint32 ConstantBufferStageIndex(ShaderStage stage) noexcept {
    return static_cast<uint32>(stage);
}

static_assert(static_cast<uint32>(ShaderStage::Compute) + 1 == ConstantBufferStageCount,
              "constant buffer stage table must cover every shader stage");

// 파이프라인이 읽는 상수 버퍼 배치다(단계마다 slot 마스크와 셰이더 선언 byte 수). 값 1로 채우고
// (ResolvePipelineSlots), 그리기 때 묶인 버퍼가 배치를 채우는지 본다(ConstantBuffersSatisfied).
struct ConstantBufferSlots {
    uint32 usedMask[ConstantBufferStageCount] = {};
    uint32 bytes[ConstantBufferStageCount][MaxConstantBufferSlots] = {};
};

// 단계·slot 하나를 더한다. slot이 범위 밖이거나 bytes가 규칙 밖이거나 이미 있으면 거짓이다.
ROCKS_GRAPHICS_API bool AddConstantBufferSlot(ConstantBufferSlots& slots, ShaderStage stage,
                                              uint32 slot, uint32 bytes) noexcept;

// 배치의 모든 (단계, slot)에 선언 크기 이상의 버퍼가 묶였는지다. bound는 묶인 버퍼의 byte 수이고
// 0은 없음이다. 아니면 backend는 그 그리기를 무시하고 IgnoredDraws를 올린다.
ROCKS_GRAPHICS_API bool ConstantBuffersSatisfied(
    const ConstantBufferSlots& slots,
    const uint32 (&bound)[ConstantBufferStageCount][MaxConstantBufferSlots]) noexcept;

// 배치가 읽는 상수 버퍼 수(모든 단계)다.
ROCKS_GRAPHICS_API uint32 ConstantBufferCount(const ConstantBufferSlots& slots) noexcept;

} // namespace rocks::graphics
