#include "MiniaudioSupport.h"

#include <AudioMiniaudio/MiniaudioOutput.h>

#include <Audio/AudioFormat.h>
#include <Core/Move.h>
#include <Platform/Atomics.h>
#include <Platform/Memory.h>
#include <Platform/Thread.h>

#include <miniaudio.h>

namespace rocks::audio {

namespace {

using namespace miniaudio_support;

constexpr char UnopenedName[] = "miniaudio";
// ma_context_get_devices가 돌려주는 목록 크기를 넘는 장치는 보지 않는다(장치 선택·열거).
constexpr usize DeviceIdTextBytes = MaxAudioDeviceIdBytes + 1;

using platform::MemoryOrder;

// miniaudio 장치 backend다(3.34.2절). context·device는 Open에서 만들고 Close에서 거둔다.
//
// 콜백 장벽: miniaudio는 비동기 backend(Core Audio·AAudio·OpenSL ES)에서 stop 실패를 삼키고
// ma_device_uninit이 먼저 멈추지 않는다(0.11.26). 그래서 이 backend가 장벽을 직접 둔다. 데이터
// 콜백은 진행 중 수(inFlight_)를 올린 뒤 수락 gate(accepting_)를 보고, 닫혀 있으면 무음만 쓰고
// callback을 부르지 않는다. Stop·Close·Start 실패는 gate를 닫고 진행 중 콜백이 끝날 때까지
// 기다린다(콜백 한 번 길이 이내). 둘 다 seq_cst라 "gate를 연 것을 본 콜백은 진행 중 수에 보인다".
class MiniaudioOutput final : public AudioOutputBackend {
public:
    explicit MiniaudioOutput(Allocator& allocator) noexcept : allocator_(allocator) {
        callbacks_.pUserData = &allocator_;
        callbacks_.onMalloc = &Malloc;
        callbacks_.onRealloc = &Realloc;
        callbacks_.onFree = &Free;
    }

    ~MiniaudioOutput() noexcept override {
        Close();
    }

    MiniaudioOutput(const MiniaudioOutput&) = delete;
    MiniaudioOutput& operator=(const MiniaudioOutput&) = delete;

    Result<AudioOutputFormat> Open(const AudioOutputRequest& request,
                                   AudioOutputCallback& callback) noexcept override {
        if (deviceOpen_) {
            return Result<AudioOutputFormat>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto context = OpenContext(); !context) {
            return Result<AudioOutputFormat>::Failure(context.Error());
        }

        ma_device_id selected;
        const ma_device_id* deviceId = nullptr;
        if (!request.device.IsDefault()) {
            const int32 found = FindDevice(request.device, selected);
            if (found != MaSuccess) {
                CloseContext();
                native_ = found;
                return Result<AudioOutputFormat>::Failure(ErrorCodeFromMaResult(found));
            }

            deviceId = &selected;
        }

        const uint32 channels = ChannelCount(request.layout);
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.pDeviceID = deviceId;
        config.playback.format = ma_format_f32;
        config.playback.channels = channels;
        config.sampleRate = request.sampleRate;
        config.periodSizeInFrames = request.callbackFrames;
        config.performanceProfile = ma_performance_profile_low_latency;
        // 링이 매번 다 채우고(모자라면 무음) 믹서가 clamp한다. 링이 아무 크기나 받으므로
        // miniaudio의 고정 크기 중간 버퍼도 끈다.
        config.noPreSilencedOutputBuffer = MA_TRUE;
        config.noClip = MA_TRUE;
        config.noFixedSizedCallback = MA_TRUE;
        // 장치 교체·재연결은 AU9의 제어 쪽 일이다(3.7.3절). miniaudio의 자동 reroute는 알림
        // 스레드에서 장치를 멈췄다 다시 시작하고 형식을 바꾸며 그 스레드에서 할당하므로 끈다.
        config.wasapi.noAutoStreamRouting = MA_TRUE;
        config.aaudio.noAutoStartAfterReroute = MA_TRUE;
        config.dataCallback = &DataCallback;
        config.pUserData = this;
        callback_ = &callback;
        const ma_result initialized = ma_device_init(&context_, &config, &device_);
        if (initialized != MA_SUCCESS) {
            callback_ = nullptr;
            CloseContext();
            native_ = initialized;
            return Result<AudioOutputFormat>::Failure(ErrorCodeFromMaResult(initialized));
        }

        deviceOpen_ = true;
        AudioOutputFormat format;
        format.sampleRate = device_.sampleRate;
        format.layout = request.layout;
        format.callbackFrames =
            CallbackFramesUpperBound(device_.playback.internalPeriodSizeInFrames,
                                     device_.sampleRate, device_.playback.internalSampleRate);
        format.sampleFormat = AudioSampleFormat::Float32;
        channels_ = channels;
        return Result<AudioOutputFormat>::Success(format);
    }

