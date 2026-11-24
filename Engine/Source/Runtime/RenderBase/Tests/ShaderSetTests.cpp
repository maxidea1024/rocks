#include "RecordingDevice.h"

#include <Core/Memory/SystemAllocator.h>
#include <Platform/Thread.h>
#include <RenderBase/PipelineCache.h>
#include <RenderBase/ShaderParameters.h>
#include <RenderBase/ShaderSet.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderLibrary.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <TestSupport/Test.h>
#include <string.h>

// 셰이더 집합·파이프라인 캐시·바인딩 지도 시험이다(Docs/Design/RenderBase 9.10절 RB3·RB4
// SH2분, SX10). 라이브러리는 ShaderCore BuildShaderLibrary로 메모리에 만들고 장치는 기록 장치다. 첫
// 사용 생성 경쟁은 기록 장치의 명시 gate(HoldShaderCreation)로 두 생성이 모두 장치에 닿은 뒤
// 게시하게 한다(sleep 없음).
using namespace rocks;
using namespace rocks::render;
using namespace rocks::shader;
using rocks::render::testing::RecordingDevice;

namespace {

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

ROCKS_SHADER_TYPE_NAME(SetVertexName, "SetTestVertex");
ROCKS_SHADER_TYPE_NAME(SetPixelName, "SetTestPixel");
ROCKS_SHADER_TYPE_NAME(SetDebugName, "SetTestDebug");

constexpr ShaderParameterMember TintMembers[] = {{"tint", ShaderParameterType::Float4, 0, 0}};
constexpr ShaderParameterLayout TintLayouts[] = {{"Tint", true, 16, TintMembers}};

struct SetTestVertex {
    static constexpr ShaderTypeDesc Type =
        MakeShaderTypeDesc(SetVertexName, "/Engine/Private/SetTest.rsf", "MainVS",
                           ShaderStageVertex, ShaderTypeOwner::Engine);
};

constexpr ShaderTypeDesc MakePixel() noexcept {
    ShaderTypeDesc desc = MakeShaderTypeDesc(SetPixelName, "/Engine/Private/SetTest.rsf", "MainPS",
                                             ShaderStagePixel, ShaderTypeOwner::Engine);
    desc.parameterLayouts = TintLayouts;
    return desc;
}

struct SetTestPixel {
    static constexpr ShaderTypeDesc Type = MakePixel();
};

constexpr ShaderTypeDesc MakeDebug() noexcept {
    ShaderTypeDesc desc = MakeShaderTypeDesc(SetDebugName, "/Engine/Private/SetTest.rsf", "MainPS",
                                             ShaderStagePixel, ShaderTypeOwner::Engine);
    desc.flags = ShaderTypeDevelopmentOnly;
    return desc;
}

constexpr ShaderTypeDesc SetTestDebug = MakeDebug();

constexpr uint8 VertexCode[] = {1, 2, 3, 4};
constexpr uint8 PixelCode[] = {5, 6, 7, 8};

// 등록부(VS·PS·개발 전용 PS)다.
struct Registry {
    Registry() noexcept : registry(Allocator_()) {
        const ShaderTypeDesc* const types[] = {&SetTestVertex::Type, &SetTestPixel::Type,
                                               &SetTestDebug};
        registered = registry.Register(types) && registry.Freeze();
    }

