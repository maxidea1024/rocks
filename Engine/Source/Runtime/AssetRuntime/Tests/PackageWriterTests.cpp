#include "AssetFixtureValues.h"

#include <Serialization/Record.h>
#include <TestSupport/NFailAllocator.h>

// 쓰기 시험이다(Docs/Design/AssetSystem.ko.md 5.1·5.5·5.7절): 옵션 검증 하나하나, 대표 에셋 규칙,
// export 상한(4096 경계), 경계 패키지(대표만·0 byte bulk만), 저장 그래프 거부(자기 패키지 객체
// 참조· 패키지에 속하지 않은 객체 — UnpackagedReference), bulk 원본 오류, 출력(sink) 실패 전파,
// 할당 실패 주입(쓰기·읽기·Prepare·Instantiate의 모든 할당 지점).
using namespace rocks;
using namespace rocks::asset_test;

namespace {

// 대표 에셋(TestAsset)만 있는 패키지다.
struct Single {
    RootRef<Package> package;
    ObjectHandle asset{};
};

bool MakeSingle(ObjectRegistry& registry, Single& single, uint64 id,
                PackageKind kind = PackageKind::Cooked) noexcept {
    auto package = NewPackage(registry, registry.OwnerAccess(), FixedId(id), kind);
    if (!package) {
        return false;
    }

    single.package = Move(package.Value());
    single.asset = Make<TestAsset>(registry, TestAssetType(), single.package.Handle(), "");
    return !single.asset.IsNull();
}

ErrorCode WriteError(AssetEnv& env, ObjectHandle package, const PackageWriteOptions& options,
                     AssetDiagnostic* failure = nullptr) noexcept {
    MemoryPackageSink sink(env.allocator);
    auto written = WritePackage(env.allocator, *env.source, env.source->OwnerAccess(), package,
                                options, sink, failure);
    return written ? ErrorCode::None : written.Error();
}

// n번째 Append(또는 WriteAt)에서 실패하는 출력이다.
struct FailingSink final : PackageSink {
    explicit FailingSink(Allocator& allocator) noexcept : inner(allocator) {
    }

    Result<void> Append(Span<const uint8> bytes) noexcept override {
        if (++appends == failAppend) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        return inner.Append(bytes);
    }

    Result<void> WriteAt(uint64 offset, Span<const uint8> bytes) noexcept override {
        if (failWriteAt) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        return inner.WriteAt(offset, bytes);
    }

    MemoryPackageSink inner;
    uint64 appends = 0;
    uint64 failAppend = 0;
    bool failWriteAt = false;
};

// 오류를 내는 bulk 원본이다.
struct ErrorBulkSource final : PackageBulkSource {
    Result<Span<const uint8>> ReadBulk(const BulkDataRef&) noexcept override {
        return Result<Span<const uint8>>::Failure(ErrorCode::Busy);
    }
};

} // namespace

