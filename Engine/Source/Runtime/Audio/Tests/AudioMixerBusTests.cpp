#include "AudioTestSupport.h"

#include <Audio/OfflineAudioRender.h>
#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// AU7f1 믹서 버스 시험이다(Docs/Design/AudioSystem 3.45.3–3.45.6·3.45.11절): 버스 gain과 첫 채택,
// pre/post 감쇠 send, send 필터, 버스 연쇄·dry·mono, send level ramp, send 칸 넘침, plan 규칙,
// 칸 재사용, 계약 위반 send, 블록 중간 마스터 gain, 일시정지, 무할당. mono clip의 Direct 패닝은
// 양쪽 0.5다.
using namespace rocks;
namespace audio_mixer_bus_tests_file {

using namespace rocks::audio;

namespace {

constexpr uint32 Block = 256;
constexpr uint32 ShortBlock = 64;
constexpr uint32 ClipFrames = 8192;
constexpr float DirectGain = 0.5f;
constexpr float Tolerance = 1.0e-6f;

struct Signals {
    Signals() noexcept {
        for (uint32 frame = 0; frame < ClipFrames; ++frame) {
            ones[frame] = 1.0f;
            alternating[frame] = frame % 2 == 0 ? 1.0f : -1.0f;
        }
    }

    float ones[ClipFrames];
    float alternating[ClipFrames];
};

AudioMixerConfig ConfigOf(uint32 block = Block, ChannelLayout layout = ChannelLayout::Stereo,
                          uint32 buses = DefaultMaxBuses) noexcept {
    AudioMixerConfig config;
    config.blockFrames = block;
    config.layout = layout;
    config.masterLimiterEnabled = false;
    config.maxVoices = 8;
    config.maxClips = 4;
    config.maxBuses = buses;
    return config;
}

OfflineAudioRender MakeRig(const AudioMixerConfig& config, Allocator& allocator) noexcept {
    auto created = OfflineAudioRender::TryCreate(config, allocator);
    if (!created) {
        platform::Fatal("Audio bus test configuration was rejected");
    }

    return Move(created.Value());
}

AudioClipId RegisterMono(AudioMixer& mixer, const float* samples) noexcept {
    AudioClipDesc desc;
    desc.channels[0] = samples;
    desc.channelCount = 1;
    desc.frameCount = ClipFrames;
    desc.sampleRate = DefaultMixSampleRate;
    auto registered = mixer.RegisterClip(desc);
    if (!registered) {
        platform::Fatal("Audio bus test clip was rejected");
    }

    return registered.Value();
}

AudioBusHandle AcquireOrDie(AudioMixer& mixer) noexcept {
    auto bus = mixer.AcquireBus();
    if (!bus) {
        platform::Fatal("Audio bus test bus was not acquired");
    }

    return bus.Value();
}

AudioBusLink LinkOf(AudioBusHandle bus, AudioBusHandle parent, float output = 1.0f,
                    float dry = 0.0f) noexcept {
    AudioBusLink link;
    link.bus = bus;
    link.parent = parent;
    link.initialParams.outputGain = output;
    link.initialParams.dryGain = dry;
    return link;
}

AudioVoiceSend SendOf(AudioBusHandle bus, float level = 1.0f,
                      AudioSendStage stage = AudioSendStage::PostAttenuation) noexcept {
    AudioVoiceSend send;
    send.bus = bus;
    send.level = level;
    send.stage = stage;
    return send;
}

AudioVoiceRoute RouteOf(const AudioVoiceSend* sends, uint32 count) noexcept {
    AudioVoiceRoute route;
    route.sendCount = count;
    for (uint32 index = 0; index < count; ++index) {
        route.sends[index] = sends[index];
    }

    return route;
}

AudioVoiceRoute RouteTo(AudioBusHandle bus, float level = 1.0f,
                        AudioSendStage stage = AudioSendStage::PostAttenuation) noexcept {
    const AudioVoiceSend send = SendOf(bus, level, stage);
    return RouteOf(&send, 1);
}

void SubmitOrDie(OfflineAudioRender& rig, const AudioRtCommand& command) noexcept {
    if (!rig.Submit(command)) {
        platform::Fatal("Audio bus test command was rejected");
    }
}

AudioVoiceHandle PlayRouted(OfflineAudioRender& rig, AudioClipId clip, const AudioVoiceRoute& route,
                            const AudioVoiceParams& params = AudioVoiceParams{},
                            uint64 frame = 0) noexcept {
    auto voice = rig.Mixer().AcquireVoice();
    if (!voice) {
        platform::Fatal("Audio bus test voice was not acquired");
    }

    auto play = rig.Mixer().PreparePlay(voice.Value(), clip, 0, true, params, route);
    if (!play) {
        platform::Fatal("Audio bus test play was rejected");
    }

    play.Value().mixFrame = frame;
    SubmitOrDie(rig, play.Value());
    return voice.Value();
}

// blocks 블록을 렌더한다(교차 표본).
Array<float> Render(OfflineAudioRender& rig, uint32 blocks, Allocator& allocator) noexcept {
    Array<float> output(allocator);
    const usize floats = static_cast<usize>(blocks) * rig.Mixer().Config().blockFrames *
                         rig.Mixer().OutputChannelCount();
    if (!output.TryResize(floats) ||
        !rig.Render(Span<float>(output.Data(), output.Size()), blocks)) {
        platform::Fatal("Audio bus test render failed");
    }

    return output;
}

float First(const OfflineAudioRender& rig, const Array<float>& output, uint32 frame) noexcept {
    return output[static_cast<usize>(frame) * rig.Mixer().OutputChannelCount()];
}

bool Near(float actual, float expected, float tolerance = Tolerance) noexcept {
    return math::Abs(actual - expected) <= tolerance;
}

} // namespace

// 기본 라우팅(Master, post): 감쇠는 gain과 따로 필터 뒤에 걸린다. 일정한 gain이면 곱과 같다.
TEST(Audio_MixerBus_DefaultRouteSplitsAttenuation) {
    SystemAllocator allocator;
    const Signals signals;
    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioVoiceParams params;
    params.gain = 0.5f;
    params.attenuationGain = 0.5f;
    (void)PlayRouted(rig, RegisterMono(rig.Mixer(), signals.ones), AudioMixer::MasterRoute(),
                     params);
    const Array<float> output = Render(rig, 2, allocator);
    CHECK(First(rig, output, 10) == DirectGain * 0.25f);
    CHECK(First(rig, output, 400) == DirectGain * 0.25f);
    CHECK(rig.Mixer().Counters().activeBuses == 1);
    CHECK(rig.Mixer().Counters().staleBusSends == 0);
}

// 버스 output gain: 새 버스는 첫 채택 블록부터 plan 첫 값을 ramp 없이 쓰고(같은 블록의 Play도 그
// 버스로 간다), 바뀐 값은 한 블록에 걸쳐 선형으로 간다.
TEST(Audio_MixerBus_BusGainFirstBlockAndRamp) {
    SystemAllocator allocator;
    const Signals signals;
    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioBusHandle bus = AcquireOrDie(mixer);
    const AudioBusLink link = LinkOf(bus, AudioMixer::MasterBus(), 0.5f);
    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>(&link, 1)));
    (void)PlayRouted(rig, RegisterMono(mixer, signals.ones), RouteTo(bus));
    Array<float> output = Render(rig, 1, allocator);
    CHECK(First(rig, output, 0) == DirectGain * 0.5f);
    CHECK(First(rig, output, Block - 1) == DirectGain * 0.5f);
    CHECK(mixer.Counters().activeBuses == 2);
    CHECK(mixer.CountersSnapshot().activeBuses == 2);

    AudioBusParams louder;
    louder.outputGain = 1.0f;
    REQUIRE(mixer.PublishBusParams(bus, louder));
    output = Render(rig, 2, allocator);
    for (const uint32 frame : {0u, 64u, 128u, 255u}) {
        const float gain = 0.5f + 0.5f * static_cast<float>(frame) / static_cast<float>(Block);
        CHECK(Near(First(rig, output, frame), DirectGain * gain));
    }

    CHECK(First(rig, output, Block + 10) == DirectGain);
}

