# 앱 정보와 명령줄 — 10. 실행 성격과 명령줄·설정

[목차](README.ko.md)

## 10. 실행 성격과 명령줄·설정

### 10.1 출처

실행 성격은 **명령줄 + 빌드·호스트 기본값**에서만 정한다. 설정 파일과 CVar로는 정하지 않는다(D6). 이유는 셋이다.
- 봉인(`CompleteStartup`) 전부터 필요하다. Fatal 대화상자 정책은 설정을 읽기 전에도 정해져 있어야 한다.
- 출시 빌드의 세션 층(`--set`)은 서버 대상만 받는다(Config D5). 실행 성격을 CVar로 두면 출시 게임에서 `--unattended`를 쓸 수 없다.
- 실행 한 번의 성격이다. 프로젝트 파일에 굳혀 둘 값이 아니다. CI 기계별 설정은 실행 명령줄로 준다.

설정 쪽과는 이렇게 잇는다.
- **보기.** 콘솔 명령 `app.info`가 AppInfo 전체(실행 성격 포함)를 출력한다. 콘솔 이름 영역 표에 `app`을 더한다(Config 5.1).
- **값 기본값.** 엔진 설정이 실행 성격에 따라 기본값이 달라야 하면 그 설정의 등록 코드가 `AppInfo::Get().Traits()`를 보고 Code 층에 쓴다. 예: benchmark면 프레임 상한을 끈다. 출처 스택에 `app traits`로 남아 Explain에서 보인다. 등록은 부트 4단계이고 AppInfo는 3단계에 설치되므로 읽을 수 있다(8.3절).
- **엔진 고정 step.** 호스트가 `EngineConfig::fixedFrameDeltaSeconds`를 `Traits().UsesFixedTimeStep() ? Traits().fixedDeltaSeconds : 0`으로 채운다.
  - 지금 Sandbox는 `--engine` 실행마다 상수 `HeadlessFrameSeconds`(1/60)를 넣는다(`Programs/Sandbox/Private/EngineHost.cpp:27,42`). 이 상수를 지우고 `--deterministic`(기본 1/30)으로 옮긴다.
  - Sandbox 자체 시험이 띄우는 자식(`--engine --frames N`)은 `--deterministic`을 함께 넘긴다. 고정 step 길이가 1/60에서 1/30으로 바뀌므로, 자식 결과를 시간 값으로 비교하는 곳이 있으면 함께 고친다(CL4).
- **게임플레이 고정 step과의 경계.** 프로젝트가 늘 고정 step으로 돌아야 하는 경우(결정적 시뮬레이션 게임)는 실행 성격이 아니라 World `TimeStepPolicy`(설정)가 맡는다. `RunTraits`의 고정 step은 실행 한 번의 엔진 프레임 delta만 바꾼다.

### 10.2 명령줄 플래그

공용 옵션 struct `HostCommandLineOptions`(14.3절)의 필드다.

뜻·기본값·출시 빌드 수용 여부는 UE 5.8과 같다(절대 규칙 — 시간). 다른 점은 고정 delta의 중간 float 반올림을 따르지 않는 것 하나다(D5).

| 옵션 | 효과 | 출시 빌드 | UE 대응 |
|---|---|---|---|
| `--unattended` | `unattended = true` | 받는다 | `-UNATTENDED`(`APPC` 278) |
| `--unattended-input` | `unattendedInput = true` | 받는다 | `-UnattendedInput`(`APPC` 286) |
| `--benchmark` | `benchmark = true`. 고정 step(`UsesFixedTimeStep`)과 `fixedSeed = true`를 함께 뜻한다. 포커스는 늘 true, 소리는 끈다(`audioOutput = false`) | **받지 않는다** | `-BENCHMARK`(`LEL` 2449–2454, 출시는 false), 고정 step(`UEN` 3005), 시드(`LEL` 2462), 소리(`APP` 437) |
| `--use-fixed-time-step` | `useFixedTimeStep = true` | 받는다 | `-UseFixedTimeStep`(`LEL` 2460) |
| `--deterministic` | `useFixedTimeStep = true`, `fixedSeed = true`. 길이는 `fixedDeltaSeconds`(기본 1/30) | 받는다 | `-Deterministic`(`LEL` 2456–2462, "-UseFixedTimeStep -FixedSeed"의 줄임) |
| `--fixed-seed` | `fixedSeed = true` | 받는다 | `-FixedSeed`(`LEL` 2462) |
| `--fps <rate>` | `fixedDeltaSeconds = 1 / rate`. **고정 step을 켜지 않는다**(길이만 바꾼다). `Range`로 rate는 0보다 크고 유한하다 | 받지 않는다 | `-FPS=`(`LEL` 4724–4731, 비출시) |
| `--headless` | `headless = true` | 받는다 | `-nullrhi`(뜻이 넓다 — 창·장치를 아예 만들지 않는다) |
| `--no-sound` | `audioOutput = false` | 받는다 | `-nosound`(`APP` 436, `-enablesound`는 두지 않는다 — 뒤 값이 앞 값을 덮는다) |
| `--mute-audio` | `audioMuted = true` | 받는다 | `-muteaudio`(`APP` 443–453) |
| `--allow-commandlet-audio` | commandlet에서도 `audioOutput`을 끄지 않는다 | 받지 않는다 | `IsAllowCommandletAudio()`(`APP` 437) |
| `--instance-id <guid>` | AppInfo `instanceId` | 받지 않는다 | `-InstanceId=`(`APPC` 182–188) |
| `--session-id <guid>`·`--session-name <text>` | AppInfo `sessionId`·`sessionName` | 받지 않는다 | `-SessionId=`·`-SessionName=`(`APPC` 191–203). UE도 `InitializeSession`을 비출시에서만 부른다(`LEL` 6880–6886) |

- UE 고정 step 계열(`-UseFixedTimeStep`·`-Deterministic`·`-FixedSeed`)은 `WITH_FIXED_TIME_STEP_SUPPORT` 안에 있고, 이 매크로는 기본 켜짐이라 Shipping에서도 동작한다(3.5절). Rocks도 출시 빌드에서 받는다.
- 시험 실행기(`HostKind::TestRunner`)는 `unattended`가 기본 true다(UE `GIsAutomationTesting` OR와 같은 뜻).
- 이름만 예약하고 지금 받지 않는 것: `--single-thread`(20절). `--no-sound`·`--mute-audio`는 개정 4에서 오디오 설계와 함께 열었다(구현은 오디오 AU3 또는 AP1 중 먼저 오는 쪽).
- 출시 빌드에서 받지 않는 옵션에는 Command 전용 지정자 `NotInRelease`를 붙인다(13.3절). Release 구성의 파서는 이 옵션을 "알 수 없는 옵션"으로 다룬다. 기준은 대상 종류와 무관한 Release 구성이다(UE `!UE_BUILD_SHIPPING`과 같은 뜻).
