#include "ExchangeImportTestSupport.h"
#include "ExchangeMeshTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <UnrealExchange/ExchangeAnimSequenceSettings.h>
#include <UnrealExchange/ExchangeAnimationClip.h>

// 교환 애니메이션 클립 변환·임포트 시험이다(Docs/Design/UnrealExchange 7.2절 8번 — UX8b): golden
// 기둥 클립 변환(비율·프레임·본 트랙 키 수·커브 플래그·이벤트 트랙·스켈레톤 참조·
// RetargetSourceAsset 경고), 경로 편집으로 커브 열거·SmartAuto·defaultValue·이벤트·동기 마커·
// 변환 커브·EnumMap·PerPlatform·형식 거부, 명령으로 클립 임포트(스켈레톤 Hard 닫힘)·재임포트·A2f
// 거부.
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_animation_clip_tests_file {

namespace {

constexpr StringView ClipPath =
    "/Game/RxGolden/SK_Column/SkeletalMeshes/SK_Column_Anim.SK_Column_Anim";
constexpr StringView SkeletonPath =
    "/Game/RxGolden/SK_Column/SkeletalMeshes/SK_Column_Skeleton.SK_Column_Skeleton";
constexpr StringView ClipAsset =
    "/Game/Unreal/Game/RxGolden/SK_Column/SkeletalMeshes/SK_Column_Anim";
// golden 클립 값이다(30/1, 30프레임 = 키 31개).
constexpr int32 GoldenRate = 30;
constexpr uint32 GoldenFrames = 30;
constexpr usize GoldenKeys = 31;

struct Golden {
    explicit Golden(Allocator& allocator) noexcept
        : bundle(allocator), bytes(allocator), blobs(allocator) {
    }

    ExchangeBundle bundle;
    uint32 clip = ExchangeNoIndex;
    Array<Array<uint8>> bytes;
    Array<Span<const uint8>> blobs;
};

bool OpenGolden(Allocator& allocator, Golden& golden) noexcept {
    StringBuilder<512> directory(allocator);
    ExchangeDiagnostics diagnostics(allocator);
    if (!FixtureSourcePath("Golden.rxbundle", directory) ||
        !ReadExchangeBundle(platform::PhysicalFileSystem(), directory.View(), {}, golden.bundle,
                            diagnostics)) {
        return false;
    }

    golden.clip = FindAsset(golden.bundle, ClipPath);
    return golden.clip != ExchangeNoIndex &&
           LoadBlobs(golden.bundle, golden.clip, golden.bytes, golden.blobs);
}

uint64 RowOf(StringView key) noexcept {
    const Span<const ExchangeSettingRow> rows = ExchangeAnimSequenceSettingRows();
    for (usize index = 0; index < rows.Size(); ++index) {
        if (rows[index].ueKey == key) {
            return index;
        }
    }

    return ExchangeUnknownSettingDetail;
}

usize CountWarnings(const ExchangeAnimationClip& clip, uint64 detail, StringView key) noexcept {
    usize count = 0;
    for (const ExchangeSettingWarning& warning : clip.Warnings()) {
        count += warning.code == AssetDiagnosticCode::UnsupportedSetting &&
                         warning.detail == detail && warning.key == key
                     ? 1
                     : 0;
    }

    return count;
}

struct Attempt {
    explicit Attempt(Allocator& allocator) noexcept : edited(allocator), clip(allocator) {
    }

    EditedAsset edited;
    ExchangeAnimationClip clip;
    ExchangeMeshFailure failure;
    ErrorCode error = ErrorCode::None;
};

void Run(MeshEnv& env, Allocator& allocator, const Golden& golden, Span<const JsonEdit> edits,
         Attempt& attempt, const OrderedMap<String, AssetId>* references = nullptr) noexcept {
    if (!Edit(allocator, golden.bundle.Assets()[golden.clip], golden.blobs.View(), edits,
              attempt.edited)) {
        attempt.error = ErrorCode::InvalidArgument;
        return;
    }

    ExchangeAnimationClipInput input{&attempt.edited.asset, attempt.edited.blobs.View(), references,
                                     &env.objects, env.access};
    auto built = attempt.clip.Build(input, attempt.failure);
    attempt.error = built ? ErrorCode::None : built.Error();
}

} // namespace

// golden 기둥 클립: 원본·커브·이벤트 트랙·스켈레톤 참조·재생 설정, RetargetSourceAsset 경고.
TEST(UnrealExchange_AnimationClipGolden) {
    SystemAllocator allocator;
    {
        MeshEnv env(allocator);
        REQUIRE(env.registered);
        Golden golden(allocator);
        REQUIRE(OpenGolden(allocator, golden));
        OrderedMap<String, AssetId> references(allocator);
        const AssetId skeletonId{Guid{0x81, 0x82}};
        String skeleton(allocator);
        REQUIRE(skeleton.TryAssign(SkeletonPath) && references.Insert(Move(skeleton), skeletonId));
        Attempt attempt(allocator);
        Run(env, allocator, golden, {}, attempt, &references);
        REQUIRE(attempt.error == ErrorCode::None);
        const AnimationClipImportSpec& spec = attempt.clip.Spec();
        CHECK(spec.frameRate.Numerator() == GoldenRate && spec.frameRate.Denominator() == 1 &&
              spec.frameCount == GoldenFrames);
        REQUIRE(!spec.tracks.IsEmpty());
        for (const ClipImportTrack& track : spec.tracks) {
            CHECK(!track.bone.IsEmpty() &&
                  (track.rotation.Size() == GoldenKeys || track.rotation.Size() == 1) &&
                  (track.translation.Size() == GoldenKeys || track.translation.Size() == 1) &&
                  (track.scale.Size() == GoldenKeys || track.scale.Size() == 1));
        }

        REQUIRE(spec.curves.Size() == 1);
        CHECK(spec.curves[0].name == "Bulge" && spec.curves[0].editable &&
              !spec.curves[0].metadata && !spec.curves[0].disabled &&
              spec.curves[0].curve != nullptr && spec.curves[0].curve->KeyCount() >= 2);
        CHECK(spec.transformCurves.IsEmpty() && spec.events.IsEmpty() &&
              spec.syncMarkers.IsEmpty());
        REQUIRE(spec.eventTracks.Size() == 1);
        CHECK(spec.eventTracks[0] == "1");
        CHECK(spec.skeleton == skeletonId && spec.additive.type == AdditiveType::None &&
              spec.additive.basePose == AdditiveBasePose::None &&
              spec.additive.basePoseClip == AssetId{} &&
              spec.interpolation == ClipInterpolation::Linear && Near(spec.rateScale, 1.0) &&
              !spec.loop && !spec.rootMotion.enable &&
              spec.rootMotion.rootLock == RootMotionRootLock::RefPose &&
              spec.rootMotion.useNormalizedScale && spec.retargetSource.IsEmpty() &&
              spec.targetFrameRate.Numerator() == GoldenRate && spec.attributeCount == 0 &&
              spec.replaceSettingsOnReimport);
        // RetargetSourceAsset(이름이 아닌 에셋 참조)만 Rocks 필드가 없다.
        CHECK(attempt.clip.Warnings().Size() == 1 &&
              CountWarnings(attempt.clip, RowOf("RetargetSourceAsset"), "RetargetSourceAsset") ==
                  1);

        // 호출 계약: 두 번 부름·클립이 아님.
        ExchangeAnimationClipInput again{&attempt.edited.asset, attempt.edited.blobs.View(),
                                         nullptr, &env.objects, env.access};
        CHECK(attempt.clip.Build(again, attempt.failure).Error() == ErrorCode::InvalidArgument);
        ExchangeAsset other(allocator);
        other.exchangeClass = ExchangeClass::Skeleton;
        ExchangeAnimationClip wrong(allocator);
        ExchangeAnimationClipInput wrongInput{&other, {}, nullptr, &env.objects, env.access};
        CHECK(wrong.Build(wrongInput, attempt.failure).Error() == ErrorCode::InvalidArgument &&
              attempt.failure.code == AssetDiagnosticCode::InvalidSource);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 편집: 커브 열거·SmartAuto·defaultValue·플래그, 이벤트·동기 마커·변환 커브, EnumMap 셋·
// PerPlatform·AttributeCurves, 형식 거부.
TEST(UnrealExchange_AnimationClipEdits) {
    SystemAllocator allocator;
    {
        MeshEnv env(allocator);
        REQUIRE(env.registered);
        Golden golden(allocator);
        REQUIRE(OpenGolden(allocator, golden));
        constexpr StringView Curve =
            R"({"keys": [{"interpMode": "RCIM_Cubic", "tangentMode": "RCTM_SmartAuto",)"
            R"( "tangentWeightMode": "RCTWM_WeightedBoth", "time": 0, "value": 1, "arriveTangent": 0,)"
            R"( "arriveTangentWeight": 0, "leaveTangent": 0, "leaveTangentWeight": 0},)"
            R"( {"interpMode": "RCIM_Constant", "tangentMode": "RCTM_Break",)"
            R"( "tangentWeightMode": "RCTWM_WeightedNone", "time": 1, "value": 2, "arriveTangent": 0,)"
            R"( "arriveTangentWeight": 0, "leaveTangent": 0, "leaveTangentWeight": 0}],)"
            R"( "preInfinityExtrap": "RCCE_Cycle", "postInfinityExtrap": "RCCE_Oscillate",)"
            R"( "defaultValue": 3.5})";
        StringBuilder<> curves(allocator);
        StringBuilder<> transforms(allocator);
        REQUIRE(
            curves.TryAppendFormat(R"([{{"name": "Blink", "flags": 80, "curve": {}}}])", Curve));
        REQUIRE(transforms.TryAppend(R"([{"name": "Ik", "flags": 0, "channels": [)"));
        for (usize channel = 0; channel < 9; ++channel) {
            REQUIRE((channel == 0 || transforms.TryAppend(", ")) && transforms.TryAppend(Curve));
        }

        REQUIRE(transforms.TryAppend("]}]"));
        constexpr StringView Events =
            R"([{"time": 0.5, "duration": 0.25, "track": 0, "name": "Step",)"
            R"( "type": "/Script/Engine.AnimNotify_PlaySound", "triggerChance": 0.5,)"
            R"( "triggerOnDedicatedServer": false, "triggerOnFollower": true,)"
            R"( "properties": {"Sound": null, "VolumeMultiplier": 1}},)"
            R"( {"time": 0.1, "duration": 0, "track": 0, "name": "Mark", "type": null,)"
            R"( "triggerChance": 1, "triggerOnDedicatedServer": true, "triggerOnFollower": false,)"
            R"( "properties": null}])";
        const JsonEdit edits[] = {
            {"data.curves", curves.View()},
            {"data.transformCurves", transforms.View()},
            {"data.events", Events},
            {"data.syncMarkers", R"([{"name": "Left", "time": 0.2, "track": 0}])"},
            {"settings.AdditiveAnimType", R"("AAT_RotationOffsetMeshSpace")"},
            {"settings.RefPoseType", R"("ABPT_AnimFrame")"},
            {"settings.RefFrameIndex", "3"},
            {"settings.RootMotionRootLock", R"("AnimFirstFrame")"},
            {"settings.bEnableRootMotion", "true"},
            {"settings.Interpolation", R"("Step")"},
            {"settings.RateScale", "2"},
            {"settings.bLoop", "true"},
            {"settings.RetargetSource", R"("Hips")"},
            {"settings.RetargetSourceAsset", "null"},
            {"settings.PlatformTargetFrameRate",
             R"({"Default": {"Numerator": 60, "Denominator": 1}, "PerPlatform": [{"key": "Mobile"}]})"},
            {"settings.AttributeCurves", R"([{}, {}])"},
        };
        Attempt attempt(allocator);
        Run(env, allocator, golden, edits, attempt);
        REQUIRE(attempt.error == ErrorCode::None);
        const AnimationClipImportSpec& spec = attempt.clip.Spec();
        REQUIRE(spec.curves.Size() == 1 && spec.transformCurves.Size() == 1);
        const TangentCurve& curve = *spec.curves[0].curve;
        REQUIRE(curve.KeyCount() == 2);
        CHECK(curve.Keys()[0].interp == CurveInterp::Cubic &&
              curve.Keys()[0].tangentMode == CurveTangentMode::Auto &&
              curve.Keys()[0].tangentWeight == CurveTangentWeight::Both &&
              curve.Keys()[1].tangentMode == CurveTangentMode::Break &&
              curve.preExtrapolation == CurveExtrapolation::Cycle &&
              curve.postExtrapolation == CurveExtrapolation::Oscillate &&
              curve.defaultValue.HasValue() && Near(curve.defaultValue.Value(), 3.5));
        // flags 80 = Metadata(0x10) | Disabled(0x40), Editable(0x04) 없음.
        CHECK(!spec.curves[0].editable && spec.curves[0].metadata && spec.curves[0].disabled);
        CHECK(spec.transformCurves[0].channels.Size() == 9 &&
              spec.transformCurves[0].channels[8].KeyCount() == 2 &&
              !spec.transformCurves[0].editable);
        REQUIRE(spec.events.Size() == 2 && spec.syncMarkers.Size() == 1);
        CHECK(spec.events[0].name == "Step" && Near(spec.events[0].time, 0.5) &&
              Near(spec.events[0].duration, 0.25) && Near(spec.events[0].triggerChance, 0.5) &&
              !spec.events[0].triggerOnDedicatedServer && spec.events[0].triggerOnFollower &&
              spec.events[0].droppedProperties == 2 &&
              spec.events[0].type == "/Script/Engine.AnimNotify_PlaySound");
        CHECK(spec.events[1].type.IsEmpty() && spec.events[1].droppedProperties == 0);
        CHECK(spec.syncMarkers[0].name == "Left" && Near(spec.syncMarkers[0].time, 0.2));
        CHECK(spec.additive.type == AdditiveType::MeshSpaceRotation &&
              spec.additive.basePose == AdditiveBasePose::Frame &&
              spec.additive.basePoseFrame == 3 &&
              spec.rootMotion.rootLock == RootMotionRootLock::FirstFrame &&
              spec.rootMotion.enable && spec.interpolation == ClipInterpolation::Step &&
              Near(spec.rateScale, 2.0) && spec.loop && spec.retargetSource == "Hips" &&
              spec.targetFrameRate.Numerator() == 60 && spec.attributeCount == 2);
        CHECK(attempt.clip.Warnings().Size() == 1 &&
              CountWarnings(attempt.clip, RowOf("PlatformTargetFrameRate"),
                            "PlatformTargetFrameRate") == 1);

        // 형식 거부(CorruptBundle — detail).
        StringBuilder<> badCurve(allocator);
        REQUIRE(
            badCurve.TryAppendFormat(R"([{{"name": "Bad", "flags": 0, "curve": {}}}])",
                                     R"({"keys": [], "preInfinityExtrap": "RCCE_Nope",)"
                                     R"( "postInfinityExtrap": "RCCE_Cycle", "defaultValue": 0})"));
        const struct {
            JsonEdit edit;
            uint64 detail;
        } rejects[] = {
            {{"data.frameRate.denominator", "0"}, 0},
            {{"data.frameCount", "-1"}, 0},
            {{"data.boneTracks.0.rotation.components", "3"}, 0},
            {{"data.boneTracks.0.bone", "1"}, 0},
            {{"data.boneTracks", "{}"}, 0},
            {{"data.curves", badCurve.View()}, 0},
            {{"data.curves.0.flags", R"("x")"}, 0},
            {{"data.curves.0.curve.keys.0.interpMode", R"("RCIM_Bogus")"}, 0},
            {{"data.curves.0.curve.keys.0.time", R"("x")"}, 0},
            {{"data.transformCurves", R"([{"name": "T", "flags": 0, "channels": []}])"}, 0},
            {{"data.eventTracks", "[1]"}, 0},
            {{"data.events", R"([{"time": 0, "track": 0, "name": "E", "type": 1}])"}, 0},
            {{"data.syncMarkers", R"([{"name": "M"}])"}, 0},
            {{"settings.RateScale", R"("x")"}, RowOf("RateScale")},
            {{"settings.Interpolation", R"("Cubic")"}, RowOf("Interpolation")},
            {{"settings.PlatformTargetFrameRate",
              R"({"Default": {"Numerator": 0, "Denominator": 1}, "PerPlatform": []})"},
             RowOf("PlatformTargetFrameRate")},
            {{"settings.AttributeCurves", "{}"}, RowOf("AttributeCurves")},
        };
        for (const auto& item : rejects) {
            Attempt rejected(allocator);
            Run(env, allocator, golden, Span<const JsonEdit>(&item.edit, 1), rejected);
            CHECK(rejected.error == ErrorCode::CorruptData &&
                  rejected.failure.code == AssetDiagnosticCode::CorruptBundle &&
                  rejected.failure.detail == item.detail);
            if (rejected.failure.code != AssetDiagnosticCode::CorruptBundle ||
                rejected.failure.detail != item.detail) {
                printf("    clip %.*s: error %d code %s detail %llu\n",
                       static_cast<int>(item.edit.path.ByteCount()), item.edit.path.Data(),
                       static_cast<int>(rejected.error), AssetDiagnosticName(rejected.failure.code),
                       static_cast<unsigned long long>(rejected.failure.detail));
            }
        }

        // EnumMap에 없는 값은 번역 거부(InvalidSource)다.
        const JsonEdit badEnum[] = {{"settings.AdditiveAnimType", R"("AAT_Bogus")"}};
        Attempt translated(allocator);
        Run(env, allocator, golden, badEnum, translated);
        CHECK(translated.error == ErrorCode::CorruptData &&
              translated.failure.code == AssetDiagnosticCode::InvalidSource);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// asset-import-unreal: 클립과 Hard 닫힘의 스켈레톤을 만들고, 재임포트·A2f 클래스 거부.
TEST(UnrealExchangeAnimation_ImportCommand) {
    SystemAllocator allocator;
    {
        REQUIRE(StartExchangeModule());
        Scene scene(allocator, "ux8b-command");
        REQUIRE(scene.ready);
        Tool tool(allocator);
        StringBuilder<256> only(allocator);
        REQUIRE(only.TryAppendFormat("--only={}", ClipPath));
        const StringView run[] = {scene.bundle.View(), only.View(), scene.project.View()};
        CHECK(tool.Unreal(run) == 0);
        CHECK(tool.Out("asset-import-unreal: 2 created, 0 reimported, 0 failed"));
        const StringView info[] = {ClipAsset, scene.project.View()};
        CHECK(tool.Info(info) == 0);
        CHECK(tool.Out("SK_Column_Skeleton") && tool.Out("Bulge"));
        const StringView importInfo[] = {ClipAsset, "--import", scene.project.View()};
        CHECK(tool.Info(importInfo) == 0 &&
              tool.Out("import.importer=unreal_anim_sequence 1 current"));

        CHECK(tool.Info(info) == 0);
        String before(allocator);
        REQUIRE(Keep(tool, before));
        const StringView edit[] = {ClipAsset, "rateScale=3", scene.project.View()};
        CHECK(tool.Set(edit) == 0);
        CHECK(tool.Unreal(run) == 0);
        CHECK(tool.Out("asset-import-unreal: 0 created, 2 reimported, 0 failed"));
        CHECK(tool.Info(info) == 0);
        CHECK(StringView(tool.captured.out.View()) == before.View());

        // 클립 문서를 다른 클래스 임포터(unreal_skeleton)로 주면 클래스 거부다.
        SourceArgs sources(allocator);
        REQUIRE(MakeSourceArgs(allocator, scene.bundle.View(), ClipPath, sources));
        StringBuilder<256> objectPath(allocator);
        REQUIRE(objectPath.TryAppendFormat("--objectPath={}", ClipPath));
        const StringView wrong[] = {"/Game/X/Wrong", scene.project.View(), sources.texts[0].View(),
                                    "--importer=unreal_skeleton", objectPath.View()};
        CHECK(tool.Import(wrong) == 1 && tool.Error("InvalidSource"));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_animation_clip_tests_file
