#include "MeshBuildInternal.h"

#include <Base/NumericLimits.h>
#include <Platform/FloatingPointEnvironment.h>

// 정적 메시 LOD 빌드의 단계 연결이다(Docs/Design/MeshAnimationAssets 5.2·5.4절, UE
// FStaticMeshBuilder::Build의 LOD 루프). 단계 함수는 MeshLodBuild.cpp에 있다.
namespace rocks {

namespace {

// buildScale 축 수다(x·y·z).
constexpr uint32 BuildScaleAxes = 3;

// failure는 BuildStaticMeshLod의 지역 진단이라 늘 있다.
void Fail(MeshDiagnostic* failure, AssetDiagnosticCode code, ErrorCode error, MeshIssue issue,
          uint32 index) noexcept {
    MeshDiagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.error = error;
    diagnostic.issue = issue;
    diagnostic.index = index;
    *failure = diagnostic;
}

// buildScale을 float로 바꾸고 검사한다(축마다 0이 아닌 유한값, 역수도 유한, double이 float 범위
// 안). 음수 성분 수를 센다.
bool ConvertBuildScale(const Vec3d& scale, Vec3& out, uint32& negatives,
                       MeshDiagnostic* failure) noexcept {
    const double axes[BuildScaleAxes] = {scale.x, scale.y, scale.z};
    float converted[BuildScaleAxes] = {};
    negatives = 0;
    for (uint32 axis = 0; axis < BuildScaleAxes; ++axis) {
        const bool inRange =
            math::Abs(axes[axis]) <= static_cast<double>(NumericLimits<float>::Max());
        converted[axis] = inRange ? static_cast<float>(axes[axis]) : 0.0f;
        if (!inRange || converted[axis] == 0.0f || !math::IsFinite(1.0f / converted[axis])) {
            Fail(failure, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                 MeshIssue::InvalidBuildScale, axis);
            return false;
        }

        negatives += converted[axis] < 0.0f ? 1 : 0;
    }

    out = Vec3(converted[0], converted[1], converted[2]);
    return true;
}

// 크기를 곱한 위치가 모두 유한한지 본다(넘치면 bounds·용접이 비유한 값을 다루게 된다).
bool CheckScaledPositions(const MeshSource& source, const Vec3& scale,
                          MeshDiagnostic* failure) noexcept {
    for (const Vec3& position : source.Positions()) {
        const float scaled[BuildScaleAxes] = {position.x * scale.x, position.y * scale.y,
                                              position.z * scale.z};
        for (uint32 axis = 0; axis < BuildScaleAxes; ++axis) {
            if (!math::IsFinite(scaled[axis])) {
                Fail(failure, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                     MeshIssue::InvalidBuildScale, axis);
                return false;
            }
        }
    }

    return true;
}

// 5.2절 6·7단계(보존 설정)와 8단계 경고: 기본값이 아닌 보존 필드마다 UnsupportedSetting, 음수
// 성분이 홀수 개면 NegativeBuildScale.
Result<void> AppendSettingWarnings(const MeshBuildSettings& settings, uint32 negatives,
                                   Array<MeshDiagnostic>& out) noexcept {
    for (uint32 field = 0; field < MeshBuildSettingFieldCount; ++field) {
        const auto value = static_cast<MeshBuildSettingField>(field);
        if (IsPreservedMeshBuildSetting(value) && IsMeshBuildSettingChanged(settings, value)) {
            auto pushed = out.TryPushBack(mesh_build::MakeWarning(
                AssetDiagnosticCode::UnsupportedSetting, MeshIssue::UnsupportedSetting,
                MeshElement::Invalid, MeshNoAttribute, field, 0));
            if (!pushed) {
                return pushed;
            }
        }
    }

    if (negatives % 2 != 0) {
        return out.TryPushBack(mesh_build::MakeWarning(
            AssetDiagnosticCode::NegativeBuildScale, MeshIssue::NegativeBuildScale,
            MeshElement::Invalid, MeshNoAttribute, MeshNoIndex, negatives));
    }

    return Result<void>::Success();
}

// 5.2절 1–5·8–10·12단계를 차례로 한다(결정적 부동소수점 환경 안).
Result<void> BuildInEnvironment(const StaticMeshBuildInput& input, Allocator& scratch,
                                StaticMeshBuildResult& result, MeshDiagnostic* failure) noexcept {
    const MeshSource& source = *input.source;
    MeshValidateOptions options;
    options.scratch = &scratch;
    if (auto valid = ValidateMeshSource(source, options, failure); !valid) {
        return valid;
    }

    const uint32 triangles = source.Count(MeshElement::Triangle);
    if (static_cast<uint64>(triangles) * 3 > NumericLimits<uint32>::Max() ||
        triangles > MeshMikkTSpaceMaxTriangles) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    Vec3 scale;
    uint32 negatives = 0;
    if (!ConvertBuildScale(input.settings.buildScale, scale, negatives, failure) ||
        !CheckScaledPositions(source, scale, failure)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 2단계: 사본에 값 정리.
    MeshSource work(scratch);
    mesh_build::WorkAttributes attributes(scratch);
    if (auto copied = mesh_build::CopyBuildMesh(source, false, work, attributes); !copied) {
        return copied;
    }

    if (auto sanitized =
            mesh_build::SanitizeWorkMesh(work, attributes, options, result.warnings, failure);
        !sanitized) {
        return sanitized;
    }

    if (auto appended = AppendSettingWarnings(input.settings, negatives, result.warnings);
        !appended) {
        return appended;
    }

    // 3–5단계.
    const float threshold = input.settings.removeDegenerates ? MeshPointsSameThreshold : 0.0f;
    mesh_build::LodBases bases(scratch);
    if (auto computed = mesh_build::ComputeLodBases(source, work, attributes, input.settings,
                                                    threshold, scratch, bases);
        !computed) {
        return computed;
    }

    if (bases.hasUv) {
        result.texCoordChannels =
            work.AttributeInfo(attributes.work[mesh_build::TexCoordSlot]).channels;
    }

    mesh_build::OverlappingWedges overlaps(scratch);
    if (auto found = mesh_build::FindOverlappingWedges(work, threshold, scratch, overlaps);
        !found) {
        return found;
    }

    // 8–10·12단계(11단계 정점 cache 최적화는 meshoptimizer 어댑터 전까지 원본 순서).
    mesh_build::WeldOptions weld;
    weld.degenerateThreshold = threshold;
    weld.positionThreshold = threshold;
    mesh_build::VertexBuffers buffers(scratch);
    if (auto built = mesh_build::BuildRenderVertices(work, bases.basis, attributes, overlaps, scale,
                                                     weld, result, buffers);
        !built) {
        return built;
    }

    if (auto built = mesh_build::BuildSections(source, input.slotNames, input.sectionInfo, buffers,
                                               scratch, result);
        !built) {
        return built;
    }

    result.bounds = mesh_build::ComputeBounds(
        result.positions.View(), input.positiveBoundsExtension, input.negativeBoundsExtension);
    return Result<void>::Success();
}

struct BuildJob {
    const StaticMeshBuildInput* input = nullptr;
    Allocator* scratch = nullptr;
    StaticMeshBuildResult* result = nullptr;
    MeshDiagnostic* failure = nullptr;
    Result<void> status = Result<void>::Success();
};

} // namespace

Result<void> BuildStaticMeshLod(const StaticMeshBuildInput& input, StaticMeshBuildResult& out,
                                MeshDiagnostic* failure) noexcept {
    if (input.source == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = out.positions.GetAllocator();
    StaticMeshBuildResult result(allocator);
    MeshDiagnostic diagnostic;
    BuildJob job;
    job.input = &input;
    job.scratch = input.scratch != nullptr ? input.scratch : &allocator;
    job.result = &result;
    job.failure = &diagnostic;
    platform::RunWithDeterministicFloatingPoint(
        [](void* context) noexcept {
            auto& build = *static_cast<BuildJob*>(context);
            build.status =
                BuildInEnvironment(*build.input, *build.scratch, *build.result, build.failure);
        },
        &job);
    if (!job.status) {
        if (failure != nullptr) {
            diagnostic.error = job.status.Error();
            *failure = diagnostic;
        }

        return Result<void>::Failure(job.status.Error());
    }

    out = Move(result);
    return Result<void>::Success();
}

} // namespace rocks