// pre send는 감쇠·LPF 앞의 신호다. 같은 voice라도 post는 둘을 거친다(UE PreDistanceAttenuation).
TEST(Audio_MixerBus_PreSendBypassesAttenuationAndFilter) {
    SystemAllocator allocator;
    const Signals signals;
    AudioVoiceParams params;
    params.attenuationGain = 0.25f;
    params.lowPassCutoffHz = 100.0f;
    for (const AudioSendStage stage :
         {AudioSendStage::PreAttenuation, AudioSendStage::PostAttenuation}) {
        OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
        (void)PlayRouted(rig, RegisterMono(rig.Mixer(), signals.alternating),
                         RouteTo(AudioMixer::MasterBus(), 1.0f, stage), params);
        const Array<float> output = Render(rig, 2, allocator);
        const float magnitude = math::Abs(First(rig, output, Block + 44));
        if (stage == AudioSendStage::PreAttenuation) {
            CHECK(magnitude == DirectGain);
        } else {
            CHECK(magnitude < 0.01f);
        }
    }
}

// send 필터는 그 send만 바꾼다(LPF는 교대 신호를, HPF는 DC를 거의 지운다).
TEST(Audio_MixerBus_SendFilterOnlyThatSend) {
    SystemAllocator allocator;
    const Signals signals;
    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioBusHandle filtered = AcquireOrDie(mixer);
    const AudioBusHandle plain = AcquireOrDie(mixer);
    const AudioBusLink links[] = {LinkOf(filtered, AudioMixer::MasterBus()),
                                  LinkOf(plain, AudioMixer::MasterBus())};
    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>(links, 2)));
    AudioVoiceSend sends[] = {SendOf(filtered), SendOf(plain)};
    sends[0].lowPassEnabled = true;
    sends[0].lowPassCutoffHz = MinSendFilterHz;
    const AudioVoiceHandle both =
        PlayRouted(rig, RegisterMono(mixer, signals.alternating), RouteOf(sends, 2));
    Array<float> output = Render(rig, 2, allocator);
    const float sum = math::Abs(First(rig, output, Block + 44));
    CHECK(sum > DirectGain - 0.02f && sum < DirectGain + 0.02f);

    // 필터 send만 남기면 거의 0이다.
    REQUIRE(mixer.PublishRoute(both, RouteOf(sends, 1)));
    output = Render(rig, 2, allocator);
    CHECK(math::Abs(First(rig, output, Block + 44)) < 0.02f);

    // HPF(상한 차단)는 DC를 지운다.
    OfflineAudioRender dc = MakeRig(ConfigOf(), allocator);
    const AudioBusHandle high = AcquireOrDie(dc.Mixer());
    const AudioBusLink highLink = LinkOf(high, AudioMixer::MasterBus());
    REQUIRE(dc.Mixer().PublishBusPlan(Span<const AudioBusLink>(&highLink, 1)));
    AudioVoiceSend highSend = SendOf(high);
    highSend.highPassEnabled = true;
    highSend.highPassCutoffHz = MaxSendFilterHz;
    (void)PlayRouted(dc, RegisterMono(dc.Mixer(), signals.ones), RouteOf(&highSend, 1));
    output = Render(dc, 3, allocator);
    CHECK(math::Abs(First(dc, output, Block * 2 + 100)) < 0.01f);
}

