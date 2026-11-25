#pragma once

#include <Core/Result.h>
#include <Graphics/Device.h>
#include <Graphics/DeviceCaps.h>
#include <Graphics/DeviceTypes.h>

// compute dispatch 도우미다(Docs/Design/RenderBase 4.4절 "GX3·GX4 compute 연결 설계" (2), UE
// FComputeShaderUtils 대응). 스레드 수에서 그룹 수를 셈하고, 장치 한도·간접 인자 규칙을 검사하고,
// 그래프 밖 기록에서 파이프라인 묶기 → 묶기 함수 → dispatch를 한 번에 낸다. 렌더 그래프의 compute
// pass 도우미(RenderGraph::AddComputePass)도 같은 규칙을 쓴다.
//
// 사용 예(그래프 밖 기록, 셰이더는 [numthreads(8, 8, 1)]):
//   const render::ComputeGroupCount groups =
//       render::GroupCountFor(width, height, render::ComputeGroupSize2D,
//                             render::ComputeGroupSize2D);
//   if (!render::ValidateGroupCount(device.Caps().limits, groups)) { ... }
//   render::DispatchCompute(commands, pipeline, groups, [&](graphics::CommandContext& c) noexcept {
//       c.SetWritableTexture(0, target);
//   });
namespace rocks::render {

// dispatch 그룹 수(축별)다. 0인 축이 있으면 빈 dispatch다(도우미는 dispatch하지 않는다).
struct ComputeGroupCount {
    uint32 x = 1;
    uint32 y = 1;
    uint32 z = 1;

    constexpr bool IsEmpty() const noexcept {
        return x == 0 || y == 0 || z == 0;
    }

    bool operator==(const ComputeGroupCount&) const noexcept = default;
};

// 2차원 compute 그룹 한 변의 권장 크기다(8 × 8: AMD GCN wave 하나, NVIDIA warp 둘을 채운다 — UE
// kGolden2DGroupSize와 같다).
inline constexpr uint32 ComputeGroupSize2D = 8;

// threads개를 groupSize개씩 덮는 그룹 수다(올림 나눗셈, uint32 끝에서도 넘치지 않는다). groupSize가
// 0이면 0이다(빈 dispatch).
constexpr uint32 GroupsFor(uint32 threads, uint32 groupSize) noexcept {
    return groupSize == 0 ? 0u : threads / groupSize + (threads % groupSize != 0 ? 1u : 0u);
}

// 스레드 수(축별)에서 그룹 수를 셈한다(UE FComputeShaderUtils::GetGroupCount). 셰이더의
// numthreads와 같은 그룹 크기를 넘긴다.
constexpr ComputeGroupCount GroupCountFor(uint32 threadsX, uint32 groupSizeX) noexcept {
    return ComputeGroupCount{GroupsFor(threadsX, groupSizeX), 1, 1};
}

constexpr ComputeGroupCount GroupCountFor(uint32 threadsX, uint32 threadsY, uint32 groupSizeX,
                                          uint32 groupSizeY) noexcept {
    return ComputeGroupCount{GroupsFor(threadsX, groupSizeX), GroupsFor(threadsY, groupSizeY), 1};
}

constexpr ComputeGroupCount GroupCountFor(uint32 threadsX, uint32 threadsY, uint32 threadsZ,
                                          uint32 groupSizeX, uint32 groupSizeY,
                                          uint32 groupSizeZ) noexcept {
    return ComputeGroupCount{GroupsFor(threadsX, groupSizeX), GroupsFor(threadsY, groupSizeY),
                             GroupsFor(threadsZ, groupSizeZ)};
}

// 그룹 수가 장치 한도(DeviceLimits::computeGroupCount, 축별) 안인지 본다. 넘으면 InvalidArgument다
// (장치 Dispatch는 그런 dispatch를 무시하고 IgnoredDraws를 올린다). 빈 그룹 수는 통과한다.
ROCKS_RENDERBASE_API Result<void> ValidateGroupCount(const graphics::DeviceLimits& limits,
                                                     ComputeGroupCount groups) noexcept;

// 간접 dispatch 인자 자리 규칙이다(UE ValidateIndirectArgsBuffer). 위반은 InvalidArgument다:
// 버퍼에 Indirect 용도가 없음, offset이 4 byte 배수가 아님, DispatchIndirectArguments(12 byte)가
// 버퍼 [offset, size) 안에 들지 않음.
ROCKS_RENDERBASE_API Result<void> ValidateDispatchArguments(const graphics::BufferDesc& buffer,
                                                            uint64 offset) noexcept;

// 그래프 밖 기록에서 compute를 낸다: pipeline을 묶고 bind(commands)로 view·상수를 묶은 뒤
// Dispatch한다(UE FComputeShaderUtils::Dispatch의 PSO 설정 → 파라미터 설정 → dispatch 순서). 빈
// 그룹 수면 아무것도 기록하지 않는다. 한도 검사는 ValidateGroupCount로 호출자가 한다. bind는
// void(graphics::CommandContext&) noexcept다.
template <class Bind>
void DispatchCompute(graphics::CommandContext& commands, graphics::PipelineHandle pipeline,
                     ComputeGroupCount groups, Bind&& bind) noexcept {
    if (groups.IsEmpty()) {
        return;
    }

    commands.SetPipeline(pipeline);
    bind(commands);
    commands.Dispatch(groups.x, groups.y, groups.z);
}

// 간접 판이다: arguments[offset]의 DispatchIndirectArguments로 낸다. 인자를 GPU가 썼다면 앞에 그
// 버퍼의 IndirectArgs 전이가 필요하다(렌더 그래프 밖 기록은 장치가 메운다).
template <class Bind>
void DispatchComputeIndirect(graphics::CommandContext& commands, graphics::PipelineHandle pipeline,
                             graphics::BufferHandle arguments, uint64 offset,
                             Bind&& bind) noexcept {
    commands.SetPipeline(pipeline);
    bind(commands);
    commands.DispatchIndirect(arguments, offset);
}

} // namespace rocks::render
