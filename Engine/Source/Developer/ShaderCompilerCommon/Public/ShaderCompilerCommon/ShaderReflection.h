#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderParameterLayout.h>

// 형식 처리기가 함께 쓰는 리플렉션 모델과 바인딩 규칙이다(Docs/Design/ShaderSystem 7.2절
// 1·2·4·5·7·9항, 16.1절 SSD29–SSD33).
//
// 형식 처리기는 도구 리플렉션(DXIL·DXBC ID3D*ShaderReflection, SPIR-V SPIRV-Reflect)을
// ShaderReflection 으로 옮기고 NormalizeShaderReflection이 형식과 무관한 규칙을 한 번에 검사한 뒤
// ShaderBindingLayout(값 1) 을 만든다. 규칙이 한 벌이라 형식마다 다른 판정이 나오지 않는다.
//
// 이름(StringView)은 도구 리플렉션이 가진 문자열을 가리킨다. 리플렉션 객체가 살아 있는 동안만 쓰고,
// 값 1 byte(EncodeShaderBindingLayout)로 옮긴 뒤 버린다.
//
// 사용 예(형식 처리기):
//   ShaderReflection reflection(allocator);
//   reflection.stage = job.stage;
//   ... 도구 리플렉션을 옮긴다 ...
//   ShaderBindingLayout layout(allocator);
//   String diagnostics(allocator);
//   if (!NormalizeShaderReflection(format, reflection, layouts, layout, diagnostics)) {
//       FailShaderJob(result, ShaderWorkerJobStatus::CompileError, diagnostics.View());
//   }
namespace rocks::shader {

// 멤버 기본 타입이다(리플렉션 철자 — 규칙이 허용하는 것은 Float·Int·Uint뿐이다).
enum class ShaderReflectedBase : uint8 {
    Float,
    Int,
    Uint,
    Bool,
    Double,
    // min16float·half 등 16 bit 타입이다.
    Half,
    // 64 bit 정수·그 밖이다.
    Other,
    // 구조체다.
    Struct
};

// 상수 버퍼 멤버 하나다. 구조체 멤버의 자식은 바로 뒤에 이어지고(parent가 그 구조체 색인), 오프셋은
// 부모(상수 버퍼 또는 구조체) 기준이다.
struct ShaderReflectedMember {
    StringView name;
    ShaderReflectedBase base = ShaderReflectedBase::Float;
    // 벡터는 rows 1·columns n, 스칼라는 1·1, 행렬은 rows·columns다.
    uint32 rows = 1;
    uint32 columns = 1;
    // 배열 원소 수(0 = 배열 아님)다.
    uint32 elements = 0;
    uint32 offset = 0;
    // 리플렉션이 준 byte 수(구조체만 쓴다)다.
    uint32 bytes = 0;
    // 부모 멤버 색인(상수 버퍼 바로 아래면 NoShaderReflectedParent)이다.
    uint32 parent = 0;
};

// 상수 버퍼 바로 아래 멤버라는 표지다.
inline constexpr uint32 NoShaderReflectedParent = NumericLimits<uint32>::Max();

// 자원 하나다.
struct ShaderReflectedResource {
    ShaderResourceKind kind = ShaderResourceKind::ConstantBuffer;
    StringView name;
    // backend 위치(D3D register, Vulkan 다시 매긴 binding — SSD30)다.
    uint32 slot = 0;
    // 배열 원소 수(1 = 배열 아님)다.
    uint32 count = 1;
    // 루트 상수 블록인가(SSD32 — 자원 표에 넣지 않는다).
    bool rootConstants = false;
    // 상수 버퍼 byte 수(리플렉션 값)다.
    uint32 constantBufferBytes = 0;
    // 멤버는 reflection.members[firstMember, firstMember + memberCount)다(구조체 자식 포함).
    uint32 firstMember = 0;
    uint32 memberCount = 0;
};

// 단계 입출력 원소 하나다. 시맨틱 이름은 끝 숫자를 뗀 것이다(TEXCOORD3 → TEXCOORD, 3).
struct ShaderReflectedSignature {
    StringView semantic;
    uint32 index = 0;
};

struct ShaderReflection {
    explicit ShaderReflection(Allocator& allocator) noexcept
        : resources(allocator), members(allocator), inputs(allocator), outputs(allocator) {
    }

    uint32 stage = 0;
    Array<ShaderReflectedResource> resources;
    Array<ShaderReflectedMember> members;
    Array<ShaderReflectedSignature> inputs;
    Array<ShaderReflectedSignature> outputs;
    // 이름 없는 전역 uniform($Globals)이 있었는가.
    bool hasGlobals = false;
};

// "TEXCOORD3"을 (TEXCOORD, 3)으로 나눈다(끝 숫자가 없으면 index 0).
ROCKS_SHADERCOMPILERCOMMON_API ShaderReflectedSignature
SplitShaderSemantic(StringView semantic) noexcept;

// format의 job 리플렉션을 검사하고 out(값 1 모양)을 채운다(규칙: Docs/Design/ShaderSystem 16.1절
// SSD33). layouts는 job의 손으로 쓴 레이아웃 표다(이름으로 상수 버퍼·루트 상수와 대조). 위반하면
// false와 diagnostics(영어 — 자원·멤버·시맨틱 이름 포함)다. out의 이름은 reflection을 가리킨다.
ROCKS_SHADERCOMPILERCOMMON_API bool
NormalizeShaderReflection(const ShaderFormatDefinition& format, const ShaderReflection& reflection,
                          Span<const ShaderParameterLayout> layouts, ShaderBindingLayout& out,
                          String& diagnostics) noexcept;

} // namespace rocks::shader
