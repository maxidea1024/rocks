#include "ExchangeTestSupport.h"

#include <AssetRuntime/AssetDiagnostic.h>
#include <MeshSource/MeshSource.h>
#include <MeshSource/MeshSourceValidation.h>
#include <MeshTools/StaticMeshBuilder.h>
#include <TestSupport/Environment.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// UE가 내보낸 외부 묶음을 리더로 읽는 선택 시험이다(Docs/Design/UnrealExchange.ko.md 11.7절 18번 —
// UX1b 왕복 검증). 환경 변수 ROCKS_UNREAL_EXCHANGE_BUNDLE이 묶음 디렉터리를 가리키면 읽고 결과를
// 출력한다. 변수가 없으면 [SKIP]을 출력하고 통과한다(기본 CI는 바뀌지 않는다).
//
// 판정: 읽기와 임포트 순서 계산이 성공하고, 오류 진단이 없으며, 범위 안 클래스 에셋은 모두 Ready,
// 범위 밖은 Unsupported, 실패한 저장 단위가 없다. 정적 메시는 LOD마다 source·prepared·baked
// 기록(5.2절)을 MeshSource로 적재해 ValidateMeshSource를 통과하고 BuildStaticMeshLod(기본 빌드
// 설정)가 성공해야 한다 (UnrealExchange.ko.md 5.4절 — UX3a 왕복 확인, 임포트 연결은 UX3b).
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_external_bundle_tests_file {

constexpr char BundleVariable[] = "ROCKS_UNREAL_EXCHANGE_BUNDLE";

const char* StateName(ExchangeAssetState state) noexcept {
    switch (state) {
    case ExchangeAssetState::Ready:
        return "Ready";
    case ExchangeAssetState::Unsupported:
        return "Unsupported";
    default:
        return "Failed";
    }
}

const char* SeverityName(ExchangeSeverity severity) noexcept {
    switch (severity) {
    case ExchangeSeverity::Error:
        return "error";
    case ExchangeSeverity::Warning:
        return "warning";
    default:
        return "info";
    }
}

// 교환 MeshSource 기록의 요소·값 타입 이름이다(번호 + 1 = MeshElement·MeshValueType 값, 5.4절).
constexpr StringView MeshElementNames[] = {"Vertex",  "Corner",   "Edge",
                                           "Polygon", "Triangle", "PolygonGroup"};
constexpr StringView MeshTypeNames[] = {"Float", "Vec2", "Vec3", "Vec4", "UInt32", "Bool", "Name"};
// 값 타입별 성분 수다(MeshTypeNames와 같은 순서).
constexpr usize MeshTypeComponents[] = {1, 2, 3, 4, 1, 1, 1};
// 위상 배열 수(MeshAnimationAssets 9.1절)와 변·삼각형의 원소당 번호 수다.
constexpr usize MeshTopologyArrays = 7;
constexpr usize EdgeWords = 2;
constexpr usize TriangleWords = 3;
constexpr usize WordBytes = sizeof(uint32);

// printf의 %.*s 인자 길이다.
int PrintLength(StringView text) noexcept {
    return static_cast<int>(text.ByteCount());
}

void PrintDiagnostics(const char* stage, const ExchangeDiagnostics& diagnostics) noexcept {
    for (const ExchangeDiagnostic& item : diagnostics.Items()) {
        const StringView text = item.text.View();
        printf("  %s diagnostic: %s %s/%s asset=%u %.*s\n", stage, SeverityName(item.severity),
               AssetDiagnosticName(item.code), ExchangeIssueName(item.issue), item.asset,
               PrintLength(text), text.Data());
    }
}

// 기록 안 blob 참조 객체와 같은 (hex, 배치, components, count)의 에셋 blob 참조다.
const ExchangeBlobRef* FindBlob(const ExchangeAsset& asset, JsonValue reference) noexcept {
    auto digest = Sha256Digest::ParseHex(reference.Find("blob").GetString());
    auto count = reference.Find("count").TryGetUInt64();
    auto components = reference.Find("components").TryGetUInt64();
    if (!digest || !count || !components) {
        return nullptr;
    }

    for (const ExchangeBlobRef& blob : asset.blobs) {
        if (blob.digest == digest.Value() && blob.count == count.Value() &&
            blob.components == components.Value() &&
            StringView(ExchangeBlobLayoutName(blob.layout)) ==
                reference.Find("layout").GetString()) {
            return &blob;
        }
    }

    return nullptr;
}

bool LoadBlobBytes(const ExchangeBundle& bundle, uint32 asset, JsonValue reference,
                   Array<uint8>& out, ExchangeDiagnostics& diagnostics) noexcept {
    const ExchangeBlobRef* blob = FindBlob(bundle.Assets()[asset], reference);
    return blob != nullptr && LoadExchangeBlob(bundle, asset, *blob, out, diagnostics);
}

uint32 Word(const Array<uint8>& bytes, usize index) noexcept {
    uint32 value = 0;
    memcpy(&value, bytes.Data() + index * WordBytes, WordBytes);
    return value;
}

uint32 CountOf(JsonValue counts, StringView key) noexcept {
    auto value = counts.Find(key).TryGetUInt64();
    return value ? static_cast<uint32>(value.Value()) : 0;
}

usize NameIndex(Span<const StringView> names, StringView name) noexcept {
    for (usize index = 0; index < names.Size(); ++index) {
        if (names[index] == name) {
            return index;
        }
    }

    return names.Size();
}

// 속성 하나의 값을 bulk 배치(MeshAnimationAssets 3.4.1절)에서 채운다.
bool FillAttribute(MeshSource& mesh, uint32 attribute, usize type, uint32 channels, uint32 count,
                   const Array<uint8>& bytes) noexcept {
    const MeshValueType valueType = static_cast<MeshValueType>(type + 1);
    if (valueType == MeshValueType::Name) {
        usize offset = 0;
        for (uint32 channel = 0; channel < channels; ++channel) {
            for (uint32 index = 0; index < count; ++index) {
                if (offset + WordBytes > bytes.Size()) {
                    return false;
                }

                uint32 length = 0;
                memcpy(&length, bytes.Data() + offset, WordBytes);
                offset += WordBytes;
                if (offset + length > bytes.Size()) {
                    return false;
                }

                const StringView text(reinterpret_cast<const char*>(bytes.Data() + offset), length);
                if (!mesh.SetNameValue(attribute, channel, index, text)) {
                    return false;
                }

                offset += length;
            }
        }

        return offset == bytes.Size();
    }

    const usize channelBytes = static_cast<usize>(count) * MeshTypeComponents[type] *
                               (valueType == MeshValueType::Bool ? 1 : WordBytes);
    if (bytes.Size() != channelBytes * channels) {
        return false;
    }

    for (uint32 channel = 0; channel < channels; ++channel) {
        const uint8* source = bytes.Data() + channel * channelBytes;
        if (channelBytes == 0) {
            continue;
        }

        if (valueType == MeshValueType::Bool) {
            memcpy(mesh.BoolValues(attribute, channel).Data(), source, channelBytes);
        } else if (valueType == MeshValueType::UInt32) {
            memcpy(mesh.UInt32Values(attribute, channel).Data(), source, channelBytes);
        } else {
            memcpy(mesh.FloatValues(attribute, channel).Data(), source, channelBytes);
        }
    }

    return true;
}

// 교환 MeshSource 기록 하나를 MeshSource로 만든다(위상 일곱 배열 + 속성, 정점 위치는
// Vertex·Position).
bool LoadExchangeMesh(const ExchangeBundle& bundle, uint32 asset, JsonValue record,
                      Allocator& allocator, MeshSource& mesh,
                      ExchangeDiagnostics& diagnostics) noexcept {
    const JsonValue counts = record.Find("counts");
    const uint32 vertices = CountOf(counts, "vertices");
    const uint32 corners = CountOf(counts, "corners");
    const uint32 edges = CountOf(counts, "edges");
    const uint32 polygons = CountOf(counts, "polygons");
    const uint32 triangles = CountOf(counts, "triangles");
    const uint32 groups = CountOf(counts, "polygonGroups");
    const JsonValue topology = record.Find("topology");
    const JsonValue attributes = record.Find("attributes");
    if (!topology.IsArray() || topology.Size() != MeshTopologyArrays || !attributes.IsArray()) {
        return false;
    }

    Array<Array<uint8>> arrays(allocator);
    for (usize index = 0; index < MeshTopologyArrays; ++index) {
        Array<uint8> bytes(allocator);
        if (!LoadBlobBytes(bundle, asset, topology.At(index), bytes, diagnostics) ||
            !arrays.TryPushBack(Move(bytes))) {
            return false;
        }
    }

    Array<uint8> positions(allocator);
    for (usize index = 0; index < attributes.Size(); ++index) {
        const JsonValue item = attributes.At(index);
        if (item.Find("element").GetString() == StringView("Vertex") &&
            item.Find("name").GetString() == MeshAttributeName::Position &&
            !LoadBlobBytes(bundle, asset, item.Find("data"), positions, diagnostics)) {
            return false;
        }
    }

    if (positions.Size() != static_cast<usize>(vertices) * sizeof(Vec3)) {
        return false;
    }

    for (uint32 vertex = 0; vertex < vertices; ++vertex) {
        Vec3 position;
        memcpy(&position, positions.Data() + vertex * sizeof(Vec3), sizeof(Vec3));
        if (!mesh.AddVertex(position)) {
            return false;
        }
    }

    for (uint32 corner = 0; corner < corners; ++corner) {
        if (!mesh.AddCorner(Word(arrays[0], corner))) {
            return false;
        }
    }

    for (uint32 edge = 0; edge < edges; ++edge) {
        if (!mesh.AddEdge(Word(arrays[1], edge * EdgeWords),
                          Word(arrays[1], edge * EdgeWords + 1))) {
            return false;
        }
    }

    for (uint32 group = 0; group < groups; ++group) {
        if (!mesh.AddPolygonGroup()) {
            return false;
        }
    }

    Array<uint32> perimeter(allocator);
    for (uint32 polygon = 0; polygon < polygons; ++polygon) {
        perimeter.Clear();
        for (uint32 at = Word(arrays[2], polygon); at < Word(arrays[2], polygon + 1); ++at) {
            if (!perimeter.TryPushBack(Word(arrays[3], at))) {
                return false;
            }
        }

        if (!mesh.AddPolygon(perimeter.View(), Word(arrays[4], polygon))) {
            return false;
        }
    }

    for (uint32 triangle = 0; triangle < triangles; ++triangle) {
        const usize first = triangle * TriangleWords;
        if (!mesh.AddTriangle(Word(arrays[5], first), Word(arrays[5], first + 1),
                              Word(arrays[5], first + 2), Word(arrays[6], triangle))) {
            return false;
        }
    }

    const uint32 elementCounts[] = {vertices, corners, edges, polygons, triangles, groups};
    for (usize index = 0; index < attributes.Size(); ++index) {
        const JsonValue item = attributes.At(index);
        const StringView name = item.Find("name").GetString();
        const usize element =
            NameIndex(Span<const StringView>(MeshElementNames), item.Find("element").GetString());
        const usize type =
            NameIndex(Span<const StringView>(MeshTypeNames), item.Find("type").GetString());
        auto channels = item.Find("channels").TryGetUInt64();
        if (element == sizeof(MeshElementNames) / sizeof(MeshElementNames[0]) ||
            type == sizeof(MeshTypeNames) / sizeof(MeshTypeNames[0]) || !channels) {
            return false;
        }

        if (element == 0 && name == MeshAttributeName::Position) {
            continue;
        }

        auto added = mesh.AddAttribute(static_cast<MeshElement>(element + 1), name,
                                       static_cast<MeshValueType>(type + 1),
                                       static_cast<uint8>(channels.Value()));
        Array<uint8> bytes(allocator);
        if (!added || !LoadBlobBytes(bundle, asset, item.Find("data"), bytes, diagnostics) ||
            !FillAttribute(mesh, added.Value(), type, static_cast<uint32>(channels.Value()),
                           elementCounts[element], bytes)) {
            return false;
        }
    }

    return true;
}

// 정적 메시 에셋의 LOD 기록(source·prepared·baked)마다 적재·검증·빌드한다. 확인한 기록 수다.
usize CheckStaticMesh(const ExchangeBundle& bundle, uint32 asset, Allocator& allocator,
                      ExchangeDiagnostics& diagnostics) {
    const JsonValue lods = bundle.Assets()[asset].data.Find("lods");
    CHECK(lods.IsArray());
    if (!lods.IsArray()) {
        return 0;
    }

    constexpr StringView Kinds[] = {"source", "prepared", "baked"};
    usize checked = 0;
    for (usize lod = 0; lod < lods.Size(); ++lod) {
        for (const StringView& kind : Kinds) {
            const JsonValue record = lods.At(lod).Find(kind);
            if (!record.IsObject()) {
                continue;
            }

            MeshSource mesh(allocator);
            const bool loaded =
                LoadExchangeMesh(bundle, asset, record, allocator, mesh, diagnostics);
            MeshDiagnostic failure;
            const bool valid = loaded && static_cast<bool>(ValidateMeshSource(mesh, {}, &failure));
            StaticMeshBuildInput input;
            input.source = &mesh;
            StaticMeshBuildResult built(allocator);
            const bool buildOk =
                valid && static_cast<bool>(BuildStaticMeshLod(input, built, &failure));
            printf(
                "    LOD %zu %.*s: loaded %d, valid %d, built %d (%u vertices, %u triangles -> %u "
                "render vertices, %zu sections, %zu warnings) %s\n",
                lod, PrintLength(kind), kind.Data(), loaded, valid, buildOk,
                mesh.Count(MeshElement::Vertex), mesh.Count(MeshElement::Triangle),
                built.VertexCount(), built.sections.Size(), built.warnings.Size(),
                buildOk ? "" : MeshIssueName(failure.issue));
            CHECK(loaded && valid && buildOk);
            ++checked;
        }
    }

    return checked;
}

TEST(UnrealExchange_ExternalBundle) {
    SystemAllocator allocator;
    StringBuilder<512> directory(allocator);
    if (!test::ReadEnvironmentVariable(BundleVariable, directory)) {
        printf("[SKIP] UnrealExchange_ExternalBundle: %s is not set\n", BundleVariable);
        return;
    }

    ExchangeBundle bundle(allocator);
    ExchangeDiagnostics diagnostics(allocator);
    const Result<void> read = ReadExchangeBundle(platform::PhysicalFileSystem(), directory.View(),
                                                 {}, bundle, diagnostics);
    printf("Bundle %.*s: read %s, %zu assets, %zu redirects\n", PrintLength(directory.View()),
           directory.View().Data(), read ? "ok" : "failed", bundle.Assets().Size(),
           bundle.Redirects().Size());
    PrintDiagnostics("read", diagnostics);
    REQUIRE(read);
    CHECK(diagnostics.Count(ExchangeSeverity::Error) == 0 && diagnostics.Dropped() == 0);
    printf("Exporter %.*s %.*s, engine %.*s (%.*s)\n", PrintLength(bundle.exporter.plugin),
           bundle.exporter.plugin.Data(), PrintLength(bundle.exporter.pluginVersion),
           bundle.exporter.pluginVersion.Data(), PrintLength(bundle.exporter.engineVersion),
           bundle.exporter.engineVersion.Data(), PrintLength(bundle.exporter.engineCommit),
           bundle.exporter.engineCommit.Data());

    for (usize index = 0; index < bundle.Assets().Size(); ++index) {
        const ExchangeAsset& asset = bundle.Assets()[index];
        printf("  [%zu] %s %.*s %.*s (%zu dependencies)\n", index, StateName(asset.state),
               PrintLength(asset.className), asset.className.Data(), PrintLength(asset.objectPath),
               asset.objectPath.Data(), asset.dependencies.Size());
        const bool inScope = asset.exchangeClass != ExchangeClass::Unsupported;
        CHECK(asset.state ==
              (inScope ? ExchangeAssetState::Ready : ExchangeAssetState::Unsupported));
    }

    for (const ExchangeRedirect& redirect : bundle.Redirects()) {
        printf("  redirect %.*s -> %.*s\n", PrintLength(redirect.from), redirect.from.Data(),
               PrintLength(redirect.to), redirect.to.Data());
    }

    ExchangeImportPlan plan(allocator);
    ExchangeDiagnostics orderDiagnostics(allocator);
    const Result<void> planned = PlanExchangeImport(bundle, {}, plan, orderDiagnostics);
    PrintDiagnostics("order", orderDiagnostics);
    REQUIRE(planned);
    CHECK(orderDiagnostics.Count(ExchangeSeverity::Error) == 0);
    usize failed = 0;
    for (const ExchangeImportUnit& unit : plan.Units()) {
        failed += unit.state == ExchangeUnitState::Failed ? 1 : 0;
    }

    printf("Import order: %zu units, %zu failed\n", plan.Units().Size(), failed);
    CHECK(failed == 0);

    usize meshRecords = 0;
    for (usize index = 0; index < bundle.Assets().Size(); ++index) {
        const ExchangeAsset& asset = bundle.Assets()[index];
        if (asset.state == ExchangeAssetState::Ready &&
            asset.exchangeClass == ExchangeClass::StaticMesh) {
            printf("  mesh [%zu] %.*s\n", index, PrintLength(asset.objectPath),
                   asset.objectPath.Data());
            meshRecords +=
                CheckStaticMesh(bundle, static_cast<uint32>(index), allocator, diagnostics);
        }
    }

    printf("Mesh records: %zu checked\n", meshRecords);
}

} // namespace exchange_external_bundle_tests_file
