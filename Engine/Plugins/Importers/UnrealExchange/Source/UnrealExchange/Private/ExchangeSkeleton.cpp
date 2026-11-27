#include <UnrealExchange/ExchangeSkeleton.h>
#include <UnrealExchange/ExchangeSkeletonSettings.h>

#include "ExchangeMeshCommon.h"

// 교환 스켈레톤 → 스켈레톤 임포트 명세 변환이다(Docs/Design/UnrealExchange 6.4절 1번).

namespace rocks {

namespace exchange_skeleton {

using namespace exchange_detail;
using SettingRow = ExchangeSkeletonSettingRow;

// 커브 메타데이터 AssetUserData 클래스다(커브는 data.curves에 있다 — 6.3절 2번).
constexpr StringView CurveMetaDataClass = "/Script/Engine.AnimCurveMetaData";
// 커브 maxLod 상한(uint8)이다.
constexpr uint64 MaxCurveLod = NumericLimits<uint8>::Max();

} // namespace exchange_skeleton

// ExchangeSkeleton::Build의 단계들이다.
struct ExchangeSkeletonBuilder : exchange_detail::ExchangeMeshContext {
    ExchangeSkeleton& skeleton;
    const ExchangeSkeletonInput& input;

    Result<void> ReadCurves() noexcept {
        using namespace exchange_skeleton;
        const JsonValue curves = Member(input.asset->data, "curves");
        if (!OptArray(curves)) {
            return Corrupt(0);
        }

        for (usize index = 0; index < curves.Size(); ++index) {
            const JsonValue item = curves.At(index);
            const JsonValue linked = Member(item, "linkedBones");
            SkeletonImportCurve curve;
            uint64 maxLod = 0;
            if (!ReadString(Member(item, "name"), curve.name) ||
                !ReadBool(Member(item, "isMaterial"), curve.isMaterial) ||
                !ReadBool(Member(item, "isMorphTarget"), curve.isMorphTarget) ||
                !ReadUInt(Member(item, "maxLod"), maxLod) || maxLod > MaxCurveLod ||
                !linked.IsArray()) {
                return Corrupt(index);
            }

            curve.maxLod = static_cast<uint8>(maxLod);
            ExchangeSkeleton::CurveBones range;
            range.start = skeleton.linkedBones_.Size();
            range.count = linked.Size();
            for (usize bone = 0; bone < linked.Size(); ++bone) {
                StringView name;
                if (!ReadString(linked.At(bone), name)) {
                    return Corrupt(index);
                }

                if (auto pushed = skeleton.linkedBones_.TryPushBack(name); !pushed) {
                    return pushed;
                }
            }

            if (auto pushed = skeleton.curves_.TryPushBack(curve); !pushed) {
                return pushed;
            }

            if (auto pushed = skeleton.curveBones_.TryPushBack(range); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    Result<void> ReadVirtualBones() noexcept {
        using namespace exchange_skeleton;
        constexpr uint64 Detail = static_cast<uint64>(SettingRow::VirtualBones);
        const JsonValue bones = Member(skeleton.translated_.Root(), "VirtualBones");
        if (!OptArray(bones)) {
            return Corrupt(Detail);
        }

        for (usize index = 0; index < bones.Size(); ++index) {
            const JsonValue item = bones.At(index);
            SkeletonImportVirtualBone bone;
            if (!ReadString(Member(item, "SourceBoneName"), bone.sourceBone) ||
                !ReadString(Member(item, "TargetBoneName"), bone.targetBone) ||
                !ReadString(Member(item, "VirtualBoneName"), bone.name)) {
                return Corrupt(Detail);
            }

            if (auto pushed = skeleton.virtualBones_.TryPushBack(bone); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    // 커브 메타데이터가 아닌 AssetUserData가 있으면 경고 하나다(6.4절 1번).
    Result<void> CheckAssetUserData() noexcept {
        using namespace exchange_skeleton;
        constexpr uint64 Detail = static_cast<uint64>(SettingRow::AssetUserData);
        const JsonValue items = Member(input.asset->settings, "AssetUserData");
        if (!OptArray(items)) {
            return Corrupt(Detail);
        }

        for (usize index = 0; index < items.Size(); ++index) {
            StringView type;
            const JsonValue item = items.At(index);
            if (!item.IsNull() &&
                (!ReadString(Member(item, "class"), type) || type != CurveMetaDataClass)) {
                return Warn(AssetDiagnosticCode::UnsupportedSetting, Detail, "AssetUserData");
            }
        }

        return Result<void>::Success();
    }

    void Finish() noexcept {
        for (usize index = 0; index < skeleton.curves_.Size(); ++index) {
            const ExchangeSkeleton::CurveBones range = skeleton.curveBones_[index];
            skeleton.curves_[index].linkedBones =
                Span<const StringView>(skeleton.linkedBones_.Data() + range.start, range.count);
        }

        SkeletonImportSpec& spec = skeleton.spec_;
        spec.bones = skeleton.bones_.View();
        spec.retargetModes = skeleton.retargetModes_.View();
        spec.virtualBones = skeleton.virtualBones_.View();
        spec.sockets = skeleton.sockets_.View();
        spec.curves = skeleton.curves_.View();
        spec.replaceOnReimport = true;
    }
};

ExchangeSkeleton::ExchangeSkeleton(Allocator& allocator) noexcept
    : allocator_(&allocator), translated_(allocator), bones_(allocator), retargetModes_(allocator),
      virtualBones_(allocator), sockets_(allocator), curves_(allocator), curveBones_(allocator),
      linkedBones_(allocator), warnings_(allocator) {
}

Result<void> ExchangeSkeleton::Build(const ExchangeSkeletonInput& input,
                                     ExchangeMeshFailure& failure) noexcept {
    using namespace exchange_skeleton;
    failure = ExchangeMeshFailure{};
    if (input.asset == nullptr || input.registry == nullptr || input.registry->Names() == nullptr ||
        input.asset->exchangeClass != ExchangeClass::Skeleton || started_) {
        failure.code = AssetDiagnosticCode::InvalidSource;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    started_ = true;
    ExchangeSkeletonBuilder builder{{failure, *allocator_, *input.registry, input.access,
                                     input.references, *input.registry->Names(), warnings_},
                                    *this,
                                    input};
    if (auto done = builder.Translate(input.asset->settings, ExchangeSkeletonSettingRows(),
                                      ExchangeClass::Skeleton, translated_);
        !done) {
        return done;
    }

    if (auto done = builder.ReadBones(Member(input.asset->data, "bones"), bones_, &retargetModes_);
        !done) {
        return done;
    }

    if (auto done = builder.ReadCurves(); !done) {
        return done;
    }

    if (auto done = builder.ReadVirtualBones(); !done) {
        return done;
    }

    if (auto done = builder.ReadBoneSockets(Member(translated_.Root(), "Sockets"), sockets_,
                                            static_cast<uint64>(SettingRow::Sockets));
        !done) {
        return done;
    }

    if (auto done = builder.CheckAssetUserData(); !done) {
        return done;
    }

    builder.Finish();
    return Result<void>::Success();
}

} // namespace rocks
