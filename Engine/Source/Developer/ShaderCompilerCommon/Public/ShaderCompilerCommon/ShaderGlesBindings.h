#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <ShaderCompilerCommon/ShaderReflection.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderGlesBindingHeader.h>

// GLES 배치와 상수 버퍼 에뮬레이션이다(Docs/Design/ShaderSystem 7.2절 4항 OpenGL ES, 16.1절 SSD42).
// 셰이더 워커의 OpenGL ES 처리기가 BuildSpirvBindingLayout을 거쳐 쓴다(도구를 열지 않는 순수 변환).
//
// BuildGlesPlan은 공용 규칙을 통과한 layout(slot = Vulkan binding)과 리플렉션(상수 버퍼 멤버
// 나무)으로 GLES slot(텍스처 유닛·image 유닛·storage block binding·상수 버퍼 순번)을 정하고,
// SPIRV-Cross에 줄 binding과 combined sampler 이름, 상수 버퍼마다 배열 선언과 멤버 참조 바꿈, 값
// 2(GLES 바인딩 머리)를 만든다. RewriteGlesUniforms는 SPIRV-Cross ESSL(상수 버퍼를 plain uniform
// 구조체로 낸 것)을 그 계획대로 고쳐 쓴다. 바꾼 뒤 버퍼 이름이 남으면(통째 읽기) false다.
namespace rocks::shader {

// SPIR-V (set, binding) 자원의 GLES binding이다(storage block·image·typed buffer).
struct GlesResourceBinding {
    uint32 set = 0;
    uint32 binding = 0;
    uint32 glBinding = 0;
};

// (텍스처, 샘플러) 결합 하나의 이름과 텍스처 유닛이다.
struct GlesCombinedSampler {
    explicit GlesCombinedSampler(Allocator& allocator) noexcept
        : texture(allocator), sampler(allocator), name(allocator) {
    }

    String texture;
    String sampler;
    String name;
    uint32 unit = 0;
};

// 멤버 참조 하나의 바꿈이다. indexed면 `Name.path[식]`을 `replacement(식)`으로, 아니면
// `Name.path`를 replacement로 바꾼다.
struct GlesUniformReference {
    explicit GlesUniformReference(Allocator& allocator) noexcept
        : path(allocator), replacement(allocator) {
    }

    String path;
    String replacement;
    bool indexed = false;
};

// 에뮬레이션할 상수 버퍼 하나다(SPIRV-Cross plain uniform 변수 이름 name).
struct GlesUniformBuffer {
    explicit GlesUniformBuffer(Allocator& allocator) noexcept
        : name(allocator), declarations(allocator), references(allocator) {
    }

    String name;
    // `uniform <타입> name;`을 바꿀 배열 선언과 배열 멤버 접근 함수다.
    String declarations;
    Array<GlesUniformReference> references;
};

struct GlesPlan {
    explicit GlesPlan(Allocator& allocator) noexcept
        : buffers(allocator), bindings(allocator), samplers(allocator), header(allocator) {
    }

    Array<GlesUniformBuffer> buffers;
    Array<GlesResourceBinding> bindings;
    Array<GlesCombinedSampler> samplers;
    // 샘플러 없이 Load만 쓰는 텍스처가 있다(결합 항목의 sampler가 비었다 — SPIRV-Cross dummy
    // 샘플러).
    bool dummySampler = false;
    // 값 2 byte다.
    Array<uint8> header;
};

// 위 설명대로 계획을 만들고 layout의 slot을 GLES 위치로 바꾼다. uses는 SPIR-V 사용 쌍이다. 실패(표
// 상한 초과·결합 없는 텍스처의 샘플러 등)는 false와 diagnostics다.
ROCKS_SHADERCOMPILERCOMMON_API bool BuildGlesPlan(const ShaderReflection& reflection,
                                                  ShaderBindingLayout& layout,
                                                  Span<const ShaderSamplerUse> uses, GlesPlan& out,
                                                  String& diagnostics) noexcept;

// essl을 plan대로 고쳐 쓴다(위 설명). 실패는 false와 diagnostics다(essl은 정해지지 않는다).
ROCKS_SHADERCOMPILERCOMMON_API bool RewriteGlesUniforms(String& essl, const GlesPlan& plan,
                                                        String& diagnostics) noexcept;

} // namespace rocks::shader
