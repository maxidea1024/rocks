#include "MeshSourceTestSupport.h"

#include <Base/Endian.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>

// MeshSource 저장·로드 시험이다(Docs/Design/MeshAnimationAssets.ko.md 3.4·3.7·9.1·9.2절, 12절
// "저장·로드 왕복의 비트 일치", "bulk 길이 변조·잘림·SHA 불일치", "morph -0.0 보존"): 실제 패키지
// (TestMeshAsset)로 쓰고 다른 Registry에 로드한 뒤 Load한다. 포맷 필드와 bulk byte를 하나씩 바꿔
// 정확한 규칙으로 거부되는지 본다.
using namespace rocks;
namespace mesh_source_storage_tests_file {

using namespace rocks::mesh_source_test;

namespace {

// 레코드 index의 값 bulk를 bytes로 바꾼다.
bool ReplaceRecord(MeshSourceData& data, MemoryBulkSource& bulk, uint32 index,
                   Array<uint8>&& bytes) noexcept {
    auto ref = bulk.Add(Move(bytes));
    if (!ref || index >= data.attributes.Size()) {
        return false;
    }

    data.attributes[index].data = ref.Value();
    return true;
}

// 위상 bulk를 bytes로 바꾼다.
bool ReplaceTopology(MeshSourceData& data, MemoryBulkSource& bulk, Array<uint8>&& bytes) noexcept {
    auto ref = bulk.Add(Move(bytes));
    if (!ref) {
        return false;
    }

    data.topology = ref.Value();
    return true;
}

// 고친 데이터의 Load가 code·issue로 실패하고 out이 그대로인지 본다.
template <class EditFn>
bool LoadFails(asset_test::AssetEnv& env, uint64 id, const MeshSource& mesh, EditFn&& edit,
               MeshIssue issue,
               AssetDiagnosticCode code = AssetDiagnosticCode::CorruptPackage) noexcept {
    MeshSource out(env.allocator);
    if (!out.AddVertex(Vec3(7, 7, 7))) {
        return false;
    }

    MeshDiagnostic diagnostic;
    auto loaded = LoadEdited(env, id, mesh, edit, out, diagnostic);
    if (loaded || loaded.Error() == ErrorCode::NotFound) {
        return false;
    }

    // 실패한 Load는 이 객체를 바꾸지 않는다.
    const bool kept = out.Count(MeshElement::Vertex) == 1 && out.AttributeCount() == 0;
    return kept && HasIssue(diagnostic, code, issue) && diagnostic.error == loaded.Error();
}

// 바이트 배열의 offset 위치 uint32를 바꾼다.
void PatchU32(Array<uint8>& bytes, usize offset, uint32 value) noexcept {
    StoreLittleEndian<uint32>(bytes.Data() + offset, value);
}

// 메시를 저장한 data의 레코드 순서가 9.2절 순서(element, 이름 byte)인지 본다.
bool RecordsSorted(asset_test::AssetEnv& env, const MeshSourceData& data) noexcept {
    for (usize index = 1; index < data.attributes.Size(); ++index) {
        const MeshAttributeRecord& left = data.attributes[index - 1];
        const MeshAttributeRecord& right = data.attributes[index];
        auto a = env.names.Acquire(left.name);
        auto b = env.names.Acquire(right.name);
        if (!a || !b) {
            return false;
        }

        if (left.element != right.element) {
            if (static_cast<uint8>(left.element) > static_cast<uint8>(right.element)) {
                return false;
            }

            continue;
        }

        const StringView x = a.Value().View();
        const StringView y = b.Value().View();
        const usize common = x.ByteCount() < y.ByteCount() ? x.ByteCount() : y.ByteCount();
        const int compared = platform::MemoryCompare(x.Data(), y.Data(), common);
        if (compared > 0 || (compared == 0 && x.ByteCount() >= y.ByteCount())) {
            return false;
        }
    }

    return true;
}

} // namespace

// 왕복: 모든 엔진 속성·사용자 속성을 단 메시를 쿠킹·에디터, 압축 없음·Zstd로 저장·로드해 같은
// 메시를 얻는다(위치·속성 float 비트, Name·영향·희소 값, -0.0 morph 포함).
TEST(MeshSource_StoreLoadRoundTrip) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource mesh(env.allocator);
    REQUIRE(BuildRichMesh(mesh));
    const PackageKind kinds[] = {PackageKind::Cooked, PackageKind::Editor};
    const BulkCodec codecs[] = {BulkCodec::None, BulkCodec::Zstd};
    uint64 id = 100;
    for (PackageKind kind : kinds) {
        for (BulkCodec codec : codecs) {
            MeshSource loaded(env.allocator);
            MeshDiagnostic diagnostic;
            REQUIRE(RoundTripMesh(env, id++, kind, mesh, loaded, &diagnostic, codec));
            CHECK(SameMesh(mesh, loaded));
            CHECK(diagnostic.code == AssetDiagnosticCode::None);
            const uint32 morph = loaded.FindAttribute(MeshElement::Vertex, "Morph-Smile");
            REQUIRE(morph != MeshNoAttribute);
            CHECK(platform::BitCast<uint32>(loaded.SparseValue(morph, 1).y) == 0x80000000u);
            CHECK(platform::BitCast<uint32>(loaded.SparseValue(morph, 4).z) == 0x80000000u);
            // Load 뒤 속성 순서는 9.2절 정렬 순서다(정점 → 코너 …, 이름 byte 순).
            CHECK(loaded.AttributeInfo(0).element == MeshElement::Vertex);
            CHECK(loaded.AttributeInfo(0).name == StringView("Morph-Smile"));
        }
    }
}

