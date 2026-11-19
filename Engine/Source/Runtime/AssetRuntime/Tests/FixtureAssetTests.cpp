#include "AssetFixtureValues.h"

// fixture 에셋 클래스별 왕복 시험이다(Docs/Design/AssetSystem.ko.md 14.1절): 결정적 생성기로 채운
// 대표 에셋을 쿠킹·에디터로 저장 → 다른 Registry에 로드 → 필드 단위 비교 → 로드한 상태를 다시
// 저장해 byte 동일.
using namespace rocks;
using namespace rocks::asset_test;

namespace {

// 대표 에셋 하나짜리 패키지의 왕복 결과다.
struct RoundTrip {
    RootRef<Object> root;
    ObjectHandle package{};
    ObjectHandle asset{};
    bool sameBytes = false;
};

// source에서 fill로 채운 T 패키지를 kind로 저장하고 target에 로드한 뒤 다시 저장해 비교한다.
template <class T, class FillFn>
bool RunRoundTrip(AssetEnv& env, uint64 id, PackageKind kind, FillFn&& fill,
                  PackageBulkSource* bulk, BulkCodec codec, RoundTrip& out) noexcept {
    ObjectRegistry& source = *env.source;
    auto package = NewPackage(source, source.OwnerAccess(), FixedId(id), kind);
    if (!package) {
        return false;
    }

    const ObjectHandle asset = Make<T>(source, IdOf<T>(), package.Value().Handle(), "");
    T* pointer = Pointer<T>(source, asset);
    if (pointer == nullptr || !fill(*pointer)) {
        return false;
    }

    PackageWriteOptions options = kind == PackageKind::Cooked ? CookedOptions() : EditorOptions();
    options.bulkSource = bulk;
    options.bulkCodec = codec;
    Array<uint8> bytes = Write(env.allocator, source, package.Value().Handle(), options);
    if (bytes.IsEmpty()) {
        return false;
    }

    const Array<uint8> original = Copy(env.allocator, bytes.View());
    Array<uint8> files[1] = {Move(bytes)};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    if (!LoadAll(env, *env.target, files, kind, nullptr, roots, handles)) {
        return false;
    }

    out.root = Move(roots[0]);
    out.package = handles[0];
    auto loaded = env.target->FindObject(env.target->OwnerAccess(), handles[0], StringView());
    if (!loaded) {
        return false;
    }

    out.asset = loaded.Value();
    PackageBulkReader reader(*env.target, handles[0]);
    options.bulkSource = &reader;
    Array<uint8> again = Write(env.allocator, *env.target, handles[0], options);
    out.sameBytes = SameBytes(again.View(), original.View());
    return true;
}

const PackageKind Kinds[] = {PackageKind::Cooked, PackageKind::Editor};

} // namespace

// 단순: 스칼라 필드(정수 폭·실수·bool·Name·String·enum·Guid).
TEST(FixtureAsset_SimpleRoundTrip) {
    for (PackageKind kind : Kinds) {
        AssetEnv env;
        REQUIRE(env.Ready());
        RoundTrip result;
        REQUIRE(RunRoundTrip<SimpleAsset>(
            env, 100, kind, [&](SimpleAsset& asset) { return FillSimple(asset, env.names, 17); },
            nullptr, BulkCodec::None, result));
        SimpleAsset* loaded = Pointer<SimpleAsset>(*env.target, result.asset);
        REQUIRE(loaded != nullptr);
        CHECK(CheckSimple(*loaded, env.names, 17));
        CHECK(!CheckSimple(*loaded, env.names, 18));
        CHECK(loaded->Id() == FixedId(100));
        CHECK(result.sameBytes);
    }
}

// 값 묶음: 중첩 struct·정적 배열·Array·Map·Set·Optional·Text·Bytes·InstancedStruct·커브·UE 단위 값.
TEST(FixtureAsset_ValueRoundTrip) {
    for (PackageKind kind : Kinds) {
        AssetEnv env;
        REQUIRE(env.Ready());
        RoundTrip result;
        REQUIRE(RunRoundTrip<ValueAsset>(
            env, 101, kind,
            [&](ValueAsset& asset) {
                return FillValue(asset, env.types, env.source->ObjectAllocator(), 23);
            },
            nullptr, BulkCodec::None, result));
        ValueAsset* loaded = Pointer<ValueAsset>(*env.target, result.asset);
        REQUIRE(loaded != nullptr);
        CHECK(CheckValue(*loaded, 23));
        CHECK(!CheckValue(*loaded, 24));
        CHECK(result.sameBytes);
    }
}

// 상속: 파생 객체의 기반 필드·파생 필드가 왕복한다.
TEST(FixtureAsset_InheritanceRoundTrip) {
    for (PackageKind kind : Kinds) {
        AssetEnv env;
        REQUIRE(env.Ready());
        RoundTrip result;
        REQUIRE(RunRoundTrip<DerivedGameAsset>(
            env, 102, kind, [&](DerivedGameAsset& asset) { return FillDerived(asset, 31); },
            nullptr, BulkCodec::None, result));
        DerivedGameAsset* loaded = Pointer<DerivedGameAsset>(*env.target, result.asset);
        REQUIRE(loaded != nullptr);
        CHECK(CheckDerived(*loaded, 31));
        CHECK(result.sameBytes);
    }
}

