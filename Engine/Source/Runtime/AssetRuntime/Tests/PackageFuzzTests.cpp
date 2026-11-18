#include "AssetFixtureValues.h"

// 고정 seed 변조 시험이다(Docs/Design/AssetSystem.ko.md 14절 12): 유효한 패키지의 byte를 수천 번
// 무작위로 바꿔 reader·Prepare·InstantiatePackages가 크래시·누수 없이 정해진 오류 코드만 내는지
// 단언한다. 절반은 변조 뒤 모든 CRC(헤더·섹션 표·비-Bulk 섹션)를 다시 맞춰 CRC 검사 뒤의 내용
// 검증까지 닿게 한다.
using namespace rocks;
using namespace rocks::asset_test;

namespace {

struct XorShift {
    uint64 state;

    uint64 Next() noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

// 헤더·섹션 표·비-Bulk 섹션 CRC를 모두 다시 맞춘다(구조가 망가져 위치를 읽을 수 없으면 그대로
// 둔다).
void FixAllCrcs(Array<uint8>& file) noexcept {
    if (file.Size() < PackageHeaderBytes) {
        return;
    }

    const uint64 table = LoadLittleEndian<uint64>(file.Data() + 16);
    const uint32 count = LoadLittleEndian<uint32>(file.Data() + 12);
    if (count <= PackageMaxSections && table <= file.Size() &&
        uint64{PackageSectionEntryBytes} * count + 4 <= file.Size() - table) {
        for (uint32 index = 0; index < count; ++index) {
            uint8* row = file.Data() + table + PackageSectionEntryBytes * index;
            const uint64 offset = LoadLittleEndian<uint64>(row + 8);
            const uint64 bytes = LoadLittleEndian<uint64>(row + 16);
            const uint32 flags = LoadLittleEndian<uint32>(row + 4);
            if ((flags & static_cast<uint32>(PackageSectionFlags::PerEntryChecksum)) == 0 &&
                offset <= file.Size() && bytes <= file.Size() - offset) {
                StoreLittleEndian<uint32>(
                    row + 24,
                    Crc32(Span<const uint8>(file.Data() + offset, static_cast<usize>(bytes))));
            }
        }

        FixTableCrc(file);
    }

    FixHeaderCrc(file);
}

bool AllowedReadError(ErrorCode error, AssetDiagnosticCode code) noexcept {
    return (error == ErrorCode::InvalidArgument && code == AssetDiagnosticCode::CorruptPackage) ||
           (error == ErrorCode::Unsupported && (code == AssetDiagnosticCode::UnsupportedVersion ||
                                                code == AssetDiagnosticCode::NewerVersion));
}

bool AllowedLoadError(ErrorCode error) noexcept {
    switch (error) {
    case ErrorCode::InvalidArgument:
    case ErrorCode::Unsupported:
    case ErrorCode::NotFound:
    case ErrorCode::TypeMismatch:
    case ErrorCode::CapacityExceeded:
    case ErrorCode::Overflow: // 변조된 정수 값이 필드 폭을 넘는다(그래프 1단계 — CorruptPackage로
                              // 분류)
        return true;
    default:
        return false;
    }
}

} // namespace

TEST(AssetPackage_FixedSeedMutationFuzz) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
    // 피해 대상: 하위 객체·import·Soft 의존·bulk(Zstd)·값 묶음을 가진 패키지.
    auto dependency = NewPackage(source, access, FixedId(201), PackageKind::Cooked);
    auto package = NewPackage(source, access, FixedId(200), PackageKind::Cooked);
    REQUIRE(dependency && package);
    const ObjectHandle target =
        Make<TestAsset>(source, TestAssetType(), dependency.Value().Handle(), "");
    const ObjectHandle asset =
        Make<TestAsset>(source, TestAssetType(), package.Value().Handle(), "");
    const ObjectHandle part = Make<TestPart>(source, TestPartType(), asset, "Part");
    REQUIRE(!target.IsNull() && !asset.IsNull() && !part.IsNull());
    TestAsset* pointer = Pointer<TestAsset>(source, asset);
    Array<uint8> pixels(env.allocator);
    FillCompressible(pixels, 3000, 1);
    TestBulkSource bulk;
    pointer->pixels = bulk.Add(1, pixels.View());
    pointer->value = 77;
    REQUIRE(pointer->label.TryAssign("fuzz"));
    REQUIRE(pointer->soft.TryAssign(FixedId(202), "Sub"));
    REQUIRE(source.SetReference(access, asset, pointer->hard, target));
    REQUIRE(WithBorrow<TestAsset>(source, asset, [&](TestAsset& owner) {
        return source.AddReference(access, asset, owner.hardList, part).IsOk();
    }));
    PackageWriteOptions options = CookedOptions();
    options.bulkSource = &bulk;
    options.bulkCodec = BulkCodec::Zstd;
    const Array<uint8> valid = Write(env.allocator, source, package.Value().Handle(), options);
    Array<uint8> dependencyBytes =
        Write(env.allocator, source, dependency.Value().Handle(), CookedOptions());
    REQUIRE(!valid.IsEmpty() && !dependencyBytes.IsEmpty());

