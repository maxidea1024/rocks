#include "MeshSourceTestSupport.h"

#include <Base/NumericLimits.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>

// 값 정리·스킨 가중치 정규화 시험이다(Docs/Design/MeshAnimationAssets 3.2·3.3·3.7절, 12절
// "정규화 잔차·동률"): 법선·tangent·UV·색 고치기와 경고 수, UV 범위 경고, 가중치 정규화(비례 배분
// + 잔차를 가장 큰 영향에, 동률이면 작은 본 번호), 합 0 거부와 실패 시 무변경, 진단 이름표.
using namespace rocks;
namespace mesh_source_sanitize_tests_file {

using namespace rocks::mesh_source_test;

namespace {

uint32 WeightSum(Span<const BoneInfluence> influences) noexcept {
    uint32 sum = 0;
    for (const BoneInfluence& influence : influences) {
        sum += influence.weight;
    }

    return sum;
}

const MeshDiagnostic* FindWarning(const Array<MeshDiagnostic>& warnings, MeshIssue issue) noexcept {
    for (const MeshDiagnostic& warning : warnings) {
        if (warning.issue == issue) {
            return &warning;
        }
    }

    return nullptr;
}

} // namespace

// 정규화 단위: 이미 Total이면 그대로, 비례 배분 뒤 잔차는 원래 가중치가 가장 큰 영향에 간다.
TEST(MeshSource_NormalizeWeightsResidual) {
    BoneInfluence already[] = {{1, 30000}, {2, 35535}};
    CHECK(!NormalizeBoneWeights(already));
    CHECK(already[0].weight == 30000 && already[1].weight == 35535);
    CHECK(!NormalizeBoneWeights(Span<BoneInfluence>()));
    BoneInfluence zero[] = {{1, 0}, {2, 0}};
    CHECK(!NormalizeBoneWeights(zero));
    CHECK(zero[0].weight == 0);

    // 1:1:1 — 65535 / 3 = 21845 나머지 0(잔차 없음).
    BoneInfluence thirds[] = {{4, 7}, {9, 7}, {2, 7}};
    CHECK(NormalizeBoneWeights(thirds));
    CHECK(thirds[0].weight == 21845 && thirds[1].weight == 21845 && thirds[2].weight == 21845);

    // 1:1 합 2 — 각 32767.5 → 내림 32767 둘, 잔차 1은 동률이라 boneIndex가 작은 쪽(3)에 간다.
    BoneInfluence tie[] = {{8, 1}, {3, 1}};
    CHECK(NormalizeBoneWeights(tie));
    CHECK(tie[0].boneIndex == 8 && tie[0].weight == 32767);
    CHECK(tie[1].boneIndex == 3 && tie[1].weight == 32768);
    CHECK(WeightSum(tie) == BoneWeightTotal);

    // 3:2:2 합 7 — 28086.43 → 28086, 18724.29 → 18724 둘, 잔차 1은 가장 큰 영향(본 5)에.
    BoneInfluence uneven[] = {{6, 2}, {5, 3}, {1, 2}};
    CHECK(NormalizeBoneWeights(uneven));
    CHECK(uneven[0].weight == 18724 && uneven[1].weight == 28087 && uneven[2].weight == 18724);
    CHECK(WeightSum(uneven) == BoneWeightTotal);

    // 합이 Total보다 큼(최대 12 × 65535 — 넘침 없음).
    BoneInfluence heavy[MeshMaxSourceInfluences];
    for (uint16 index = 0; index < MeshMaxSourceInfluences; ++index) {
        heavy[index] = BoneInfluence{static_cast<uint16>(20 - index), BoneWeightTotal};
    }

    CHECK(NormalizeBoneWeights(heavy));
    CHECK(WeightSum(heavy) == BoneWeightTotal);
    // 모두 같으므로 잔차는 boneIndex가 가장 작은 마지막 영향(본 9)에 간다.
    CHECK(heavy[11].weight == heavy[0].weight + 3);

    // 비교: 본이 다르거나(동률 영향) 가중치만 다르면 다른 영향이다.
    CHECK(!(thirds[0] == thirds[1]));
    BoneInfluence shifted = thirds[0];
    shifted.weight = static_cast<uint16>(shifted.weight + 1);
    CHECK(!(shifted == thirds[0]));
    shifted.weight = thirds[0].weight;
    CHECK(shifted == thirds[0]);

    // 영향 하나는 Total이 된다.
    BoneInfluence single[] = {{0, 12}};
    CHECK(NormalizeBoneWeights(single));
    CHECK(single[0].weight == BoneWeightTotal);
}

// 값 정리: 법선·tangent의 비유한·길이 0은 0 벡터(경고 없음), UV 비유한 성분은 0(경고 — 값 수),
// 그 뒤 범위 밖 UV 수(경고 — 값은 그대로), 색 비유한값은 흰색(경고), 정상 값은 그대로.
TEST(MeshSource_SanitizeFixesValues) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildRichMesh(mesh));
    const float nan = TestQuietNaN();
    const float inf = TestInfinity();
    const uint32 normal = mesh.FindAttribute(MeshElement::Corner, "Normal");
    const uint32 tangent = mesh.FindAttribute(MeshElement::Corner, "Tangent");
    const uint32 uv = mesh.FindAttribute(MeshElement::Corner, "TexCoord");
    const uint32 color = mesh.FindAttribute(MeshElement::Corner, "Color");
    mesh.SetVec3(normal, 0, 0, Vec3(nan, 0.0f, 1.0f));
    mesh.SetVec3(normal, 0, 1, Vec3(0.0f, 0.0f, 0.0f));
    mesh.SetVec3(normal, 0, 2, Vec3(1e-30f, 0.0f, 0.0f));
    mesh.SetVec3(tangent, 0, 3, Vec3(1.0f, -inf, 0.0f));
    mesh.SetVec3(tangent, 0, 5, Vec3(1.0f, 0.0f, nan));
    mesh.SetVec2(uv, 0, 0, Vec2(nan, 0.5f));
    mesh.SetVec2(uv, 1, 1, Vec2(inf, -inf));
    mesh.SetVec2(uv, 0, 2, Vec2(1024.0f, -1024.0f));
    mesh.SetVec2(uv, 1, 3, Vec2(0.0f, -1024.5f));
    mesh.SetVec2(uv, 0, 4, Vec2(2000.0f, 0.0f));
    mesh.SetVec4(color, 0, 5, Vec4(0.5f, nan, 0.5f, 1.0f));
    mesh.SetVec4(color, 0, 6, Vec4(0.5f, 0.5f, 0.5f, -inf));
    Array<MeshDiagnostic> warnings(allocator);
    REQUIRE(SanitizeMeshSource(mesh, {}, warnings));
    const Vec3 fixedNormal = mesh.GetVec3(normal, 0, 0);
    CHECK(fixedNormal.x == 0.0f && fixedNormal.y == 0.0f && fixedNormal.z == 0.0f);
    CHECK(mesh.GetVec3(normal, 0, 2).x == 0.0f);
    CHECK(mesh.GetVec3(normal, 0, 3).z == 1.0f);
    CHECK(mesh.GetVec3(tangent, 0, 3).x == 0.0f);
    CHECK(mesh.GetVec3(tangent, 0, 4).x == 1.0f);
    CHECK(mesh.GetVec3(tangent, 0, 5).x == 0.0f);
    CHECK(mesh.GetVec2(uv, 0, 0).x == 0.0f && mesh.GetVec2(uv, 0, 0).y == 0.5f);
    CHECK(mesh.GetVec2(uv, 1, 1).x == 0.0f && mesh.GetVec2(uv, 1, 1).y == 0.0f);
    CHECK(mesh.GetVec2(uv, 1, 3).y == -1024.5f);
    CHECK(mesh.GetVec4(color, 0, 5).x == 1.0f && mesh.GetVec4(color, 0, 5).y == 1.0f);
    CHECK(mesh.GetVec4(color, 0, 6).w == 1.0f);
    CHECK(mesh.GetVec4(color, 0, 4).y == 0.5f);
    const MeshDiagnostic* nonFiniteUv = FindWarning(warnings, MeshIssue::NonFiniteTexCoord);
    const MeshDiagnostic* range = FindWarning(warnings, MeshIssue::UvOutOfRange);
    const MeshDiagnostic* nonFiniteColor = FindWarning(warnings, MeshIssue::NonFiniteColor);
    REQUIRE(nonFiniteUv != nullptr && range != nullptr && nonFiniteColor != nullptr);
    CHECK(nonFiniteUv->code == AssetDiagnosticCode::NonFiniteAttribute && nonFiniteUv->count == 2 &&
          nonFiniteUv->attribute == uv && nonFiniteUv->element == MeshElement::Corner &&
          nonFiniteUv->error == ErrorCode::None);
    // 1024는 경계(넘지 않음), -1024.5와 2000만 센다.
    CHECK(range->code == AssetDiagnosticCode::UvOutOfRange && range->count == 2);
    CHECK(nonFiniteColor->code == AssetDiagnosticCode::NonFiniteAttribute &&
          nonFiniteColor->count == 2 && nonFiniteColor->attribute == color);
    // 리치 메시의 가중치는 이미 정규화되어 있어 NonNormalizedWeights가 없다.
    CHECK(FindWarning(warnings, MeshIssue::NonNormalizedWeights) == nullptr);
    CHECK(warnings.Size() == 3);
    CHECK(ValidateMeshSource(mesh).IsOk());

    // 고칠 것이 없으면 경고도 없다(두 번째 실행).
    Array<MeshDiagnostic> again(allocator);
    REQUIRE(SanitizeMeshSource(mesh, {}, again));
    CHECK(again.Size() == 1 && again[0].issue == MeshIssue::UvOutOfRange);
}