// 저장 형태: 레코드는 (element, 이름 byte) 엄격 증가 순이고 정점 Position 레코드가 있으며, 같은
// 메시를 두 번 저장하면 같은 byte다. 저장 → 로드 → 재저장한 패키지 파일은 처음 파일과 byte가 같다.
TEST(MeshSource_StoreIsDeterministicAndResaveIdentical) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource mesh(env.allocator);
    REQUIRE(BuildRichMesh(mesh));
    MemoryBulkSource first(env.allocator);
    MemoryBulkSource second(env.allocator);
    MeshSourceData a(env.allocator);
    MeshSourceData b(env.allocator);
    REQUIRE(mesh.Store(first, env.names, a));
    REQUIRE(mesh.Store(second, env.names, b));
    REQUIRE(a.attributes.Size() == mesh.AttributeCount() + 1);
    CHECK(RecordsSorted(env, a));
    CHECK(FindRecord(env, a, MeshElement::Vertex, "Position") != MeshNoAttribute);
    CHECK(a.topology == b.topology);
    for (usize index = 0; index < a.attributes.Size(); ++index) {
        CHECK(a.attributes[index].data == b.attributes[index].data);
        CHECK(a.attributes[index].type == b.attributes[index].type);
    }

    // 원본 패키지 → 로드 → 다른 환경에서 같은 id로 재저장.
    MeshPackage original(env.allocator);
    REQUIRE(WriteAndLoad(
        env, 200, PackageKind::Editor, BulkCodec::None,
        [&](TestMeshAsset& asset, MemoryBulkSource& bulk) {
            return mesh.Store(bulk, env.names, asset.mesh).IsOk();
        },
        original));
    MeshSource loaded(env.allocator);
    REQUIRE(
        loaded.Load(LoadedAsset(env, original)->mesh, *LoadedPackage(env, original), env.names));
    asset_test::AssetEnv again;
    REQUIRE(again.Ready());
    MeshPackage resaved(again.allocator);
    REQUIRE(WriteAndLoad(
        again, 200, PackageKind::Editor, BulkCodec::None,
        [&](TestMeshAsset& asset, MemoryBulkSource& bulk) {
            return loaded.Store(bulk, again.names, asset.mesh).IsOk();
        },
        resaved));
    CHECK(asset_test::SameBytes(original.bytes.View(), resaved.bytes.View()));
}

// 한 bulk 원본을 공유하는 여러 원본(LOD 배열·EditorOnly 필드): 에디터 패키지는 셋 다 읽고, 쿠킹
// 패키지는 EditorOnly 원본이 빠져 빈 ref라 BulkUnreadable(CorruptPackage)이다.
TEST(MeshSource_SeveralSourcesShareBulk) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource quad(env.allocator);
    REQUIRE(BuildQuad(quad));
    MeshSource rich(env.allocator);
    REQUIRE(BuildRichMesh(rich));
    const PackageKind kinds[] = {PackageKind::Editor, PackageKind::Cooked};
    uint64 id = 300;
    for (PackageKind kind : kinds) {
        MeshPackage loaded(env.allocator);
        REQUIRE(WriteAndLoad(
            env, id++, kind, BulkCodec::Zstd,
            [&](TestMeshAsset& asset, MemoryBulkSource& bulk) {
                asset.lods.Clear();
                if (!asset.lods.TryEmplaceBack(asset.lods.GetAllocator()) ||
                    !asset.lods.TryEmplaceBack(asset.lods.GetAllocator())) {
                    return false;
                }

                return quad.Store(bulk, env.names, asset.lods[0]).IsOk() &&
                       rich.Store(bulk, env.names, asset.lods[1]).IsOk() &&
                       rich.Store(bulk, env.names, asset.editorMesh).IsOk() &&
                       quad.Store(bulk, env.names, asset.mesh).IsOk();
            },
            loaded));
        TestMeshAsset* asset = LoadedAsset(env, loaded);
        Package* package = LoadedPackage(env, loaded);
        REQUIRE(asset != nullptr && package != nullptr && asset->lods.Size() == 2);
        MeshSource lod0(env.allocator);
        MeshSource lod1(env.allocator);
        MeshSource main(env.allocator);
        CHECK(lod0.Load(asset->lods[0], *package, env.names));
        CHECK(lod1.Load(asset->lods[1], *package, env.names));
        CHECK(main.Load(asset->mesh, *package, env.names));
        CHECK(SameMesh(lod0, quad) && SameMesh(lod1, rich) && SameMesh(main, quad));
        MeshSource editor(env.allocator);
        MeshDiagnostic diagnostic;
        auto read = editor.Load(asset->editorMesh, *package, env.names, &diagnostic);
        if (kind == PackageKind::Editor) {
            CHECK(read && SameMesh(editor, rich));
        } else {
            CHECK(!read && read.Error() == ErrorCode::InvalidArgument);
            CHECK(HasIssue(diagnostic, AssetDiagnosticCode::CorruptPackage,
                           MeshIssue::BulkUnreadable));
        }
    }
}

// 경계: 요소 수 0인 빈 메시와 정점만 있는 메시는 정상 왕복한다(위상 blob은 둘레 시작 표 [0] 4 byte,
// 위치 레코드는 0 byte entry).
TEST(MeshSource_EmptyMeshRoundTrip) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource empty(env.allocator);
    MemoryBulkSource bulk(env.allocator);
    MeshSourceData data(env.allocator);
    REQUIRE(empty.Store(bulk, env.names, data));
    CHECK(data.topology.rawBytes == 4);
    REQUIRE(data.attributes.Size() == 1);
    CHECK(data.attributes[0].data.rawBytes == 0 && !data.attributes[0].data.IsEmpty());
    MeshSource loaded(env.allocator);
    REQUIRE(loaded.AddVertex(Vec3(1, 1, 1)));
    REQUIRE(RoundTripMesh(env, 400, PackageKind::Cooked, empty, loaded));
    CHECK(SameMesh(empty, loaded));
    MeshSource points(env.allocator);
    REQUIRE(points.AddVertex(Vec3(-0.0f, 1.0f, 2.0f)));
    REQUIRE(points.AddVertex(Vec3(3.0f, 4.0f, 5.0f)));
    REQUIRE(points.AddPolygonGroup());
    REQUIRE(RoundTripMesh(env, 401, PackageKind::Editor, points, loaded));
    CHECK(SameMesh(points, loaded));
    CHECK(platform::BitCast<uint32>(loaded.Positions()[0].x) == 0x80000000u);
}

