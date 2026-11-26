#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"

#include <AssetNugget/NuggetCache.h>
#include <AssetNugget/NuggetCacheSettings.h>
#include <AssetNugget/NuggetScc.h>
#include <AssetRuntime/MemoryBulkSource.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <AssetRuntime/PackageLoad.h>
#include <AssetRuntime/PackageReader.h>
#include <Audio/AudioDecoder.h>
#include <AudioAssets/AudioAssetsTypes.h>
#include <AudioAssets/SoundClip.generated.h>
#include <AudioOpus/OpusAudioDecoder.h>
#include <AudioTools/CookResampler.h>
#include <AudioTools/SoundClipCook.h>
#include <Base/NumericLimits.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/NamePool.h>
#include <Object/ConstructionContext.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/ObjectRegistryOptions.h>
#include <Platform/File.h>
#include <TestSupport/Test.h>

// 사운드 clip 쿠킹·쿠킹 재생 시험이다(Docs/Design/AudioSystem 3.37.5절): codec 해석 경계, 쿠킹 배치
// (위치 환산·짝수 반올림·길이 0 → 1·끝 경계·같은 rate 그대로), PCM16·ADPCM·Opus 쿠킹 → 로드 →
// LoadCookedPcm(허용오차), 44.1 kHz Opus의 48 kHz 필드·머리 loop, 결정성, 너겟(미적중·적중 같은
// 패키지 byte, 키에 드는 설정·안 드는 설정, 손상 값 Reject 뒤 다시 만듦, verify 불일치), 실패와
// 에디터 객체 불변(쓰기 실패 포함), LoadCookedPcm 오류(없음·손상·머리 불일치·디코더
// 없음·채널·재사용), 처리기 등록.
using namespace rocks;
namespace sound_clip_cook_tests_file {

using audio::AudioCodecId;

namespace {

constexpr uint32 Rate44 = 44100;
constexpr uint32 Rate48 = 48000;
constexpr uint32 Rate96 = 96000;
constexpr double ToneAmplitude = 0.3;
// 왕복 SNR 하한(에너지 비 = 20 dB — ADPCM·Opus 품질 80 순음).
constexpr double MinimumSignalToNoise = 100.0;
// 시작·끝 과도 응답을 빼는 frame 수다.
constexpr uint32 EdgeFrames = 4800;
constexpr double PerfectSignalToNoise = 1.0e30;
// PCM16 왕복 허용오차(1 LSB)다.
constexpr float Pcm16Lsb = 1.0f / 32768.0f;
constexpr uint32 Int16Bytes = 2;
constexpr float Int16Scale = 32767.0f;

AssetId SoundId(uint64 seed) noexcept {
    return AssetId{Guid{0x534e44434f4f4b00ull, seed}};
}

// 실패하는 쓰기 sink다(쓰기 실패 뒤 되돌리기).
class FailingSink final : public PackageSink {
public:
    Result<void> Append(Span<const uint8>) noexcept override {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    Result<void> WriteAt(uint64, Span<const uint8>) noexcept override {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }
};

struct CookOut {
    explicit CookOut(Allocator& allocator) noexcept : sink(allocator), warnings(allocator) {
    }

    MemoryPackageSink sink;
    AssetDiagnostic failure;
    Array<CookDiagnostic> warnings;
};

struct Loaded {
    RootRef<Object> root;
    ObjectHandle package{};
    ObjectHandle clip{};
};

// 채널마다 다른 주파수의 순음 Int16 원본 byte다(planar LE — SoundClipSource 저장 규칙).
Array<uint8> ToneSource(Allocator& allocator, uint32 channels, uint32 frames,
                        uint32 rate) noexcept {
    Array<uint8> out(allocator);
    CHECK(out.TryResize(static_cast<usize>(channels) * frames * Int16Bytes));
    for (uint32 channel = 0; channel < channels; ++channel) {
        const double frequency = 440.0 + 220.0 * channel;
        for (uint32 frame = 0; frame < frames; ++frame) {
            const double value = ToneAmplitude *
                                 math::Sin(math::TwoPi<double> * frequency * frame / rate) *
                                 Int16Scale;
            const auto sample = static_cast<int16>(value >= 0.0 ? value + 0.5 : value - 0.5);
            const usize at = (static_cast<usize>(channel) * frames + frame) * Int16Bytes;
            out[at] = static_cast<uint8>(static_cast<uint16>(sample) & 0xFFu);
            out[at + 1] = static_cast<uint8>(static_cast<uint16>(sample) >> 8);
        }
    }

    return out;
}

// 쿠킹 패키지 bulk를 읽는다(SoundBulkReader — context = Package).
Result<Span<const uint8>> ReadPackageBulk(void* context, const BulkDataRef& ref) noexcept {
    return static_cast<const Package*>(context)->BulkBytes(ref);
}

// 고정 byte를 돌려준다(손상 payload 시험 — context = Array<uint8>).
Result<Span<const uint8>> ReadFixedBytes(void* context, const BulkDataRef&) noexcept {
    return Result<Span<const uint8>>::Success(static_cast<const Array<uint8>*>(context)->View());
}

double SignalToNoise(const float* reference, const float* decoded, uint64 frames) noexcept {
    double signal = 0.0;
    double noise = 0.0;
    for (uint64 frame = EdgeFrames; frame + EdgeFrames < frames; ++frame) {
        const double difference = static_cast<double>(decoded[frame]) - reference[frame];
        signal += static_cast<double>(reference[frame]) * reference[frame];
        noise += difference * difference;
    }

    return noise > 0.0 ? signal / noise : PerfectSignalToNoise;
}

// SoundClip 타입을 등록한 패키지 정책 Registry 둘(에디터·쿠킹 로드)과 에디터 패키지 하나다.
struct CookScene {
    CookScene() noexcept
        : types(allocator), registration(types, allocator), names(allocator), sourceBulk(allocator),
          sourceBytes(allocator), profile(allocator), entry(allocator) {
        ready = RegisterAudioAssetsTypes(registration).IsOk() && names.Initialize().IsOk();
        RegistryOptions options;
        options.names = &names;
        options.policy.packageNativeType = NativeTypeKey<Package>();
        options.policy.rejectWorldOnly = true;
        auto made = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                       ContextId{0x534e434bull, 1}, options);
        auto other = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                        ContextId{0x534e434bull, 2}, options);
        if (made && other) {
            editor = Move(made.Value());
            cooked = Move(other.Value());
        }

        ready = ready && editor.Get() != nullptr && cooked.Get() != nullptr &&
                static_cast<bool>(profile.id.TryAssign("Windows")) &&
                audio::RegisterBuiltInAudioDecoders(decoders).IsOk() &&
                audio::RegisterOpusAudioDecoder(decoders).IsOk();
    }

