# 오디오 시스템 설계

기준일: 2026-11-22, 개정 1. 소리를 재생하는 엔진 기능 전체를 정한다. 범위는 실시간 믹서, 장치 출력, 엔진·World 연결, 사운드 에셋, 임포트, 코덱·쿠킹, 스트리밍, 감쇠·공간화·동시 발음·가상화, 버스·효과·리버브 존, 진단이다. UE 5.8의 AudioMixerCore·AudioMixer·SignalProcessing·Engine 오디오 클래스와 AudioGameplayVolume 플러그인에 해당한다.

[엔진 구현 계획](EngineImplementationPlan.ko.md) §18은 AudioRT 무대기 계약(Object·파일 IO·동적 할당·lock·직접 로그 금지), voice handle 세대, stop mailbox, epoch ACK, 48 kHz·256/512 frame, 성능 목표를 이미 정했다. 이 문서는 그 계약을 구현 수준으로 구체화한다. 설계가 애매한 곳은 UE 방식을 따르고, Rocks 원칙(결정적 시험, `Result` 오류, 명시적 소유, 전역 생성자 금지)과 부딪히는 곳만 다르게 정한다(7절).

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`, commit `f45729c0b6f0`)이고, 경로는 `Engine/` 기준이다. UE 이름은 대응표와 근거에만 적는다. 같은 날 작성한 미추적 분석 초안(`tmp/Rocks_Audio_System_UE58_Analysis.ko.md`)의 내용을 이어받았고, 그 초안의 단계 이름 A0–A6은 [에셋 시스템](AssetSystem.ko.md) 단계와 겹쳐 AU로 바꿨다.

상태: **설계 초안**(결정 AD1–AD28. AD5만 사용자 결정이고 나머지는 권장안. 교차 검증 1회 반영 — 필수 9·권장 13). 구현 단계는 AU1–AU10(6절)이며 착수 시점은 사용자가 정한다. 계획 순서(A0-B → A0-O → 리플렉션 B9 → Curve → A0-G → A1–A6 → 화면 마일스톤)에 끼워 넣지 않는다. AU1–AU3은 에셋 후속 단계에 기대지 않고, AU4 이후는 해당 에셋 단계(A2f·A3·A4)가 끝나야 시작한다.

## 1. 현재 상태

| 항목 | 현재 Rocks | 문제 |
|---|---|---|
| 오디오 코드 | 없다. 오디오 모듈·장치 backend·디코더·인코더·ThirdParty 오디오 라이브러리·사운드 에셋 클래스·임포터가 없다 | — |
| 설계 계약 | 계획 §18(1673–1706행): AudioRT 금지 목록, `AudioVoiceHandle{index,generation}`, `AudioCommand{sampleTime,voice,op,data}`, `RenderAudioBlock(float*, frames) noexcept`, 불변 graph plan 교체·epoch ACK, 48 kHz·256/512 frame, underrun 0·callback p99.9 ≤ 블록 50%. 마일스톤 M4 기본 오디오, M7 audio graph(§30.1) | 콜백에서 직접 믹스하는 모양으로 적혀 있다. UE 구조(전용 렌더 스레드 + 출력 링)와 다르다(3.2절, AD2) |
| Task | `TaskLane::Audio`, `DedicatedLane`(FIFO 작업 lane, Thread·Inline 모드), `SerialTaskChain`(`Core/Source/Runtime/Task`) | `DedicatedLane::Post`는 `Function<void()>` 작업 노드를 받는다. 블록마다 작업을 게시하는 모양은 AudioRT의 무할당 계약과 맞지 않는다(3.2절) |
| 스레드 우선순위 | `ThreadPriority { Lowest, BelowNormal, Normal, AboveNormal, Highest }`(`Core/Source/Runtime/PlatformBase/Public/Platform/Thread.h:23`). Windows `Highest` = `THREAD_PRIORITY_HIGHEST`(`Private/Win64/Concurrency.cpp:232`) | UE 오디오 렌더 스레드도 `TPri_Highest`다(3.2절). 실시간 등급은 없지만 UE와 같은 수준은 있다 |
| 동시 컨테이너 | `BoundedMpscQueue`(구현). `BoundedSpscQueue`·`FixedBlockPool`·`IndexedFreeList`는 [동기화](Synchronization.ko.md) SY7 설계만 | `BoundedMpscQueue`는 생산자가 예약 슬롯을 게시하기 전에 선점되면 소비자 `TryPop`이 실패할 수 있다. AudioRT 입력은 SPSC여야 한다(Synchronization D27) |
| 깨우기 수단 | `ManualResetEvent`·`Semaphore`는 Signal·Release 안에서 `Mutex`를 잡는다(`Core/Source/Runtime/PlatformBase/Public/Platform/Sync.h:86–108`, `Private/Common/Sync.cpp:61–68`). SY0 `WaiterSignal` 설계는 Windows `WaitOnAddress`, Android futex, macOS `pthread_mutex_t`+`pthread_cond_t`(Synchronization 350행) | 장치 콜백이 AudioRT를 깨울 때 잠금을 공유하면 우선순위 역전이 생긴다. 무잠금 깨우기가 없다(3.2절, PR21) |
| 엔진 수명 | `EngineSubsystem`의 `Initialize`·`OnFrameBegin`·`OnFrameEnd`·`BeginShutdown`·`CompleteInFlightWork`·`Deinitialize`(`Engine/Source/Runtime/Engine/Public/Engine/Engine.h:46–70`). 종료 2단계 주석에 "audio epoch" 대기 | `CompleteInFlightWork` 실패는 `KeepFirst`로 누적되고 정리가 이어진다. 장치 콜백이 살아 있는 채 소유자가 풀릴 수 있다(3.8.6절) |
| 명령줄 | `--no-sound`·`--mute-audio` 이름만 예약, 볼륨 배수는 "오디오 모듈 설계 때"로 미룸([AppInfo·명령줄](AppInfoCommandLine.ko.md) 128행, §8.6) | 판정 함수가 없다(3.8.4절) |
| 설정 | 콘솔 영역 `"audio"`(`Core/Source/Runtime/Config/Public/Config/ConsoleSettings.h:66`), `audio.master.volume` 시험 사용 | 오디오 CVar가 없다 |
| 빌드 대상 | [빌드 타깃](BuildTargets.ko.md) 59–63행에 모듈 유형 `ClientOnly`. 69행은 Server의 기능 축소 범위를 네트워크 단계 스펙에 넘긴다 | 빌드 코드에 `ClientOnly`가 없다. 오디오 모듈의 Server 범위는 이 문서가 정한다(AD24) |
| 에셋 | A1 구현, A2a–A2e 구현, A2f 임포트 프레임워크는 A2f1a 구현(진단 등록·원본 루트·원본 읽기·임포터 표)·A2f1b·A2f2 설계(`AssetImporter`·`AssetImporterTable`, AssetSystem 7.1절 18), A3 AssetNugget·A4 AssetCooker 설계. `ReadyMask { Cpu, Render }`(`Engine/Source/Runtime/AssetRuntime/Public/AssetRuntime/AssetManager.h:36`) | 오디오 재생 구간이 준비됐는지 표현할 수단이 없다(3.12.4절) |
| Sparks | 스키마 `Engine/Source/Developer/SparksTestHost/Schema/Engine.sparkbind.json`에 `SoundAsset`·`AudioSystem.PlayAt/StartLoop`·`World.Audio`. [Sparks 엔진 통합](SparksEngineIntegration.ko.md) SE-G7·EI5는 모듈 없음으로 대기 | — |
| policy | `PolicyChecks.WindowsMacroLeak.cs`가 `xaudio2.h`·`mmdeviceapi.h`·`audioclient.h`를 Windows 매크로 누출 헤더로 안다 | 장치 헤더는 backend 모듈의 Private에만 둔다 |

## 2. 용어 대응

| UE 5.8 | Rocks | 비고 |
|---|---|---|
| `FAudioDeviceManager` | `AudioEngineSubsystem` | 엔진 소유, 장치 생성·World 연결 |
| `FAudioDevice`(오디오 pipe 쪽 상태) | `AudioDeviceContext` | 장치 하나의 제어 상태: 재생 목록·동시 발음·가상화·리스너 |
| `FMixerDevice`·`FMixerSourceManager` | `AudioMixer` | AudioRT 쪽 블록 믹서 |
| `IAudioMixerPlatformInterface` | `AudioOutputBackend` | 장치 열기·협상·콜백 |
| `FMixerNullCallback` | `NullAudioOutput` | 장치 없이 시간만 진행 |
| `FMixerPlatformNonRealtime` | `OfflineAudioRender` | 요청한 frame만큼 렌더 |
| `FAudioThread`·`GAudioPipe` | 오디오 제어 pipe(`AudioControlPipe`) | 직렬 task |
| 오디오 렌더 스레드 | AudioRT 스레드 | 계획 §8.1 lane 이름 그대로 |
| `USoundWave` | `SoundClip` | 사운드 원본·재생 단위 에셋 |
| `USoundCue`·`USoundNode*` | `SoundEvent` | 레이어·랜덤·switch(AU10) |
| `USoundBase` | `SoundSource`(추상) | `SoundClip`·`SoundEvent`의 공통 기반 |
| `FActiveSound` | 재생(`Playback`), 핸들 `PlaybackHandle` | 게임이 시작한 논리 재생 하나 |
| `FWaveInstance` | 재생층(`PlaybackLayer`) | 재생이 평가한 clip 하나 |
| `FMixerSource`·`FMixerSourceVoice` | `AudioVoice`, 핸들 `AudioVoiceHandle` | 실제 샘플을 만드는 슬롯 |
| `UAudioComponent` | `AudioComponent` | `SceneComponent` 파생 |
| `USoundSubmix` | `MixBus` | 출력 그래프 노드 |
| `UAudioBus`·`USoundSourceBus` | `SignalBus` | 신호를 다른 source 입력으로 넘기는 버스(AU10) |
| `USoundClass` | `SoundCategory` | 계층형 볼륨·피치 분류 |
| `USoundMix` | `MixSnapshot` | 시간에 따라 적용하는 분류 조정 묶음 |
| `USoundConcurrency` | `ConcurrencyGroup` | |
| `USoundAttenuation`·`FSoundAttenuationSettings` | `AttenuationSettings` | 에셋과 인라인 struct |
| `UReverbEffect` | `ReverbPreset` | |
| `AAudioVolume`·`UReverbVolumeComponent` | `ReverbZoneComponent` | |
| `FInteriorSettings` | `ZoneInteriorSettings` | |
| `FAudioVirtualLoop` | 가상 재생(virtual playback) | |
| Quartz | `BeatClock` | AU10 |
| MetaSounds | `SoundGraph` | M7 |
| `ISoundGenerator` | `SoundGenerator` | procedural source 입력 |
| `IAudioFormat` | `AudioEncoder` | 쿠킹 쪽 |
| `ICompressedAudioInfo`·`IAudioInfoFactory` | `AudioDecoder`·`AudioDecoderTable` | 런타임 쪽 |
| `FCachedAudioStreamingManager`·`FAudioChunkCache` | `AudioChunkCache` | |
| `ESoundWaveLoadingBehavior` | `SoundLoadingBehavior` | |
| `FDerivedAudioDataCompressor`(DDC) | 오디오 쿠킹 처리기(AssetNugget) | |
| `USoundFactory` | `SoundClipImporter` | A2f `AssetImporter` 구현 |
| PIE | Tryout | |
| Audio Insights | `AudioInspector` | 화면 마일스톤 뒤 |
| `FApp::CanEverRenderAudio`·`IsAudioMuted` | `RunTraits::audioOutput`·`audioMuted` | 3.8.4절 |

## 3. 설계

### 3.1 층과 모듈

UE는 장치 추상(AudioMixerCore) → DSP(SignalProcessing) → 믹서(AudioMixer) → 게임 객체(Engine의 Sound* 클래스) 4층이고, 플랫폼마다 backend 모듈(AudioMixerWasapi·XAudio2·CoreAudio·AudioUnit·Android·SDL)을 `[Audio] AudioMixerModuleName`(`Config/<Platform>/*Engine.ini`)으로 고른다. Rocks도 같은 층을 두되 처음부터 나누지 않을 것은 한 모듈 안에서 시작한다(AD10).

| 모듈 | 위치 | 유형 | 책임 | 의존 |
|---|---|---|---|---|
| `Audio` | `Engine/Source/Runtime/Audio` | Runtime | 형식·frame·채널 레이아웃, PCM 버퍼, 리샘플러·필터·패너·dynamics·리버브 DSP, voice pool, 명령·회수 경로, mix clock, 버스 graph plan, `AudioMixer`, `AudioOutputBackend`·`NullAudioOutput`·`OfflineAudioRender`, `AudioDecoder` 인터페이스와 PCM·ADPCM 디코더, 숫자 trace 링 | Core, Task |
| `AudioMiniaudio` | `Engine/Source/Runtime/AudioMiniaudio` | ClientOnly | miniaudio 장치 IO 어댑터(`AudioOutputBackend` 구현) | Audio, ThirdParty `Miniaudio`(Private) |
| `AudioOpus` | `Engine/Source/Runtime/AudioOpus` | ClientOnly | Opus 디코더(`AudioDecoder` 구현) | Audio, ThirdParty `Opus`(Private) |
| `EngineAudio` | `Engine/Source/Runtime/EngineAudio` | Runtime | `AudioEngineSubsystem`, `AudioWorldSubsystem`, `AudioComponent`, `ReverbZoneComponent`, 사운드 에셋 클래스, `AudioDeviceContext`(동시 발음·가상화·감쇠·리스너), 재생 API | Engine, World, AssetRuntime, EngineAssets, SpatialIndex, Audio |
| `AudioTools` | `Engine/Source/Developer/AudioTools` | Developer | `SoundClipImporter`, WAV·AIFF 파서, 손실·무손실 원본 디코드, `AudioEncoder`와 인코더, 쿠킹 처리기, 파형 미리보기 데이터 | AssetDatabase, AssetRuntime, EngineAudio(`SoundClip` 필드·TypeId), Audio, ThirdParty `Flac`·`Vorbis`·`Ogg`·`Opus`·`Minimp3`(Private) |

- 서버 실행 파일은 `AudioMiniaudio`·`AudioOpus`를 링크하지 않는다. `Audio`·`EngineAudio`는 링크한다. 재생 API와 에셋 클래스가 있어야 서버의 게임 코드·Sparks가 그대로 빌드되고, 장치가 없으면 재생 요청은 "무시됨" 결과를 낸다(3.8.4절, AD8). 서버가 오디오 길이·마커 같은 메타데이터만 읽는 경우는 `Audio`의 플랫폼 데이터 헤더 해석으로 충분하다. 쿠킹된 서버 패키지에는 오디오 payload를 넣지 않는다(3.11.5절).
- `Audio`는 World·Engine·AssetRuntime·Object를 모른다. 공개 헤더에 장치·코덱 라이브러리 타입을 두지 않는다. `EngineAudio`가 Object 세계와 `Audio`를 잇는다. UE는 Engine과 AudioMixer가 서로 의존하지만 Rocks는 단방향이다(7절).
- backend·디코더 연결: `AudioMiniaudio`의 모듈 `Startup`이 [모듈 로더](ModuleLoader.ko.md) `HostServices`에 `AudioOutputBackendFactory`를 등록하고, `AudioOpus`는 `AudioDecoderTable`에 디코더를 등록한다. `AudioEngineSubsystem`은 `HostServices::Find`로 factory를 찾고, 없으면(서버·모듈 미적재) `NullAudioOutput`을 쓴다. 호스트(Launch·Sandbox)는 ClientOnly 모듈을 직접 참조하지 않는다. Engine 모듈이 graphics·window를 모르는 원칙(EngineRuntime)과 같다.

버린 안: UE처럼 SignalProcessing·AudioExtensions·AudioPlatformConfiguration·SoundFieldRendering을 처음부터 따로 둔다. 쓰는 곳이 하나뿐인 경계를 미리 만들면 이동 비용만 생긴다. 두 번째 사용자가 생기면 동작 불변 이동 커밋으로 나눈다.

### 3.2 스레드와 소유권

UE 5.8은 세 문맥이다.
1. 게임 스레드: `UAudioComponent`·`UGameplayStatics`가 `FActiveSound`를 만들어 장치에 넘긴다.
2. 오디오 pipe: 전용 OS 스레드가 아니라 `UE::Tasks::FPipe GAudioPipe`(`Source/Runtime/Core/Private/HAL/ThreadingBase.cpp:245`)다. 게임 스레드 명령을 일괄로 넘기고(`Engine/Private/AudioThread.cpp`), `FAudioDevice::Update`가 게임 틱마다 여기서 정렬·동시 발음·가상화·source 시작/정지를 한다.
3. 오디오 렌더 스레드: `IAudioMixerPlatformInterface::BeginGeneratingAudio`가 `TPri_Highest`(`Source/Runtime/AudioMixerCore/Private/AudioMixer.cpp:94·695`)로 만든다. `FOutputBuffer::MixNextBuffer`로 출력 링이 찰 때까지 블록을 렌더하고 렌더 이벤트를 기다린다. 장치 콜백(`ReadNextBuffer`, 583–)은 링에서 버퍼 하나를 통째로 꺼내 제출하고 이벤트를 깨운다. UE는 렌더 블록 크기를 콜백 크기와 같게 두고, 링은 2칸 아래로 두지 않는다(`NumOutputBuffers = FMath::Max(NumBuffers, 2)` — 677). 기본값은 48 kHz·1024 frame·버퍼 2(`AudioMixerCore/Public/AudioMixerTypes.h:82–84`). UE 믹서는 MMCSS를 쓰지 않는다(`AvSetMmThreadCharacteristics`는 AudioCaptureRtAudio에만 있다).

Rocks도 세 문맥으로 둔다.

| 문맥 | 실행 | 혼자 바꾸는 상태 | 넘기는 것 |
|---|---|---|---|
| Game | Game lane | Object(`AudioComponent`·에셋), `AssetLease`, World 등록 표, 사용자 콜백 | 제어 명령(고정 크기 POD, 3.5절)을 제어 pipe 입력 큐로 |
| 오디오 제어 pipe | `SerialTaskChain` 하나(`AudioControlPipe`). Game의 `OnFrameEnd`가 그 프레임 명령 묶음과 리스너·emitter 스냅샷을 한 task로 제출 | `AudioDeviceContext`: 재생 표·재생층·동시 발음 그룹·가상화·감쇠·우선순위 정렬·버스 plan 작성·PCM pin | AudioRT 명령 링(SPSC), 파라미터 최신값 슬롯, stop mailbox, plan 교체 |
| AudioRT | 전용 `platform::Thread`(`ThreadPriority::Highest`), 장치마다 하나 | voice 상태·DSP 상태·gain ramp·mix clock·현재 plan·출력 링 쓰기 위치 | terminal 슬롯, 숫자 이벤트 링, epoch ACK |
| 장치 콜백 | backend 스레드(OS·miniaudio 소유) | 출력 링 읽기 위치 | `AudioWakeSignal` 깨우기, underrun 계수 |
| 디코드·IO worker | `WorkerScheduler` | 디코더 인스턴스 상태, 요청 버퍼 | 세대·요청 번호가 붙은 PCM 페이지 |

AudioRT 규칙(계획 §18 그대로): Object 접근, 파일 IO, 동적 할당·해제, mutex·조건 변수, 일반 task 대기, 문자열 로그를 하지 않는다. 기다리는 것은 렌더 깨우기 신호 하나뿐이다(출력 링이 찼을 때). trace는 사전 할당 숫자 링에만 쓴다. 실제 적용된 스레드 우선순위를 장치 열기 로그에 남긴다(`SetThreadPriority` 실패는 지금 무시된다 — `Private/Win64/Concurrency.cpp:237`).

렌더 깨우기 신호(`AudioWakeSignal`, AD26): 장치 콜백이 Signal하고 AudioRT가 Wait하는 단일 대기자 자동 리셋 신호다. Signal은 잠금을 잡지 않는다: Windows `WakeByAddressSingle`(또는 auto-reset event), Android futex wake, macOS Mach `semaphore_signal`. 기존 `ManualResetEvent`·`Semaphore`(내부 Mutex)와 SY0 `WaiterSignal` macOS 구현(pthread 조건 변수)은 쓰지 않는다. 이 신호는 [동기화](Synchronization.ko.md) 개정으로 PlatformBase에 둔다(선행 PR21).

출력 링: 블록 단위 칸 `bufferCount`개. 장치 열기는 `callbackFrames` = Q를 요청하고(3.7.1절), 협상 결과로 칸 수를 정한다.
- `bufferCount = max(MinOutputBufferCount, ceil(callbackFrames / Q) + 1, audio.mixer.buffercount)`, `MinOutputBufferCount` = 2(UE와 같다), 상한 `MaxOutputBufferCount` = 16.
- 근거: 콜백 한 번이 `callbackFrames`를 가져가는 동안 AudioRT가 다음 블록을 하나 더 채울 수 있어야 구조적 underrun이 없다. 예: miniaudio WASAPI 공유 모드 기본 10 ms = 480 frame, Q = 256이면 `ceil(480/256)+1` = 3칸(768 frame). 2칸이면 한 번 소비 뒤 남는 양이 512 − 480 = 32 frame뿐이라 다음 콜백 전에 채워야 할 양이 블록 기간보다 짧아진다.
- AudioRT는 빈 칸이 있으면 블록을 렌더하고, 없으면 깨우기 신호를 기다린다. 장치 콜백은 요청 frame 수만큼 링에서 복사하고, 블록 경계가 콜백 경계와 다르므로 블록 안 읽기 위치를 유지한다(UE는 블록 = 콜백이라 이 상태가 없다 — 7절). 링이 비면 무음을 채우고 underrun을 센다.
- 대기 시간 초과(`RenderWaitTimeoutMs`)는 장치 정지로 보고 제어 쪽에 알린다(UE는 시간 초과 때 장치 교체를 시작한다 — 3.7.3절).

지연: 렌더 쪽 선행은 최대 `bufferCount × Q` frame이다. 48 kHz·Q=256·`callbackFrames`=256이면 2칸 = 10.67 ms, 위 WASAPI 예(3칸)면 16 ms다. 여기에 장치 버퍼가 더해진다.

Inline(결정적) 모드: `OfflineAudioRender`가 AudioRT 스레드 대신 호출 스레드에서 같은 `AudioMixer::RenderBlock`을 부른다. 제어 pipe도 Inline으로 돌린다(SerialTaskChain에 넣지 않고 호출 스레드에서 바로 실행). 모든 믹서 시험은 이 모드로 결정적으로 단언한다. 실제 스레드 모드와 Inline 모드의 PCM 일치 시험은 mix frame을 명시한 명령으로만 한다. "즉시" 명령은 AudioRT가 링을 소비하는 시점에 따라 시작 frame이 달라지기 때문이다.

버린 안:
- 장치 콜백에서 바로 믹스(계획 §18·분석 초안). 렌더 쪽 선행이 줄지만, 콜백 스레드의 우선순위·스택·호출 주기를 backend마다 따로 다뤄야 하고, null·오프라인 경로가 실장치 경로와 달라진다. UE도 콜백과 렌더를 링으로 떼어 놓았다.
- 출력 링 1칸 허용. UE가 막는 값이고, Q ≠ `callbackFrames`면 구조적 underrun이 난다.
- AudioRT를 `DedicatedLane`으로. 블록마다 작업 노드를 게시하는 모양이고 `Function` 할당이 따른다. AudioRT는 이벤트를 기다리며 같은 일을 반복하는 고정 루프이므로 전용 스레드가 맞다(UE도 전용 `FRunnable`). 제어 pipe는 할당이 허용되므로 `SerialTaskChain`을 쓴다.
- 제어 상태를 Game lane에서 직접(분석 초안). 단순하지만 정렬·동시 발음·가상화 비용이 게임 틱에 얹힌다. UE 5.8의 pipe 구조를 따른다. Inline 모드에서는 결과가 같다.
- 실시간 등급(Windows MMCSS "Pro Audio", macOS time-constraint, Android `SCHED_FIFO`). UE 믹서가 쓰지 않는다. AU9 측정에서 `Highest`로 목표(3.18절)를 못 맞출 때 PlatformLayer에 등급을 더한다(AD3).

### 3.3 형식과 시간

- **frame**: 한 시점의 모든 채널 샘플. 스테레오 256 frame = float 512개.
- **내부 형식**: float32, 채널 비교차(planar)로 voice·버스 버퍼를 둔다. 장치 경계에서만 장치 형식(교차 float32 또는 int16)으로 바꾼다. UE도 내부는 float다.
- **mix rate**: 장치 하나에 하나. 기본 `DefaultMixSampleRate` = 48000. 장치가 다른 rate를 요구하면 그 rate로 mix한다(UE와 같다 — 장치 협상값). rate가 바뀌면 clock epoch를 올리고 예약 시각을 초 단위로 다시 맞춘다(3.7.3절).
- **블록 크기 Q**: 기본 `DefaultMixBlockFrames` = 256(계획 §18), 64·128·256·512·1024 중 설정(`audio.mixer.blockframes`). UE 기본 1024보다 작다(AD7).
- **채널 레이아웃**: `ChannelLayout { Mono, Stereo, Quad, Surround51, Surround71 }`, 최대 `MaxOutputChannels` = 8. 내부 채널 순서는 FL, FR, FC, LFE, SL, SR, BL, BR(UE `AudioChannelUtils` 기본 순서 — `Source/Runtime/SignalProcessing/Private/AudioChannelUtils.cpp:16–26`)이고 5.1은 앞 여섯이다. UE `EAudioMixerChannel` 열거 순서(FL, FR, FC, LFE, BL, BR, FLC, FRC, BC, SL, SR… — `AudioMixerCore/Public/AudioMixerChannel.h:12–34`)와 다르다. 장치 순서는 backend가 알려주는 채널 마스크로 매핑한다(WAVEFORMATEXTENSIBLE·miniaudio 7.1 장치는 FL, FR, FC, LFE, BL, BR, SL, SR). 디코더가 내는 순서(Vorbis 순서, Opus 채널 매핑 family 1 = FL, FC, FR, …, LFE)도 디코드 직후 내부 순서로 바꾼다. 첫 장치 출력은 스테레오다. 다채널 출력·downmix 행렬은 AU9.
- **mix clock**: 장치 하나의 64비트 frame 카운터(`MixFrame`). 블록 시작마다 Q만큼 증가한다. 명령의 예약 시각은 이 단위다.
- **device clock**: backend가 알려주는 실제 재생 위치. mix clock과의 차이를 지연 추정치로 공개한다.
- **clip 커서**: clip 원본 rate의 frame 단위 정수부 + 소수부(리샘플 상태).
- 오버플로 검사: `frames × channels × bytesPerSample`과 버퍼 길이는 AudioRT 진입 전에 검사한다.

블록 안 예약: 블록 범위를 `[start, start+Q)`로 보고, 그 블록에 걸린 명령을 (예약 frame, 순번) 순으로 정렬해 블록을 구간으로 나눈다. 예: 48 kHz·Q=256에서 다음 블록이 1024에 시작하고 재생 예약이 1124면, 앞 100 frame은 기존 신호만, voice는 offset 100부터 낸다. 이미 지난 시각에 도착한 명령은 블록 시작에 실행하고 늦은 frame 수를 결과로 알린다(3.5.4절). 한 블록의 구간 수는 `MaxBlockSegments`(= 16)로 막고, 넘는 명령은 다음 블록으로 미룬다(늦음으로 보고). UE는 `SetSubBufferDelayForSound`로 Quartz 예약만 블록 안 offset을 준다. Rocks는 모든 시작·정지 명령에 같은 규칙을 쓴다(7절).

### 3.4 ID와 핸들

| 이름 | 모양 | 뜻 | 수명 |
|---|---|---|---|
| `SoundClipRef` | `AssetId` + revision | 사운드 내용 | 에셋 |
| `PlaybackHandle` | `{slot: uint32, generation: uint32}` | 게임이 시작한 재생 하나 | 재생 수락부터 terminal 결과 수거까지 |
| `AudioVoiceHandle` | `{index: uint32, generation: uint32}`(계획 §18) | 실제 샘플을 만드는 voice 슬롯 | voice 배정부터 AudioRT 마지막 사용 ACK까지 |
| `AudioSceneId` | `uint32` + epoch | World 하나의 재생 공간 | World 등록부터 해제까지 |
| `AudioEmitterId` | `{slot, generation}` | 부착 재생의 위치 원천(`AudioComponent` 하나) | 컴포넌트 등록부터 해제까지 |
| `AudioDeviceId` | `uint32` | 장치 하나 | 장치 생성부터 파괴까지 |

재생 하나가 voice 여럿(레이어)을 쓰거나 voice 없이(가상) 살아 있을 수 있으므로 둘을 처음부터 나눈다. 게임에 노출하는 것은 `PlaybackHandle`뿐이다. 슬롯 재사용은 generation으로 막는다. 이전 generation의 stop·파라미터 명령은 아무 일도 하지 않는다(시험).

### 3.5 명령과 회수 경로

#### 3.5.1 Game → 제어 pipe

- 재생 API(3.8.3절)는 Game에서 `PlaybackHandle` 슬롯과 결과 mailbox를 먼저 예약한다. 예약할 수 없으면 `CapacityExceeded`로 즉시 실패한다. 성공한 `Play`의 뜻은 "제어 쪽이 이 재생의 시작과 끝을 추적할 자리를 확보했다"이다. 소리가 났다는 뜻이 아니다.
- 명령은 Game 쪽 배열에 모았다가 `OnFrameEnd`에서 한 task로 제어 pipe에 넘긴다(UE `GAudioAsyncBatcher`와 같은 일괄 전송). 리스너·emitter 위치는 같은 task가 스냅샷으로 가져간다.

#### 3.5.2 제어 pipe → AudioRT

| 경로 | 구조 | 포화 때 |
|---|---|---|
| 명령 링 | `BoundedSpscQueue<AudioRtCommand>`(SY7), 용량 `AudioRtCommandCapacity`(= 4096) | 제어 쪽이 다음 프레임으로 미루고 그 재생을 `Pending`으로 둔다. 같은 voice의 시각 지정 명령 순서는 보존한다 |
| 파라미터 최신값 | voice마다 seqlock 슬롯(gain·pitch·pan·send·LPF 목표값). 제어 쪽이 순번을 홀수로 올리고 값을 쓰고 다시 짝수로 올린다. AudioRT는 블록 시작에 순번 → 값 → 순번을 읽고, 두 순번이 다르거나 홀수면 이번 블록은 이전 값을 유지한다(재시도 루프 없음 — 대기 금지) | 덮어쓴다(최신값 병합 — 계획 §18) |
| stop mailbox | voice 슬롯마다 `{generation, fadeFrames}` 원자 값 | 포화가 없다. 같은 generation의 반복 stop은 한 번만 처리 |
| scene·device epoch | stop-all·World 제거용 원자 epoch | — |
| plan 교체 | 미리 만든 버스 graph plan(3.14.4절)을 원자 포인터로 게시, 미확인 교체는 `MaxPendingPlans`(= 2)까지 | 제어 쪽이 다음 프레임으로 미룬다 |

`AudioRtCommand`는 고정 크기 POD다. 문자열·Object·소유 포인터를 담지 않는다. PCM·DSP 상태는 제어 쪽이 미리 만들고 pin한 메모리를 가리키는 인덱스로만 넘긴다.

#### 3.5.3 AudioRT → 제어 pipe

- **terminal 슬롯**: voice 슬롯마다 종료 사유(자연 종료·정지·steal·디코드 굶음·장치 정지·World 제거)와 마지막 사용 epoch를 적는다. 일반 이벤트 링이 차도 terminal 기록은 제어 쪽이 수거할 때까지 남는다.
- **이벤트 링**: 사전 할당 숫자 이벤트(재생 시작 frame, 늦은 frame 수, underrun, clipping, NaN 대체) + 버린 수.
- **epoch ACK**: AudioRT가 블록을 끝낼 때마다 처리한 epoch를 게시한다. plan·PCM·DSP 상태는 마지막 사용 epoch 이후 ACK가 오기 전에는 해제·재사용하지 않는다(계획 §18).

#### 3.5.4 Game으로 가는 결과

재생마다 결과는 두 번 이하로 온다: 시작(시작 frame 또는 시작 실패 사유)과 terminal(종료 사유). 둘 다 Game lane에서 받는다. 자연 종료·명시적 정지·steal·디코드 오류·World 종료가 겹쳐도 terminal은 하나만 낸다. 부착 소유자(`AudioComponent`)가 먼저 사라졌으면 `WeakObjectRef` 확인 뒤 버린다. 결과 콜백은 Game lane의 `OnFrameBegin`에서 부른다.

#### 3.5.5 수명 순서

1. 제어 쪽이 voice를 해제하기로 하면 stop mailbox를 쓴다.
2. AudioRT가 fade를 마치고 terminal 슬롯에 적는다.
3. 제어 쪽이 terminal을 수거하고, 그 voice가 쓰던 PCM 페이지·디코드 요청을 취소하고, 디코드 취소 완료와 AudioRT ACK(마지막 사용 epoch 이후)를 확인한다.
4. 그 뒤에만 voice 슬롯·PCM pin·`AssetLease`를 Game에 돌려준다.

### 3.6 믹서 블록 처리

UE `FMixerDevice::OnProcessAudioStream`(`Source/Runtime/AudioMixer/Private/AudioMixerDevice.cpp:1666`)의 순서는 명령 큐 소비 → Quartz clock 갱신 → `SourceManager->ComputeNextBlockOfSamples()` → `MainSubmix->ProcessAudio` → endpoint submix → 계량이다. Rocks `AudioMixer::RenderBlock(AudioOutputView out)`도 같은 순서다.

1. 블록 시작: stop mailbox·epoch 관찰, 명령 링 소비(블록에 걸린 것만), plan 교체 확인, 파라미터 최신값 읽기.
2. voice마다(구간별):
   1. PCM 취득: 상주 PCM은 커서로 바로 읽고, 스트리밍·압축은 voice PCM 링에서 읽는다. 준비되지 않았으면 무음을 넣고 굶음을 센다(기다리지 않는다).
   2. 리샘플·피치: 원본 rate·pitch → mix rate. 소수 커서와 필터 이력을 블록 사이에 보존한다. loop는 `[loopBegin, loopEnd)` 원본 frame 경계에서 이어 붙인다.
   3. source 필터: LPF·HPF(차폐·거리 공기 흡수·사용자 값).
   4. gain: 재생 gain × 분류 gain × 거리 감쇠 × fade. 목표값으로 블록 안에서 선형 ramp한다(클릭 방지).
   5. 공간화: 2D는 채널 그대로(또는 지정 pan), 3D는 패너(3.13.2절)가 채널 gain 행렬을 정한다.
   6. 출력: dry를 대상 `MixBus`에, send를 각 send 대상 버스에 더한다.
3. 버스: plan의 위상 순서대로 자식·send 합산 → 효과 체인 → 부모로.
4. 마스터: 마스터 gain(`audio.master.volume`·비활성 창 볼륨·mute) → limiter → NaN·Inf를 0으로 바꾸고 센다.
5. 출력 형식 변환·채널 매핑 → 출력 링.
6. mix clock += Q, epoch ACK 게시, 계량값 게시.

출력이 무음이어도 모든 채널·frame을 채운다. 블록 처리 중 할당은 0이다(시험이 할당 hook으로 단언).

병렬 렌더: UE는 `au.UseRenderScheduler`로 source·bus 렌더를 task 그래프로 나눌 수 있다(`AudioMixer/Private/AudioRenderScheduler.cpp`). Rocks는 AU1–AU7을 단일 스레드로 하고, voice 수가 목표를 넘는 것이 측정되면 AU9에서 AudioRT 보조 worker(고정 수, 블록 안 fork-join, 무할당)를 검토한다.

### 3.7 출력 backend와 장치

#### 3.7.1 인터페이스

```cpp
struct AudioOutputRequest {
    uint32 sampleRate;          // 0 = 장치 기본
    ChannelLayout layout;       // 원하는 레이아웃
    uint32 callbackFrames;      // 믹서가 Q를 넣는다. backend가 다른 값을 고르면 협상 결과로 알린다
    AudioDeviceSelector device; // 기본 장치 또는 장치 id
};

struct AudioOutputFormat {      // 협상 결과
    uint32 sampleRate;
    ChannelLayout layout;
    uint32 callbackFrames;
    AudioSampleFormat sampleFormat; // Float32 | Int16
};

class AudioOutputBackend {
public:
    virtual ~AudioOutputBackend() = default;
    virtual Result<AudioOutputFormat> Open(const AudioOutputRequest&, AudioOutputCallback&) noexcept = 0;
    virtual Result<void> Start() noexcept = 0;
    virtual Result<void> Stop() noexcept = 0;   // 돌아오면 콜백이 다시 불리지 않는다
    virtual void Close() noexcept = 0;
    virtual Result<uint32> EnumerateDevices(Span<AudioDeviceInfo>) noexcept = 0;
};
```

- `AudioOutputCallback::Fill(AudioOutputView, uint32 frames) noexcept`가 장치 콜백에서 불린다. 이 함수는 출력 링에서 복사만 한다.
- `Stop`의 계약은 "돌아온 뒤 콜백이 진행 중이지 않고 다시 불리지 않는다"이다. 종료·장치 교체의 장벽이 이것이다(3.7.3절, 3.8.6절).
- 오류는 원래 OS·라이브러리 오류 코드를 보존한다(로깅 원칙).
- 공개 헤더에 miniaudio 타입을 두지 않는다.

#### 3.7.2 backend 구현

| backend | 모듈 | 플랫폼 | 단계 |
|---|---|---|---|
| `NullAudioOutput` | Audio | 모두 | AU2. 장치가 없거나 열기 실패면 이것으로 시간만 진행한다(UE `FMixerNullCallback`과 같다). 자체 타이머 스레드가 블록 기간마다 장치 콜백 역할(링에서 한 블록 소비 + `AudioWakeSignal` 깨우기)을 한다 |
| `OfflineAudioRender` | Audio | 모두 | AU1. 시험·오프라인 캡처 |
| `MiniaudioOutput` | AudioMiniaudio | Windows(WASAPI) → macOS(Core Audio) → Android(AAudio, 없으면 OpenSL ES) | AU2. miniaudio 저수준 장치 API만 쓴다. 실제 선택된 backend 이름·협상값을 로그에 남긴다 |

miniaudio의 고수준 engine·resource manager·decoder·node graph는 쓰지 않는다. Rocks 에셋·스트리밍·믹서와 책임이 겹친다. 장치 IO만으로 부족하다는 측정(장치 교체 복구·지연·특정 기기 결함)이 나오면 같은 인터페이스로 플랫폼 직접 구현(WASAPI 등)을 더한다(AD4). UE는 플랫폼마다 직접 구현한다(Windows 기본 `AudioMixerWasapi`, Mac `AudioMixerCoreAudio`, iOS `AudioMixerAudioUnit`, Android `AudioMixerAndroid` — OpenSL ES만, Linux `AudioMixerSDL`).

#### 3.7.3 장치 상태와 교체

상태: `Closed → Opening → Running ⇄ Suspended → Running`, 실패나 분리는 `Reconfiguring` 또는 `Unavailable`(Null로 진행).

- 열기: 협상 형식 확인 → 출력 링·변환 버퍼 할당 → 초기 무음 → AudioRT 시작 → backend Start.
- 기본 장치 변경·분리·rate 변경 통지는 제어 pipe로 넘긴다. 순서: backend `Stop`(콜백 종료 확인) → AudioRT를 Null 타이머로 전환(시간 진행 유지) → 새 장치 열기·협상 → rate가 바뀌었으면 clock epoch를 올리고 예약을 초 단위로 다시 맞춤 → 링 재구성 → backend Start → 짧은 fade-in. UE `FAudioMixerPlatformSwappable`과 같은 흐름이다.
- 재시도는 제어 pipe 타이머(`audio.device.retryms`)로만 한다. 명시적으로 고른 장치는 OS 기본 장치 변경을 따라가지 않는다(사용자 설정).
- 앱 비활성·백그라운드·통화(모바일)·오디오 포커스는 PlatformLayer 앱 수명 이벤트(선행 PR9)로 받는다. 비활성 동안 `audio.unfocused.volume` 배수를 적용하고, 모바일 백그라운드는 Suspended로 간다.

### 3.8 엔진·World 연결

#### 3.8.1 subsystem

- `AudioEngineSubsystem`(EngineSubsystem): 장치를 만들고 가진다. `Initialize`에서 `RunTraits::audioOutput`이 false면 장치 없이 시작한다(모든 재생 요청이 "무시됨"). 등록 의존에 `EngineAssetsSubsystem`을 명시해 초기화는 뒤, 종료 drain은 앞에 오게 한다. Build.cs 의존만으로는 실행 순서가 정해지지 않는다.
- `AudioWorldSubsystem`(WorldSubsystem): World 하나의 `AudioSceneId`를 등록하고, 그 World의 emitter 표·리버브 존 표·기본 리버브를 가진다. World 해제 때 scene epoch를 올려 그 World의 재생·결과를 모두 끝낸다. 세션 음악처럼 World 교체를 넘는 재생은 엔진 소유로 시작한다(`AudioEngineSubsystem::Play2D(..., Ownership::Engine)`).
- 프레임 흐름: Game `OnFrameEnd`에서 각 World의 등록 emitter 위치·리스너·영역 판정 결과(3.15절)를 스냅샷으로 모아 제어 pipe에 제출한다. 파괴 요청까지 반영된 World 상태에서 뽑는다. 결과 수거와 콜백은 다음 `OnFrameBegin`.

#### 3.8.2 장치와 World

- 기본: 엔진에 장치 하나, 게임 World들이 공유한다(UE `EAudioDeviceScope::Shared`).
- Tryout: World마다 새 장치를 만드는 것이 기본이다(UE `bCreateNewAudioDeviceForPlayInEditor` 기본 true — `Source/Editor/UnrealEd/Private/Settings/SettingsClasses.cpp:470`, 설정 `Source/Editor/UnrealEd/Classes/Settings/LevelEditorMiscSettings.h:69`). 설정 `audio.tryout.newdevice`로 끌 수 있다. 장치마다 AudioRT 스레드가 하나씩이다. Tryout Stop은 그 장치를 닫고 모든 재생·lease를 정리한다(계획 §22.6).
- 에디터 미리듣기는 에디터 장치에서 별도 `MixBus`·동시 발음 그룹으로 격리한다(화면 마일스톤 뒤).
- 리스너: World마다 주 리스너 하나(플레이어 카메라 또는 명시 지정). 분할 화면은 가장 가까운 리스너 기준으로 감쇠·패닝한다(UE와 같다). 리스너 수만큼 같은 신호를 더해 커지는 경로는 만들지 않는다.

#### 3.8.3 재생 API

```cpp
// AudioWorldSubsystem
Result<PlaybackHandle> Play2D(SoundSourceRef sound, const PlayOptions& options) noexcept;
Result<PlaybackHandle> PlayAt(SoundSourceRef sound, const Vec3d& worldPosition, const SpatialPlayOptions& options) noexcept;
Result<PlaybackHandle> PlayAttached(AudioEmitterId emitter, SoundSourceRef sound, const PlayOptions& options) noexcept;
Result<void> SetParameter(PlaybackHandle handle, PlaybackParameter parameter, float value, uint32 rampFrames) noexcept;
Result<void> Stop(PlaybackHandle handle, uint32 fadeFrames) noexcept;
PlaybackStatus Status(PlaybackHandle handle) const noexcept;
```

- `PlayOptions`: 시작 시각(즉시 / mix frame / 다음 박 — AU10), volume·pitch 배수, 시작 offset, 동시 발음 재정의, 결과 콜백(Game lane).
- 즉시 실패: `InvalidArgument`(빈 사운드·NaN 위치), `CapacityExceeded`(재생 슬롯), `NotReady`(사운드 준비 정책이 `FailIfNotReady`일 때 — 3.12.4절), `ShuttingDown`(`BeginShutdown` 뒤 — 3.8.6절). 장치가 없으면 `Ignored` 상태의 유효 핸들을 돌려준다(`Status`가 `Ignored`, terminal은 다음 프레임에 `Ignored`).
- 발소리·UI음처럼 위치만 있으면 `PlayAt`/`Play2D`를 쓴다. 컴포넌트를 만들 필요가 없다(UE `PlaySoundAtLocation`·`PlaySound2D`와 같다).
- `AudioComponent`(SceneComponent): 사운드·감쇠·동시 발음·자동 재생·파라미터를 가진다. `OnRegister`에서 emitter 등록, `OnUnregister`에서 해제(멱등), `OnDisable`의 동작은 속성(`StopOnDisable` 기본 true)으로 정한다. 위치는 `SceneComponent::WorldMatrix()`에서 뽑는다. TRS 분해 실패(shear)는 이동 성분만 쓴다.

#### 3.8.4 무음 실행·서버·명령줄

UE `FApp::CanEverRenderAudio()`(`Source/Runtime/Core/Public/Misc/App.h:431–440`)는 `UE_SERVER`, `-nosound`(`-enablesound`가 없으면), 벤치마크, 전용 서버, 오디오를 허용하지 않은 commandlet에서 false다. `IsAudioMuted`(`-muteaudio`)는 엔진은 돌리고 마지막 출력만 끈다.

Rocks는 AppInfo `RunTraits`에 `audioOutput`·`audioMuted`를 더한다(AppInfoCommandLine 128행이 예약한 자리).
- `audioOutput` = false: Server 대상, `--no-sound`, commandlet(`--allow-commandlet-audio`가 없으면), 벤치마크. 장치를 만들지 않는다.
- `audioMuted` = true: `--mute-audio`. 장치·믹서는 돌고 마스터 gain만 0이다(오디오 분석에 기대는 게임 기능이 그대로 돈다 — UE 주석과 같은 이유).
- AppInfo 구현 전이면 AU3이 `EngineAudio` 안에 같은 판정을 두고, AppInfo 구현 때 옮긴다.
- UE `-enablesound`(`-nosound`를 되돌림)는 두지 않는다. Rocks 명령줄은 뒤의 값이 앞의 값을 덮으므로 필요 없다.

게임 진행은 재생 완료 통지에 기대면 안 된다(서버에서 오지 않거나 다음 프레임에 `Ignored`로 온다). Sparks 문서의 "서버에서 no-op"과 같다.

#### 3.8.5 설정

CVar(이름은 [CVar](CVarSystem.ko.md) 규칙 — 소문자 점 구분):

| 이름 | 기본 | 뜻 |
|---|---|---|
| `audio.master.volume` | 1.0 | 마스터 볼륨 |
| `audio.category.<이름>.volume` | 1.0 | 사용자 분류 볼륨(Music·Sfx·Dialogue·Ui) |
| `audio.unfocused.volume` | 0.0 | 창 비활성 배수(AppInfo §8.6이 넘긴 것) |
| `audio.mixer.blockframes` | 256 | Q |
| `audio.mixer.buffercount` | 2 | 출력 링 칸 수 |
| `audio.mixer.samplerate` | 0 | 0 = 장치 기본 |
| `audio.voices.max` | 32 | 실제 voice 상한(UE `MaxChannels` 기본 32 — `Source/Runtime/Engine/Classes/Sound/AudioSettings.h:93`) |
| `audio.device.retryms` | 1000 | 장치 재시도 간격 |
| `audio.tryout.newdevice` | true | Tryout 장치 분리 |
| `audio.stream.cachekb` | 32768(`DefaultStreamCacheKiB`) | 청크 캐시 상한. 품질 프로필(AU9)이 플랫폼별로 덮는다 |
| `audio.stream.defaultloading` | `PrimeOnLoad` | 분류 계층에서도 정하지 않은 `Inherited`의 최종 값(3.12.1절) |

장치 형식에 닿는 값(blockframes·buffercount·samplerate)은 장치 재구성 때 적용한다. 품질 프로필(voice 수·캐시 상한·Q)은 Config CF4 기기 프로필에서 고른다(선행 PR13).

#### 3.8.6 종료

`Engine::Shutdown` 순서 안에서:
1. `BeginShutdown`: 새 일반 `Play`를 닫는다. World 파괴가 내는 stop·detach는 받는다.
2. World 파괴: 각 World의 scene epoch를 올린다.
3. `CompleteInFlightWork`(AudioEngineSubsystem): stop-all epoch → AudioRT의 그 epoch ACK 대기(장치가 정상일 때). 장치가 이미 정지·분리됐으면 backend `Stop` 반환을 장벽으로 쓴다(블록이 더 나오지 않는 장치에서 epoch를 기다리면 종료가 멈춘다). → backend `Stop`(콜백 종료) → AudioRT 종료 플래그 설정 → 깨우기 신호 Signal(링이 차서 대기 중인 AudioRT를 깨운다) → AudioRT join → `NullAudioOutput`을 쓰던 중이면 그 타이머 스레드도 같은 순서(플래그 → 깨우기 → join) → 디코드·IO 취소 완료 → PCM pin·`AssetLease` 반환.
4. `Deinitialize`: backend `Close`, 링·plan·DSP 상태 해제.

대기에는 상한(`ShutdownDrainTimeoutMs`)을 두고, 넘으면 오류를 돌려준다. 이때 지금의 `KeepFirst` 누적 후 정리 계속 흐름은 콜백이 살아 있을 수 있는 소유자를 풀어버린다. 그래서 Engine 종료에 재시도 게이트가 필요하다(선행 PR8): 오디오 `CompleteInFlightWork` 실패면 `AssetManager` 종료·subsystem 해제로 넘어가지 않고 재시도하거나, 재시도할 수 없으면 소유자를 해제하지 않은 채 `platform::Fatal`로 끝낸다(해제 뒤 접근보다 명시 실패가 낫다). 이 게이트의 정확한 계약은 [엔진 런타임](EngineRuntime.ko.md)·[크래시·실행](CrashHandlingAndLaunch.ko.md) 개정으로 정한다.

### 3.9 사운드 에셋

모두 리플렉션 클래스(`ROCKS_CLASS`)이고 `EngineAudio`에 둔다.

| 클래스 | 주요 필드 | 단계 |
|---|---|---|
| `SoundSource`(추상) | 분류(`SoundCategory` 참조), 감쇠(`AttenuationSettings` 참조 또는 인라인), 동시 발음(`ConcurrencyGroup` 참조 목록), 우선순위, 가상화 모드, 출력 버스·send 목록, 최대 거리 | AU3 |
| `SoundClip` | 임포트 결과: 채널 레이아웃, 원본 rate, 총 frame, loop 구간·cue 목록, 원본 코덱(`sourceCodec`), PCM bulk(에디터 데이터). 설정: 압축 형식(`PlatformDefault`·`Pcm`·`Adpcm`·`Opus`), 품질, 로딩 동작, 플랫폼별 rate 재정의, looping. 쿠킹 데이터: 플랫폼 데이터 헤더 + 청크 | AU3(메모리 PCM 시험 생성), AU4(임포트), AU5(쿠킹) |
| `AttenuationSettings` | 거리 곡선 모양(선형·로그·역수·자연 음·사용자 커브)·내부 반경·감쇠 거리, 공간화 여부·spread, 공기 흡수 LPF 거리 곡선, 거리별 리버브 send, 우선순위 거리 감쇠 | AU3(거리 곡선), AU7(나머지) |
| `SoundCategory` | 부모, volume, pitch, 기본 출력 버스, UI 분류 여부(일시정지 무시) | AU7 |
| `MixSnapshot` | 분류 조정 목록(volume·pitch 배수), fade in/out 시간, 지속 시간 | AU7 |
| `ConcurrencyGroup` | 최대 수, 해결 규칙(3.13.3절), 재정의 시 볼륨 감쇠(ducking) | AU7 |
| `MixBus` | 부모, 효과 체인, 채널 레이아웃(부모와 같음 기본), 출력 volume | AU7 |
| `ReverbPreset` | 3.15절 | AU7 |
| `SoundEvent` | 노드 그래프(랜덤·레이어·switch·지연·loop) | AU10 |

에셋 경계(쿠킹): 에디터 전용 필드(원본 PCM bulk, 임포트 기록)는 쿠킹 패키지에 넣지 않는다. 쿠킹된 실행 파일은 플랫폼 데이터(3.11절)만 본다.

에디터·Tryout 재생(AD25): 쿠킹하지 않은 `SoundClip`은 재생 요청 때 실행 플랫폼의 플랫폼 데이터를 만든다. UE도 에디터에서 DDC로 실행 플랫폼 데이터를 만들어 재생한다. AU4(코덱 전)에는 원본 PCM bulk를 PCM 플랫폼 데이터로 메모리에서 바꾸는 변환만 있고, AU5부터는 쿠킹 처리기와 AssetNugget을 거친다(같은 키면 캐시 적중). 만드는 동안 재생 요청은 준비 정책(3.12.4절)을 따른다.

`AudioComponent`·`ReverbZoneComponent`는 World 컴포넌트다(3.8.3절, 3.15절).

### 3.10 임포트

#### 3.10.1 원본 형식

**사용자 결정(2026-11-22): 오디오는 무손실·손실 원본을 모두 받는다.** [에셋 시스템](AssetSystem.ko.md) 12.2절의 텍스처 무손실 원칙(`LossySource` 거부)은 오디오에 적용하지 않는다(AD5).

| 확장자 | 형식 | 해석 | UE |
|---|---|---|---|
| `wav` | RIFF/RIFX WAVE: PCM 8·16·24·32비트 정수, IEEE float 32·64, `WAVE_FORMAT_EXTENSIBLE`, 1–8채널. `smpl`(loop)·`cue `·`LIST adtl`(marker 이름) chunk | 자체 파서 | in-engine(`SoundFactory.cpp:152`) |
| `aif`·`aiff` | AIFF·AIFF-C(비압축 `NONE`·`sowt`·`fl32`) | 자체 파서 | sndfile |
| `flac` | FLAC | libFLAC | sndfile |
| `ogg` | Ogg Vorbis | libvorbis + libogg | sndfile |
| `opus` | Ogg Opus | libopus + libopusfile + libogg | sndfile |
| `mp3` | MPEG-1/2 Layer III | minimp3 | sndfile |

UE는 WAV가 아니면 libsndfile로 WAV로 바꾼 뒤 WAV 경로로 임포트하고, 16비트가 아닌 WAV도 16비트로 바꾼다. 원본 bulk(`RawData`)에는 그 **16비트 교차 WAV byte(헤더 포함)**를 저장한다(`Source/Editor/AudioEditor/Private/Factories/SoundFactory.cpp:200–225, 395–415, 652, 667`). Rocks는 디코드한 PCM을 저장하고 원본 파일 byte를 보관하지 않는 점은 같지만, 정밀도와 배치는 다르다(AD12, 7절).
- 원본 bulk = 디코드한 PCM, 채널 비교차(planar), 헤더 없음(형식은 `SoundClip` 필드). 저장 형식: 8비트(부호 없는) → int16 변환, 16비트 → int16, 24비트 → int24 packed, 32비트 정수·32/64비트 float·손실 디코드 결과 → float32. UE처럼 16비트로 줄이지 않는다. 24비트·float 원본의 정밀도를 쿠킹까지 유지하기 위해서다.
- 디코드한 PCM 상한 `MaxSoundClipPcmBytes` = 2 GiB(float32 스테레오 48 kHz로 약 93분). 넘으면 `InvalidSource` / CapacityExceeded. A2f 원본 파일 상한(1 GiB)만으로는 압축 원본이 풀린 뒤 크기를 막지 못한다.
- 원본 파일 byte는 보관하지 않는다. 원본 파일 경로·SHA-256은 A2f `ImportRecord`가 이미 기록한다(재임포트는 원본 파일에서 한다).
- `sourceCodec`(Pcm·Flac·Vorbis·Opus·Mp3 등)을 `SoundClip`에 적는다. 손실 원본을 다시 손실 코덱으로 쿠킹하면 세대 손실이 생기므로 `asset-info`에 보인다. 경고 진단은 내지 않는다(UE도 내지 않는다).
- 손실 디코드의 encoder delay·끝 padding은 디코더가 알려주는 값으로 잘라낸다(Opus pre-skip, MP3 LAME/Xing 헤더 gapless 정보, Vorbis granule). 그래서 frame 수가 원래 길이와 같다(시험).

libsndfile은 LGPL이라 쓰지 않는다. 디코더 라이브러리는 Developer(`AudioTools`) 전용이고 런타임 실행 파일에 링크하지 않는다(Opus는 클라이언트 런타임 `AudioOpus`가 따로 링크한다).

결정성: 임포트 bulk의 SHA와 AssetNugget 키가 기기마다 같아야 한다. float 경로가 CPU 기능에 따라 갈리는 라이브러리(libopus RTCD 등)는 런타임 CPU 탐지를 끄고 플랫폼마다 SIMD 수준을 빌드 때 고정한다(x86-64는 SSE2 기준, ARM64는 NEON). 시험이 같은 원본의 디코드 결과 SHA를 고정값으로 단언한다.

#### 3.10.2 임포터

`SoundClipImporter`는 A2f `AssetImporter`를 구현한다.
- `AssetImporterInfo`: 이름 `SoundClip`, 출력 타입 `SoundClip`, 기본 확장자 `wav`·`aif`·`aiff`·`flac`·`ogg`·`opus`·`mp3`, roles는 빈 목록(A2f 정규형 — 원본 하나 `source`), 옵션 struct `SoundClipImportOptions`.
- 옵션(`SoundClipImportOptions`, 바뀌면 재임포트): `channelHandling`(Keep·DownmixToMono), `loopFromFile`(파일 `smpl` loop·`cue `를 쓸지, 기본 true), `normalize`(bool, 기본 false)와 `normalizePeakDb`(float, 기본 −1.0 — `normalize`가 true일 때만 쓴다. ROCKS_STRUCT에 optional이 없어 bool 짝으로 둔다). 옵션 필드 이름은 `SoundClip` 노출 속성 이름·A2f 명령 예약 이름과 겹치면 안 된다(A2f 등록 규칙이 `InvalidArgument`로 막는다). 그래서 `SoundClip` 설정 속성과 같은 이름(`looping` 등)을 옵션에 쓰지 않는다.
- 임포트 결과로 쓰는 필드(매번 전부 쓴다 — A2f 계약): 채널 레이아웃, rate, 총 frame, 파일 loop·cue(`loopFromFile` = false거나 파일에 없으면 빈 값을 쓴다), `sourceCodec`, PCM bulk. 사용자가 고친 loop 구간은 임포트 필드가 아닌 설정 속성 `loopOverride`(사용 여부 + 구간)에 둔다. 그래야 재임포트가 사용자 편집을 덮지 않는다. 재생은 `loopOverride`가 켜져 있으면 그것을, 아니면 파일 loop를 쓴다.
- 다채널 파일 하나를 채널별 파일(`_L`·`_R` 등)로 나눠 받는 UE 방식은 받지 않는다(범위 밖).

진단: `InvalidSource`(chunk 경계·크기·길이 불일치, 디코드 실패, PCM 상한 초과)와 `UnsupportedSourceFormat`(지원하지 않는 WAV 부호화·AIFF-C 압축·9채널 이상·rate 범위 밖)는 A2f1a가 이미 등록했다. 이 임포터가 더하는 것은 경고 `IgnoredSourceChunks`(모르는 chunk) 하나이고, `AssetImportContext::Warn`은 경고로 등록된 코드만 받으므로 경고로 등록한다. 정상 rate 범위는 `MinSourceSampleRate` = 8000 – `MaxSourceSampleRate` = 192000이다.

### 3.11 코덱과 쿠킹

#### 3.11.1 인터페이스

```cpp
// AudioTools — 쿠킹 쪽
class AudioEncoder {
public:
    virtual const AudioCodecInfo& Info() const noexcept = 0;   // 이름, 버전, 지원 rate·채널
    virtual Result<EncodedAudio> Encode(const PcmView& pcm, const AudioEncodeSettings&) noexcept = 0;
};

// Audio — 런타임 쪽
class AudioDecoder {
public:
    virtual Result<void> Open(const AudioPlatformDataHeader&, Span<const uint8> firstChunk) noexcept = 0;
    virtual Result<uint32> Decode(Span<const uint8> chunk, PcmWriteView out) noexcept = 0;  // 쓴 frame 수
    virtual Result<void> SeekToChunk(uint32 chunkIndex, uint32 preRollFrames) noexcept = 0;
};
```

UE `IAudioFormat`(Cook·CookSurround·SplitDataForStreaming·ExtractSeekTableForStreaming — `Source/Developer/TargetPlatform/Public/Interfaces/IAudioFormat.h`)와 `ICompressedAudioInfo`(`Source/Runtime/Engine/Public/AudioDecompress.h`)·`IAudioInfoFactoryRegistry`에 해당한다. 디코더는 `AudioDecoderTable`에 코덱 id로 등록한다(모듈 시작 때, 전역 생성자 없이).

#### 3.11.2 코덱

UE `ESoundAssetCompressionType`(`Source/Runtime/Engine/Classes/Sound/SoundWave.h:348`)은 BinkAudio·ADPCM·PCM·Opus·RADAudio·PlatformSpecific·ProjectDefined다. 새 SoundWave는 `ProjectDefined`(`Source/Runtime/Engine/Private/SoundWave.cpp:1056`)이고 이것이 `DefaultAudioCompressionType = BinkAudio`(`Source/Runtime/Engine/Private/AudioSettings.cpp:32`)로 풀리므로 UE 기본은 모든 플랫폼에서 BINKA다. 플랫폼 ini의 형식(Mac·Android OGG, iOS ADPCM)은 `PlatformSpecific`을 고른 경우에만 쓰인다. Rocks는 PCM·ADPCM·Opus를 둔다(AD6). Bink·RAD는 독점 코덱이고, Vorbis는 Opus가 같은 자리를 더 낮은 bitrate로 맡는다.

| 코덱 | 용도 | 비고 |
|---|---|---|
| PCM16 | 아주 짧은 SFX, 디코드 비용 0 | |
| ADPCM | 짧은 SFX, 약 4:1, 디코드 아주 쌈 | 블록 형식은 UE ADPCM 디코더(`Source/Runtime/AdpcmAudioDecoder`)를 AU5에서 확인해 정한다 |
| Opus | 음악·대사·긴 환경음 | rate는 48 kHz만 쓴다(Opus 내부 rate). 다른 원본은 쿠킹 때 48 kHz로 리샘플한다. 3채널 이상은 Opus multistream(채널 매핑 family 1, 디코드 뒤 내부 순서로 바꿈 — 3.3절) |

플랫폼 기본(`PlatformDefault`가 고르는 것, 프로젝트 설정으로 바꿀 수 있다): Windows·macOS·Android = Opus, 길이 `ShortClipSeconds`(= 1.0) 미만은 ADPCM. UE 기본(모든 플랫폼 BINKA)과 다르다(7절). iOS는 보류 플랫폼이다.

Opus 48 kHz 리샘플의 결과:
- loop·cue 위치는 `round(frame × 48000 / srcRate)`(가까운 정수, 동률은 짝수)로 바꾼다. loop 길이가 0이 되면 1 frame으로 둔다. 쿠킹 시험이 경계값을 확인한다.
- 3.11.3절의 rate 상한 표는 PCM·ADPCM에만 쓴다. Opus는 항상 48 kHz다.
- 쿠킹 리샘플러는 런타임 리샘플러와 다른 고품질 필터(band-limited)이며, 그 버전을 AssetNugget 키에 넣는다.

#### 3.11.3 플랫폼 데이터

쿠킹 산출물(`SoundClip` 플랫폼 데이터):
- 헤더: 형식 버전, 코덱 id·버전, 채널 레이아웃, 인코딩 rate, 유효 PCM frame 수, loop 구간(인코딩 rate 기준), encoder delay·끝 padding, 청크 수, 로딩 동작, 첫 청크 상주 여부.
- 청크 표: 청크마다 payload 위치·크기, 첫 frame·frame 수, 독립 디코드 시작 여부, 필요한 pre-roll frame, SHA-256(서명 프로필에서 manifest가 덮는다).
- 청크 크기: 목표 `TargetChunkBytes`(= 256 KiB, 플랫폼 프로필로 바꿈). 청크 경계는 코덱 프레임 경계다.
- 청크 독립성: 모든 청크는 그 청크만으로 디코드를 시작할 수 있다. Opus는 앞 청크 끝의 패킷을 `OpusPreRollMs`(= 80 ms, RFC 7845 권장값)만큼 이 청크 앞에 **복제**해 두고, 디코더는 그 구간을 디코드해 버린다(`preRollFrames`). 앞 청크를 읽지 않아도 seek·재시작이 되며, 대가는 청크마다 약 80 ms 분량의 byte다. ADPCM·PCM은 블록 독립이라 pre-roll이 없다.
- loop 이음매: loop 끝에서 처음으로 돌아갈 때 디코더가 pre-roll을 다시 디코드하면 그 블록에 출력이 늦는다. 그래서 looping clip은 재생 시작 때 loop 머리 `LoopHeadFrames`(= pre-roll + 2 × Q 이상) PCM을 디코드해 voice 링 옆에 두고, 디코드 worker는 loop 끝에 닿기 전에 loop 시작 청크로 미리 seek한다.
- 플랫폼 재정의(UE `FPlatformAudioCookOverrides`): rate 상한 표(최대·높음·중간·낮음·최소 — PCM·ADPCM만), 품질 배수, 자동 스트리밍 길이 임계, 첫 청크 인라인.

#### 3.11.4 AssetNugget 키

키 = AssetSystem의 공통 너겟 키(툴 빌드 식별 등 — AssetSystem 508행 부근) + 원본 PCM SHA-256 + 쿠킹에 닿는 설정(코덱·품질·rate·loop·`loopOverride`·로딩 동작·채널 처리) + 플랫폼 재정의 + 인코더 이름·버전·고정 SIMD 수준 + 쿠킹 리샘플러 버전 + 플랫폼 데이터 형식 버전. UE `FDerivedAudioDataCompressor`(`Source/Runtime/Engine/Private/AudioDerivedData.cpp`)가 DDC 키로 같은 일을 한다. 같은 입력은 같은 byte를 낸다(인코더 결정성 — 멀티스레드 인코더 옵션 끔).

#### 3.11.5 패키지 게시

- 상주 clip(`ForceInline`·짧은 clip): 플랫폼 데이터 전체를 패키지 bulk에 넣는다.
- 스트리밍 clip: 첫 청크(선택)만 패키지에, 나머지 청크는 쿠커가 만드는 오디오 sidecar 파일에 넣는다. 청크 파일을 먼저 게시·검증하고 manifest를 마지막에 게시한다. 기존 revision은 참조가 끝날 때까지 둔다.
- Server 대상 쿠킹은 오디오 payload를 넣지 않고 헤더만 남긴다.

### 3.12 로딩과 스트리밍

#### 3.12.1 로딩 동작

UE `ESoundWaveLoadingBehavior`(`Source/Runtime/Engine/Classes/Sound/SoundWaveLoadingBehavior.h:9–30`)와 같은 다섯을 둔다. `Inherited`는 UE처럼 분류 계층(`SoundCategory` 부모 방향)에서 처음 정해진 값을 쓰고, 끝까지 없으면 CVar `audio.stream.defaultloading`을 쓴다.

| 값 | 뜻 |
|---|---|
| `Inherited` | 분류 계층 → CVar 순으로 물려받음(기본) |
| `RetainOnLoad` | 로드 때 첫 청크를 캐시에 넣고 에셋이 살아 있는 동안 고정 |
| `PrimeOnLoad` | 로드 때 첫 청크를 캐시에 넣되 고정하지 않음 |
| `LoadOnDemand` | 재생 요청 때 읽음 |
| `ForceInline` | 플랫폼 데이터 전체가 패키지에 상주(스트리밍 안 함) |

#### 3.12.2 청크 캐시

`AudioChunkCache`(제어 pipe 소유, UE `FCachedAudioStreamingManager`·`FAudioChunkCache` — `Source/Runtime/Engine/Public/AudioStreamingCache.h`):
- 상한 `audio.stream.cachekb`. 압축 청크를 (clip revision, 청크 번호)로 공유하고 LRU로 회수한다. pin된 청크(재생 중·고정)는 회수하지 않는다.
- 읽기: `AsyncReadFile::SubmitRead(request, offset, buffer, bytes, …)`(`Core/Source/Runtime/PlatformBase/Public/Platform/AsyncFile.h`) → 길이·SHA 검증 → 캐시 게시. 짧은 읽기·IO 오류·hash 불일치는 그 청크 실패(재생은 `DecodeStarved` 또는 terminal `ChunkError`).
- 상한에 닿고 모두 pin이면 새 스트림 수락을 줄인다(`CapacityExceeded` 또는 우선순위 낮은 스트림 가상화).

#### 3.12.3 디코드

- voice마다 PCM 링(원본 rate frame 단위). 용량 = `2 × max(디코더 최대 출력 단위, ceil(Q × MaxPitchScale × srcRate / mixRate))`. Opus 최대 출력 단위는 패킷 최대 길이 120 ms = 5760 frame, ADPCM은 블록 frame 수, PCM은 Q다. `MaxPitchScale` = 2.0(UE `GlobalMaxPitchScale` 기본 — `Source/Runtime/Engine/Private/AudioSettings.cpp:30`). 디코드 worker가 다음 구간을 채운다. 시작 water mark·재충전 water mark는 품질 프로필 값이다.
- 디코더 인스턴스 하나에 작업 하나만 돈다. seek·stop 뒤 늦게 끝난 디코드는 (voice generation, seek 번호) 불일치로 버린다.
- AudioRT는 디코드를 기다리지 않는다. 링이 비면 무음을 넣고 굶음을 센다. 음악은 굶은 동안 transport를 진행할지(건너뜀) 멈출지(이어 재생)를 `SoundClip` 설정으로 정한다.
- 예산 예: 48 kHz 스테레오 float32는 초당 384,000 B다. 3분 곡을 풀면 69.12 MB다. Opus 48 kHz 스테레오 스트림의 PCM 링은 2 × max(5760, 512) = 11,520 frame × 2채널 × 4 B = 92,160 B이고, 스트리밍 voice 8개면 737,280 B(720 KiB)다. 여기에 디코더 상태·압축 청크·IO 진행분이 더해진다.

#### 3.12.4 재생 준비

에셋의 `ReadyMask`(Cpu·Render)는 "객체가 공개됐다"는 뜻이라 "이 clip의 frame N부터 즉시 낼 수 있다"를 표현하지 못한다. 같은 clip의 서로 다른 seek 구간 둘을 동시에 준비하는 경우도 있다. 그래서 오디오는 별도 준비 티켓을 둔다(계획 AAA10, 선행 PR12).

```cpp
Result<AudioReadyTicket> PreparePlayback(SoundSourceRef sound, uint64 startFrame, uint32 preRollFrames) noexcept;
AudioReadyState Query(AudioReadyTicket) const noexcept;
// Pending | Ready | MissedDeadline | Unavailable | Cancelled | Failed (계획 AAA10의 결과 집합)
```

`MissedDeadline`은 요청 기한까지 준비하지 못함, `Unavailable`은 장치 없음·서버·사운드 없음, `Cancelled`는 호출자 취소·World 제거, `Failed`는 IO·디코드 오류다.

재생 준비 정책(`PlayOptions::notReady`): `StartWhenReady`(기본 — 준비되면 시작, 늦은 frame 보고), `FailIfNotReady`, `SkipToNow`(음악 transport 유지). 박자에 맞춘 음악 전환은 준비가 끝난 뒤에만 예약을 수락하고, 늦으면 `MissedDeadline`을 돌려준다.

### 3.13 감쇠·공간화·동시 발음·가상화

#### 3.13.1 좌표와 감쇠

- 좌표·단위는 UE와 같다(+X 전방, +Y 오른쪽, +Z 위, cm).
- 제어 pipe가 리스너 기준 상대 위치를 double로 빼고 float로 내린다(큰 월드).
- 거리 감쇠: `AttenuationSettings`의 곡선(UE `EAttenuationDistanceModel`: Linear·Logarithmic·Inverse·LogReverse·NaturalSound·Custom)을 내부 반경까지 1, 감쇠 거리 끝에서 0(또는 dB 하한)으로 적용한다. 감쇠 모양은 구(Sphere)부터 하고 캡슐·상자·원뿔은 AU7.
- 공기 흡수: 거리에 따라 LPF 차단 주파수를 낮춘다(AU7).
- 단위 변환이 필요한 외부 공간화 플러그인에만 `CentimetersToMeters` 등(`Core/Source/Runtime/Core/Public/Core/Math/UnitConversion.h`)을 쓴다.

#### 3.13.2 패닝

UE 기본 패너 `FMixerDevice::Get3DChannelMap`(`AudioMixerDevice.cpp:2983`)은 방위각으로 인접한 두 스피커 사이를 equal-power로 나눈다. Rocks도 같다.
- 스테레오: 리스너 right 축 투영으로 pan ∈ [−1, 1], `θ = (pan + 1)π/4`, `L = cos θ`, `R = sin θ`.
- +Y 위치는 오른쪽이 커야 하고, 리스너를 180° 돌리면 반대여야 한다(시험).
- 영거리·내부 반경 안에서는 spread(비공간화 비율)를 늘려 양쪽에 고르게 나눈다.
- 스테레오 원본의 3D 재생은 채널별 위치 spread로 한다(UE stereo spread).
- 다채널 출력 패닝은 AU9.

#### 3.13.3 동시 발음과 voice 상한

UE는 동시 발음 그룹 평가(`FSoundConcurrencyManager`)와 장치 전체 voice 상한(`MaxChannels`, 정렬 뒤 초과분 탈락 — `FAudioDevice::GetSortedActiveWaveInstances`)을 다른 단계로 둔다. Rocks도 같다.

1. 재생 수락 때 `ConcurrencyGroup`마다 최대 수를 검사한다. 넘으면 해결 규칙(UE `EMaxConcurrentResolutionRule` — `Source/Runtime/Engine/Classes/Sound/SoundConcurrency.h:31`)을 따른다: `PreventNew`, `StopOldest`, `StopFarthestThenPreventNew`, `StopFarthestThenOldest`, `StopLowestPriority`, `StopQuietest`, `StopLowestPriorityThenPreventNew`. 그룹이 여럿이면 모두 통과해야 하고, 희생자는 모든 그룹 검사가 끝난 뒤 확정한다.
2. 매 갱신마다 실제 voice가 필요한 재생층을 (우선순위 × 들리는 크기 추정) 내림차순, 동률은 수락 순번으로 정렬해 `audio.voices.max`까지 voice를 준다. 나머지는 가상(3.13.4절)이거나 종료다.
3. voice steal은 짧은 release ramp(`StealFadeFrames`)를 쓰고, 그 ramp 동안의 voice도 상한에 센다.

#### 3.13.4 가상화

UE `EVirtualizationMode`(`Source/Runtime/Engine/Classes/Sound/SoundBase.h:57`): Disabled, PlayWhenSilent, Restart, SeekRestart(UE 5.8에서 `UE_EXPERIMENTAL(5.6)`). Rocks도 같은 넷을 둔다. SeekRestart는 Rocks에서 정식 기능이다(논리 커서를 이미 유지하므로 추가 비용이 seek뿐이다).
- 들리지 않거나 voice 경쟁에서 진 looping 재생은 voice를 놓고 논리 커서만 진행한다(`FAudioVirtualLoop`과 같다).
- 다시 들리면 모드대로 처음부터(Restart) 또는 논리 커서 위치에서(SeekRestart) 짧은 fade-in으로 복귀한다.
- 경계 왕복으로 voice가 들락거리지 않게 들리는 크기 임계에 hysteresis와 최소 유지 시간을 둔다.
- 무음으로 계속 렌더하는 방식은 가상으로 세지 않는다(디코드·DSP 비용이 그대로다).
- 짧은 one-shot은 가상화하지 않고 종료한다(Disabled 기본).

### 3.14 버스·분류·믹스·효과

#### 3.14.1 버스 graph

UE는 Main·BaseDefault·Reverb·EQ 필수 submix를 둔다(`ERequiredSubmixes` — `Source/Runtime/AudioMixer/Public/AudioMixerDevice.h:32`, 옛 `EMasterSubmixType`은 deprecated). Rocks 기본 graph:

```
Master
 ├─ Music
 ├─ Sfx
 ├─ Dialogue
 ├─ Ui
 └─ ReverbReturn   (리버브 존 리버브가 여기 산다 — 3.15절)
```

프로젝트가 `MixBus` 에셋으로 버스를 더한다. 각 재생은 dry를 기본 버스(분류의 기본 버스 또는 사운드 지정)로, send를 `ReverbReturn`과 지정 버스로 보낸다. send는 거리 감쇠 전/후(pre/post)를 고른다(UE와 같다).

#### 3.14.2 분류와 믹스 스냅샷

- `SoundCategory` 계층: 자식 volume·pitch = 부모 값 × 자기 값. 사용자 볼륨 설정(CVar)은 분류에 곱한다.
- `MixSnapshot`: push·pop으로 적용하고, 조정값을 fade 시간 동안 보간한다. 여러 스냅샷이 겹치면 곱한다. 대사 중 음악 낮추기(ducking)가 대표 용도다. 사용자 볼륨 설정은 바꾸지 않는다.

#### 3.14.3 효과

| 효과 | 단계 | UE |
|---|---|---|
| 1극·2극 LPF/HPF | AU1(source), AU7(버스) | SignalProcessing 필터 |
| gain·limiter | AU1 | DynamicsProcessor |
| EQ(파라메트릭 4밴드) | AU7 | `FSubmixEffectSubmixEQ` |
| compressor | AU7 | `FSubmixEffectDynamicsProcessor` |
| 알고리즘 리버브(plate) | AU7 | `FSubmixEffectReverb` → `FPlateReverbFast` |
| delay | AU7 | Synthesis 플러그인 |
| convolution 리버브 | AU10 | Synthesis 플러그인 |

효과 인터페이스(`AudioEffect`)는 `Prepare(format, maxFrames)`(제어 쪽, 할당), `Process(in, out, frames) noexcept`(AudioRT, 무할당), `SetParameters`(최신값 슬롯), `TailFrames()`를 가진다.

#### 3.14.4 plan 교체

버스 추가·제거·효과 체인 변경은 제어 pipe가 새 plan(위상 정렬 순서, 버퍼 배정, 효과 상태 블록)을 만들어 게시하고, AudioRT가 블록 경계에서 바꾼다(계획 §18). send·side-chain 순환은 plan을 만들 때 거부한다. 같은 효과가 옛 plan과 새 plan에 다 있으면 상태를 넘긴다(잔향 유지). 효과 체인 자체를 바꿀 때는 UE `SetSubmixEffectChainOverride`(`AudioMixer/Private/AudioMixerSubmix.cpp:852–885`)처럼 옛 체인과 새 체인을 fade 시간 동안 함께 돌려 교차 페이드한다. 옛 plan은 ACK 뒤 해제한다.

### 3.15 리버브 존

동굴·실내·터널처럼 공간에 따라 울림을 바꾼다. UE는 Audio Volume(`AAudioVolume` + `UReverbEffect`)과 AudioGameplayVolume 플러그인의 `UReverbVolumeComponent`가 맡는다.

#### 3.15.1 UE 5.8 동작

- 프리셋 `UReverbEffect`(`Source/Runtime/Engine/Classes/Sound/ReverbEffect.h`): bBypassEarlyReflections, ReflectionsDelay, GainHF, ReflectionsGain, bBypassLateReflections, LateDelay, DecayTime, Density, Diffusion, AirAbsorptionGainHF, DecayHFRatio, LateGain, Gain, RoomRolloffFactor.
- 영역 선택: 오디오 pipe의 `FAudioDevice::UpdateAudioVolumeEffects`(`Source/Runtime/Engine/Private/AudioDevice.cpp:4923–4997`)가 리스너 위치로 영역을 고른다. 겹치면 Priority가 높은 쪽, 영역 밖이면 World 기본값, `ActivateReverbEffect`(4058)로 켠 리버브가 더 높으면 그것이다. AudioGameplayVolume의 `FProxyMutator_Reverb::Apply`도 결국 `ActivateReverbEffect`를 부른다(`Plugins/AudioGameplayVolume/Source/AudioGameplayVolume/Private/ReverbVolumeComponent.cpp:16–32`).
- 전환: `FAudioEffectsManager::SetReverbSettings`(`Source/Runtime/Engine/Private/AudioEffect.cpp:396–441`)가 현재 값을 시작점, 새 프리셋을 끝점, 끝 시각을 현재 + FadeTime으로 잡는다. 리버브를 끄면 끝점 Volume이 0이다. `FAudioReverbEffect::Interpolate`(149–210)가 오디오 갱신마다 벽시계(`FApp::GetCurrentTime`) 진행률로 모든 파라미터를 선형 보간한다. bypass 쪽 값은 0으로 보고 보간한다. 리버브 인스턴스는 마스터 리버브 submix의 `FSubmixEffectReverb` 하나다.
- 적용: `FSubmixEffectReverb::SetParameters`(`Source/Runtime/AudioMixer/Private/Effects/AudioMixerSubmixEffectReverb.cpp:182–227`)가 `FPlateReverbFast` 설정으로 바꾸고 다음 블록 시작에 반영한다. wet 양(Volume)은 리버브 입력에 블록 안 램프를 걸어 적용한다(140–180). 그래서 wet을 줄여도 쌓인 잔향 꼬리는 자연히 사라진다. `FPlateReverbFast::SetSettings`는 새 값을 바로 적용하고 샘플 단위 평활은 없다(`Source/Runtime/SignalProcessing/Private/ReverbFast.cpp:44–63`). 보간이 게임 틱마다 조금씩 오므로 한 번에 크게 바뀌지 않는다. 다만 early·late reflection이 꺼지는 순간 그 이력을 지운다(46–57). 그래서 bypass 프리셋으로의 전환이 끝나는 순간 남은 꼬리가 잘린다.
- 안팎: `FInteriorSettings`(`Source/Runtime/Engine/Classes/Sound/AudioVolume.h:75–113`)의 ExteriorVolume·ExteriorTime·ExteriorLPF·ExteriorLPFTime(리스너가 안에 있을 때 바깥 소리), InteriorVolume·InteriorTime·InteriorLPF·InteriorLPFTime(리스너가 밖에 있을 때 안쪽 소리).

#### 3.15.2 Rocks

| 하는 일 | 문맥 | 입력 → 출력 |
|---|---|---|
| 리스너·emitter의 영역 판정, 겹침 우선순위(동률은 안정 id), 경계 hysteresis | Game(`AudioWorldSubsystem`, `OnFrameEnd`) | 영역 형상(상자·구·볼록체)과 위치 → 영역 id. SpatialIndex 후보 검색 + 형상 내부 판정. 물리 엔진은 쓰지 않는다 |
| 프리셋 선택, 전환 시작(시작값·끝값·끝 frame), 재생별 wet send·안팎 gain·LPF 목표값 | 제어 pipe | 영역 id·거리 → AudioRT 명령 |
| 파라미터 보간, 리버브 DSP, 잔향 꼬리 유지 | AudioRT | 목표값만 받는다 |

- `ReverbPreset`: UE `UReverbEffect`와 같은 파라미터 이름·범위. 기본 프리셋(동굴·대성당·작은 방·복도·야외)을 엔진 콘텐츠로 준다.
- `ReverbZoneComponent`: 형상, 프리셋, wet 양(0–1), 전환 시간, 우선순위, `ZoneInteriorSettings`(UE `FInteriorSettings`와 같은 여덟 값).
- World 기본 리버브: World 설정에 프리셋(없음 가능)과 안팎 기본값.
- **전환(AD11)**: UE와 같이 리버브 하나의 파라미터를 전환 시간 동안 선형 보간한다. bypass 쪽은 0으로 보고 보간한다. wet은 리버브 입력에 램프를 걸어 잔향 꼬리를 보존한다. UE와 다른 점은 진행률을 벽시계가 아니라 mix clock frame으로 계산하고 AudioRT가 블록마다 진행하는 것이다(오프라인 렌더 결정성 — 7절).
- **bypass 지연(AD27)**: 전환 끝값이 early·late bypass이거나 리버브 없음이어도, 해당 단계는 출력 에너지가 `ReverbTailSilenceDb`(= −90 dBFS) 아래로 `ReverbTailHoldFrames`(= Q × 4) 동안 머문 뒤에야 끄고 이력을 지운다. UE는 전환 끝에 바로 지워 꼬리가 잘린다(위). 그 사이 다른 프리셋으로 다시 전환하면 이력을 유지한 채 보간을 시작한다.
- 거리별 리버브 send: `AttenuationSettings`의 거리 곡선으로 먼 소리의 wet 비율을 늘린다(UE와 같다).
- 차폐(벽에 막힘)는 별개 기능이다. 물리 레이 질의(선행 PR16)가 들어오기 전에는 고정 fixture로 계약만 시험한다. UE `FActiveSound::CheckOcclusion`(`Source/Runtime/Engine/Private/ActiveSound.cpp:1577`)은 게임 스레드 비동기 line trace 결과로 LPF·volume을 보간한다. Rocks도 질의는 Game/물리, AudioRT에는 목표값만 간다.
- convolution 리버브는 AU10이다. `ReverbPreset`이 알고리즘·convolution 둘 다 가리킬 수 있게 종류 필드를 처음부터 둔다(AU7에서는 알고리즘만 허용, convolution은 `UnsupportedSetting`).

### 3.16 진단

- trace 채널 `Audio`(제어 쪽: 재생 수락·거부·가상화·종료 사유)와 `AudioMixer`(AudioRT 숫자 이벤트: underrun·굶음·clipping·NaN·블록 처리 시간), 카운터(활성 voice·가상 재생·스트림·캐시 사용량). UE `AudioChannel`·`AudioMixerChannel`(`Source/Runtime/AudioMixerCore/Public/AudioMixerTrace.h`)에 해당한다. AudioRT는 사전 할당 숫자 링에 쓰고, 제어 pipe가 그것을 trace로 옮긴다(TraceSystem은 메모리 부족 때 기다리지 않고 버린다).
- 재생 하나 추적: `PlaybackHandle`로 사운드·revision·scene·수락 결과·예약/실제 시작 frame·디코드 선행량·가상/실제 전환·버스 gain·장치 epoch·종료 사유를 조회한다(`audio.playback <id>`).
- 콘솔 명령: `audio.voices`(활성 voice·가상 재생 표), `audio.buses`(버스 peak/RMS), `audio.streams`(water mark·캐시), `audio.capture <초> <경로>`(마스터 출력을 WAV로). UE `au.Debug.*`·`stat Sounds`에 해당한다.
- 3D 시각화(감쇠 반경·리버브 존·가상 재생)와 `AudioInspector` UI는 화면 마일스톤 뒤(선행 PR17).

### 3.17 Sparks

`SoundAsset` = `SoundSource` 참조, `World.Audio.PlayAt(sound, position)`, `StartLoop`이 돌려주는 `LoopHandle?`은 쥔 자원(스코프를 벗어나거나 소유 객체가 사라지면 fade-stop). 지금 스키마(`Engine/Source/Developer/SparksTestHost/Schema/Engine.sparkbind.json:313–345`)에는 `Play2D`가 없고 `StartLoop`은 nullable `LoopHandle?`을 돌려준다. 스키마 개정으로 `World.Audio.Play2D(sound)`를 더하고, `StartLoop`의 null은 "재생 자리를 얻지 못함"(`CapacityExceeded`·`ShuttingDown`)으로 정한다. 서버와 `--no-sound`에서는 null이 아니라 `Ignored` 상태의 유효 핸들을 돌려준다(3.8.3절과 같다 — 스크립트가 서버에서도 같은 경로를 탄다). 바인딩은 AU3 API 확정 뒤 [Sparks 엔진 통합](SparksEngineIntegration.ko.md) EI5에서 한다(AU3b).

### 3.18 성능 목표

계획 §18 목표를 그대로 쓴다.
- 정상 부하 1시간에 출력 underrun 0.
- AudioRT 블록 처리 시간 p99.9 ≤ 블록 기간 50%(256/48k에서 약 2.67 ms, 512/48k에서 약 5.33 ms). 최댓값과 초과 횟수도 기록한다.
- 측정마다 기기·OS·실제 Q·rate·CPU 주파수·동시 디코드 수를 함께 기록한다.
- 첫 부하 sweep: mono voice 16·32·64·128 × 스트림 0·2·8. 보장 voice 수는 목표 기기 측정 뒤 품질 프로필에 정한다.

### 3.19 시험

모든 믹서 시험은 `OfflineAudioRender`로 결정적으로 단언한다. sleep·실장치 타이밍에 기대지 않는다. 규칙마다 양성·음성 시험을 둔다.

| 영역 | 입력·고장 주입 | 합격 |
|---|---|---|
| PCM·채널 | 좌/우 impulse, mono center, +Y emitter와 리스너 180° 회전 | 지정 frame·채널·부호·레벨, 정수 fixture는 정확 일치, DSP는 허용오차 명시 |
| 예약 | 예약 offset 0·Q−1·Q·Q+1, 같은 시각 명령, 늦은 명령, 블록 크기 64·256·512 | 첫 비무음 frame 위치, 같은 시각 stop·play 순서, 블록 크기가 달라도 같은 출력 |
| 리샘플 | 44.1→48 kHz sweep, pitch 0.5·2.0, loop 경계 impulse | aliasing 허용치, loop 이음매 불연속 없음 |
| click·clipping | 무음→최대 gain, steal·stop, 고레벨 다중 voice | ramp 길이, 불연속 검출, NaN/Inf 0, limiter 동작 |
| 큐·수명 | 명령 링 용량 1·2, stop storm, slot 재사용, 예약 Play 직전 World 파괴, 늦은 디코드 완료 | 수락/거부/병합 수 일치, 재생당 terminal 1회, 이전 generation 명령 무효 |
| 파라미터 seqlock | 쓰기 도중 읽기(시험 hook으로 순번 홀수 상태 주입) | 찢어진 값 없이 이전 값 유지 |
| 결정성 | mix frame을 명시한 같은 입력을 Inline·Thread 모드로 | 같은 PCM |
| 출력 링 | 협상 콜백 크기 256·480·512·1024 × Q 64·256·512(가짜 backend가 콜백 주기를 결정적으로 구동) | 칸 수 공식대로, 정상 주기에서 underrun 0 |
| 장치 | Null 전환, 장치 없음, 열기 실패, 분리 직후 종료, 링이 차서 대기 중인 AudioRT 종료 | 콜백 종료 뒤 접근 0, join 완료, 자원 반환 |
| 임포트 | 형식마다 정상·손상·경계(빈 data chunk, 홀수 padding, 잘린 파일, 9채널, rate 범위 밖) | 명시 오류, 손실 원본 frame 수가 delay·padding 제거 뒤 원래 길이와 일치 |
| 쿠킹 | 같은 입력 두 번, 설정 변경 | 같은 byte, 새 키 |
| 스트리밍 | 부분 읽기, IO 오류, hash 불일치, 마지막 짧은 청크, 100회 seek 뒤 역순 완료 | 손상 PCM 미게시, 늦은 완료 폐기, 메모리 상한 유지 |
| 동시 발음·가상화 | 발소리 폭주 중 대사, 100개 환경 루프 경계 왕복 | 보호 그룹 유지, 실제 voice 상한, 복귀 위상 오차, 왕복 churn 없음 |
| 리버브 존 | 경계 왕복, 겹친 영역, 안에서 바깥 소리, 영역 이탈 뒤 꼬리, bypass 프리셋으로 전환 | 전환 시간 안 수렴·불연속 없음, 동률 안정, 안팎 gain·LPF 수렴, 꼬리 에너지가 임계까지 연속 감소(전환 끝에 잘리지 않음) |
| 채널 순서 | 5.1·7.1 장치 마스크, Vorbis·Opus family 1 다채널 원본 | 각 채널 impulse가 내부 순서·장치 순서의 맞는 자리에 |
| 종료 | 재생 중 종료, 장치 정지 상태 종료, drain 시간 초과 주입 | 해제 뒤 접근 없음, 재시도 게이트 동작 |

실장치 시험(AU2·AU9)은 실행한 플랫폼만 판정하고, 실행하지 않은 플랫폼은 "보류"로 적는다.

## 4. 결정 요약

- 모듈: `Audio`(믹서·DSP·backend 인터페이스) / `AudioMiniaudio`(장치, ClientOnly) / `AudioOpus` / `EngineAudio`(Object 세계) / `AudioTools`(임포트·쿠킹). 의존은 단방향.
- 스레드: Game → 제어 pipe(`SerialTaskChain`, UE 오디오 pipe) → AudioRT 전용 스레드(`Highest`) → 출력 링(최소 2칸, 협상한 콜백 크기로 계산) → 장치 콜백(복사와 무잠금 깨우기만).
- AudioRT는 무할당·무잠금·무대기(깨우기 신호 제외). 결정적 시험은 Inline 모드 오프라인 렌더.
- 장치 IO는 miniaudio 저수준 API, 그 위 책임은 모두 Rocks.
- 원본은 무손실·손실 모두 받아 PCM으로 저장(사용자 결정), 24비트·float 정밀도 유지. 런타임 코덱은 PCM·ADPCM·Opus. 에디터는 재생 때 실행 플랫폼 데이터를 만든다.
- 재생 핸들과 voice 핸들 분리, 동시 발음 그룹과 voice 상한 분리, 가상화 4모드(UE 그대로).
- 리버브 존은 UE처럼 리버브 하나 파라미터 보간, 진행률은 mix clock.
- 서버·`--no-sound`는 장치 없이 `Ignored`, 게임 진행은 완료 통지에 기대지 않는다.

## 5. 결정 사항

| ID | 결정 | 상태 |
|---|---|---|
| AD1 | 단계 코드 AU, 결정 코드 AD(에셋 A0–A6과 겹치지 않게) | 권장안 |
| AD2 | 전용 AudioRT 스레드가 출력 링에 렌더하고 장치 콜백은 복사만 한다(UE 구조). 계획 §18의 콜백 직결 문구는 개정한다(8절) | 권장안 |
| AD3 | AudioRT 우선순위는 `ThreadPriority::Highest`(UE `TPri_Highest`와 같다). 실시간 등급은 AU9 측정 뒤 필요할 때 | 권장안 |
| AD4 | 장치 IO는 `AudioOutputBackend` 뒤 miniaudio 저수준 장치 API. 고수준 engine·resource manager·decoder 미사용. 직접 구현은 측정 근거가 생기면 같은 인터페이스로 | 권장안 |
| AD5 | 오디오 원본은 무손실·손실 모두 받는다(WAV·AIFF·FLAC·OGG Vorbis·Opus·MP3) | **사용자 결정**(2026-11-22) |
| AD6 | 런타임 코덱은 PCM·ADPCM·Opus. Bink·RAD·Vorbis 런타임 디코더는 두지 않는다 | 권장안 |
| AD7 | 내부 float32 planar, 기본 48 kHz·Q=256, 장치 협상값 우선. 출력 링 칸 수 = max(2, ceil(callbackFrames/Q)+1, 설정값) | 권장안 |
| AD8 | 서버·`--no-sound`·commandlet·벤치마크는 장치 없이 `Ignored`. `--mute-audio`는 엔진은 돌리고 마스터 gain만 0. 게임 진행은 완료 통지에 기대지 않는다 | 권장안 |
| AD9 | Tryout은 World마다 새 장치가 기본(UE 기본 true), 설정으로 공유 가능 | 권장안 |
| AD10 | DSP·BeatClock·진단은 `Audio` 안에서 시작하고 두 번째 사용자가 생길 때 분리 | 권장안 |
| AD11 | 리버브 존 전환은 리버브 하나 파라미터 선형 보간(bypass 쪽 0), wet은 입력 램프로 꼬리 보존, 진행률은 mix clock frame. 효과 체인 교체만 교차 페이드 | 권장안(UE 확인) |
| AD12 | 임포트는 디코드한 PCM을 원본 bulk로 저장하고 원본 파일 byte는 보관하지 않는다(UE와 같다). 단 UE의 16비트 교차 WAV와 달리 24비트·float 정밀도를 유지한 planar PCM으로 저장한다. 원본 코덱은 `sourceCodec`에 기록, 경고 없음 | 권장안(UE 확인) |
| AD13 | 임포트 디코더: libFLAC, libvorbis+libogg, libopus+libopusfile, minimp3. libsndfile(LGPL) 미사용. Developer 전용 링크 | 권장안 |
| AD14 | 제어 상태는 `SerialTaskChain` 제어 pipe에 둔다(UE `GAudioPipe`). Game은 `OnFrameEnd`에 일괄 제출 | 권장안 |
| AD15 | AudioRT는 `DedicatedLane`이 아닌 전용 `platform::Thread` 고정 루프 | 권장안 |
| AD16 | 모든 시작·정지 명령에 블록 안 frame offset을 준다(한 블록 구간 상한 `MaxBlockSegments`) | 권장안 |
| AD17 | 재생 수락(`Play` 성공)은 추적 자리 확보의 뜻. 결과는 시작·terminal 두 번 이하, terminal은 정확히 한 번 | 권장안 |
| AD18 | 오디오 재생 준비는 `ReadyMask`에 비트를 더하지 않고 별도 준비 티켓(`PreparePlayback`)으로 | 권장안 |
| AD19 | 동시 발음 해결 규칙 7개·가상화 모드 4개·로딩 동작 5개(`Inherited` 포함)는 UE와 같은 집합. UE에서 실험 기능인 SeekRestart는 정식 기능으로 | 권장안 |
| AD20 | 기본 버스 graph: Master ← Music·Sfx·Dialogue·Ui·ReverbReturn | 권장안 |
| AD21 | 플랫폼 기본 코덱: Opus, `ShortClipSeconds` 미만은 ADPCM | 권장안 |
| AD22 | Opus는 48 kHz로 쿠킹(다른 rate 원본은 쿠킹 때 리샘플) | 권장안 |
| AD23 | 오디오 종료 drain 실패 시 소유자 해제로 넘어가지 않는 Engine 종료 재시도 게이트를 요구한다(계약은 EngineRuntime 개정) | 권장안 |
| AD24 | `Audio`·`EngineAudio`는 서버에도 링크하고 장치 backend(`AudioMiniaudio`)와 디코더(`AudioOpus`)는 ClientOnly. 연결은 `HostServices`·`AudioDecoderTable` 등록으로, 호스트는 ClientOnly 모듈을 직접 참조하지 않는다. 서버 쿠킹은 오디오 payload 제외 | 권장안 |
| AD25 | 에디터·Tryout은 쿠킹하지 않은 clip을 재생할 때 실행 플랫폼 데이터를 만든다(UE DDC 방식). AU4는 PCM 변환만, AU5부터 AssetNugget 경유 | 권장안 |
| AD26 | 장치 콜백 → AudioRT 깨우기는 잠금 없는 단일 대기자 신호(`AudioWakeSignal`: Windows `WaitOnAddress`, Android futex, macOS Mach semaphore) | 권장안 |
| AD27 | 리버브 bypass·없음으로 전환해도 꼬리 에너지가 임계 아래로 머문 뒤에 끄고 이력을 지운다(UE는 즉시 지움) | 권장안 |
| AD28 | Opus 청크는 앞 청크 끝 80 ms 패킷을 복제해 독립 디코드, looping clip은 loop 머리 PCM을 미리 디코드해 둔다 | 권장안 |

## 6. 단계

### 6.1 선행작업

| ID | 선행작업 | 상태(2026-11-22) | 할 일 | 막는 단계 |
|---|---|---|---|---|
| PR1 | 이 스펙 | 개정 1 | 교차 검증 반영 | AU1 이상 |
| PR2 | 스레드 우선순위 | 구현(`Highest`) | 없음. 실시간 등급은 AU9 측정 뒤 PlatformLayer 개정 | — |
| PR3 | Core 동시 컨테이너 SY7(`BoundedSpscQueue`·`FixedBlockPool`·`IndexedFreeList`) | 설계(선행 SY0·SY1·SY2) | SY0→SY1→SY2→SY7 구현 | AU1 |
| PR21 | 무잠금 깨우기 신호 `AudioWakeSignal`(AD26) | 없음(기존 이벤트는 Mutex 사용, SY0 `WaiterSignal` macOS는 조건 변수) | Synchronization 개정: SY0 OS 대기 위에 단일 대기자 자동 리셋 신호, macOS는 Mach semaphore | AU2 |
| PR4 | `SerialTaskChain`·`WorkerScheduler`·`TaskLane::Audio` | 구현 | 없음 | — |
| PR5 | ThirdParty 반입 | 절차 구현, 오디오 라이브러리 없음 | miniaudio(AU2), libFLAC·libvorbis·libogg·libopus·libopusfile·minimp3(AU4), 런타임 libopus(AU5) | AU2, AU4, AU5 |
| PR6 | 모듈 유형 `ClientOnly`, Server 대상 오디오 제외 | 설계 | BuildTargets 구현 | AU2(서버 빌드 확인은 AU3) |
| PR7 | `RunTraits::audioOutput`·`audioMuted` | 이름만 예약 | AppInfo 구현 또는 AU3 임시 판정 | AU3 |
| PR8 | Engine 종료 재시도 게이트 | 없음 | EngineRuntime·CrashHandlingAndLaunch 개정과 구현 | AU3 |
| PR9 | 앱 수명 이벤트(활성·비활성·백그라운드·오디오 포커스) | 없음 | PlatformLayer 개정과 구현 | AU3(비활성 볼륨), AU9 |
| PR10 | 에셋 A2f 임포트 프레임워크 | A2f1a 구현, A2f1b·A2f2 설계(7.1절 18) | 계획 순서대로 | AU4 |
| PR11 | 에셋 A3 AssetNugget·A4 AssetCooker | 설계 | 계획 순서대로 | AU5, AU6 |
| PR12 | 오디오 재생 준비 티켓·sidecar 범위 읽기 | 없음(`ReadyMask`는 Cpu·Render) | AssetSystem 개정 | AU6 |
| PR13 | CVar(구현)·기기 품질 프로필(Config CF4) | 일부 | CF4 | AU9 |
| PR14 | Trace | 구현(TR1–TR5) | 오디오 채널만 추가 | — |
| PR15 | Tryout | 설계(계획 §22.6) | AU3은 정리 계약만, 실제 시험은 Tryout 구현 뒤 | AU9 |
| PR16 | 물리 레이 질의 | 설계(PhysicsJoltAdapter) | 그 전까지 차폐는 fixture | AU7(차폐 완성) |
| PR17 | 화면 마일스톤(debug draw·UI 툴킷) | 설계 | 계획 순서대로 | AU8(시각화), AU10 |
| PR18 | Sparks EI5 | 대기 | AU3 뒤 | AU3b |
| PR19 | Linux x64(BuildTargets BS6) | 설계 | 서버·CI 확인용 | AU9 |
| PR20 | 지역화 음성 pack | 일부(`Localization` 모듈) | 대사 id → 언어별 clip·자막 cue | AU10 |

### 6.2 구현 단계

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| AU1 | `Audio` 모듈: 형식·채널 레이아웃·PCM 버퍼, voice pool, 명령 링·파라미터 슬롯·stop mailbox·epoch·terminal 슬롯, mix clock·블록 안 예약, 리샘플러, source LPF/HPF, gain ramp, 2D·equal-power 패너, 거리 곡선, 마스터 limiter, `OfflineAudioRender`, 메모리 PCM clip | PR1, PR3 | 3.19절 PCM·예약·리샘플·click·큐·결정성 시험, AudioRT 할당 0 |
| AU2 | `AudioOutputBackend`, AudioRT 스레드·출력 링(칸 수 계산)·`AudioWakeSignal`, `NullAudioOutput`, ThirdParty `Miniaudio`, `AudioMiniaudio`(Windows 먼저)와 `HostServices` 등록 | AU1, PR5, PR6, PR21 | Sandbox 시험음 재생, 협상값·실제 스레드 우선순위·underrun 기록, Q ≠ 콜백 크기에서 underrun 0, 장치 없음·열기 실패·종료 자원 반환. macOS·Android는 실행한 것만 판정 |
| AU3 | `EngineAudio`: `AudioEngineSubsystem`·`AudioWorldSubsystem`·제어 pipe·`AudioDeviceContext`, `SoundSource`·`SoundClip`(메모리 PCM)·`AttenuationSettings`(거리), `AudioComponent`, 재생 API·결과, 리스너, CVar(비활성 창 볼륨 포함), 무음 실행·서버, 종료 drain, Tryout 정리 계약 | AU2, PR7, PR8, PR9 | Sandbox에서 키 입력으로 UI음·움직이는 부착 루프, 좌우 방향, 맵 교체·컴포넌트 파괴·GC 뒤 stale voice·lease 0, 서버 빌드에 `AudioMiniaudio` 미링크 |
| AU3b | Sparks 바인딩 | AU3, PR18 | Sparks 시험: 재생·정지·`LoopHandle` 수명, 서버 `Ignored` |
| AU4 | `AudioTools`: `SoundClipImporter`, WAV·AIFF 파서, FLAC·Vorbis·Opus·MP3 디코드(SIMD 고정), ThirdParty 디코더, 에디터 재생용 PCM 플랫폼 데이터 변환(AD25) | AU3, PR10, PR5 | 3.19절 임포트 시험, 디코드 SHA 고정값, 재생 중 재임포트 시 새 재생부터 새 revision |
| AU5 | `AudioEncoder`·`AudioDecoder`·`AudioDecoderTable`, ADPCM·Opus 인코더·디코더, `AudioOpus`, 플랫폼 데이터·청크 표·플랫폼 재정의, 쿠킹 처리기·AssetNugget 키 | AU4, PR11, PR5 | 같은 입력 같은 byte, 설정 변경 새 키, 디코드 허용오차, 손상 payload 거부 |
| AU6 | 로딩 동작(`Inherited` 포함), `AudioChunkCache`, sidecar 게시, 디코드 worker·PCM 링, seek·loop·loop 머리 PCM, 준비 티켓, 굶음 정책 | AU5, PR12 | 3분 곡 첫 음 전 전체 read 없음, 100회 seek 메모리 상한, 늦은 완료 폐기, Opus loop 이음매 불연속 없음 |
| AU7 | `ConcurrencyGroup`·해결 규칙, voice 상한·steal, 가상화, `SoundCategory`·`MixSnapshot`, `MixBus` graph·send·plan 교체, EQ·compressor·plate 리버브·delay, 리버브 존(3.15절), 감쇠 나머지(모양·공기 흡수·거리별 send), 차폐 목표값 입력 | AU3(AU6과 병행 가능), 차폐 완성은 PR16 | 3.19절 동시 발음·가상화·리버브 존 시험, 그래프 교체 시 꼬리 보존 |
| AU8 | trace 채널·카운터, 재생 추적, 콘솔 명령, WAV 캡처. 3D 시각화·`AudioInspector`는 PR17 뒤 같은 단계의 후반(AU8b) | AU3(AU8b는 PR17) | "왜 안 들리나"를 `audio.playback`으로 재현, trace-report 집계 |
| AU9 | 장치 교체·rate 변경·앱 수명·모바일 세션, 다채널 출력·downmix, 품질 프로필, 1시간 부하, 실시간 등급 판단, 병렬 렌더 판단 | AU2, PR9, PR13, PR15, PR19 | 3.18절 목표, 콜백 종료 뒤 접근 0, 실행 플랫폼별 결과 기록 |
| AU10 | `BeatClock`, 모듈레이션, `SoundEvent`, `SoundGraph`(M7), `SignalBus`, 공간화·soundfield 확장 인터페이스(HRTF·ambisonics), convolution 리버브, 지역화 대사, 에디터 미리듣기. 마이크 입력은 capture backend 설계를 이 문서 개정으로 먼저 정한다 | AU6, AU7, AU8, PR17, PR20 | 기능별 CPU·메모리 예산 측정, 그래프 교체·꼬리 보존, 오프라인 재현 |

단계마다 리뷰어 1명이 교차 검증하고, 스펙·문서 관점은 묶음 끝에 본다.

## 7. UE와 다르게 정한 점

| 항목 | UE 5.8 | Rocks | 이유 |
|---|---|---|---|
| 모듈 의존 | Engine과 AudioMixer가 서로 의존 | `Audio` ← `EngineAudio` 단방향, `Audio`는 Object를 모름 | 믹서를 Object 없이 시험·재사용 |
| 런타임 코덱 | BINKA·RADA·OGG·ADPCM·PCM·Opus | PCM·ADPCM·Opus | 독점 코덱 제외, Vorbis는 Opus로 대체 |
| 기본 코덱 | `ProjectDefined` → 모든 플랫폼 BINKA(`PlatformSpecific`이면 ini 형식) | Opus(짧은 clip ADPCM) | 위와 같음 |
| 임포트 디코더 | libsndfile | 형식별 참조 구현 + minimp3, SIMD 고정 | LGPL 회피, 기기 간 결정성 |
| 원본 bulk | 16비트 교차 WAV byte(헤더 포함) | 24비트·float 정밀도 유지 planar PCM | 고해상도 원본 보존 |
| 장치 backend | 플랫폼별 직접 구현 | miniaudio 저수준 장치 IO(같은 인터페이스로 직접 구현 추가 가능) | 초기 구현·검증 비용 |
| 기본 블록 | 1024 frame, 블록 = 콜백 크기 | 256 frame, 콜백 크기와 다를 수 있고 링 칸 수를 계산 | 계획 §18 지연 목표, backend가 콜백 크기를 정하는 경우 |
| 렌더 깨우기 | `FEvent` | 잠금 없는 `AudioWakeSignal` | 장치 콜백과 AudioRT의 우선순위 역전 방지 |
| 리버브 bypass 전환 끝 | early·late 이력 즉시 지움(꼬리 잘림) | 꼬리 에너지가 임계 아래로 머문 뒤 지움 | 꼬리 보존 |
| 블록 안 offset | Quartz 예약만 | 모든 시작·정지 | 결정적 시각과 시험 |
| 리버브 보간 진행률 | 벽시계(`FApp::GetCurrentTime`), 게임 틱 단위 | mix clock frame, 블록 단위 | 오프라인 렌더 결정성 |
| Android | OpenSL ES만 | AAudio 우선(miniaudio), 없으면 OpenSL ES | AAudio가 현재 권장 API |
| 실행 판정 | `FApp::CanEverRenderAudio` 정적 지역 변수 | `RunTraits` 값 | AppInfo 설계(명령줄 고정 시점 명시) |
| 오류 | `check`·로그 | `Result`, 원래 OS 오류 코드 보존 | Rocks 원칙 |
| 다채널 분리 임포트 | `_L`·`_R` 파일 묶음 임포트 | 받지 않음 | 수요 생기면 |

## 8. 다른 문서 개정

이번 개정에서는 다른 문서를 고치지 않는다. 각 단계 착수 전에 아래를 고친다(스펙 먼저 원칙).

| 문서 | 고칠 것 | 시점 |
|---|---|---|
| [엔진 구현 계획](EngineImplementationPlan.ko.md) §18 | 콜백 직결 → 렌더 스레드 + 출력 링(AD2), 이 문서 링크 | AU1 전 |
| [AppInfo·명령줄](AppInfoCommandLine.ko.md) 128행·§8.6 | `RunTraits::audioOutput`·`audioMuted`, `--allow-commandlet-audio`, `audio.unfocused.volume` | AU3 전 |
| [빌드 타깃](BuildTargets.ko.md) 3.7절·69행 | 오디오 모듈 유형(AD24), 서버 쿠킹 payload 제외 | AU2 전 |
| [에셋 시스템](AssetSystem.ko.md) | 오디오 진단(`IgnoredSourceChunks`), 오디오 쿠킹 처리기, sidecar 청크 게시, 재생 준비 티켓(AD18) | AU4·AU5·AU6 전 |
| [엔진 런타임](EngineRuntime.ko.md)·[크래시·실행](CrashHandlingAndLaunch.ko.md) | 종료 재시도 게이트(AD23), subsystem 등록 의존 | AU3 전 |
| [플랫폼 계층](PlatformLayer.ko.md) | 앱 수명 이벤트(PR9) | AU9 전 |
| [Sparks 엔진 통합](SparksEngineIntegration.ko.md) EI5, `Engine.sparkbind.json` | 바인딩 이름 확정, `World.Audio.Play2D` 추가, `StartLoop` null 의미 | AU3b 전 |
| [Trace 시스템](TraceSystem.ko.md) | `Audio`·`AudioMixer` 채널 | AU8 전 |
| [동기화](Synchronization.ko.md) | `AudioWakeSignal`(AD26, PR21). D27(AudioRT SPSC 입력)은 그대로 | AU2 전 |

## 9. 범위 밖과 채택하지 않은 UE 요소

### 9.1 채택하지 않은 UE 요소

- Bink Audio·RAD Audio 코덱, Vorbis 런타임 디코더(AD6).
- libsndfile(AD13).
- 레거시 `FIOSAudioDevice`, XAudio2 backend.
- `EAudioDeviceScope`의 장치 참조 계수 핸들(`FAudioDeviceHandle`) 공개 — Rocks는 `AudioEngineSubsystem`이 소유하고 World는 id만 쓴다.
- 다채널 `_L`·`_R` 분리 파일 임포트.
- `USynthComponent`(procedural은 `SoundGenerator` source 입력으로 — AU10).

### 9.2 범위 밖

- 콘솔 플랫폼 backend·코덱(UE 클론에도 없다).
- AudioLink(외부 오디오 미들웨어 연결), VoIP·음성 채팅(네트워크 단계 뒤).
- UE 실험 기능: Subsonic, `IAudioMixerGeneratorSource`, AudioChannelAgnosticCore, Harmonix.
- Interchange 오디오 번역기.
- 형상 기반 자동 음향(레이 트레이싱 울림 계산). 필요하면 AU10 공간화 확장 위 후속 항목.
- 오디오 분석(AudioSynesthesia) 기능.

## 10. 개정 이력

- 개정 1(2026-11-22): 초판. UE 5.8.4 대조(AudioMixerCore·AudioMixer·Engine 오디오·AudioGameplayVolume·SoundFactory·리버브 보간), 결정 AD1–AD28, 선행작업 PR1–PR21, 단계 AU1–AU10. 사용자 결정: 손실 원본 허용(AD5). 교차 검증 1회 반영:
  - 필수 9: 내부·장치 채널 순서(3.3절), UE 기본 코덱은 모든 플랫폼 BINKA(3.11.2절), 출력 링 칸 수 공식과 UE 최소 2칸(3.2절), 잠금 없는 깨우기 신호 PR21·AD26, PCM 링 용량 공식과 예산 재계산(3.12.3절), 에디터 재생 경로 AD25, `AudioTools` 의존, 상태 줄의 사용자 결정 수, UE 원본 bulk는 16비트 WAV(AD12·7절).
  - 권장 13: `ERequiredSubmixes`, 로딩 동작 `Inherited`·SeekRestart 실험 표시, A2f 사용 규칙(roles·옵션 이름·loop 필드·`loopOverride`·`normalize` 표현·PCM 상한·8비트·A2f1a 상태), 디코더 SIMD 고정·너겟 키, Inline·Thread 시험 조건, 파라미터 seqlock, AudioRT 종료 깨우기·Null 타이머 join, 선행 관계(PR9→AU3, PR17→AU8b, PR20→AU10, 캐시 기본 상수), AD24 인용·`AudioOpus` ClientOnly·`HostServices` 등록, Opus 48 kHz 결과(rate 표 적용 범위·loop 반올림·채널 매핑·청크 pre-roll 복제·loop 머리 PCM — AD28), 준비 결과 집합(AAA10), bypass 꼬리(AD27), `SpatialIndex` 의존·Sparks 스키마.