// 자식 → 부모 → Master 연쇄의 gain 곱, dry(UE 조건 — 1e−8 이하는 건너뜀), mono 출력.
TEST(Audio_MixerBus_ChainDryAndMono) {
    SystemAllocator allocator;
    const Signals signals;
    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioBusHandle child = AcquireOrDie(mixer);
    const AudioBusHandle parent = AcquireOrDie(mixer);
    const AudioBusHandle dry = AcquireOrDie(mixer);
    const AudioBusHandle negligible = AcquireOrDie(mixer);
    const AudioBusLink links[] = {LinkOf(child, parent, 0.5f),
                                  LinkOf(parent, AudioMixer::MasterBus(), 0.5f),
                                  LinkOf(dry, AudioMixer::MasterBus(), 1.0f, 0.5f),
                                  LinkOf(negligible, AudioMixer::MasterBus(), 1.0f, 1.0e-9f)};
    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>(links, 4)));
    const AudioClipId clip = RegisterMono(mixer, signals.ones);
    const AudioVoiceHandle voice = PlayRouted(rig, clip, RouteTo(child));
    Array<float> output = Render(rig, 1, allocator);
    CHECK(First(rig, output, 100) == DirectGain * 0.25f);

    REQUIRE(mixer.PublishRoute(voice, RouteTo(dry)));
    output = Render(rig, 2, allocator);
    CHECK(Near(First(rig, output, Block + 100), DirectGain * 1.5f));

    REQUIRE(mixer.PublishRoute(voice, RouteTo(negligible)));
    output = Render(rig, 2, allocator);
    CHECK(First(rig, output, Block + 100) == DirectGain);

    OfflineAudioRender mono = MakeRig(ConfigOf(Block, ChannelLayout::Mono), allocator);
    const AudioBusHandle half = AcquireOrDie(mono.Mixer());
    const AudioBusLink halfLink = LinkOf(half, AudioMixer::MasterBus(), 0.5f);
    REQUIRE(mono.Mixer().PublishBusPlan(Span<const AudioBusLink>(&halfLink, 1)));
    (void)PlayRouted(mono, RegisterMono(mono.Mixer(), signals.ones), RouteTo(half));
    output = Render(mono, 1, allocator);
    CHECK(First(mono, output, 100) == 0.5f);
}

