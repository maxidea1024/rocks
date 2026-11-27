#include "MeshSourceInternal.h"

#include <Core/Assert.h>
#include <Core/Math/MathUtility.h>
#include <MeshSource/MeshSourceValidation.h>

// 원본 메시 값 정리와 스킨 가중치 정규화다(Docs/Design/MeshAnimationAssets 3.2·3.3·3.7절).
namespace rocks {

namespace {

// 경고를 낼 수 있는 속성 하나당 최대 경고 수다(TexCoord: 비유한값 + 범위 밖).
constexpr usize SanitizeWarningsPerAttribute = 2;

// 색 비유한값을 바꿀 값의 성분이다(UE ValidateAndFixData와 같은 흰색).
constexpr float SanitizeColorFallback = 1.0f;

MeshDiagnostic SanitizeWarning(AssetDiagnosticCode code, MeshIssue issue, MeshElement element,
                               uint32 attribute, uint64 count) noexcept {
    MeshDiagnostic warning;
    warning.code = code;
    warning.issue = issue;
    warning.element = element;
    warning.attribute = attribute;
    warning.count = count;
    return warning;
}

// 법선·tangent 성분 배열(Vec3)의 비유한값·길이 0을 0 벡터로 바꾼다.
void SanitizeDirections(Span<float> values) noexcept {
    for (usize at = 0; at + 3 <= values.Size(); at += 3) {
        const float x = values[at];
        const float y = values[at + 1];
        const float z = values[at + 2];
        const bool finite = math::IsFinite(x) && math::IsFinite(y) && math::IsFinite(z);
        if (!finite || x * x + y * y + z * z == 0.0f) {
            values[at] = 0.0f;
            values[at + 1] = 0.0f;
            values[at + 2] = 0.0f;
        }
    }
}

// UV 비유한 성분을 0으로 바꾼 값 수와, 그 뒤 범위 밖 값 수를 센다.
void SanitizeTexCoords(Span<float> values, uint64& fixed, uint64& outOfRange) noexcept {
    for (usize at = 0; at + 2 <= values.Size(); at += 2) {
        bool changed = false;
        for (usize component = at; component < at + 2; ++component) {
            if (!math::IsFinite(values[component])) {
                values[component] = 0.0f;
                changed = true;
            }
        }

        fixed += changed ? 1 : 0;
        if (math::Abs(values[at]) > MeshUvRangeWarning ||
            math::Abs(values[at + 1]) > MeshUvRangeWarning) {
            ++outOfRange;
        }
    }
}

// 색 비유한값을 흰색으로 바꾼 값 수다.
uint64 SanitizeColors(Span<float> values) noexcept {
    uint64 fixed = 0;
    for (usize at = 0; at + 4 <= values.Size(); at += 4) {
        bool finite = true;
        for (usize component = at; component < at + 4; ++component) {
            finite = finite && math::IsFinite(values[component]);
        }

        if (!finite) {
            for (usize component = at; component < at + 4; ++component) {
                values[component] = SanitizeColorFallback;
            }

            ++fixed;
        }
    }

    return fixed;
}

uint64 InfluenceWeightSum(Span<const BoneInfluence> influences) noexcept {
    uint64 sum = 0;
    for (const BoneInfluence& influence : influences) {
        sum += influence.weight;
    }

    return sum;
}

} // namespace

bool NormalizeBoneWeights(Span<BoneInfluence> influences) noexcept {
    const uint64 sum = InfluenceWeightSum(influences);
    if (influences.IsEmpty() || sum == BoneWeightTotal || sum == 0) {
        return false;
    }

    // 잔차를 받을 영향: 원래 가중치가 가장 크고, 같으면 boneIndex가 작은 쪽.
    usize largest = 0;
    for (usize index = 1; index < influences.Size(); ++index) {
        const BoneInfluence& candidate = influences[index];
        const BoneInfluence& best = influences[largest];
        if (candidate.weight > best.weight ||
            (candidate.weight == best.weight && candidate.boneIndex < best.boneIndex)) {
            largest = index;
        }
    }

    uint64 assigned = 0;
    for (BoneInfluence& influence : influences) {
        influence.weight = static_cast<uint16>(influence.weight * uint64{BoneWeightTotal} / sum);
        assigned += influence.weight;
    }

    // 내림 잔차는 영향 수보다 작고, 받는 쪽의 새 값 + 잔차 = Total − 나머지 새 값 합이라 넘치지
    // 않는다.
    influences[largest].weight =
        static_cast<uint16>(influences[largest].weight + (BoneWeightTotal - assigned));
    return true;
}

Result<void> SanitizeMeshSource(MeshSource& mesh, const MeshValidateOptions& options,
                                Array<MeshDiagnostic>& warnings, MeshDiagnostic* failure) noexcept {
    if (auto valid = ValidateMeshSource(mesh, options, failure); !valid) {
        return valid;
    }

    Array<MeshSource::Attribute>& attributes = MeshSourceAccess::Attributes(mesh);
    // 1. 고치기 전에 실패할 수 있는 것을 모두 본다: 가중치 합 0, 경고 칸.
    for (usize attribute = 0; attribute < attributes.Size(); ++attribute) {
        if (rocks::mesh_source_detail::ClassifyAttributeName(attributes[attribute].name.View()) !=
            rocks::mesh_source_detail::EngineName::SkinWeights) {
            continue;
        }

        for (uint32 vertex = 0; vertex < mesh.Count(MeshElement::Vertex); ++vertex) {
            const Span<const BoneInfluence> influences =
                mesh.Influences(static_cast<uint32>(attribute), vertex);
            if (!influences.IsEmpty() && InfluenceWeightSum(influences) == 0) {
                return rocks::mesh_source_detail::Fail(
                    failure, AssetDiagnosticCode::InvalidMeshData, ErrorCode::InvalidArgument,
                    MeshIssue::ZeroWeightSum, MeshElement::Vertex, static_cast<uint32>(attribute),
                    vertex);
            }
        }
    }

    if (auto reserved = warnings.TryEnsureCapacity(
            warnings.Size() + attributes.Size() * SanitizeWarningsPerAttribute);
        !reserved) {
        return reserved;
    }

    // 2. 고친다(실패하지 않는다).
    for (usize index = 0; index < attributes.Size(); ++index) {
        MeshSource::Attribute& attribute = attributes[index];
        const auto attributeIndex = static_cast<uint32>(index);
        const rocks::mesh_source_detail::EngineName kind =
            rocks::mesh_source_detail::ClassifyAttributeName(attribute.name.View());
        switch (kind) {
        case rocks::mesh_source_detail::EngineName::Normal:
        case rocks::mesh_source_detail::EngineName::Tangent:
            SanitizeDirections(attribute.channels[0].floats.View());
            break;
        case rocks::mesh_source_detail::EngineName::TexCoord: {
            uint64 fixed = 0;
            uint64 outOfRange = 0;
            for (MeshSource::Channel& channel : attribute.channels) {
                SanitizeTexCoords(channel.floats.View(), fixed, outOfRange);
            }

            if (fixed != 0) {
                ROCKS_ASSERT(
                    warnings
                        .TryPushBack(SanitizeWarning(AssetDiagnosticCode::NonFiniteAttribute,
                                                     MeshIssue::NonFiniteTexCoord,
                                                     attribute.element, attributeIndex, fixed))
                        .IsOk());
            }

            if (outOfRange != 0) {
                ROCKS_ASSERT(warnings
                                 .TryPushBack(SanitizeWarning(
                                     AssetDiagnosticCode::UvOutOfRange, MeshIssue::UvOutOfRange,
                                     attribute.element, attributeIndex, outOfRange))
                                 .IsOk());
            }

            break;
        }
        case rocks::mesh_source_detail::EngineName::Color:
            if (const uint64 fixed = SanitizeColors(attribute.channels[0].floats.View());
                fixed != 0) {
                ROCKS_ASSERT(
                    warnings
                        .TryPushBack(SanitizeWarning(AssetDiagnosticCode::NonFiniteAttribute,
                                                     MeshIssue::NonFiniteColor, attribute.element,
                                                     attributeIndex, fixed))
                        .IsOk());
            }

            break;
        case rocks::mesh_source_detail::EngineName::SkinWeights: {
            MeshSource::Channel& channel = attribute.channels[0];
            uint64 normalized = 0;
            for (const MeshSource::ValueRange& range : channel.ranges) {
                Span<BoneInfluence> influences(channel.influences.Data() + range.offset,
                                               range.count);
                normalized += NormalizeBoneWeights(influences) ? 1 : 0;
            }

            if (normalized != 0) {
                ROCKS_ASSERT(
                    warnings
                        .TryPushBack(SanitizeWarning(AssetDiagnosticCode::NonNormalizedWeights,
                                                     MeshIssue::NonNormalizedWeights,
                                                     attribute.element, attributeIndex, normalized))
                        .IsOk());
            }

            break;
        }
        default:
            break;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
