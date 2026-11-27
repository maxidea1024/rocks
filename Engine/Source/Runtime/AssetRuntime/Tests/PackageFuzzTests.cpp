#include "AssetFixtureValues.h"

// 고정 seed 변조 시험이다(Docs/Design/AssetSystem 14절 12): 유효한 패키지의 byte를 수천 번
// 무작위로 바꿔 reader·Prepare·InstantiatePackages가 크래시·누수 없이 정해진 오류 코드만 내는지
// 단언한다. 절반은 변조 뒤 모든 CRC(헤더·섹션 표·비-Bulk 섹션)를 다시 맞춰 CRC 검사 뒤의 내용
// 검증까지 닿게 한다.
using namespace rocks;
namespace package_fuzz_tests_file {

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
    return (error == ErrorCode::CorruptData && code == AssetDiagnosticCode::CorruptPackage) ||
           (error == ErrorCode::Unsupported && (code == AssetDiagnosticCode::UnsupportedVersion ||
                                                code == AssetDiagnosticCode::NewerVersion));
}

bool AllowedLoadError(ErrorCode error) noexcept {
    switch (error) {
    case ErrorCode::CorruptData: // 손상 입력(AssetSystem 19장 — InvalidArgument는 받지 않는다)
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

// 쿠킹(Zstd bulk·Soft·하위 객체)과 에디터(EditorOnly 필드·EditorOnly bulk·EditorOnly Hard·Build·
// ImportOwner 의존·읽기 전용 부분집합 경로) 패키지를 각각 4000번 변조한다. reader에는 등록 저장소
// (types)를 넘겨 타입 버전 축까지 닿게 하고, 거부(읽기·로드)와 받아들임이 모두 일어나는지 본다.
TEST(AssetPackage_FixedSeedMutationFuzz) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
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
    Array<uint8> original(env.allocator);
    FillIncompressible(original, 200, 2);
    TestBulkSource bulk;
    pointer->pixels = bulk.Add(1, pixels.View());
    pointer->source = bulk.Add(2, original.View());
    pointer->value = 77;
    REQUIRE(pointer->label.TryAssign("fuzz"));
    REQUIRE(pointer->note.TryAssign("editor only"));
    REQUIRE(pointer->soft.TryAssign(FixedId(202), "Sub"));
    REQUIRE(source.SetReference(access, asset, pointer->hard, target));
    REQUIRE(source.SetReference(access, asset, pointer->editorRef, target));
    REQUIRE(WithBorrow<TestAsset>(source, asset, [&](TestAsset& owner) {
        return source.AddReference(access, asset, owner.hardList, part).IsOk();
    }));
    PackageWriteOptions cooked = CookedOptions();
    cooked.bulkSource = &bulk;
    cooked.bulkCodec = BulkCodec::Zstd;
    const PackageDependencyEntry extra[] = {
        {FixedId(203), PackageDependencyKind::Build, TypeId{}, false},
        {FixedId(204), PackageDependencyKind::ImportOwner, TypeId{}, true}};
    PackageWriteOptions editor = EditorOptions();
    editor.bulkSource = &bulk;
    editor.extraDependencies = extra;
    const Array<uint8> validCooked = Write(env.allocator, source, package.Value().Handle(), cooked);
    const Array<uint8> validEditor = Write(env.allocator, source, package.Value().Handle(), editor);
    Array<uint8> dependencyBytes =
        Write(env.allocator, source, dependency.Value().Handle(), CookedOptions());
    REQUIRE(!validCooked.IsEmpty() && !validEditor.IsEmpty() && !dependencyBytes.IsEmpty());

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
    // 받아들인 byte를 로드해 읽은 설정 그대로 다시 쓴다(실패하면 빈 배열).
    auto resaveFile = [&](Span<const uint8> file, PackageKind kind) noexcept {
        Array<uint8> out(env.allocator);
        PackageReadOptions read;
        read.expectedKind = kind;
        read.engineVersion = TestEngineVersion;
        read.types = &env.types;
        auto contents = ReadPackage(env.allocator, file, read);
        if (!contents) {
            return out;
        }

        PackageLoadOptions loadOptions = LoadOptions(kind);
        PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                               loadOptions);
        auto prepared = state.Prepare(Copy(env.allocator, file));
        if (!prepared) {
            return out;
        }

        ObjectHandle handles[1];
        RootRef<Object> roots[1];
        {
            ConstructionContext transaction(*env.target, env.target->OwnerAccess());
            PackageLoadState* states[] = {&state};
            prepared = InstantiatePackages(transaction, states, &resolver, handles);
            if (prepared) {
                prepared = transaction.Commit(handles, roots);
            }
        }

        if (prepared) {
            out = ResaveAsRead(env.allocator, *env.target, handles[0], contents.Value());
            (void)env.target->Discard(env.target->OwnerAccess(), handles[0]);
            roots[0].Reset();
            (void)env.target->Collect(env.target->OwnerAccess());
        }

        return out;
    };
    auto fuzz = [&](const Array<uint8>& valid, PackageKind kind, uint64 seed) noexcept {
        XorShift random{seed};
        uint32 readFailures = 0;
        uint32 loadFailures = 0;
        uint32 accepted = 0;
        uint32 resaved = 0;
        uint32 normalized = 0;
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

            PackageReadOptions read;
            read.expectedKind = kind;
            read.engineVersion = TestEngineVersion;
            read.types = &env.types;
            AssetDiagnostic readFailure;
            auto contents = ReadPackage(env.allocator, bytes.View(), read, &readFailure);
            if (!contents) {
                ++readFailures;
                CHECK(AllowedReadError(contents.Error(), readFailure.code));
                continue;
            }

            // bulk raw 합 상한을 작게 둔다(변조된 rawBytes가 큰 할당을 요구하면 자원 한도
            // CapacityExceeded).
            PackageLoadOptions loadOptions = LoadOptions(kind);
            loadOptions.maxBulkRawBytes = 64 * 1024 * 1024;
            PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                                   loadOptions);
            AssetDiagnostic loadFailure;
            const Array<uint8> input = Copy(env.allocator, bytes.View());
            auto prepared = state.Prepare(Move(bytes), &loadFailure);
            if (prepared) {
                ObjectHandle handles[1];
                RootRef<Object> roots[1];
                {
                    ConstructionContext transaction(*env.target, env.target->OwnerAccess());
                    PackageLoadState* states[] = {&state};
                    prepared =
                        InstantiatePackages(transaction, states, &resolver, handles, &loadFailure);
                    if (prepared) {
                        prepared = transaction.Commit(handles, roots);
                    }
                }

                // 받아들인 입력은 한 번 다시 쓰면 정준이다: 읽기 전용이 아니면 같은 옵션으로 다시
                // 쓴 byte가 입력과 같거나, 다르면 그 byte를 다시 쓴 결과가 그대로다(차이 형식 —
                // 변조가 값을 클래스 기본값과 같게 만들면 다시 쓸 때 그 필드가 빠진다, UE delta
                // 직렬화와 같다; AssetSystem 14.1.83절). 그 밖의 규칙 구멍(같은 객체로 풀리는 다른
                // byte가 다시 쓸 때마다 바뀜)은 여기서 드러난다.
                if (prepared) {
                    Array<uint8> again(env.allocator);
                    if (!state.IsReadOnly()) {
                        ++resaved;
                        again =
                            ResaveAsRead(env.allocator, *env.target, handles[0], contents.Value());
                    }

                    CHECK(env.target->Discard(env.target->OwnerAccess(), handles[0]));
                    roots[0].Reset();
                    CHECK(env.target->Collect(env.target->OwnerAccess()));
                    if (!state.IsReadOnly() && !SameBytes(again.View(), input.View())) {
                        ++normalized;
                        const Array<uint8> twice = resaveFile(again.View(), kind);
                        CHECK(!twice.IsEmpty() && SameBytes(twice.View(), again.View()));
                    }
                }
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

        // 변조 대부분은 거부되고 받아들인 입력(값 byte만 바뀐 경우)도 객체를 남기지 않는다. 읽기
        // 거부·로드 거부·받아들임이 모두 일어나야 세 경로를 다 지난 것이다.
        CHECK(readFailures + loadFailures + accepted == Iterations);
        CHECK(readFailures > Iterations / 2);
        CHECK(loadFailures > 0);
        CHECK(accepted > 0);
        CHECK(resaved > 0 && normalized < resaved);
        CHECK(env.target->Stats().liveObjects == baseline);
    };

    fuzz(validCooked, PackageKind::Cooked, 0x5EEDF00DCAFEull);
    fuzz(validEditor, PackageKind::Editor, 0xED170A5EEDull);
}
} // namespace package_fuzz_tests_file
