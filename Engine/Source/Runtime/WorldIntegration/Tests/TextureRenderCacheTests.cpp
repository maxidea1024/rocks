#include <AssetRuntime/Tests/LoaderTestSupport.h>
#include <RenderBase/Tests/RecordingDevice.h>

#include <AssetRuntime/AssetManager.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Move.h>
#include <Core/Text/StringBuilder.h>
#include <Meta/GeneratedType.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <RenderBase/RenderLane.h>
#include <TestSupport/Test.h>
#include <Texture/Texture.generated.h>
#include <Texture/Texture.h>
#include <WorldIntegration/Textures/TextureRenderCache.h>

// 텍스처 렌더 캐시 시험이다(Docs/Design/RenderBase 18.5절 8항 — SM8). 쿠킹 Texture2D 패키지를 쓰고
// 손으로 돌리는 로더 IO·worker 위의 AssetManager로 읽고, Inline 렌더 lane과 기록 장치에 올린다.
// 순서는 모두 시험이 정한다(Settle·lane 표지 대기 — 시간·프레임 수에 기대지 않는다).
using namespace rocks;
using namespace rocks::asset_test;
using rocks::render::testing::RecordingDevice;

namespace texture_render_cache_tests_file {

// 시험 텍스처 크기·mip이다(2D RGBA8 4×4, mip 셋 — mip 0 스트리밍, mip 1·2 꼬리).
constexpr uint32 Side = 4;
constexpr uint32 MipCount = 3;
constexpr uint32 ResidentTailMips = 2;
constexpr uint32 Mip0Bytes = Side * Side * 4;
// 꼬리: mip 1(2×2 = 16 byte) 오프셋 0, mip 2(1×1 = 4 byte) 오프셋 16(16 byte 정렬).
constexpr uint32 TailBytes = 20;
constexpr uint64 TailMip2Offset = 16;

// 장면 텍스처 하나의 정의다.
struct TextureNode {
    uint64 id = 0;
    TextureAvailability availability = TextureAvailability::GPU;
    // 참이면 mip 0의 bulk ref를 비운다(변환의 CorruptEntry).
    bool emptyMipBulk = false;
    // 참이면 mip 0 byte 수를 1 늘린다(검사의 MipByteSize).
    bool wrongByteSize = false;
};

// 쿠킹 텍스처 장면·로더·AssetManager·렌더 lane·캐시다. 파괴 순서: 캐시 Shutdown → AssetManager
// Shutdown(늦은 callback이 캐시가 살아 있을 때 온다) → 캐시 → AssetManager → lane Shutdown.
struct CacheRig {
    CacheRig() noexcept
        : types(allocator), modules(types, allocator), names(allocator), manifest(allocator),
          files(allocator), paths(allocator), mip0(allocator), tail(allocator), io(allocator),
          worker(allocator), device(allocator), lane(allocator, device) {
        ready = RegisterTextureTypes(modules).IsOk() && names.Initialize().IsOk();
        RegistryOptions options;
        options.names = &names;
        options.policy.packageNativeType = NativeTypeKey<Package>();
        options.policy.rejectWorldOnly = true;
        constexpr uint64 SourceContextHigh = 0x5452434143484553ull;
        auto made = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                       ContextId{SourceContextHigh, 1}, options);
        if (made) {
            source = Move(made.Value());
        }

        for (uint32 index = 0; index < Mip0Bytes; ++index) {
            ready = ready && mip0.TryPushBack(static_cast<uint8>(index)).IsOk();
        }

        for (uint32 index = 0; index < TailBytes; ++index) {
            ready = ready && tail.TryPushBack(static_cast<uint8>(0x80 + index)).IsOk();
        }

        // 장치 능력: 넉넉한 한도와 모든 형식 능력(업로드 검사가 통과하게).
        constexpr uint32 LimitSide = 16384;
        constexpr uint32 LimitLayers = 2048;
        graphics::DeviceCaps& caps = device.MutableCaps();
        caps.limits.textureSize2D = LimitSide;
        caps.limits.textureSize3D = LimitSide;
        caps.limits.textureSizeCube = LimitSide;
        caps.limits.textureArrayLayers = LimitLayers;
        caps.features.textureCubeArray = true;
        device.SetPixelFormatCaps(
            graphics::PixelFormatCaps::Sampled | graphics::PixelFormatCaps::Texture3D |
            graphics::PixelFormatCaps::TextureCube | graphics::PixelFormatCaps::Filterable |
            graphics::PixelFormatCaps::Mipmaps);
        ready = ready && source.Get() != nullptr;
    }