// 옵션 검증(PackageWriteOptions 계약): 종류·정렬·codec, 에디터의 엔진 버전·프로파일 금지, 쿠킹의
// 프로파일 id(빈 값·4 KiB 초과·UTF-8)·ImportRecord·Tags·extraDependencies 금지, 커스텀 버전
// 정렬·중복, extraDependencies의 에셋·종류·expectedType·자기 에셋·중복, ImportRecord·Tags의 Record
// 모양.
TEST(AssetPackage_WriterOptionValidation) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Single single;
    REQUIRE(MakeSingle(*env.source, single, 800));
    const ObjectHandle package = single.package.Handle();
    CHECK(WriteError(env, package, CookedOptions()) == ErrorCode::None);
    CHECK(WriteError(env, package, EditorOptions()) == ErrorCode::None);

    auto invalid = [&](PackageWriteOptions options) noexcept {
        return WriteError(env, package, options) == ErrorCode::InvalidArgument;
    };

    PackageWriteOptions options = CookedOptions();
    options.kind = static_cast<PackageKind>(3);
    CHECK(invalid(options));
    const uint32 alignmentValues[] = {48u, 4u, 131072u, 0x80000000u};
    for (uint32 alignment : alignmentValues) {
        options = CookedOptions();
        options.bulkAlignment = alignment;
        CHECK(invalid(options));
    }

    options = CookedOptions();
    options.bulkCodec = static_cast<BulkCodec>(2);
    CHECK(invalid(options));
    options = EditorOptions();
    options.engineVersion = EngineVersion{};
    CHECK(invalid(options));
    options = EditorOptions();
    options.profileId = "Windows";
    CHECK(invalid(options));
    options = CookedOptions();
    options.profileId = StringView();
    CHECK(invalid(options));
    char longProfile[PackageMaxStringBytes + 1];
    for (char& value : longProfile) {
        value = 'w';
    }

    options.profileId = StringView(longProfile, sizeof(longProfile));
    CHECK(invalid(options));
    options.profileId = StringView(longProfile, PackageMaxStringBytes);
    CHECK(!invalid(options));
    const char badUtf8[2] = {static_cast<char>(0xC3), static_cast<char>(0x28)};
    options.profileId = StringView(badUtf8, 2);
    CHECK(invalid(options));

    RecordBuilder record(env.allocator, Guid{1, 1}, 1);
    auto built = record.Build();
    REQUIRE(built);
    const uint8 junk[4] = {1, 2, 3, 4};
    options = CookedOptions();
    options.importRecord = built.Value().View();
    CHECK(invalid(options));
    options = CookedOptions();
    options.tags = built.Value().View();
    CHECK(invalid(options));
    options = EditorOptions();
    options.importRecord = Span<const uint8>(junk, 4);
    CHECK(invalid(options));
    options = EditorOptions();
    options.tags = Span<const uint8>(junk, 4);
    CHECK(invalid(options));
    options = EditorOptions();
    options.importRecord = built.Value().View();
    options.tags = built.Value().View();
    CHECK(!invalid(options));

    const PackageCustomVersion unsorted[] = {{Guid{2, 2}, 1}, {Guid{1, 1}, 1}};
    const PackageCustomVersion duplicate[] = {{Guid{1, 1}, 1}, {Guid{1, 1}, 2}};
    options = CookedOptions();
    options.customVersions = unsorted;
    CHECK(invalid(options));
    options.customVersions = duplicate;
    CHECK(invalid(options));

    const PackageDependencyEntry build{FixedId(850), PackageDependencyKind::Build, TypeId{}, false};
    const PackageDependencyEntry badExtras[][1] = {
        {{AssetId{}, PackageDependencyKind::Build, TypeId{}, false}},
        {{FixedId(850), PackageDependencyKind::Build, TypeId{1, 1}, false}},
        {{FixedId(850), PackageDependencyKind::Hard, TypeId{}, false}},
        {{FixedId(850), PackageDependencyKind::Soft, TypeId{}, false}},
        {{FixedId(850), PackageDependencyKind::EditorOnly, TypeId{}, false}},
        {{FixedId(800), PackageDependencyKind::Build, TypeId{}, false}}};
    for (const auto& extra : badExtras) {
        options = EditorOptions();
        options.extraDependencies = extra;
        CHECK(invalid(options));
    }

    const PackageDependencyEntry twice[] = {build, build};
    options = EditorOptions();
    options.extraDependencies = twice;
    CHECK(invalid(options));
    options = CookedOptions();
    const PackageDependencyEntry one[] = {build};
    options.extraDependencies = one;
    CHECK(invalid(options));
    const PackageDependencyEntry owner[] = {
        build, {FixedId(851), PackageDependencyKind::ImportOwner, TypeId{}, true}};
    options = EditorOptions();
    options.extraDependencies = owner;
    CHECK(!invalid(options));
}

// 대표 에셋 규칙(AS 4.1절, 5.1절): 패키지가 아닌 handle, 대표 없음, 이름 있는 첫 export, Asset이
// 아닌 대표, 저장 불가 대표.
TEST(AssetPackage_WriterRepresentativeRules) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    Single single;
    REQUIRE(MakeSingle(registry, single, 810));
    // 패키지가 아닌 객체를 패키지로 넘긴다.
    CHECK(WriteError(env, single.asset, CookedOptions()) != ErrorCode::None);

    auto named = NewPackage(registry, access, FixedId(811), PackageKind::Cooked);
    REQUIRE(named);
    REQUIRE(!Make<TestAsset>(registry, TestAssetType(), named.Value().Handle(), "Named").IsNull());
    CHECK(WriteError(env, named.Value().Handle(), CookedOptions()) == ErrorCode::InvalidArgument);

    auto notAsset = NewPackage(registry, access, FixedId(812), PackageKind::Cooked);
    REQUIRE(notAsset);
    REQUIRE(!Make<TestPart>(registry, TestPartType(), notAsset.Value().Handle(), "").IsNull());
    CHECK(WriteError(env, notAsset.Value().Handle(), CookedOptions()) ==
          ErrorCode::InvalidArgument);

    // 저장 불가 객체만 있는 패키지(대표가 빠진다).
    auto scratch = NewPackage(registry, access, FixedId(813), PackageKind::Cooked);
    REQUIRE(scratch);
    REQUIRE(!Make<TestScratch>(registry, TestScratchType(), scratch.Value().Handle(), "").IsNull());
    CHECK(WriteError(env, scratch.Value().Handle(), CookedOptions()) == ErrorCode::InvalidArgument);

    // null id 패키지는 만들 수 없다.
    CHECK(NewPackage(registry, access, AssetId{}, PackageKind::Cooked).Error() ==
          ErrorCode::InvalidArgument);

    // 메모리 출력의 WriteAt 범위.
    MemoryPackageSink sink(env.allocator);
    const uint8 bytes[4] = {1, 2, 3, 4};
    REQUIRE(sink.Append(Span<const uint8>(bytes, 4)));
    CHECK(sink.WriteAt(0, Span<const uint8>(bytes, 4)));
    CHECK(sink.WriteAt(1, Span<const uint8>(bytes, 4)).Error() == ErrorCode::InvalidArgument);
    CHECK(sink.WriteAt(5, Span<const uint8>(bytes, 0)).Error() == ErrorCode::InvalidArgument);
}

