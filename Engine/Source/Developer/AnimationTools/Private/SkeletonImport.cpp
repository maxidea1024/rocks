#include <AnimationTools/SkeletonImport.h>
#include <AnimationTools/SkeletonMergeApply.h>

#include <Animation/Skeleton.generated.h>
#include <Base/Compiler.h>
#include <Core/Text/NamePool.h>
#include <Object/ObjectRegistry.h>

// 스켈레톤 임포트 서비스다(Docs/Design/MeshAnimationAssets 6.6절 — MA4b).
namespace rocks {
namespace {

// 설정 위반이다(detail = 그 목록 안 위치).
Result<void> Invalid(AssetImportContext& context, uint64 detail,
                     ErrorCode error = ErrorCode::InvalidArgument) noexcept {
    return context.Fail(AssetDiagnosticCode::InvalidSetting, error, detail);
}

// 진단 코드 없는 실패(할당 실패 등 — 원래 오류를 그대로 남긴다)다.
Result<void> Failed(AssetImportContext& context, const Result<void>& result) noexcept {
    return context.Fail(AssetDiagnosticCode::None, result.Error());
}

// 스켈레톤 진단을 문맥 진단으로 옮긴다(위치가 없으면 detail 0).
Result<void> FailSkeleton(AssetImportContext& context, const SkeletonDiagnostic& diagnostic,
                          ErrorCode error, SkeletonDiagnostic* failure) noexcept {
    if (failure != nullptr) {
        *failure = diagnostic;
    }

    return context.Fail(diagnostic.code, error,
                        diagnostic.index == SkeletonNoIndex ? 0 : diagnostic.index);
}

// text를 intern한다(실패는 InvalidSetting과 원래 오류 — 이름 규칙·pool 상한·종료).
Result<Name> Intern(AssetImportContext& context, NamePool& names, StringView text,
                    uint64 detail) noexcept {
    auto name = names.TryIntern(text);
    if (!name) {
        (void)Invalid(context, detail, name.Error());
    }

    return name;
}

// 명세를 Name 값으로 바꾼 것이다(출력을 쓰기 전에 모두 만든다).
struct InternedSpec {
    explicit InternedSpec(Allocator& allocator) noexcept
        : bones(allocator), virtualBones(allocator), sockets(allocator), curves(allocator) {
    }

