#include "ExchangeTestSupport.h"

#include <MeshSource/Tests/MeshSourceTestSupport.h>

#include <Animation/ClipSource.h>
#include <Animation/SkeletonMerge.h>
#include <Animation/Tests/AnimationTestSupport.h>
#include <Animation/Tests/ClipTestSupport.h>
#include <AssetRuntime/AssetDiagnostic.h>
#include <MeshSource/MeshSource.h>
#include <MeshSource/MeshSourceValidation.h>
#include <MeshTools/SkinnedMeshBuilder.h>
#include <MeshTools/StaticMeshBuilder.h>
#include <TestSupport/Environment.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// UE가 내보낸 교환 묶음을 리더로 읽어 확인하는 시험이다(Docs/Design/UnrealExchange 11.7절
// 18번, 12.3절).
// - UnrealExchange_ExternalBundle(선택): 환경 변수 ROCKS_UNREAL_EXCHANGE_BUNDLE이 가리키는 묶음.
//   변수가 없으면 [SKIP]을 출력하고 통과한다.
// - UnrealExchange_GoldenBundle(늘 돈다): 저장소 golden 묶음(Tests/Bundles/Golden.rxbundle —
//   RocksExport로 만든 실제 묶음, 만드는 명령은 12.3절)과 그것이 담은 경계 사례.
//
// 판정: 읽기와 임포트 순서 계산이 성공하고, 오류 진단이 없으며, 범위 안 클래스 에셋은 모두 Ready,
// 범위 밖은 Unsupported, 실패한 저장 단위가 없다. 메시 기록(5.2절)은 MA1 저장 형태(MeshSourceData의
// 위상 bulk·속성 레코드 bulk = 교환 blob)로 옮겨 실제 패키지로 쓰고 로드한 뒤 MeshSource::Load(배치
// 판정·ValidateMeshSource)를 통과해야 한다. 정적 메시는 LOD마다 source·prepared·baked 기록이
// BuildStaticMeshLod(기본 빌드 설정)까지 성공해야 한다(5.4절 — UX3a, 임포트 연결은 UX3b).
// 스켈레톤은 MA4a Skeleton으로 채워 ValidateSkeleton을, 스킨 메시는 메시 계층 검증·스켈레톤 병합
// 판정(MergeBoneHierarchy)과 LOD 기록의 적재·검증(본 수)·BuildSkinnedMeshLod를 통과해야
// 한다(6.3절 — UX7a). 클립은 MA6a ClipSource로 적재해
// ValidateClipSource, 키 시간 샘플링, 원본·두 배 비율 BuildClipPlayback과 쿠킹 샘플링을 통과해야
// 한다(7.1절 — UX8a). 머티리얼 계열은 번역(UX4b) 전이라 기록 구조의 불변식만 본다: 노드 이름
// 유일, 연결 대상 노드·출력 번호 존재, mask 형식, 함수 호출·인스턴스 부모가 dependencies에 있음,
// 머티리얼 출력 핀 이름 유일, resolved 항목 형식(8.6절 — UX4a). 모든 에셋의 { "objectPath" }
// 참조는 dependencies 또는 redirect 대상으로 이어져야 한다(11.7절 21번).
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
constexpr StringView MeshTypeNames[] = {"Float", "Vec2", "Vec3",           "Vec4",      "UInt32",
                                        "Bool",  "Name", "BoneInfluences", "SparseVec3"};
// 위상 배열 수다(MeshAnimationAssets 9.1절).
constexpr usize MeshTopologyArrays = 7;
// 이동 리타깃 방식 이름(UE EBoneTranslationRetargetingMode 값 이름 = BoneTranslationRetarget
// 순서)이다.
constexpr StringView RetargetModeNames[] = {"Animation", "Skeleton", "AnimationScaled",
                                            "AnimationRelative", "OrientAndScale"};
// 시험이 만드는 스켈레톤 패키지 id의 시작 값이다.
constexpr uint64 ExternalSkeletonFirstId = 0x7000;
// 시험이 만드는 클립 패키지 id의 시작 값이다(에셋 번호를 더한다).
constexpr uint64 ExternalClipFirstId = 0x9000;
// 메시 기록 저장 왕복 패키지 id의 시작 값이다(기록마다 하나씩 늘린다 — 위 두 범위와 겹치지 않는다).
constexpr uint64 ExternalMeshPackageFirstId = 0x100000;
// golden 묶음(Tests/Bundles/Golden.rxbundle — 12.3절)의 기대 값이다: 에셋 11개(범위 밖 1),
// 정적 메시 기록 4개(정육면체·퇴화 메시의 source·prepared), 스킨 메시 기록 3개와 클립 빌드 2개,
// 머티리얼 기록 2개, 퇴화 메시 원본의 변 5개(UE 변 6개 중 자기 변 1개를 뺐다).
constexpr usize GoldenAssets = 11;
constexpr usize GoldenMeshRecords = 4;
constexpr usize GoldenSkeletalRecords = 5;
constexpr usize GoldenMaterialRecords = 2;
constexpr uint32 GoldenDegenerateEdges = 5;
constexpr StringView GoldenDegeneratePath = "/Game/RxGolden/SM_Degenerate.SM_Degenerate";
constexpr StringView GoldenClipPath =
    "/Game/RxGolden/SK_Column/SkeletalMeshes/SK_Column_Anim.SK_Column_Anim";
constexpr StringView GoldenSoftTargetPath = "/Game/RxGolden/SK_SoftTarget.SK_SoftTarget";
// UE FRichCurve 열거 값 이름(Rocks Curves 열거 순서)이다. RCTM_SmartAuto는 Auto로 본다.
constexpr StringView CurveInterpNames[] = {"RCIM_Linear", "RCIM_Constant", "RCIM_Cubic"};
constexpr StringView CurveTangentNames[] = {"RCTM_Auto", "RCTM_User", "RCTM_Break",
                                            "RCTM_SmartAuto"};
constexpr usize SmartAutoTangent = 3;
constexpr StringView CurveWeightNames[] = {"RCTWM_WeightedNone", "RCTWM_WeightedArrive",
                                           "RCTWM_WeightedLeave", "RCTWM_WeightedBoth"};
constexpr StringView CurveExtrapolationNames[] = {"RCCE_Cycle", "RCCE_CycleWithOffset",
                                                  "RCCE_Oscillate", "RCCE_Linear", "RCCE_Constant"};
// UE 커브 플래그 비트(AnimCurveTypes.h EAnimAssetCurveFlags)다.
constexpr int64 UeCurveEditable = 0x04;
constexpr int64 UeCurveMetadata = 0x10;
constexpr int64 UeCurveDisabled = 0x40;

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