// 경계값(AS 5.1절 상한): export 4096개(대표 + 4095)는 쓰고 읽고 로드되며, 4097개는
// CapacityExceeded다. 대표만인 패키지는 Summary·Exports·Objects 세 섹션이고, 0 byte bulk만 있으면
// Bulk 섹션이 0 byte다.
TEST(AssetPackage_WriterBoundaries) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    {
        Single single;
        REQUIRE(MakeSingle(registry, single, 820));
        Array<uint8> bytes =
            Write(env.allocator, registry, single.package.Handle(), CookedOptions());
        REQUIRE(!bytes.IsEmpty());
        CHECK(LoadLittleEndian<uint32>(bytes.Data() + 12) == 3);
        Array<uint8> files[1] = {Move(bytes)};
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        CHECK(LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, roots, handles));
    }

    {
        Single single;
        REQUIRE(MakeSingle(registry, single, 821));
        TestBulkSource bulk;
        Pointer<TestAsset>(registry, single.asset)->pixels = bulk.Add(1, Span<const uint8>());
        PackageWriteOptions options = CookedOptions();
        options.bulkSource = &bulk;
        Array<uint8> bytes = Write(env.allocator, registry, single.package.Handle(), options);
        REQUIRE(!bytes.IsEmpty());
        usize row = 0;
        uint64 offset = 0;
        uint64 size = 0;
        REQUIRE(FindSection(bytes.View(), PackageSectionKind::Bulk, row, offset, size));
        CHECK(size == 0);
        Array<uint8> files[1] = {Move(bytes)};
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        REQUIRE(LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, roots, handles));
        Package* package = Pointer<Package>(*env.target, handles[0]);
        REQUIRE(package != nullptr);
        CHECK(package->BulkCount() == 1);
    }

    Single many;
    REQUIRE(MakeSingle(registry, many, 822));
    char name[8];
    for (uint32 index = 0; index < PackageMaxExports - 1; ++index) {
        // 이름 순서 = 생성 순서가 아니어도 writer가 정렬한다(깊이 우선, 이름 순).
        uint32 value = PackageMaxExports - 1 - index;
        for (usize digit = 0; digit < 5; ++digit) {
            name[4 - digit] = static_cast<char>('0' + value % 10);
            value /= 10;
        }

        REQUIRE(
            !Make<TestPart>(registry, TestPartType(), many.asset, StringView(name, 5)).IsNull());
    }

    Array<uint8> full = Write(env.allocator, registry, many.package.Handle(), CookedOptions());
    REQUIRE(!full.IsEmpty());
    auto contents = ReadPackage(env.allocator, full.View(), PackageReadOptions{});
    REQUIRE(contents);
    CHECK(contents.Value().exports.Size() == PackageMaxExports);
    {
        Array<uint8> files[1] = {Copy(env.allocator, full.View())};
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        CHECK(LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, roots, handles));
    }

    REQUIRE(!Make<TestPart>(registry, TestPartType(), many.asset, "Extra").IsNull());
    CHECK(WriteError(env, many.package.Handle(), CookedOptions()) == ErrorCode::CapacityExceeded);
}