// send 추가·제거·level 변경은 DefaultParamRampFrames로 이어진다. 새 send는 0에서, 빠진 send는
// 0으로.
TEST(Audio_MixerBus_SendLevelChangesRamp) {
    SystemAllocator allocator;
    const Signals signals;
    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioBusHandle first = AcquireOrDie(mixer);
    const AudioBusHandle second = AcquireOrDie(mixer);
    const AudioBusLink links[] = {LinkOf(first, AudioMixer::MasterBus()),
                                  LinkOf(second, AudioMixer::MasterBus())};
    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>(links, 2)));
    const AudioVoiceHandle voice =
        PlayRouted(rig, RegisterMono(mixer, signals.ones), RouteTo(first));
    Array<float> output = Render(rig, 1, allocator);
    CHECK(First(rig, output, 100) == DirectGain);

    const AudioVoiceSend both[] = {SendOf(first, 0.5f), SendOf(second)};
    REQUIRE(mixer.PublishRoute(voice, RouteOf(both, 2)));
    output = Render(rig, 2, allocator);
    const float ramp = static_cast<float>(DefaultParamRampFrames);
    for (const uint32 frame : {0u, 128u, 255u}) {
        const float step = static_cast<float>(frame) / ramp;
        CHECK(Near(First(rig, output, frame), DirectGain * ((1.0f - 0.5f * step) + step)));
    }

    CHECK(Near(First(rig, output, Block + 10), DirectGain * 1.5f));
    REQUIRE(mixer.PublishRoute(voice, RouteTo(second)));
    output = Render(rig, 2, allocator);
    CHECK(Near(First(rig, output, 128), DirectGain * (0.5f * 0.5f + 1.0f)));
    CHECK(Near(First(rig, output, Block + 10), DirectGain));
}