// 검증 hook: 유효한 값은 왕복하고, 거부되는 값은 저장이 실패한다(쓰기 경로의 hook). 로드 쪽 거부는
// 저장된 byte의 값을 바꿔(섹션 CRC는 맞춤) 그래프 1단계가 CorruptPackage로 거부하는지 본다.
TEST(FixtureAsset_ValidateHook) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RoundTrip result;
    REQUIRE(RunRoundTrip<ValidatedAsset>(
        env, 103, PackageKind::Cooked,
        [&](ValidatedAsset& asset) { return FillValidated(asset, 5); }, nullptr, BulkCodec::None,
        result));
    ValidatedAsset* loaded = Pointer<ValidatedAsset>(*env.target, result.asset);
    REQUIRE(loaded != nullptr);
    CHECK(CheckValidated(*loaded, 5));
    CHECK(result.sameBytes);

    // 저장은 값 검증을 하지 않는다(그래프 저장 규칙 — hook은 쓰기 경로와 로드에서 돈다). low >
    // high를 C++로 직접 넣어 저장한 패키지는 로드(적용 단계)가 hook으로 거부하고
    // CorruptPackage로 분류하며 아무 객체도 공개하지 않는다.
    ObjectRegistry& source = *env.source;
    auto package = NewPackage(source, source.OwnerAccess(), FixedId(104), PackageKind::Cooked);
    REQUIRE(package);
    const ObjectHandle asset =
        Make<ValidatedAsset>(source, IdOf<ValidatedAsset>(), package.Value().Handle(), "");
    ValidatedAsset* pointer = Pointer<ValidatedAsset>(source, asset);
    REQUIRE(pointer != nullptr);
    // 변형 0: 필드 struct가 거부 값, 변형 1: 배열 원소 struct가 거부 값.
    for (usize variant = 0; variant < 2; ++variant) {
        pointer->range = variant == 0 ? FixtureRange{9, 1} : FixtureRange{0, 1};
        pointer->ranges.Clear();
        if (variant == 1) {
            REQUIRE(pointer->ranges.TryPushBack(FixtureRange{5, -5}));
        }

        Array<uint8> files[1] = {
            Write(env.allocator, source, package.Value().Handle(), CookedOptions())};
        REQUIRE(!files[0].IsEmpty());
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        AssetDiagnostic failure;
        const uint32 before = env.target->Stats().liveObjects;
        CHECK(!LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, roots, handles,
                       &failure));
        CHECK(failure.code == AssetDiagnosticCode::CorruptPackage);
        CHECK(failure.error == ErrorCode::InvalidArgument);
        CHECK(env.target->Stats().liveObjects == before);
    }
}

// 큰 데이터: 4 MiB 압축되는 무늬·2 MiB 압축되지 않는 무늬·0 byte·bulk 배열을 None·Zstd 둘 다로
// 왕복한다.
TEST(FixtureAsset_BulkRoundTrip) {
    const BulkCodec codecs[] = {BulkCodec::None, BulkCodec::Zstd};
    for (PackageKind kind : Kinds) {
        for (BulkCodec codec : codecs) {
            AssetEnv env;
            REQUIRE(env.Ready());
            Array<uint8> big(env.allocator);
            Array<uint8> noise(env.allocator);
            Array<uint8> chunk(env.allocator);
            FillCompressible(big, 4 * 1024 * 1024, 3);
            FillIncompressible(noise, 2 * 1024 * 1024, 7);
            FillCompressible(chunk, 777, 11);
            TestBulkSource bulk;
            RoundTrip result;
            REQUIRE(RunRoundTrip<BulkAsset>(
                env, 105, kind,
                [&](BulkAsset& asset) {
                    asset.compressible = bulk.Add(10, big.View());
                    asset.incompressible = bulk.Add(20, noise.View());
                    asset.empty = bulk.Add(30, Span<const uint8>());
                    return asset.chunks.TryPushBack(bulk.Add(40, chunk.View())).IsOk() &&
                           asset.chunks.TryPushBack(asset.empty).IsOk();
                },
                &bulk, codec, result));
            CHECK(result.sameBytes);
            BulkAsset* loaded = Pointer<BulkAsset>(*env.target, result.asset);
            Package* package = Pointer<Package>(*env.target, result.package);
            REQUIRE(loaded != nullptr && package != nullptr);
            CHECK(package->BulkCount() == 4);
            auto compressible = package->BulkBytes(loaded->compressible);
            auto incompressible = package->BulkBytes(loaded->incompressible);
            auto empty = package->BulkBytes(loaded->empty);
            REQUIRE(compressible && incompressible && empty);
            CHECK(SameBytes(compressible.Value(), big.View()));
            CHECK(SameBytes(incompressible.Value(), noise.View()));
            CHECK(empty.Value().IsEmpty());
            REQUIRE(loaded->chunks.Size() == 2);
            // 같은 옛 id는 같은 새 id다(empty가 두 번 쓰였다).
            CHECK(loaded->chunks[1] == loaded->empty);
            auto chunkBytes = package->BulkBytes(loaded->chunks[0]);
            REQUIRE(chunkBytes);
            CHECK(SameBytes(chunkBytes.Value(), chunk.View()));
        }
    }
}
