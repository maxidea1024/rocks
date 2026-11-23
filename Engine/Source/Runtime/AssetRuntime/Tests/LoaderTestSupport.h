#pragma once

#include "ManifestTestSupport.h"

#include <AssetRuntime/AssetLoader.h>
#include <Core/Text/StringBuilder.h>

// 로더 시험 도구다(Docs/Design/AssetSystem 11.6절): 쿠킹 패키지 장면(A1a writer로 쓴 패키지와
// 그 결과로 만든 매니페스트), 완료를 명시 gate로 내보내는 IO, 손으로 돌리는 worker, 단계마다 정해진
// 만큼 가는 시계, 로더 묶음. 순서는 모두 시험이 정한다(시간·경쟁에 기대지 않는다).
namespace rocks::asset_test {

// 쿠킹 패키지 file로 매니페스트 항목을 채운다(경로 /Game/P<id>, 의존은 패키지 Dependencies 그대로).
inline bool FillLoaderEntry(Allocator& allocator, CookedManifestEntry& entry,
                            const Array<uint8>& file, uint64 id) noexcept {
    auto read = ReadPackage(allocator, file.View(), PackageReadOptions{});
    if (!read) {
        return false;
    }

    entry.asset = read.Value().header.assetId;
    entry.type = read.Value().exports[0].type;
    entry.packageBytes = file.Size();
    entry.packageSha256 = Sha256::Hash(file.View());
    entry.cookKey = ManifestDigestOf(static_cast<uint8>(id));
    entry.bulkRawBytes = 0;
    for (const PackageBulkEntry& bulk : read.Value().bulkEntries) {
        entry.bulkRawBytes += bulk.rawBytes;
    }

    for (const PackageDependencyEntry& dependency : read.Value().dependencies) {
        if (!entry.dependencies
                 .TryPushBack(CookedManifestDependency{dependency.asset, dependency.kind})
                 .IsOk()) {
            return false;
        }
    }

    char name[24] = {};
    usize length = 0;
    uint64 value = id;
    char digits[24];
    do {
        digits[length++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);

    name[0] = 'P';
    for (usize at = 0; at < length; ++at) {
        name[1 + at] = digits[length - 1 - at];
    }

    return entry.pathHint.TryAppend("/Game/").IsOk() &&
           entry.pathHint.TryAppend(StringView(name, length + 1)).IsOk();
}

// root + "/" + 쿠킹 패키지 경로를 out에 쓴다.
inline bool LoaderPackagePath(StringBuilderBase& out, StringView root, uint64 id,
                              const Sha256Digest& sha) noexcept {
    return out.TryAppend(root).IsOk() && out.TryAppendAscii('/').IsOk() &&
           AppendCookedPackagePath(out, FixedId(id), sha).IsOk();
}

// 장면 패키지 하나의 정의다(id는 FixedId 하위 값).
struct LoaderNode {
    uint64 id = 0;
    // Hard 대상(hardList 원소)과 Soft 대상(0 = 없음)이다.
    uint64 hard[8] = {};
    uint32 hardCount = 0;
    uint64 soft = 0;
    // 0이 아니면 이 크기의 bulk(Zstd)를 붙인다.
    uint32 bulkBytes = 0;
    // 참이면 이 패키지는 다른 Registry에서 대표 에셋을 TestRequiredAsset(target → 자기)으로 다시 쓴
    // 쿠킹 결과로 바꾼다 — 이 패키지를 Hard로 가리키는 패키지는 TestAsset을 기대하고 쓰였으므로
    // 공개 단계에서 TypeMismatch다(오래된 의존 쿠킹 결과).
    bool staleType = false;
    // 참이면 대표 에셋을 TestHookAsset으로 만들고 첫 Hard 대상(hard[0])을 typed로 가리킨다(생성자·
    // 소멸자 hook — 공개 트랜잭션 안의 사용자 코드 시험). 나머지 Hard·Soft·bulk는 쓰지 않는다.
    bool hookType = false;
};

// 쿠킹 패키지 장면이다: 노드를 더하고 Cook하면 패키지 byte와 매니페스트가 생긴다.
class LoaderScene {
public:
    explicit LoaderScene(AssetEnv& env) noexcept
        : manifest(env.allocator), env_(env), nodes_(env.allocator), files_(env.allocator),
          paths_(env.allocator) {
    }

    // 노드를 더한다(id 오름차순일 필요는 없다).
    LoaderNode& Add(uint64 id, uint32 bulkBytes = 0) noexcept {
        LoaderNode node;
        node.id = id;
        node.bulkBytes = bulkBytes;
        (void)nodes_.TryPushBack(node);
        return nodes_[nodes_.Size() - 1];
    }

    LoaderNode* Find(uint64 id) noexcept {
        for (LoaderNode& node : nodes_) {
            if (node.id == id) {
                return &node;
            }
        }

        return nullptr;
    }

    void Hard(uint64 from, uint64 to) noexcept {
        LoaderNode* node = Find(from);
        node->hard[node->hardCount++] = to;
    }

    void Soft(uint64 from, uint64 to) noexcept {
        Find(from)->soft = to;
    }

    // 모든 노드를 source Registry에 만들고 쿠킹 패키지로 쓴 뒤 매니페스트를 만든다.
    bool Cook() noexcept {
        ObjectRegistry& source = *env_.source;
        const ObjectAccessContext access = source.OwnerAccess();
        Array<RootRef<Package>> packages(env_.allocator);
        Array<ObjectHandle> assets(env_.allocator);
        for (const LoaderNode& node : nodes_) {
            auto made = NewPackage(source, access, FixedId(node.id), PackageKind::Cooked);
            if (!made) {
                return false;
            }

            const ObjectHandle asset =
                node.hookType ? Make<asset_fixture::TestHookAsset>(
                                    source, GeneratedType<asset_fixture::TestHookAsset>::Id(),
                                    made.Value().Handle(), "")
                              : Make<TestAsset>(source, TestAssetType(), made.Value().Handle(), "");
            if (asset.IsNull() || !packages.TryPushBack(Move(made.Value())) ||
                !assets.TryPushBack(asset)) {
                return false;
            }
        }

        Array<Array<uint8>> bulkData(env_.allocator);
        for (usize index = 0; index < nodes_.Size(); ++index) {
            const LoaderNode& node = nodes_[index];
            const ObjectHandle asset = assets[index];
            if (node.hookType) {
                bool linked = node.hardCount == 0;
                if (!linked) {
                    auto* hook = Pointer<asset_fixture::TestHookAsset>(source, asset);
                    linked = hook != nullptr && source
                                                    .SetReference(access, asset, hook->typed,
                                                                  assets[IndexOf(node.hard[0])])
                                                    .IsOk();
                }

                if (!linked) {
                    return false;
                }

                continue;
            }

            bool ok = WithBorrow<TestAsset>(source, asset, [&](TestAsset& owner) {
                for (uint32 edge = 0; edge < node.hardCount; ++edge) {
                    const ObjectHandle target = assets[IndexOf(node.hard[edge])];
                    if (!source.AddReference(access, asset, owner.hardList, target)) {
                        return false;
                    }
                }

                owner.value = static_cast<int32>(node.id);
                return node.soft == 0 ||
                       owner.soft.TryAssign(FixedId(node.soft), StringView()).IsOk();
            });
            if (!ok) {
                return false;
            }
        }

        FillManifestHeader(manifest);
        // 매니페스트는 AssetId(하위 값) 오름차순이다.
        Array<uint32> order(env_.allocator);
        for (uint32 index = 0; index < nodes_.Size(); ++index) {
            if (!order.TryPushBack(index)) {
                return false;
            }
        }

        order.Sort([this](uint32 left, uint32 right) noexcept {
            return nodes_[left].id < nodes_[right].id;
        });
        for (uint32 index : order) {
            const LoaderNode& node = nodes_[index];
            TestBulkSource bulk;
            Array<uint8> data(env_.allocator);
            PackageWriteOptions options = CookedOptions();
            if (node.bulkBytes != 0) {
                FillCompressible(data, node.bulkBytes, static_cast<uint32>(node.id));
                TestAsset* pointer = Pointer<TestAsset>(source, assets[index]);
                pointer->pixels = bulk.Add(1, data.View());
                options.bulkSource = &bulk;
                options.bulkCodec = BulkCodec::Zstd;
            }

            Array<uint8> file =
                node.staleType ? WriteStale(node.id)
                               : Write(env_.allocator, source, packages[index].Handle(), options);
            if (file.IsEmpty()) {
                return false;
            }

            auto added = manifest.entries.TryEmplaceBack(env_.allocator);
            if (!added || !FillLoaderEntry(env_.allocator, *added.Value(), file, node.id)) {
                return false;
            }

            StringBuilder<128> path(env_.allocator);
            if (!LoaderPackagePath(path, root, node.id, added.Value()->packageSha256)) {
                return false;
            }

            String copy(env_.allocator);
            if (!copy.TryAssign(path.View()) || !paths_.TryPushBack(Move(copy)) ||
                !files_.TryPushBack(Move(file))) {
                return false;
            }
        }

        return ValidateCookedManifest(manifest, env_.allocator).IsOk();
    }

    // id 패키지의 매니페스트 항목이다.
    CookedManifestEntry& EntryOf(uint64 id) noexcept {
        for (CookedManifestEntry& entry : manifest.entries) {
            if (entry.asset == FixedId(id)) {
                return entry;
            }
        }

        return manifest.entries[0];
    }

    // id 패키지의 파일 byte와 경로다(매니페스트 순서 index로도 찾는다).
    Array<uint8>& FileOf(uint64 id) noexcept {
        return files_[EntryIndexOf(id)];
    }

    StringView PathOf(uint64 id) const noexcept {
        return paths_[EntryIndexOf(id)].View();
    }

    usize FileCount() const noexcept {
        return files_.Size();
    }

    Array<uint8>& FileAt(usize index) noexcept {
        return files_[index];
    }

    StringView PathAt(usize index) const noexcept {
        return paths_[index].View();
    }

    CookedManifest manifest;
    // 쿠킹 루트다(패키지 경로 앞부분 — 실제 파일 시험은 임시 디렉터리로 바꾼다). 가리키는 문자열은
    // 장면보다 오래 살아야 한다.
    StringView root = "cooked";

private:
    // id 패키지를 따로 만든 Registry에서 대표 에셋 TestRequiredAsset(target → 자기)으로 쓴다.
    Array<uint8> WriteStale(uint64 id) noexcept {
        constexpr uint64 StaleContextHigh = 0x5354414c45ull;
        RegistryOptions options;
        options.names = &env_.names;
        options.policy.packageNativeType = NativeTypeKey<Package>();
        options.policy.rejectWorldOnly = true;
        auto registry = UniquePtr<ObjectRegistry>::TryMake(
            env_.allocator, env_.allocator, env_.types, ContextId{StaleContextHigh, id}, options);
        if (!registry) {
            return Array<uint8>(env_.allocator);
        }

        ObjectRegistry& stale = *registry.Value();
        const ObjectAccessContext access = stale.OwnerAccess();
        auto package = NewPackage(stale, access, FixedId(id), PackageKind::Cooked);
        if (!package) {
            return Array<uint8>(env_.allocator);
        }

        const ObjectHandle asset =
            Make<TestRequiredAsset>(stale, TestRequiredType(), package.Value().Handle(), "");
        TestRequiredAsset* pointer = Pointer<TestRequiredAsset>(stale, asset);
        if (pointer == nullptr || !stale.SetReference(access, asset, pointer->target, asset)) {
            return Array<uint8>(env_.allocator);
        }

        return Write(env_.allocator, stale, package.Value().Handle(), CookedOptions());
    }

    usize IndexOf(uint64 id) const noexcept {
        for (usize index = 0; index < nodes_.Size(); ++index) {
            if (nodes_[index].id == id) {
                return index;
            }
        }

        return 0;
    }

    usize EntryIndexOf(uint64 id) const noexcept {
        for (usize index = 0; index < manifest.entries.Size(); ++index) {
            if (manifest.entries[index].asset == FixedId(id)) {
                return index;
            }
        }

        return 0;
    }

    AssetEnv& env_;
    Array<LoaderNode> nodes_;
    Array<Array<uint8>> files_;
    Array<String> paths_;
};

// 완료를 시험이 내보내는 IO다. 파일은 경로 → byte 표이고, 경로마다 오류를 끼울 수 있다. hold면
// 완료를 쌓아 두고 Complete*가 내보낸다(아니면 Begin 안에서 바로 완료한다).
class TestPackageIo final : public CookedPackageIo {
public:
    struct File {
        String path;
        Span<const uint8> bytes;
        // 완료 결과 오류(None이면 정상)와 Begin 즉시 실패 오류다.
        ErrorCode completeError = ErrorCode::None;
        ErrorCode beginError = ErrorCode::None;
    };

    struct Held {
        CookedPackageRead* read = nullptr;
        CookedPackageReadResult result;
    };

    explicit TestPackageIo(Allocator& allocator) noexcept
        : files(allocator), held(allocator), begun(allocator) {
    }

    // scene의 모든 파일을 등록한다.
    bool AddScene(LoaderScene& scene) noexcept {
        for (usize index = 0; index < scene.FileCount(); ++index) {
            if (!Add(scene.PathAt(index), scene.FileAt(index).View())) {
                return false;
            }
        }

        return true;
    }

    bool Add(StringView path, Span<const uint8> bytes) noexcept {
        File file{String(files.GetAllocator()), bytes};
        return file.path.TryAssign(path).IsOk() && files.TryPushBack(Move(file)).IsOk();
    }

    File* FindFile(StringView path) noexcept {
        for (File& file : files) {
            if (file.path.View() == path) {
                return &file;
            }
        }

        return nullptr;
    }

    Result<void> Begin(CookedPackageRead& read) noexcept override {
        const StringView path(read.path, read.pathBytes);
        String record(files.GetAllocator());
        (void)record.TryAssign(path);
        (void)begun.TryPushBack(Move(record));
        File* file = FindFile(path);
        if (file != nullptr && file->beginError != ErrorCode::None) {
            return Result<void>::Failure(file->beginError);
        }

        if (onBegin != nullptr) {
            onBegin(onBeginContext);
        }

        ++active;
        Held entry;
        entry.read = &read;
        if (file == nullptr) {
            entry.result.error = ErrorCode::NotFound;
        } else if (file->completeError != ErrorCode::None) {
            entry.result.error = file->completeError;
        } else {
            entry.result.fileBytes = file->bytes.Size();
            if (file->bytes.Size() == read.expectedBytes) {
                for (usize at = 0; at < file->bytes.Size(); ++at) {
                    read.destination[at] = file->bytes[at];
                }
            }
        }

        if (hold) {
            (void)held.TryPushBack(entry);
            return Result<void>::Success();
        }

        read.complete(read, entry.result);
        return Result<void>::Success();
    }

    void Release(CookedPackageRead&) noexcept override {
        ++released;
        --active;
        if (onRelease != nullptr) {
            onRelease(onReleaseContext);
        }
    }

    // 쌓인 완료를 앞에서 count개 내보낸다.
    void Complete(usize count = NumericLimits<usize>::Max()) noexcept {
        usize done = 0;
        while (!held.IsEmpty() && done < count) {
            Held entry = held[0];
            (void)held.TryRemoveAt(0);
            entry.read->complete(*entry.read, entry.result);
            ++done;
        }
    }

    // 쌓인 완료를 뒤에서부터(늦게 시작한 것 먼저) 모두 내보낸다.
    void CompleteReverse() noexcept {
        while (!held.IsEmpty()) {
            Held entry = held[held.Size() - 1];
            (void)held.TryPopBack();
            entry.read->complete(*entry.read, entry.result);
        }
    }

    // path로 시작한 첫 쌓인 완료를 내보낸다.
    bool CompletePath(StringView path) noexcept {
        for (usize index = 0; index < held.Size(); ++index) {
            if (StringView(held[index].read->path, held[index].read->pathBytes) == path) {
                Held entry = held[index];
                (void)held.TryRemoveAt(index);
                entry.read->complete(*entry.read, entry.result);
                return true;
            }
        }

        return false;
    }

    // 시험 hook: Begin·Release마다 불린다(로더 재진입 시험).
    void (*onBegin)(void* context) noexcept = nullptr;
    void* onBeginContext = nullptr;
    void (*onRelease)(void* context) noexcept = nullptr;
    void* onReleaseContext = nullptr;
    Array<File> files;
    Array<Held> held;
    // Begin을 부른 경로(순서대로)다.
    Array<String> begun;
    bool hold = false;
    uint32 active = 0;
    uint32 released = 0;
};

// 손으로 돌리는 worker다. hold면 작업을 쌓아 두고 RunAll·RunFirst가 실행한다(아니면 Run 안에서
// 바로).
class TestLoaderWorker final : public AssetLoaderWorker {
public:
    struct Pending {
        Job job = nullptr;
        void* context = nullptr;
    };

    explicit TestLoaderWorker(Allocator& allocator) noexcept : pending(allocator) {
    }

    Result<void> Run(Job job, void* context) noexcept override {
        if (failRun != ErrorCode::None) {
            return Result<void>::Failure(failRun);
        }

        ++runs;
        if (onRun != nullptr) {
            onRun(onRunContext);
        }

        if (hold) {
            (void)pending.TryPushBack(Pending{job, context});
            return Result<void>::Success();
        }

        job(context);
        return Result<void>::Success();
    }

    void RunAll() noexcept {
        while (!pending.IsEmpty()) {
            RunFirst();
        }
    }

    void RunFirst() noexcept {
        RunAt(0);
    }

    void RunAt(usize index) noexcept {
        const Pending chosen = pending[index];
        (void)pending.TryRemoveAt(index);
        chosen.job(chosen.context);
    }

    // 쌓인 작업을 뒤에서부터 모두 실행한다.
    void RunAllReverse() noexcept {
        while (!pending.IsEmpty()) {
            RunAt(pending.Size() - 1);
        }
    }

    // 시험 hook: Run마다 불린다(로더 재진입 시험).
    void (*onRun)(void* context) noexcept = nullptr;
    void* onRunContext = nullptr;
    Array<Pending> pending;
    bool hold = false;
    ErrorCode failRun = ErrorCode::None;
    uint32 runs = 0;
};

// AssetEnv와 같은 정책의 에셋 Registry를 하나 더 만든다(시험 안에서 먼저 지워야 하는 Registry —
// 예: 로더 allocator가 시험 지역 객체일 때). 실패하면 빈 포인터다.
inline UniquePtr<ObjectRegistry> MakeLoaderRegistry(AssetEnv& env, uint64 context,
                                                    Allocator* allocator = nullptr) noexcept {
    Allocator& owner = allocator != nullptr ? *allocator : env.allocator;
    constexpr uint64 LoaderContextHigh = 0x4c4f41444552ull;
    RegistryOptions options;
    options.names = &env.names;
    options.policy.packageNativeType = NativeTypeKey<Package>();
    options.policy.rejectWorldOnly = true;
    auto made = UniquePtr<ObjectRegistry>::TryMake(env.allocator, owner, env.types,
                                                   ContextId{LoaderContextHigh, context}, options);
    return made ? Move(made.Value()) : UniquePtr<ObjectRegistry>();
}

// 할당 수를 세고 아래 allocator로 넘긴다.
class CountingAllocator final : public Allocator {
public:
    explicit CountingAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        ++allocations;
        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

    uint64 allocations = 0;

private:
    Allocator& inner_;
};

// PostLoad 호출 순서를 적는 handler다.
class RecordingPostLoad final : public AssetPostLoadHandler {
public:
    explicit RecordingPostLoad(Allocator& allocator) noexcept
        : order(allocator), packages(allocator) {
    }

    void PostLoad(ObjectHandle package, AssetId asset) noexcept override {
        (void)order.TryPushBack(asset);
        (void)packages.TryPushBack(package);
    }

    // 넘어온 package handle이 모두 그 에셋의 FindPackage 결과와 같은지다(공개한 패키지가 남아 있을
    // 때).
    bool PackagesMatch(AssetLoader& loader) const noexcept {
        for (usize index = 0; index < order.Size(); ++index) {
            auto found = loader.FindPackage(order[index]);
            if (!found || !(found.Value() == packages[index])) {
                return false;
            }
        }

        return true;
    }

    // asset의 기록 위치다(없으면 order 크기).
    usize IndexOf(AssetId asset) const noexcept {
        for (usize index = 0; index < order.Size(); ++index) {
            if (order[index] == asset) {
                return index;
            }
        }

        return order.Size();
    }

    // asset이 몇 번 기록됐는지다.
    usize CountOf(AssetId asset) const noexcept {
        usize count = 0;
        for (const AssetId& recorded : order) {
            count += recorded == asset ? 1 : 0;
        }

        return count;
    }

    Array<AssetId> order;
    Array<ObjectHandle> packages;
};

// id 요청이다(우선순위 Normal, 취소 없음).
inline AssetLoadRequest LoaderRequestOf(uint64 id) noexcept {
    AssetLoadRequest request;
    request.id = FixedId(id);
    return request;
}

// 부를 때마다 step만큼 가는 시계다.
struct LoaderStepClock {
    uint64 now = 0;
    uint64 step = 0;

    static uint64 Read(void* context) noexcept {
        auto& clock = *static_cast<LoaderStepClock*>(context);
        clock.now += clock.step;
        return clock.now;
    }
};

// 로더 시험 묶음이다: 환경·장면·IO·worker·설정·로더.
struct LoaderRig {
    explicit LoaderRig(AssetEnv& env) noexcept
        : env(env), scene(env), io(env.allocator), worker(env.allocator) {
    }

    // 장면을 쿠킹하고 IO에 등록한 뒤 설정을 채운다(로더는 Start가 만든다).
    bool Cook() noexcept {
        if (!scene.Cook() || !io.AddScene(scene)) {
            return false;
        }

        config.allocator = &env.allocator;
        config.registry = env.target.Get();
        config.types = &env.types;
        config.manifest = &scene.manifest;
        config.cookedRoot = scene.root;
        config.runtimeProfile = "Windows";
        config.io = &io;
        config.worker = &worker;
        return true;
    }

    bool Start() noexcept {
        auto made = UniquePtr<AssetLoader>::TryMake(env.allocator, config);
        if (!made) {
            return false;
        }

        loader = Move(made.Value());
        return loader->Initialize().IsOk();
    }

    AssetLoadHandle Request(uint64 id, LoadPriority priority = LoadPriority::Normal,
                            CancelToken cancel = CancelToken()) noexcept {
        AssetLoadRequest request;
        request.id = FixedId(id);
        request.priority = priority;
        request.cancel = Move(cancel);
        auto handle = loader->Request(request);
        return handle ? handle.Value() : AssetLoadHandle{};
    }

    AssetLoadStatus StatusOf(AssetLoadHandle handle) noexcept {
        auto status = loader->Status(handle);
        return status ? status.Value() : AssetLoadStatus{};
    }

    AssetLoadState StateOf(AssetLoadHandle handle) noexcept {
        return StatusOf(handle).state;
    }

    // IO·worker를 막지 않은 채 Pump를 끝날 때까지 되풀이한다(횟수 상한 — 넘으면 false).
    bool PumpUntilSettled(uint32 limit = 10000) noexcept {
        for (uint32 round = 0; round < limit; ++round) {
            auto pumped = loader->Pump();
            if (!pumped) {
                return false;
            }

            AssetLoaderSnapshot snapshot(env.allocator);
            if (!loader->Describe(snapshot)) {
                return false;
            }

            if (snapshot.units.IsEmpty() && pumped.Value().completions == 0) {
                return true;
            }
        }

        return false;
    }

    // id 파일 byte를 바꾼 뒤 매니페스트 SHA-256을 맞추고 새 이름(내용 주소)으로 IO에 다시 등록한다.
    bool Republish(uint64 id) noexcept {
        Array<uint8>& file = scene.FileOf(id);
        CookedManifestEntry& entry = scene.EntryOf(id);
        entry.packageSha256 = Sha256::Hash(file.View());
        StringBuilder<128> path(env.allocator);
        return LoaderPackagePath(path, scene.root, id, entry.packageSha256) &&
               io.Add(path.View(), file.View());
    }

    // 쌓인 IO·worker를 순서(앞부터 또는 뒤부터)대로 내보내며 Pump를 단위가 없어질 때까지
    // 되풀이한다.
    bool SettleHeld(bool reverse, uint32 limit = 1000) noexcept {
        for (uint32 round = 0; round < limit; ++round) {
            if (reverse) {
                io.CompleteReverse();
                worker.RunAllReverse();
            } else {
                io.Complete();
                worker.RunAll();
            }

            if (!loader->Pump()) {
                return false;
            }

            AssetLoaderSnapshot snapshot(env.allocator);
            if (!loader->Describe(snapshot)) {
                return false;
            }

            if (snapshot.units.IsEmpty() && io.held.IsEmpty() && worker.pending.IsEmpty() &&
                io.active == 0) {
                return true;
            }
        }

        return false;
    }

    bool IsLoaded(uint64 id) noexcept {
        return loader->FindPackage(FixedId(id)).IsOk();
    }

    // 공개한 id 패키지의 대표 에셋 handle이다(없으면 null).
    ObjectHandle AssetOf(uint64 id) noexcept {
        auto package = loader->FindPackage(FixedId(id));
        if (!package) {
            return ObjectHandle{};
        }

        auto found = env.target->FindObject(env.target->OwnerAccess(), package.Value(), "");
        return found ? found.Value() : ObjectHandle{};
    }

    AssetEnv& env;
    LoaderScene scene;
    TestPackageIo io;
    TestLoaderWorker worker;
    AssetLoaderConfig config;
    UniquePtr<AssetLoader> loader;
};

} // namespace rocks::asset_test
