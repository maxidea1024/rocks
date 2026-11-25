#pragma once

#include <Audio/AudioFormat.h>
#include <Base/Compiler.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Module/HostServiceKey.h>
#include <Core/Result.h>

// 오디오 출력 장치 경로의 공개 형식이다(Docs/Design/AudioSystem 3.7·3.33절).
// backend(miniaudio·Null· 플랫폼 직접 구현)는 AudioOutputBackend를 구현하고, 장치 콜백은
// AudioOutputCallback::Fill을 부른다. 공개 헤더에 OS·라이브러리 타입을 두지 않는다.
namespace rocks::audio {

// 장치 표본 형식이다. 믹서는 늘 float이고 변환은 출력 링 복사 한 곳에서 한다(3.33.2절).
enum class AudioSampleFormat : uint8 {
    Float32,
    Int16
};

// 장치 id·이름 칸의 최대 byte 수다(UTF-8, NUL 제외).
inline constexpr usize MaxAudioDeviceIdBytes = 256;
inline constexpr usize MaxAudioDeviceNameBytes = 256;

// backend가 만든 불투명 장치 id다(WASAPI endpoint id 등). 빈 값(text[0] == 0)은 기본 장치다. 고정
// 크기라 할당이 없다.
struct AudioDeviceId {
    char text[MaxAudioDeviceIdBytes + 1] = {};

    bool IsDefault() const noexcept {
        return text[0] == '\0';
    }
};

struct AudioDeviceInfo {
    AudioDeviceId id;
    char name[MaxAudioDeviceNameBytes + 1] = {};
    bool isDefault = false;
};

// 장치 열기 요청이다(3.7.1절).
struct AudioOutputRequest {
    // 0이면 장치 기본 rate다.
    uint32 sampleRate = 0;
    ChannelLayout layout = ChannelLayout::Stereo;
    // 믹서 블록 frame 수(Q)를 넣는다. backend가 다른 주기를 고르면 협상 결과로 알린다.
    uint32 callbackFrames = 0;
    AudioDeviceId device;
};

// 협상 결과다. callbackFrames는 콜백 주기의 최대 frame 수이고, 콜백은 그보다 적게 달라고 할 수
// 있다.
struct AudioOutputFormat {
    uint32 sampleRate = 0;
    ChannelLayout layout = ChannelLayout::Stereo;
    uint32 callbackFrames = 0;
    AudioSampleFormat sampleFormat = AudioSampleFormat::Float32;
};

// 장치 콜백이 채울 교차(interleaved) 버퍼다. data는 frames × channels 표본이다.
struct AudioOutputView {
    void* data = nullptr;
    uint32 frames = 0;
    uint32 channels = 0;
    AudioSampleFormat format = AudioSampleFormat::Float32;
};

// 장치 콜백이 부르는 함수다. backend 스레드(OS·라이브러리 소유)에서 불리므로 잠금·할당·대기·로그를
// 하지 않는다(3.2절). 구현은 출력 링에서 복사만 한다.
class AudioOutputCallback {
public:
    virtual ~AudioOutputCallback() = default;

    // view를 정확히 채운다(모자란 부분은 무음).
    virtual void Fill(const AudioOutputView& view) noexcept = 0;
};

// 출력 장치 backend다(3.7.1절). 한 객체가 장치 하나를 연다.
//
// - Open: 장치를 열고 협상 형식을 돌려준다. callback은 Close까지 살아 있어야 한다. 실패하면
//   OS·라이브러리 원래 오류 코드를 LastNativeError로 남긴다(로깅 원칙 — 원본 오류 코드 보존).
// - Start: 콜백을 시작한다. Open 뒤에만 부른다. 실패해도 그 전에 콜백이 불렸을 수 있다(호출자는
//   Close한 뒤에야 callback이 쓰는 자원을 거둔다).
// - Stop: 돌아오면 콜백이 진행 중이지 않고 다시 불리지 않는다(종료·장치 교체의 장벽).
// - Close: 장치를 닫는다. 콜백이 돌 수 있는 상태면(Start 성공·실패 모두) 먼저 멈추므로 Stop과 같은
//   장벽이다. 두 번 불러도 된다.
// - EnumerateDevices: 출력 장치를 out 앞에서부터 채우고 장치 전체 수를 돌려준다(out이 작으면
//   앞부분만).
// - Name: 실제로 쓰는 OS backend 이름(예: "WASAPI")이다. 로그용이며 Open 뒤에 확정된다.
// - LastNativeError: 마지막으로 실패한 호출의 OS·라이브러리 오류 코드다(없으면 0).
//
// 사용 예:
//   auto format = backend.Open(request, ringCallback);
//   if (format && backend.Start()) { ...; backend.Stop(); }
//   backend.Close();
class AudioOutputBackend {
public:
    virtual ~AudioOutputBackend() = default;

    virtual Result<AudioOutputFormat> Open(const AudioOutputRequest& request,
                                           AudioOutputCallback& callback) noexcept = 0;
    virtual Result<void> Start() noexcept = 0;
    virtual Result<void> Stop() noexcept = 0;
    virtual void Close() noexcept = 0;
    virtual Result<uint32> EnumerateDevices(Span<AudioDeviceInfo> out) noexcept = 0;
    virtual const char* Name() const noexcept = 0;
    virtual int32 LastNativeError() const noexcept = 0;
};

// backend 하나를 만드는 함수다. ClientOnly 모듈(AudioMiniaudio)이 등록한다(3.33.6절).
using AudioOutputBackendFactory =
    Result<UniquePtr<AudioOutputBackend>> (*)(Allocator& allocator) noexcept;

// 호스트가 HostServices에 등록하는 backend factory 칸이다(3.33.6절, AD24). Runtime 모듈 Audio는
// ClientOnly 장치 모듈을 링크하지 않으므로 그 모듈의 Startup이 여기에 factory를 넣는다. factory가
// 없으면(전용 서버·장치 모듈 없음) 장치는 Null로 연다.
//
// 사용 예:
//   // 호스트:
//   AudioOutputBackendRegistry registry;
//   (void)loader.Services().Register(registry);
//   // AudioMiniaudio Startup:
//   if (auto* registry = context.Services().Find<AudioOutputBackendRegistry>()) {
//       registry->SetFactory(&CreateMiniaudioOutput);
//   }
// 등록·조회는 로더 스레드에서 한다(HostServices 계약).
class AudioOutputBackendRegistry final {
public:
    // factory를 바꾼다(nullptr이면 지운다). 모듈 Shutdown은 자기 factory를 지운다.
    void SetFactory(AudioOutputBackendFactory factory) noexcept {
        factory_ = factory;
    }

    AudioOutputBackendFactory Factory() const noexcept {
        return factory_;
    }

private:
    AudioOutputBackendFactory factory_ = nullptr;
};

} // namespace rocks::audio

ROCKS_DECLARE_HOST_SERVICE(rocks::audio::AudioOutputBackendRegistry, ROCKS_AUDIO_API)