    ~CookScene() noexcept {
        package.Reset();
        editor.Reset();
        cooked.Reset();
        CHECK(registration.Unregister());
    }

    ObjectAccessContext EditorAccess() const noexcept {
        return editor->OwnerAccess();
    }

    // 새 에디터 패키지 id에 순음 Int16 원본 clip을 만든다.
    bool Make(uint64 id, uint32 channels, uint32 frames, uint32 rate) noexcept {
        package.Reset();
        asset = SoundId(id);
        auto made = NewPackage(*editor, EditorAccess(), asset, PackageKind::Editor);
        if (!made) {
            return false;
        }

        package = Move(made.Value());
        auto created = editor->NewObject<SoundClip>(EditorAccess(), TypeIdOf<SoundClip>(),
                                                    package.Handle(), "");
        if (!created) {
            return false;
        }

        clip = created.Value().Handle();
        sourceBytes = ToneSource(allocator, channels, frames, rate);
        auto ref = sourceBulk.AddCopy(sourceBytes.View());
        if (!ref) {
            return false;
        }

        const BulkDataRef bulk = ref.Value();
        return Edit([&](SoundClip& value) noexcept {
            value.channelCount = channels;
            value.sampleRate = rate;
            value.frameCount = frames;
            value.source.format = SoundSourceStorage::Int16;
            value.source.bulk = bulk;
            return true;
        });
    }

    template <class Fn>
    bool Edit(Fn&& fn) noexcept {
        auto borrowed = editor->ResolveForWrite<SoundClip>(EditorAccess(), clip);
        if (!borrowed) {
            return false;
        }

        auto value = borrowed.Value().Get();
        return value && fn(*value.Value());
    }

    // 원본 PCM이다(에디터 재생과 같은 변환).
    SharedPtr<SoundPcm> SourcePcm(uint32 channels, uint32 frames, uint32 rate) noexcept {
        auto made = DecodeSoundClipSource(allocator, SoundSourceStorage::Int16, channels, rate,
                                          frames, sourceBytes.View());
        return made ? Move(made.Value()) : SharedPtr<SoundPcm>{};
    }

    Result<void> Cook(const SoundClipCookProcessor& processor, CookOut& out,
                      NuggetCache* nuggets = nullptr, bool verify = false,
                      PackageSink* sink = nullptr) noexcept {
        entry.id = asset;
        CookContext context;
        context.allocator = &allocator;
        context.registry = editor.Get();
        context.access = EditorAccess();
        context.package = package.Handle();
        context.entry = &entry;
        context.profile = &profile;
        context.cookerVersion = 1;
        context.bulkSource = &sourceBulk;
        context.nuggets = nuggets;
        context.verifyNuggets = verify;
        context.sink = sink != nullptr ? sink : &out.sink;
        context.failure = &out.failure;
        context.warnings = &out.warnings;
        return processor.Cook(context);
    }

    bool Load(Span<const uint8> bytes, Loaded& out) noexcept {
        Array<uint8> copy(allocator);
        if (!copy.TryAppend(bytes)) {
            return false;
        }

        PackageLoadOptions options;
        options.read.expectedKind = PackageKind::Cooked;
        auto made = UniquePtr<PackageLoadState>::TryMake(allocator, allocator, *cooked, types,
                                                         cooked->OwnerAccess(), options);
        if (!made) {
            return false;
        }

        UniquePtr<PackageLoadState> state = Move(made.Value());
        if (!state->Prepare(Move(copy), nullptr)) {
            return false;
        }

        ConstructionContext transaction(*cooked, cooked->OwnerAccess());
        PackageLoadState* pointers[1] = {state.Get()};
        ObjectHandle handles[1] = {};
        RootRef<Object> roots[1];
        if (!InstantiatePackages(transaction, Span<PackageLoadState* const>(pointers, 1), nullptr,
                                 Span<ObjectHandle>(handles), nullptr) ||
            !transaction.Commit(Span<ObjectHandle>(handles), Span<RootRef<Object>>(roots))) {
            return false;
        }

        out.root = Move(roots[0]);
        out.package = handles[0];
        auto child = cooked->FindChild(cooked->OwnerAccess(), out.package, "");
        out.clip = child ? child.Value() : ObjectHandle{};
        return !out.clip.IsNull();
    }

    SoundClip* LoadedClip(const Loaded& loaded) noexcept {
        auto borrowed = cooked->ResolveForWrite<SoundClip>(cooked->OwnerAccess(), loaded.clip);
        if (!borrowed) {
            return nullptr;
        }

        auto value = borrowed.Value().Get();
        return value ? value.Value() : nullptr;
    }

    const Package* LoadedPackage(const Loaded& loaded) noexcept {
        auto borrowed = cooked->Resolve<Package>(cooked->OwnerAccess(), loaded.package);
        if (!borrowed) {
            return nullptr;
        }

        auto value = borrowed.Value().Get();
        return value ? value.Value() : nullptr;
    }

    // 쿠킹 → 로드 → LoadCookedPcm(실패하면 null).
    SoundClip* CookAndLoad(const SoundClipCookProcessor& processor, CookOut& out,
                           Loaded& loaded) noexcept {
        if (!Cook(processor, out) || !Load(out.sink.Bytes(), loaded)) {
            return nullptr;
        }

        SoundClip* value = LoadedClip(loaded);
        const Package* owner = LoadedPackage(loaded);
        if (value == nullptr || owner == nullptr ||
            !value->LoadCookedPcm(allocator, &ReadPackageBulk, const_cast<Package*>(owner),
                                  decoders)) {
            return nullptr;
        }

        return value;
    }

    // 쿠킹 clip의 플랫폼 데이터 byte다(사본).
    Array<uint8> PlatformBytes(const Loaded& loaded) noexcept {
        Array<uint8> out(allocator);
        SoundClip* value = LoadedClip(loaded);
        const Package* owner = LoadedPackage(loaded);
        if (value != nullptr && owner != nullptr) {
            auto bytes = owner->BulkBytes(value->platformData);
            if (bytes) {
                CHECK(out.TryAppend(bytes.Value()));
            }
        }

        return out;
    }