// 빠지는 send까지 MaxVoiceSendSlots를 넘으면 가장 오래 빠지는 중인 칸을 끊고 센다. 일시정지된
// voice의 라우팅 변경은 빠지는 칸을 바로 비운다(끊을 일이 없다).
TEST(Audio_MixerBus_SendSlotOverflowCutsOldest) {
    SystemAllocator allocator;
    const Signals signals;
    OfflineAudioRender rig = MakeRig(ConfigOf(ShortBlock), allocator);
    AudioMixer& mixer = rig.Mixer();
    constexpr uint32 Groups = 3;
    AudioBusHandle buses[Groups * MaxVoiceSends] = {};
    AudioBusLink links[Groups * MaxVoiceSends] = {};
    for (uint32 index = 0; index < Groups * MaxVoiceSends; ++index) {
        buses[index] = AcquireOrDie(mixer);
        links[index] = LinkOf(buses[index], AudioMixer::MasterBus());
    }

    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>(links, Groups * MaxVoiceSends)));
    AudioVoiceRoute routes[Groups];
    for (uint32 group = 0; group < Groups; ++group) {
        routes[group].sendCount = MaxVoiceSends;
        for (uint32 send = 0; send < MaxVoiceSends; ++send) {
            routes[group].sends[send] = SendOf(buses[group * MaxVoiceSends + send], 0.125f);
        }
    }

    const AudioVoiceHandle voice = PlayRouted(rig, RegisterMono(mixer, signals.ones), routes[0]);
    (void)Render(rig, 1, allocator);
    REQUIRE(mixer.PublishRoute(voice, routes[1]));
    (void)Render(rig, 1, allocator);
    REQUIRE(mixer.PublishRoute(voice, routes[2]));
    (void)Render(rig, 1, allocator);
    CHECK(mixer.Counters().cutBusSends == MaxVoiceSends);
    CHECK(mixer.CountersSnapshot().cutBusSends == MaxVoiceSends);

    AudioVoiceParams paused;
    paused.paused = true;
    REQUIRE(mixer.PublishParams(voice, paused));
    (void)Render(rig, 2, allocator);
    REQUIRE(mixer.PublishRoute(voice, routes[0]));
    (void)Render(rig, 1, allocator);
    REQUIRE(mixer.PublishRoute(voice, routes[1]));
    (void)Render(rig, 1, allocator);
    CHECK(mixer.Counters().cutBusSends == MaxVoiceSends);
}

