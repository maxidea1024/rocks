#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <ShaderCore/ShaderReadStatus.h>

// 손으로 쓴 셰이더 파라미터 레이아웃 표와 그 서명이다(Docs/Design/ShaderSystem 7.2절 5항,
// 12.2.1절 SSD27).
//
// MetaCompiler(13절 P7)가 생기기 전에는 셰이더 원문의 상수 버퍼·루트 상수 구조체마다 C++ 쪽에 이
// 표를 손으로 두고, C++ 구조체(ShaderParameterTypes.h의 정렬 타입으로 쓴다)와 static_assert로
// 맞춘다. 셰이더 타입은 표 목록을 가지며(ShaderTypeDesc::parameterLayouts) 등록부가 Validate한다.
// 목록의 서명(ShaderParameterLayoutSignature)이 job 키 8행(10절)과 라이브러리 항목의 레이아웃
// 서명(RenderBase SX6)이다. 워커는 표를 받아 리플렉션 오프셋과 대조한다(SH2b — 7.3절 (a)).
//
// 사용 예:
//   struct alignas(16) DrawConstants {
//       ShaderFloat4x4 transform;
//       ShaderFloat4 color;
//   };
//   constexpr ShaderParameterMember DrawConstantsMembers[] = {
//       {"transform", ShaderParameterType::Float4x4, offsetof(DrawConstants, transform), 0},
//       {"color", ShaderParameterType::Float4, offsetof(DrawConstants, color), 0}};
//   constexpr ShaderParameterLayout DrawConstantsLayout{"DrawConstants", true,
//                                                       sizeof(DrawConstants),
//                                                       DrawConstantsMembers};
namespace rocks::shader {

// 루트 상수(그리기별 상수) 구조체의 최대 byte 수다(32 DWORD, Vulkan push constant 최소 보장 —
// 7.2절 2항).
inline constexpr uint32 MaxShaderRootConstantBytes = 128;
// 상수 버퍼 슬롯·정렬 단위(vec4) byte 수다.
inline constexpr uint32 ShaderConstantSlotBytes = 16;
// 상수 버퍼 하나의 최대 byte 수다(D3D 4096 vec4 — 64 KiB).
inline constexpr uint32 MaxShaderConstantBufferBytes = 65'536;
// 구조체 하나의 최대 멤버 수다.
inline constexpr usize MaxShaderParameterMembers = 256;
// 셰이더 타입 하나의 최대 레이아웃 표 수다.
inline constexpr usize MaxShaderParameterLayouts = 16;
// 구조체·멤버 이름의 최대 byte 수(NUL 제외)다.
inline constexpr usize MaxShaderParameterNameBytes = 64;

// 허용 멤버 타입이다(7.2절 5항 — 4 byte 스칼라, 그 2–4성분 벡터, float4x4). 값은 정규 byte(서명)에
// 들어가므로 바꾸지 않는다.
enum class ShaderParameterType : uint8 {
    Float = 0,
    Float2 = 1,
    Float3 = 2,
    Float4 = 3,
    Int = 4,
    Int2 = 5,
    Int3 = 6,
    Int4 = 7,
    Uint = 8,
    Uint2 = 9,
    Uint3 = 10,
    Uint4 = 11,
    Float4x4 = 12
};

// 멤버 기본 타입(GLES 에뮬레이션 배열 h·i·u — 7.2절 4항)이다.
enum class ShaderParameterBaseType : uint8 {
    Float,
    Int,
    Uint
};

// 구조체 멤버 하나다. arrayCount 0은 배열이 아님이다.
struct ShaderParameterMember {
    StringView name;
    ShaderParameterType type = ShaderParameterType::Float;
    uint32 offset = 0;
    uint32 arrayCount = 0;
};

// 상수 버퍼·루트 상수 구조체 하나의 표다(읽기 전용 값 — 가리키는 목록은 정적 수명이어야 한다).
struct ShaderParameterLayout {
    // 원문의 cbuffer·ROCKS_ROOT_CONSTANTS 이름이다(식별자).
    StringView name;
    // 루트 상수(그리기별 상수, 7.2절 2항)인가.
    bool rootConstants = false;
    // 구조체 전체 byte 수(16 byte 배수)다.
    uint32 totalBytes = 0;
    // 멤버 표(오프셋 오름차순)다.
    Span<const ShaderParameterMember> members;
};

// 타입 하나의 byte 수(배열 아님)다. 범위 밖이면 0이다.
ROCKS_SHADERCORE_API uint32 ShaderParameterTypeBytes(ShaderParameterType type) noexcept;

// 타입의 정렬(스칼라 4·2성분 8·3·4성분과 행렬 16)이다. 범위 밖이면 0이다.
ROCKS_SHADERCORE_API uint32 ShaderParameterTypeAlignment(ShaderParameterType type) noexcept;

// 타입의 기본 타입이다(float4x4는 Float).
ROCKS_SHADERCORE_API ShaderParameterBaseType
ShaderParameterTypeBase(ShaderParameterType type) noexcept;

// 타입 이름("float"·"int3"·"float4x4" 등 HLSL 철자, 정적 수명)이다. 범위 밖이면 "unknown".
ROCKS_SHADERCORE_API const char* ShaderParameterTypeName(ShaderParameterType type) noexcept;

// 멤버 하나의 byte 수다(배열이면 원소 크기 × 원소 수 — 원소 크기는 16 byte 배수여야 한다).
ROCKS_SHADERCORE_API uint64
ShaderParameterMemberBytes(const ShaderParameterMember& member) noexcept;

// 표 하나를 7.2절 5항 규칙으로 검사한다. 오류(InvalidArgument — 진단은 reason에 영어 정적 문구):
// 이름이 식별자가 아님, 멤버 수 0 또는 MaxShaderParameterMembers 초과, 타입이 범위 밖, 멤버 정렬
// 위반, 배열 원소 크기가 16 byte 배수가 아님, 멤버가 오프셋 오름차순이 아니거나 겹침, 16 byte 슬롯
// 하나에 기본 타입을 섞음, 멤버가 totalBytes를 넘음, totalBytes가 16 byte 배수가 아니거나 0이거나
// MaxShaderConstantBufferBytes 초과, 루트 상수인데 MaxShaderRootConstantBytes 초과, 멤버 이름 중복.
ROCKS_SHADERCORE_API Result<void>
ValidateShaderParameterLayout(const ShaderParameterLayout& layout,
                              const char** reason = nullptr) noexcept;

// 셰이더 타입 하나의 표 목록을 검사한다: 표마다 ValidateShaderParameterLayout, 목록 길이
// MaxShaderParameterLayouts 이하, 구조체 이름 중복 없음, 루트 상수 표는 하나 이하(7.2절 2항).
ROCKS_SHADERCORE_API Result<void>
ValidateShaderParameterLayouts(Span<const ShaderParameterLayout> layouts,
                               const char** reason = nullptr) noexcept;

// 표 목록의 서명이다(SSD27). 목록이 비면 0(모든 byte 0)이다. 아니면 정규 byte의 SHA-256이다:
//   u32 표 수, 표마다 [u8 이름 byte 수, 이름 byte, u8 루트 상수(0·1), u32 totalBytes, u32 멤버 수,
//   멤버마다 (u8 이름 byte 수, 이름 byte, u8 타입 값, u32 오프셋, u32 원소 수)] — 모두
//   little-endian, 목록 순서 그대로.
// 목록은 ValidateShaderParameterLayouts를 통과해야 한다(이름 byte 수가 u8 안이다).
ROCKS_SHADERCORE_API Sha256Digest
ShaderParameterLayoutSignature(Span<const ShaderParameterLayout> layouts) noexcept;

// 표 목록의 정규 byte(위 서명의 입력과 같다 — Sha256(byte) = 서명)를 out 끝에 붙인다. 관리자가 워커
// job에 담아 보낸다(SSD31). 오류: 목록이 ValidateShaderParameterLayouts를 통과하지 못하면
// InvalidArgument(out은 바뀌지 않는다), 할당 실패.
ROCKS_SHADERCORE_API Result<void>
EncodeShaderParameterLayouts(Span<const ShaderParameterLayout> layouts, Array<uint8>& out) noexcept;

// 읽은 표 목록이다. 이름은 원본 byte를 가리키므로 원본이 이 값보다 오래 살아야 한다.
struct ROCKS_SHADERCORE_API ShaderParameterLayoutSet {
    explicit ShaderParameterLayoutSet(Allocator& allocator) noexcept
        : layouts(allocator), members(allocator) {
    }

    ShaderParameterLayoutSet(const ShaderParameterLayoutSet&) = delete;
    ShaderParameterLayoutSet& operator=(const ShaderParameterLayoutSet&) = delete;

    Span<const ShaderParameterLayout> View() const noexcept {
        return layouts.View();
    }

    // name의 표다. 없으면 null이다.
    const ShaderParameterLayout* Find(StringView name) const noexcept;

    Array<ShaderParameterLayout> layouts;
    Array<ShaderParameterMember> members;
};

// 정규 byte를 읽는다(빈 byte는 빈 목록). 잘림·남는 byte·타입 범위 밖·검사
// (ValidateShaderParameterLayouts) 위반은 CorruptPackage, 할당 실패도 CorruptPackage(이유가
// 따로)다.
ROCKS_SHADERCORE_API ShaderReadStatus
DecodeShaderParameterLayouts(Span<const uint8> bytes, ShaderParameterLayoutSet& out) noexcept;

} // namespace rocks::shader