    // 에디터 객체의 쿠킹 칸이 기본값이고 형식 필드가 원래 값인지다(3.37.3절 7번 — 쿠킹 뒤 불변).
    bool EditorUntouched(uint32 rate, uint32 frames) noexcept {
        return Edit([&](SoundClip& value) noexcept {
            return value.platformData.IsEmpty() && value.sampleRate == rate &&
                   value.frameCount == frames && value.source.format == SoundSourceStorage::Int16;
        });
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration registration;
    NamePool names;
    UniquePtr<ObjectRegistry> editor;
    UniquePtr<ObjectRegistry> cooked;
    RootRef<Package> package;
    ObjectHandle clip{};
    AssetId asset{};
    MemoryBulkSource sourceBulk;
    Array<uint8> sourceBytes;
    TargetProfile profile;
    AssetIndexEntry entry;
    audio::AudioDecoderTable decoders;
    bool ready = false;
};

// 시험 캐시(실제 디스크 — 임시 프로젝트의 Intermediate/AssetNuggets)다.
struct NuggetScene {
    NuggetScene(Allocator& sceneAllocator, const char* tag) noexcept
        : allocator(sceneAllocator), project(tag), settings(sceneAllocator) {
        ready = project.made &&
                static_cast<bool>(settings.localPath.TryAssign(NuggetDefaultLocalPath)) && Reopen();
    }

    // 새 캐시 객체로 연다(Memory 계층이 비어 디스크만 본다).
    bool Reopen() noexcept {
        cache.Reset();
        auto made = UniquePtr<NuggetCache>::TryMake(allocator, allocator,
                                                    platform::PhysicalFileSystem(), clock);
        if (!made) {
            return false;
        }

        cache = Move(made.Value());
        return static_cast<bool>(cache->Open(settings, project.Root()));
    }

    // key 항목을 bytes 하나로 바꿔 쓴다(Reject 뒤 Put — 같은 키 다른 값의 적중을 만든다).
    bool Replace(const Sha256Digest& key, AssetId member, Span<const uint8> bytes) noexcept {
        NuggetCacheGetResult got = cache->GetSccMember(NuggetBucketAudio, key, member);
        if (got.status == NuggetStatus::Hit) {
            auto rejected = cache->Reject(NuggetBucketAudio, key, got.digest);
            if (!rejected) {
                return false;
            }
        }

        Array<NuggetValue> values(allocator);
        NuggetValue value(allocator);
        value.id = NuggetSccValueId(0, 0);
        if (!value.bytes.TryAppend(bytes) || !values.TryPushBack(Move(value))) {
            return false;
        }

        NuggetCachePut put;
        put.bucket = NuggetBucketAudio;
        put.key = key;
        put.values = values.View();
        const AssetId members[1] = {member};
        const NuggetCachePutResult stored = cache->PutScc(Span<const AssetId>(members), put);
        return stored.status == NuggetStatus::Stored && Reopen();
    }