// 저장 그래프 거부(AS 5.1절 참조 표기): 자기 패키지 객체(Package 자신)를 가리키는 참조는
// InvalidArgument, 어떤 패키지에도 속하지 않은 객체는 UnpackagedReference(패키지 정책이 아닌
// Registry에서만 생긴다), bulk 원본 오류는 그 오류, 같은 옛 bulkId에 다른 크기·해시는
// InvalidArgument다.
TEST(AssetPackage_WriterGraphRejections) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    {
        Single single;
        REQUIRE(MakeSingle(registry, single, 830));
        TestAsset* asset = Pointer<TestAsset>(registry, single.asset);
        REQUIRE(WithBorrow<TestAsset>(registry, single.asset, [&](TestAsset& value) {
            return registry
                .AddReference(access, single.asset, value.hardList, single.package.Handle())
                .IsOk();
        }));
        (void)asset;
        CHECK(WriteError(env, single.package.Handle(), CookedOptions()) ==
              ErrorCode::InvalidArgument);
    }

    {
        // 패키지 정책이 없는 Registry: 패키지 밖 자유 객체를 가리킨다.
        RegistryOptions options;
        options.names = &env.names;
        auto plain = UniquePtr<ObjectRegistry>::TryMake(env.allocator, env.allocator, env.types,
                                                        ContextId{0x4153534554ull, 9}, options);
        REQUIRE(plain);
        ObjectRegistry& free = *plain.Value();
        auto package = NewPackage(free, free.OwnerAccess(), FixedId(831), PackageKind::Cooked);
        REQUIRE(package);
        const ObjectHandle asset =
            Make<TestAsset>(free, TestAssetType(), package.Value().Handle(), "");
        auto loose =
            free.NewObject<TestAsset>(free.OwnerAccess(), TestAssetType(), ObjectHandle{}, "Loose");
        REQUIRE(!asset.IsNull() && loose);
        REQUIRE(free.SetReference(free.OwnerAccess(), asset, Pointer<TestAsset>(free, asset)->hard,
                                  loose.Value().Handle()));
        MemoryPackageSink sink(env.allocator);
        AssetDiagnostic failure;
        auto written = WritePackage(env.allocator, free, free.OwnerAccess(),
                                    package.Value().Handle(), CookedOptions(), sink, &failure);
        CHECK(!written);
        CHECK(failure.code == AssetDiagnosticCode::UnpackagedReference);
        // 진단 out 인자 없이도 같은 오류다.
        MemoryPackageSink bare(env.allocator);
        CHECK(WritePackage(env.allocator, free, free.OwnerAccess(), package.Value().Handle(),
                           CookedOptions(), bare)
                  .Error() == written.Error());
        CHECK(failure.section == static_cast<uint32>(PackageSectionKind::Objects));
    }

    {
        Single single;
        REQUIRE(MakeSingle(registry, single, 832));
        const uint8 data[3] = {1, 2, 3};
        TestBulkSource bulk;
        Pointer<TestAsset>(registry, single.asset)->pixels =
            bulk.Add(1, Span<const uint8>(data, 3));
        ErrorBulkSource broken;
        PackageWriteOptions options = CookedOptions();
        options.bulkSource = &broken;
        CHECK(WriteError(env, single.package.Handle(), options) == ErrorCode::Busy);
        // 같은 옛 id 1에 다른 크기: 메모리 상태 모순.
        BulkDataRef other = Pointer<TestAsset>(registry, single.asset)->pixels;
        other.rawBytes = 2;
        Pointer<TestAsset>(registry, single.asset)->source = other;
        options = EditorOptions();
        options.bulkSource = &bulk;
        CHECK(WriteError(env, single.package.Handle(), options) == ErrorCode::InvalidArgument);
        // 쿠킹은 EditorOnly source를 쓰지 않으므로 같은 상태가 저장된다.
        options = CookedOptions();
        options.bulkSource = &bulk;
        CHECK(WriteError(env, single.package.Handle(), options) == ErrorCode::None);
    }
}

// 출력 실패(AS 5.5절 — 파일 쓰기 실패 주입): n번째 Append마다 실패시키면 쓰기가 그 오류로
// 끝나고(헤더 patch 실패도), 실패 없이 쓴 byte는 다시 읽힌다.
TEST(AssetPackage_WriterSinkFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    Single single;
    REQUIRE(MakeSingle(registry, single, 840));
    const ObjectHandle part = Make<TestPart>(registry, TestPartType(), single.asset, "Part");
    REQUIRE(!part.IsNull());
    Array<uint8> data(env.allocator);
    FillCompressible(data, 2000, 1);
    TestBulkSource bulk;
    Pointer<TestAsset>(registry, single.asset)->pixels = bulk.Add(1, data.View());
    Pointer<TestPart>(registry, part)->data = bulk.Add(2, data.View().Slice(0, 100));
    // 설정 셋: 에디터(bulk 정렬 64), 쿠킹 + Zstd(bulk 정렬 4096 — Bulk 앞 패딩이 0이 아니다),
    // 에디터 + ImportRecord·Tags 섹션.
    RecordBuilder record(env.allocator, Guid{0x61, 0x62}, 1);
    const uint8 value[4] = {1, 2, 3, 4};
    REQUIRE(record.AddField(Guid{0x63, 0x64}, WireType::U32, value));
    auto built = record.Build();
    REQUIRE(built);
    PackageWriteOptions configurations[3] = {EditorOptions(), CookedOptions(), EditorOptions()};
    configurations[1].bulkCodec = BulkCodec::Zstd;
    configurations[2].importRecord = built.Value().View();
    configurations[2].tags = built.Value().View();
    for (PackageWriteOptions& options : configurations) {
        options.bulkSource = &bulk;
        uint64 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            FailingSink sink(env.allocator);
            sink.failAppend = attempt;
            auto written = WritePackage(env.allocator, registry, registry.OwnerAccess(),
                                        single.package.Handle(), options, sink);
            if (sink.appends < attempt) {
                REQUIRE(written);
                PackageReadOptions read;
                read.expectedKind = options.kind;
                CHECK(ReadPackage(env.allocator, sink.inner.Bytes(), read));
                break;
            }

            CHECK(!written && written.Error() == ErrorCode::Busy);
            ++failures;
        }

        CHECK(failures > 5);
        FailingSink sink(env.allocator);
        sink.failWriteAt = true;
        CHECK(WritePackage(env.allocator, registry, registry.OwnerAccess(), single.package.Handle(),
                           options, sink)
                  .Error() == ErrorCode::Busy);
    }
}

