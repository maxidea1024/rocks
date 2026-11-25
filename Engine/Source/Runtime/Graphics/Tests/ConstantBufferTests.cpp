#include <Core/Memory/SystemAllocator.h>
#include <Graphics/ConstantBuffer.h>
#include <Graphics/RootBudget.h>
#include <Graphics/ShaderBindings.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderFormatTypes.h>
#include <TestSupport/Test.h>
#include <string.h>

// 상수 버퍼 계약 시험이다(Docs/Design/RenderBase 9.11절 GX6): 크기 규칙, 파이프라인 배치와 묶음
// 조건, 값 1 상수 버퍼 수용(이름 표 밖)·slot 범위·진단 버퍼 표시, D3D12 root 예산 계획(보장값
// 13·12·14, 경계 하나 위의 table 내림, 64 DWORD 이하, 결정적 배치).
using namespace rocks;
using namespace rocks::graphics;

namespace constant_buffer_tests_file {

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

constexpr uint8 Code[] = {0x03, 0x02, 0x23, 0x07, 0, 0, 1, 0};
constexpr uint32 FullMask = (1u << MaxConstantBufferSlots) - 1;
// 루트 상수 계약 최대(32 DWORD)다.
constexpr uint32 MaxRootConstantDwords = MaxDrawConstantBytes / sizeof(uint32);

uint32 CountBits(uint32 value) noexcept {
    uint32 count = 0;
    for (; value != 0; value &= value - 1) {
        ++count;
    }

    return count;
}

// 단계 하나의 장치 셰이더다(상수 버퍼만).
struct Stage {
    explicit Stage(uint32 stageBit) noexcept
        : layout(Allocator_()), bytes(Allocator_()), code(Allocator_()) {
        layout.stage = stageBit;
    }

    bool AddConstantBuffer(uint16 slot, const char* name, uint32 size) noexcept {
        shader::ShaderBindingResource resource;
        resource.kind = shader::ShaderResourceKind::ConstantBuffer;
        resource.slot = slot;
        resource.name = StringView(name);
        resource.constantBufferBytes = size;
        return static_cast<bool>(layout.resources.TryPushBack(resource));
    }

    bool AddDebugBuffer() noexcept {
        shader::ShaderBindingResource resource;
        resource.kind = shader::ShaderResourceKind::StorageBufferReadWrite;
        resource.slot = 0;
        resource.name = StringView(PixelDebugBufferName);
        resource.shape = shader::ShaderResourceShape::RawBuffer;
        return static_cast<bool>(layout.resources.TryPushBack(resource));
    }

    bool Load(ShaderStage stage) noexcept {
        bytes.Clear();
        if (!shader::EncodeShaderBindingLayout(layout, bytes)) {
            return false;
        }

        ShaderCodeDesc desc;
        desc.kind = ShaderCodeKind::Dxil;
        desc.stage = stage;
        desc.code = Span<const uint8>(Code, sizeof(Code));
        desc.bindingLayout = bytes.View();
        const char* reason = "";
        return static_cast<bool>(code.Load(desc, ShaderCodeKind::Dxil, &reason));
    }