    Allocator& allocator;
    asset_database_test::TempProject project;
    NuggetCacheSettings settings;
    NuggetSystemClock clock;
    UniquePtr<NuggetCache> cache;
    bool ready = false;
};

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

// 쿠킹 패키지의 bulk 항목 수다(읽기 실패면 최댓값).
usize BulkEntryCount(Allocator& allocator, Span<const uint8> bytes) noexcept {
    PackageReadOptions read;
    read.expectedKind = PackageKind::Cooked;
    auto contents = ReadPackage(allocator, bytes, read, nullptr);
    return contents ? contents.Value().bulkEntries.Size() : NumericLimits<usize>::Max();
}

} // namespace

// PlatformDefault 해석(3.37.2절): 원본 rate의 정수 비교라 44.1 kHz 44099 frame은 ADPCM, 44100은
// Opus다. 명시 형식은 그대로, 범위 밖 값은 None이다.
TEST(SoundClipCook_ResolveCompression) {
    CHECK(ResolveSoundCompression(SoundCompression::PlatformDefault, Rate44, 44099) ==
          AudioCodecId::Adpcm);
    CHECK(ResolveSoundCompression(SoundCompression::PlatformDefault, Rate44, 44100) ==
          AudioCodecId::Opus);
    CHECK(ResolveSoundCompression(SoundCompression::PlatformDefault, Rate48, 24000) ==
          AudioCodecId::Adpcm);
    CHECK(ResolveSoundCompression(SoundCompression::PlatformDefault, Rate48, 96000) ==
          AudioCodecId::Opus);
    CHECK(ResolveSoundCompression(SoundCompression::Pcm, Rate48, 1) == AudioCodecId::Pcm16);
    CHECK(ResolveSoundCompression(SoundCompression::Adpcm, Rate48, 96000) == AudioCodecId::Adpcm);
    CHECK(ResolveSoundCompression(SoundCompression::Opus, Rate48, 1) == AudioCodecId::Opus);
    CHECK(ResolveSoundCompression(static_cast<SoundCompression>(9), Rate48, 1) ==
          AudioCodecId::None);
}

// loop 맞추기(3.37.3절 4번): 결과는 늘 길이 1 이상((0, 0)도 (0, 1) — clip 전체 판정은 호출자가
// 원본 값으로 한다), begin이 끝이면 frames − 1, frames 0은 그대로.
TEST(SoundClipCook_ClampLoop) {
    uint32 begin = 0;
    uint32 end = 0;
    ClampSoundCookedLoop(begin, end, 10);
    CHECK(begin == 0 && end == 1);
    begin = 0;
    end = 0;
    ClampSoundCookedLoop(begin, end, 0);
    CHECK(begin == 0 && end == 0);
    begin = 4;
    end = 4;
    ClampSoundCookedLoop(begin, end, 10);
    CHECK(begin == 4 && end == 5);
    begin = 10;
    end = 10;
    ClampSoundCookedLoop(begin, end, 10);
    CHECK(begin == 9 && end == 10);
    begin = 2;
    end = 12;
    ClampSoundCookedLoop(begin, end, 10);
    CHECK(begin == 2 && end == 10);
}

// 쿠킹 배치(3.37.3절 4번): Opus 96 kHz → 48 kHz의 frame·위치 환산(동률 짝수, 길이 0 → 1, 끝 경계),
// 44.1 kHz → 48 kHz는 CookResampled* 그대로, 48 kHz Opus·44.1 kHz PCM16은 그대로. 잘못된 loop·cue는
// InvalidArgument다.
TEST(SoundClipCook_LayoutConvertsPositions) {
    CookScene scene;
    REQUIRE(scene.ready);
    REQUIRE(scene.Make(1, 1, 9, Rate96));
    SoundCookedLayout layout(scene.allocator);
    REQUIRE(scene.Edit([&](SoundClip& clip) noexcept {
        clip.hasFileLoop = true;
        clip.fileLoopBegin = 3;
        clip.fileLoopEnd = 4;
        clip.loopOverride.enabled = true;
        clip.loopOverride.begin = 8;
        clip.loopOverride.end = 9;
        SoundCuePoint cue(scene.allocator);
        cue.frame = 5;
        CHECK(clip.cues.TryPushBack(Move(cue)));
        return ComputeSoundCookedLayout(clip, AudioCodecId::Opus, layout).IsOk();
    }));
    // 9 × 0.5 = 4.5 → 4(짝수). 파일 loop [1.5, 2) → [2, 2) → [2, 3). override [4, 4.5→4) → begin은
    // frames − 1 = 3, end 4. cue 2.5 → 2.
    CHECK(layout.codec == AudioCodecId::Opus && layout.sampleRate == Rate48 && layout.resampled);
    CHECK(layout.frames == 4);
    CHECK(layout.fileLoopBegin == 2 && layout.fileLoopEnd == 3);
    CHECK(layout.overrideBegin == 3 && layout.overrideEnd == 4);
    CHECK(layout.playLoopBegin == 3 && layout.playLoopEnd == 4);
    REQUIRE(layout.cueFrames.Size() == 1);
    CHECK(layout.cueFrames[0] == 2);

    // 환산 뒤 (0, 0)이 되는 짧은 loop는 clip 전체가 아니라 길이 1이다([0, 1) 96 kHz → [0, 0.5→0)).
    // 원본이 (0, 0)인 활성 override는 clip 전체 그대로다.
    REQUIRE(scene.Make(5, 1, 9, Rate96));
    REQUIRE(scene.Edit([&](SoundClip& clip) noexcept {
        clip.loopOverride.enabled = true;
        clip.loopOverride.begin = 0;
        clip.loopOverride.end = 1;
        return ComputeSoundCookedLayout(clip, AudioCodecId::Opus, layout).IsOk();
    }));
    CHECK(layout.overrideBegin == 0 && layout.overrideEnd == 1);
    CHECK(layout.playLoopBegin == 0 && layout.playLoopEnd == 1);
    REQUIRE(scene.Edit([&](SoundClip& clip) noexcept {
        clip.loopOverride.end = 0;
        return ComputeSoundCookedLayout(clip, AudioCodecId::Opus, layout).IsOk();
    }));
    CHECK(layout.overrideBegin == 0 && layout.overrideEnd == 0);
    CHECK(layout.playLoopBegin == 0 && layout.playLoopEnd == 0);

    // 44.1 kHz → 48 kHz는 CookResampled*와 같다. loop가 없으면 재생 loop는 (0, 0)이다.
    REQUIRE(scene.Make(2, 2, Rate44, Rate44));
    REQUIRE(scene.Edit([&](SoundClip& clip) noexcept {
        SoundCuePoint cue(scene.allocator);
        cue.frame = 12345;
        CHECK(clip.cues.TryPushBack(Move(cue)));
        return ComputeSoundCookedLayout(clip, AudioCodecId::Opus, layout).IsOk();
    }));
    auto frames = audio::CookResampledFrames(Rate44, Rate44, Rate48);
    auto cue = audio::CookResampledPosition(12345, Rate44, Rate48);
    REQUIRE(frames && cue);
    CHECK(layout.frames == frames.Value() && layout.cueFrames[0] == cue.Value());
    CHECK(layout.playLoopBegin == 0 && layout.playLoopEnd == 0);

    // 같은 rate(48 kHz Opus, 44.1 kHz PCM16)는 그대로다.
    REQUIRE(scene.Make(3, 1, 1000, Rate48));
    REQUIRE(scene.Edit([&](SoundClip& clip) noexcept {
        clip.hasFileLoop = true;
        clip.fileLoopBegin = 10;
        clip.fileLoopEnd = 900;
        return ComputeSoundCookedLayout(clip, AudioCodecId::Opus, layout).IsOk();
    }));
    CHECK(!layout.resampled && layout.frames == 1000 && layout.fileLoopBegin == 10 &&
          layout.fileLoopEnd == 900 && layout.playLoopEnd == 900);
    REQUIRE(scene.Make(4, 1, 1000, Rate44));
    REQUIRE(scene.Edit([&](SoundClip& clip) noexcept {
        return ComputeSoundCookedLayout(clip, AudioCodecId::Pcm16, layout).IsOk();
    }));
    CHECK(!layout.resampled && layout.sampleRate == Rate44 && layout.frames == 1000);

    // 잘못된 override·파일 loop·cue, codec None.
    REQUIRE(scene.Edit([&](SoundClip& clip) noexcept {
        clip.loopOverride.enabled = true;
        clip.loopOverride.begin = 5;
        clip.loopOverride.end = 5;
        CHECK(ComputeSoundCookedLayout(clip, AudioCodecId::Pcm16, layout).Error() ==
              ErrorCode::InvalidArgument);
        clip.loopOverride.end = 1001;
        CHECK(ComputeSoundCookedLayout(clip, AudioCodecId::Pcm16, layout).Error() ==
              ErrorCode::InvalidArgument);
        clip.loopOverride.begin = 0;
        clip.loopOverride.end = 0;
        CHECK(ComputeSoundCookedLayout(clip, AudioCodecId::Pcm16, layout).IsOk());
        clip.hasFileLoop = true;
        clip.fileLoopBegin = 7;
        clip.fileLoopEnd = 3;
        CHECK(ComputeSoundCookedLayout(clip, AudioCodecId::Pcm16, layout).Error() ==
              ErrorCode::InvalidArgument);
        clip.hasFileLoop = false;
        SoundCuePoint late(scene.allocator);
        late.frame = 1001;
        CHECK(clip.cues.TryPushBack(Move(late)));
        CHECK(ComputeSoundCookedLayout(clip, AudioCodecId::Pcm16, layout).Error() ==
              ErrorCode::InvalidArgument);
        clip.cues.Clear();
        CHECK(ComputeSoundCookedLayout(clip, AudioCodecId::None, layout).Error() ==
              ErrorCode::InvalidArgument);
        return true;
    }));
}

// PCM16 쿠킹(3.37.5절 AU5b·AU5c): 쿠킹 패키지에 source가 없고 platformData가 있으며 bulk는 하나다.
// LoadCookedPcm 결과는 원본과 1 LSB 안이고 같은 ref는 다시 올리지 않는다. 에디터 객체는 원래
// 값이다.
TEST(SoundClipCook_Pcm16RoundTrip) {
    CookScene scene;
    REQUIRE(scene.ready);
    constexpr uint32 Frames = 3000;
    REQUIRE(scene.Make(10, 2, Frames, Rate44));
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.compression = SoundCompression::Pcm;
        return true;
    }));
    SoundClipCookProcessor processor;
    CookOut out(scene.allocator);
    Loaded loaded;
    SoundClip* cooked = scene.CookAndLoad(processor, out, loaded);
    REQUIRE(cooked != nullptr);
    CHECK(processor.Builds() == 1);
    CHECK(BulkEntryCount(scene.allocator, out.sink.Bytes()) == 1);
    CHECK(cooked->source.format == SoundSourceStorage::None && cooked->source.bulk.IsEmpty());
    CHECK(!cooked->platformData.IsEmpty());
    CHECK(cooked->sampleRate == Rate44 && cooked->frameCount == Frames &&
          cooked->channelCount == 2);
    const SharedPtr<SoundPcm> source = scene.SourcePcm(2, Frames, Rate44);
    REQUIRE(source && cooked->Pcm());
    for (uint32 channel = 0; channel < 2; ++channel) {
        const Span<const float> expected = source->Channel(channel);
        const Span<const float> actual = cooked->Pcm()->Channel(channel);
        float worst = 0.0f;
        for (uint32 frame = 0; frame < Frames; ++frame) {
            worst = math::Max(worst, math::Abs(expected[frame] - actual[frame]));
        }

        CHECK(worst <= Pcm16Lsb);
    }

    const uint64 revision = cooked->PcmRevision();
    const Package* owner = scene.LoadedPackage(loaded);
    REQUIRE(cooked->LoadCookedPcm(scene.allocator, &ReadPackageBulk, const_cast<Package*>(owner),
                                  scene.decoders));
    CHECK(cooked->PcmRevision() == revision);
    CHECK(scene.EditorUntouched(Rate44, Frames));
    CHECK(out.warnings.IsEmpty());
}