// 저장 전 검증: 불변식을 어긴 메시는 저장하지 않고(InvalidMeshData) data를 바꾸지 않는다.
TEST(MeshSource_StoreRejectsInvalidMesh) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource mesh(env.allocator);
    REQUIRE(BuildQuad(mesh));
    REQUIRE(mesh.AddEdge(3, 3));
    MemoryBulkSource bulk(env.allocator);
    MeshSourceData data(env.allocator);
    data.vertexCount = 77;
    MeshDiagnostic diagnostic;
    auto stored = mesh.Store(bulk, env.names, data, &diagnostic);
    CHECK(!stored && stored.Error() == ErrorCode::InvalidArgument);
    CHECK(HasIssue(diagnostic, AssetDiagnosticCode::InvalidMeshData, MeshIssue::EdgeSameVertex));
    CHECK(data.vertexCount == 77 && data.attributes.IsEmpty() && bulk.Count() == 0);
}

// 포맷 필드 변조(리플렉션 필드): 버전, 요소 수 상한·경계, 위상 ref(빈 값·크기·SHA), 레코드
// element·값 타입·채널·이름·순서·중복, Position 레코드 누락·형태.
TEST(MeshSource_LoadRejectsFormatFields) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource mesh(env.allocator);
    REQUIRE(BuildRichMesh(mesh));
    uint64 id = 500;
    auto fails = [&](auto&& edit, MeshIssue issue,
                     AssetDiagnosticCode code = AssetDiagnosticCode::CorruptPackage) {
        return LoadFails(env, id++, mesh, edit, issue, code);
    };

    CHECK(fails(
        [](MeshSourceData& data, MemoryBulkSource&) {
            data.formatVersion = MeshSourceFormatVersion + 1;
            return true;
        },
        MeshIssue::FormatVersion, AssetDiagnosticCode::UnsupportedVersion));
    CHECK(fails(
        [](MeshSourceData& data, MemoryBulkSource&) {
            data.formatVersion = 0;
            return true;
        },
        MeshIssue::FormatVersion, AssetDiagnosticCode::UnsupportedVersion));
    // 요소 종류마다 상한 + 1은 ElementCountTooLarge, 상한 그대로는 그 판정을 지나 길이에서 걸린다.
    uint32 MeshSourceData::* counts[] = {
        &MeshSourceData::vertexCount,   &MeshSourceData::cornerCount,
        &MeshSourceData::edgeCount,     &MeshSourceData::polygonCount,
        &MeshSourceData::triangleCount, &MeshSourceData::polygonGroupCount};
    for (auto count : counts) {
        CHECK(fails(
            [&](MeshSourceData& data, MemoryBulkSource&) {
                data.*count = MeshMaxElements + 1;
                return true;
            },
            MeshIssue::ElementCountTooLarge));
    }

    CHECK(fails(
        [](MeshSourceData& data, MemoryBulkSource&) {
            data.cornerCount = MeshMaxElements;
            return true;
        },
        MeshIssue::TopologyLength));
    CHECK(fails(
        [](MeshSourceData& data, MemoryBulkSource&) {
            data.vertexCount = MeshMaxElements;
            return true;
        },
        MeshIssue::AttributeLength));
    // 묶음 수를 줄이면 위상 길이는 맞고(묶음은 위상에 없다) 묶음 속성(MaterialSlotName) 길이에서,
    // 묶음 속성이 없는 사각형이면 내용 검증(묶음 번호 범위)에서 걸린다.
    CHECK(fails(
        [](MeshSourceData& data, MemoryBulkSource&) {
            data.polygonGroupCount = 1;
            return true;
        },
        MeshIssue::AttributeLength));
    MeshSource quad(env.allocator);
    REQUIRE(BuildQuad(quad));
    CHECK(LoadFails(
        env, id++, quad,
        [](MeshSourceData& data, MemoryBulkSource&) {
            data.polygonGroupCount = 0;
            return true;
        },
        MeshIssue::PolygonGroupRange));
    // 삼각형 수를 늘리면 둘레 시작 표는 제자리라 전체 길이가 어긋나고, 변 수를 늘리면 시작 표
    // 자리가 밀려 코너 정점(시작 표로 읽힘)의 첫 값이 0이 아니게 된다.
    CHECK(fails(
        [](MeshSourceData& data, MemoryBulkSource&) {
            data.triangleCount += 1;
            return true;
        },
        MeshIssue::TopologyLength));
    CHECK(fails(
        [](MeshSourceData& data, MemoryBulkSource&) {
            data.edgeCount += 1;
            return true;
        },
        MeshIssue::PolygonStarts));
    CHECK(fails(
        [](MeshSourceData& data, MemoryBulkSource&) {
            data.topology = BulkDataRef{};
            return true;
        },
        MeshIssue::BulkUnreadable));
    // 레코드 element·값 타입·채널 수.
    auto recordKind = [&](auto&& change) {
        return fails(
            [&](MeshSourceData& data, MemoryBulkSource&) {
                change(data.attributes[3]);
                return true;
            },
            MeshIssue::AttributeRecordKind);
    };
    CHECK(recordKind([](MeshAttributeRecord& record) { record.element = MeshElement::Invalid; }));
    CHECK(recordKind([](MeshAttributeRecord& record) { record.type = MeshValueType::Invalid; }));
    CHECK(recordKind([](MeshAttributeRecord& record) { record.channels = 0; }));
    CHECK(fails(
        [&](MeshSourceData& data, MemoryBulkSource&) {
            const uint32 skin = FindRecord(env, data, MeshElement::Vertex, "SkinWeights");
            data.attributes[skin].channels = 2;
            return true;
        },
        MeshIssue::AttributeRecordKind));
    // 레코드 이름: None.
    CHECK(fails(
        [](MeshSourceData& data, MemoryBulkSource&) {
            data.attributes[2].name = Name{};
            return true;
        },
        MeshIssue::AttributeRecordName));
    // 순서: 두 레코드를 바꿈, 같은 레코드를 두 번(같은 이름 byte — 엄격 증가 위반).
    CHECK(fails(
        [](MeshSourceData& data, MemoryBulkSource&) {
            const MeshAttributeRecord swap = data.attributes[1];
            data.attributes[1] = data.attributes[2];
            data.attributes[2] = swap;
            return true;
        },
        MeshIssue::AttributeOrder));
    CHECK(fails(
        [](MeshSourceData& data, MemoryBulkSource&) {
            return data.attributes.TryInsert(MeshAttributeRecord(data.attributes[4]), 4).IsOk();
        },
        MeshIssue::AttributeOrder));
    // 앞 이름이 뒤 이름의 접두인데 더 긴 쪽이 먼저("SkinWeights-Cloth" → "SkinWeights").
    CHECK(fails(
        [&](MeshSourceData& data, MemoryBulkSource&) {
            const uint32 skin = FindRecord(env, data, MeshElement::Vertex, "SkinWeights");
            const MeshAttributeRecord swap = data.attributes[skin];
            data.attributes[skin] = data.attributes[skin + 1];
            data.attributes[skin + 1] = swap;
            return true;
        },
        MeshIssue::AttributeOrder));
    // element 순서가 거꾸로(코너 레코드를 맨 앞으로).
    CHECK(fails(
        [&](MeshSourceData& data, MemoryBulkSource&) {
            const uint32 normal = FindRecord(env, data, MeshElement::Corner, "Normal");
            const MeshAttributeRecord record = data.attributes[normal];
            return data.attributes.TryRemoveAt(normal).IsOk() &&
                   data.attributes.TryInsert(record, 0).IsOk();
        },
        MeshIssue::AttributeOrder));
    // Position 레코드 누락·형태.
    CHECK(fails(
        [&](MeshSourceData& data, MemoryBulkSource&) {
            const uint32 position = FindRecord(env, data, MeshElement::Vertex, "Position");
            return data.attributes.TryRemoveAt(position).IsOk();
        },
        MeshIssue::MissingPosition));
    CHECK(fails(
        [&](MeshSourceData& data, MemoryBulkSource&) {
            const uint32 position = FindRecord(env, data, MeshElement::Vertex, "Position");
            data.attributes[position].type = MeshValueType::Vec2;
            return true;
        },
        MeshIssue::MissingPosition));
    CHECK(fails(
        [&](MeshSourceData& data, MemoryBulkSource&) {
            const uint32 position = FindRecord(env, data, MeshElement::Vertex, "Position");
            data.attributes[position].channels = 2;
            return true;
        },
        MeshIssue::MissingPosition));
}

