#include <AnimationTools/AnimationClipCook.h>

#include <Animation/AnimationClip.generated.h>
#include <Animation/ClipSource.h>
#include <Animation/Skeleton.generated.h>
#include <AssetCooker/CookBulkSource.h>
#include <AssetRuntime/MemoryBulkSource.h>
#include <Core/Text/NamePool.h>
#include <Object/ObjectRegistry.h>

// 애니메이션 클립 쿠킹 처리기다(Docs/Design/MeshAnimationAssets 7.6.1절 3 — MA6b1).
namespace rocks {
// 유니티 빌드에서 다른 처리기의 같은 이름 도우미와 겹치지 않게 파일 전용 namespace에 둔다.
namespace animation_clip_cook {

// 실패 진단을 채우고 error로 실패한다.
Result<void> CookFail(const CookContext& context, AssetDiagnosticCode code, ErrorCode error,
                      uint64 detail = 0) noexcept {
    if (context.failure != nullptr) {
        *context.failure = AssetDiagnostic{};
        context.failure->code = error == ErrorCode::OutOfMemory ? AssetDiagnosticCode::None : code;
        context.failure->error = error;
        context.failure->asset = context.entry != nullptr ? context.entry->id : AssetId{};
        context.failure->detail = detail;
    }

    return Result<void>::Failure(error);
}

// 클립 진단을 쿠킹 실패로 옮긴다(detail = index, 없으면 0).
Result<void> ClipCookFail(const CookContext& context, const ClipDiagnostic& diagnostic,
                          ErrorCode error) noexcept {
    const uint64 detail = diagnostic.index == ClipNoIndex ? 0 : diagnostic.index;
    return CookFail(context, diagnostic.code, error, detail);
}

// 처리기 경고를 문맥에 둔다(없으면 버린다).
Result<void> CookWarn(const CookContext& context, AssetDiagnosticCode code,
                      uint32 setting) noexcept {
    if (context.warnings == nullptr) {
        return Result<void>::Success();
    }

    const AssetId asset = context.entry != nullptr ? context.entry->id : AssetId{};
    return context.warnings->TryPushBack(
        CookDiagnostic{code, true, asset, {}, setting, ErrorCode::None});
}

// 에디터 객체의 playback을 비운다(에디터 패키지의 playback은 늘 비어 있다 — 할당하지 않는다).
Result<void> Restore(ObjectRegistry& registry, const ObjectAccessContext& access,
                     ObjectHandle handle) noexcept {
    auto borrowed = registry.ResolveForWrite<AnimationClip>(access, handle);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    AnimationClip& clip = *value.Value();
    ClipPlaybackData empty(clip.playback.trackToBone.GetAllocator());
    clip.playback = Move(empty);
    return Result<void>::Success();
}

} // namespace animation_clip_cook

Result<void> AnimationClipCookProcessor::Cook(CookContext& context) const noexcept {
    using namespace animation_clip_cook;
    Allocator& allocator = *context.allocator;
    ObjectRegistry& registry = *context.registry;
    const ObjectAccessContext& access = context.access;

    // 1. 대표 에셋과 타입.
    auto asset = registry.FindChild(access, context.package, "");
    if (!asset) {
        return CookFail(context, AssetDiagnosticCode::None, asset.Error());
    }

    auto type = registry.AcquireType(access, asset.Value());
    if (!type) {
        return CookFail(context, AssetDiagnosticCode::None, type.Error());
    }

    if (type.Value().Id() != TypeIdOf<AnimationClip>()) {
        return CookFail(context, AssetDiagnosticCode::TypeMismatch, ErrorCode::TypeMismatch);
    }

    NamePool* names = registry.Names();
    if (names == nullptr || context.bulkSource == nullptr) {
        return CookFail(context, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    auto found = names->TryFind(context.profile->id.View());
    const Name profileName = found ? found.Value() : Name{};

    // 2. 검증·스켈레톤·원본 로드·쿠킹 변환(읽기 Borrow 안). objectAllocator는 에디터 객체 배열의
    // 할당기다(playback을 그것으로 만들어야 되돌린 뒤 객체가 쿠킹 할당기를 붙잡지 않는다).
    ClipPlayback playback(allocator);
    ClipBuildReport report(allocator);
    Allocator* objectAllocator = nullptr;
    {
        auto borrowed = registry.Resolve<AnimationClip>(access, asset.Value());
        if (!borrowed) {
            return CookFail(context, AssetDiagnosticCode::None, borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return CookFail(context, AssetDiagnosticCode::None, value.Error());
        }

        const AnimationClip& clip = *value.Value();
        objectAllocator = &clip.playback.trackToBone.GetAllocator();
        ClipDiagnostic diagnostic;
        if (auto valid = ValidateAnimationClip(clip, allocator, &diagnostic); !valid) {
            return ClipCookFail(context, diagnostic, valid.Error());
        }

        const ObjectHandle skeletonHandle = clip.skeleton.Handle();
        if (skeletonHandle.IsNull()) {
            return CookFail(context, AssetDiagnosticCode::MissingDependency,
                            ErrorCode::InvalidArgument);
        }

        auto skeletonBorrow = registry.Resolve<Skeleton>(access, skeletonHandle);
        if (!skeletonBorrow) {
            return CookFail(context, AssetDiagnosticCode::MissingDependency,
                            skeletonBorrow.Error());
        }

        auto skeleton = skeletonBorrow.Value().Get();
        if (!skeleton) {
            return CookFail(context, AssetDiagnosticCode::MissingDependency, skeleton.Error());
        }

        ClipSource source(allocator);
        if (auto loaded =
                LoadClipSource(clip.source, *context.bulkSource, *names, source, &diagnostic);
            !loaded) {
            return ClipCookFail(context, diagnostic, loaded.Error());
        }

        ClipBuildInput input;
        input.source = &source;
        input.curves = clip.source.curves.View();
        input.transformCurves = clip.source.transformCurves.View();
        input.skeleton = skeleton.Value();
        input.targetRate = clip.targetFrameRate.Resolve(profileName);
        input.interpolation = clip.interpolation;
        ++builds_;
        if (auto built = BuildClipPlayback(input, playback, report, &diagnostic); !built) {
            return ClipCookFail(context, diagnostic, built.Error());
        }
    }

    // 3. 경고(원본 트랙 순, 그다음 변환 커브).
    for (const ClipDiagnostic& warning : report.warnings) {
        if (auto warned = CookWarn(context, AssetDiagnosticCode::UnknownBoneTrack, warning.index);
            !warned) {
            return warned;
        }
    }

    if (report.skippedTransformCurves != 0) {
        if (auto warned = CookWarn(context, AssetDiagnosticCode::UnsupportedSetting,
                                   report.skippedTransformCurves);
            !warned) {
            return warned;
        }
    }

    // 4. 쿠킹 데이터를 bulk로 쓰고 playback을 채운 뒤 쓰고, 성공·실패 모두 되돌린다.
    MemoryBulkSource stored(allocator);
    ClipPlaybackData data(*objectAllocator);
    if (auto written = StoreClipPlayback(playback, stored, data); !written) {
        return CookFail(context, AssetDiagnosticCode::None, written.Error());
    }

    auto bytes = stored.ReadBulk(data.tracks);
    if (!bytes) {
        return CookFail(context, AssetDiagnosticCode::None, bytes.Error());
    }

    CookChainedBulkSource bulk(allocator, context.bulkSource);
    auto added = bulk.Add(bytes.Value());
    if (!added) {
        return CookFail(context, AssetDiagnosticCode::None, added.Error());
    }

    data.tracks = added.Value();
    {
        auto borrowed = registry.ResolveForWrite<AnimationClip>(access, asset.Value());
        if (!borrowed) {
            return CookFail(context, AssetDiagnosticCode::None, borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return CookFail(context, AssetDiagnosticCode::None, value.Error());
        }

        value.Value()->playback = Move(data);
    }

    CookContext write = context;
    write.bulkSource = &bulk;
    // bulk 정렬은 프로파일 값이다(스트리밍 덩어리가 아니다 — 0이면 프로파일 값 그대로).
    Result<void> written = WriteCookedPackage(write, 0);
    Result<void> restored = Restore(registry, access, asset.Value());
    if (!written) {
        return written;
    }

    if (!restored) {
        return CookFail(context, AssetDiagnosticCode::None, restored.Error());
    }

    return Result<void>::Success();
}

Result<void> RegisterAnimationClipCookProcessors(void* state, CookProcessorTable& table) noexcept {
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return table.Register(TypeIdOf<AnimationClip>(),
                          *static_cast<const AnimationClipCookProcessor*>(state));
}

} // namespace rocks
