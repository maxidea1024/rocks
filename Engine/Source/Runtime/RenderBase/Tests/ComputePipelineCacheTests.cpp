#include "RecordingDevice.h"

#include <Core/Memory/SystemAllocator.h>
#include <RenderBase/ComputeDispatch.h>
#include <RenderBase/PipelineCache.h>
#include <RenderBase/ShaderSet.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderLibrary.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <TestSupport/Test.h>

// compute 파이프라인 캐시와 dispatch 도우미 시험이다(Docs/Design/RenderBase 4.4절 "GX3·GX4 compute
// 연결 설계" (1)·(2)). 라이브러리는 ShaderCore BuildShaderLibrary로 메모리에 만들고(VS 하나·CS
// 하나) 장치는 기록 장치다.
using namespace rocks;
using namespace rocks::render;
using namespace rocks::shader;
using rocks::render::testing::RecordingDevice;

namespace compute_pipeline_cache_tests_file {

namespace {

SystemAllocator& TestAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

ROCKS_SHADER_TYPE_NAME(ComputeTestVertexName, "ComputeTestVertex");
ROCKS_SHADER_TYPE_NAME(ComputeTestComputeName, "ComputeTestCompute");

struct ComputeTestVertex {
    static constexpr ShaderTypeDesc Type =
        MakeShaderTypeDesc(ComputeTestVertexName, "/Engine/Private/ComputeTest.rsf", "MainVS",
                           ShaderStageVertex, ShaderTypeOwner::Engine);
};

struct ComputeTestCompute {
    static constexpr ShaderTypeDesc Type =
        MakeShaderTypeDesc(ComputeTestComputeName, "/Engine/Private/ComputeTest.rsf", "MainCS",
                           ShaderStageCompute, ShaderTypeOwner::Engine);
};

constexpr uint8 VertexBytes[] = {1, 2, 3, 4};
constexpr uint8 ComputeBytes[] = {9, 8, 7, 6};
// CS의 그리기별 상수 byte 수다(파이프라인 선언이 이보다 작으면 실제 장치는 거부한다).
constexpr uint32 ComputeRootBytes = 16;

struct ComputeRegistry {
    ComputeRegistry() noexcept : registry(TestAllocator()) {
        const ShaderTypeDesc* const types[] = {&ComputeTestVertex::Type, &ComputeTestCompute::Type};
        registered = registry.Register(types) && registry.Freeze();
    }