// ADPCM(0.5 s PlatformDefault)·Opus(48 kHz 2 s PlatformDefault) 쿠킹 재생은 원본과 SNR 20 dB
// 이상이다(3.24·3.32절 값).
TEST(SoundClipCook_AdpcmAndOpusDecodeWithinTolerance) {
    CookScene scene;
    REQUIRE(scene.ready);
    struct Case {
        uint32 channels;
        uint32 frames;
        uint32 rate;
        AudioCodecId codec;
    };
    const Case cases[] = {{2, Rate48 / 2, Rate48, AudioCodecId::Adpcm},
                          {1, Rate48 * 2, Rate48, AudioCodecId::Opus},
                          {2, Rate48 * 2, Rate48, AudioCodecId::Opus}};
    uint64 id = 20;
    for (const Case& item : cases) {
        REQUIRE(scene.Make(id++, item.channels, item.frames, item.rate));
        SoundClipCookProcessor processor;
        CookOut out(scene.allocator);
        Loaded loaded;
        SoundClip* cooked = scene.CookAndLoad(processor, out, loaded);
        REQUIRE(cooked != nullptr);
        const Array<uint8> bytes = scene.PlatformBytes(loaded);
        auto view = audio::AudioPlatformDataView::Parse(bytes.View());
        REQUIRE(view);
        CHECK(view.Value().Header().codec == item.codec);
        const SharedPtr<SoundPcm> source = scene.SourcePcm(item.channels, item.frames, item.rate);
        REQUIRE(source && cooked->Pcm());
        for (uint32 channel = 0; channel < item.channels; ++channel) {
            CHECK(SignalToNoise(source->Channel(channel).Data(),
                                cooked->Pcm()->Channel(channel).Data(),
                                item.frames) >= MinimumSignalToNoise);
        }
    }
}

// 44.1 kHz Opus(3.37.3절 4·7번): 쿠킹 필드는 48 kHz 위치(파일 loop·override·cue)이고 머리 loop는
// 재생 loop 환산과 같다. 디코드 결과는 같은 쿠킹 리샘플러로 바꾼 원본과 SNR 20 dB 이상이다. 같은
// 입력의 두 번 쿠킹은 같은 패키지 byte다. 에디터 객체(44.1 kHz 위치·cue 이름)는 그대로다.
TEST(SoundClipCook_OpusResamplesAndConvertsPositions) {
    CookScene scene;
    REQUIRE(scene.ready);
    constexpr uint32 Frames = Rate44 * 3 / 2;
    REQUIRE(scene.Make(30, 1, Frames, Rate44));
    REQUIRE(scene.Edit([&](SoundClip& clip) noexcept {
        clip.looping = true;
        clip.hasFileLoop = true;
        clip.fileLoopBegin = 1000;
        clip.fileLoopEnd = 40000;
        clip.loopOverride.enabled = true;
        clip.loopOverride.begin = 2205;
        clip.loopOverride.end = 33075;
        SoundCuePoint cue(scene.allocator);
        cue.id = 7;
        cue.frame = 22050;
        CHECK(cue.name.TryAssign("hit"));
        CHECK(clip.cues.TryPushBack(Move(cue)));
        return true;
    }));
    SoundCookedLayout layout(scene.allocator);
    REQUIRE(scene.Edit([&](SoundClip& clip) noexcept {
        return ComputeSoundCookedLayout(clip, AudioCodecId::Opus, layout).IsOk();
    }));
    SoundClipCookProcessor processor;
    CookOut out(scene.allocator);
    Loaded loaded;
    SoundClip* cooked = scene.CookAndLoad(processor, out, loaded);
    REQUIRE(cooked != nullptr);
    CHECK(cooked->sampleRate == Rate48 && cooked->frameCount == layout.frames);
    CHECK(cooked->fileLoopBegin == layout.fileLoopBegin &&
          cooked->fileLoopEnd == layout.fileLoopEnd);
    CHECK(cooked->loopOverride.enabled && cooked->loopOverride.begin == layout.overrideBegin &&
          cooked->loopOverride.end == layout.overrideEnd);
    REQUIRE(cooked->cues.Size() == 1);
    CHECK(cooked->cues[0].id == 7 && cooked->cues[0].frame == layout.cueFrames[0] &&
          cooked->cues[0].name.View() == StringView("hit"));
    CHECK(cooked->looping);
    const Array<uint8> bytes = scene.PlatformBytes(loaded);
    auto view = audio::AudioPlatformDataView::Parse(bytes.View());
    REQUIRE(view);
    CHECK(view.Value().Header().loopStart == layout.overrideBegin &&
          view.Value().Header().loopEnd == layout.overrideEnd);

    const SharedPtr<SoundPcm> source = scene.SourcePcm(1, Frames, Rate44);
    REQUIRE(source && cooked->Pcm());
    audio::PlanarConstView input;
    input.channelCount = 1;
    input.frameCount = Frames;
    input.channels[0] = source->Channel(0).Data();
    audio::DecodedAudioPcm reference(scene.allocator);
    REQUIRE(audio::ResampleForCooking(input, Rate44, Rate48, scene.allocator, reference));
    REQUIRE(reference.FrameCount() == layout.frames);
    CHECK(SignalToNoise(reference.Channel(0), cooked->Pcm()->Channel(0).Data(), layout.frames) >=
          MinimumSignalToNoise);

    // 재생 스냅숏: PCM 차원이 필드와 맞고(머리 대조의 목적) loop는 쿠킹 위치다.
    auto playable =
        cooked->MakePlayable(scene.allocator, *scene.cooked, scene.cooked->OwnerAccess());
    REQUIRE(playable);
    const SoundPlayable* snapshot = playable.Value().Get();
    REQUIRE(snapshot != nullptr && snapshot->pcm);
    CHECK(snapshot->pcm->FrameCount() == layout.frames && snapshot->pcm->SampleRate() == Rate48);
    CHECK(snapshot->looping && snapshot->loopBegin == layout.overrideBegin &&
          snapshot->loopEnd == layout.overrideEnd);

    // 결정성: 다시 쿠킹하면 같은 byte다.
    CookOut again(scene.allocator);
    REQUIRE(scene.Cook(processor, again));
    CHECK(SameBytes(out.sink.Bytes(), again.sink.Bytes()));
    CHECK(processor.Builds() == 2);
    CHECK(scene.Edit([](SoundClip& clip) noexcept {
        return clip.platformData.IsEmpty() && clip.sampleRate == Rate44 &&
               clip.frameCount == Frames && clip.fileLoopBegin == 1000 &&
               clip.loopOverride.begin == 2205 && clip.cues.Size() == 1 &&
               clip.cues[0].frame == 22050 && clip.cues[0].name.View() == StringView("hit");
    }));
}