// 대소문자만 다른 레코드 이름(Name 비교 규칙으로 같은 이름)은 byte 순서는 맞아도 내용 검증의
// DuplicateAttributeName이다. 정점 "POSITION"과 "Position"도 같은 이름이다.
TEST(MeshSource_LoadRejectsCaseOnlyDuplicateNames) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource mesh(env.allocator);
    REQUIRE(BuildRichMesh(mesh));
    // "NORMAL" < "Normal"(byte) — Normal 레코드 앞에 같은 bulk를 가진 "NORMAL" 레코드를 넣는다.
    CHECK(LoadFails(
        env, 600, mesh,
        [&](MeshSourceData& data, MemoryBulkSource&) {
            const uint32 normal = FindRecord(env, data, MeshElement::Corner, "Normal");
            MeshAttributeRecord copy = data.attributes[normal];
            auto token = env.names.TryIntern("NORMAL");
            if (!token) {
                return false;
            }

            copy.name = token.Value();
            return data.attributes.TryInsert(copy, normal).IsOk();
        },
        MeshIssue::DuplicateAttributeName));
    CHECK(LoadFails(
        env, 601, mesh,
        [&](MeshSourceData& data, MemoryBulkSource&) {
            // byte 순서: "Morph-Smile" < "POSITION" < "Pinned" < "Position".
            const uint32 position = FindRecord(env, data, MeshElement::Vertex, "Position");
            const uint32 pinned = FindRecord(env, data, MeshElement::Vertex, "Pinned");
            MeshAttributeRecord copy = data.attributes[position];
            auto token = env.names.TryIntern("POSITION");
            if (!token) {
                return false;
            }

            copy.name = token.Value();
            return data.attributes.TryInsert(copy, pinned).IsOk();
        },
        MeshIssue::DuplicateAttributeName));
    // 엔진 이름 형태 위반은 로드에서도 내용 검증으로 거부한다.
    CHECK(LoadFails(
        env, 602, mesh,
        [&](MeshSourceData& data, MemoryBulkSource& bulk) {
            const uint32 sign = FindRecord(env, data, MeshElement::Corner, "BinormalSign");
            Array<uint8> bytes = BulkCopy(env.allocator, bulk, data.attributes[sign].data);
            if (!bytes.TryAppend(Span<const uint8>(bytes.Data(), bytes.Size()))) {
                return false;
            }

            data.attributes[sign].channels = 2;
            return ReplaceRecord(data, bulk, sign, Move(bytes));
        },
        MeshIssue::EngineAttributeMismatch));
}

// 직접 받은 data의 레코드 필드: 구조가 잘못된 이름 토큰은 AttributeRecordName(CorruptPackage), 다른
// pool의 토큰은 진단 코드 없이 pool 오류(호출자가 pool을 잘못 넘긴 것), 열거 밖 element·값 타입은
// AttributeRecordKind다.
TEST(MeshSource_LoadRecordNameTokens) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource mesh(env.allocator);
    REQUIRE(BuildQuad(mesh));
    REQUIRE(mesh.AddAttribute(MeshElement::Corner, "Normal", MeshValueType::Vec3, 1));
    MeshPackage loaded(env.allocator);
    REQUIRE(WriteAndLoad(
        env, 650, PackageKind::Editor, BulkCodec::None,
        [&](TestMeshAsset& asset, MemoryBulkSource& bulk) {
            return mesh.Store(bulk, env.names, asset.mesh).IsOk();
        },
        loaded));
    TestMeshAsset* asset = LoadedAsset(env, loaded);
    Package* package = LoadedPackage(env, loaded);
    REQUIRE(asset != nullptr && package != nullptr);
    MeshAttributeRecord& record = asset->mesh.attributes[1];
    const Name original = record.name;
    record.name.kind = static_cast<NameKind>(9);
    MeshSource out(env.allocator);
    MeshDiagnostic diagnostic;
    auto read = out.Load(asset->mesh, *package, env.names, &diagnostic);
    CHECK(!read && HasIssue(diagnostic, AssetDiagnosticCode::CorruptPackage,
                            MeshIssue::AttributeRecordName));
    record.name = original;
    NamePool other(env.allocator);
    REQUIRE(other.Initialize());
    MeshDiagnostic foreign;
    read = out.Load(asset->mesh, *package, other, &foreign);
    CHECK(!read && read.Error() != ErrorCode::InvalidArgument);
    CHECK(foreign.code == AssetDiagnosticCode::None);
    REQUIRE(other.Shutdown());
    // 패키지 쓰기가 받지 않는 열거 밖 값도 Load는 직접 받은 data에서 거부한다.
    record.element = static_cast<MeshElement>(7);
    MeshDiagnostic element;
    CHECK(!out.Load(asset->mesh, *package, env.names, &element));
    CHECK(HasIssue(element, AssetDiagnosticCode::CorruptPackage, MeshIssue::AttributeRecordKind));
    record.element = MeshElement::Corner;
    record.type = static_cast<MeshValueType>(10);
    MeshDiagnostic type;
    CHECK(!out.Load(asset->mesh, *package, env.names, &type));
    CHECK(HasIssue(type, AssetDiagnosticCode::CorruptPackage, MeshIssue::AttributeRecordKind));
    record.type = MeshValueType::Vec3;
    // 진단 없이 불러도 내용 위반(묶음 번호 범위)은 실패다.
    asset->mesh.polygonGroupCount = 0;
    auto content = out.Load(asset->mesh, *package, env.names);
    CHECK(!content && content.Error() == ErrorCode::InvalidArgument);
    asset->mesh.polygonGroupCount = 1;
    CHECK(out.Load(asset->mesh, *package, env.names));
}

