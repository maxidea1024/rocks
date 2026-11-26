#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Hash.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <ShaderCore/ShaderReadStatus.h>

// 셰이더 파라미터 레이아웃 표와 그 서명이다(Docs/Design/ShaderSystem 7.2절 5항, 12.2.1절 SSD27,
// 13절 P7, 16.2절 SSD53).
//
// 표의 출처는 셋이다: MetaCompiler가 ROCKS_SHADER_PARAMETERS 구조체에서 만든 생성 표
// (ShaderParameterLayoutOf<T>() — 구조체 identity와 생성 HLSL 원문을 함께 가진다, SH3), 손으로 쓴
// 표(C++ 구조체와 static_assert로 맞춘다 — 시험·도구), 머티리얼 동적 layout(실행 중 데이터). 셰이더
// 타입은 표 목록을 가지며(ShaderTypeDesc::parameterLayouts) 등록부가 Validate한다.
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
// 파라미터 구조체 C++ 정규 이름("rocks::render::TonemapConstants")의 최대 byte 수다.
inline constexpr usize MaxShaderParameterStructNameBytes = 128;
// 생성 HLSL 원문 하나의 최대 byte 수다.
inline constexpr usize MaxShaderParameterSourceBytes = 64 * 1024;
// 생성 HLSL 원문 가상 경로의 최대 byte 수다(ShaderVirtualPath의 상한보다 작다).
inline constexpr usize MaxShaderParameterSourcePathBytes = 200;
// 생성 HLSL 원문이 놓이는 가상 디렉터리 이름이다(<루트>/Generated/…, 16.2절 SSD52).
inline constexpr char ShaderParameterGeneratedDirectory[] = "Generated";

// 파라미터 구조체 identity다(RenderBase SX8 — C++ 정규 이름의 64-bit 해시, 대소문자 구분). 빈
// 이름은 0(identity 없음)이다.
constexpr uint64 ShaderParameterStructId(StringView structName) noexcept {
    return structName.IsEmpty() ? 0 : HashStringBytes(structName);
}

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
    Float4x4 = 12,
    // 다른 파라미터 구조체(중첩 — 1단, ShaderParameterMember::structLayout, 16.2절 SSD53)다.
    Struct = 13
};

// 멤버 기본 타입(GLES 에뮬레이션 배열 h·i·u — 7.2절 4항)이다.
enum class ShaderParameterBaseType : uint8 {
    Float,
    Int,
    Uint
};

struct ShaderParameterLayout;

// 구조체 멤버 하나다. arrayCount 0은 배열이 아님이다. type이 Struct면 structLayout이 중첩 표다(그
// 표의 멤버 오프셋은 이 구조체 기준이다).
struct ShaderParameterMember {
    constexpr ShaderParameterMember() noexcept = default;

    constexpr ShaderParameterMember(StringView memberName, ShaderParameterType memberType,
                                    uint32 memberOffset, uint32 memberArrayCount,
                                    const ShaderParameterLayout* nestedLayout = nullptr) noexcept
        : name(memberName), type(memberType), offset(memberOffset), arrayCount(memberArrayCount),
          structLayout(nestedLayout) {
    }

    StringView name;
    ShaderParameterType type = ShaderParameterType::Float;
    uint32 offset = 0;
    uint32 arrayCount = 0;
    const ShaderParameterLayout* structLayout = nullptr;
};

// 상수 버퍼·루트 상수 구조체 하나의 표다(읽기 전용 값 — 가리키는 목록은 정적 수명이어야 한다).
// 중첩 구조체 표의 name은 HLSL 구조체 타입 이름이다.
struct ShaderParameterLayout {
    constexpr ShaderParameterLayout() noexcept = default;

    // 손으로 쓴 표·동적 layout(identity·원문 없음)이다.
    constexpr ShaderParameterLayout(StringView layoutName, bool root, uint32 bytes,
                                    Span<const ShaderParameterMember> layoutMembers) noexcept
        : name(layoutName), rootConstants(root), totalBytes(bytes), members(layoutMembers) {
    }