// plan 규칙: 순환·없는 부모·중복·잡지 않은 칸·범위 밖 값·Master 포함 거부, 낡은 handle,
// 미채택 2개면 Busy, 채택 전 ReleaseBus는 Busy, 칸 부족, 설정 범위.
TEST(Audio_MixerBus_PlanRules) {
    SystemAllocator allocator;
    const Signals signals;
    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioBusHandle first = AcquireOrDie(mixer);
    const AudioBusHandle second = AcquireOrDie(mixer);
    const AudioBusHandle master = AudioMixer::MasterBus();
    const AudioBusLink cycle[] = {LinkOf(first, second), LinkOf(second, first)};
    CHECK(mixer.PublishBusPlan(Span<const AudioBusLink>(cycle, 2)).Error() ==
          ErrorCode::InvalidArgument);
    const AudioBusLink orphan = LinkOf(first, second);
    CHECK(mixer.PublishBusPlan(Span<const AudioBusLink>(&orphan, 1)).Error() ==
          ErrorCode::InvalidArgument);
    const AudioBusLink twice[] = {LinkOf(first, master), LinkOf(first, master)};
    CHECK(mixer.PublishBusPlan(Span<const AudioBusLink>(twice, 2)).Error() ==
          ErrorCode::InvalidArgument);
    const AudioBusLink unacquired = LinkOf(AudioBusHandle{5, 1}, master);
    CHECK(mixer.PublishBusPlan(Span<const AudioBusLink>(&unacquired, 1)).Error() ==
          ErrorCode::StaleHandle);
    const AudioBusLink stale = LinkOf(AudioBusHandle{first.index, first.generation + 1}, master);
    CHECK(mixer.PublishBusPlan(Span<const AudioBusLink>(&stale, 1)).Error() ==
          ErrorCode::StaleHandle);
    const AudioBusLink loud = LinkOf(first, master, MaxBusGain * 2.0f);
    CHECK(mixer.PublishBusPlan(Span<const AudioBusLink>(&loud, 1)).Error() ==
          ErrorCode::InvalidArgument);
    const AudioBusLink self = LinkOf(master, master);
    CHECK(mixer.PublishBusPlan(Span<const AudioBusLink>(&self, 1)).Error() ==
          ErrorCode::InvalidArgument);

    // plan에 없는 버스로는 라우팅할 수 없다.
    const AudioClipId clip = RegisterMono(mixer, signals.ones);
    auto voice = mixer.AcquireVoice();
    REQUIRE(voice);
    CHECK(mixer.PreparePlay(voice.Value(), clip, 0, true, AudioVoiceParams{}, RouteTo(first))
              .Error() == ErrorCode::InvalidArgument);
    const AudioVoiceSend duplicate[] = {SendOf(master), SendOf(master)};
    CHECK(mixer.PreparePlay(voice.Value(), clip, 0, true, AudioVoiceParams{}, RouteOf(duplicate, 2))
              .Error() == ErrorCode::InvalidArgument);

    // 미채택 2개까지 게시하고 세 번째는 Busy, 채택 뒤에는 된다.
    const AudioBusLink one = LinkOf(first, master);
    const AudioBusLink two[] = {LinkOf(first, master), LinkOf(second, master)};
    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>(&one, 1)));
    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>(two, 2)));
    CHECK(mixer.PublishBusPlan(Span<const AudioBusLink>(two, 2)).Error() == ErrorCode::Busy);
    (void)Render(rig, 1, allocator);
    CHECK(mixer.AdoptedPlanSerial() == mixer.PublishedPlanSerial());

    // 놓기: Master 거부, 최근 plan에 있으면 Busy, 빠진 plan을 채택하기 전 Busy, 채택 뒤 성공.
    CHECK(mixer.ReleaseBus(master).Error() == ErrorCode::InvalidArgument);
    CHECK(mixer.ReleaseBus(first).Error() == ErrorCode::Busy);
    const AudioBusLink onlySecond = LinkOf(second, master);
    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>(&onlySecond, 1)));
    CHECK(mixer.ReleaseBus(first).Error() == ErrorCode::Busy);
    (void)Render(rig, 1, allocator);
    REQUIRE(mixer.ReleaseBus(first));
    CHECK(mixer.ReleaseBus(first).Error() == ErrorCode::StaleHandle);
    const AudioBusHandle again = AcquireOrDie(mixer);
    CHECK(again.index == first.index && again.generation != first.generation);

    // 칸 부족과 설정 범위.
    OfflineAudioRender small = MakeRig(ConfigOf(Block, ChannelLayout::Stereo, 2), allocator);
    REQUIRE(small.Mixer().AcquireBus());
    CHECK(small.Mixer().AcquireBus().Error() == ErrorCode::CapacityExceeded);
    for (const uint32 buses : {0u, MaxMixerBuses + 1}) {
        CHECK(
            OfflineAudioRender::TryCreate(ConfigOf(Block, ChannelLayout::Stereo, buses), allocator)
                .Error() == ErrorCode::InvalidArgument);
    }
}