// ref 변조: 크기·SHA-256이 entry와 다르면 Package::BulkBytes가 거부하고 Load는 BulkUnreadable이다.
TEST(MeshSource_LoadRejectsRefMismatch) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource mesh(env.allocator);
    REQUIRE(BuildRichMesh(mesh));
    MeshPackage loaded(env.allocator);
    REQUIRE(WriteAndLoad(
        env, 700, PackageKind::Editor, BulkCodec::None,
        [&](TestMeshAsset& asset, MemoryBulkSource& bulk) {
            return mesh.Store(bulk, env.names, asset.mesh).IsOk();
        },
        loaded));
    TestMeshAsset* asset = LoadedAsset(env, loaded);
    Package* package = LoadedPackage(env, loaded);
    REQUIRE(asset != nullptr && package != nullptr);
    MeshSourceData& data = asset->mesh;
    auto expect = [&](MeshElement element, uint32 attribute) {
        MeshSource out(env.allocator);
        MeshDiagnostic diagnostic;
        auto read = out.Load(data, *package, env.names, &diagnostic);
        return !read &&
               HasIssue(diagnostic, AssetDiagnosticCode::CorruptPackage,
                        MeshIssue::BulkUnreadable) &&
               diagnostic.element == element && diagnostic.attribute == attribute &&
               diagnostic.error == read.Error();
    };
    const BulkDataRef topology = data.topology;
    data.topology.rawBytes += 1;
    CHECK(expect(MeshElement::Invalid, MeshNoAttribute));
    data.topology = topology;
    data.topology.sha256.bytes[0] ^= 1;
    CHECK(expect(MeshElement::Invalid, MeshNoAttribute));
    data.topology = topology;
    data.topology.bulkId = package->BulkCount() + 1;
    CHECK(expect(MeshElement::Invalid, MeshNoAttribute));
    data.topology = topology;
    const BulkDataRef record = data.attributes[5].data;
    data.attributes[5].data.sha256.bytes[31] ^= 0x80;
    CHECK(expect(data.attributes[5].element, 5));
    data.attributes[5].data = record;
    MeshSource out(env.allocator);
    CHECK(out.Load(data, *package, env.names));
}

// 패키지 파일의 bulk byte 하나를 바꾸면(섹션 CRC는 맞춤) 패키지 로드가 SHA-256 대조로
// CorruptPackage다(손상이 메시 Load까지 오지 않는다).
TEST(MeshSource_PackageBulkTamperIsCorrupt) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource mesh(env.allocator);
    REQUIRE(BuildRichMesh(mesh));
    MeshPackage loaded(env.allocator);
    REQUIRE(WriteAndLoad(
        env, 750, PackageKind::Cooked, BulkCodec::None,
        [&](TestMeshAsset& asset, MemoryBulkSource& bulk) {
            return mesh.Store(bulk, env.names, asset.mesh).IsOk();
        },
        loaded));
    Array<uint8> file = asset_test::Copy(env.allocator, loaded.bytes.View());
    usize row = 0;
    uint64 offset = 0;
    uint64 bytes = 0;
    REQUIRE(asset_test::FindSection(file.View(), PackageSectionKind::Bulk, row, offset, bytes));
    file[static_cast<usize>(offset + bytes - 1)] ^= 0x5a;
    asset_test::FixSectionCrc(file, PackageSectionKind::Bulk);
    Array<uint8> files[1] = {Move(file)};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    asset_test::AssetEnv other;
    REQUIRE(other.Ready());
    AssetDiagnostic failure;
    CHECK(!asset_test::LoadAll(other, *other.target, files, PackageKind::Cooked, nullptr, roots,
                               handles, &failure));
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage);
}