// 할당 실패 주입: 쓰기·읽기·Prepare·bulk 해제의 n번째 할당을 실패시키면 OutOfMemory(진단 코드
// None — 손상으로 분류하지 않는다)로 끝나고 상태가 남지 않는다. 실패가 일어나지 않는 n에서 끝난다.
TEST(AssetPackage_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    auto dependency = NewPackage(registry, access, FixedId(861), PackageKind::Editor);
    REQUIRE(dependency);
    const ObjectHandle target =
        Make<TestAsset>(registry, TestAssetType(), dependency.Value().Handle(), "");
    Single single;
    REQUIRE(MakeSingle(registry, single, 860, PackageKind::Editor));
    const ObjectHandle part = Make<TestPart>(registry, TestPartType(), single.asset, "Part");
    REQUIRE(!target.IsNull() && !part.IsNull());
    Array<uint8> data(env.allocator);
    FillCompressible(data, 3000, 2);
    TestBulkSource bulk;
    TestAsset* asset = Pointer<TestAsset>(registry, single.asset);
    asset->pixels = bulk.Add(1, data.View());
    REQUIRE(asset->soft.TryAssign(FixedId(862), "Sub"));
    REQUIRE(registry.SetReference(access, single.asset, asset->hard, target));
    const PackageDependencyEntry extra[] = {
        {FixedId(863), PackageDependencyKind::Build, TypeId{}, false}};
    const PackageCustomVersion customs[] = {{Guid{1, 1}, 1}};
    const PackageKind kindValues[] = {PackageKind::Cooked, PackageKind::Editor};
    for (PackageKind kind : kindValues) {
        PackageWriteOptions options =
            kind == PackageKind::Cooked ? CookedOptions() : EditorOptions();
        options.bulkSource = &bulk;
        options.bulkCodec = BulkCodec::Zstd;
        options.customVersions = customs;
        if (kind == PackageKind::Editor) {
            options.extraDependencies = extra;
        }

        // 쓰기.
        test::NFailAllocator failing(env.allocator);
        Array<uint8> written(env.allocator);
        uint32 writeFailures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            failing.FailOnAttempt(attempt);
            MemoryPackageSink sink(env.allocator);
            AssetDiagnostic failure;
            auto result = WritePackage(failing, registry, access, single.package.Handle(), options,
                                       sink, &failure);
            if (failing.Attempts() < attempt) {
                REQUIRE(result);
                written = sink.Take();
                break;
            }

            CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
            CHECK(failure.code == AssetDiagnosticCode::None);
            ++writeFailures;
        }

        CHECK(writeFailures > 10);
        // 읽기.
        PackageReadOptions read;
        read.expectedKind = kind;
        read.engineVersion = TestEngineVersion;
        read.types = &env.types;
        read.customVersions = customs;
        uint32 readFailures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            failing.FailOnAttempt(attempt);
            AssetDiagnostic failure;
            auto result = ReadPackage(failing, written.View(), read, &failure);
            if (failing.Attempts() < attempt) {
                REQUIRE(result);
                break;
            }

            CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
            CHECK(failure.code == AssetDiagnosticCode::None);
            ++readFailures;
        }

        CHECK(readFailures > 5);
        // Prepare(상태 allocator) — bulk·그래프 1단계 포함.
        PackageLoadOptions load = LoadOptions(kind);
        load.read.customVersions = customs;
        uint32 prepareFailures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            failing.FailOnAttempt(attempt);
            PackageLoadState state(failing, *env.target, env.types, env.target->OwnerAccess(),
                                   load);
            AssetDiagnostic failure;
            auto result = state.Prepare(Copy(env.allocator, written.View()), &failure);
            if (failing.Attempts() < attempt) {
                REQUIRE(result);
                break;
            }

            CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
            CHECK(failure.code == AssetDiagnosticCode::None);
            ++prepareFailures;
        }

        CHECK(prepareFailures > 5);
    }
}