    TestResolver resolver;
    RootRef<Object> dependencyRoot;
    {
        Array<uint8> files[1] = {Move(dependencyBytes)};
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        REQUIRE(LoadAll(env, *env.target, files, PackageKind::Cooked, &resolver, roots, handles));
        dependencyRoot = Move(roots[0]);
        resolver.Add(FixedId(201), handles[0]);
    }

    const uint32 baseline = env.target->Stats().liveObjects;
    XorShift random{0x5EEDF00DCAFEull};
    uint32 readFailures = 0;
    uint32 loadFailures = 0;
    uint32 accepted = 0;
    constexpr uint32 Iterations = 4000;
    for (uint32 iteration = 0; iteration < Iterations; ++iteration) {
        Array<uint8> bytes = Copy(env.allocator, valid.View());
        const uint32 edits = 1 + static_cast<uint32>(random.Next() % 4);
        for (uint32 edit = 0; edit < edits; ++edit) {
            const usize at = static_cast<usize>(random.Next() % bytes.Size());
            bytes[at] = static_cast<uint8>(random.Next());
        }

        // 일부는 잘라 낸다(Stat 크기 불일치·짧은 입력).
        if (random.Next() % 16 == 0) {
            (void)bytes.TryResize(static_cast<usize>(random.Next() % bytes.Size()));
        }

        if (iteration % 2 == 1) {
            FixAllCrcs(bytes);
        }

        AssetDiagnostic readFailure;
        auto read = ReadPackage(env.allocator, bytes.View(), PackageReadOptions{}, &readFailure);
        if (!read) {
            ++readFailures;
            CHECK(AllowedReadError(read.Error(), readFailure.code));
            continue;
        }

        // bulk raw 합 상한을 작게 둔다(변조된 rawBytes가 큰 할당을 요구하면 자원 한도
        // CapacityExceeded).
        PackageLoadOptions loadOptions = LoadOptions(PackageKind::Cooked);
        loadOptions.maxBulkRawBytes = 64 * 1024 * 1024;
        PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                               loadOptions);
        AssetDiagnostic loadFailure;
        auto prepared = state.Prepare(Move(bytes), &loadFailure);
        if (prepared) {
            ConstructionContext transaction(*env.target, env.target->OwnerAccess());
            PackageLoadState* states[] = {&state};
            ObjectHandle handles[1];
            prepared = InstantiatePackages(transaction, states, &resolver, handles, &loadFailure);
            // 성공해도 공개하지 않는다(Rollback — Registry가 기준선으로 돌아오는지 아래에서 본다).
            transaction.Rollback();
        }

        if (!prepared) {
            ++loadFailures;
            CHECK(AllowedLoadError(prepared.Error()));
            // 자원 한도(CapacityExceeded)가 아니면 에셋 진단 분류가 붙는다.
            CHECK(loadFailure.code != AssetDiagnosticCode::None ||
                  prepared.Error() == ErrorCode::CapacityExceeded);
            CHECK(loadFailure.error == prepared.Error());
        } else {
            ++accepted;
        }
    }

    // 변조 대부분은 거부되고, 받아들인 입력(값 byte만 바뀐 경우)도 객체를 남기지 않는다.
    CHECK(readFailures + loadFailures + accepted == Iterations);
    CHECK(readFailures > Iterations / 2);
    CHECK(env.target->Stats().liveObjects == baseline);
}