    ~CacheRig() noexcept {
        Close();
        source.Reset();
        CHECK(modules.Unregister());
    }

    // 텍스처 패키지 하나를 쿠킹 byte로 쓰고 매니페스트·IO에 더한다.
    bool AddTexture(const TextureNode& node) noexcept {
        ObjectRegistry& registry = *source;
        const ObjectAccessContext access = registry.OwnerAccess();
        auto package = NewPackage(registry, access, FixedId(node.id), PackageKind::Cooked);
        if (!package) {
            return false;
        }

        const ObjectHandle texture =
            Make<Texture2D>(registry, GeneratedType<Texture2D>::Id(), package.Value().Handle(), "");
        TestBulkSource bulk;
        const BulkDataRef mipRef = bulk.Add(1, mip0.View());
        const BulkDataRef tailRef = bulk.Add(2, tail.View());
        const bool filled = WithBorrow<Texture2D>(registry, texture, [&](Texture2D& value) {
            value.availability = node.availability;
            TexturePlatformData& data = value.platformData;
            data.dimension = TextureDimension::Texture2D;
            data.pixelFormat =
                static_cast<uint8>(graphics::PixelFormatNumber(graphics::PixelFormat::RGBA8Unorm));
            data.sizeX = Side;
            data.sizeY = Side;
            data.sizeZ = 1;
            data.numSlices = 1;
            data.numMips = MipCount;
            data.residentTailMips = ResidentTailMips;
            data.tailBulk = tailRef;
            data.swizzleA = TextureSwizzle::One;
            const uint32 sides[MipCount] = {Side, Side / 2, Side / 4};
            const uint64 offsets[MipCount] = {0, 0, TailMip2Offset};
            for (uint32 level = 0; level < MipCount; ++level) {
                TextureMipData mip;
                mip.sizeX = sides[level];
                mip.sizeY = sides[level];
                mip.sizeZ = 1;
                mip.byteSize = static_cast<uint64>(sides[level]) * sides[level] * 4;
                mip.tailOffset = offsets[level];
                if (level == 0) {
                    mip.bulk = node.emptyMipBulk ? BulkDataRef{} : mipRef;
                    mip.byteSize += node.wrongByteSize ? 1u : 0u;
                }

                if (!data.mips.TryPushBack(mip)) {
                    return false;
                }
            }

            return true;
        });
        if (!filled) {
            return false;
        }

        PackageWriteOptions options = CookedOptions();
        options.bulkSource = &bulk;
        Array<uint8> file = Write(allocator, registry, package.Value().Handle(), options);
        if (file.IsEmpty()) {
            return false;
        }

        auto added = manifest.entries.TryEmplaceBack(allocator);
        if (!added || !FillLoaderEntry(allocator, *added.Value(), file, node.id)) {
            return false;
        }

        StringBuilder<128> path(allocator);
        String copy(allocator);
        if (!LoaderPackagePath(path, root, node.id, added.Value()->packageSha256) ||
            !copy.TryAssign(path.View()) || !paths.TryPushBack(Move(copy)) ||
            !files.TryPushBack(Move(file))) {
            return false;
        }

        return true;
    }

    // 매니페스트를 검증하고 AssetManager·lane·캐시를 시작한다(매니페스트는 AssetId 오름차순 —
    // AddTexture를 id 오름차순으로 부른다).
    bool Start() noexcept {
        FillManifestHeader(manifest);
        if (!ValidateCookedManifest(manifest, allocator)) {
            return false;
        }

        for (usize index = 0; index < files.Size(); ++index) {
            if (!io.Add(paths[index].View(), files[index].View())) {
                return false;
            }
        }

        constexpr uint64 ManagerContextHigh = 0x5452434d4752ull;
        AssetManagerConfig config;
        config.loader.allocator = &allocator;
        config.loader.types = &types;
        config.loader.manifest = &manifest;
        config.loader.cookedRoot = root;
        config.loader.runtimeProfile = "Windows";
        config.loader.io = &io;
        config.loader.worker = &worker;
        config.registryContext = ContextId{ManagerContextHigh, 1};
        config.registryOptions.names = &names;
        auto made = UniquePtr<AssetManager>::TryMake(allocator, config);
        if (!made) {
            return false;
        }

        manager = Move(made.Value());
        render::RenderLaneConfig laneConfig;
        laneConfig.mode = render::RenderLaneMode::Inline;
        laneConfig.hangTimeoutMs = 0;
        if (!manager->Initialize() || !lane.Start(laneConfig)) {
            return false;
        }

        laneStarted = true;
        auto cacheMade =
            UniquePtr<TextureRenderCache>::TryMake(allocator, allocator, *manager, lane, device);
        if (!cacheMade) {
            return false;
        }

        cache = Move(cacheMade.Value());
        return true;
    }