// 위상 blob 변조: 길이(잘림·덧붙임·시작 표도 못 담는 짧음), 둘레 시작 표(첫 값·감소·상한), 내용
// (변 순서 거꾸로·코너 정점 범위 — 로드 중 내용 위반도 CorruptPackage).
TEST(MeshSource_LoadRejectsTopologyBytes) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource mesh(env.allocator);
    REQUIRE(BuildSharedMesh(mesh));
    // 공유 메시: 코너 7, 변 7, 다각형 4, 둘레 14, 삼각형 6. 시작 표는 코너 7 + 변 14 = 21번째 word.
    const usize startsOffset = (7 + 2 * 7) * 4;
    uint64 id = 800;
    auto topology = [&](auto&& change, MeshIssue issue) {
        return LoadFails(
            env, id++, mesh,
            [&](MeshSourceData& data, MemoryBulkSource& bulk) {
                Array<uint8> bytes = BulkCopy(env.allocator, bulk, data.topology);
                if (!change(bytes)) {
                    return false;
                }

                return ReplaceTopology(data, bulk, Move(bytes));
            },
            issue);
    };
    CHECK(topology([](Array<uint8>& bytes) { return bytes.TryResize(bytes.Size() - 4).IsOk(); },
                   MeshIssue::TopologyLength));
    CHECK(topology([](Array<uint8>& bytes) { return bytes.TryResize(bytes.Size() + 4).IsOk(); },
                   MeshIssue::TopologyLength));
    CHECK(topology([&](Array<uint8>& bytes) { return bytes.TryResize(startsOffset).IsOk(); },
                   MeshIssue::TopologyLength));
    CHECK(topology(
        [&](Array<uint8>& bytes) {
            PatchU32(bytes, startsOffset, 1);
            return true;
        },
        MeshIssue::PolygonStarts));
    CHECK(topology(
        [&](Array<uint8>& bytes) {
            // 다각형 1의 끝(8)을 다각형 0의 끝(4)보다 작게.
            PatchU32(bytes, startsOffset + 2 * 4, 3);
            return true;
        },
        MeshIssue::PolygonStarts));
    CHECK(topology(
        [&](Array<uint8>& bytes) {
            PatchU32(bytes, startsOffset + 4 * 4, MeshMaxElements + 1);
            return true;
        },
        MeshIssue::PolygonStarts));
    // 둘레 끝을 하나 늘리면 길이가 어긋난다.
    CHECK(topology(
        [&](Array<uint8>& bytes) {
            PatchU32(bytes, startsOffset + 4 * 4, 15);
            return true;
        },
        MeshIssue::TopologyLength));
    CHECK(topology(
        [&](Array<uint8>& bytes) {
            // 변 0(0, 1)을 (1, 0)으로.
            PatchU32(bytes, 7 * 4, 1);
            PatchU32(bytes, 7 * 4 + 4, 0);
            return true;
        },
        MeshIssue::EdgeOrder));
    CHECK(topology(
        [&](Array<uint8>& bytes) {
            PatchU32(bytes, 4, 6);
            return true;
        },
        MeshIssue::CornerVertexRange));
}

// 속성 값 bulk 변조: 고정 크기(Float·Vec·UInt32·Bool) 길이, Bool 0·1 밖, 위치 길이.
TEST(MeshSource_LoadRejectsFixedValueBytes) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource mesh(env.allocator);
    REQUIRE(BuildRichMesh(mesh));
    uint64 id = 900;
    auto record = [&](MeshElement element, StringView name, auto&& change, MeshIssue issue) {
        return LoadFails(
            env, id++, mesh,
            [&](MeshSourceData& data, MemoryBulkSource& bulk) {
                const uint32 index = FindRecord(env, data, element, name);
                if (index == MeshNoAttribute) {
                    return false;
                }

                Array<uint8> bytes = BulkCopy(env.allocator, bulk, data.attributes[index].data);
                return change(bytes) && ReplaceRecord(data, bulk, index, Move(bytes));
            },
            issue);
    };
    auto shorter = [](Array<uint8>& bytes) { return bytes.TryResize(bytes.Size() - 1).IsOk(); };
    auto longer = [](Array<uint8>& bytes) { return bytes.TryResize(bytes.Size() + 4).IsOk(); };
    CHECK(record(MeshElement::Vertex, "Position", shorter, MeshIssue::AttributeLength));
    CHECK(record(MeshElement::Vertex, "Position", longer, MeshIssue::AttributeLength));
    CHECK(record(MeshElement::Corner, "Normal", shorter, MeshIssue::AttributeLength));
    CHECK(record(MeshElement::Corner, "BinormalSign", longer, MeshIssue::AttributeLength));
    CHECK(record(MeshElement::Corner, "TexCoord", shorter, MeshIssue::AttributeLength));
    CHECK(record(MeshElement::Triangle, "SourceIds", shorter, MeshIssue::AttributeLength));
    CHECK(record(MeshElement::Edge, "IsHard", longer, MeshIssue::AttributeLength));
    CHECK(record(MeshElement::Vertex, "Pinned", shorter, MeshIssue::AttributeLength));
    CHECK(record(
        MeshElement::Vertex, "Pinned",
        [](Array<uint8>& bytes) {
            bytes[bytes.Size() - 1] = 2;
            return true;
        },
        MeshIssue::BoolValue));
}

// Name 값 bulk 변조: 길이 칸이 없음, 길이 > MaxNameFieldBytes, 길이가 남은 byte보다 김, 남는 byte,
// 잘못된 UTF-8(내용 검증 NameValue).
TEST(MeshSource_LoadRejectsNameValueBytes) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource mesh(env.allocator);
    REQUIRE(BuildRichMesh(mesh));
    uint64 id = 1000;
    auto part = [&](auto&& change, MeshIssue issue) {
        return LoadFails(
            env, id++, mesh,
            [&](MeshSourceData& data, MemoryBulkSource& bulk) {
                const uint32 index = FindRecord(env, data, MeshElement::Polygon, "PartName");
                Array<uint8> bytes = BulkCopy(env.allocator, bulk, data.attributes[index].data);
                return change(bytes) && ReplaceRecord(data, bulk, index, Move(bytes));
            },
            issue);
    };
    // PartName: 다각형 4개, 값 "Body"·"Visor"·"팔"·"Trim_7"(4 + 4, 4 + 5, 4 + 3, 4 + 6 byte).
    CHECK(part([](Array<uint8>& bytes) { return bytes.TryResize(8 + 9 + 7).IsOk(); },
               MeshIssue::AttributeLength));
    CHECK(part([](Array<uint8>& bytes) { return bytes.TryResize(bytes.Size() - 1).IsOk(); },
               MeshIssue::AttributeLength));
    CHECK(part([](Array<uint8>& bytes) { return bytes.TryPushBack(static_cast<uint8>(0)).IsOk(); },
               MeshIssue::AttributeLength));
    CHECK(part(
        [](Array<uint8>& bytes) {
            PatchU32(bytes, 8, static_cast<uint32>(MaxNameFieldBytes + 1));
            return true;
        },
        MeshIssue::NameValueLength));
    CHECK(part(
        [](Array<uint8>& bytes) {
            // 첫 값 길이를 남은 byte보다 길게(상한 안).
            PatchU32(bytes, 0, 1000);
            return true;
        },
        MeshIssue::AttributeLength));
    CHECK(part(
        [](Array<uint8>& bytes) {
            bytes[4] = 0xff;
            return true;
        },
        MeshIssue::NameValue));
}

