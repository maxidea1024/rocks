#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Graphics/Device.h>
#include <RenderBase/EngineShaderHost.h>
#include <ShaderCore/ShaderFormatFacts.h>
#include <ShaderCore/ShaderLibrary.h>

// 쿠킹 라이브러리의 런타임 형식 선택이다(Docs/Design/ShaderSystem 12.3.1절 SSD15, 16.1절 SSD47, UE
// D3D12TargetedShaderFormats의 앞에서부터 장치가 받는 첫 형식).
//
// 사용 예(엔진 렌더 자원):
//   CookedShaderFormatChoice choice(allocator);
//   if (auto selected = SelectCookedShaderFormat(source, device, ShaderLibraryKind::Engine,
//                                                choice);
//       selected) {
//       set.OpenCooked(Move(choice.bytes));     // choice.FormatId()의 라이브러리
//   } else if (selected.Error() == ErrorCode::NotFound) {
//       ... 공급자 경로(장치 backend가 쿠킹에 없음) ...
//   }
namespace rocks::render {

// 형식 선택 실패 설명 버퍼의 byte 수다.
inline constexpr usize CookedShaderFormatProblemBytes = 512;

// 형식 선택 결과다.
struct CookedShaderFormatChoice {
    explicit CookedShaderFormatChoice(Allocator& allocator) noexcept : bytes(allocator) {
    }

    StringView FormatId() const noexcept {
        return StringView(formatId);
    }

    StringView Problem() const noexcept {
        return StringView(problem);
    }

    // 고른 형식 id(NUL 종료)다.
    char formatId[shader::MaxShaderFormatIdBytes] = {};
    // 고른 형식의 라이브러리 byte다.
    Array<uint8> bytes;
    // 실패의 영어 설명이다(형식마다 맞지 않은 이유를 잇는다 — 넘치면 잘린다).
    char problem[CookedShaderFormatProblemBytes] = {};
};

// facts의 상한(limits)·장치 요구(deviceRequirements)가 장치 능력 안인가(SSD47 대조 규칙). 맞으면
// null, 아니면 첫 위반의 영어 이유(정적 수명)다.
//
// 대조: 단계(VS·PS·CS)마다 상수 버퍼·SRV 텍스처·샘플러 ≤ 장치 단계 한도, storage buffer·storage
// image는 uavStages의 단계만, renderTargets ≤ colorAttachments, interStageVectors·
// rootConstantBytes ≤ 장치 값, uavTotal ≤ pipelineUavs(둘 다 적용될 때), uniformVectors ≤ 장치
// 단계 값(둘 다 적용될 때), Vulkan backend는 PS 합 ≤ perStageResources(적용될 때),
// deviceRequirements ⊆ DeviceFeatures::shaderDeviceRequirements.
ROCKS_RENDERBASE_API const char*
CheckShaderFormatFitsDevice(const shader::ShaderFormatFacts& facts, graphics::Backend backend,
                            const graphics::DeviceCaps& caps) noexcept;

// source에서 device에 맞는 library 종류의 형식을 고른다(SSD15): 장치 backend의 Formats() 앞에서부터
// (1) 라이브러리가 있음(NotFound면 다음) (2)·(3) 라이브러리 머리 형식 사실이
// CheckShaderFormatFitsDevice를 통과하는 첫 형식이다. out은 먼저 비운다.
//
// 오류: 셰이더 없는 쿠킹(Backends() 빔) Unsupported("this cook has no shader formats"), 장치
// backend가 쿠킹에 없음 NotFound(공급자 경로), 맞는 형식이 없음 Unsupported(problem에 형식마다
// 이유), 라이브러리 머리를 읽지 못함 CorruptData(손상 — 6.6절), ReadLibrary의 그 밖 오류(그대로 —
// problem에 원천 설명).
ROCKS_RENDERBASE_API Result<void> SelectCookedShaderFormat(CookedShaderLibrarySource& source,
                                                           const graphics::Device& device,
                                                           shader::ShaderLibraryKind library,
                                                           CookedShaderFormatChoice& out) noexcept;

} // namespace rocks::render