// 가중치 정규화 경고: 합이 Total이 아닌 정점 수를 세고 SkinWeights 프로필에도 적용한다. 엔진
// 이름이 아닌 BoneInfluences 속성은 건드리지 않는다.
TEST(MeshSource_SanitizeNormalizesWeights) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildQuad(mesh));
    const uint32 skin =
        mesh.AddAttribute(MeshElement::Vertex, "SkinWeights", MeshValueType::BoneInfluences, 1)
            .Value();
    const uint32 cloth = mesh.AddAttribute(MeshElement::Vertex, "SkinWeights-Cloth",
                                           MeshValueType::BoneInfluences, 1)
                             .Value();
    const uint32 custom =
        mesh.AddAttribute(MeshElement::Vertex, "Extra", MeshValueType::BoneInfluences, 1).Value();
    const BoneInfluence half[] = {{2, 100}, {1, 100}};
    const BoneInfluence full[] = {{0, BoneWeightTotal}};
    const BoneInfluence zero[] = {{5, 0}};
    REQUIRE(mesh.SetInfluences(skin, 0, half));
    REQUIRE(mesh.SetInfluences(skin, 1, full));
    REQUIRE(mesh.SetInfluences(skin, 2, half));
    REQUIRE(mesh.SetInfluences(cloth, 3, half));
    REQUIRE(mesh.SetInfluences(custom, 0, half));
    REQUIRE(mesh.SetInfluences(custom, 1, zero));
    Array<MeshDiagnostic> warnings(allocator);
    REQUIRE(SanitizeMeshSource(mesh, {}, warnings));
    REQUIRE(warnings.Size() == 2);
    CHECK(warnings[0].attribute == skin && warnings[0].count == 2 &&
          warnings[0].code == AssetDiagnosticCode::NonNormalizedWeights);
    CHECK(warnings[1].attribute == cloth && warnings[1].count == 1);
    CHECK(mesh.Influences(skin, 0)[0].weight == 32767 &&
          mesh.Influences(skin, 0)[1].weight == 32768);
    CHECK(mesh.Influences(skin, 1)[0].weight == BoneWeightTotal);
    CHECK(mesh.Influences(cloth, 3)[1].weight == 32768);
    CHECK(mesh.Influences(custom, 0)[0].weight == 100);
    CHECK(mesh.Influences(custom, 1)[0].weight == 0);
}