// 교환 MeshSource 기록 하나를 MA1 저장 형태로 옮겨 실제 패키지로 쓰고 다시 로드한 뒤
// MeshSource::Load로 만든다(UnrealExchange 12.3절 — UX3b 임포트가 할 저장과 같은 규칙):
// MeshSourceData의 요소 수는 counts, 위상 bulk 하나는 위상 blob 일곱을 9.1절 순서로 이어 붙인 것,
// 속성 레코드는 기록 순서(9.2절 정렬과 같다)이고 값 bulk는 교환 blob bytes 그대로다(배치가 같다 —
// MeshAnimationAssets 3.4.1절). Load가 배치 판정과 ValidateMeshSource(본 수 모름)를 한다.
// failure는 Load의 진단이다. 패키지 번호는 nextPackage에서 하나 쓴다.
bool LoadExchangeMesh(const ExchangeBundle& bundle, uint32 asset, JsonValue record,
                      asset_test::AssetEnv& env, uint64& nextPackage, MeshSource& mesh,
                      ExchangeDiagnostics& diagnostics, MeshDiagnostic* failure) noexcept {
    const JsonValue counts = record.Find("counts");
    const JsonValue topology = record.Find("topology");
    const JsonValue attributes = record.Find("attributes");
    if (!topology.IsArray() || topology.Size() != MeshTopologyArrays || !attributes.IsArray()) {
        return false;
    }

    mesh_source_test::MeshPackage stored(env.allocator);
    const bool written = mesh_source_test::WriteAndLoad(
        env, nextPackage++, PackageKind::Editor, BulkCodec::None,
        [&](asset_fixture::TestMeshAsset& target, MemoryBulkSource& bulk) {
            MeshSourceData& data = target.mesh;
            data.vertexCount = CountOf(counts, "vertices");
            data.cornerCount = CountOf(counts, "corners");
            data.edgeCount = CountOf(counts, "edges");
            data.polygonCount = CountOf(counts, "polygons");
            data.triangleCount = CountOf(counts, "triangles");
            data.polygonGroupCount = CountOf(counts, "polygonGroups");
            Array<uint8> joined(env.allocator);
            for (usize index = 0; index < MeshTopologyArrays; ++index) {
                Array<uint8> bytes(env.allocator);
                if (!LoadBlobBytes(bundle, asset, topology.At(index), bytes, diagnostics) ||
                    !joined.TryAppend(bytes.View())) {
                    return false;
                }
            }

            auto topologyRef = bulk.Add(Move(joined));
            if (!topologyRef) {
                return false;
            }

            data.topology = topologyRef.Value();
            for (usize index = 0; index < attributes.Size(); ++index) {
                const JsonValue item = attributes.At(index);
                const usize element = NameIndex(Span<const StringView>(MeshElementNames),
                                                item.Find("element").GetString());
                const usize type =
                    NameIndex(Span<const StringView>(MeshTypeNames), item.Find("type").GetString());
                auto channels = item.Find("channels").TryGetUInt64();
                auto name = env.names.TryIntern(item.Find("name").GetString());
                Array<uint8> bytes(env.allocator);
                if (element == sizeof(MeshElementNames) / sizeof(MeshElementNames[0]) ||
                    type == sizeof(MeshTypeNames) / sizeof(MeshTypeNames[0]) || !channels ||
                    !name || !LoadBlobBytes(bundle, asset, item.Find("data"), bytes, diagnostics)) {
                    return false;
                }

                auto valueRef = bulk.Add(Move(bytes));
                MeshAttributeRecord attribute;
                attribute.name = name.Value();
                attribute.element = static_cast<MeshElement>(element + 1);
                attribute.type = static_cast<MeshValueType>(type + 1);
                attribute.channels = static_cast<uint8>(channels.Value());
                if (!valueRef) {
                    return false;
                }

                attribute.data = valueRef.Value();
                if (!data.attributes.TryPushBack(Move(attribute))) {
                    return false;
                }
            }

            return true;
        },
        stored);
    return written && static_cast<bool>(mesh.Load(mesh_source_test::LoadedAsset(env, stored)->mesh,
                                                  *mesh_source_test::LoadedPackage(env, stored),
                                                  env.names, failure));
}