// 너겟(3.37.5절 AU5b): 미적중은 Put, 다시 연 캐시의 적중은 인코드 없이 같은 쿠킹 패키지 byte다.
// Opus 품질·codec·loop는 새 키, 키에 넣지 않는 설정(looping·volume·cue 이름)은 적중하되 쿠킹
// 패키지에 바뀐 값이 들어간다. PCM16은 품질만 바꾸면 적중이다.
TEST(SoundClipCook_NuggetsHitAndKeyInputs) {
    CookScene scene;
    REQUIRE(scene.ready);
    NuggetScene nuggets(scene.allocator, "sound-cook-nuggets");
    REQUIRE(nuggets.ready);
    REQUIRE(scene.Make(40, 1, Rate44 + 500, Rate44));
    REQUIRE(scene.Edit([&](SoundClip& clip) noexcept {
        clip.hasFileLoop = true;
        clip.fileLoopBegin = 100;
        clip.fileLoopEnd = 30000;
        SoundCuePoint cue(scene.allocator);
        cue.frame = 100;
        CHECK(cue.name.TryAssign("a"));
        return clip.cues.TryPushBack(Move(cue)).IsOk();
    }));
    SoundClipCookProcessor processor;
    CookOut first(scene.allocator);
    REQUIRE(scene.Cook(processor, first, nuggets.cache.Get()));
    CHECK(processor.Builds() == 1 && nuggets.cache->Stats().stored == 1);
    const Sha256Digest firstKey = processor.LastNuggetKey();

    REQUIRE(nuggets.Reopen());
    CookOut second(scene.allocator);
    REQUIRE(scene.Cook(processor, second, nuggets.cache.Get()));
    CHECK(processor.Builds() == 1);
    CHECK(SameBytes(first.sink.Bytes(), second.sink.Bytes()));

    // 키에 넣지 않는 설정(looping·volume·cue 이름·cue 위치·비활성 override 값): 적중이고 쿠킹
    // 패키지에는 바뀐 값(위치는 48 kHz 환산)이 있다.
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.looping = true;
        clip.volume = 0.5f;
        clip.cues[0].frame = 200;
        clip.loopOverride.begin = 50;
        clip.loopOverride.end = 60;
        return static_cast<bool>(clip.cues[0].name.TryAssign("b"));
    }));
    CookOut third(scene.allocator);
    REQUIRE(scene.Cook(processor, third, nuggets.cache.Get()));
    CHECK(processor.Builds() == 1 && processor.LastNuggetKey() == firstKey);
    Loaded loaded;
    REQUIRE(scene.Load(third.sink.Bytes(), loaded));
    SoundClip* cooked = scene.LoadedClip(loaded);
    REQUIRE(cooked != nullptr);
    CHECK(cooked->looping && cooked->volume == 0.5f &&
          cooked->cues[0].name.View() == StringView("b"));
    auto cueAt = audio::CookResampledPosition(200, Rate44, Rate48);
    auto overrideAt = audio::CookResampledPosition(50, Rate44, Rate48);
    auto loopAt = audio::CookResampledPosition(30000, Rate44, Rate48);
    REQUIRE(cueAt && overrideAt && loopAt);
    CHECK(cooked->cues[0].frame == cueAt.Value() &&
          cooked->loopOverride.begin == overrideAt.Value() && !cooked->loopOverride.enabled);
    CHECK(cooked->hasFileLoop && cooked->fileLoopEnd == loopAt.Value());

    // 키에 드는 설정: Opus 품질, codec, loop.
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.compressionQuality = 40;
        return true;
    }));
    CookOut fourth(scene.allocator);
    REQUIRE(scene.Cook(processor, fourth, nuggets.cache.Get()));
    CHECK(processor.Builds() == 2 && !(processor.LastNuggetKey() == firstKey));
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.loopOverride.enabled = true;
        clip.loopOverride.begin = 10;
        clip.loopOverride.end = 2000;
        return true;
    }));
    CookOut fifth(scene.allocator);
    REQUIRE(scene.Cook(processor, fifth, nuggets.cache.Get()));
    CHECK(processor.Builds() == 3);
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.compression = SoundCompression::Pcm;
        return true;
    }));
    CookOut sixth(scene.allocator);
    REQUIRE(scene.Cook(processor, sixth, nuggets.cache.Get()));
    CHECK(processor.Builds() == 4);
    const Sha256Digest pcmKey = processor.LastNuggetKey();

    // PCM16은 품질만 바꾸면 같은 키다.
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.compressionQuality = 90;
        return true;
    }));
    CookOut seventh(scene.allocator);
    REQUIRE(scene.Cook(processor, seventh, nuggets.cache.Get()));
    CHECK(processor.Builds() == 4 && processor.LastNuggetKey() == pcmKey);
    // 패키지 byte는 compressionQuality 필드 값만큼 다르고, 플랫폼 데이터 byte는 같다.
    Loaded sixthLoaded;
    Loaded seventhLoaded;
    REQUIRE(scene.Load(sixth.sink.Bytes(), sixthLoaded));
    REQUIRE(scene.Load(seventh.sink.Bytes(), seventhLoaded));
    const Array<uint8> sixthData = scene.PlatformBytes(sixthLoaded);
    const Array<uint8> seventhData = scene.PlatformBytes(seventhLoaded);
    CHECK(!sixthData.IsEmpty() && SameBytes(sixthData.View(), seventhData.View()));
    const SoundClip* seventhClip = scene.LoadedClip(seventhLoaded);
    CHECK(seventhClip != nullptr && seventhClip->compressionQuality == 90);
}