// 가중치 합 0인 정점은 정규화할 수 없어 ZeroWeightSum(InvalidMeshData)이고, 앞서 고칠 수 있던 값도
// 고치지 않는다(실패 전에 모두 확인). 검증 실패도 아무것도 바꾸지 않는다.
TEST(MeshSource_SanitizeFailureKeepsMesh) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildRichMesh(mesh));
    const uint32 uv = mesh.FindAttribute(MeshElement::Corner, "TexCoord");
    const uint32 skin = mesh.FindAttribute(MeshElement::Vertex, "SkinWeights");
    mesh.SetVec2(uv, 0, 0, Vec2(TestQuietNaN(), 0.0f));
    const BoneInfluence zero[] = {{1, 0}, {2, 0}};
    REQUIRE(mesh.SetInfluences(skin, 4, zero));
    Array<MeshDiagnostic> warnings(allocator);
    MeshDiagnostic failure;
    auto sanitized = SanitizeMeshSource(mesh, {}, warnings, &failure);
    CHECK(!sanitized && sanitized.Error() == ErrorCode::InvalidArgument);
    CHECK(HasIssue(failure, AssetDiagnosticCode::InvalidMeshData, MeshIssue::ZeroWeightSum));
    CHECK(failure.attribute == skin && failure.index == 4 &&
          failure.element == MeshElement::Vertex);
    CHECK(warnings.IsEmpty());
    CHECK(!(mesh.GetVec2(uv, 0, 0).x == mesh.GetVec2(uv, 0, 0).x));

    MeshSource invalid(allocator);
    REQUIRE(BuildQuad(invalid));
    REQUIRE(invalid.AddEdge(1, 0));
    MeshDiagnostic duplicate;
    CHECK(!SanitizeMeshSource(invalid, {}, warnings, &duplicate));
    CHECK(duplicate.issue == MeshIssue::DuplicateEdge);
    CHECK(!SanitizeMeshSource(invalid, {}, warnings));
}

