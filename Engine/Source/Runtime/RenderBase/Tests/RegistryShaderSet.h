#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <RenderBase/ShaderSet.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderLibrary.h>
#include <ShaderCore/ShaderLibraryContents.h>
#include <ShaderCore/ShaderTypeRegistry.h>

// 등록부의 모든 조합(완결성 검사 대상)을 가짜 코드로 담은 쿠킹 라이브러리로 연 셰이더 집합이다
// (RenderBase·RenderGraph 시험 공용). 형식은 D3D12_SM6이고, 항목마다 코드 byte는 (타입 해시, 정점
// 입력, 퍼뮤테이션)에서 만든 4 byte, 값 1은 그 단계만 적은 빈 바인딩 layout이다. 기록 장치는 코드를
// 해석하지 않으므로 파이프라인 캐시·그래프·팩 시험의 셰이더 자리로 쓴다.
//
// 사용 예:
//   shader::ShaderTypeRegistry registry(allocator);
//   (void)registry.Register(render::EngineShaderTypes());
//   (void)registry.Freeze();
//   render::testing::RegistryShaderSet shaders(allocator, device, registry);
//   REQUIRE(shaders.ready);
namespace rocks::render::testing {

struct RegistryShaderSet {
    RegistryShaderSet(Allocator& allocator, graphics::Device& device,
                      const shader::ShaderTypeRegistry& registry) noexcept
        : set(Desc(allocator, device, registry)) {
        shader::ShaderLibraryBuildDesc build;
        build.kind = shader::ShaderLibraryKind::Engine;
        if (!shader::SetShaderFormatFactsId(build.facts, "D3D12_SM6")) {
            return;
        }

        build.facts.backend = shader::ShaderBackend::D3D12;
        Array<shader::ShaderLibraryCombination> combinations(allocator);
        if (!shader::EnumerateShaderLibraryCombinations(registry.Types(), VertexInputs(registry),
                                                        build.facts, build.kind, false,
                                                        combinations)) {
            return;
        }

        // 항목마다 코드·layout byte다(만들기가 끝날 때까지 산다).
        Array<Array<uint8>> layouts(allocator);
        Array<uint32> codes(allocator);
        Array<shader::ShaderLibraryBuildEntry> entries(allocator);
        if (!layouts.TryReserve(combinations.Size()) || !codes.TryReserve(combinations.Size()) ||
            !entries.TryReserve(combinations.Size())) {
            return;
        }

        Array<shader::ShaderLibraryValue> values(allocator);
        if (!values.TryReserve(combinations.Size() * 2)) {
            return;
        }

        for (const shader::ShaderLibraryCombination& combination : combinations) {
            (void)layouts.TryPushBack(Array<uint8>(allocator));
            shader::ShaderBindingLayout binding(allocator);
            binding.stage = combination.type->stage;
            if (!shader::EncodeShaderBindingLayout(binding, layouts[layouts.Size() - 1])) {
                return;
            }

            (void)codes.TryPushBack(static_cast<uint32>(combination.type->nameHash) ^
                                    static_cast<uint32>(combination.vertexInputType) ^
                                    (combination.permutationId * 0x9E37'79B9u));
            const usize first = values.Size();
            (void)values.TryPushBack(shader::ShaderLibraryValue{
                shader::ShaderLibraryValueCode,
                Span<const uint8>(reinterpret_cast<const uint8*>(&codes[codes.Size() - 1]),
                                  sizeof(uint32))});
            (void)values.TryPushBack(shader::ShaderLibraryValue{
                shader::ShaderLibraryValueBindingLayout, layouts[layouts.Size() - 1].View()});
            shader::ShaderLibraryBuildEntry entry;
            entry.key = shader::ShaderLibraryCombinationKey(combination);
            entry.typeName = combination.type->name;
            entry.stage = combination.type->stage;
            entry.layoutSignature =
                shader::ShaderParameterLayoutSignature(combination.type->parameterLayouts);
            entry.values = Span<const shader::ShaderLibraryValue>(values.Data() + first, 2);
            (void)entries.TryPushBack(entry);
        }

        build.typeSetSha256 = registry.TypeSetDigest();
        build.entries = entries.View();
        Array<uint8> bytes(allocator);
        ready = shader::BuildShaderLibrary(build, allocator, bytes) && set.OpenCooked(Move(bytes));
    }

    static ShaderSetDesc Desc(Allocator& allocator, graphics::Device& device,
                              const shader::ShaderTypeRegistry& registry) noexcept {
        ShaderSetDesc desc;
        desc.allocator = &allocator;
        desc.device = &device;
        desc.registry = &registry;
        desc.formatId = "D3D12_SM6";
        return desc;
    }

    // 등록부의 정점 입력 타입이다(완결성 검사와 같은 목록).
    static Span<const shader::ShaderVertexInputType>
    VertexInputs(const shader::ShaderTypeRegistry& registry) noexcept {
        return registry.VertexInputTypes();
    }

    ShaderSet set;
    bool ready = false;
};

} // namespace rocks::render::testing