    // 생성 표(MetaCompiler)다. identity는 이름에서 계산한다.
    constexpr ShaderParameterLayout(StringView layoutName, bool root, uint32 bytes,
                                    Span<const ShaderParameterMember> layoutMembers,
                                    StringView qualifiedName, StringView path,
                                    StringView text) noexcept
        : name(layoutName), rootConstants(root), totalBytes(bytes), members(layoutMembers),
          structName(qualifiedName), structId(ShaderParameterStructId(qualifiedName)),
          sourcePath(path), source(text) {
    }

    // 원문의 cbuffer·ROCKS_ROOT_CONSTANTS 이름이다(식별자).
    StringView name;
    // 루트 상수(그리기별 상수, 7.2절 2항)인가.
    bool rootConstants = false;
    // 구조체 전체 byte 수(16 byte 배수)다.
    uint32 totalBytes = 0;
    // 멤버 표(오프셋 오름차순)다.
    Span<const ShaderParameterMember> members;
    // 생성 표의 C++ 정규 이름과 그 identity(ShaderParameterStructId)다. 손으로 쓴 표·동적
    // layout은 빈 이름·0이다.
    StringView structName;
    uint64 structId = 0;
    // 생성 HLSL 원문의 가상 경로(/Engine/Generated/<모듈>/<이름>.rsh)와 원문이다. 컴파일 관리자가
    // job 전처리의 메모리 원문으로 준다(16.2절 SSD52). 없으면 빈 값이다.
    StringView sourcePath;
    StringView source;
};

// 생성 표 특성이다. MetaCompiler 생성 헤더(<헤더>.shaderparams.h)가 ROCKS_SHADER_PARAMETERS
// 구조체마다 특수화해 `static constexpr ShaderParameterLayout Layout`을 둔다. 특수화가 없는 타입을
// ShaderParameterLayoutOf에 쓰면 컴파일 오류다(생성 헤더를 include하지 않았다).
//
// 사용 예:
//   #include <RenderBase/EngineShaderTypes.shaderparams.h>
//   constexpr ShaderParameterLayout Layouts[] = {ShaderParameterLayoutOf<TonemapConstants>()};
template <class T>
struct ShaderParameterStruct;

template <class T>
constexpr const ShaderParameterLayout& ShaderParameterLayoutOf() noexcept {
    return ShaderParameterStruct<T>::Layout;
}

// 타입 하나의 byte 수(배열 아님)다. 범위 밖이면 0이다.
ROCKS_SHADERCORE_API uint32 ShaderParameterTypeBytes(ShaderParameterType type) noexcept;

// 타입의 정렬(스칼라 4·2성분 8·3·4성분과 행렬 16)이다. 범위 밖이면 0이다.
ROCKS_SHADERCORE_API uint32 ShaderParameterTypeAlignment(ShaderParameterType type) noexcept;

// 타입의 기본 타입이다(float4x4는 Float).
ROCKS_SHADERCORE_API ShaderParameterBaseType
ShaderParameterTypeBase(ShaderParameterType type) noexcept;

// 타입 이름("float"·"int3"·"float4x4" 등 HLSL 철자, 정적 수명)이다. 범위 밖이면 "unknown".
ROCKS_SHADERCORE_API const char* ShaderParameterTypeName(ShaderParameterType type) noexcept;

// 멤버 하나의 byte 수다(배열이면 원소 크기 × 원소 수 — 원소 크기는 16 byte 배수여야 한다). Struct는
// 중첩 표의 totalBytes(표가 없으면 0)다.
ROCKS_SHADERCORE_API uint64
ShaderParameterMemberBytes(const ShaderParameterMember& member) noexcept;