// BoneInfluences bulk 변조: 시작 표가 짧음, 첫 값 0 아님, 감소, 차가 상한 초과, 영향 byte 수
// 불일치, 한 정점 안 같은 본(내용 검증).
TEST(MeshSource_LoadRejectsInfluenceBytes) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource mesh(env.allocator);
    REQUIRE(BuildRichMesh(mesh));
    uint64 id = 1100;
    // SkinWeights: 정점 6개, 영향 수 2,2,1,2,2,1 → 시작 표 [0,2,4,5,7,9,10](7 word) + 영향 10개.
    const usize table = 7 * 4;
    auto skin = [&](auto&& change, MeshIssue issue) {
        return LoadFails(
            env, id++, mesh,
            [&](MeshSourceData& data, MemoryBulkSource& bulk) {
                const uint32 index = FindRecord(env, data, MeshElement::Vertex, "SkinWeights");
                Array<uint8> bytes = BulkCopy(env.allocator, bulk, data.attributes[index].data);
                return bytes.Size() == table + 10 * 4 && change(bytes) &&
                       ReplaceRecord(data, bulk, index, Move(bytes));
            },
            issue);
    };
    CHECK(skin([&](Array<uint8>& bytes) { return bytes.TryResize(table - 4).IsOk(); },
               MeshIssue::AttributeLength));
    CHECK(skin(
        [](Array<uint8>& bytes) {
            PatchU32(bytes, 0, 1);
            return true;
        },
        MeshIssue::InfluenceStarts));
    CHECK(skin(
        [](Array<uint8>& bytes) {
            PatchU32(bytes, 2 * 4, 1);
            return true;
        },
        MeshIssue::InfluenceStarts));
    CHECK(skin(
        [](Array<uint8>& bytes) {
            PatchU32(bytes, 1 * 4, MeshMaxSourceInfluences + 1);
            return true;
        },
        MeshIssue::InfluenceStarts));
    CHECK(skin([](Array<uint8>& bytes) { return bytes.TryResize(bytes.Size() - 4).IsOk(); },
               MeshIssue::AttributeLength));
    CHECK(skin(
        [&](Array<uint8>& bytes) {
            // 정점 0의 둘째 영향 boneIndex를 첫째(0)와 같게.
            StoreLittleEndian<uint16>(bytes.Data() + table + 4, 0);
            return true;
        },
        MeshIssue::DuplicateInfluenceBone));
}

// SparseVec3 bulk 변조: 개수 칸 없음, 길이 불일치, 번호 범위 밖·같음·감소, +0.0 항목. -0.0 항목은
// 정상이다(저장한 것 그대로).
TEST(MeshSource_LoadRejectsSparseBytes) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource mesh(env.allocator);
    REQUIRE(BuildRichMesh(mesh));
    uint64 id = 1200;
    // Morph-Smile: 항목 둘(정점 1 (1.5, -0, 0), 정점 4 (-0, -0, -0)) → 4 + 8 + 24 byte.
    auto morph = [&](auto&& change, MeshIssue issue) {
        return LoadFails(
            env, id++, mesh,
            [&](MeshSourceData& data, MemoryBulkSource& bulk) {
                const uint32 index = FindRecord(env, data, MeshElement::Vertex, "Morph-Smile");
                Array<uint8> bytes = BulkCopy(env.allocator, bulk, data.attributes[index].data);
                return bytes.Size() == 36 && change(bytes) &&
                       ReplaceRecord(data, bulk, index, Move(bytes));
            },
            issue);
    };
    CHECK(morph([](Array<uint8>& bytes) { return bytes.TryResize(3).IsOk(); },
                MeshIssue::AttributeLength));
    CHECK(morph(
        [](Array<uint8>& bytes) {
            PatchU32(bytes, 0, 3);
            return true;
        },
        MeshIssue::AttributeLength));
    CHECK(morph(
        [](Array<uint8>& bytes) {
            PatchU32(bytes, 8, 6);
            return true;
        },
        MeshIssue::SparseIndexRange));
    CHECK(morph(
        [](Array<uint8>& bytes) {
            PatchU32(bytes, 8, 1);
            return true;
        },
        MeshIssue::SparseIndexOrder));
    CHECK(morph(
        [](Array<uint8>& bytes) {
            PatchU32(bytes, 4, 5);
            return true;
        },
        MeshIssue::SparseIndexOrder));
    CHECK(morph(
        [](Array<uint8>& bytes) {
            // 정점 4 항목을 +0.0 셋으로.
            PatchU32(bytes, 24, 0);
            PatchU32(bytes, 28, 0);
            PatchU32(bytes, 32, 0);
            return true;
        },
        MeshIssue::SparseZeroEntry));
}

// Store 실패 주입: 메시 allocator·bulk allocator의 모든 할당 지점과 이름 pool 상한. 실패하면 data는
// 그대로이고 진단 코드는 없다.
TEST(MeshSource_StoreFailures) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource expected(env.allocator);
    REQUIRE(BuildRichMesh(expected));
    bool meshDone = false;
    for (uint64 attempt = 1; attempt <= 1024 && !meshDone; ++attempt) {
        test::NFailAllocator failing(env.allocator);
        MeshSource mesh(failing);
        REQUIRE(BuildRichMesh(mesh));
        MemoryBulkSource bulk(failing);
        // 결과 레코드 배열도 같은 실패 주입 allocator를 쓴다.
        MeshSourceData data(failing);
        data.vertexCount = 99;
        failing.FailOnAttempt(attempt);
        MeshDiagnostic diagnostic;
        auto stored = mesh.Store(bulk, env.names, data, &diagnostic);
        failing.DisableFailure();
        if (stored) {
            meshDone = true;
            CHECK(data.vertexCount == 6);
        } else {
            CHECK(stored.Error() == ErrorCode::OutOfMemory);
            CHECK(diagnostic.code == AssetDiagnosticCode::None);
            CHECK(data.vertexCount == 99 && data.attributes.IsEmpty());
            CHECK(SameMesh(mesh, expected));
        }
    }

    CHECK(meshDone);
    // 이름 pool 상한: intern이 실패하면 그 오류다.
    NamePool tiny(env.allocator);
    NamePoolConfig config;
    config.maxEntries = 2;
    REQUIRE(tiny.Initialize(config));
    {
        MemoryBulkSource bulk(env.allocator);
        MeshSourceData data(env.allocator);
        auto stored = expected.Store(bulk, tiny, data);
        CHECK(!stored && data.attributes.IsEmpty());
    }
    REQUIRE(tiny.Shutdown());
}