    // 로더 IO·worker를 내보내며 Pump를 로더 단위가 없어질 때까지 되풀이한다(횟수 상한).
    bool Settle() noexcept {
        constexpr uint32 SettleRounds = 1000;
        for (uint32 round = 0; round < SettleRounds; ++round) {
            io.Complete();
            worker.RunAll();
            if (!manager->Pump()) {
                return false;
            }

            AssetManagerSnapshot snapshot(allocator);
            if (!manager->Describe(snapshot)) {
                return false;
            }

            if (snapshot.loader.units.IsEmpty() && io.held.IsEmpty() && worker.pending.IsEmpty() &&
                io.active == 0) {
                return true;
            }
        }

        return false;
    }

    // 상주 단위 수다(lease를 놓고 Settle하면 0이 된다).
    usize ResidentUnits() noexcept {
        AssetManagerSnapshot snapshot(allocator);
        return manager->Describe(snapshot) ? snapshot.units.Size() : 0;
    }

    // 지금까지 게시한 lane 작업을 끝내고 통지를 캐시에 넘긴다. 캐시가 바꾼 항목 수다.
    uint32 DeliverNotices() noexcept {
        auto marker = lane.PostMarker();
        if (!marker || !lane.WaitMarker(marker.Value())) {
            return 0;
        }

        notices.Clear();
        if (!lane.Notices().TakeAll(notices)) {
            return 0;
        }

        return cache->HandleNotices(notices.View());
    }

    uint32 CountOps(RecordingDevice::Op op) const noexcept {
        uint32 count = 0;
        for (const RecordingDevice::Call& call : device.Calls()) {
            count += call.op == op ? 1u : 0u;
        }

        return count;
    }

    TextureRenderInfo InfoOf(uint64 id) const noexcept {
        TextureRenderInfo info;
        info.state = static_cast<TextureRenderState>(0xff);
        (void)cache->Find(FixedId(id), info);
        return info;
    }