// 칸을 다시 쓰면 RT 상태가 새 generation의 첫 값으로 초기화된다(옛 gain에서 ramp하지 않는다).
// 쓰는 voice가 남은 채로 버스를 빼면(계약 위반) 그 send는 건너뛰고 센다.
TEST(Audio_MixerBus_SlotReuseAndStaleSend) {
    SystemAllocator allocator;
    const Signals signals;
    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioClipId clip = RegisterMono(mixer, signals.ones);
    const AudioBusHandle quiet = AcquireOrDie(mixer);
    const AudioBusLink quietLink = LinkOf(quiet, AudioMixer::MasterBus(), 0.25f);
    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>(&quietLink, 1)));
    const AudioVoiceHandle stale = PlayRouted(rig, clip, RouteTo(quiet));
    Array<float> output = Render(rig, 1, allocator);
    CHECK(First(rig, output, 100) == DirectGain * 0.25f);

    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>()));
    output = Render(rig, 1, allocator);
    CHECK(First(rig, output, 100) == 0.0f);
    CHECK(mixer.Counters().staleBusSends > 0);
    CHECK(mixer.CountersSnapshot().staleBusSends == mixer.Counters().staleBusSends);
    auto stop = mixer.MakeStop(stale, mixer.CompletedMixFrame(), 0);
    REQUIRE(stop);
    SubmitOrDie(rig, stop.Value());
    (void)Render(rig, 1, allocator);

    REQUIRE(mixer.ReleaseBus(quiet));
    const AudioBusHandle reused = AcquireOrDie(mixer);
    REQUIRE(reused.index == quiet.index);
    const AudioBusLink loudLink = LinkOf(reused, AudioMixer::MasterBus(), 1.0f);
    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>(&loudLink, 1)));
    (void)PlayRouted(rig, clip, RouteTo(reused), AudioVoiceParams{}, mixer.CompletedMixFrame());
    output = Render(rig, 1, allocator);
    CHECK(First(rig, output, 0) == DirectGain);
}

// PreparePlay 뒤 Play 적용 전에 낸 라우팅은 순번이 뒤라 시작에서 ramp 없이 이긴다. send 필터는 껐다
// 켜면 상태를 새로 만든다(켜는 블록의 첫 표본이 처음 켠 때와 같다).
TEST(Audio_MixerBus_LaterRouteWinsAtStartAndFilterRestarts) {
    SystemAllocator allocator;
    const Signals signals;
    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioBusHandle half = AcquireOrDie(mixer);
    const AudioBusLink link = LinkOf(half, AudioMixer::MasterBus(), 0.5f);
    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>(&link, 1)));
    const AudioClipId clip = RegisterMono(mixer, signals.ones);
    auto voice = mixer.AcquireVoice();
    REQUIRE(voice);
    auto play = mixer.PreparePlay(voice.Value(), clip, 0, true, AudioVoiceParams{},
                                  AudioMixer::MasterRoute());
    REQUIRE(play);
    REQUIRE(mixer.PublishRoute(voice.Value(), RouteTo(half)));
    play.Value().mixFrame = 0;
    SubmitOrDie(rig, play.Value());
    Array<float> output = Render(rig, 1, allocator);
    CHECK(First(rig, output, 0) == DirectGain * 0.5f);

    // 필터를 켜고, 끄고, 다시 켠다. 다시 켠 블록의 출력은 처음 켠 블록과 같다(상태 초기화).
    AudioVoiceSend filtered = SendOf(half);
    filtered.lowPassEnabled = true;
    filtered.lowPassCutoffHz = 200.0f;
    REQUIRE(mixer.PublishRoute(voice.Value(), RouteOf(&filtered, 1)));
    const Array<float> first = Render(rig, 1, allocator);
    REQUIRE(mixer.PublishRoute(voice.Value(), RouteTo(half)));
    (void)Render(rig, 2, allocator);
    REQUIRE(mixer.PublishRoute(voice.Value(), RouteOf(&filtered, 1)));
    const Array<float> again = Render(rig, 1, allocator);
    for (const uint32 frame : {0u, 1u, 10u, 100u}) {
        CHECK(First(rig, again, frame) == First(rig, first, frame));
    }
}

