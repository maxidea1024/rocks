#include <AudioMiniaudio/Private/MiniaudioSupport.h>

#include <Audio/AudioOutputDevice.h>
#include <AudioMiniaudio/MiniaudioOutput.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
namespace miniaudio_output_tests_file {

using namespace rocks::audio;
using namespace rocks::audio::miniaudio_support;

// AU2b miniaudio backend 시험이다(Docs/Design/AudioSystem 3.34.6절). 순수 도움 함수는 결정적으로
// 본다. 실장치 시험은 장치가 있을 때만 단언한다(CI에는 장치가 없다 — 열기가 NotFound·Unsupported면
// 그 사실만 남긴다).

namespace {

// 다른 스레드(장치 worker)의 진행을 기다리는 상한이다. 넘으면 실패로 본다(멈춤 검출).
constexpr uint64 ProgressTimeoutNs = 10ull * 1000 * 1000 * 1000;

Result<UniquePtr<AudioOutputBackend>> FakeFactory(Allocator& allocator) noexcept {
    (void)allocator;
    return Result<UniquePtr<AudioOutputBackend>>::Failure(ErrorCode::Unsupported);
}

// 콜백 수만 세는 시험 콜백이다(장치 worker 스레드가 부른다).
class CountingCallback final : public AudioOutputCallback {
public:
    void Fill(const AudioOutputView& view) noexcept override {
        if (view.format == AudioSampleFormat::Float32) {
            memset(view.data, 0, sizeof(float) * view.frames * view.channels);
        }

        calls_.FetchAdd<platform::MemoryOrder::Release>(1);
        // Stop이 돌아온 뒤의 호출은 장벽 위반이다(시험 끝에 단언한다).
        if (stopReturned_.Load<platform::MemoryOrder::SequentiallyConsistent>() != 0) {
            lateCalls_.FetchAdd<platform::MemoryOrder::Relaxed>(1);
        }
    }

    uint64 Calls() const noexcept {
        return calls_.Load<platform::MemoryOrder::Acquire>();
    }

    // Stop이 돌아온 직후 부른다. 그 뒤 Fill은 lateCalls로 센다.
    void MarkStopReturned(bool returned) noexcept {
        stopReturned_.Store<platform::MemoryOrder::SequentiallyConsistent>(returned ? 1 : 0);
    }