    ShaderTypeRegistry registry;
    bool registered = false;
};

bool EncodeStage(uint32 stage, uint32 rootBytes, Array<uint8>& out) noexcept {
    ShaderBindingLayout layout(TestAllocator());
    layout.stage = stage;
    layout.rootConstantBytes = rootBytes;
    return static_cast<bool>(EncodeShaderBindingLayout(layout, out));
}

bool BuildComputeLibrary(const ComputeRegistry& types, Array<uint8>& out) noexcept {
    Array<uint8> vertexLayout(TestAllocator());
    Array<uint8> computeLayout(TestAllocator());
    if (!EncodeStage(ShaderStageVertex, 0, vertexLayout) ||
        !EncodeStage(ShaderStageCompute, ComputeRootBytes, computeLayout)) {
        return false;
    }

    const ShaderLibraryValue vertexValues[] = {
        {ShaderLibraryValueCode, Span<const uint8>(VertexBytes, sizeof(VertexBytes))},
        {ShaderLibraryValueBindingLayout, vertexLayout.View()}};
    const ShaderLibraryValue computeValues[] = {
        {ShaderLibraryValueCode, Span<const uint8>(ComputeBytes, sizeof(ComputeBytes))},
        {ShaderLibraryValueBindingLayout, computeLayout.View()}};
    ShaderLibraryBuildEntry entries[2];
    entries[0].key = ShaderLibraryKey{0, ComputeTestVertexName.hash, NoVertexInputType, 0};
    entries[0].typeName = ComputeTestVertexName.name;
    entries[0].stage = ShaderStageVertex;
    entries[0].layoutSignature =
        ShaderParameterLayoutSignature(Span<const ShaderParameterLayout>());
    entries[0].values = vertexValues;
    entries[1].key = ShaderLibraryKey{0, ComputeTestComputeName.hash, NoVertexInputType, 0};
    entries[1].typeName = ComputeTestComputeName.name;
    entries[1].stage = ShaderStageCompute;
    entries[1].layoutSignature =
        ShaderParameterLayoutSignature(Span<const ShaderParameterLayout>());
    entries[1].values = computeValues;
    ShaderLibraryBuildDesc desc;
    desc.kind = ShaderLibraryKind::Engine;
    if (!SetShaderFormatFactsId(desc.facts, "D3D12_SM6")) {
        return false;
    }

    desc.facts.backend = ShaderBackend::D3D12;
    desc.typeSetSha256 = types.registry.TypeSetDigest();
    desc.entries = entries;
    return static_cast<bool>(BuildShaderLibrary(desc, TestAllocator(), out));
}

ShaderSetDesc ComputeSetDesc(RecordingDevice& device, const ComputeRegistry& types) noexcept {
    ShaderSetDesc desc;
    desc.allocator = &TestAllocator();
    desc.device = &device;
    desc.registry = &types.registry;
    desc.formatId = "D3D12_SM6";
    return desc;
}

Result<void> OpenComputeSet(ShaderSet& set, const ComputeRegistry& types) noexcept {
    Array<uint8> bytes(TestAllocator());
    if (!BuildComputeLibrary(types, bytes)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return set.OpenCooked(Move(bytes));
}

usize CountOps(const RecordingDevice& device, RecordingDevice::Op op) noexcept {
    usize count = 0;
    for (const RecordingDevice::Call& call : device.Calls()) {
        count += call.op == op ? 1u : 0u;
    }

    return count;
}

} // namespace

// compute 요청은 같은 캐시의 다른 키 종류다: 적중·미적중, 상수 크기·이름 표가 키를 가르고, 단계가
// 맞지 않으면 거부하며, RetireSet이 compute 항목도 Retire한다.
TEST(PipelineCache_ComputeHitsMissesAndRetire) {
    ComputeRegistry types;
    REQUIRE(types.registered);
    RecordingDevice device(TestAllocator());
    ShaderSet set(ComputeSetDesc(device, types));
    REQUIRE(OpenComputeSet(set, types));
    auto compute = TypedShader<ComputeTestCompute>::Find(set);
    auto vertex = TypedShader<ComputeTestVertex>::Find(set);
    REQUIRE(compute && vertex);
    {
        PipelineCache cache(TestAllocator(), device);
        ComputePipelineRequest request;
        request.set = &set;
        request.computeEntry = compute.Value().Entry();
        request.state.drawConstantBytes = ComputeRootBytes;
        request.state.bindings.writableBuffers[0] = "Output";
        auto first = cache.FindOrCreate(request);
        auto second = cache.FindOrCreate(request);
        REQUIRE(first && second);
        CHECK(first.Value() == second.Value());
        CHECK(cache.Hits() == 1 && cache.Misses() == 1 && cache.Size() == 1);
        CHECK(device.LastPipelineComputeShader() == compute.Value().DeviceShader().Value());
        CHECK(CountOps(device, RecordingDevice::Op::CreateComputePipeline) == 1);

        // 상수 크기·이름 표가 다르면 다른 항목이다.
        ComputePipelineRequest larger = request;
        larger.state.drawConstantBytes = 2 * ComputeRootBytes;
        auto largerPipeline = cache.FindOrCreate(larger);
        REQUIRE(largerPipeline);
        CHECK(!(largerPipeline.Value() == first.Value()));
        ComputePipelineRequest renamed = request;
        renamed.state.bindings.writableBuffers[0] = "Other";
        REQUIRE(cache.FindOrCreate(renamed));
        renamed.state.bindings.bufferViews[1] = "Input";
        REQUIRE(cache.FindOrCreate(renamed));
        CHECK(cache.Misses() == 4 && cache.Size() == 4 && set.LeaseCount() == 4);

        // 단계가 CS가 아니거나 번호가 밖이거나 집합이 없으면 거부한다(담지 않는다).
        ComputePipelineRequest wrongStage = request;
        wrongStage.computeEntry = vertex.Value().Entry();
        CHECK(cache.FindOrCreate(wrongStage).Error() == ErrorCode::InvalidArgument);
        ComputePipelineRequest outside = request;
        outside.computeEntry = set.EntryCount();
        CHECK(cache.FindOrCreate(outside).Error() == ErrorCode::InvalidArgument);
        ComputePipelineRequest noSet = request;
        noSet.set = nullptr;
        CHECK(cache.FindOrCreate(noSet).Error() == ErrorCode::InvalidArgument);
        // 그래픽 요청의 PS 자리에 CS를 넣어도 거부한다(같은 항목 번호여도 키 종류가 다르다).
        GraphicsPipelineRequest graphics;
        graphics.set = &set;
        graphics.vertexEntry = vertex.Value().Entry();
        graphics.pixelEntry = compute.Value().Entry();
        CHECK(cache.FindOrCreate(graphics).Error() == ErrorCode::InvalidArgument);
        // 장치가 거부한 선언(상수 크기 규칙 밖)은 담지 않고 lease도 놓는다.
        ComputePipelineRequest invalid = request;
        invalid.state.drawConstantBytes = 20;
        CHECK(cache.FindOrCreate(invalid).Error() == ErrorCode::InvalidArgument);
        CHECK(cache.Size() == 4 && set.LeaseCount() == 4);

        cache.RetireSet(set);
        CHECK(cache.Size() == 0 && set.LeaseCount() == 0);
        CHECK(CountOps(device, RecordingDevice::Op::RetirePipeline) == 4);
    }

    {
        // 열지 않은 집합은 NotReady, Retire한 집합은 Destroyed다.
        PipelineCache cache(TestAllocator(), device);
        ShaderSet closed(ComputeSetDesc(device, types));
        ComputePipelineRequest request;
        request.set = &closed;
        request.computeEntry = 0;
        CHECK(cache.FindOrCreate(request).Error() == ErrorCode::NotReady);
        set.Retire();
        request.set = &set;
        request.computeEntry = compute.Value().Entry();
        CHECK(cache.FindOrCreate(request).Error() == ErrorCode::Destroyed);
        CHECK(cache.Size() == 0);
    }
}

// 그룹 수 셈(올림·넘침 없음·0), 장치 한도 검사, 간접 인자 규칙이다.
TEST(ComputeDispatch_GroupCountsAndRules) {
    CHECK(GroupsFor(0, 64) == 0 && GroupsFor(1, 64) == 1 && GroupsFor(64, 64) == 1 &&
          GroupsFor(65, 64) == 2);
    CHECK(GroupsFor(0xFFFF'FFFFu, 2) == 0x8000'0000u && GroupsFor(0xFFFF'FFFFu, 1) == 0xFFFF'FFFFu);
    CHECK(GroupsFor(10, 0) == 0);
    static_assert(GroupCountFor(100, 64) == ComputeGroupCount{2, 1, 1});
    static_assert(GroupCountFor(17, 9, ComputeGroupSize2D, ComputeGroupSize2D) ==
                  ComputeGroupCount{3, 2, 1});
    static_assert(GroupCountFor(4, 4, 5, 4, 4, 4) == ComputeGroupCount{1, 1, 2});
    CHECK(GroupCountFor(0, 8, ComputeGroupSize2D, ComputeGroupSize2D).IsEmpty());
    CHECK(!ComputeGroupCount{}.IsEmpty());

    graphics::DeviceLimits limits;
    limits.computeGroupCount[0] = 65535;
    limits.computeGroupCount[1] = 65535;
    limits.computeGroupCount[2] = 64;
    CHECK(ValidateGroupCount(limits, {65535, 65535, 64}).IsOk());
    CHECK(ValidateGroupCount(limits, {65536, 1, 1}).Error() == ErrorCode::InvalidArgument);
    CHECK(ValidateGroupCount(limits, {1, 1, 65}).Error() == ErrorCode::InvalidArgument);
    // 빈 그룹 수는 dispatch하지 않으므로 한도와 무관하게 통과한다.
    CHECK(ValidateGroupCount(limits, {0, 70000, 70000}).IsOk());

    graphics::BufferDesc arguments;
    arguments.size = 32;
    arguments.usage = graphics::BufferUsage::Indirect | graphics::BufferUsage::ShaderWrite |
                      graphics::BufferUsage::Raw;
    CHECK(ValidateDispatchArguments(arguments, 0).IsOk());
    CHECK(ValidateDispatchArguments(arguments, 20).IsOk());
    CHECK(ValidateDispatchArguments(arguments, 24).Error() == ErrorCode::InvalidArgument);
    CHECK(ValidateDispatchArguments(arguments, 2).Error() == ErrorCode::InvalidArgument);
    CHECK(ValidateDispatchArguments(arguments, 64).Error() == ErrorCode::InvalidArgument);
    arguments.usage = graphics::BufferUsage::ShaderWrite | graphics::BufferUsage::Raw;
    CHECK(ValidateDispatchArguments(arguments, 0).Error() == ErrorCode::InvalidArgument);
}

// 그래프 밖 기록 도우미: 파이프라인 → 묶기 → dispatch 순서, 빈 그룹 수는 아무것도 기록하지 않는다.
TEST(ComputeDispatch_RecordsPipelineBindAndDispatch) {
    RecordingDevice device(TestAllocator());
    graphics::ComputePipelineDesc desc;
    auto pipeline = device.CreateComputePipeline(desc);
    graphics::TextureDesc targetDesc;
    targetDesc.width = 4;
    targetDesc.height = 4;
    targetDesc.usage = graphics::TextureUsage::RenderTarget;
    auto target = device.CreateTexture(targetDesc, {});
    REQUIRE(pipeline && target);
    auto commands = device.BeginOffscreen(target.Value(), {});
    REQUIRE(commands);
    uint32 binds = 0;
    DispatchCompute(*commands.Value(), pipeline.Value(), {2, 3, 4},
                    [&binds](graphics::CommandContext&) noexcept { ++binds; });
    DispatchCompute(*commands.Value(), pipeline.Value(), {0, 3, 4},
                    [&binds](graphics::CommandContext&) noexcept { ++binds; });
    REQUIRE(device.EndFrame());
    CHECK(binds == 1);
    CHECK(CountOps(device, RecordingDevice::Op::Dispatch) == 1);
    CHECK(CountOps(device, RecordingDevice::Op::SetPipeline) == 1);
    bool found = false;
    for (const RecordingDevice::Call& call : device.Calls()) {
        found = found || (call.op == RecordingDevice::Op::Dispatch &&
                          call.value == RecordingDevice::DispatchValue(2, 3, 4));
    }

    CHECK(found);
    device.Retire(pipeline.Value());
    device.Retire(target.Value());
}

} // namespace compute_pipeline_cache_tests_file
