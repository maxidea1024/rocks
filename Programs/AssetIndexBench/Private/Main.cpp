#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/AssetIndexCache.h>
#include <AssetDatabase/ProjectLayout.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <AssetRuntime/PackageTags.h>
#include <AssetRuntime/PackageWriter.h>
#include <AssetTestFixtures/AssetTestFixtures.module.generated.h>
#include <AssetTestFixtures/TestAssets.generated.h>
#include <AssetTestFixtures/TestAssets.h>
#include <Core/Algorithms.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/String.h>
#include <Launch/EntryPoint.h>
#include <Object/ModuleRegistration.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Thread.h>
#include <Task/WorkerScheduler.h>

#include <stdio.h>

// 색인 검색 측정 도구다(Docs/Design/AssetSystem.ko.md 7.1절 16 A2d2 — 제품 밖). 합성
// 프로젝트(fixture TestAsset 패키지 count개, 태그 둘, 중첩 디렉터리 25개)를 처음 한 번 만들고
// Rebuild를 직렬· 병렬(worker 수별)·캐시 재사용으로 rounds번 재 중앙값을 보고한다. 첫 round는 OS
// 파일 캐시를 데우므로 중앙값은 따뜻한 캐시 기준이다(차가운 디스크는 재부팅 뒤 첫 실행으로 따로
// 잰다).
//
// 사용: AssetIndexBench <프로젝트 디렉터리> [count(기본 50000)] [rounds(기본 5)]
// Release로 빌드해 같은 기기에서 비교한다:
//   dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=AssetIndexBench -p:Configuration=Release
namespace {

using namespace rocks;
using asset_fixture::TestAsset;

constexpr uint64 DefaultCount = 50000;
constexpr uint64 MaxCount = 1000000;
constexpr uint32 DefaultRounds = 5;
constexpr uint32 MaxRounds = 50;
constexpr usize DirectoryFanout = 5;
constexpr EngineVersion BenchEngineVersion{1, 0, 0};
constexpr uint64 BenchIdHigh = 0xbe9c0000a55e7000ull;

PackageReadOptions ReadOptions() noexcept {
    PackageReadOptions options;
    options.expectedKind = PackageKind::Editor;
    options.engineVersion = BenchEngineVersion;
    return options;
}

bool ParseCount(StringView text, uint64 maximum, uint64& out) noexcept {
    uint64 value = 0;
    if (text.IsEmpty()) {
        return false;
    }

    for (char digit : text) {
        if (digit < '0' || digit > '9') {
            return false;
        }

        value = value * 10 + static_cast<uint64>(digit - '0');
        if (value > maximum) {
            return false;
        }
    }

    out = value;
    return value != 0;
}

bool WriteFile(StringView path, Span<const uint8> bytes) noexcept {
    platform::FileSystem& files = platform::PhysicalFileSystem();
    usize slash = path.ByteCount();
    while (slash > 0 && path.Data()[slash - 1] != '/') {
        --slash;
    }

    if (!platform::MakeDirectoryTree(files, path.Data(), slash - 1)) {
        return false;
    }

    platform::File file;
    if (!file.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::CreateAlways)) {
        return false;
    }

    const bool written = static_cast<bool>(file.WriteAll(bytes.Data(), bytes.Size()).status);
    return static_cast<bool>(file.Close()) && written;
}

// Content 아래 패키지 count개를 쓴다(타입 등록·Registry는 여기서만 쓴다).
bool Generate(Allocator& allocator, StringView content, uint64 count) noexcept {
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    NamePool names(allocator);
    if (!RegisterAssetRuntimeTypes(modules) ||
        !generated::AssetTestFixtures::RegisterModuleTypes(modules) || !names.Initialize()) {
        fprintf(stderr, "cannot register the fixture types\n");
        return false;
    }

    RegistryOptions options;
    options.names = &names;
    options.policy.packageNativeType = NativeTypeKey<Package>();
    options.policy.rejectWorldOnly = true;
    auto registry = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                       ContextId{0x42454e4348ull, 1}, options);
    if (!registry) {
        return false;
    }

    const PackageTagInput tagInput[] = {{"Kind", "Bench"}, {"Tier", "Common"}};
    auto tags = WritePackageTags(allocator, tagInput);
    if (!tags) {
        return false;
    }

    ObjectRegistry& objects = *registry.Value();
    const ObjectAccessContext access = objects.OwnerAccess();
    bool ok = true;
    for (uint64 at = 0; at < count && ok; ++at) {
        auto package =
            NewPackage(objects, access, AssetId{Guid{BenchIdHigh, at + 1}}, PackageKind::Editor);
        ok = static_cast<bool>(package) &&
             objects
                 .NewObject<TestAsset>(access, GeneratedType<TestAsset>::Id(),
                                       package.Value().Handle(), "")
                 .IsOk();
        if (!ok) {
            break;
        }

        PackageWriteOptions write;
        write.kind = PackageKind::Editor;
        write.engineVersion = BenchEngineVersion;
        write.tags = tags.Value().View();
        MemoryPackageSink sink(allocator);
        ok = static_cast<bool>(
            WritePackage(allocator, objects, access, package.Value().Handle(), write, sink));
        if (!ok) {
            break;
        }

        const Array<uint8> bytes = sink.Take();
        char relative[96] = {};
        (void)snprintf(relative, sizeof(relative), "/Bench%llu/Group%llu/Asset%llu.rasset",
                       static_cast<unsigned long long>(at % DirectoryFanout),
                       static_cast<unsigned long long>((at / DirectoryFanout) % DirectoryFanout),
                       static_cast<unsigned long long>(at));
        String path(allocator);
        ok = path.TryAppend(content) && path.TryAppend(StringView(relative)) &&
             WriteFile(path.View(), bytes.View());
    }

    registry.Value().Reset();
    (void)names.Shutdown();
    (void)modules.Unregister();
    return ok;
}