// 적중 수용 검사(3.37.3절 6번): 손상 값·머리가 layout과 다른 값은 Reject 뒤 다시 만들어 Put한다.
// verifyNuggets는 같은 layout의 다른 byte를 NuggetMismatch로 거부한다.
TEST(SoundClipCook_NuggetRejectAndVerify) {
    CookScene scene;
    REQUIRE(scene.ready);
    NuggetScene nuggets(scene.allocator, "sound-cook-reject");
    REQUIRE(nuggets.ready);
    REQUIRE(scene.Make(50, 1, Rate48 * 2, Rate48));
    SoundClipCookProcessor processor;
    CookOut first(scene.allocator);
    REQUIRE(scene.Cook(processor, first, nuggets.cache.Get()));
    const Sha256Digest key = processor.LastNuggetKey();

    // 손상 값(형식이 아니다).
    const uint8 junk[] = {1, 2, 3, 4};
    REQUIRE(nuggets.Replace(key, scene.asset, Span<const uint8>(junk, sizeof(junk))));
    CookOut second(scene.allocator);
    REQUIRE(scene.Cook(processor, second, nuggets.cache.Get()));
    CHECK(processor.Builds() == 2);
    CHECK(SameBytes(first.sink.Bytes(), second.sink.Bytes()));
    REQUIRE(nuggets.Reopen());
    CookOut third(scene.allocator);
    REQUIRE(scene.Cook(processor, third, nuggets.cache.Get()));
    CHECK(processor.Builds() == 2);

    // 머리가 layout과 다른 값(같은 clip의 PCM16 결과).
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.compression = SoundCompression::Pcm;
        return true;
    }));
    CookOut pcm(scene.allocator);
    REQUIRE(scene.Cook(processor, pcm));
    Loaded pcmLoaded;
    REQUIRE(scene.Load(pcm.sink.Bytes(), pcmLoaded));
    const Array<uint8> pcmBytes = scene.PlatformBytes(pcmLoaded);
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.compression = SoundCompression::PlatformDefault;
        return true;
    }));
    REQUIRE(nuggets.Replace(key, scene.asset, pcmBytes.View()));
    const uint64 builds = processor.Builds();
    CookOut fourth(scene.allocator);
    REQUIRE(scene.Cook(processor, fourth, nuggets.cache.Get()));
    CHECK(processor.Builds() == builds + 1);
    CHECK(SameBytes(first.sink.Bytes(), fourth.sink.Bytes()));
    // 다시 만든 값이 Put됐다(다시 연 캐시가 인코드 없이 적중한다).
    REQUIRE(nuggets.Reopen());
    CookOut fifth(scene.allocator);
    REQUIRE(scene.Cook(processor, fifth, nuggets.cache.Get()));
    CHECK(processor.Builds() == builds + 1);

    // verify 성공: 적중 + 다시 만든 byte가 같음 → 성공, 인코드 한 번, Put 없음.
    const uint64 stored = nuggets.cache->Stats().stored;
    CookOut checked(scene.allocator);
    REQUIRE(scene.Cook(processor, checked, nuggets.cache.Get(), true));
    CHECK(processor.Builds() == builds + 2);
    CHECK(nuggets.cache->Stats().stored == stored);
    CHECK(SameBytes(first.sink.Bytes(), checked.sink.Bytes()));

    // 같은 layout의 다른 byte(품질 40 결과)는 적중으로 받지만 verify가 거부한다.
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.compressionQuality = 40;
        return true;
    }));
    CookOut low(scene.allocator);
    REQUIRE(scene.Cook(processor, low));
    Loaded lowLoaded;
    REQUIRE(scene.Load(low.sink.Bytes(), lowLoaded));
    const Array<uint8> lowBytes = scene.PlatformBytes(lowLoaded);
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.compressionQuality = audio::DefaultAudioCompressionQuality;
        return true;
    }));
    REQUIRE(nuggets.Replace(key, scene.asset, lowBytes.View()));
    CookOut verified(scene.allocator);
    CHECK(scene.Cook(processor, verified, nuggets.cache.Get(), true).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(verified.failure.code == AssetDiagnosticCode::NuggetMismatch);
    CHECK(scene.EditorUntouched(Rate48, Rate48 * 2));
}