    shader::ShaderBindingLayout layout;
    Array<uint8> bytes;
    DeviceShaderCode code;
};

// 계획이 예산식을 지키고 배치가 입력과 맞는지 본다.
void CheckPlanInvariants(const RootBudgetInput& input, const RootBudgetPlan& plan) noexcept {
    CHECK(plan.dwords <= MaxRootSignatureDwords);
    uint32 tables = 0;
    uint32 cbvTables = 0;
    uint32 total = 0;
    for (uint32 stage = 0; stage < input.stageCount; ++stage) {
        tables += input.stages[stage].tables;
        cbvTables += plan.tableMask[stage] != 0 ? 1u : 0u;
        total += CountBits(input.stages[stage].constantMask);
        // 모든 CBV가 root 아니면 table이고, 겹치지 않는다.
        CHECK((plan.rootMask[stage] | plan.tableMask[stage]) == input.stages[stage].constantMask);
        CHECK((plan.rootMask[stage] & plan.tableMask[stage]) == 0);
        // 넘친 것은 그 단계의 높은 slot이다.
        if (plan.tableMask[stage] != 0 && plan.rootMask[stage] != 0) {
            uint32 highestRoot = 0;
            for (uint32 slot = 0; slot < MaxConstantBufferSlots; ++slot) {
                if ((plan.rootMask[stage] & (1u << slot)) != 0) {
                    highestRoot = slot;
                }
            }

            CHECK((plan.tableMask[stage] & ((2u << highestRoot) - 1)) == 0);
        }
    }

    CHECK(plan.tableCount == tables + cbvTables);
    CHECK(plan.dwords == input.rootConstantDwords + plan.tableCount * RootTableDwords +
                             plan.rootCbvCount * RootDescriptorDwords +
                             (input.debugUav ? RootDescriptorDwords : 0u));
    uint32 rootBits = 0;
    for (uint32 stage = 0; stage < input.stageCount; ++stage) {
        rootBits += CountBits(plan.rootMask[stage]);
    }

    CHECK(rootBits == plan.rootCbvCount);
    // root 순서는 (slot, 단계) 오름차순이다.
    for (uint32 entry = 1; entry < plan.rootCbvCount; ++entry) {
        const uint32 previous =
            plan.rootOrderSlot[entry - 1] * MaxRootBudgetStages + plan.rootOrderStage[entry - 1];
        const uint32 current =
            plan.rootOrderSlot[entry] * MaxRootBudgetStages + plan.rootOrderStage[entry];
        CHECK(previous < current);
    }

    CHECK(plan.rootCbvCount <= total);
}

TEST(ConstantBuffer_SizeRulesAndSlotBookkeeping) {
    CHECK(!IsValidConstantBufferBytes(0));
    CHECK(!IsValidConstantBufferBytes(8));
    CHECK(IsValidConstantBufferBytes(ConstantBufferGranularity));
    CHECK(!IsValidConstantBufferBytes(ConstantBufferGranularity + 4));
    CHECK(IsValidConstantBufferBytes(MaxConstantBufferBytes));
    CHECK(!IsValidConstantBufferBytes(MaxConstantBufferBytes + ConstantBufferGranularity));
    CHECK(AlignConstantBufferBytes(16) == ConstantBufferAlignment);
    CHECK(AlignConstantBufferBytes(ConstantBufferAlignment) == ConstantBufferAlignment);
    CHECK(AlignConstantBufferBytes(ConstantBufferAlignment + 16) == 2 * ConstantBufferAlignment);
    CHECK(strcmp(ConstantBufferLifetimeName(ConstantBufferLifetime::MultiFrame), "MultiFrame") ==
          0);

    ConstantBufferSlots slots;
    CHECK(AddConstantBufferSlot(slots, ShaderStage::Pixel, 3, 64));
    CHECK(AddConstantBufferSlot(slots, ShaderStage::Vertex, 0, 16));
    // 같은 자리 두 번, slot 범위 밖, 크기 규칙 밖.
    CHECK(!AddConstantBufferSlot(slots, ShaderStage::Pixel, 3, 64));
    CHECK(!AddConstantBufferSlot(slots, ShaderStage::Pixel, MaxConstantBufferSlots, 64));
    CHECK(!AddConstantBufferSlot(slots, ShaderStage::Pixel, 4, 20));
    CHECK(ConstantBufferCount(slots) == 2);

    uint32 bound[ConstantBufferStageCount][MaxConstantBufferSlots] = {};
    CHECK(!ConstantBuffersSatisfied(slots, bound));
    bound[ConstantBufferStageIndex(ShaderStage::Pixel)][3] = 64;
    bound[ConstantBufferStageIndex(ShaderStage::Vertex)][0] = 256;
    CHECK(ConstantBuffersSatisfied(slots, bound));
    // 선언보다 작은 버퍼는 그리지 않는다.
    bound[ConstantBufferStageIndex(ShaderStage::Pixel)][3] = 48;
    CHECK(!ConstantBuffersSatisfied(slots, bound));
}

TEST(ConstantBuffer_PipelineSlotsTakeConstantBuffersByLocation) {
    Stage vertex(shader::ShaderStageVertex);
    REQUIRE(vertex.AddConstantBuffer(0, "View", 64));
    REQUIRE(vertex.AddConstantBuffer(2, "Placement", 16));
    REQUIRE(vertex.Load(ShaderStage::Vertex));
    Stage pixel(shader::ShaderStagePixel);
    REQUIRE(pixel.AddConstantBuffer(1, "Material", 32));
    REQUIRE(pixel.AddDebugBuffer());
    REQUIRE(pixel.Load(ShaderStage::Pixel));
    const DeviceShaderCode* stages[] = {&vertex.code, &pixel.code};
    PipelineBindingNames names;
    PipelineSlots slots;
    char error[PipelineSlotErrorBytes] = {};
    REQUIRE(ResolvePipelineSlots(stages, names, 0, slots, error));
    // 이름 표 없이 값 1 slot 그대로다. 진단 버퍼는 slot 표가 아니라 표시다.
    CHECK(slots.count == 0);
    CHECK(slots.debugBuffer);
    CHECK(slots.constantSlots.usedMask[ConstantBufferStageIndex(ShaderStage::Vertex)] == 0b101);
    CHECK(slots.constantSlots.usedMask[ConstantBufferStageIndex(ShaderStage::Pixel)] == 0b10);
    CHECK(slots.constantSlots.bytes[ConstantBufferStageIndex(ShaderStage::Vertex)][2] == 16);
    CHECK(slots.constantSlots.bytes[ConstantBufferStageIndex(ShaderStage::Pixel)][1] == 32);
    CHECK(FindConstantBufferSlot(vertex.layout, "Placement").Value() == 2);
    CHECK(FindConstantBufferSlot(pixel.layout, "Placement").Error() == ErrorCode::NotFound);

    // slot 범위 밖(14)은 InvalidArgument다.
    Stage wide(shader::ShaderStagePixel);
    REQUIRE(wide.AddConstantBuffer(MaxConstantBufferSlots, "Far", 16));
    REQUIRE(wide.Load(ShaderStage::Pixel));
    const DeviceShaderCode* far[] = {&wide.code};
    CHECK(ResolvePipelineSlots(far, names, 0, slots, error).Error() == ErrorCode::InvalidArgument);
    CHECK(strstr(error, "Far") != nullptr);

    // 진단 이름은 PS의 쓰기 raw 버퍼만이다(VS의 같은 이름은 그래픽 단계 UAV로 거부).
    Stage vertexDebug(shader::ShaderStageVertex);
    REQUIRE(vertexDebug.AddDebugBuffer());
    REQUIRE(vertexDebug.Load(ShaderStage::Vertex));
    const DeviceShaderCode* debugVertex[] = {&vertexDebug.code};
    CHECK(!ResolvePipelineSlots(debugVertex, names, 0, slots, error));
}

TEST(ConstantBuffer_RootBudgetGuaranteesAndOverflow) {
    // 그래픽 최악: VS·PS 각 SRV·Sampler table(CBV table은 계획이 센다), slot 14개씩, 루트 상수 32.
    RootBudgetInput graphics;
    graphics.stageCount = 2;
    graphics.rootConstantDwords = MaxRootConstantDwords;
    graphics.stages[0] = {2, FullMask};
    graphics.stages[1] = {2, FullMask};
    auto full = PlanRootBudget(graphics);
    REQUIRE(full);
    CHECK(full.Value().rootCbvCount == 13);
    CHECK(full.Value().dwords == MaxRootSignatureDwords);
    CheckPlanInvariants(graphics, full.Value());
    // root는 slot 0–5 양 단계와 slot 6의 VS다(slot 순, 같은 slot은 VS 먼저).
    CHECK(full.Value().rootMask[0] == 0b1111111 && full.Value().rootMask[1] == 0b111111);

    // 진단 퍼뮤테이션: root UAV 2 DWORD 때문에 하나 적다(보장값 12, ShaderSystem 7.2절 12항).
    RootBudgetInput debug = graphics;
    debug.debugUav = true;
    auto diagnosed = PlanRootBudget(debug);
    REQUIRE(diagnosed);
    CHECK(diagnosed.Value().rootCbvCount == 12);
    CheckPlanInvariants(debug, diagnosed.Value());

    // CS: table 넷(SRV·Sampler·UAV + CBV), 14개가 모두 root다.
    RootBudgetInput compute;
    compute.stageCount = 1;
    compute.rootConstantDwords = MaxRootConstantDwords;
    compute.stages[0] = {3, FullMask};
    auto cs = PlanRootBudget(compute);
    REQUIRE(cs);
    CHECK(cs.Value().rootCbvCount == 14 && cs.Value().tableMask[0] == 0);
    CheckPlanInvariants(compute, cs.Value());

    // 경계: 루트 상수 32·table 6이면 root 13까지는 table이 없고 하나 더면 그 단계가 table로 내린다.
    RootBudgetInput edge;
    edge.stageCount = 2;
    edge.rootConstantDwords = MaxRootConstantDwords;
    edge.stages[0] = {3, (1u << 7) - 1};
    edge.stages[1] = {3, (1u << 6) - 1};
    auto fits = PlanRootBudget(edge);
    REQUIRE(fits);
    CHECK(fits.Value().rootCbvCount == 13 && fits.Value().tableMask[0] == 0 &&
          fits.Value().tableMask[1] == 0);
    CheckPlanInvariants(edge, fits.Value());
    edge.stages[1].constantMask = (1u << 7) - 1;
    auto spills = PlanRootBudget(edge);
    REQUIRE(spills);
    // 넘친 PS table 1 DWORD를 세면 root는 12다(VS 0–6, PS 0–4 + … 앞 12개), 넘친 것은 높은 slot.
    CHECK(spills.Value().rootCbvCount + CountBits(spills.Value().tableMask[0]) +
              CountBits(spills.Value().tableMask[1]) ==
          14);
    CHECK(spills.Value().tableMask[1] != 0);
    CheckPlanInvariants(edge, spills.Value());

    // 루트 상수가 없으면 더 많이 둔다(28 CBV 모두 root: 6 + 56 = 62).
    RootBudgetInput roomy = graphics;
    roomy.rootConstantDwords = 0;
    auto wide = PlanRootBudget(roomy);
    REQUIRE(wide);
    CHECK(wide.Value().rootCbvCount == 28);
    CheckPlanInvariants(roomy, wide.Value());

    // 결정적이다(같은 입력 → 같은 배치).
    auto again = PlanRootBudget(graphics);
    REQUIRE(again);
    CHECK(memcmp(&again.Value(), &full.Value(), sizeof(RootBudgetPlan)) == 0);

    // 상수 버퍼 없는 파이프라인과 잘못된 입력.
    RootBudgetInput none;
    none.stageCount = 2;
    none.rootConstantDwords = MaxRootConstantDwords;
    auto empty = PlanRootBudget(none);
    REQUIRE(empty);
    CHECK(empty.Value().rootCbvCount == 0 && empty.Value().dwords == MaxRootConstantDwords);
    RootBudgetInput broken = none;
    broken.stageCount = 0;
    CHECK(PlanRootBudget(broken).Error() == ErrorCode::InvalidArgument);
    broken = none;
    broken.stages[0].constantMask = 1u << MaxConstantBufferSlots;
    CHECK(PlanRootBudget(broken).Error() == ErrorCode::InvalidArgument);
    broken = none;
    broken.rootConstantDwords = MaxRootConstantDwords + 1;
    CHECK(PlanRootBudget(broken).Error() == ErrorCode::InvalidArgument);
    broken = none;
    broken.stages[0].tables = MaxRootSignatureDwords;
    CHECK(PlanRootBudget(broken).Error() == ErrorCode::CapacityExceeded);
}

} // namespace constant_buffer_tests_file