    Result<void> Start() noexcept override {
        if (!deviceOpen_) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        accepting_.Store<MemoryOrder::SequentiallyConsistent>(1);
        const ma_result started = ma_device_start(&device_);
        if (started != MA_SUCCESS) {
            // 시작 도중 실패해도 OS 스트림이 돌 수 있다(AAudio 상태 대기 시간 초과 등). gate를 닫아
            // callback이 더 불리지 않게 한다.
            CloseGate();
            native_ = started;
            return Result<void>::Failure(ErrorCodeFromMaResult(started));
        }

        return Result<void>::Success();
    }

    // 돌아오면 callback이 진행 중이지 않고 다시 불리지 않는다(gate 장벽). ma_device_stop이 실패하면
    // OS 스트림이 돌 수 있지만 무음만 받는다. 실패는 native 코드와 함께 알린다.
    Result<void> Stop() noexcept override {
        if (!deviceOpen_) {
            return Result<void>::Success();
        }

        ma_result stopped = MA_SUCCESS;
        if (ma_device_get_state(&device_) != ma_device_state_stopped) {
            stopped = ma_device_stop(&device_);
        }

        CloseGate();
        if (stopped != MA_SUCCESS) {
            native_ = stopped;
            return Result<void>::Failure(ErrorCodeFromMaResult(stopped));
        }

        return Result<void>::Success();
    }

    // ma_device_uninit은 0.11.26에서 먼저 멈추지 않으므로 Stop을 먼저 부른다(gate 장벽 포함).
    void Close() noexcept override {
        if (deviceOpen_) {
            (void)Stop();
            ma_device_uninit(&device_);
            deviceOpen_ = false;
        }

        callback_ = nullptr;
        CloseContext();
    }

    Result<uint32> EnumerateDevices(Span<AudioDeviceInfo> out) noexcept override {
        const bool temporary = !contextOpen_;
        if (temporary) {
            if (auto context = OpenContext(); !context) {
                return Result<uint32>::Failure(context.Error());
            }
        }

        ma_device_info* playback = nullptr;
        ma_uint32 count = 0;
        const ma_result listed =
            ma_context_get_devices(&context_, &playback, &count, nullptr, nullptr);
        if (listed != MA_SUCCESS) {
            native_ = listed;
            if (temporary) {
                CloseContext();
            }

            return Result<uint32>::Failure(ErrorCodeFromMaResult(listed));
        }

        // id가 칸에 들어가지 않는 장치는 고를 수 없으므로 내지 않고 전체 수에서도 뺀다.
        uint32 total = 0;
        char text[DeviceIdTextBytes];
        for (ma_uint32 index = 0; index < count; ++index) {
            if (EncodeId(playback[index].id, Span<char>(text)) == 0) {
                continue;
            }

            if (total < out.Size()) {
                AudioDeviceInfo& info = out[total];
                platform::MemoryCopy(info.id.text, text, sizeof(text));
                (void)CopyDeviceName(playback[index].name, sizeof(playback[index].name),
                                     Span<char>(info.name));
                info.isDefault = playback[index].isDefault != MA_FALSE;
            }

            ++total;
        }

        if (temporary) {
            CloseContext();
        }

        return Result<uint32>::Success(total);
    }

    const char* Name() const noexcept override {
        return contextOpen_ ? ma_get_backend_name(context_.backend) : UnopenedName;
    }

    int32 LastNativeError() const noexcept override {
        return native_;
    }

private:
    static void* Malloc(size_t bytes, void* userData) noexcept {
        return AllocateForMiniaudio(bytes, userData);
    }

    static void* Realloc(void* address, size_t bytes, void* userData) noexcept {
        return ReallocateForMiniaudio(address, bytes, userData);
    }

    static void Free(void* address, void* userData) noexcept {
        FreeForMiniaudio(address, userData);
    }

    // 장치 콜백(miniaudio worker·OS 오디오 스레드)이다. 링 복사만 한다(할당·잠금·로그 없음). gate가
    // 닫혀 있으면 무음만 쓴다.
    static void DataCallback(ma_device* device, void* output, const void* input,
                             ma_uint32 frameCount) noexcept {
        (void)input;
        auto& self = *static_cast<MiniaudioOutput*>(device->pUserData);
        self.inFlight_.FetchAdd<MemoryOrder::SequentiallyConsistent>(1);
        if (self.accepting_.Load<MemoryOrder::SequentiallyConsistent>() != 0) {
            AudioOutputView view;
            view.data = output;
            view.frames = frameCount;
            view.channels = self.channels_;
            view.format = AudioSampleFormat::Float32;
            self.callback_->Fill(view);
        } else {
            platform::MemoryZero(output, sizeof(float) * frameCount * self.channels_);
        }

        self.inFlight_.FetchSub<MemoryOrder::Release>(1);
    }