// Registry 할당 실패(객체·Package·bulk 저장소): 대상 Registry의 allocator를 n번째에서 실패시키면
// Prepare 또는 InstantiatePackages가 OutOfMemory로 끝나고(진단 None) 객체가 남지 않는다.
TEST(AssetPackage_RegistryAllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    Single single;
    REQUIRE(MakeSingle(registry, single, 870));
    const ObjectHandle part = Make<TestPart>(registry, TestPartType(), single.asset, "Part");
    REQUIRE(!part.IsNull());
    Array<uint8> data(env.allocator);
    FillCompressible(data, 1000, 3);
    TestBulkSource bulk;
    Pointer<TestAsset>(registry, single.asset)->pixels = bulk.Add(1, data.View());
    PackageWriteOptions options = CookedOptions();
    options.bulkCodec = BulkCodec::Zstd;
    options.bulkSource = &bulk;
    const Array<uint8> bytes = Write(env.allocator, registry, single.package.Handle(), options);
    REQUIRE(!bytes.IsEmpty());

    test::NFailAllocator failing(env.allocator);
    RegistryOptions registryOptions;
    registryOptions.names = &env.names;
    registryOptions.policy.packageNativeType = NativeTypeKey<Package>();
    registryOptions.policy.rejectWorldOnly = true;
    auto made = UniquePtr<ObjectRegistry>::TryMake(env.allocator, failing, env.types,
                                                   ContextId{0x4153534554ull, 7}, registryOptions);
    REQUIRE(made);
    ObjectRegistry& target = *made.Value();
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        PackageLoadState state(env.allocator, target, env.types, target.OwnerAccess(),
                               LoadOptions(PackageKind::Cooked));
        AssetDiagnostic failure;
        Result<void> result = state.Prepare(Copy(env.allocator, bytes.View()), &failure);
        ObjectHandle handles[1];
        RootRef<Object> roots[1];
        if (result) {
            ConstructionContext transaction(target, target.OwnerAccess());
            PackageLoadState* states[1] = {&state};
            result = InstantiatePackages(transaction, states, nullptr, handles, &failure);
            if (result) {
                result = transaction.Commit(handles, roots);
            }
        }

        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            REQUIRE(result);
            break;
        }

        if (!result) {
            CHECK(result.Error() == ErrorCode::OutOfMemory);
            CHECK(failure.code == AssetDiagnosticCode::None || failure.error == ErrorCode::None);
            ++failures;
        }

        // 공개된 패키지는 Discard로만 거둔다(패키지 정책).
        if (roots[0].IsValid()) {
            CHECK(target.Discard(target.OwnerAccess(), handles[0]));
        }

        roots[0].Reset();
        CHECK(target.Collect(target.OwnerAccess()));
        CHECK(target.Stats().liveObjects == 0);
    }

    CHECK(failures > 0);
}

namespace {

// 큰 할당 실패 시나리오다: A(1400) ↔ B(1401) 하드 순환(한 묶음), A가 B의 깊은 하위 객체
// Gear.Tooth.Tix + U+0300(outer import 사슬 — 결합 문자 이름)·B.Head·자기 Part를 배열로, B 대표
// 에셋을 Hard로, 없는 에셋을 Soft로 가리키고 bulk 둘·커스텀 버전 셋·타입 여럿(TestAsset·TestPart)을
// 가진다. D(1404)는 공개된 A·B를 resolver로 찾는 패키지다(A 대표 에셋·B.Gear.Tooth).
struct Large {
    explicit Large(Allocator& allocator) noexcept : pixels(allocator), data(allocator) {
    }

    RootRef<Package> a;
    RootRef<Package> b;
    RootRef<Package> d;
    Array<uint8> pixels;
    Array<uint8> data;
    TestBulkSource bulk;
};

bool MakeLarge(ObjectRegistry& registry, Large& large) noexcept {
    const ObjectAccessContext access = registry.OwnerAccess();
    auto a = NewPackage(registry, access, FixedId(1400), PackageKind::Editor);
    auto b = NewPackage(registry, access, FixedId(1401), PackageKind::Editor);
    auto d = NewPackage(registry, access, FixedId(1404), PackageKind::Editor);
    if (!a || !b || !d) {
        return false;
    }

    large.a = Move(a.Value());
    large.b = Move(b.Value());
    large.d = Move(d.Value());
    const ObjectHandle aAsset = Make<TestAsset>(registry, TestAssetType(), large.a.Handle(), "");
    const ObjectHandle bAsset = Make<TestAsset>(registry, TestAssetType(), large.b.Handle(), "");
    const ObjectHandle dAsset = Make<TestAsset>(registry, TestAssetType(), large.d.Handle(), "");
    const ObjectHandle part = Make<TestPart>(registry, TestPartType(), aAsset, "Part");
    const ObjectHandle gear = Make<TestPart>(registry, TestPartType(), bAsset, "Gear");
    const ObjectHandle tooth = Make<TestPart>(registry, TestPartType(), gear, "Tooth");
    // 결합 문자 이름(x + U+0300 — 합성형이 없어 NFC다)은 이름 검사의 NFC 판정이 scratch를 쓴다.
    const ObjectHandle tip = Make<TestPart>(registry, TestPartType(), tooth, "Tix\xCC\x80");
    const ObjectHandle head = Make<TestPart>(registry, TestPartType(), bAsset, "Head");
    if (aAsset.IsNull() || bAsset.IsNull() || dAsset.IsNull() || part.IsNull() || tip.IsNull() ||
        head.IsNull()) {
        return false;
    }

    FillCompressible(large.pixels, 2500, 5);
    FillIncompressible(large.data, 300, 5);
    TestAsset* asset = Pointer<TestAsset>(registry, aAsset);
    asset->pixels = large.bulk.Add(1, large.pixels.View());
    Pointer<TestPart>(registry, part)->data = large.bulk.Add(2, large.data.View());
    TestAsset* other = Pointer<TestAsset>(registry, bAsset);
    TestAsset* last = Pointer<TestAsset>(registry, dAsset);
    return asset->soft.TryAssign(FixedId(1402), "Sub").IsOk() &&
           registry.SetReference(access, aAsset, asset->hard, bAsset).IsOk() &&
           registry.SetReference(access, bAsset, other->hard, aAsset).IsOk() &&
           registry.SetReference(access, dAsset, last->hard, aAsset).IsOk() &&
           WithBorrow<TestAsset>(
               registry, aAsset,
               [&](TestAsset& owner) {
                   return registry.AddReference(access, aAsset, owner.hardList, tip).IsOk() &&
                          registry.AddReference(access, aAsset, owner.hardList, head).IsOk() &&
                          registry.AddReference(access, aAsset, owner.hardList, part).IsOk();
               }) &&
           WithBorrow<TestAsset>(registry, dAsset, [&](TestAsset& owner) {
               return registry.AddReference(access, dAsset, owner.hardList, tooth).IsOk();
           });
}

// registry allocator가 failing인 target에 files를 한 묶음으로 공개하는 n번째 할당 실패 sweep이다.
// resolver가 있으면 묶음 밖 import를 그것으로 찾는다. 실패는 OutOfMemory·진단 None이고 매번 공개한
// 것을 거둔 뒤 객체가 baseline으로 돌아온다. 실패 횟수를 돌려준다.
uint32 RegistrySweep(AssetEnv& env, test::NFailAllocator& failing, ObjectRegistry& target,
                     Span<const Array<uint8>* const> files, const PackageLoadOptions& load,
                     PackageImportResolver* resolver) noexcept {
    const uint32 baseline = target.Stats().liveObjects;
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.DisableFailure();
        PackageLoadState first(env.allocator, target, env.types, target.OwnerAccess(), load);
        PackageLoadState second(env.allocator, target, env.types, target.OwnerAccess(), load);
        PackageLoadState* states[2] = {&first, &second};
        for (usize index = 0; index < files.Size(); ++index) {
            if (!states[index]->Prepare(Copy(env.allocator, files[index]->View()))) {
                test::RecordFailure("prepare failed without failure injection", __FILE__, __LINE__);
                return failures;
            }
        }

        failing.FailOnAttempt(attempt);
        ObjectHandle handles[2];
        RootRef<Object> roots[2];
        AssetDiagnostic failure;
        Result<void> result = Result<void>::Success();
        {
            ConstructionContext transaction(target, target.OwnerAccess());
            result = InstantiatePackages(
                transaction, Span<PackageLoadState* const>(states, files.Size()), resolver,
                Span<ObjectHandle>(handles, files.Size()), &failure);
            if (result) {
                result = transaction.Commit(Span<const ObjectHandle>(handles, files.Size()),
                                            Span<RootRef<Object>>(roots, files.Size()));
            }
        }

        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!result) {
            CHECK(result.Error() == ErrorCode::OutOfMemory);
            CHECK(failure.code == AssetDiagnosticCode::None || failure.error == ErrorCode::None);
            ++failures;
        }

