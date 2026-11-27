#include "LoaderTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>
#include <Core/ScopeExit.h>
#include <Platform/AsyncFile.h>
#include <Platform/File.h>
#include <Platform/Process.h>

#include <stdio.h>

// 로더의 sidecar(스트리밍 bulk) 시험이다(Docs/Design/AssetSystem 20.1 7, 20.4 로드): 로더는 sidecar
// byte를 하나도 읽지 않고(IO 복사 byte 계수) 크기만 IO가 잰다(패키지를 다 읽은 뒤 한 번), Prepare
// 할당 최댓값은 sidecar보다 작고, 매니페스트 bulkRawBytes는 sidecar를 빼고 센다. sidecar 크기가
// 다르거나(짧음·김) 없으면 SidecarBytes 칸의 CorruptPackage, Stat의 다른 IO 오류는 그 오류다.
// 디스크 IO(AsyncCookedPackageIo)도 같은 판정이다.
using namespace rocks;
namespace loader_sidecar_tests_file {

using namespace rocks::asset_test;

namespace {

// 크게 잡은 sidecar bulk다(Prepare 할당이 이보다 작음을 본다 — 패키지 안 Zstd bulk의 블록 버퍼
// 2^PackageBulkBlockSizeLog2보다 충분히 크다).
constexpr uint32 StreamedBytes = (uint32{1} << PackageBulkBlockSizeLog2) * 16;
constexpr uint32 ResidentBytes = 3000;

// 가장 큰 할당 한 번의 byte를 적는 할당기다.
class LargestAllocation final : public Allocator {
public:
    explicit LargestAllocation(Allocator& inner) noexcept : inner_(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        largest = bytes > largest ? bytes : largest;
        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

    usize largest = 0;

private:
    Allocator& inner_;
};

// 장면: 1(sidecar bulk) → 2(패키지 안 bulk) Hard, 3(sidecar bulk — 단독).
bool CookScene(LoaderRig& rig) noexcept {
    rig.scene.Add(1, StreamedBytes).sidecar = true;
    rig.scene.Add(2, ResidentBytes);
    rig.scene.Add(3, ResidentBytes).sidecar = true;
    rig.scene.Hard(1, 2);
    return rig.Cook();
}

AssetLoadStatus LoadOne(LoaderRig& rig, uint64 id) noexcept {
    if (!rig.Start()) {
        return AssetLoadStatus{};
    }

    const AssetLoadHandle handle = rig.Request(id);
    if (!rig.PumpUntilSettled()) {
        return AssetLoadStatus{};
    }

    return rig.StatusOf(handle);
}

bool SidecarCorrupt(const AssetLoadStatus& status, uint64 id) noexcept {
    return status.state == AssetLoadState::Failed && status.error == ErrorCode::CorruptData &&
           status.diagnostic.code == AssetDiagnosticCode::CorruptPackage &&
           status.diagnostic.section == static_cast<uint32>(CookedManifestField::SidecarBytes) &&
           status.diagnostic.detail == CookedManifestNoEntry &&
           status.diagnostic.asset == FixedId(id);
}

bool WriteWholeFile(StringView path, Span<const uint8> bytes) noexcept {
    platform::File file;
    if (!file.Open(platform::PhysicalFileSystem(), path.Data(), path.ByteCount(),
                   platform::FileOpenMode::CreateAlways)) {
        return false;
    }

    auto written = file.WriteAll(bytes.Data(), bytes.Size());
    return written.status && written.bytes == bytes.Size() && file.Close();
}

// path의 부모 디렉터리를 만든다.
bool MakeParent(StringView path) noexcept {
    usize slash = path.ByteCount();
    while (slash > 0 && path.Data()[slash - 1] != '/') {
        --slash;
    }

    if (slash <= 1) {
        return true;
    }

    return static_cast<bool>(
        platform::MakeDirectoryTree(platform::PhysicalFileSystem(), path.Data(), slash - 1));
}

} // namespace

// sidecar를 읽지 않는다(AS 20.1 7): 1(sidecar 4 MiB) → 2를 로드하면 IO가 복사한 byte는 두 패키지
// 파일뿐이고 sidecar 경로로 Begin하지 않으며 sidecar Stat은 1의 한 번이다. 1의 bulkRawBytes는 0,
// sidecarBytes는 sidecar 크기이고, 로더 할당기의 가장 큰 할당은 sidecar보다 작다. 공개된 1의
// sidecar bulk는 BulkBytes가 Unsupported, 2의 bulk는 byte다.
TEST(AssetLoader_SidecarIsNotRead) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    REQUIRE(CookScene(rig));
    LargestAllocation tracking(env.allocator);
    rig.config.allocator = &tracking;
    const CookedManifestEntry& one = rig.scene.EntryOf(1);
    const CookedManifestEntry& two = rig.scene.EntryOf(2);
    CHECK(one.bulkRawBytes == 0 && one.sidecarBytes == rig.scene.SidecarOf(1).Size());
    CHECK(one.sidecarBytes > StreamedBytes && two.sidecarBytes == 0);
    CHECK(two.bulkRawBytes == ResidentBytes && rig.scene.SidecarOf(2).IsEmpty());
    const AssetLoadStatus status = LoadOne(rig, 1);
    CHECK(status.state == AssetLoadState::Succeeded);
    CHECK(rig.io.bytesRead == one.packageBytes + two.packageBytes);
    CHECK(rig.io.sidecarStats == 1);
    for (const String& begun : rig.io.begun) {
        CHECK(begun.View() != rig.scene.SidecarPathOf(1));
    }