// Load 실패 주입: 메시 allocator의 모든 할당 지점에서 실패해도 진단 코드 없이 그 오류이고 메시는
// 그대로다. 마지막에는 성공한다.
TEST(MeshSource_LoadFailures) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource source(env.allocator);
    REQUIRE(BuildRichMesh(source));
    MeshPackage loaded(env.allocator);
    REQUIRE(WriteAndLoad(
        env, 1300, PackageKind::Editor, BulkCodec::None,
        [&](TestMeshAsset& asset, MemoryBulkSource& bulk) {
            return source.Store(bulk, env.names, asset.mesh).IsOk();
        },
        loaded));
    TestMeshAsset* asset = LoadedAsset(env, loaded);
    Package* package = LoadedPackage(env, loaded);
    REQUIRE(asset != nullptr && package != nullptr);
    bool done = false;
    uint64 failures = 0;
    for (uint64 attempt = 1; attempt <= 2048 && !done; ++attempt) {
        test::NFailAllocator failing(env.allocator);
        MeshSource mesh(failing);
        REQUIRE(BuildQuad(mesh));
        failing.FailOnAttempt(attempt);
        MeshDiagnostic diagnostic;
        auto read = mesh.Load(asset->mesh, *package, env.names, &diagnostic);
        failing.DisableFailure();
        if (read) {
            done = true;
            CHECK(SameMesh(mesh, source));
        } else {
            ++failures;
            CHECK(read.Error() == ErrorCode::OutOfMemory);
            CHECK(diagnostic.code == AssetDiagnosticCode::None);
            CHECK(mesh.Count(MeshElement::Vertex) == 4 && mesh.AttributeCount() == 0);
        }
    }

    CHECK(done);
    CHECK(failures > 30);
}

// 할당 크기 상한을 둔 할당기다(가장 큰 요청을 기록하고, limit보다 큰 요청은 실제로 잡지 않고
// OutOfMemory로 돌려준다 — 변조된 요소 수가 bulk와 무관한 큰 할당을 만드는지 결정적으로 본다).
class PeakAllocator final : public Allocator {
public:
    PeakAllocator(Allocator& allocator, usize limit) noexcept
        : allocator_(allocator), limit_(limit) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        peak_ = bytes > peak_ ? bytes : peak_;
        if (bytes > limit_) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        return allocator_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return allocator_.Deallocate(address);
    }

    void TrimMemory() noexcept override {
        allocator_.TrimMemory();
    }

    void MarkThreadIdle() noexcept override {
        allocator_.MarkThreadIdle();
    }

    void MarkThreadActive() noexcept override {
        allocator_.MarkThreadActive();
    }

    usize Peak() const noexcept {
        return peak_;
    }

private:
    Allocator& allocator_;
    usize limit_ = 0;
    usize peak_ = 0;
};

// 변조된 패키지가 크게 쓴 요소 수(3.1절·3.7절 4·12): 요소 종류 여섯 모두 상한 경계(위상 길이·위치
// 길이·Name 값 길이·묶음 상한)에서 CorruptPackage이고, 로드는 bulk 크기에 묶인 할당만 한다(16
// MiB보다 큰 요청이 없다). Position보다 앞서 읽히는 정점 Name 속성과 묶음 Name 속성도 길이 판정이
// 먼저다.
TEST(MeshSource_LoadBoundsElementAllocations) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    constexpr usize limit = usize{16} << 20;
    MeshSource rich(env.allocator);
    REQUIRE(BuildRichMesh(rich));
    // 정점 Name 속성 "Alpha"는 (element, 이름 byte) 순서에서 Position보다 앞이다.
    MeshSource named(env.allocator);
    REQUIRE(BuildQuad(named));
    REQUIRE(named.AddAttribute(MeshElement::Vertex, "Alpha", MeshValueType::Name, 1));
    struct Case {
        const MeshSource* mesh;
        uint32 MeshSourceData::* count;
        uint32 value;
        MeshIssue issue;
    };

    const Case cases[] = {
        {&rich, &MeshSourceData::vertexCount, MeshMaxElements, MeshIssue::AttributeLength},
        {&rich, &MeshSourceData::cornerCount, MeshMaxElements, MeshIssue::TopologyLength},
        {&rich, &MeshSourceData::edgeCount, MeshMaxElements, MeshIssue::TopologyLength},
        {&rich, &MeshSourceData::polygonCount, MeshMaxElements, MeshIssue::TopologyLength},
        {&rich, &MeshSourceData::triangleCount, MeshMaxElements, MeshIssue::TopologyLength},
        {&rich, &MeshSourceData::polygonGroupCount, MeshMaxPolygonGroups,
         MeshIssue::AttributeLength},
        {&rich, &MeshSourceData::polygonGroupCount, MeshMaxPolygonGroups + 1,
         MeshIssue::ElementCountTooLarge},
        {&named, &MeshSourceData::vertexCount, MeshMaxElements, MeshIssue::AttributeLength},
    };
    uint64 id = 900;
    for (const Case& item : cases) {
        PeakAllocator bounded(env.allocator, limit);
        MeshSource out(bounded);
        MeshDiagnostic diagnostic;
        auto loaded = LoadEdited(
            env, id++, *item.mesh,
            [&](MeshSourceData& data, MemoryBulkSource&) {
                data.*item.count = item.value;
                return true;
            },
            out, diagnostic);
        CHECK(!loaded && loaded.Error() == ErrorCode::InvalidArgument);
        CHECK(HasIssue(diagnostic, AssetDiagnosticCode::CorruptPackage, item.issue));
        CHECK(bounded.Peak() <= limit);
    }

    // 묶음 상한은 편집 API도 지킨다(65535개까지 더하고 그다음은 CapacityExceeded).
    MeshSource groups(env.allocator);
    for (uint32 group = 0; group < MeshMaxPolygonGroups; ++group) {
        REQUIRE(groups.AddPolygonGroup());
    }

    auto refused = groups.AddPolygonGroup();
    CHECK(!refused && refused.Error() == ErrorCode::CapacityExceeded &&
          groups.Count(MeshElement::PolygonGroup) == MeshMaxPolygonGroups);
}

} // namespace mesh_source_storage_tests_file
