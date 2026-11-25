#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderReadStatus.h>

// GLES 바인딩 머리 v1(셰이더 Record 값 2 GlesCopyTable)이다(Docs/Design/ShaderSystem 7.2절 4항
// OpenGL ES, 12.2.1절 SSD26, UE FOpenGLShaderBindings·UniformBuffersCopyInfo 대응).
//
// GLES 형식은 상수 버퍼와 루트 상수를 단계·기본 타입별 vec4 배열로 에뮬레이션한다. 런타임은 링크 때
// 배열 위치를 이름으로 한 번 찾고, 그리기 때 바뀐 버퍼만 복사 표대로 glUniform4fv·4iv·4uiv로
// 올린다. 배열 이름은 규칙(<단계>c<순번>_<타입>, 루트 상수는 <단계>u_<타입>)에서 나오므로 담지
// 않는다(ShaderGlesUniformArrayName).
//
// byte 형식(모두 little-endian, 이 순서):
//   u32 형식 버전(= ShaderGlesBindingHeaderVersion), u32 단계, u32 attribute 마스크(VS만),
//   u32 배열 수, 배열마다 (u16 상수 버퍼 순번 — 루트 상수는 ShaderGlesRootConstantsBuffer, u8 기본
//   타입 'h'·'i'·'u', u32 vec4 수),
//   u32 샘플러 수, 샘플러마다 (u16 텍스처 유닛, u8 이름 byte 수, 이름 byte, u8 샘플러 출처),
//   u32 복사 수, 복사마다 (u16 상수 버퍼 순번, u32 원본 byte 오프셋, u32 byte 수, u16 대상 배열
//   색인, u32 대상 vec4 오프셋).
// 배열은 (순번, 타입) 오름차순·중복 없음, 샘플러는 유닛 오름차순·중복 없음, 복사는 (순번, 원본
// 오프셋) 오름차순이다. 복사 byte 수는 4의 배수이고 원본 구간끼리, 대상 구간끼리 겹치지 않으며 대상
// 배열 안에 있다. 복사의 순번은 대상 배열의 순번과 같다.
namespace rocks::shader {

inline constexpr uint32 ShaderGlesBindingHeaderVersion = 1;
// 루트 상수 배열의 상수 버퍼 순번 자리 표지다.
inline constexpr uint16 ShaderGlesRootConstantsBuffer = 0xFFFF;
// 표마다 최대 원소 수다.
inline constexpr usize MaxShaderGlesArrays = 64;
inline constexpr usize MaxShaderGlesSamplers = 64;
inline constexpr usize MaxShaderGlesCopies = 1024;
// 배열 이름 버퍼 크기(NUL 포함 — "pc65534_h"보다 넉넉하다)다.
inline constexpr usize ShaderGlesArrayNameBytes = 16;

// 배열 기본 타입이다(값은 byte 형식의 ASCII 철자 그대로).
enum class ShaderGlesUniformType : uint8 {
    Float = 'h',
    Int = 'i',
    Uint = 'u'
};

struct ShaderGlesUniformArray {
    uint16 buffer = 0;
    ShaderGlesUniformType type = ShaderGlesUniformType::Float;
    uint32 vec4Count = 0;
};

struct ShaderGlesSampler {
    uint16 unit = 0;
    // combined sampler 이름(<단계>s<순번>)이다.
    StringView name;
    ShaderSamplerSource source = ShaderSamplerSource::None;
};

struct ShaderGlesCopy {
    uint16 buffer = 0;
    uint32 sourceOffset = 0;
    uint32 bytes = 0;
    uint16 arrayIndex = 0;
    uint32 vec4Offset = 0;
};

// GLES 바인딩 머리 하나다. 이름은 원본 byte(읽기) 또는 호출자 문자열(쓰기)을 가리킨다.
struct ShaderGlesBindingHeader {
    explicit ShaderGlesBindingHeader(Allocator& allocator) noexcept
        : arrays(allocator), samplers(allocator), copies(allocator) {
    }

    uint32 stage = 0;
    uint32 attributeMask = 0;
    Array<ShaderGlesUniformArray> arrays;
    Array<ShaderGlesSampler> samplers;
    Array<ShaderGlesCopy> copies;
};

// header를 위 byte 형식으로 out 끝에 붙인다(읽기와 같은 규칙으로 먼저 검사 — 위반은
// InvalidArgument이고 out은 바뀌지 않는다).
ROCKS_SHADERCORE_API Result<void>
EncodeShaderGlesBindingHeader(const ShaderGlesBindingHeader& header, Array<uint8>& out) noexcept;

// bytes(값 2 전체)를 읽어 out을 채운다. 버전이 다르면 UnsupportedVersion, 위반은 CorruptPackage다.
ROCKS_SHADERCORE_API ShaderReadStatus
DecodeShaderGlesBindingHeader(Span<const uint8> bytes, ShaderGlesBindingHeader& out) noexcept;

// 배열의 GLSL 이름(<v·p·c>c<순번>_<h·i·u>, 루트 상수는 <v·p·c>u_<h·i·u>)을 out에 NUL 종료로 쓴다.
// 단계가 한 비트가 아니면 false다.
ROCKS_SHADERCORE_API bool
ShaderGlesUniformArrayName(uint32 stage, const ShaderGlesUniformArray& array,
                           char (&out)[ShaderGlesArrayNameBytes]) noexcept;

} // namespace rocks::shader