    CHECK(tracking.largest < StreamedBytes);
    auto package = rig.loader->FindPackage(FixedId(1));
    REQUIRE(package);
    Package* pointer = Pointer<Package>(*env.target, package.Value());
    TestAsset* asset = Pointer<TestAsset>(*env.target, rig.AssetOf(1));
    REQUIRE(pointer != nullptr && asset != nullptr);
    CHECK(pointer->BulkBytes(asset->pixels).Error() == ErrorCode::Unsupported);
    auto resident = rig.loader->FindPackage(FixedId(2));
    REQUIRE(resident);
    Package* residentPackage = Pointer<Package>(*env.target, resident.Value());
    TestAsset* residentAsset = Pointer<TestAsset>(*env.target, rig.AssetOf(2));
    REQUIRE(residentPackage != nullptr && residentAsset != nullptr);
    auto bytes = residentPackage->BulkBytes(residentAsset->pixels);
    REQUIRE(bytes);
    CHECK(bytes.Value().Size() == ResidentBytes);
    REQUIRE(rig.loader->Shutdown());
}

// sidecar 크기 대조(AS 20.1 7): 짧음·김·없음은 SidecarBytes 칸의 CorruptPackage이고, Stat의 다른 IO
// 오류는 그 오류(진단 None)다. 패키지 크기가 다르면 sidecar를 재지 않는다(PackageBytes가 먼저).
TEST(AssetLoader_SidecarSizeIsChecked) {
    // 0 짧음, 1 김, 2 없음, 3 IO 오류, 4 패키지 크기 다름.
    for (int kind = 0; kind < 5; ++kind) {
        AssetEnv env;
        REQUIRE(env.Ready());
        LoaderRig rig(env);
        REQUIRE(CookScene(rig));
        TestPackageIo::File* sidecar = rig.io.FindFile(rig.scene.SidecarPathOf(3));
        REQUIRE(sidecar != nullptr);
        Array<uint8> longer = Copy(env.allocator, rig.scene.SidecarOf(3).View());
        REQUIRE(longer.TryPushBack(0));
        if (kind == 0) {
            sidecar->bytes = sidecar->bytes.Slice(0, sidecar->bytes.Size() - 1);
        } else if (kind == 1) {
            sidecar->bytes = longer.View();
        } else if (kind == 2) {
            sidecar->path.Clear();
            REQUIRE(sidecar->path.TryAssign("moved-away"));
        } else if (kind == 3) {
            sidecar->completeError = ErrorCode::IoError;
        } else {
            TestPackageIo::File* package = rig.io.FindFile(rig.scene.PathOf(3));
            REQUIRE(package != nullptr);
            package->bytes = package->bytes.Slice(0, package->bytes.Size() - 1);
        }

        const AssetLoadStatus status = LoadOne(rig, 3);
        if (kind <= 2) {
            CHECK(SidecarCorrupt(status, 3));
            CHECK(rig.io.sidecarStats == 1);
        } else if (kind == 3) {
            CHECK(status.state == AssetLoadState::Failed && status.error == ErrorCode::IoError &&
                  status.diagnostic.code == AssetDiagnosticCode::None &&
                  status.diagnostic.asset == FixedId(3));
        } else {
            CHECK(status.state == AssetLoadState::Failed &&
                  status.diagnostic.section ==
                      static_cast<uint32>(CookedManifestField::PackageBytes));
            CHECK(rig.io.sidecarStats == 0);
        }

        REQUIRE(rig.loader->Shutdown());
    }
}

// 디스크 IO(AsyncCookedPackageIo — 마지막 조각 완료 스레드에서 Stat): 1·2·3을 임시 디렉터리에 쓰되
// 3의 sidecar는 한 byte 짧게 쓰면 1은 성공, 3은 SidecarBytes 손상이다. 3의 sidecar를 지우면 다시
// 손상이고, 맞는 크기로 다시 쓰면 성공한다.
TEST(AssetLoader_SidecarSizeOnDisk) {
    AssetEnv env;
    REQUIRE(env.Ready());
    char root[96] = {};
    const int count = snprintf(root, sizeof(root), "rocks-loader-sidecar-%u",
                               static_cast<unsigned>(platform::CurrentProcessId()));
    REQUIRE(count > 0 && static_cast<usize>(count) < sizeof(root));
    REQUIRE(platform::MakeDirectoryTree(platform::PhysicalFileSystem(), root,
                                        static_cast<usize>(count)));
    auto removeRoot = MakeScopeExit([&]() noexcept {
        CHECK(platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), root,
                                            static_cast<usize>(count)));
    });
    LoaderRig rig(env);
    rig.scene.root = StringView(root, static_cast<usize>(count));
    REQUIRE(CookScene(rig));
    for (usize index = 0; index < rig.scene.FileCount(); ++index) {
        REQUIRE(MakeParent(rig.scene.PathAt(index)));
        REQUIRE(WriteWholeFile(rig.scene.PathAt(index), rig.scene.FileAt(index).View()));
        const Array<uint8>& sidecar = rig.scene.SidecarAt(index);
        if (sidecar.IsEmpty()) {
            continue;
        }

        const bool shorter = rig.scene.SidecarPathAt(index) == rig.scene.SidecarPathOf(3);
        REQUIRE(WriteWholeFile(rig.scene.SidecarPathAt(index),
                               sidecar.View().Slice(0, sidecar.Size() - (shorter ? 1 : 0))));
    }

    platform::AsyncIoContext io;
    REQUIRE(io.Start(platform::DefaultAsyncIoBackend()));
    {
        constexpr uint64 SmallChunk = 777;
        AsyncCookedPackageIo files(env.allocator, io, SmallChunk);
        rig.config.io = &files;
        auto loadOnce = [&](uint64 id) noexcept {
            AssetLoadStatus result;
            if (!rig.Start()) {
                return result;
            }

            const AssetLoadHandle handle = rig.Request(id);
            auto waited = rig.loader->WaitFor(handle, "LoaderSidecarTests");
            if (waited) {
                result = waited.Value();
            }

            (void)rig.loader->Shutdown();
            rig.loader.Reset();
            return result;
        };
        CHECK(loadOnce(1).state == AssetLoadState::Succeeded);
        CHECK(SidecarCorrupt(loadOnce(3), 3));
        const StringView three = rig.scene.SidecarPathOf(3);
        REQUIRE(platform::PhysicalFileSystem().Delete(three.Data(), three.ByteCount()));
        CHECK(SidecarCorrupt(loadOnce(3), 3));
        REQUIRE(WriteWholeFile(three, rig.scene.SidecarOf(3).View()));
        CHECK(loadOnce(3).state == AssetLoadState::Succeeded);
    }

    CHECK(io.Shutdown());
}

} // namespace loader_sidecar_tests_file