// 경고 칸 할당이 실패하면 그 오류이고 메시는 그대로다.
TEST(MeshSource_SanitizeWarningAllocation) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildRichMesh(mesh));
    const uint32 color = mesh.FindAttribute(MeshElement::Corner, "Color");
    mesh.SetVec4(color, 0, 0, Vec4(TestQuietNaN(), 0.0f, 0.0f, 0.0f));
    test::NFailAllocator failing(allocator);
    Array<MeshDiagnostic> warnings(failing);
    failing.FailOnAttempt(1);
    MeshValidateOptions options;
    options.scratch = &allocator;
    auto sanitized = SanitizeMeshSource(mesh, options, warnings);
    CHECK(!sanitized && sanitized.Error() == ErrorCode::OutOfMemory);
    CHECK(!(mesh.GetVec4(color, 0, 0).x == mesh.GetVec4(color, 0, 0).x));
    failing.DisableFailure();
    REQUIRE(SanitizeMeshSource(mesh, options, warnings));
    CHECK(mesh.GetVec4(color, 0, 0).x == 1.0f);
}

// 진단 이름표: 모든 MeshIssue가 고유한 이름을 갖고 열거 밖 값은 "Unknown"이다. 메시 진단 코드는
// AssetSystem 4.6절 이름표에 등록되어 있다.
TEST(MeshSource_DiagnosticNames) {
    constexpr uint32 last = static_cast<uint32>(MeshIssue::RenderInverseBind);
    for (uint32 index = 0; index <= last; ++index) {
        const StringView name(MeshIssueName(static_cast<MeshIssue>(index)));
        CHECK(name != StringView("Unknown"));
        for (uint32 other = 0; other < index; ++other) {
            CHECK(name != StringView(MeshIssueName(static_cast<MeshIssue>(other))));
        }
    }

    CHECK(StringView(MeshIssueName(static_cast<MeshIssue>(last + 1))) == StringView("Unknown"));
    CHECK(StringView(MeshIssueName(MeshIssue::DuplicateEdge)) == StringView("DuplicateEdge"));
    CHECK(StringView(AssetDiagnosticName(AssetDiagnosticCode::InvalidMeshData)) ==
          StringView("InvalidMeshData"));
    CHECK(StringView(AssetDiagnosticName(AssetDiagnosticCode::TooManyInfluences)) ==
          StringView("TooManyInfluences"));
    CHECK(StringView(AssetDiagnosticName(AssetDiagnosticCode::InfluencesTrimmed)) ==
          StringView("InfluencesTrimmed"));
    CHECK(StringView(AssetDiagnosticName(AssetDiagnosticCode::MissingInfluences)) ==
          StringView("MissingInfluences"));
    // 엔진 접두 판정(3.7절 5): ASCII 대소문자 무시, 접두 뒤가 비면 거짓.
    CHECK(MatchesMeshAttributePrefix("skinweights-Cloth", MeshAttributeName::SkinWeightsPrefix));
    CHECK(!MatchesMeshAttributePrefix("SkinWeights-", MeshAttributeName::SkinWeightsPrefix));
    CHECK(!MatchesMeshAttributePrefix("SkinWeights", MeshAttributeName::SkinWeightsPrefix));
    CHECK(!MatchesMeshAttributePrefix("MorphNormal-Smile", MeshAttributeName::MorphPrefix));
    CHECK(MatchesMeshAttributePrefix("MORPH-Smile", MeshAttributeName::MorphPrefix));
    MeshDiagnostic warning;
    warning.code = AssetDiagnosticCode::UvOutOfRange;
    warning.count = 17;
    const AssetDiagnostic asset = ToAssetDiagnostic(warning, AssetId{});
    CHECK(asset.code == AssetDiagnosticCode::UvOutOfRange && asset.detail == 17 &&
          asset.error == ErrorCode::None);
    // 요소 번호가 있으면 detail은 그 번호다.
    warning.index = 5;
    CHECK(ToAssetDiagnostic(warning, AssetId{}).detail == 5);
}

} // namespace mesh_source_sanitize_tests_file