// 정적 메시 에셋의 LOD 기록(source·prepared·baked)마다 적재·검증·빌드한다. 확인한 기록 수다.
usize CheckStaticMesh(const ExchangeBundle& bundle, uint32 asset, asset_test::AssetEnv& env,
                      uint64& nextPackage, ExchangeDiagnostics& diagnostics) {
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

            MeshSource mesh(env.allocator);
            MeshDiagnostic failure;
            const bool loaded = LoadExchangeMesh(bundle, asset, record, env, nextPackage, mesh,
                                                 diagnostics, &failure);
            const bool valid = loaded;
            StaticMeshBuildInput input;
            input.source = &mesh;
            StaticMeshBuildResult built(env.allocator);
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

// 실수 배열 JSON(교환 숫자 — 3.2절)을 읽는다.
template <usize Count>
bool ReadNumbers(JsonValue array, double (&out)[Count]) noexcept {
    if (!array.IsArray() || array.Size() != Count) {
        return false;
    }

    for (usize index = 0; index < Count; ++index) {
        if (!ReadExchangeNumber(array.At(index), out[index])) {
            return false;
        }
    }

    return true;
}

// 교환 본 배열(6.3절 1번)을 BoneHierarchy로 만든다. 회전은 저장 전 정규화 규칙(MA4a 3)을 적용한다.
bool ReadBones(JsonValue bones, NamePool& names, BoneHierarchy& out) noexcept {
    if (!bones.IsArray()) {
        return false;
    }

    for (usize index = 0; index < bones.Size(); ++index) {
        const JsonValue item = bones.At(index);
        const JsonValue pose = item.Find("pose");
        auto name = names.TryIntern(item.Find("name").GetString());
        auto parent = item.Find("parent").TryGetInt64();
        double translation[3] = {};
        double rotation[4] = {};
        double scale[3] = {};
        if (!name || !parent || !ReadNumbers(pose.Find("translation"), translation) ||
            !ReadNumbers(pose.Find("rotation"), rotation) ||
            !ReadNumbers(pose.Find("scale"), scale)) {
            return false;
        }

        BoneInfo bone(out.bones.GetAllocator());
        bone.name = name.Value();
        bone.parentIndex = static_cast<int32>(parent.Value());
        const Transform transform(Quatd(rotation[0], rotation[1], rotation[2], rotation[3]),
                                  Vec3d(translation[0], translation[1], translation[2]),
                                  Vec3d(scale[0], scale[1], scale[2]));
        if (!bone.exportName.TryAssign(item.Find("exportName").GetString()) ||
            !out.bones.TryPushBack(Move(bone)) || !out.referencePose.TryPushBack(transform)) {
            return false;
        }
    }

    NormalizeBoneReferencePose(out);
    return true;
}

// 교환 스켈레톤 data(6.3절 1·2번)로 Skeleton의 계층·리타깃 방식·커브를 채운다. 그 밖의 필드(가상
// 본·소켓 등)는 settings 덤프에 있고 대응은 UX7b다.
bool FillExchangeSkeleton(JsonValue data, NamePool& names, Skeleton& skeleton) noexcept {
    const JsonValue bones = data.Find("bones");
    if (!ReadBones(bones, names, skeleton.bones)) {
        return false;
    }

    for (usize index = 0; index < bones.Size(); ++index) {
        const usize mode = NameIndex(Span<const StringView>(RetargetModeNames),
                                     bones.At(index).Find("retargetMode").GetString());
        if (mode == sizeof(RetargetModeNames) / sizeof(RetargetModeNames[0]) ||
            !skeleton.retargetModes.TryPushBack(static_cast<BoneTranslationRetarget>(mode))) {
            return false;
        }
    }

    const JsonValue curves = data.Find("curves");
    for (usize index = 0; curves.IsArray() && index < curves.Size(); ++index) {
        const JsonValue item = curves.At(index);
        SkeletonCurveInfo curve(skeleton.bones.bones.GetAllocator());
        auto name = names.TryIntern(item.Find("name").GetString());
        auto maxLod = item.Find("maxLod").TryGetUInt64();
        if (!name || !maxLod) {
            return false;
        }

        curve.name = name.Value();
        curve.isMaterial = item.Find("isMaterial").GetBool();
        curve.isMorphTarget = item.Find("isMorphTarget").GetBool();
        curve.maxLod = static_cast<uint8>(maxLod.Value());
        const JsonValue linked = item.Find("linkedBones");
        for (usize at = 0; at < linked.Size(); ++at) {
            auto bone = names.TryIntern(linked.At(at).GetString());
            if (!bone || !curve.linkedBones.TryPushBack(bone.Value())) {
                return false;
            }
        }

        if (!skeleton.curves.TryPushBack(Move(curve))) {
            return false;
        }
    }

    return true;
}

// 묶음 안 스켈레톤 하나다(스킨 메시의 병합 대상).
struct LoadedSkeleton {
    StringView objectPath;
    Skeleton* skeleton = nullptr;
};

// 스킨 메시 하나: 메시 계층 검증, 스켈레톤 병합 판정(MergeBoneHierarchy), LOD 기록(source·baked)의
// 적재·검증 (본 수)·스킨 빌드(BuildSkinnedMeshLod). 확인한 기록 수다.
usize CheckSkeletalMesh(const ExchangeBundle& bundle, uint32 asset,
                        Span<const LoadedSkeleton> skeletons, asset_test::AssetEnv& env,
                        uint64& nextPackage, ExchangeDiagnostics& diagnostics) {
    const ExchangeAsset& entry = bundle.Assets()[asset];
    BoneHierarchy meshBones(env.allocator);
    SkeletonDiagnostic boneFailure;
    const bool bonesOk =
        ReadBones(entry.data.Find("bones"), env.names, meshBones) &&
        static_cast<bool>(ValidateBoneHierarchy(meshBones, env.allocator, &boneFailure));
    CHECK(bonesOk);
    if (!bonesOk) {
        printf("    mesh bones invalid: %s\n", SkeletonIssueName(boneFailure.issue));
        return 0;
    }

    // 묶음 안 스켈레톤 의존(class Skeleton)과 병합 판정한다.
    usize merges = 0;
    for (const ExchangeDependency& dependency : entry.dependencies) {
        for (const LoadedSkeleton& loaded : skeletons) {
            if (dependency.target == ExchangeNoIndex ||
                !(bundle.Assets()[dependency.target].objectPath == loaded.objectPath)) {
                continue;
            }

            BoneMergeResult merged(env.allocator);
            SkeletonDiagnostic failure;
            const bool mergeOk = static_cast<bool>(
                MergeBoneHierarchy(loaded.skeleton->bones, meshBones, merged, &failure));
            printf("    merge into %.*s: %d (%zu bones -> %zu) %s\n",
                   PrintLength(loaded.objectPath), loaded.objectPath.Data(), mergeOk,
                   meshBones.bones.Size(), merged.bones.bones.Size(),
                   mergeOk ? "" : SkeletonIssueName(failure.issue));
            CHECK(mergeOk);
            ++merges;
        }
    }

    CHECK(merges == 1);
    const JsonValue lods = entry.data.Find("lods");
    constexpr StringView Kinds[] = {"source", "baked"};
    usize checked = 0;
    for (usize lod = 0; lods.IsArray() && lod < lods.Size(); ++lod) {
        for (const StringView& kind : Kinds) {
            const JsonValue record = lods.At(lod).Find(kind);
            if (!record.IsObject()) {
                continue;
            }

            MeshSource mesh(env.allocator);
            MeshDiagnostic failure;
            const bool loaded = LoadExchangeMesh(bundle, asset, record, env, nextPackage, mesh,
                                                 diagnostics, &failure);
            MeshValidateOptions options;
            options.boneCount = static_cast<uint32>(meshBones.bones.Size());
            const bool valid =
                loaded && static_cast<bool>(ValidateMeshSource(mesh, options, &failure));
            SkinnedMeshBuildInput input;
            input.source = &mesh;
            input.bones = &meshBones;
            SkinnedMeshBuildResult built(env.allocator);
            const bool buildOk =
                valid && static_cast<bool>(BuildSkinnedMeshLod(input, built, &failure));
            printf(
                "    LOD %zu %.*s: loaded %d, valid %d, built %d (%u vertices, %u triangles -> %u "
                "render vertices, %u influences/vertex, %zu bone map, %zu morph targets, %zu "
                "warnings) "
                "%s\n",
                lod, PrintLength(kind), kind.Data(), loaded, valid, buildOk,
                mesh.Count(MeshElement::Vertex), mesh.Count(MeshElement::Triangle),
                built.mesh.VertexCount(), built.influencesPerVertex, built.boneMap.Size(),
                built.morphTargets.Size(), built.mesh.warnings.Size(),
                buildOk ? "" : MeshIssueName(failure.issue));
            CHECK(loaded && valid && buildOk);
            ++checked;
        }
    }

    return checked;
}

// 유한한 float인가다(NaN·±Inf의 차는 NaN이다).
bool FiniteFloat(float value) noexcept {
    return value - value == 0.0f;
}

// 교환 FRichCurve 객체(7.1절)를 TangentCurve로 옮긴다. UE 열거 값 이름을 Rocks 값으로
// 바꾸고(RCTM_SmartAuto는 Auto), DefaultValue가 UE "없음" 표지(float 최대)면 비운다.
bool ReadTangentCurve(JsonValue object, Allocator& allocator, TangentCurve& out) noexcept {
    const JsonValue keys = object.Find("keys");
    if (!keys.IsArray()) {
        return false;
    }

    Array<TangentCurveKey> converted(allocator);
    for (usize index = 0; index < keys.Size(); ++index) {
        const JsonValue item = keys.At(index);
        TangentCurveKey key;
        constexpr const char* Fields[] = {"time",          "value",
                                          "arriveTangent", "arriveTangentWeight",
                                          "leaveTangent",  "leaveTangentWeight"};
        double numbers[sizeof(Fields) / sizeof(Fields[0])] = {};
        for (usize at = 0; at < sizeof(Fields) / sizeof(Fields[0]); ++at) {
            if (!ReadExchangeNumber(item.Find(Fields[at]), numbers[at])) {
                return false;
            }
        }

        const usize interp = NameIndex(Span<const StringView>(CurveInterpNames),
                                       item.Find("interpMode").GetString());
        usize tangent = NameIndex(Span<const StringView>(CurveTangentNames),
                                  item.Find("tangentMode").GetString());
        tangent = tangent == SmartAutoTangent ? 0 : tangent;
        const usize weight = NameIndex(Span<const StringView>(CurveWeightNames),
                                       item.Find("tangentWeightMode").GetString());
        if (interp >= sizeof(CurveInterpNames) / sizeof(CurveInterpNames[0]) ||
            tangent >= SmartAutoTangent ||
            weight >= sizeof(CurveWeightNames) / sizeof(CurveWeightNames[0])) {
            return false;
        }

        key.time = static_cast<float>(numbers[0]);
        key.value = static_cast<float>(numbers[1]);
        key.arriveTangent = static_cast<float>(numbers[2]);
        key.arriveTangentWeight = static_cast<float>(numbers[3]);
        key.leaveTangent = static_cast<float>(numbers[4]);
        key.leaveTangentWeight = static_cast<float>(numbers[5]);
        key.interp = static_cast<CurveInterp>(interp);
        key.tangentMode = static_cast<CurveTangentMode>(tangent);
        key.tangentWeight = static_cast<CurveTangentWeight>(weight);
        if (!converted.TryPushBack(key)) {
            return false;
        }
    }

    const usize pre = NameIndex(Span<const StringView>(CurveExtrapolationNames),
                                object.Find("preInfinityExtrap").GetString());
    const usize post = NameIndex(Span<const StringView>(CurveExtrapolationNames),
                                 object.Find("postInfinityExtrap").GetString());
    double defaultValue = 0.0;
    if (pre >= sizeof(CurveExtrapolationNames) / sizeof(CurveExtrapolationNames[0]) ||
        post >= sizeof(CurveExtrapolationNames) / sizeof(CurveExtrapolationNames[0]) ||
        !ReadExchangeNumber(object.Find("defaultValue"), defaultValue) ||
        !out.TrySetKeys(converted.View())) {
        return false;
    }

    out.preExtrapolation = static_cast<CurveExtrapolation>(pre);
    out.postExtrapolation = static_cast<CurveExtrapolation>(post);
    if (static_cast<float>(defaultValue) != NumericLimits<float>::Max()) {
        out.defaultValue = static_cast<float>(defaultValue);
    }

    return true;
}

// 성분 blob 하나(f32 × components, 키 1개 또는 키 수)를 읽는다.
template <typename T>
bool ReadClipComponent(const ExchangeBundle& bundle, uint32 asset, JsonValue reference,
                       Allocator& allocator, ExchangeDiagnostics& diagnostics,
                       Array<T>& out) noexcept {
    Array<uint8> bytes(allocator);
    if (!LoadBlobBytes(bundle, asset, reference, bytes, diagnostics) ||
        bytes.Size() % sizeof(T) != 0) {
        return false;
    }

    for (usize at = 0; at < bytes.Size(); at += sizeof(T)) {
        T value;
        memcpy(&value, bytes.Data() + at, sizeof(T));
        if (!out.TryPushBack(value)) {
            return false;
        }
    }

    return true;
}

// 클립 하나(7.4절 — UX8a): 원본 본 트랙을 ClipSource로 적재해 ValidateClipSource, 키 시간 샘플링이
// 키 값과 비트로 같은지, 커브를 ClipCurve(TangentCurve)로 옮겨 원본 비율·두 배 비율
// BuildClipPlayback과 쿠킹 샘플링을 본다.
usize CheckClip(const ExchangeBundle& bundle, uint32 asset, Span<const LoadedSkeleton> skeletons,
                asset_test::AssetEnv& env, ExchangeDiagnostics& diagnostics) {
    const ExchangeAsset& entry = bundle.Assets()[asset];
    const JsonValue data = entry.data;
    const Skeleton* skeleton = nullptr;
    for (const ExchangeDependency& dependency : entry.dependencies) {
        for (const LoadedSkeleton& loaded : skeletons) {
            if (dependency.target != ExchangeNoIndex &&
                bundle.Assets()[dependency.target].objectPath == loaded.objectPath) {
                skeleton = loaded.skeleton;
            }
        }
    }

    CHECK(skeleton != nullptr);
    auto numerator = data.Find("frameRate").Find("numerator").TryGetInt64();
    auto denominator = data.Find("frameRate").Find("denominator").TryGetInt64();
    auto frames = data.Find("frameCount").TryGetUInt64();
    auto rate = numerator && denominator
                    ? FrameRate::TryCreate(static_cast<int32>(numerator.Value()),
                                           static_cast<int32>(denominator.Value()))
                    : Result<FrameRate>::Failure(ErrorCode::InvalidArgument);
    if (skeleton == nullptr || !rate || !frames) {
        CHECK(rate && frames);
        return 0;
    }

    ClipSource source(env.allocator);
    source.frameRate = rate.Value();
    source.frameCount = static_cast<uint32>(frames.Value());
    const JsonValue tracks = data.Find("boneTracks");
    bool loaded = tracks.IsArray();
    for (usize index = 0; loaded && index < tracks.Size(); ++index) {
        const JsonValue item = tracks.At(index);
        ClipTrack track(env.allocator);
        auto bone = env.names.TryIntern(item.Find("bone").GetString());
        loaded = bone &&
                 ReadClipComponent(bundle, asset, item.Find("translation"), env.allocator,
                                   diagnostics, track.translation) &&
                 ReadClipComponent(bundle, asset, item.Find("rotation"), env.allocator, diagnostics,
                                   track.rotation) &&
                 ReadClipComponent(bundle, asset, item.Find("scale"), env.allocator, diagnostics,
                                   track.scale);
        if (loaded) {
            track.bone = bone.Value();
            loaded = static_cast<bool>(source.tracks.TryPushBack(Move(track)));
        }
    }

    ClipDiagnostic failure;
    const bool valid =
        loaded && static_cast<bool>(ValidateClipSource(source, env.allocator, &failure));
    CHECK(valid);
    if (!valid) {
        printf("    clip invalid: %s\n", loaded ? ClipIssueName(failure.issue) : "load failed");
        return 0;
    }

    // 키 시간의 샘플링은 그 키 값이다(MA6a 7.5절 8 — subframe 0).
    usize mismatches = 0;
    for (uint32 track = 0; track < source.tracks.Size(); ++track) {
        const ClipTrack& keys = source.tracks[track];
        for (uint32 key = 0; key < source.KeyCount(); ++key) {
            const double seconds = static_cast<double>(key) * source.frameRate.Denominator() /
                                   source.frameRate.Numerator();
            const Transform3f sampled =
                SampleClipSource(source, track, seconds, ClipInterpolation::Linear);
            const Vec3& translation = keys.translation[keys.translation.Size() == 1 ? 0 : key];
            const Quat& rotation = keys.rotation[keys.rotation.Size() == 1 ? 0 : key];
            const Vec3& scale = keys.scale[keys.scale.Size() == 1 ? 0 : key];
            mismatches += memcmp(&sampled.translation, &translation, sizeof(Vec3)) != 0 ||
                                  memcmp(&sampled.rotation, &rotation, sizeof(Quat)) != 0 ||
                                  memcmp(&sampled.scale, &scale, sizeof(Vec3)) != 0
                              ? 1
                              : 0;
        }
    }

    CHECK(mismatches == 0);
    Array<ClipCurve> curves(env.allocator);
    const JsonValue curveList = data.Find("curves");
    bool curvesOk = curveList.IsArray();
    for (usize index = 0; curvesOk && index < curveList.Size(); ++index) {
        const JsonValue item = curveList.At(index);
        ClipCurve curve(env.allocator);
        auto name = env.names.TryIntern(item.Find("name").GetString());
        auto flags = item.Find("flags").TryGetInt64();
        curvesOk =
            name && flags && ReadTangentCurve(item.Find("curve"), env.allocator, curve.curve);
        if (curvesOk) {
            curve.name = name.Value();
            curve.editable = (flags.Value() & UeCurveEditable) != 0;
            curve.metadata = (flags.Value() & UeCurveMetadata) != 0;
            curve.disabled = (flags.Value() & UeCurveDisabled) != 0;
            curvesOk = static_cast<bool>(curves.TryPushBack(Move(curve)));
        }
    }

    CHECK(curvesOk);

    // 이벤트·이벤트 트랙·동기 마커를 AnimationClip에 담아 정렬(SortClipEvents·SortClipSyncMarkers
    // — 7.5절 11) 뒤 ValidateAnimationClip을 본다. 커브는 그 클립 원본 커브로 옮긴다.
    ObjectHandle package{};
    AnimationClip* clip = asset_test::Pointer<AnimationClip>(
        *env.source,
        clip_test::MakeClip(env, ExternalClipFirstId + asset, PackageKind::Editor, package));
    bool clipOk = clip != nullptr;
    if (clipOk) {
        clip->source.frameRate = source.frameRate;
        clip->source.frameCount = source.frameCount;
        for (ClipCurve& curve : curves) {
            clipOk = clipOk && static_cast<bool>(clip->source.curves.TryPushBack(Move(curve)));
        }

        const JsonValue eventTracks = data.Find("eventTracks");
        for (usize index = 0; clipOk && index < eventTracks.Size(); ++index) {
            auto name = env.names.TryIntern(eventTracks.At(index).GetString());
            clipOk = name && clip->eventTracks.TryPushBack(name.Value());
        }

        const JsonValue events = data.Find("events");
        for (usize index = 0; clipOk && index < events.Size(); ++index) {
            const JsonValue item = events.At(index);
            ClipEvent event;
            double time = 0.0;
            double duration = 0.0;
            double chance = 0.0;
            auto track = item.Find("track").TryGetInt64();
            auto name = env.names.TryIntern(item.Find("name").GetString());
            auto type = item.Find("type").IsString()
                            ? env.names.TryIntern(item.Find("type").GetString())
                            : Result<Name>::Success(Name{});
            clipOk = track && name && type && ReadExchangeNumber(item.Find("time"), time) &&
                     ReadExchangeNumber(item.Find("duration"), duration) &&
                     ReadExchangeNumber(item.Find("triggerChance"), chance);
            if (clipOk) {
                event.time = static_cast<float>(time);
                event.duration = static_cast<float>(duration);
                event.track = static_cast<int32>(track.Value());
                event.name = name.Value();
                event.type = type.Value();
                event.triggerChance = static_cast<float>(chance);
                event.triggerOnDedicatedServer = item.Find("triggerOnDedicatedServer").GetBool();
                event.triggerOnFollower = item.Find("triggerOnFollower").GetBool();
                clipOk = static_cast<bool>(clip->events.TryPushBack(event));
            }
        }

        const JsonValue markers = data.Find("syncMarkers");
        for (usize index = 0; clipOk && index < markers.Size(); ++index) {
            const JsonValue item = markers.At(index);
            ClipSyncMarker marker;
            double time = 0.0;
            auto track = item.Find("track").TryGetInt64();
            auto name = env.names.TryIntern(item.Find("name").GetString());
            clipOk = track && name && ReadExchangeNumber(item.Find("time"), time);
            if (clipOk) {
                marker.name = name.Value();
                marker.time = static_cast<float>(time);
                marker.track = static_cast<int32>(track.Value());
                clipOk = static_cast<bool>(clip->syncMarkers.TryPushBack(marker));
            }
        }
    }

    if (clipOk) {
        SortClipEvents(*clip);
        SortClipSyncMarkers(*clip);
        clipOk = static_cast<bool>(ValidateAnimationClip(*clip, env.allocator, &failure));
    }

    printf(
        "    clip asset: %d (%zu events, %zu event tracks, %zu sync markers) %s\n", clipOk,
        clip != nullptr ? clip->events.Size() : 0, clip != nullptr ? clip->eventTracks.Size() : 0,
        clip != nullptr ? clip->syncMarkers.Size() : 0, clipOk ? "" : ClipIssueName(failure.issue));
    CHECK(clipOk);
    if (clip == nullptr) {
        return 0;
    }

    usize builds = 0;
    const int32 rateFactors[] = {1, 2};
    for (const int32 factor : rateFactors) {
        ClipBuildInput input;
        input.source = &source;
        input.curves = clip->source.curves.View();
        input.skeleton = skeleton;
        input.targetRate = FrameRate::TryCreate(source.frameRate.Numerator() * factor,
                                                source.frameRate.Denominator())
                               .Value();
        ClipPlayback cooked(env.allocator);
        ClipBuildReport report(env.allocator);
        const bool built = static_cast<bool>(BuildClipPlayback(input, cooked, report, &failure));
        bool finite = built;
        for (uint32 track = 0; built && track < cooked.tracks.Size(); ++track) {
            const Transform3f mid =
                SampleClipPlayback(cooked, track, cooked.Length() * 0.5, ClipInterpolation::Linear);
            finite = finite && FiniteFloat(mid.translation.x) && FiniteFloat(mid.rotation.w) &&
                     FiniteFloat(mid.scale.z);
        }

        printf("    cook x%d: built %d (%u samples, %zu tracks, %zu curves, %zu unknown bone "
               "tracks) sampling %s %s\n",
               factor, built, cooked.sampleCount, cooked.tracks.Size(), cooked.curves.Size(),
               report.warnings.Size(), finite ? "finite" : "non-finite",
               built ? "" : ClipIssueName(failure.issue));
        CHECK(built && finite);
        builds += built ? 1 : 0;
    }

    printf("    clip: %u frames at %d/%d, %zu tracks, %zu curves, %zu key mismatches\n",
           source.frameCount, source.frameRate.Numerator(), source.frameRate.Denominator(),
           source.tracks.Size(), clip->source.curves.Size(), mismatches);
    return builds;
}

// 스켈레톤·스킨 메시를 확인한다(6.3절 7번). 둘 다 없으면 아무것도 하지 않는다. 확인한 기록 수다.
usize CheckSkeletalAssets(const ExchangeBundle& bundle, asset_test::AssetEnv& env,
                          uint64& nextPackage, ExchangeDiagnostics& diagnostics) {
    bool any = false;
    for (const ExchangeAsset& asset : bundle.Assets()) {
        any = any || (asset.state == ExchangeAssetState::Ready &&
                      (asset.exchangeClass == ExchangeClass::Skeleton ||
                       asset.exchangeClass == ExchangeClass::SkeletalMesh ||
                       asset.exchangeClass == ExchangeClass::AnimSequence));
    }

    if (!any) {
        return 0;
    }

    Array<LoadedSkeleton> skeletons(env.allocator);
    uint64 nextId = ExternalSkeletonFirstId;
    for (usize index = 0; index < bundle.Assets().Size(); ++index) {
        const ExchangeAsset& asset = bundle.Assets()[index];
        if (asset.state != ExchangeAssetState::Ready ||
            asset.exchangeClass != ExchangeClass::Skeleton) {
            continue;
        }

        Skeleton* skeleton =
            asset_test::Pointer<Skeleton>(*env.source, animation_test::MakeSkeleton(env, nextId++));
        SkeletonDiagnostic failure;
        const bool filled =
            skeleton != nullptr && FillExchangeSkeleton(asset.data, env.names, *skeleton);
        const bool valid = filled && static_cast<bool>(ValidateSkeleton(*skeleton, env.names,
                                                                        env.allocator, &failure));
        printf("  skeleton [%zu] %.*s: filled %d, valid %d (%zu bones, %zu curves) %s\n", index,
               PrintLength(asset.objectPath), asset.objectPath.Data(), filled, valid,
               filled ? skeleton->bones.bones.Size() : 0, filled ? skeleton->curves.Size() : 0,
               valid ? "" : SkeletonIssueName(failure.issue));
        CHECK(valid);
        if (valid && !skeletons.TryPushBack(LoadedSkeleton{asset.objectPath, skeleton})) {
            CHECK(false);
            return 0;
        }
    }

    usize checked = 0;
    for (usize index = 0; index < bundle.Assets().Size(); ++index) {
        const ExchangeAsset& asset = bundle.Assets()[index];
        if (asset.state == ExchangeAssetState::Ready &&
            asset.exchangeClass == ExchangeClass::SkeletalMesh) {
            printf("  skinned mesh [%zu] %.*s\n", index, PrintLength(asset.objectPath),
                   asset.objectPath.Data());
            checked += CheckSkeletalMesh(bundle, static_cast<uint32>(index), skeletons.View(), env,
                                         nextPackage, diagnostics);
        }
    }

    for (usize index = 0; index < bundle.Assets().Size(); ++index) {
        const ExchangeAsset& asset = bundle.Assets()[index];
        if (asset.state == ExchangeAssetState::Ready &&
            asset.exchangeClass == ExchangeClass::AnimSequence) {
            printf("  clip [%zu] %.*s\n", index, PrintLength(asset.objectPath),
                   asset.objectPath.Data());
            checked +=
                CheckClip(bundle, static_cast<uint32>(index), skeletons.View(), env, diagnostics);
        }
    }

    return checked;
}

// 머티리얼 기록 하나의 확인 결과다. issue는 처음 어긋난 불변식이다(통과하면 nullptr).
struct MaterialCheck {
    usize nodes = 0;
    usize links = 0;
    const char* issue = nullptr;

    void Fail(const char* what) noexcept {
        if (issue == nullptr) {
            issue = what;
        }
    }
};

bool HasDependency(const ExchangeAsset& asset, StringView objectPath) noexcept {
    for (const ExchangeDependency& dependency : asset.dependencies) {
        if (dependency.objectPath == objectPath) {
            return true;
        }
    }

    return false;
}

// value 아래 { "objectPath" } 참조(11.5절)가 모두 dependencies에 있는지 본다. 본문에 저장된 옛
// 경로(UE redirector)는 bundle.json redirects로 이은 대상이 dependencies에 있으면 되고(11.7절
// 21번), 자기 자신 참조(하위 객체가 에셋을 가리키는 경우)는 의존에 넣지 않으므로 받는다(11.7절
// 4번).
void CheckReferences(const ExchangeBundle& bundle, const ExchangeAsset& asset, JsonValue value,
                     MaterialCheck& check) noexcept {
    if (value.IsArray()) {
        for (usize index = 0; index < value.Size(); ++index) {
            CheckReferences(bundle, asset, value.At(index), check);
        }

        return;
    }

    if (!value.IsObject()) {
        return;
    }

    const JsonValue path = value.Find("objectPath");
    if (value.Size() == 1 && path.IsString() && !(path.GetString() == asset.objectPath) &&
        !HasDependency(asset, path.GetString()) &&
        !HasDependency(asset, bundle.ResolveRedirect(path.GetString()))) {
        check.Fail("reference missing from dependencies");
    }

    for (usize index = 0; index < value.Size(); ++index) {
        CheckReferences(bundle, asset, value.ValueAt(index), check);
    }
}

// mask는 null 또는 0·1 넷이다.
bool IsMask(JsonValue mask) noexcept {
    if (mask.IsNull()) {
        return true;
    }

    if (!mask.IsArray() || mask.Size() != 4) {
        return false;
    }

    for (usize index = 0; index < mask.Size(); ++index) {
        auto bit = mask.At(index).TryGetUInt64();
        if (!bit || bit.Value() > 1) {
            return false;
        }
    }

    return true;
}

// 이름이 name인 노드다. 없으면 무효 값이다.
JsonValue FindNode(JsonValue nodes, StringView name) noexcept {
    for (usize index = 0; index < nodes.Size(); ++index) {
        const JsonValue node = nodes.At(index);
        if (node.Find("name").IsString() && node.Find("name").GetString() == name) {
            return node;
        }
    }

    return {};
}

// 연결(from)은 null 또는 { node: 있는 노드 이름, output: 그 노드 outputs 범위 안 번호 }다.
void CheckLink(JsonValue nodes, JsonValue from, MaterialCheck& check) noexcept {
    if (from.IsNull()) {
        return;
    }

    const JsonValue name = from.Find("node");
    auto output = from.Find("output").TryGetUInt64();
    const JsonValue target = name.IsString() ? FindNode(nodes, name.GetString()) : JsonValue();
    if (!target.IsValid() || !output || output.Value() >= target.Find("outputs").Size()) {
        check.Fail("link to a missing node or output");
        return;
    }

    ++check.links;
}

// 그래프 노드 배열(8.1절, 8.6절)을 확인한다.
void CheckGraph(const ExchangeAsset& asset, JsonValue nodes, MaterialCheck& check) noexcept {
    if (!nodes.IsArray()) {
        check.Fail("nodes is not an array");
        return;
    }

    check.nodes = nodes.Size();
    for (usize index = 0; index < nodes.Size(); ++index) {
        const JsonValue node = nodes.At(index);
        const JsonValue name = node.Find("name");
        const JsonValue position = node.Find("position");
        const JsonValue inputs = node.Find("inputs");
        const JsonValue outputs = node.Find("outputs");
        if (!name.IsString() || name.GetString().IsEmpty() || !node.Find("guid").IsString() ||
            !node.Find("class").IsString() || !position.IsArray() || position.Size() != 2 ||
            !inputs.IsArray() || !outputs.IsArray() || !node.Find("properties").IsObject()) {
            check.Fail("node shape");
            continue;
        }

        for (usize other = 0; other < index; ++other) {
            if (nodes.At(other).Find("name").IsString() &&
                nodes.At(other).Find("name").GetString() == name.GetString()) {
                check.Fail("duplicate node name");
            }
        }

        for (usize slot = 0; slot < inputs.Size(); ++slot) {
            const JsonValue input = inputs.At(slot);
            if (!input.Find("name").IsString() || !IsMask(input.Find("mask"))) {
                check.Fail("input shape");
            }

            CheckLink(nodes, input.Find("from"), check);
        }

        for (usize slot = 0; slot < outputs.Size(); ++slot) {
            if (!outputs.At(slot).Find("name").IsString() ||
                !IsMask(outputs.At(slot).Find("mask"))) {
                check.Fail("output shape");
            }
        }

        // 함수 호출은 함수를 dependencies로 가져야 한다(번역이 함수 그래프를 펼친다).
        const JsonValue function = node.Find("properties").Find("MaterialFunction");
        if (node.Find("class").GetString() == "MaterialExpressionMaterialFunctionCall" &&
            !function.IsNull() &&
            !(function.Find("objectPath").IsString() &&
              HasDependency(asset, function.Find("objectPath").GetString()))) {
            check.Fail("function call without dependency");
        }
    }
}

// 머티리얼 출력 핀 배열을 확인한다(이름 유일, 연결 또는 상수 키).
void CheckMaterialOutputs(JsonValue nodes, JsonValue pins, MaterialCheck& check) noexcept {
    if (!pins.IsArray() || pins.Size() == 0) {
        check.Fail("material outputs missing");
        return;
    }

    for (usize index = 0; index < pins.Size(); ++index) {
        const JsonValue pin = pins.At(index);
        const JsonValue name = pin.Find("name");
        if (!name.IsString() || !IsMask(pin.Find("mask")) || !pin.Find("constant").IsValid()) {
            check.Fail("material output shape");
            continue;
        }

        for (usize other = 0; other < index; ++other) {
            if (pins.At(other).Find("name").IsString() &&
                pins.At(other).Find("name").GetString() == name.GetString()) {
                check.Fail("duplicate material output");
            }
        }

        CheckLink(nodes, pin.Find("from"), check);
    }
}

// 인스턴스의 부모와 resolved 항목(8.2절)을 확인한다.
void CheckInstance(const ExchangeAsset& asset, MaterialCheck& check) noexcept {
    const JsonValue parent = asset.settings.Find("Parent");
    if (!parent.IsValid() ||
        (!parent.IsNull() && !(parent.Find("objectPath").IsString() &&
                               HasDependency(asset, parent.Find("objectPath").GetString())))) {
        check.Fail("parent missing from dependencies");
    }

    const JsonValue resolved = asset.data.Find("resolved");
    if (!resolved.IsArray()) {
        check.Fail("resolved is not an array");
        return;
    }

    for (usize index = 0; index < resolved.Size(); ++index) {
        const JsonValue entry = resolved.At(index);
        const JsonValue name = entry.Find("parameter").Find("Name");
        if (!name.IsString() || name.GetString().IsEmpty() || !entry.Find("type").IsString() ||
            !entry.Find("value").IsValid()) {
            check.Fail("resolved entry shape");
        }
    }
}

// 머티리얼 계열 Ready 기록을 확인한다(8.6절 — UX4a). 확인한 기록 수다.
usize CheckMaterials(const ExchangeBundle& bundle) noexcept {
    usize checked = 0;
    for (usize index = 0; index < bundle.Assets().Size(); ++index) {
        const ExchangeAsset& asset = bundle.Assets()[index];
        const ExchangeClass type = asset.exchangeClass;
        if (asset.state != ExchangeAssetState::Ready ||
            (type != ExchangeClass::Material && type != ExchangeClass::MaterialFunction &&
             type != ExchangeClass::MaterialInstanceConstant &&
             type != ExchangeClass::MaterialFunctionInstance &&
             type != ExchangeClass::MaterialParameterCollection)) {
            continue;
        }

        MaterialCheck check;
        const JsonValue nodes = asset.data.Find("nodes");
        if (type == ExchangeClass::Material || type == ExchangeClass::MaterialFunction) {
            CheckGraph(asset, nodes, check);
        }

        if (type == ExchangeClass::Material) {
            CheckMaterialOutputs(nodes, asset.data.Find("outputs"), check);
        }

        if (type == ExchangeClass::MaterialInstanceConstant) {
            CheckInstance(asset, check);
        }

        printf("  material [%zu] %s %.*s: %zu nodes, %zu links %s\n", index,
               ExchangeClassName(type), PrintLength(asset.objectPath), asset.objectPath.Data(),
               check.nodes, check.links, check.issue != nullptr ? check.issue : "");
        CHECK(check.issue == nullptr);
        ++checked;
    }

    return checked;
}

// 모든 Ready 에셋의 settings·data { "objectPath" } 참조가 dependencies(또는 redirect 대상)에 있는지
// 본다(11.5절 — 덤프 참조는 의존에 합쳐진다). 확인한 에셋 수다.
usize CheckAllReferences(const ExchangeBundle& bundle) noexcept {
    usize checked = 0;
    for (usize index = 0; index < bundle.Assets().Size(); ++index) {
        const ExchangeAsset& asset = bundle.Assets()[index];
        if (asset.state != ExchangeAssetState::Ready) {
            continue;
        }

        MaterialCheck check;
        CheckReferences(bundle, asset, asset.settings, check);
        CheckReferences(bundle, asset, asset.data, check);
        if (check.issue != nullptr) {
            printf("  references [%zu] %.*s: %s\n", index, PrintLength(asset.objectPath),
                   asset.objectPath.Data(), check.issue);
        }

        CHECK(check.issue == nullptr);
        ++checked;
    }

    return checked;
}

// 묶음 하나를 확인한 결과(기록 수)다.
struct BundleTotals {
    usize assets = 0;
    usize redirects = 0;
    usize meshRecords = 0;
    usize skeletalRecords = 0;
    usize materialRecords = 0;
    usize referenceAssets = 0;
};

// directory의 묶음을 읽어 위 판정을 모두 돌린다(UnrealExchange 11.7절 18번, 12.3절). 읽기에
// 실패하면 bundle은 비어 있고 totals는 0이다.
BundleTotals CheckBundle(StringView directory, Allocator& allocator, ExchangeBundle& bundle) {
    ExchangeDiagnostics diagnostics(allocator);
    const Result<void> read =
        ReadExchangeBundle(platform::PhysicalFileSystem(), directory, {}, bundle, diagnostics);
    printf("Bundle %.*s: read %s, %zu assets, %zu redirects\n", PrintLength(directory),
           directory.Data(), read ? "ok" : "failed", bundle.Assets().Size(),
           bundle.Redirects().Size());
    PrintDiagnostics("read", diagnostics);
    CHECK(read);
    BundleTotals totals;
    if (!read) {
        return totals;
    }

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
    CHECK(planned);
    CHECK(orderDiagnostics.Count(ExchangeSeverity::Error) == 0);
    usize failed = 0;
    for (const ExchangeImportUnit& unit : plan.Units()) {
        failed += unit.state == ExchangeUnitState::Failed ? 1 : 0;
    }

    printf("Import order: %zu units, %zu failed\n", plan.Units().Size(), failed);
    CHECK(failed == 0);

    // 메시 기록은 실제 패키지로 저장했다가 Load한다(LoadExchangeMesh). 애니메이션 타입은
    // 스켈레톤·클립 확인이 쓴다.
    asset_test::AssetEnv env;
    const bool ready = env.Ready() && static_cast<bool>(RegisterAnimationTypes(env.modules));
    CHECK(ready);
    if (!ready) {
        return totals;
    }

    uint64 nextPackage = ExternalMeshPackageFirstId;
    for (usize index = 0; index < bundle.Assets().Size(); ++index) {
        const ExchangeAsset& asset = bundle.Assets()[index];
        if (asset.state == ExchangeAssetState::Ready &&
            asset.exchangeClass == ExchangeClass::StaticMesh) {
            printf("  mesh [%zu] %.*s\n", index, PrintLength(asset.objectPath),
                   asset.objectPath.Data());
            totals.meshRecords +=
                CheckStaticMesh(bundle, static_cast<uint32>(index), env, nextPackage, diagnostics);
        }
    }

    printf("Mesh records: %zu checked\n", totals.meshRecords);
    totals.skeletalRecords = CheckSkeletalAssets(bundle, env, nextPackage, diagnostics);
    printf("Skinned mesh records and clip builds: %zu checked\n", totals.skeletalRecords);
    totals.materialRecords = CheckMaterials(bundle);
    printf("Material records: %zu checked\n", totals.materialRecords);
    totals.referenceAssets = CheckAllReferences(bundle);
    printf("Reference checks: %zu assets\n", totals.referenceAssets);
    totals.assets = bundle.Assets().Size();
    totals.redirects = bundle.Redirects().Size();
    return totals;
}

TEST(UnrealExchange_ExternalBundle) {
    SystemAllocator allocator;
    StringBuilder<512> directory(allocator);
    if (!test::ReadEnvironmentVariable(BundleVariable, directory)) {
        printf("[SKIP] UnrealExchange_ExternalBundle: %s is not set\n", BundleVariable);
        return;
    }

    ExchangeBundle bundle(allocator);
    CheckBundle(directory.View(), allocator, bundle);
}

// 이름 있는 에셋이다(없으면 ExchangeNoIndex).
uint32 FindAsset(const ExchangeBundle& bundle, StringView objectPath) noexcept {
    for (usize index = 0; index < bundle.Assets().Size(); ++index) {
        if (bundle.Assets()[index].objectPath == objectPath) {
            return static_cast<uint32>(index);
        }
    }

    return ExchangeNoIndex;
}

// 플러그인 진단에 code가 있는가다.
bool HasExporterDiagnostic(const ExchangeAsset& asset, StringView code) noexcept {
    for (usize index = 0; index < asset.exporterDiagnostics.Size(); ++index) {
        if (asset.exporterDiagnostics.At(index).Find("code").GetString() == code) {
            return true;
        }
    }

    return false;
}

// 저장소 golden 묶음(RocksExport로 만든 실제 묶음 — UnrealExchange 12.3절)을 늘 읽어 위
// 판정을 모두 돌리고, 묶음이 담은 경계 사례를 확인한다: 퇴화 다각형(자기 변을 뺀 변 수와
// DegenerateEdgesDropped), 옛 경로 soft 참조(redirect 하나, 클립 의존이 새 경로·SkeletalMesh·soft·
// 묶음 안 대상), 범위 밖 에셋 하나(PhysicsAsset Unsupported).
TEST(UnrealExchange_GoldenBundle) {
    SystemAllocator allocator;
    StringBuilder<512> directory(allocator);
    REQUIRE(FixtureSourcePath("Golden.rxbundle", directory));
    ExchangeBundle bundle(allocator);
    const BundleTotals totals = CheckBundle(directory.View(), allocator, bundle);
    CHECK(totals.assets == GoldenAssets);
    CHECK(totals.redirects == 1);
    CHECK(totals.meshRecords == GoldenMeshRecords);
    CHECK(totals.skeletalRecords == GoldenSkeletalRecords);
    CHECK(totals.materialRecords == GoldenMaterialRecords);
    CHECK(totals.referenceAssets == GoldenAssets - 1);

    const uint32 degenerate = FindAsset(bundle, GoldenDegeneratePath);
    REQUIRE(degenerate != ExchangeNoIndex);
    const JsonValue source = bundle.Assets()[degenerate].data.Find("lods").At(0).Find("source");
    CHECK(CountOf(source.Find("counts"), "edges") == GoldenDegenerateEdges);
    CHECK(HasExporterDiagnostic(bundle.Assets()[degenerate], "DegenerateEdgesDropped"));

    const uint32 clip = FindAsset(bundle, GoldenClipPath);
    const uint32 target = FindAsset(bundle, GoldenSoftTargetPath);
    REQUIRE(clip != ExchangeNoIndex);
    REQUIRE(target != ExchangeNoIndex);
    CHECK(bundle.Redirects()[0].to == GoldenSoftTargetPath);
    bool soft = false;
    for (const ExchangeDependency& dependency : bundle.Assets()[clip].dependencies) {
        soft = soft || (dependency.objectPath == GoldenSoftTargetPath &&
                        dependency.kind == ExchangeDependencyKind::Soft &&
                        dependency.targetClass == ExchangeClass::SkeletalMesh &&
                        dependency.target == target);
    }

    CHECK(soft);
}

} // namespace exchange_external_bundle_tests_file