    BoneHierarchy bones;
    Array<VirtualBone> virtualBones;
    Array<BoneSocket> sockets;
    Array<SkeletonCurveInfo> curves;
};

Result<void> InternBones(AssetImportContext& context, NamePool& names,
                         const SkeletonImportSpec& spec, Allocator& allocator,
                         BoneHierarchy& out) noexcept {
    if (auto reserved = out.bones.TryReserve(spec.bones.Size()); !reserved) {
        return Failed(context, reserved);
    }

    if (auto reserved = out.referencePose.TryReserve(spec.bones.Size()); !reserved) {
        return Failed(context, reserved);
    }

    for (usize index = 0; index < spec.bones.Size(); ++index) {
        const SkeletonImportBone& source = spec.bones[index];
        auto name = Intern(context, names, source.name, index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        BoneInfo bone(allocator);
        bone.name = name.Value();
        bone.parentIndex = source.parentIndex;
#if ROCKS_WITH_EDITOR_DATA
        if (auto assigned = bone.exportName.TryAssign(source.name); !assigned) {
            return Failed(context, assigned);
        }
#endif
        (void)out.bones.TryPushBack(Move(bone));
        (void)out.referencePose.TryPushBack(source.referencePose);
    }

    // 저장 전 정규화(6.4절 3 — 저장 경로가 부른다).
    (void)NormalizeBoneReferencePose(out);
    return Result<void>::Success();
}

Result<void> InternVirtualBones(AssetImportContext& context, NamePool& names,
                                const SkeletonImportSpec& spec, Array<VirtualBone>& out) noexcept {
    if (auto reserved = out.TryReserve(spec.virtualBones.Size()); !reserved) {
        return Failed(context, reserved);
    }

    for (usize index = 0; index < spec.virtualBones.Size(); ++index) {
        const SkeletonImportVirtualBone& source = spec.virtualBones[index];
        auto sourceBone = Intern(context, names, source.sourceBone, index);
        if (!sourceBone) {
            return Result<void>::Failure(sourceBone.Error());
        }

        auto targetBone = Intern(context, names, source.targetBone, index);
        if (!targetBone) {
            return Result<void>::Failure(targetBone.Error());
        }

        auto name = Intern(context, names, source.name, index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        VirtualBone bone;
        bone.sourceBone = sourceBone.Value();
        bone.targetBone = targetBone.Value();
        bone.name = name.Value();
        (void)out.TryPushBack(bone);
    }

    return Result<void>::Success();
}

Result<void> InternSockets(AssetImportContext& context, NamePool& names,
                           const SkeletonImportSpec& spec, Array<BoneSocket>& out) noexcept {
    if (auto reserved = out.TryReserve(spec.sockets.Size()); !reserved) {
        return Failed(context, reserved);
    }

    for (usize index = 0; index < spec.sockets.Size(); ++index) {
        const SkeletonImportSocket& source = spec.sockets[index];
        auto name = Intern(context, names, source.name, index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        auto bone = Intern(context, names, source.boneName, index);
        if (!bone) {
            return Result<void>::Failure(bone.Error());
        }

        BoneSocket socket;
        socket.name = name.Value();
        socket.boneName = bone.Value();
        socket.relativeLocation = source.relativeLocation;
        socket.relativeRotation = source.relativeRotation;
        socket.relativeScale = source.relativeScale;
        socket.forceAlwaysAnimated = source.forceAlwaysAnimated;
        (void)out.TryPushBack(socket);
    }

    return Result<void>::Success();
}

Result<void> InternCurves(AssetImportContext& context, NamePool& names,
                          const SkeletonImportSpec& spec, Allocator& allocator,
                          Array<SkeletonCurveInfo>& out) noexcept {
    if (auto reserved = out.TryReserve(spec.curves.Size()); !reserved) {
        return Failed(context, reserved);
    }

    for (usize index = 0; index < spec.curves.Size(); ++index) {
        const SkeletonImportCurve& source = spec.curves[index];
        auto name = Intern(context, names, source.name, index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        SkeletonCurveInfo curve(allocator);
        curve.name = name.Value();
        curve.isMaterial = source.isMaterial;
        curve.isMorphTarget = source.isMorphTarget;
        curve.maxLod = source.maxLod;
        if (auto reserved = curve.linkedBones.TryReserve(source.linkedBones.Size()); !reserved) {
            return Failed(context, reserved);
        }

        for (StringView linked : source.linkedBones) {
            auto bone = Intern(context, names, linked, index);
            if (!bone) {
                return Result<void>::Failure(bone.Error());
            }

            (void)curve.linkedBones.TryPushBack(bone.Value());
        }

        (void)out.TryPushBack(Move(curve));
    }

    return Result<void>::Success();
}

// existing에 이름(Name 비교)이 같은 항목이 없는 added 소켓만 뒤에 더한다(재임포트 편집 보존).
Result<void> AppendMissingSockets(Array<BoneSocket>& existing, Array<BoneSocket>& added) noexcept {
    const usize before = existing.Size();
    for (BoneSocket& item : added) {
        bool present = false;
        for (usize index = 0; index < before && !present; ++index) {
            present = existing[index].name == item.name;
        }

        if (!present) {
            if (auto pushed = existing.TryPushBack(item); !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

// 가상 본은 이름이나 (원본, 대상) 쌍이 같은 기존 항목이 있으면 건너뛴다(UE AddNewVirtualBone은 같은
// 쌍이면 거짓 — 사용자가 이름을 바꾼 가상 본을 다시 더하지 않는다).
Result<void> AppendMissingVirtualBones(Array<VirtualBone>& existing,
                                       Array<VirtualBone>& added) noexcept {
    const usize before = existing.Size();
    for (VirtualBone& item : added) {
        bool present = false;
        for (usize index = 0; index < before && !present; ++index) {
            const VirtualBone& old = existing[index];
            present = old.name == item.name ||
                      (old.sourceBone == item.sourceBone && old.targetBone == item.targetBone);
        }

        if (!present) {
            if (auto pushed = existing.TryPushBack(item); !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

// 같은 이름의 기존 커브는 머티리얼·morph 표시만 OR로 합치고(UE AccumulateCurveMetaData — 표시는
// 더하기만 한다) 연결 본·maxLod 편집은 둔다. 없는 커브는 뒤에 더한다.
Result<void> MergeCurves(Array<SkeletonCurveInfo>& existing,
                         Array<SkeletonCurveInfo>& added) noexcept {
    const usize before = existing.Size();
    for (SkeletonCurveInfo& item : added) {
        SkeletonCurveInfo* match = nullptr;
        for (usize index = 0; index < before && match == nullptr; ++index) {
            if (existing[index].name == item.name) {
                match = &existing[index];
            }
        }

        if (match != nullptr) {
            match->isMaterial = match->isMaterial || item.isMaterial;
            match->isMorphTarget = match->isMorphTarget || item.isMorphTarget;
            continue;
        }

        if (auto pushed = existing.TryPushBack(Move(item)); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// 명세 본 index의 리타깃 방식이다(명세가 비면 Animation — UE 새 본 기본).
BoneTranslationRetarget SpecMode(const SkeletonImportSpec& spec, usize index) noexcept {
    return spec.retargetModes.IsEmpty() ? BoneTranslationRetarget::Animation
                                        : spec.retargetModes[index];
}

} // namespace

Result<void> ImportSkeleton(AssetImportContext& context, ObjectHandle output,
                            const SkeletonImportSpec& spec, SkeletonDiagnostic* failure) noexcept {
    ObjectRegistry& registry = context.Registry();
    const ObjectAccessContext& access = context.Access();
    NamePool* names = registry.Names();
    if (names == nullptr) {
        return context.Fail(AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    auto outputType = registry.AcquireType(access, output);
    if (!outputType) {
        return context.Fail(AssetDiagnosticCode::None, outputType.Error());
    }

    if (outputType.Value().Id() != TypeIdOf<Skeleton>()) {
        return context.Fail(AssetDiagnosticCode::TypeMismatch, ErrorCode::TypeMismatch);
    }

    // 본이 없는 원본은 받지 않는다(UE CreateReferenceSkeletonFromMesh도 만들지 않는다 — 재임포트의
    // 병합 판정과 같은 진단).
    if (spec.bones.IsEmpty()) {
        SkeletonDiagnostic empty;
        empty.code = AssetDiagnosticCode::SkeletonMismatch;
        empty.error = ErrorCode::InvalidArgument;
        empty.issue = SkeletonIssue::EmptyMeshHierarchy;
        empty.role = BoneHierarchyRole::Mesh;
        return FailSkeleton(context, empty, ErrorCode::InvalidArgument, failure);
    }

    if (!spec.retargetModes.IsEmpty() && spec.retargetModes.Size() != spec.bones.Size()) {
        return Invalid(context, spec.retargetModes.Size());
    }

    Allocator& allocator = registry.ObjectAllocator();
    InternedSpec interned(allocator);
    if (auto made = InternBones(context, *names, spec, allocator, interned.bones); !made) {
        return made;
    }

    if (auto made = InternVirtualBones(context, *names, spec, interned.virtualBones); !made) {
        return made;
    }

    if (auto made = InternSockets(context, *names, spec, interned.sockets); !made) {
        return made;
    }

    if (auto made = InternCurves(context, *names, spec, allocator, interned.curves); !made) {
        return made;
    }

    auto borrowed = registry.ResolveForWrite<Skeleton>(access, output);
    if (!borrowed) {
        return context.Fail(AssetDiagnosticCode::None, borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return context.Fail(AssetDiagnosticCode::None, value.Error());
    }

    Skeleton& skeleton = *value.Value();
    SkeletonDiagnostic diagnostic;
    if (!context.IsReimport()) {
        // 새 임포트: 명세 값으로 쓰고 보존 필드를 비운다(6.6절 3).
        Array<BoneTranslationRetarget> modes(allocator);
        if (auto reserved = modes.TryReserve(spec.bones.Size()); !reserved) {
            return Failed(context, reserved);
        }

        for (usize index = 0; index < spec.bones.Size(); ++index) {
            (void)modes.TryPushBack(SpecMode(spec, index));
        }

        skeleton.bones = Move(interned.bones);
        skeleton.retargetModes = Move(modes);
        skeleton.virtualBones = Move(interned.virtualBones);
        skeleton.sockets = Move(interned.sockets);
        skeleton.curves = Move(interned.curves);
        skeleton.slotGroups.Clear();
        skeleton.retargetSources.Clear();
        skeleton.compatibleSkeletons.Clear();
        skeleton.useRetargetModesFromCompatibleSkeleton = false;
    } else {
        // 재임포트: 본은 병합(지우지 않는다), 나머지는 편집 보존 또는 명세 값(6.6절 4).
        BoneMergeResult merged(allocator);
        if (auto applied = MergeBonesIntoSkeleton(skeleton, interned.bones, spec.retargetModes,
                                                  merged, &diagnostic);
            !applied) {
            return FailSkeleton(context, diagnostic, applied.Error(), failure);
        }

        Result<void> step = Result<void>::Success();
        if (spec.replaceOnReimport) {
            // 원본이 정본: 명세 본의 리타깃 방식은 명세 값(비면 Animation)이다.
            for (usize index = 0; index < merged.meshToMerged.Size(); ++index) {
                skeleton.retargetModes[static_cast<usize>(merged.meshToMerged[index])] =
                    SpecMode(spec, index);
            }

            skeleton.virtualBones = Move(interned.virtualBones);
            skeleton.sockets = Move(interned.sockets);
            skeleton.curves = Move(interned.curves);
        } else {
            step = AppendMissingVirtualBones(skeleton.virtualBones, interned.virtualBones);
            if (step) {
                step = AppendMissingSockets(skeleton.sockets, interned.sockets);
            }

            if (step) {
                step = MergeCurves(skeleton.curves, interned.curves);
            }
        }

        if (!step) {
            return Failed(context, step);
        }
    }

    if (auto valid = ValidateSkeleton(skeleton, *names, allocator, &diagnostic); !valid) {
        return FailSkeleton(context, diagnostic, valid.Error(), failure);
    }

    return Result<void>::Success();
}

} // namespace rocks