// 표 하나를 7.2절 5항 규칙으로 검사한다. 오류(InvalidArgument — 진단은 reason에 영어 정적 문구):
// 이름이 식별자가 아님, 멤버 수 0 또는 MaxShaderParameterMembers 초과, 타입이 범위 밖, 멤버 정렬
// 위반, 배열 원소 크기가 16 byte 배수가 아님, 멤버가 오프셋 오름차순이 아니거나 겹침, 16 byte 슬롯
// 하나에 기본 타입을 섞음, 멤버가 totalBytes를 넘음, totalBytes가 16 byte 배수가 아니거나 0이거나
// MaxShaderConstantBufferBytes 초과, 루트 상수인데 MaxShaderRootConstantBytes 초과, 멤버 이름 중복,
// Struct 멤버의 표 없음·2단 중첩·배열·16 byte 정렬 아님·중첩 표가 루트 상수, structName이 정규
// 이름(::로 이은 식별자, 128 byte 이하)이 아니거나 structId가 그 해시와 다름, 원문 경로가
// /<루트>/Generated/… 의 .rsh가 아니거나 원문만 있음·원문 크기 초과.
ROCKS_SHADERCORE_API Result<void>
ValidateShaderParameterLayout(const ShaderParameterLayout& layout,
                              const char** reason = nullptr) noexcept;

// 셰이더 타입 하나의 표 목록을 검사한다: 표마다 ValidateShaderParameterLayout, 목록 길이
// MaxShaderParameterLayouts 이하, 구조체 이름 중복 없음, 루트 상수 표는 하나 이하(7.2절 2항).
ROCKS_SHADERCORE_API Result<void>
ValidateShaderParameterLayouts(Span<const ShaderParameterLayout> layouts,
                               const char** reason = nullptr) noexcept;

// 표 목록의 서명이다(SSD27·SSD53). 목록이 비면 0(모든 byte 0)이다. 아니면 정규 byte의 SHA-256이다:
//   u32 표 수, 표마다 표 byte. 표 byte = [u8 이름 byte 수, 이름 byte, u8 구조체 이름 byte 수, 구조체
//   이름 byte, u8 루트 상수(0·1), u32 totalBytes, u32 멤버 수, 멤버마다 (u8 이름 byte 수, 이름 byte,
//   u8 타입 값, u32 오프셋, u32 원소 수, Struct면 중첩 표의 표 byte)] — 모두 little-endian, 목록
//   순서 그대로. identity는 이름에서 다시 계산되므로 넣지 않고, 생성 원문과 그 경로도 넣지 않는다
//   (원문은 include 폐포로 job 키에 들어간다).
// 목록은 ValidateShaderParameterLayouts를 통과해야 한다(이름 byte 수가 u8 안이다).
ROCKS_SHADERCORE_API Sha256Digest
ShaderParameterLayoutSignature(Span<const ShaderParameterLayout> layouts) noexcept;

// 표 목록의 정규 byte(위 서명의 입력과 같다 — Sha256(byte) = 서명)를 out 끝에 붙인다. 관리자가 워커
// job에 담아 보낸다(SSD31). 오류: 목록이 ValidateShaderParameterLayouts를 통과하지 못하면
// InvalidArgument(out은 바뀌지 않는다), 할당 실패.
ROCKS_SHADERCORE_API Result<void>
EncodeShaderParameterLayouts(Span<const ShaderParameterLayout> layouts, Array<uint8>& out) noexcept;

// 읽은 표 목록이다. 이름은 원본 byte를 가리키므로 원본이 이 값보다 오래 살아야 한다. 중첩 표는
// nested에 있고 Struct 멤버가 가리킨다(원문 칸은 정규 byte에 없어 빈 값이다).
struct ROCKS_SHADERCORE_API ShaderParameterLayoutSet {
    explicit ShaderParameterLayoutSet(Allocator& allocator) noexcept
        : layouts(allocator), members(allocator), nested(allocator) {
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
    Array<ShaderParameterLayout> nested;
};

// 정규 byte를 읽는다(빈 byte는 빈 목록). 잘림·남는 byte·타입 범위 밖·검사
// (ValidateShaderParameterLayouts) 위반은 CorruptPackage, 할당 실패도 CorruptPackage(이유가
// 따로)다.
ROCKS_SHADERCORE_API ShaderReadStatus
DecodeShaderParameterLayouts(Span<const uint8> bytes, ShaderParameterLayoutSet& out) noexcept;

} // namespace rocks::shader