    uint64 LateCalls() const noexcept {
        return lateCalls_.Load<platform::MemoryOrder::Relaxed>();
    }

private:
    platform::Atomic<uint64> calls_;
    platform::Atomic<uint32> stopReturned_;
    platform::Atomic<uint64> lateCalls_;
};

template <typename Predicate>
bool WaitForProgress(Predicate&& predicate) noexcept {
    const uint64 deadline = platform::MonotonicNanoseconds() + ProgressTimeoutNs;
    while (!predicate()) {
        if (platform::MonotonicNanoseconds() > deadline) {
            return false;
        }

        platform::YieldThread();
    }

    return true;
}

bool NoDevice(ErrorCode error) noexcept {
    return error == ErrorCode::NotFound || error == ErrorCode::Unsupported;
}

} // namespace

TEST(Audio_Miniaudio_ErrorMapping) {
    CHECK(ErrorCodeFromMaResult(MaSuccess) == ErrorCode::None);
    CHECK(ErrorCodeFromMaResult(MaOutOfMemory) == ErrorCode::OutOfMemory);
    CHECK(ErrorCodeFromMaResult(MaDoesNotExist) == ErrorCode::NotFound);
    CHECK(ErrorCodeFromMaResult(MaNoBackend) == ErrorCode::NotFound);
    CHECK(ErrorCodeFromMaResult(MaNoDevice) == ErrorCode::NotFound);
    CHECK(ErrorCodeFromMaResult(MaBusy) == ErrorCode::Busy);
    // 그 밖(예: MA_ERROR −1, MA_INVALID_ARGS −2, MA_FAILED_TO_INIT_BACKEND −300)은 Unsupported다.
    CHECK(ErrorCodeFromMaResult(-1) == ErrorCode::Unsupported);
    CHECK(ErrorCodeFromMaResult(-2) == ErrorCode::Unsupported);
    CHECK(ErrorCodeFromMaResult(-300) == ErrorCode::Unsupported);
}

TEST(Audio_Miniaudio_CallbackFramesUpperBound) {
    // 같은 rate·모르는 rate는 내부 주기 그대로다.
    CHECK(CallbackFramesUpperBound(256, 48000, 48000) == 256);
    CHECK(CallbackFramesUpperBound(256, 48000, 0) == 256);
    CHECK(CallbackFramesUpperBound(256, 0, 44100) == 256);
    // 256 frame@44.1k 장치, 48k 클라이언트: ceil(278.6) + 1 = 280(링 칸 3이 필요하다).
    CHECK(CallbackFramesUpperBound(256, 48000, 44100) == 280);
    CHECK(ChooseOutputBufferCount(CallbackFramesUpperBound(256, 48000, 44100), 256, 0) == 3);
    // 나누어떨어져도 + 1, 줄어드는 방향도 같은 식이다.
    CHECK(CallbackFramesUpperBound(480, 96000, 48000) == 961);
    CHECK(CallbackFramesUpperBound(480, 44100, 48000) == 442);
    CHECK(CallbackFramesUpperBound(0xFFFFFFFFu, 0xFFFFFFFFu, 1) == 0xFFFFFFFFu);
}

TEST(Audio_Miniaudio_Utf16DeviceId) {
    char out[32];
    // WASAPI endpoint id 모양(ASCII)과 NUL에서 멈춤.
    const char16_t endpoint[] = u"{0.0.0.00000000}.{a1}\0junk";
    CHECK(EncodeUtf16DeviceId(endpoint, sizeof(endpoint) / sizeof(endpoint[0]), Span<char>(out)) ==
          21);
    CHECK(strcmp(out, "{0.0.0.00000000}.{a1}") == 0);

    // 2·3·4 byte UTF-8과 대리 쌍(U+1F50A).
    const char16_t mixed[] = {0x00E9, 0xD55C, 0xD83D, 0xDD0A, 0};
    CHECK(EncodeUtf16DeviceId(mixed, 5, Span<char>(out)) == 2 + 3 + 4);
    CHECK(strcmp(out, "\xC3\xA9\xED\x95\x9C\xF0\x9F\x94\x8A") == 0);

    // 짝 없는 대리 값은 U+FFFD.
    const char16_t lone[] = {0xD800, u'a', 0xDC00, 0};
    CHECK(EncodeUtf16DeviceId(lone, 4, Span<char>(out)) == 7);
    CHECK(strcmp(out, "\xEF\xBF\xBD"
                      "a"
                      "\xEF\xBF\xBD") == 0);

    // units 경계에서 멈춤(NUL 없이 꽉 찬 id), 대리 쌍 앞쪽만 남으면 U+FFFD.
    const char16_t full[] = {u'a', u'b', 0xD83D, 0xDD0A};
    CHECK(EncodeUtf16DeviceId(full, 3, Span<char>(out)) == 5);
    CHECK(strcmp(out, "ab\xEF\xBF\xBD") == 0);

    // 빈 id·들어가지 않는 id는 0이고 out은 빈 문자열이다(잘린 id는 다른 장치일 수 있다).
    const char16_t empty[] = {0};
    CHECK(EncodeUtf16DeviceId(empty, 1, Span<char>(out)) == 0 && out[0] == '\0');
    char small[4];
    CHECK(EncodeUtf16DeviceId(u"abcd", 4, Span<char>(small)) == 0 && small[0] == '\0');
    CHECK(EncodeUtf16DeviceId(u"abc", 3, Span<char>(small)) == 3);
    CHECK(EncodeUtf16DeviceId(nullptr, 3, Span<char>(small)) == 0);
}

TEST(Audio_Miniaudio_TextAndIntegerDeviceId) {
    char out[16];
    const char uid[] = "BuiltInSpeaker\0x";
    CHECK(EncodeTextDeviceId(uid, sizeof(uid), Span<char>(out)) == 14);
    CHECK(strcmp(out, "BuiltInSpeaker") == 0);
    CHECK(EncodeTextDeviceId("abcd", 2, Span<char>(out)) == 2 && strcmp(out, "ab") == 0);
    CHECK(EncodeTextDeviceId("0123456789abcdef", 16, Span<char>(out)) == 0 && out[0] == '\0');
    CHECK(EncodeTextDeviceId("", 1, Span<char>(out)) == 0);

    CHECK(EncodeIntegerDeviceId(0, Span<char>(out)) == 1 && strcmp(out, "0") == 0);
    CHECK(EncodeIntegerDeviceId(42, Span<char>(out)) == 2 && strcmp(out, "42") == 0);
    CHECK(EncodeIntegerDeviceId(-7, Span<char>(out)) == 2 && strcmp(out, "-7") == 0);
    CHECK(EncodeIntegerDeviceId(4294967295ll, Span<char>(out)) == 10 &&
          strcmp(out, "4294967295") == 0);
    char wide[24];
    CHECK(EncodeIntegerDeviceId(NumericLimits<int64>::Min(), Span<char>(wide)) == 20 &&
          strcmp(wide, "-9223372036854775808") == 0);
    char tiny[3];
    CHECK(EncodeIntegerDeviceId(123, Span<char>(tiny)) == 0 && tiny[0] == '\0');
    CHECK(EncodeIntegerDeviceId(12, Span<char>(tiny)) == 2);
}

TEST(Audio_Miniaudio_DeviceNameTruncatesAtCharacter) {
    char out[6];
    CHECK(CopyDeviceName("Speakers", 9, Span<char>(out)) == 5 && strcmp(out, "Speak") == 0);
    // "ab" + 한(3 byte) + "c": 5 byte 칸에서 한 가운데를 자르지 않는다.
    CHECK(CopyDeviceName("ab\xED\x95\x9C"
                         "c",
                         6, Span<char>(out)) == 5 &&
          strcmp(out, "ab\xED\x95\x9C") == 0);
    CHECK(CopyDeviceName("abc\xED\x95\x9C", 6, Span<char>(out)) == 3 && strcmp(out, "abc") == 0);
    CHECK(CopyDeviceName("x", 0, Span<char>(out)) == 0 && out[0] == '\0');
    CHECK(CopyDeviceName(nullptr, 4, Span<char>(out)) == 0 && out[0] == '\0');
    CHECK(CopyDeviceName("abc", 3, Span<char>()) == 0);
}

TEST(Audio_Miniaudio_AllocationBridge) {
    SystemAllocator system;
    void* allocator = &system;
    auto* first = static_cast<unsigned char*>(AllocateForMiniaudio(10, allocator));
    CHECK(first != nullptr);
    CHECK(reinterpret_cast<uptr>(first) % MiniaudioAllocationHeader == 0);
    for (unsigned char index = 0; index < 10; ++index) {
        first[index] = index;
    }

    // 늘림·줄임 모두 앞부분을 보존한다.
    auto* grown = static_cast<unsigned char*>(ReallocateForMiniaudio(first, 100, allocator));
    CHECK(grown != nullptr);
    for (unsigned char index = 0; index < 10; ++index) {
        CHECK(grown[index] == index);
    }

    auto* shrunk = static_cast<unsigned char*>(ReallocateForMiniaudio(grown, 4, allocator));
    CHECK(shrunk != nullptr && shrunk[0] == 0 && shrunk[3] == 3);
    // nullptr realloc은 할당, nullptr free는 아무 일도 하지 않는다.
    void* fresh = ReallocateForMiniaudio(nullptr, 8, allocator);
    CHECK(fresh != nullptr);
    FreeForMiniaudio(fresh, allocator);
    FreeForMiniaudio(shrunk, allocator);
    FreeForMiniaudio(nullptr, allocator);

    // 할당 실패는 nullptr이고 realloc 실패면 옛 블록이 그대로다.
    test::NFailAllocator failing(system);
    // 두 번째 할당(realloc의 새 블록)이 실패한다.
    failing.FailOnAttempt(2);
    void* kept = AllocateForMiniaudio(16, &failing);
    CHECK(kept != nullptr);
    static_cast<unsigned char*>(kept)[0] = 9;
    CHECK(ReallocateForMiniaudio(kept, 32, &failing) == nullptr);
    CHECK(static_cast<unsigned char*>(kept)[0] == 9);
    CHECK(AllocateForMiniaudio(NumericLimits<usize>::Max(), &system) == nullptr);
    FreeForMiniaudio(kept, &failing);
}

TEST(Audio_Miniaudio_RegistryRespectsExistingFactory) {
    AudioOutputBackendRegistry registry;
    CHECK(RegisterMiniaudioOutput(registry));
    CHECK(registry.Factory() == &CreateMiniaudioOutput);
    CHECK(!RegisterMiniaudioOutput(registry));
    UnregisterMiniaudioOutput(registry);
    CHECK(registry.Factory() == nullptr);

    registry.SetFactory(&FakeFactory);
    CHECK(!RegisterMiniaudioOutput(registry));
    UnregisterMiniaudioOutput(registry);
    CHECK(registry.Factory() == &FakeFactory);
}

TEST(Audio_Miniaudio_DefaultDeviceWhenPresent) {
    SystemAllocator allocator;
    auto made = CreateMiniaudioOutput(allocator);
    CHECK(made);
    AudioOutputBackend& backend = *made.Value();
    CHECK(strcmp(backend.Name(), "miniaudio") == 0);
    CHECK(backend.Stop());
    backend.Close();

    AudioOutputRequest request;
    request.sampleRate = 0;
    request.layout = ChannelLayout::Stereo;
    request.callbackFrames = 256;
    CountingCallback callback;
    auto format = backend.Open(request, callback);
    if (!format) {
        // 장치 없음(CI): 원래 코드를 남기고 판정하지 않는다. 출력 장치가 열거되는데 기본 장치를
        // 열지 못했으면 장치 없음이 아니라 회귀다.
        CHECK(NoDevice(format.Error()) || format.Error() == ErrorCode::Busy);
        CHECK(backend.LastNativeError() != 0);
        AudioDeviceInfo present[1];
        auto listed = backend.EnumerateDevices(Span<AudioDeviceInfo>(present));
        CHECK(!listed || listed.Value() == 0);
        return;
    }

    CHECK(format.Value().layout == ChannelLayout::Stereo);
    CHECK(format.Value().sampleFormat == AudioSampleFormat::Float32);
    CHECK(format.Value().sampleRate >= 8000 && format.Value().sampleRate <= 384000);
    CHECK(format.Value().callbackFrames > 0);
    CHECK(strcmp(backend.Name(), "miniaudio") != 0);
    CHECK(backend.Open(request, callback).Error() == ErrorCode::InvalidArgument);

    // 열거: 기본 장치가 하나 이상 있다.
    AudioDeviceInfo devices[8];
    auto listed = backend.EnumerateDevices(Span<AudioDeviceInfo>(devices));
    CHECK(listed && listed.Value() >= 1);
    CHECK(devices[0].id.text[0] != '\0');

    CHECK(backend.Start());
    CHECK(WaitForProgress([&callback]() noexcept { return callback.Calls() >= 2; }));
    CHECK(backend.Stop());
    callback.MarkStopReturned(true);
    // 다시 시작하면 콜백이 이어지고, 두 번째 Stop 뒤에도 늦은 호출이 없다.
    callback.MarkStopReturned(false);
    const uint64 beforeRestart = callback.Calls();
    CHECK(backend.Start());
    CHECK(WaitForProgress(
        [&callback, beforeRestart]() noexcept { return callback.Calls() >= beforeRestart + 2; }));
    CHECK(backend.Stop());
    callback.MarkStopReturned(true);
    CHECK(backend.Stop());
    backend.Close();
    backend.Close();
    CHECK(callback.LateCalls() == 0);
    callback.MarkStopReturned(false);

    // 열거로 얻은 id로 같은 장치를 다시 연다.
    request.device = devices[0].id;
    auto again = backend.Open(request, callback);
    CHECK(again);
    backend.Close();

    // 없는 id는 NotFound다.
    constexpr char Missing[] = "rocks-no-such-device";
    memcpy(request.device.text, Missing, sizeof(Missing));
    CHECK(backend.Open(request, callback).Error() == ErrorCode::NotFound);
}

TEST(Audio_Miniaudio_OutputDeviceUsesBackend) {
    // AudioOutputDevice가 miniaudio backend로 열리거나(장치 있음) Null로 간다(장치 없음 — 원래 오류
    // 보존).
    SystemAllocator allocator;
    auto made = CreateMiniaudioOutput(allocator);
    CHECK(made);
    AudioOutputRequest request;
    request.callbackFrames = 256;
    auto device = AudioOutputDevice::TryOpen(AudioMixerConfig{}, Move(made.Value()), request,
                                             AudioOutputDeviceOptions{}, allocator);
    CHECK(device);
    const AudioOutputDeviceInfo& info = device.Value()->Info();
    if (info.usingNull) {
        CHECK(NoDevice(info.backendError) || info.backendError == ErrorCode::Busy);
        CHECK(info.backendNativeError != 0);
        return;
    }

    CHECK(strcmp(info.backendName, "Null") != 0);
    CHECK(device.Value()->Mixer().Config().sampleRate == info.format.sampleRate);
    const AudioOutputRing& ring = device.Value()->Ring();
    const uint64 initial = ring.WrittenBlocks();
    CHECK(WaitForProgress(
        [&ring, initial]() noexcept { return ring.WrittenBlocks() > initial + 4; }));
    device.Value()->Close();
}

} // namespace miniaudio_output_tests_file