    ShaderTypeRegistry registry;
    bool registered = false;
};

// 라이브러리를 만드는 선택이다.
struct LibraryOptions {
    bool pixel = true;
    bool badSignature = false;
    // 0이 아니면 단계 사이 마스크다(VS·PS 연결 검사).
    uint32 vertexInterStage = 1;
    uint32 pixelInterStage = 1;
    uint32 pixelRuntimeFeatures = 0;
    const char* formatId = "D3D12_SM6";
    ShaderBackend backend = ShaderBackend::D3D12;
    bool wrongTypeSet = false;
};

bool EncodeLayout(uint32 stage, uint32 interStage, uint32 rootBytes, Array<uint8>& out) noexcept {
    ShaderBindingLayout layout(Allocator_());
    layout.stage = stage;
    layout.interStageMask = interStage;
    layout.rootConstantBytes = rootBytes;
    layout.renderTargetMask = stage == ShaderStagePixel ? 1u : 0u;
    return static_cast<bool>(EncodeShaderBindingLayout(layout, out));
}

bool BuildLibrary(const Registry& types, const LibraryOptions& options,
                  Array<uint8>& out) noexcept {
    Array<uint8> vertexLayout(Allocator_());
    Array<uint8> pixelLayout(Allocator_());
    if (!EncodeLayout(ShaderStageVertex, options.vertexInterStage, 0, vertexLayout) ||
        !EncodeLayout(ShaderStagePixel, options.pixelInterStage, 16, pixelLayout)) {
        return false;
    }

    const ShaderLibraryValue vertexValues[] = {
        {ShaderLibraryValueCode, Span<const uint8>(VertexCode, sizeof(VertexCode))},
        {ShaderLibraryValueBindingLayout, vertexLayout.View()}};
    const ShaderLibraryValue pixelValues[] = {
        {ShaderLibraryValueCode, Span<const uint8>(PixelCode, sizeof(PixelCode))},
        {ShaderLibraryValueBindingLayout, pixelLayout.View()}};
    ShaderLibraryBuildEntry entries[2];
    entries[0].key = ShaderLibraryKey{0, SetVertexName.hash, NoVertexInputType, 0};
    entries[0].typeName = SetVertexName.name;
    entries[0].stage = ShaderStageVertex;
    entries[0].layoutSignature =
        ShaderParameterLayoutSignature(Span<const ShaderParameterLayout>());
    entries[0].values = vertexValues;
    entries[1].key = ShaderLibraryKey{0, SetPixelName.hash, NoVertexInputType, 0};
    entries[1].typeName = SetPixelName.name;
    entries[1].stage = ShaderStagePixel;
    entries[1].runtimeFeatures = options.pixelRuntimeFeatures;
    entries[1].layoutSignature = ShaderParameterLayoutSignature(
        options.badSignature ? Span<const ShaderParameterLayout>()
                             : Span<const ShaderParameterLayout>(TintLayouts));
    entries[1].values = pixelValues;
    ShaderLibraryBuildDesc desc;
    desc.kind = ShaderLibraryKind::Engine;
    if (!SetShaderFormatFactsId(desc.facts, options.formatId)) {
        return false;
    }

    desc.facts.backend = options.backend;
    desc.typeSetSha256 = types.registry.TypeSetDigest();
    if (options.wrongTypeSet) {
        desc.typeSetSha256.bytes[0] ^= 1;
    }

    desc.entries = Span<const ShaderLibraryBuildEntry>(entries, options.pixel ? 2 : 1);
    return static_cast<bool>(BuildShaderLibrary(desc, Allocator_(), out));
}

ShaderSetDesc SetDesc(RecordingDevice& device, const Registry& types) noexcept {
    ShaderSetDesc desc;
    desc.allocator = &Allocator_();
    desc.device = &device;
    desc.registry = &types.registry;
    desc.formatId = "D3D12_SM6";
    return desc;
}

// 라이브러리를 만들어 연다.
Result<void> OpenWith(ShaderSet& set, const Registry& types,
                      const LibraryOptions& options) noexcept {
    Array<uint8> bytes(Allocator_());
    if (!BuildLibrary(types, options, bytes)) {
        CHECK(false);
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return set.OpenCooked(Move(bytes));
}

// 이 시험 안에서 끝내지 않는 공급자다(요청만 받아 둔다).
struct DeferredProvider final : ShaderProvider {
    Result<void> RequestLibrary(const ShaderProvisionRequest& request,
                                ShaderSet& set) noexcept override {
        ++requests;
        formatMatches = request.formatId == StringView("D3D12_SM6");
        target = &set;
        return refuse ? Result<void>::Failure(ErrorCode::Unsupported) : Result<void>::Success();
    }

    ShaderSet* target = nullptr;
    uint32 requests = 0;
    bool formatMatches = false;
    bool refuse = false;
};

// 경쟁 스레드 하나다.
struct Racer {
    ShaderSet* set = nullptr;
    uint32 entry = 0;
    Result<graphics::ShaderHandle> result =
        Result<graphics::ShaderHandle>::Failure(ErrorCode::None);

    static void Run(void* context) noexcept {
        Racer& self = *static_cast<Racer*>(context);
        self.result = self.set->DeviceShader(self.entry);
    }
};

} // namespace

TEST(ShaderSet_CookedLibraryOpensAndCreatesDeviceShadersOnFirstUse) {
    Registry types;
    REQUIRE(types.registered);
    RecordingDevice device(Allocator_());
    ShaderSet set(SetDesc(device, types));
    REQUIRE(OpenWith(set, types, {}));
    CHECK(set.State() == ShaderSetState::Ready);
    CHECK(set.Problem() == ShaderSetProblem::None);
    // 개발 전용 타입은 라이브러리에 없어도 완결성을 깨지 않는다.
    CHECK(set.EntryCount() == 2);
    auto vertex = TypedShader<SetTestVertex>::Find(set);
    auto pixel = TypedShader<SetTestPixel>::Find(set);
    REQUIRE(vertex && pixel);
    CHECK(set.BindingLayout(pixel.Value().Entry())->rootConstantBytes == 16);
    CHECK(!TypedShader<SetTestPixel>::Find(set, 1));
    CHECK(TypedShader<SetTestPixel>::Find(set, 1).Error() == ErrorCode::NotFound);
    CHECK(set.PublishedShaders() == 0 && device.CreatedShaders() == 0);
    auto first = vertex.Value().DeviceShader();
    auto again = vertex.Value().DeviceShader();
    REQUIRE(first && again);
    CHECK(first.Value() == again.Value());
    CHECK(device.CreatedShaders() == 1 && set.PublishedShaders() == 1);
    CHECK(set.DeviceShader(99).Error() == ErrorCode::InvalidArgument);
    // 다시 열기는 Busy다.
    Array<uint8> bytes(Allocator_());
    CHECK(set.OpenCooked(Move(bytes)).Error() == ErrorCode::Busy);
    set.Retire();
    CHECK(device.RetiredShaders() == 1);
}

TEST(ShaderSet_IncompatibleLibrariesFailWithTheirProblem) {
    Registry types;
    REQUIRE(types.registered);
    RecordingDevice device(Allocator_());
    struct Case {
        LibraryOptions options;
        ShaderSetProblem problem;
        const char* detail;
    };

    Case cases[6];
    cases[0].options.pixel = false;
    cases[0].problem = ShaderSetProblem::MissingEntry;
    cases[0].detail = "SetTestPixel";
    cases[1].options.badSignature = true;
    cases[1].problem = ShaderSetProblem::SignatureMismatch;
    cases[1].detail = "SetTestPixel";
    cases[2].options.wrongTypeSet = true;
    cases[2].problem = ShaderSetProblem::TypeSetMismatch;
    cases[2].detail = "type set";
    cases[3].options.formatId = "D3D11_SM5";
    cases[3].options.backend = ShaderBackend::D3D11;
    cases[3].problem = ShaderSetProblem::WrongFormat;
    cases[3].detail = "D3D11_SM5";
    cases[4].options.backend = ShaderBackend::Vulkan;
    cases[4].problem = ShaderSetProblem::WrongFormat;
    cases[4].detail = "D3D12 device";
    cases[5].problem = ShaderSetProblem::CorruptLibrary;
    cases[5].detail = "cannot be read";
    for (usize index = 0; index < 6; ++index) {
        ShaderSet set(SetDesc(device, types));
        Result<void> opened = Result<void>::Success();
        if (index == 5) {
            Array<uint8> bytes(Allocator_());
            REQUIRE(BuildLibrary(types, {}, bytes));
            bytes[bytes.Size() - 1] ^= 0x40;
            opened = set.OpenCooked(Move(bytes));
        } else {
            opened = OpenWith(set, types, cases[index].options);
        }

        CHECK(!opened && opened.Error() == ErrorCode::DependencyFailed);
        CHECK(set.State() == ShaderSetState::Failed);
        CHECK(set.Problem() == cases[index].problem);
        const StringView detail = set.ProblemDetail();
        CHECK(strstr(detail.Data(), cases[index].detail) != nullptr);
        CHECK(set.FindEntry(SetVertexName.hash).Error() == ErrorCode::NotReady);
        CHECK(!set.TryAcquireLease());
    }
}

TEST(ShaderSet_FirstUseRacePublishesOnceAndRetiresTheLoser) {
    Registry types;
    REQUIRE(types.registered);
    RecordingDevice device(Allocator_());
    ShaderSet set(SetDesc(device, types));
    REQUIRE(OpenWith(set, types, {}));
    auto pixel = set.FindEntry(SetPixelName.hash);
    REQUIRE(pixel);
    // 두 생성이 모두 장치에 들어온 뒤에야 둘 다 돌아온다(명시 gate).
    device.HoldShaderCreation(2);
    Racer racers[2];
    platform::Thread threads[2];
    for (usize index = 0; index < 2; ++index) {
        racers[index].set = &set;
        racers[index].entry = pixel.Value();
        REQUIRE(threads[index].Start(&Racer::Run, &racers[index]) ==
                platform::ThreadStatus::Success);
    }

    for (platform::Thread& thread : threads) {
        CHECK(thread.Join() == platform::ThreadStatus::Success);
    }

    device.HoldShaderCreation(0);
    REQUIRE(racers[0].result && racers[1].result);
    CHECK(racers[0].result.Value() == racers[1].result.Value());
    CHECK(device.CreatedShaders() == 2);
    CHECK(device.RetiredShaders() == 1);
    CHECK(set.PublishedShaders() == 1);
    CHECK(device.LiveShaders() == 1);
}

TEST(ShaderSet_RuntimeFeaturesAndRetireRefuseNewShaders) {
    Registry types;
    REQUIRE(types.registered);
    RecordingDevice device(Allocator_());
    LibraryOptions options;
    options.pixelRuntimeFeatures = ShaderFeatureVertexViewportLayerOutput;
    ShaderSet set(SetDesc(device, types));
    REQUIRE(OpenWith(set, types, options));
    REQUIRE(!device.Caps().features.vertexViewportLayerOutput);
    auto pixel = set.FindEntry(SetPixelName.hash);
    REQUIRE(pixel);
    CHECK(set.DeviceShader(pixel.Value()).Error() == ErrorCode::Unsupported);
    CHECK(device.CreatedShaders() == 0);
    auto vertex = set.FindEntry(SetVertexName.hash);
    REQUIRE(vertex && set.DeviceShader(vertex.Value()));
    REQUIRE(set.TryAcquireLease());
    CHECK(set.LeaseCount() == 1);
    set.Retire();
    CHECK(set.State() == ShaderSetState::Retired);
    CHECK(set.DeviceShader(vertex.Value()).Error() == ErrorCode::Destroyed);
    CHECK(!set.TryAcquireLease());
    CHECK(device.RetiredShaders() == 1 && device.LiveShaders() == 0);
    // 잡은 lease는 Retire 뒤에도 layout을 읽을 수 있다.
    CHECK(set.BindingLayout(vertex.Value()) != nullptr);
    set.ReleaseLease();
}

TEST(ShaderSet_ProviderPathWaitsThenChecks) {
    Registry types;
    REQUIRE(types.registered);
    RecordingDevice device(Allocator_());
    {
        ShaderSet set(SetDesc(device, types));
        auto opened = set.OpenFromProvider(nullptr);
        CHECK(!opened && opened.Error() == ErrorCode::UnsupportedCapability);
        CHECK(set.Problem() == ShaderSetProblem::NoProvider);
    }

    {
        DeferredProvider provider;
        ShaderSet set(SetDesc(device, types));
        REQUIRE(set.OpenFromProvider(&provider));
        CHECK(provider.requests == 1 && provider.formatMatches && provider.target == &set);
        CHECK(set.State() == ShaderSetState::WaitingForProvider);
        CHECK(set.FindEntry(SetVertexName.hash).Error() == ErrorCode::NotReady);
        Array<uint8> bytes(Allocator_());
        REQUIRE(BuildLibrary(types, {}, bytes));
        set.CompleteProvision(Result<void>::Success(), Move(bytes), StringView());
        CHECK(set.WaitUntilSettled() == ShaderSetState::Ready);
        // 두 번째 완료는 무시한다.
        Array<uint8> empty(Allocator_());
        set.CompleteProvision(Result<void>::Failure(ErrorCode::Cancelled), Move(empty), "late");
        CHECK(set.State() == ShaderSetState::Ready);
    }

    {
        // 공급자가 만든 라이브러리도 같은 검사를 받는다.
        DeferredProvider provider;
        ShaderSet set(SetDesc(device, types));
        REQUIRE(set.OpenFromProvider(&provider));
        Array<uint8> bytes(Allocator_());
        LibraryOptions missing;
        missing.pixel = false;
        REQUIRE(BuildLibrary(types, missing, bytes));
        set.CompleteProvision(Result<void>::Success(), Move(bytes), StringView());
        CHECK(set.WaitUntilSettled() == ShaderSetState::Failed);
        CHECK(set.Problem() == ShaderSetProblem::MissingEntry);
    }

    {
        DeferredProvider provider;
        ShaderSet set(SetDesc(device, types));
        REQUIRE(set.OpenFromProvider(&provider));
        Array<uint8> empty(Allocator_());
        set.CompleteProvision(Result<void>::Failure(ErrorCode::DependencyFailed), Move(empty),
                              "compile error in SetTest.rsf");
        CHECK(set.WaitUntilSettled() == ShaderSetState::Failed);
        CHECK(set.Problem() == ShaderSetProblem::ProviderFailed);
        CHECK(strstr(set.ProblemDetail().Data(), "SetTest.rsf") != nullptr);
    }

    {
        DeferredProvider provider;
        provider.refuse = true;
        ShaderSet set(SetDesc(device, types));
        CHECK(set.OpenFromProvider(&provider).Error() == ErrorCode::Unsupported);
        CHECK(set.State() == ShaderSetState::Failed);
        CHECK(set.Problem() == ShaderSetProblem::ProviderFailed);
    }
}

TEST(PipelineCache_HitsMissesLinkAndRetire) {
    Registry types;
    REQUIRE(types.registered);
    RecordingDevice device(Allocator_());
    ShaderSet set(SetDesc(device, types));
    REQUIRE(OpenWith(set, types, {}));
    auto vertex = TypedShader<SetTestVertex>::Find(set);
    auto pixel = TypedShader<SetTestPixel>::Find(set);
    REQUIRE(vertex && pixel);
    {
        PipelineCache cache(Allocator_(), device);
        GraphicsPipelineRequest request;
        request.set = &set;
        request.vertexEntry = vertex.Value().Entry();
        request.pixelEntry = pixel.Value().Entry();
        request.state.colorFormats[0] = graphics::PixelFormat::RGBA8Unorm;
        request.state.drawConstantBytes = 16;
        auto first = cache.FindOrCreate(request);
        auto second = cache.FindOrCreate(request);
        REQUIRE(first && second);
        CHECK(first.Value() == second.Value());
        CHECK(cache.Hits() == 1 && cache.Misses() == 1 && cache.Size() == 1);
        CHECK(device.LastPipelineVertexShader() == vertex.Value().DeviceShader().Value());
        // 상태 하나가 다르면 다른 항목이다.
        request.state.raster.cullMode = graphics::CullMode::Back;
        auto culled = cache.FindOrCreate(request);
        REQUIRE(culled);
        CHECK(!(culled.Value() == first.Value()));
        request.state.bindings.texture = "Albedo";
        REQUIRE(cache.FindOrCreate(request));
        CHECK(cache.Misses() == 3 && cache.Size() == 3);
        CHECK(set.LeaseCount() == 3);
        // VS·PS 순서가 뒤바뀐 요청은 단계 검사로 거부한다.
        GraphicsPipelineRequest swapped = request;
        swapped.vertexEntry = request.pixelEntry;
        swapped.pixelEntry = request.vertexEntry;
        CHECK(cache.FindOrCreate(swapped).Error() == ErrorCode::InvalidArgument);
        cache.RetireSet(set);
        CHECK(cache.Size() == 0 && set.LeaseCount() == 0);
        usize retired = 0;
        for (const RecordingDevice::Call& call : device.Calls()) {
            retired += call.op == RecordingDevice::Op::RetirePipeline ? 1 : 0;
        }

        CHECK(retired == 3);
    }

    {
        // 단계 사이 변수가 맞지 않는 쌍은 만들지 않는다(CheckShaderStageLink).
        ShaderSet linked(SetDesc(device, types));
        LibraryOptions options;
        options.vertexInterStage = 0b01;
        options.pixelInterStage = 0b10;
        REQUIRE(OpenWith(linked, types, options));
        PipelineCache cache(Allocator_(), device);
        GraphicsPipelineRequest request;
        request.set = &linked;
        request.vertexEntry = TypedShader<SetTestVertex>::Find(linked).Value().Entry();
        request.pixelEntry = TypedShader<SetTestPixel>::Find(linked).Value().Entry();
        const uint64 calls = device.PipelineCreateCalls();
        CHECK(cache.FindOrCreate(request).Error() == ErrorCode::InvalidArgument);
        CHECK(cache.LastLinkFailure()[0] != '\0');
        CHECK(device.PipelineCreateCalls() == calls && cache.Size() == 0);
        linked.Retire();
        CHECK(cache.FindOrCreate(request).Error() == ErrorCode::Destroyed);
    }
}

TEST(ShaderBindingMap_RootConstantsAndBlocks) {
    ShaderBindingLayout vertex(Allocator_());
    vertex.stage = ShaderStageVertex;
    ShaderBindingLayout pixel(Allocator_());
    pixel.stage = ShaderStagePixel;
    pixel.rootConstantBytes = 16;
    const ShaderBindingLayout* stages[] = {&vertex, &pixel};
    auto map = ShaderBindingMap::Build(TintLayouts[0], stages);
    REQUIRE(map);
    REQUIRE(map.Value().Placements().Size() == 1);
    CHECK(map.Value().Placements()[0].stage == ShaderStagePixel &&
          map.Value().Placements()[0].rootConstants && map.Value().Placements()[0].bytes == 16);
    uint32 offset = 1;
    uint32 bytes = 0;
    CHECK(map.Value().FindMember("tint", offset, bytes) && offset == 0 && bytes == 16);
    CHECK(map.Value().FindMember("missing", offset, bytes).Error() == ErrorCode::NotFound);
    // 단계 블록이 표보다 크거나 어느 단계도 읽지 않으면 거부한다.
    pixel.rootConstantBytes = 32;
    const char* reason = nullptr;
    CHECK(!ShaderBindingMap::Build(TintLayouts[0], stages, &reason) && reason != nullptr);
    pixel.rootConstantBytes = 0;
    CHECK(!ShaderBindingMap::Build(TintLayouts[0], stages, &reason));

    // 상수 버퍼 표는 멤버 이름·오프셋을 단계 layout과 대조한다.
    static constexpr ShaderParameterMember viewMembers[] = {
        {"scale", ShaderParameterType::Float4, 0, 0}};
    static constexpr ShaderParameterLayout viewLayout = {"View", false, 16, viewMembers};
    ShaderBindingMember member{"scale", 0, 16};
    REQUIRE(pixel.members.TryPushBack(member));
    ShaderBindingResource buffer;
    buffer.kind = ShaderResourceKind::ConstantBuffer;
    buffer.slot = 2;
    buffer.name = "View";
    buffer.constantBufferBytes = 16;
    buffer.memberCount = 1;
    REQUIRE(pixel.resources.TryPushBack(buffer));
    auto view = ShaderBindingMap::Build(viewLayout, stages);
    REQUIRE(view);
    CHECK(view.Value().Placements()[0].slot == 2 && !view.Value().Placements()[0].rootConstants);
    pixel.members[0].offset = 4;
    CHECK(!ShaderBindingMap::Build(viewLayout, stages, &reason));

    ConstantBlock block(Allocator_(), TintLayouts[0], ConstantBlockLifetime::PerDraw);
    REQUIRE(block.Initialize());
    const float tint[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    const uint64 version = block.Version();
    REQUIRE(block.SetValue("tint", tint));
    CHECK(block.Version() == version + 1);
    CHECK(memcmp(block.Bytes().Data(), tint, sizeof(tint)) == 0);
    CHECK(block.Set("tint", Span<const uint8>(block.Bytes().Data(), 4)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(block.SetValue("nothing", tint).Error() == ErrorCode::NotFound);

    // 업로드 판단: 같은 program·slot의 같은 블록은 건너뛰고 program을 바꾸면 다시 올린다.
    ConstantUploadTracker tracker(Allocator_());
    CHECK(tracker.NeedsUpload(1, 0, block));
    CHECK(!tracker.NeedsUpload(1, 0, block));
    CHECK(tracker.NeedsUpload(2, 0, block));
    REQUIRE(block.SetValue("tint", tint));
    CHECK(tracker.NeedsUpload(1, 0, block));
    tracker.ForgetProgram(2);
    CHECK(tracker.NeedsUpload(2, 0, block));
    CHECK(tracker.Uploads() == 4 && tracker.Skips() == 1);
}