double Seconds(uint64 ticks) noexcept {
    return static_cast<double>(ticks) / static_cast<double>(platform::MonotonicTickFrequency());
}

// rounds번 잰 시간(초)의 중앙값이다. 첫 round(OS 파일 캐시 데우기)도 포함한 최솟값·최댓값을 함께
// 보고한다.
template <typename Run>
bool Measure(const char* label, uint32 rounds, Run&& run) noexcept {
    double samples[MaxRounds] = {};
    usize count = 0;
    for (uint32 round = 0; round < rounds; ++round) {
        const uint64 start = platform::MonotonicTicks();
        if (!run()) {
            fprintf(stderr, "%s: rebuild failed\n", label);
            return false;
        }

        samples[count++] = Seconds(platform::MonotonicTicks() - start);
    }

    algo::Sort(Span<double>(samples, count));
    printf("%-22s median %8.3f s  min %8.3f s  max %8.3f s\n", label, samples[count / 2],
           samples[0], samples[count - 1]);
    return true;
}

int RunMain(launch::ApplicationContext& context) noexcept {
    const Span<const StringView> arguments = context.Arguments();
    uint64 count = DefaultCount;
    uint64 rounds = DefaultRounds;
    if (arguments.IsEmpty() || arguments.Size() > 3 ||
        (arguments.Size() >= 2 && !ParseCount(arguments[1], MaxCount, count)) ||
        (arguments.Size() == 3 && !ParseCount(arguments[2], MaxRounds, rounds))) {
        fprintf(stderr,
                "usage: AssetIndexBench <project directory> [count (1-%llu)] [rounds "
                "(1-%u)]\n",
                static_cast<unsigned long long>(MaxCount), MaxRounds);
        return 2;
    }

    SystemAllocator allocator;
    ProjectLayout layout(allocator);
    if (!platform::MakeDirectoryTree(platform::PhysicalFileSystem(), arguments[0].Data(),
                                     arguments[0].ByteCount()) ||
        !layout.Initialize(arguments[0])) {
        fprintf(stderr, "cannot use the project directory\n");
        return 1;
    }

    platform::FileSystem& files = platform::PhysicalFileSystem();
    const StringView content = layout.ContentDirectory();
    bool exists = false;
    if (!files.Exists(content.Data(), content.ByteCount(), exists) || !exists) {
        printf("generating %llu packages...\n", static_cast<unsigned long long>(count));
        const uint64 start = platform::MonotonicTicks();
        if (!Generate(allocator, content, count)) {
            fprintf(stderr, "cannot generate the project\n");
            return 1;
        }

        printf("generated in %.1f s\n", Seconds(platform::MonotonicTicks() - start));
    }

    AssetIndex probe(allocator, layout);
    if (!probe.Rebuild(files, ReadOptions())) {
        fprintf(stderr, "cannot index the project\n");
        return 1;
    }

    printf("%zu packages, %zu diagnostics, %u logical processors\n", probe.Count(),
           probe.Diagnostics().Size(), platform::LogicalProcessorCount());
    Array<uint8> cacheBytes(allocator);
    const AssetIndexCacheKey key = MakeAssetIndexCacheKey(ReadOptions());
    AssetIndexCache cache(allocator);
    if (!EncodeAssetIndexCache(probe, key, cacheBytes) || !cache.Parse(cacheBytes.View(), key)) {
        fprintf(stderr, "cannot build the cache\n");
        return 1;
    }

    const uint32 rounds32 = static_cast<uint32>(rounds);
    bool ok = Measure("serial", rounds32, [&]() noexcept {
        AssetIndex index(allocator, layout);
        return static_cast<bool>(index.Rebuild(files, ReadOptions()));
    });
    ok = ok && Measure("serial + cache", rounds32, [&]() noexcept {
             AssetIndex index(allocator, layout);
             return static_cast<bool>(index.Rebuild(files, ReadOptions(), &cache));
         });
    const uint32 processors = platform::LogicalProcessorCount();
    const uint32 helpers[] = {1, 3, 7, processors > 1 ? processors - 1 : 1};
    for (uint32 workers : helpers) {
        WorkerScheduler scheduler(allocator);
        WorkerSchedulerConfig config;
        config.workerCount = workers;
        if (!scheduler.Start(config)) {
            fprintf(stderr, "cannot start %u workers\n", workers);
            return 1;
        }

        char label[48] = {};
        (void)snprintf(label, sizeof(label), "parallel %u+1", workers);
        ok = ok && Measure(label, rounds32, [&]() noexcept {
                 AssetIndex index(allocator, layout, &scheduler);
                 return static_cast<bool>(index.Rebuild(files, ReadOptions()));
             });
        (void)snprintf(label, sizeof(label), "parallel %u+1 + cache", workers);
        ok = ok && Measure(label, rounds32, [&]() noexcept {
                 AssetIndex index(allocator, layout, &scheduler);
                 return static_cast<bool>(index.Rebuild(files, ReadOptions(), &cache));
             });
        (void)scheduler.Shutdown();
    }

    return ok ? 0 : 1;
}

const launch::ApplicationDescription& Describe() noexcept {
    static constexpr launch::ApplicationDescription Description{
        .name = "AssetIndexBench", .kind = launch::ApplicationKind::Tool, .run = &RunMain};
    return Description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)