    // gate를 닫고 진행 중 콜백이 끝날 때까지 기다린다(콜백 한 번 길이 이내 — 제어 쪽 대기).
    void CloseGate() noexcept {
        accepting_.Store<MemoryOrder::SequentiallyConsistent>(0);
        platform::SpinBackoff backoff;
        while (inFlight_.Load<MemoryOrder::Acquire>() != 0) {
            backoff.Pause();
        }
    }

    Result<void> OpenContext() noexcept {
        if (contextOpen_) {
            return Result<void>::Success();
        }

        ma_context_config config = ma_context_config_init();
        config.allocationCallbacks = callbacks_;
        const ma_result initialized = ma_context_init(nullptr, 0, &config, &context_);
        if (initialized != MA_SUCCESS) {
            native_ = initialized;
            return Result<void>::Failure(ErrorCodeFromMaResult(initialized));
        }

        contextOpen_ = true;
        return Result<void>::Success();
    }

    void CloseContext() noexcept {
        if (contextOpen_) {
            (void)ma_context_uninit(&context_);
            contextOpen_ = false;
        }
    }

    // 이 context backend의 ma_device_id를 문자열로 옮긴다(3.34.3절 표).
    usize EncodeId(const ma_device_id& id, Span<char> out) const noexcept {
        switch (context_.backend) {
        case ma_backend_wasapi: {
            // ma_wchar_win32는 Windows에서 wchar_t(UTF-16), 그 밖에서는 ma_uint16이다. 단위별로
            // 옮긴다.
            constexpr usize Units = sizeof(id.wasapi) / sizeof(id.wasapi[0]);
            char16_t units[Units];
            for (usize index = 0; index < Units; ++index) {
                units[index] = static_cast<char16_t>(id.wasapi[index]);
            }

            return EncodeUtf16DeviceId(units, Units, out);
        }
        case ma_backend_coreaudio:
            return EncodeTextDeviceId(id.coreaudio, sizeof(id.coreaudio), out);
        case ma_backend_aaudio:
            return EncodeIntegerDeviceId(id.aaudio, out);
        case ma_backend_opensl:
            return EncodeIntegerDeviceId(id.opensl, out);
        default:
            return EncodeTextDeviceId(nullptr, 0, out);
        }
    }

    // 같은 id의 장치를 찾는다. ma_result를 돌려준다(목록 실패는 그 값, 없으면 MaDoesNotExist).
    int32 FindDevice(const AudioDeviceId& wanted, ma_device_id& found) noexcept {
        ma_device_info* playback = nullptr;
        ma_uint32 count = 0;
        const ma_result listed =
            ma_context_get_devices(&context_, &playback, &count, nullptr, nullptr);
        if (listed != MA_SUCCESS) {
            return listed;
        }

        char text[DeviceIdTextBytes];
        for (ma_uint32 index = 0; index < count; ++index) {
            const usize length = EncodeId(playback[index].id, Span<char>(text));
            if (length != 0 && platform::MemoryCompare(text, wanted.text, length + 1) == 0) {
                found = playback[index].id;
                return MaSuccess;
            }
        }

        return MaDoesNotExist;
    }

    Allocator& allocator_;
    ma_allocation_callbacks callbacks_{};
    ma_context context_{};
    ma_device device_{};
    AudioOutputCallback* callback_ = nullptr;
    // 콜백 수락 gate와 진행 중 콜백 수다(클래스 주석의 장벽).
    platform::Atomic<uint32> accepting_;
    platform::Atomic<uint32> inFlight_;
    uint32 channels_ = 0;
    int32 native_ = 0;
    bool contextOpen_ = false;
    bool deviceOpen_ = false;
};

} // namespace

Result<UniquePtr<AudioOutputBackend>> CreateMiniaudioOutput(Allocator& allocator) noexcept {
    auto made = UniquePtr<MiniaudioOutput>::TryMake(allocator, allocator);
    if (!made) {
        return Result<UniquePtr<AudioOutputBackend>>::Failure(made.Error());
    }

    return Result<UniquePtr<AudioOutputBackend>>::Success(
        UniquePtr<AudioOutputBackend>(Move(made.Value())));
}

bool RegisterMiniaudioOutput(AudioOutputBackendRegistry& registry) noexcept {
    if (registry.Factory() != nullptr) {
        return false;
    }

    registry.SetFactory(&CreateMiniaudioOutput);
    return true;
}

void UnregisterMiniaudioOutput(AudioOutputBackendRegistry& registry) noexcept {
    if (registry.Factory() == &CreateMiniaudioOutput) {
        registry.SetFactory(nullptr);
    }
}

} // namespace rocks::audio