    // 파괴 순서(위 주석)대로 닫는다. 두 번째 호출은 무동작이다.
    void Close() noexcept {
        if (cache) {
            cache->Shutdown();
        }

        if (manager) {
            CHECK(manager->Shutdown());
        }

        cache.Reset();
        manager.Reset();
        if (laneStarted) {
            laneStarted = false;
            CHECK(lane.Shutdown());
            CHECK(lane.Handles().Count() == 0 && device.PendingRetireCount() == 0);
        }
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    NamePool names;
    UniquePtr<ObjectRegistry> source;
    CookedManifest manifest;
    Array<Array<uint8>> files;
    Array<String> paths;
    Array<uint8> mip0;
    Array<uint8> tail;
    StringView root = "cooked";
    TestPackageIo io;
    TestLoaderWorker worker;
    UniquePtr<AssetManager> manager;
    RecordingDevice device;
    render::RenderLane lane;
    bool laneStarted = false;
    UniquePtr<TextureRenderCache> cache;
    Array<render::RenderNotice> notices{allocator};
    bool ready = false;
};

constexpr uint64 TextureA = 11;
constexpr uint64 TextureB = 12;

// 양성: 로드 → Update(Uploading, handle 예약·장치 생성 한 번) → 통지(Ready), 표시 값. 같은 id 두
// 번 Acquire는 사용자 둘·요청 하나, 첫 Release 뒤에도 Ready, 마지막 Release가 Retire 뒤 lease를
// 놓는다(상주 단위 0).
TEST(WorldIntegration_TextureRenderCacheUploadsAndReleases) {
    CacheRig rig;
    REQUIRE(rig.ready);
    REQUIRE(rig.AddTexture({TextureA}));
    REQUIRE(rig.Start());
    TextureRenderCache& cache = *rig.cache;
    REQUIRE(cache.Acquire(FixedId(TextureA)));
    REQUIRE(cache.Acquire(FixedId(TextureA)));
    CHECK(cache.Count() == 1 && cache.UsersOf(FixedId(TextureA)) == 2);
    CHECK(rig.InfoOf(TextureA).state == TextureRenderState::Loading);
    CHECK(cache.Update() == 0);
    REQUIRE(rig.Settle());
    CHECK(rig.InfoOf(TextureA).state == TextureRenderState::Loading);
    CHECK(cache.Update() == 1);
    const TextureRenderInfo uploading = rig.InfoOf(TextureA);
    CHECK(uploading.state == TextureRenderState::Uploading && !uploading.handle.IsNull());
    CHECK(uploading.dimension == graphics::TextureDimension::Texture2D);
    CHECK(uploading.format == graphics::PixelFormat::RGBA8Unorm);
    CHECK(uploading.width == Side && uploading.height == Side && uploading.depth == 1 &&
          uploading.slices == 1 && uploading.mips == MipCount);
    CHECK(uploading.swizzle.alpha == graphics::SwizzleSource::One &&
          uploading.swizzle.red == graphics::SwizzleSource::Red);
    CHECK(cache.Update() == 0);
    CHECK(rig.DeliverNotices() == 1);
    CHECK(rig.InfoOf(TextureA).state == TextureRenderState::Ready);
    CHECK(rig.InfoOf(TextureA).handle == uploading.handle);
    CHECK(rig.CountOps(RecordingDevice::Op::CreateTexture) == 1);
    CHECK(rig.lane.Handles().Texture(uploading.handle));
    CHECK(cache.CountIn(TextureRenderState::Ready) == 1);
    CHECK(rig.ResidentUnits() == 1);
    REQUIRE(cache.Release(FixedId(TextureA)));
    CHECK(rig.InfoOf(TextureA).state == TextureRenderState::Ready);
    CHECK(rig.lane.Handles().Count() == 1);
    REQUIRE(cache.Release(FixedId(TextureA)));
    CHECK(cache.Count() == 0);
    TextureRenderInfo missing;
    CHECK(!cache.Find(FixedId(TextureA), missing));
    auto again = cache.Release(FixedId(TextureA));
    CHECK(!again && again.Error() == ErrorCode::NotFound);
    REQUIRE(rig.lane.DrainRetires());
    CHECK(rig.lane.Handles().Count() == 0);
    CHECK(rig.CountOps(RecordingDevice::Op::RetireTexture) == 1);
    REQUIRE(rig.Settle());
    CHECK(rig.ResidentUnits() == 0);
}

// CPU Availability는 올리지 않는다: CpuPlaceholder·handle 없음·장치 생성 없음, 쿠킹 데이터는
// lease로 읽을 수 있다(WithPlatformData).
TEST(WorldIntegration_TextureRenderCacheCpuAvailability) {
    CacheRig rig;
    REQUIRE(rig.ready);
    TextureNode node{TextureA};
    node.availability = TextureAvailability::CPU;
    REQUIRE(rig.AddTexture(node));
    REQUIRE(rig.Start());
    TextureRenderCache& cache = *rig.cache;
    REQUIRE(cache.Acquire(FixedId(TextureA)));
    REQUIRE(rig.Settle());
    CHECK(cache.Update() == 1);
    const TextureRenderInfo info = rig.InfoOf(TextureA);
    CHECK(info.state == TextureRenderState::CpuPlaceholder && info.handle.IsNull());
    CHECK(rig.CountOps(RecordingDevice::Op::CreateTexture) == 0);
    CHECK(rig.lane.Handles().Count() == 0);
    struct Visit {
        uint32 sizeX = 0;
        uint64 mip0Bytes = 0;
    } visit;
    REQUIRE(cache.WithPlatformData(
        FixedId(TextureA),
        [](void* raw, const TexturePlatformData& data, PlatformBulkReader reader,
           void* readerContext) noexcept {
            auto& out = *static_cast<Visit*>(raw);
            out.sizeX = data.sizeX;
            auto bytes = reader(readerContext, data.mips[0].bulk);
            out.mip0Bytes = bytes ? bytes.Value().Size() : 0;
        },
        &visit));
    CHECK(visit.sizeX == Side && visit.mip0Bytes == Mip0Bytes);
    REQUIRE(cache.Release(FixedId(TextureA)));
    REQUIRE(rig.Settle());
    CHECK(rig.ResidentUnits() == 0);
}

// 변환·검사 실패: 빈 mip bulk ref는 Failed(InvalidArgument, handle 없음), mip byte 수 불일치는
// Failed(MipByteSize — 예약한 handle을 그 자리에서 Retire)다. 장치 생성은 없다.
TEST(WorldIntegration_TextureRenderCacheConversionFailures) {
    CacheRig rig;
    REQUIRE(rig.ready);
    TextureNode empty{TextureA};
    empty.emptyMipBulk = true;
    TextureNode wrong{TextureB};
    wrong.wrongByteSize = true;
    REQUIRE(rig.AddTexture(empty));
    REQUIRE(rig.AddTexture(wrong));
    REQUIRE(rig.Start());
    TextureRenderCache& cache = *rig.cache;
    REQUIRE(cache.Acquire(FixedId(TextureA)));
    REQUIRE(cache.Acquire(FixedId(TextureB)));
    REQUIRE(rig.Settle());
    CHECK(cache.Update() == 2);
    const TextureRenderInfo corrupt = rig.InfoOf(TextureA);
    CHECK(corrupt.state == TextureRenderState::Failed && corrupt.handle.IsNull());
    CHECK(corrupt.error == ErrorCode::InvalidArgument &&
          corrupt.issue == render::CookedTextureIssue::None);
    const TextureRenderInfo checked = rig.InfoOf(TextureB);
    CHECK(checked.state == TextureRenderState::Failed && checked.handle.IsNull());
    CHECK(checked.error == ErrorCode::InvalidArgument &&
          checked.issue == render::CookedTextureIssue::MipByteSize);
    CHECK(cache.Update() == 0);
    REQUIRE(rig.lane.DrainRetires());
    CHECK(rig.lane.Handles().Count() == 0);
    CHECK(rig.CountOps(RecordingDevice::Op::CreateTexture) == 0);
    CHECK(cache.CountIn(TextureRenderState::Failed) == 2);
    REQUIRE(cache.Release(FixedId(TextureA)));
    REQUIRE(cache.Release(FixedId(TextureB)));
    CHECK(cache.Count() == 0);
}

// 장치 생성 실패: ResourceFailed → Failed(오류, handle 있음 — 그리면 typed 대체), Release가
// Retire한다.
TEST(WorldIntegration_TextureRenderCacheDeviceFailure) {
    CacheRig rig;
    REQUIRE(rig.ready);
    REQUIRE(rig.AddTexture({TextureA}));
    rig.device.SetCreateTextureError(ErrorCode::OutOfMemory);
    REQUIRE(rig.Start());
    TextureRenderCache& cache = *rig.cache;
    REQUIRE(cache.Acquire(FixedId(TextureA)));
    REQUIRE(rig.Settle());
    CHECK(cache.Update() == 1);
    CHECK(rig.DeliverNotices() == 1);
    const TextureRenderInfo failed = rig.InfoOf(TextureA);
    CHECK(failed.state == TextureRenderState::Failed && !failed.handle.IsNull());
    CHECK(failed.error == ErrorCode::OutOfMemory);
    CHECK(rig.lane.Handles().Count() == 1);
    REQUIRE(cache.Release(FixedId(TextureA)));
    REQUIRE(rig.lane.DrainRetires());
    CHECK(rig.lane.Handles().Count() == 0);
}

// 요청·로드 실패: null id InvalidArgument, 매니페스트에 없는 id는 RequestAsset 오류(항목 없음), IO
// 실패는 준비 callback이 Failed로 적는다(lease·handle 없음, WithPlatformData NotReady).
TEST(WorldIntegration_TextureRenderCacheLoadFailures) {
    CacheRig rig;
    REQUIRE(rig.ready);
    REQUIRE(rig.AddTexture({TextureA}));
    REQUIRE(rig.Start());
    TextureRenderCache& cache = *rig.cache;
    auto null = cache.Acquire(AssetId{});
    CHECK(!null && null.Error() == ErrorCode::InvalidArgument);
    constexpr uint64 UnknownTexture = 99;
    auto unknown = cache.Acquire(FixedId(UnknownTexture));
    CHECK(!unknown && unknown.Error() == ErrorCode::NotFound);
    CHECK(cache.Count() == 0);
    TestPackageIo::File* file = rig.io.FindFile(rig.paths[0].View());
    REQUIRE(file != nullptr);
    file->completeError = ErrorCode::IoError;
    REQUIRE(cache.Acquire(FixedId(TextureA)));
    REQUIRE(rig.Settle());
    CHECK(cache.Update() == 0);
    const TextureRenderInfo info = rig.InfoOf(TextureA);
    CHECK(info.state == TextureRenderState::Failed && info.handle.IsNull());
    CHECK(info.error != ErrorCode::None);
    auto inspect = cache.WithPlatformData(
        FixedId(TextureA),
        [](void*, const TexturePlatformData&, PlatformBulkReader, void*) noexcept {}, nullptr);
    CHECK(!inspect && inspect.Error() == ErrorCode::NotReady);
    REQUIRE(cache.Release(FixedId(TextureA)));
}

// 늦은 callback: Loading 중 Release하면 그 callback(Cancelled)은 무동작이고 업로드가 없다. Release
// 뒤 다시 Acquire하면 옛 callback은 순번이 달라 무시되고 새 요청만 올린다. 다른 handle·종류의
// 통지는 무시한다.
TEST(WorldIntegration_TextureRenderCacheLateCallbacks) {
    CacheRig rig;
    REQUIRE(rig.ready);
    REQUIRE(rig.AddTexture({TextureA}));
    REQUIRE(rig.AddTexture({TextureB}));
    REQUIRE(rig.Start());
    TextureRenderCache& cache = *rig.cache;
    REQUIRE(cache.Acquire(FixedId(TextureA)));
    REQUIRE(cache.Release(FixedId(TextureA)));
    REQUIRE(cache.Acquire(FixedId(TextureB)));
    REQUIRE(cache.Release(FixedId(TextureB)));
    REQUIRE(cache.Acquire(FixedId(TextureB)));
    CHECK(cache.Count() == 1 && cache.UsersOf(FixedId(TextureB)) == 1);
    REQUIRE(rig.Settle());
    CHECK(cache.Count() == 1);
    CHECK(cache.Update() == 1);
    CHECK(rig.InfoOf(TextureB).state == TextureRenderState::Uploading);
    TextureRenderInfo missing;
    CHECK(!cache.Find(FixedId(TextureA), missing));
    const render::RenderNotice foreign[] = {
        {render::RenderNoticeKind::ResourceReady, ErrorCode::None, 0, render::RenderHandle{}, 0},
        {render::RenderNoticeKind::FrameCompleted, ErrorCode::None, 1, rig.InfoOf(TextureB).handle,
         0}};
    CHECK(cache.HandleNotices(Span<const render::RenderNotice>(foreign)) == 0);
    CHECK(rig.InfoOf(TextureB).state == TextureRenderState::Uploading);
    CHECK(rig.DeliverNotices() == 1);
    CHECK(rig.InfoOf(TextureB).state == TextureRenderState::Ready);
    CHECK(rig.CountOps(RecordingDevice::Op::CreateTexture) == 1);
    REQUIRE(cache.Release(FixedId(TextureB)));
}

// 종료: Shutdown이 모든 handle을 Retire하고 lease·티켓을 놓는다(Retire 게시가 먼저 — 항목 소멸이
// lease를 놓는다). 이후 Acquire ShuttingDown, Update 무동작, 두 번째 Shutdown 무동작이다. 놓은
// 티켓의 늦은 통지는 AssetManager Shutdown이 전달해도 무동작이다.
TEST(WorldIntegration_TextureRenderCacheShutdownOrder) {
    CacheRig rig;
    REQUIRE(rig.ready);
    REQUIRE(rig.AddTexture({TextureA}));
    REQUIRE(rig.AddTexture({TextureB}));
    REQUIRE(rig.Start());
    TextureRenderCache& cache = *rig.cache;
    REQUIRE(cache.Acquire(FixedId(TextureA)));
    REQUIRE(rig.Settle());
    CHECK(cache.Update() == 1);
    CHECK(rig.DeliverNotices() == 1);
    REQUIRE(cache.Acquire(FixedId(TextureB)));
    CHECK(rig.lane.Handles().Count() == 1);
    cache.Shutdown();
    CHECK(cache.IsShutDown() && cache.Count() == 0);
    auto late = cache.Acquire(FixedId(TextureA));
    CHECK(!late && late.Error() == ErrorCode::ShuttingDown);
    CHECK(cache.Update() == 0);
    cache.Shutdown();
    REQUIRE(rig.lane.DrainRetires());
    CHECK(rig.lane.Handles().Count() == 0);
    CHECK(rig.CountOps(RecordingDevice::Op::RetireTexture) == 1);
    REQUIRE(rig.Settle());
    CHECK(rig.ResidentUnits() == 0);
    CHECK(rig.CountOps(RecordingDevice::Op::CreateTexture) == 1);
}

} // namespace texture_render_cache_tests_file