// 블록 중간 SetMasterGain은 버스가 있어도 그 frame부터 걸린다. 장면 일시정지 pause gain은 pre·post
// send 모두에 걸린다.
TEST(Audio_MixerBus_MasterGainAndPauseCoverSends) {
    SystemAllocator allocator;
    const Signals signals;
    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioBusHandle post = AcquireOrDie(mixer);
    const AudioBusHandle pre = AcquireOrDie(mixer);
    const AudioBusLink links[] = {LinkOf(post, AudioMixer::MasterBus()),
                                  LinkOf(pre, AudioMixer::MasterBus())};
    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>(links, 2)));
    const AudioVoiceSend sends[] = {SendOf(post, 0.5f),
                                    SendOf(pre, 0.5f, AudioSendStage::PreAttenuation)};
    const AudioVoiceHandle voice =
        PlayRouted(rig, RegisterMono(mixer, signals.ones), RouteOf(sends, 2));
    auto silence = mixer.MakeSetMasterGain(100, 0.0f, 0);
    REQUIRE(silence);
    SubmitOrDie(rig, silence.Value());
    Array<float> output = Render(rig, 1, allocator);
    CHECK(First(rig, output, 99) == DirectGain);
    CHECK(First(rig, output, 100) == 0.0f);

    auto restore = mixer.MakeSetMasterGain(mixer.CompletedMixFrame(), 1.0f, 0);
    REQUIRE(restore);
    SubmitOrDie(rig, restore.Value());
    AudioVoiceParams paused;
    paused.paused = true;
    REQUIRE(mixer.PublishParams(voice, paused));
    output = Render(rig, 2, allocator);
    CHECK(First(rig, output, PauseFadeFrames + 10) == 0.0f);
    CHECK(First(rig, output, Block + 10) == 0.0f);
}

// 버스·pre send·send 필터가 있는 렌더와 제어 쪽 버스·라우팅 출판은 할당하지 않는다.
TEST(Audio_MixerBus_RenderDoesNotAllocate) {
    SystemAllocator system;
    test::NFailAllocator allocator(system);
    const Signals signals;
    OfflineAudioRender rig = MakeRig(ConfigOf(), allocator);
    AudioMixer& mixer = rig.Mixer();
    const AudioBusHandle child = AcquireOrDie(mixer);
    const AudioBusHandle parent = AcquireOrDie(mixer);
    const AudioClipId clip = RegisterMono(mixer, signals.alternating);
    const AudioBusLink links[] = {LinkOf(child, parent, 0.5f, 0.25f),
                                  LinkOf(parent, AudioMixer::MasterBus())};
    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>(links, 2)));
    AudioVoiceSend sends[] = {SendOf(child), SendOf(parent, 0.5f, AudioSendStage::PreAttenuation)};
    sends[1].lowPassEnabled = true;
    sends[1].lowPassCutoffHz = 1000.0f;
    // 시험 렌더러의 명령 배열은 처음 Submit에서 자라므로 그 뒤부터 센다.
    const AudioVoiceHandle voice = PlayRouted(rig, clip, RouteOf(sends, 2));
    const uint64 before = allocator.Attempts();
    (void)Render(rig, 2, system);
    const AudioBusLink flat[] = {LinkOf(child, AudioMixer::MasterBus()),
                                 LinkOf(parent, AudioMixer::MasterBus())};
    REQUIRE(mixer.PublishBusPlan(Span<const AudioBusLink>(flat, 2)));
    REQUIRE(mixer.PublishRoute(voice, RouteOf(sends, 1)));
    AudioBusParams changed;
    changed.outputGain = 0.75f;
    REQUIRE(mixer.PublishBusParams(parent, changed));
    (void)Render(rig, 2, system);
    CHECK(allocator.Attempts() == before);
}

} // namespace audio_mixer_bus_tests_file