// 실패(3.37.3절): 원본 없음·채널 셋·품질·compression 범위 밖·잘못된 loop·손상 원본·쓰기 실패, 모두
// 에디터 객체는 원래 값이다.
TEST(SoundClipCook_FailuresLeaveEditorObject) {
    CookScene scene;
    REQUIRE(scene.ready);
    SoundClipCookProcessor processor;
    const auto expect = [&](AssetDiagnosticCode code, ErrorCode error) noexcept {
        CookOut out(scene.allocator);
        CHECK(scene.Cook(processor, out).Error() == error);
        CHECK(out.failure.code == code);
        CHECK(out.failure.asset == scene.asset);
    };

    REQUIRE(scene.Make(60, 1, 1000, Rate48));
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.source.format = SoundSourceStorage::None;
        return true;
    }));
    expect(AssetDiagnosticCode::InvalidSource, ErrorCode::NotReady);
    REQUIRE(scene.Make(65, 1, 1000, Rate48));
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.source.bulk = BulkDataRef{};
        return true;
    }));
    expect(AssetDiagnosticCode::InvalidSource, ErrorCode::NotReady);

    // 원본 사실 오류: rate 범위 밖은 UnsupportedSourceFormat, 채널 0·frame 0은 InvalidSource.
    REQUIRE(scene.Make(66, 1, 1000, Rate48));
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.sampleRate = audio::MinPlatformSampleRate - 1;
        return true;
    }));
    expect(AssetDiagnosticCode::UnsupportedSourceFormat, ErrorCode::Unsupported);
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.sampleRate = audio::MaxPlatformSampleRate + 1;
        return true;
    }));
    expect(AssetDiagnosticCode::UnsupportedSourceFormat, ErrorCode::Unsupported);
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.sampleRate = Rate48;
        clip.channelCount = 0;
        return true;
    }));
    expect(AssetDiagnosticCode::InvalidSource, ErrorCode::CorruptData);
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.channelCount = 1;
        clip.frameCount = 0;
        return true;
    }));
    expect(AssetDiagnosticCode::InvalidSource, ErrorCode::CorruptData);

    REQUIRE(scene.Make(61, 3, 1000, Rate48));
    expect(AssetDiagnosticCode::UnsupportedSourceFormat, ErrorCode::Unsupported);

    REQUIRE(scene.Make(62, 1, 1000, Rate48));
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.compressionQuality = 0;
        return true;
    }));
    expect(AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument);
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.compressionQuality = audio::MaxAudioCompressionQuality + 1;
        return true;
    }));
    expect(AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument);
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.compressionQuality = audio::DefaultAudioCompressionQuality;
        clip.compression = static_cast<SoundCompression>(9);
        return true;
    }));
    expect(AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument);
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.compression = SoundCompression::Pcm;
        clip.loopOverride.enabled = true;
        clip.loopOverride.begin = 900;
        clip.loopOverride.end = 100;
        return true;
    }));
    expect(AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument);

    // 손상 원본(byte 수가 형식·채널·frame과 다르다).
    REQUIRE(scene.Make(63, 1, 1000, Rate48));
    REQUIRE(scene.Edit([](SoundClip& clip) noexcept {
        clip.frameCount = 1001;
        return true;
    }));
    expect(AssetDiagnosticCode::InvalidSource, ErrorCode::CorruptData);

    // 쓰기 실패 뒤에도 에디터 객체(cue 포함)는 원래 값이다.
    REQUIRE(scene.Make(64, 1, Rate44 * 2, Rate44));
    REQUIRE(scene.Edit([&](SoundClip& clip) noexcept {
        SoundCuePoint cue(scene.allocator);
        cue.frame = 4410;
        CHECK(cue.name.TryAssign("mark"));
        return clip.cues.TryPushBack(Move(cue)).IsOk();
    }));
    FailingSink failing;
    CookOut out(scene.allocator);
    CHECK(scene.Cook(processor, out, nullptr, false, &failing).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(scene.EditorUntouched(Rate44, Rate44 * 2));
    CHECK(scene.Edit([](SoundClip& clip) noexcept {
        return clip.cues.Size() == 1 && clip.cues[0].frame == 4410 &&
               clip.cues[0].name.View() == StringView("mark");
    }));
}

// LoadCookedPcm 오류(3.37.4절): platformData 없음 NotReady, 손상 payload·머리와 필드 불일치
// CorruptData, 디코더 없음 NotFound, 채널 셋 Unsupported. 실패는 앞 PCM을 그대로 둔다.
TEST(SoundClipCook_LoadCookedPcmErrors) {
    CookScene scene;
    REQUIRE(scene.ready);
    REQUIRE(scene.Make(70, 1, Rate48 * 2, Rate48));
    SoundClipCookProcessor processor;
    CookOut out(scene.allocator);
    Loaded loaded;
    REQUIRE(scene.Cook(processor, out) && scene.Load(out.sink.Bytes(), loaded));
    SoundClip* cooked = scene.LoadedClip(loaded);
    REQUIRE(cooked != nullptr);
    Package* owner = const_cast<Package*>(scene.LoadedPackage(loaded));

    // 디코더 없음(Opus 없는 표).
    audio::AudioDecoderTable builtIn;
    REQUIRE(audio::RegisterBuiltInAudioDecoders(builtIn));
    CHECK(cooked->LoadCookedPcm(scene.allocator, &ReadPackageBulk, owner, builtIn).Error() ==
          ErrorCode::NotFound);
    CHECK(!cooked->Pcm());

    // 손상 payload(마지막 byte 하나 — 청크 SHA가 다르다).
    Array<uint8> damaged = scene.PlatformBytes(loaded);
    REQUIRE(!damaged.IsEmpty());
    damaged[damaged.Size() - 1] ^= 0x5Au;
    CHECK(
        cooked->LoadCookedPcm(scene.allocator, &ReadFixedBytes, &damaged, scene.decoders).Error() ==
        ErrorCode::CorruptData);
    // 형식 위반(앞 byte).
    Array<uint8> broken = scene.PlatformBytes(loaded);
    broken[0] ^= 0xFFu;
    CHECK(
        cooked->LoadCookedPcm(scene.allocator, &ReadFixedBytes, &broken, scene.decoders).Error() ==
        ErrorCode::CorruptData);

    // 정상 로드 뒤 머리와 필드가 다르면 CorruptData이고 앞 PCM을 둔다.
    REQUIRE(cooked->LoadCookedPcm(scene.allocator, &ReadPackageBulk, owner, scene.decoders));
    const uint64 revision = cooked->PcmRevision();
    cooked->sampleRate = Rate44;
    CHECK(cooked->LoadCookedPcm(scene.allocator, &ReadPackageBulk, owner, scene.decoders).Error() ==
          ErrorCode::CorruptData);
    CHECK(cooked->PcmRevision() == revision && cooked->Pcm());
    cooked->sampleRate = Rate48;
    cooked->channelCount = 3;
    CHECK(cooked->LoadCookedPcm(scene.allocator, &ReadPackageBulk, owner, scene.decoders).Error() ==
          ErrorCode::Unsupported);
    cooked->channelCount = 1;
    const BulkDataRef ref = cooked->platformData;
    cooked->platformData = BulkDataRef{};
    CHECK(cooked->LoadCookedPcm(scene.allocator, &ReadPackageBulk, owner, scene.decoders).Error() ==
          ErrorCode::NotReady);
    CHECK(cooked->LoadCookedPcm(scene.allocator, nullptr, owner, scene.decoders).Error() ==
          ErrorCode::NotReady);
    cooked->platformData = ref;
    REQUIRE(cooked->LoadCookedPcm(scene.allocator, &ReadPackageBulk, owner, scene.decoders));
    CHECK(cooked->PcmRevision() == revision);
}

// 처리기 등록: null state InvalidArgument, SoundClip은 이 처리기, 두 번 AlreadyExists.
TEST(SoundClipCook_RegisterProcessor) {
    CookScene scene;
    REQUIRE(scene.ready);
    SoundClipCookProcessor processor;
    DefaultCookProcessor fallback;
    CookProcessorTable table(scene.allocator, scene.types, fallback);
    CHECK(RegisterSoundClipCookProcessor(nullptr, table).Error() == ErrorCode::InvalidArgument);
    REQUIRE(RegisterSoundClipCookProcessor(&processor, table));
    CHECK(RegisterSoundClipCookProcessor(&processor, table).Error() == ErrorCode::AlreadyExists);
    auto found = table.Find(TypeIdOf<SoundClip>());
    CHECK(found && found.Value() == &processor);
    auto other = table.Find(TypeIdOf<Package>());
    CHECK(other && other.Value() == &fallback);
}

} // namespace sound_clip_cook_tests_file