        for (usize index = 0; index < files.Size(); ++index) {
            if (roots[index].IsValid()) {
                CHECK(target.Discard(target.OwnerAccess(), handles[index]));
                roots[index].Reset();
            }
        }

        CHECK(target.Collect(target.OwnerAccess()));
        CHECK(target.Stats().liveObjects == baseline);
        if (!triggered) {
            CHECK(result.IsOk());
            break;
        }
    }

    return failures;
}

} // namespace

// 할당 실패 주입(큰 시나리오 — AS 5.7절 12): 쿠킹·에디터마다 A·B 쓰기, A 읽기, A Prepare, 두 상태를
// 한 묶음으로 올리는 InstantiatePackages(상태 allocator)의 n번째 할당을 실패시키면 OutOfMemory·진단
// None이고 객체가 남지 않는다. 대상 Registry allocator를 실패시키는 묶음 로드(A·B)와 resolver
// 로드(공개된 A·B를 찾는 D)도 같다.
TEST(AssetPackage_AllocationFailuresLargeScenario) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    Large large(env.allocator);
    REQUIRE(MakeLarge(registry, large));
    const PackageCustomVersion customs[] = {{Guid{1, 1}, 1}, {Guid{1, 2}, 3}, {Guid{4, 0}, 2}};
    const PackageDependencyEntry extra[] = {
        {FixedId(1403), PackageDependencyKind::Build, TypeId{}, false}};
    const PackageKind kindValues[] = {PackageKind::Cooked, PackageKind::Editor};
    test::NFailAllocator failing(env.allocator);
    ObjectRegistry& target = *env.target;
    for (PackageKind kind : kindValues) {
        PackageWriteOptions options =
            kind == PackageKind::Cooked ? CookedOptions() : EditorOptions();
        options.bulkSource = &large.bulk;
        options.bulkCodec = BulkCodec::Zstd;
        options.customVersions = customs;
        if (kind == PackageKind::Editor) {
            options.extraDependencies = extra;
        }

        Array<uint8> files[2] = {Array<uint8>(env.allocator), Array<uint8>(env.allocator)};
        const ObjectHandle packages[2] = {large.a.Handle(), large.b.Handle()};
        for (usize which = 0; which < 2; ++which) {
            uint32 failures = 0;
            for (uint64 attempt = 1;; ++attempt) {
                failing.FailOnAttempt(attempt);
                MemoryPackageSink sink(env.allocator);
                AssetDiagnostic failure;
                auto result = WritePackage(failing, registry, access, packages[which], options,
                                           sink, &failure);
                if (failing.Attempts() < attempt) {
                    REQUIRE(result);
                    files[which] = sink.Take();
                    break;
                }

                CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
                CHECK(failure.code == AssetDiagnosticCode::None);
                ++failures;
            }

            CHECK(failures > 20);
        }

        PackageReadOptions read;
        read.expectedKind = kind;
        read.engineVersion = TestEngineVersion;
        read.types = &env.types;
        read.customVersions = customs;
        uint32 readFailures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            failing.FailOnAttempt(attempt);
            AssetDiagnostic failure;
            auto result = ReadPackage(failing, files[0].View(), read, &failure);
            if (failing.Attempts() < attempt) {
                REQUIRE(result);
                CHECK(result.Value().imports.Size() == 5);
                break;
            }

            CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
            CHECK(failure.code == AssetDiagnosticCode::None);
            ++readFailures;
        }

        CHECK(readFailures > 10);
        PackageLoadOptions load = LoadOptions(kind);
        load.read.customVersions = customs;
        uint32 prepareFailures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            failing.FailOnAttempt(attempt);
            PackageLoadState state(failing, target, env.types, target.OwnerAccess(), load);
            AssetDiagnostic failure;
            auto result = state.Prepare(Copy(env.allocator, files[0].View()), &failure);
            if (failing.Attempts() < attempt) {
                REQUIRE(result);
                break;
            }

            CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
            CHECK(failure.code == AssetDiagnosticCode::None);
            ++prepareFailures;
        }

        CHECK(prepareFailures > 10);
        // InstantiatePackages(상태 allocator — import 대상 배열 등): 묶음 A·B.
        uint32 instantiateFailures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            failing.DisableFailure();
            PackageLoadState first(failing, target, env.types, target.OwnerAccess(), load);
            PackageLoadState second(failing, target, env.types, target.OwnerAccess(), load);
            REQUIRE(first.Prepare(Copy(env.allocator, files[0].View())));
            REQUIRE(second.Prepare(Copy(env.allocator, files[1].View())));
            failing.FailOnAttempt(attempt);
            ConstructionContext transaction(target, target.OwnerAccess());
            PackageLoadState* states[2] = {&first, &second};
            ObjectHandle handles[2];
            AssetDiagnostic failure;
            auto result = InstantiatePackages(transaction, states, nullptr, handles, &failure);
            const bool triggered = failing.Attempts() >= attempt;
            failing.DisableFailure();
            transaction.Rollback();
            CHECK(target.Stats().liveObjects == 0);
            if (!triggered) {
                REQUIRE(result);
                break;
            }

            CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
            CHECK(failure.code == AssetDiagnosticCode::None);
            ++instantiateFailures;
        }

        CHECK(instantiateFailures > 0);
    }

    // 대상 Registry allocator.
    PackageWriteOptions cooked = CookedOptions();
    cooked.bulkSource = &large.bulk;
    const Array<uint8> a = Write(env.allocator, registry, large.a.Handle(), cooked);
    const Array<uint8> b = Write(env.allocator, registry, large.b.Handle(), cooked);
    const Array<uint8> d = Write(env.allocator, registry, large.d.Handle(), cooked);
    REQUIRE(!a.IsEmpty() && !b.IsEmpty() && !d.IsEmpty());
    RegistryOptions registryOptions;
    registryOptions.names = &env.names;
    registryOptions.policy.packageNativeType = NativeTypeKey<Package>();
    registryOptions.policy.rejectWorldOnly = true;
    auto made = UniquePtr<ObjectRegistry>::TryMake(env.allocator, failing, env.types,
                                                   ContextId{0x4153534554ull, 12}, registryOptions);
    REQUIRE(made);
    ObjectRegistry& failingTarget = *made.Value();
    const PackageLoadOptions load = LoadOptions(PackageKind::Cooked);
    const Array<uint8>* pair[2] = {&a, &b};
    CHECK(RegistrySweep(env, failing, failingTarget, pair, load, nullptr) > 0);
    // A·B를 실패 없이 공개한 뒤 D를 resolver로 로드한다.
    failing.DisableFailure();
    Array<uint8> published[2] = {Copy(env.allocator, a.View()), Copy(env.allocator, b.View())};
    RootRef<Object> roots[2];
    ObjectHandle handles[2];
    REQUIRE(LoadAll(env, failingTarget, published, PackageKind::Cooked, nullptr, roots, handles));
    TestResolver resolver;
    resolver.Add(FixedId(1400), handles[0]);
    resolver.Add(FixedId(1401), handles[1]);
    const Array<uint8>* last[1] = {&d};
    CHECK(RegistrySweep(env, failing, failingTarget, last, load, &resolver) > 0);
    for (usize index = 0; index < 2; ++index) {
        CHECK(failingTarget.Discard(failingTarget.OwnerAccess(), handles[index]));
        roots[index].Reset();
    }
}
