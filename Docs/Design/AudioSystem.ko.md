# 오디오 시스템 설계

기준일: 2026-11-23, 개정 26(3.15.2절 AudioGameplayVolume 컴포넌트 대응·submix send·효과 교체 영역 속성, 9.1절 Synthesis 신스 — [컴포넌트 도입](ComponentCatalog/09-Plugins.ko.md) 9절 CR54). 개정 25(3.15.2절 리버브 존 영역 형상을 공용 `VolumeShape`로 — [컴포넌트 도입](ComponentCatalog/04-Prerequisites.ko.md) CR17). 개정 24(3.31절 리뷰 반영 — 끝 무음 덧붙이기로 끝 frame 보장, 블록 처리, 할당 전 PCM 상한, 위치 변환 Result, 겹침 거부, 시험 보강). 개정 23: 3.31절 AU5 앞당김 C2 — 쿠킹 리샘플러, ThirdParty SampleRate 반입. 개정 22: 3.30절 설계 교차 검증 반영 — 청크 가운데 시작, HostServices 경유 등록, 단위 framing, 메타·payload 분리, 채널 마스크·bitstream 판, 정렬, 재생 정책 칸 제외. 개정 21: 3.30절 AU5 앞당김 C1 — 플랫폼 데이터 형식·청크 표·인코더/디코더 인터페이스, 디코더 표 등록 방식. 개정 20: 3.29절 리뷰 반영 — Vorbis·Opus 다채널을 내부 순서로, OpusHead 사전 확인, op_pcm_total 길이 대조, libm 결정성·hardening 한계, 시험 보강. 개정 19: 3.29절 Opus 원본 디코드, ThirdParty Opus·OpusFile 반입. 개정 18: 3.27절 리뷰 반영 — LOST_SYNC 뒤 audio 거부, frame 연속성, 미리 잡기 상한, 시험 보강; 3.28절 Vorbis 원본 디코드, ThirdParty Ogg·Vorbis 반입. 개정 17: 3.27절 FLAC 원본 디코드, ThirdParty Flac 반입. 개정 16: 3.26절 리뷰 반영 — 디코드 단계 형식 재확인, frame당 표본 수로 무음 채움, minimp3_ex식 gapless 길이, 한계. 개정 15: 3.26절 MP3 원본 디코드, ThirdParty Minimp3 반입. 개정 14: 3.25절 리뷰 반영 — RIFX GUID 끝 방향, cue dwPosition, 잘린 파일 허용, float 비유한 거부, 실패 시 비움, AIFF 1–32비트. 개정 13: 3.25절 WAV·AIFF 원본 디코드. 개정 12: 3.24절 ADPCM 블록 코덱 앞당김. 개정 11: Mixer2 구현 리뷰 반영 — mailbox·stop-all 최소 fade, 시험 강화. 개정 10: 3.23절 AU1 Mixer2 스레드 경계 — 소유 분리, seqlock·mailbox·stop-all·시작/terminal·ACK, 반복 Stop 규칙). 개정 9: Mixer1 구현 리뷰 반영 — fade 끝·동점 규칙, 종류별 무시 계수, 마스터 gain 상한, 비유한 정리를 limiter 앞으로. 개정 8: 3.22절 AU1 믹서 코어 — 명령 인자 전달로 PR3을 AU2로, 상태 표, Q 불변 ramp, FTZ 실행 함수. DSP2 리뷰 반영 — 비유한 key, ratio·gain 범위, 큰 블록, FTZ·DAZ 계약. 개정 6: 3.21절 AU1 DSP2 dynamics, DSP2 범위 바로잡음. 개정 5: DSP1 리뷰 반영 — HPF fs/2 clamp, HPF bypass 20 Hz, 리샘플러 비율 범위 UE와 같게. 개정 4: DSP1 구현 반영(리샘플러 고정소수점, 방위각 경계, 오류 코드). 개정 3: 3.20절 AU1 구현 세부 DSP1, 3.13.2절 패닝을 UE 방식으로 고침. 개정 2: 8절 다른 문서 개정 반영. 소리를 재생하는 엔진 기능 전체를 정한다. 범위는 실시간 믹서, 장치 출력, 엔진·World 연결, 사운드 에셋, 임포트, 코덱·쿠킹, 스트리밍, 감쇠·공간화·동시 발음·가상화, 버스·효과·리버브 존, 진단이다. UE 5.8의 AudioMixerCore·AudioMixer·SignalProcessing·Engine 오디오 클래스와 AudioGameplayVolume 플러그인에 해당한다.

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
| 명령줄 | AppInfo 개정 4가 `RunTraits::audioOutput`·`audioMuted`와 `--no-sound`·`--mute-audio`를 설계했다(미구현 — AP1) | 판정 값이 아직 코드에 없다(3.8.4절) |
| 설정 | 콘솔 영역 `"audio"`(`Core/Source/Runtime/Config/Public/Config/ConsoleSettings.h:66`), `audio.master.volume` 시험 사용 | 오디오 CVar가 없다 |
| 빌드 대상 | [빌드 타깃](BuildTargets/README.ko.md) 59–63행에 모듈 유형 `ClientOnly`. 69행은 Server의 기능 축소 범위를 네트워크 단계 스펙에 넘긴다 | 빌드 코드에 `ClientOnly`가 없다. 오디오 모듈의 Server 범위는 이 문서가 정한다(AD24) |
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
| 장치 콜백 | backend 스레드(OS·miniaudio 소유) | 출력 링 읽기 위치 | `RealtimeWakeSignal` 깨우기, underrun 계수 |
| 디코드·IO worker | `WorkerScheduler` | 디코더 인스턴스 상태, 요청 버퍼 | 세대·요청 번호가 붙은 PCM 페이지 |

AudioRT 규칙(계획 §18 그대로): Object 접근, 파일 IO, 동적 할당·해제, mutex·조건 변수, 일반 task 대기, 문자열 로그를 하지 않는다. 기다리는 것은 렌더 깨우기 신호 하나뿐이다(출력 링이 찼을 때). trace는 사전 할당 숫자 링에만 쓴다. 실제 적용된 스레드 우선순위를 장치 열기 로그에 남긴다(`SetThreadPriority` 실패는 지금 무시된다 — `Private/Win64/Concurrency.cpp:237`).

렌더 깨우기 신호(`RealtimeWakeSignal` — [동기화](Synchronization.ko.md) 9.6절, AD26): 장치 콜백이 Signal하고 AudioRT가 Wait하는 단일 대기자 자동 리셋 신호다. Signal은 잠금을 잡지 않는다: Windows `WakeByAddressSingle`(또는 auto-reset event), Android futex wake, macOS Mach `semaphore_signal`. 기존 `ManualResetEvent`·`Semaphore`(내부 Mutex)와 SY0 `WaiterSignal` macOS 구현(pthread 조건 변수)은 쓰지 않는다. 이 신호는 [동기화](Synchronization.ko.md) 개정으로 PlatformBase에 둔다(선행 PR21).

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
- **채널 레이아웃**: `ChannelLayout { Mono, Stereo, Quad, Surround51, Surround71 }`, 최대 `MaxOutputChannels` = 8. 내부 채널 순서는 FL, FR, FC, LFE, SL, SR, BL, BR(UE `AudioChannelUtils` 기본 순서 — `Source/Runtime/SignalProcessing/Private/AudioChannelUtils.cpp:16–26`)이고 5.1은 앞 여섯이다. UE `EAudioMixerChannel` 열거 순서(FL, FR, FC, LFE, BL, BR, FLC, FRC, BC, SL, SR… — `AudioMixerCore/Public/AudioMixerChannel.h:12–34`)와 다르다. 장치 순서는 backend가 알려주는 채널 마스크로 매핑한다(WAVEFORMATEXTENSIBLE·miniaudio 7.1 장치는 FL, FR, FC, LFE, BL, BR, SL, SR). 디코더가 내는 순서(Vorbis 순서, Opus 채널 매핑 family 1 = FL, FC, FR, …, LFE)도 디코드 직후 내부 순서로 바꾼다(채널 수별 표는 3.29.2절). 첫 장치 출력은 스테레오다. 다채널 출력·downmix 행렬은 AU9.
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

- **terminal 슬롯**: voice 슬롯마다 종료 사유(자연 종료·정지·steal·디코드 굶음·장치 정지·World 제거)와 마지막 사용 frame(`endMixFrame`·`releaseMixFrame` — 3.23.5절)을 적는다. 일반 이벤트 링이 차도 terminal 기록은 제어 쪽이 수거할 때까지 남는다.
- **이벤트 링**: 사전 할당 숫자 이벤트(재생 시작 frame, 늦은 frame 수, underrun, clipping, NaN 대체) + 버린 수.
- **epoch ACK**: AudioRT가 블록을 끝낼 때마다 `completedMixFrame`을 게시한다. plan·PCM·DSP 상태는 ACK가 마지막 사용 블록의 끝(`releaseMixFrame`)에 닿기 전에는 해제·재사용하지 않는다(계획 §18, 3.23.6절).

#### 3.5.4 Game으로 가는 결과

재생마다 결과는 두 번 이하로 온다: 시작(시작 frame 또는 시작 실패 사유)과 terminal(종료 사유). 둘 다 Game lane에서 받는다. 자연 종료·명시적 정지·steal·디코드 오류·World 종료가 겹쳐도 terminal은 하나만 낸다. 부착 소유자(`AudioComponent`)가 먼저 사라졌으면 `WeakObjectRef` 확인 뒤 버린다. 결과 콜백은 Game lane의 `OnFrameBegin`에서 부른다.

#### 3.5.5 수명 순서

1. 제어 쪽이 voice를 해제하기로 하면 stop mailbox를 쓴다.
2. AudioRT가 fade를 마치고 terminal 슬롯에 적는다.
3. 제어 쪽이 terminal을 수거하고, 그 voice가 쓰던 PCM 페이지·디코드 요청을 취소하고, 디코드 취소 완료와 AudioRT ACK(`completedMixFrame ≥ releaseMixFrame`)를 확인한다.
4. 그 뒤에만 voice 슬롯·PCM pin·`AssetLease`를 Game에 돌려준다.

### 3.6 믹서 블록 처리

UE `FMixerDevice::OnProcessAudioStream`(`Source/Runtime/AudioMixer/Private/AudioMixerDevice.cpp:1666`)의 순서는 명령 큐 소비 → Quartz clock 갱신 → `SourceManager->ComputeNextBlockOfSamples()` → `MainSubmix->ProcessAudio` → endpoint submix → 계량이다. Rocks `AudioMixer::RenderBlock(commands, interleavedOut)`도 같은 순서다(명령은 인자로 받는다 — 3.22.1절).

1. 블록 시작: stop mailbox·epoch 관찰, 파라미터 최신값 읽기, 블록에 걸린 명령 적용(호출자가 정렬해 넘긴 것 — 3.22.1절), plan 교체 확인.
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
| `NullAudioOutput` | Audio | 모두 | AU2. 장치가 없거나 열기 실패면 이것으로 시간만 진행한다(UE `FMixerNullCallback`과 같다). 자체 타이머 스레드가 블록 기간마다 장치 콜백 역할(링에서 한 블록 소비 + `RealtimeWakeSignal` 깨우기)을 한다 |
| `OfflineAudioRender` | Audio | 모두 | AU1. 시험·오프라인 캡처 |
| `MiniaudioOutput` | AudioMiniaudio | Windows(WASAPI) → macOS(Core Audio) → Android(AAudio, 없으면 OpenSL ES) | AU2. miniaudio 저수준 장치 API만 쓴다. 실제 선택된 backend 이름·협상값을 로그에 남긴다 |

miniaudio의 고수준 engine·resource manager·decoder·node graph는 쓰지 않는다. Rocks 에셋·스트리밍·믹서와 책임이 겹친다. 장치 IO만으로 부족하다는 측정(장치 교체 복구·지연·특정 기기 결함)이 나오면 같은 인터페이스로 플랫폼 직접 구현(WASAPI 등)을 더한다(AD4). UE는 플랫폼마다 직접 구현한다(Windows 기본 `AudioMixerWasapi`, Mac `AudioMixerCoreAudio`, iOS `AudioMixerAudioUnit`, Android `AudioMixerAndroid` — OpenSL ES만, Linux `AudioMixerSDL`).

#### 3.7.3 장치 상태와 교체

상태: `Closed → Opening → Running ⇄ Suspended → Running`, 실패나 분리는 `Reconfiguring` 또는 `Unavailable`(Null로 진행).

- 열기: 협상 형식 확인 → 출력 링·변환 버퍼 할당 → 초기 무음 → AudioRT 시작 → backend Start.
- 기본 장치 변경·분리·rate 변경 통지는 제어 pipe로 넘긴다. 순서: backend `Stop`(콜백 종료 확인) → AudioRT를 Null 타이머로 전환(시간 진행 유지) → 새 장치 열기·협상 → rate가 바뀌었으면 clock epoch를 올리고 예약을 초 단위로 다시 맞춤 → 링 재구성 → backend Start → 짧은 fade-in. UE `FAudioMixerPlatformSwappable`과 같은 흐름이다.
- 재시도는 제어 pipe 타이머(`audio.device.retryms`)로만 한다. 명시적으로 고른 장치는 OS 기본 장치 변경을 따라가지 않는다(사용자 설정).
- 데스크톱 창 비활성은 PlatformWindow 포커스(`HasFocus`·`FocusLost`)로, 모바일 백그라운드·통화·오디오 포커스는 [플랫폼 계층](PlatformLayer.ko.md) 앱 수명 이벤트(선행 PR9)로 받는다. 비활성 동안 `audio.unfocused.volume` 배수를 적용하고, 모바일 백그라운드는 Suspended로 간다.

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

대기에는 상한(`ShutdownDrainTimeoutMs`)을 두고, 넘으면 오류를 돌려준다. 이때 지금의 `KeepFirst` 누적 후 정리 계속 흐름은 콜백이 살아 있을 수 있는 소유자를 풀어버린다. 그래서 Engine 종료에 재시도 게이트가 필요하다(선행 PR8): 오디오 `CompleteInFlightWork` 실패면 `AssetManager` 종료·subsystem 해제로 넘어가지 않고 재시도하거나, 재시도할 수 없으면 소유자를 해제하지 않은 채 `platform::Fatal`로 끝낸다(해제 뒤 접근보다 명시 실패가 낫다). 이 게이트의 정확한 계약은 [엔진 런타임](EngineRuntime/README.ko.md)·[크래시·실행](CrashHandlingAndLaunch.ko.md) 개정으로 정한다.

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
| `mp3` | MPEG-1/2/2.5 Layer III | minimp3 | sndfile |

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

개정 22: 아래 인터페이스와 3.11.3절 머리 목록은 3.30절이 대체한다(청크 가운데 시작, 단위씩 디코드, 채널 마스크, 재생 정책 칸 제외). 아래는 처음 설계로 남긴다.

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
| ADPCM | 짧은 SFX, 약 4:1, 디코드 아주 쌈 | MS ADPCM 512 byte 블록(UE와 같음), 채널 블록 교차 배치 — 3.24절 |
| Opus | 음악·대사·긴 환경음 | rate는 48 kHz만 쓴다(Opus 내부 rate). 다른 원본은 쿠킹 때 48 kHz로 리샘플한다. 3채널 이상은 Opus multistream(채널 매핑 family 1, 디코드 뒤 내부 순서로 바꿈 — 3.3절) |

플랫폼 기본(`PlatformDefault`가 고르는 것, 프로젝트 설정으로 바꿀 수 있다): Windows·macOS·Android = Opus, 길이 `ShortClipSeconds`(= 1.0) 미만은 ADPCM. UE 기본(모든 플랫폼 BINKA)과 다르다(7절). iOS는 보류 플랫폼이다.

Opus 48 kHz 리샘플의 결과:
- loop·cue 위치는 `round(frame × 48000 / srcRate)`(가까운 정수, 동률은 짝수)로 바꾼다. loop 길이가 0이 되면 1 frame으로 둔다. 쿠킹 시험이 경계값을 확인한다.
- 3.11.3절의 rate 상한 표는 PCM·ADPCM에만 쓴다. Opus는 항상 48 kHz다.
- 쿠킹 리샘플러는 런타임 리샘플러와 다른 고품질 필터(band-limited)이며, 그 버전을 AssetNugget 키에 넣는다.

#### 3.11.3 플랫폼 데이터

쿠킹 산출물(`SoundClip` 플랫폼 데이터):
- 헤더: 형식 버전, 코덱 id·버전, 채널 레이아웃, 인코딩 rate, 유효 PCM frame 수, loop 구간(인코딩 rate 기준), encoder delay·끝 padding, 청크 수, 로딩 동작, 첫 청크 상주 여부. (개정 22: 3.30.1절이 대체 — 채널 레이아웃은 채널 마스크, 로딩 동작·첫 청크 상주는 `SoundClip` 쿠킹 필드)
- 청크 표: 청크마다 payload 위치·크기, 첫 frame·frame 수, 독립 디코드 시작 여부, 필요한 pre-roll frame, SHA-256(서명 프로필에서 manifest가 덮는다).
- 청크 크기: 목표 `TargetChunkBytes`(= 256 KiB, 플랫폼 프로필로 바꿈). 청크 경계는 코덱 프레임 경계다.
- 청크 독립성: 모든 청크는 그 청크만으로 디코드를 시작할 수 있다. Opus는 앞 청크 끝의 패킷을 `OpusPreRollMs`(= 80 ms, RFC 7845 권장값)만큼 이 청크 앞에 **복제**해 두고, 디코더는 그 구간을 디코드해 버린다(`preRollFrames`). 앞 청크를 읽지 않아도 seek·재시작이 되며, 대가는 청크마다 약 80 ms 분량의 byte다. ADPCM·PCM은 블록 독립이라 pre-roll이 없다.
- loop 이음매: loop 끝에서 처음으로 돌아갈 때 디코더가 pre-roll을 다시 디코드하면 그 블록에 출력이 늦는다. 그래서 looping clip은 재생 시작 때 loop 머리 `LoopHeadFrames`(= pre-roll + 2 × Q 이상) PCM을 디코드해 voice 링 옆에 두고, 디코드 worker는 loop 끝에 닿기 전에 loop 시작 청크로 미리 seek한다.
- 플랫폼 재정의(UE `FPlatformAudioCookOverrides`): rate 상한 표(최대·높음·중간·낮음·최소 — PCM·ADPCM만), 품질 배수, 자동 스트리밍 길이 임계, 첫 청크 인라인.

#### 3.11.4 AssetNugget 키

키 = AssetSystem의 공통 너겟 키(툴 빌드 식별 등 — AssetSystem 508행 부근) + 원본 PCM SHA-256 + 쿠킹에 닿는 설정(코덱·품질·rate·loop·`loopOverride`·채널 처리. 로딩 동작·첫 청크 상주는 재생 정책이라 넣지 않는다 — 3.30.1절) + 플랫폼 재정의 + 인코더 이름·버전·고정 SIMD 수준 + 쿠킹 리샘플러 버전 + 플랫폼 데이터 형식 버전. UE `FDerivedAudioDataCompressor`(`Source/Runtime/Engine/Private/AudioDerivedData.cpp`)가 DDC 키로 같은 일을 한다. 같은 입력은 같은 byte를 낸다(인코더 결정성 — 멀티스레드 인코더 옵션 끔).

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

- voice마다 PCM 링(원본 rate frame 단위). 용량 = `2 × max(디코더 최대 출력 단위, ceil(Q × MaxPitchScale × srcRate / mixRate))`. Opus 최대 출력 단위는 패킷 최대 길이 120 ms = 5760 frame, ADPCM은 블록 frame 수, PCM16은 단위 1024 frame이다(디코더 `MaxFramesPerCall`, 3.30.2절). `MaxPitchScale` = 2.0(UE `GlobalMaxPitchScale` 기본 — `Source/Runtime/Engine/Private/AudioSettings.cpp:30`). 디코드 worker가 다음 구간을 채운다. 시작 water mark·재충전 water mark는 품질 프로필 값이다.
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

UE 기본 패너 `FMixerDevice::Get3DChannelMap`(`AudioMixerDevice.cpp:2983`)은 방위각으로 인접한 두 스피커 사이를 equal-power로 나눈다. Rocks도 같다(세부는 3.20.6절, 개정 3에서 고침 — 개정 2까지의 "right 축 투영" 식은 UE와 결과가 달랐다).
- 방위각은 리스너 앞이 0°, 오른쪽이 90°인 0–360° 시계 방향이다(UE `FAudioDevice::GetAzimuth` — `AudioDevice.cpp:6479–6511`).
- 스테레오 출력은 FL = 270°, FR = 90°로 두고(UE `InitializeChannelAzimuthMap` — `AudioMixerChannelMaps.cpp:147–`), 두 스피커 사이의 각도 비율 f로 `next = sin(f·π/2)`, `prev = cos(f·π/2)`다. 그래서 정면·정후면은 양쪽 1/√2, 정오른쪽은 R만 1이다.
- +Y 위치는 오른쪽이 커야 하고, 리스너를 180° 돌리면 반대여야 한다(시험).
- 영거리·내부 반경 안에서는 비공간화 비율을 늘려 `Lerp(pan, 1/채널 수, 비율)`로 고르게 나눈다(UE OmniDirectional 모드).
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
| 리스너·emitter의 영역 판정, 겹침 우선순위(동률은 안정 id), 경계 hysteresis | Game(`AudioWorldSubsystem`, `OnFrameEnd`) | 영역 형상과 위치 → 영역 id. 형상은 후처리 볼륨 등과 함께 쓰는 공용 `VolumeShape`(상자·구·캡슐·볼록체 — UE brush volume 대체, [컴포넌트 도입](ComponentCatalog/04-Prerequisites.ko.md) CR17·CD1)다. SpatialIndex 후보 검색 + 형상 내부 판정. 물리 엔진은 쓰지 않는다 |
| 프리셋 선택, 전환 시작(시작값·끝값·끝 frame), 재생별 wet send·안팎 gain·LPF 목표값 | 제어 pipe | 영역 id·거리 → AudioRT 명령 |
| 파라미터 보간, 리버브 DSP, 잔향 꼬리 유지 | AudioRT | 목표값만 받는다 |

- `ReverbPreset`: UE `UReverbEffect`와 같은 파라미터 이름·범위. 기본 프리셋(동굴·대성당·작은 방·복도·야외)을 엔진 콘텐츠로 준다.
- `ReverbZoneComponent`: 형상, 프리셋, wet 양(0–1), 전환 시간, 우선순위, `ZoneInteriorSettings`(UE `FInteriorSettings`와 같은 여덟 값).
- World 기본 리버브: World 설정에 프리셋(없음 가능)과 안팎 기본값.
- UE AudioGameplayVolume 플러그인 컴포넌트 대응([컴포넌트 도입](ComponentCatalog/09-Plugins.ko.md) 9.5절 대체, CR54): `UReverbVolumeComponent` → 프리셋·wet·전환, `UAttenuationVolumeComponent`·`UFilterVolumeComponent` → `ZoneInteriorSettings`의 안팎 gain·LPF, `USubmixSendVolumeComponent` → 영역 안 재생에 더하는 `MixBus` send(AU7 send), `USubmixOverrideVolumeComponent` → 영역 안에서 `MixBus` 효과 체인 교체(3.14.4절 plan 교체). 영역 하나(`ReverbZoneComponent`)가 이 속성들을 함께 갖고, UE의 volume 컴포넌트 + mutator 분리는 들이지 않는다. send·효과 교체 속성은 AU7에서 구현한다.
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
| 큐·수명 | 명령 링 용량 1·2(AU2), stop storm, slot 재사용, 예약 Play 직전 World 파괴, 늦은 디코드 완료 | 수락/거부/병합 수 일치, 재생당 terminal 1회, 이전 generation 명령 무효 |
| 파라미터 seqlock | 쓰기 도중 읽기(시험 hook으로 순번 홀수 상태 주입) | 찢어진 값 없이 이전 값 유지 |
| 결정성 | mix frame을 명시한 같은 입력을 Inline·Thread 모드로(AU2), Q 64–1024(AU1) | 같은 PCM |
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

### 3.20 AU1 구현 세부 — DSP1(개정 3)

AU1을 셋으로 나눈다. DSP1은 스레드·큐 없이 버퍼를 받는 순수 DSP와 형식 타입이고, DSP2(dynamics 처리기 — 3.21절)와 Inline 믹서가 뒤따른다. biquad EQ·plate 리버브는 효과 표(3.14절)대로 AU7이다(개정 6에서 바로잡음 — 개정 3–5는 DSP2에 넣었지만 AU1 마스터 경로가 쓰는 효과는 limiter뿐이다). 모두 `Audio` 모듈(`Engine/Source/Runtime/Audio`, `EngineModuleRules`, 유니티 빌드, 의존 Core)이고 이름공간은 `rocks::audio`다. 시험은 모듈 `Tests/`에 두고 EngineTests(`FoundationTests.Build.cs`의 `EngineTestModules`)에 등록한다. 모든 처리 함수는 `noexcept`이고 할당하지 않는다(할당은 생성 `TryCreate`에서만).

#### 3.20.1 형식(`Audio/AudioFormat.h`)

- `ChannelLayout { Mono, Stereo, Surround51, Surround71 }`, `ChannelCount(layout)`. Quad는 UE 4채널 장치 순서를 AU9에서 확인한 뒤 더한다.
- 내부 채널 `AudioChannel { FrontLeft, FrontRight, FrontCenter, LowFrequency, SideLeft, SideRight, BackLeft, BackRight }`(3.3절 순서). 레이아웃의 i번째 채널은 이 순서의 앞 `ChannelCount`개다(Mono는 FrontCenter 하나).
- 상수: `DefaultMixSampleRate` = 48000, `MinMixSampleRate` = 8000, `MaxMixSampleRate` = 192000, `MaxOutputChannels` = 8, `MaxBlockFrames` = 4096(콜백 크기 상한 겸 처리 단위 상한).

#### 3.20.2 planar 버퍼(`Audio/AudioBuffer.h`)

- `PlanarBuffer`: 채널 수 × frame 용량 float32를 한 번에 할당한다. 채널마다 시작 주소를 `AudioBufferAlignment` = 64 byte에 맞춘다(SIMD·캐시 줄). `TryCreate(channels, frameCapacity, Allocator&) -> Result<PlanarBuffer>`(채널 0·`MaxOutputChannels` 초과·frame 0·`MaxBlockFrames` 초과는 `InvalidArgument`, 할당 실패는 `OutOfMemory`).
- 보기: `PlanarView`(쓰기)·`PlanarConstView`(읽기) = 채널 포인터 배열 + 채널 수 + frame 수. 소유하지 않는다.
- 혼합: `MixAdd(dst, src, gain)`, `MixAddRamp(dst, src, startGain, endGain)` — n번째 frame 이득은 `start + (end − start)·n/N`이다(UE `Audio::ArrayMixIn`의 시작·끝 이득 규칙. 마지막 frame은 끝값에 닿지 않고 다음 블록이 끝값에서 시작한다).

#### 3.20.3 gain ramp(`Audio/GainRamp.h`)

`GainRamp`: 현재값·목표값·남은 frame 수. `SetTarget(target, rampFrames)`(0이면 즉시, target은 유한해야 하고 아니면 assert — 출력 전체를 오염시키는 호출자 계약 위반), `Apply(PlanarView, frames)`. frame마다 현재값을 곱한 뒤 한 단계 진행하고, 남은 수가 0이 되는 frame에서 현재값을 목표값으로 정확히 맞춘다(부동소수 누적 오차가 목표에 남지 않게). 블록을 어떻게 나눠 불러도 같은 출력이다(시험).

#### 3.20.4 1극 필터(`Audio/OnePoleFilter.h`)

UE `FInterpolatedLPF`·`FInterpolatedHPF`(`Source/Runtime/SignalProcessing/Private/InterpolatedOnePole.cpp`)와 같은 식이다. source의 차폐·공기 흡수·사용자 LPF/HPF가 쓴다.
- LPF: `b1 = exp(−π·clamp(2·fc/fs, 0, 1))`, `y = x + b1·(z − x)`, `z = y`.
- HPF: fc를 UE `NyquistLimit`처럼 [0, fs/2 − 1 Hz]로 자른 뒤(`HighPassNyquistMargin` = 1 Hz) `G = tan(π·fc/fs)`, `a0 = G/(1 + G)`, `v = (x − z)·a0`, `lp = v + z`, `z = v + lp`, `y = x − lp`. UE는 `FastTan` 근사를 쓰고 Rocks는 `PlatformMath::Tan`을 쓴다(7절).
- 계수 보간: `SetCutoff(fc, interpFrames)`는 계수를 `interpFrames` frame에 걸쳐 선형으로 옮긴다. 첫 설정은 보간 없이 바로 적용한다(UE `bIsFirstFrequencyChange`).
- bypass: LPF 차단 주파수가 `FilterBypassFrequency` − `FilterFrequencyTolerance`(20000 Hz − 1e-4) 이상이면 처리하지 않는다. HPF는 `FilterMinFrequency` + `FilterFrequencyTolerance`(20 Hz + 1e-4) 이하면 bypass다(UE `MAX_FILTER_FREQUENCY`·`MIN_FILTER_FREQUENCY`·`KINDA_SMALL_NUMBER` — `Source/Runtime/AudioMixerCore/Public/AudioDefines.h:47–48`, 판정 `AudioMixerSourceManager.cpp:3589–3590`). bypass에서 다시 켤 때 상태 `z`는 0에서 시작한다.
- fs/2 clamp가 필요한 이유: a0 = 1이면 극이 −1에 놓여 상태 `z`가 Nyquist 성분을 따라 끝없이 커지고 `(x − z) + z`의 자릿수 소실이 출력 잡음이 된다(8 kHz 믹서에서 HPF 5 kHz 같은 정상 입력으로 닿는다). 1 Hz를 빼면 상태는 정상값 |a0/(1 − a0)|에 머문다(시험).
- ramp 중 재설정은 마지막으로 쓴 계수에서 새 ramp를 시작한다.

#### 3.20.5 선형 리샘플러(`Audio/LinearResampler.h`)

UE 런타임 리샘플러 `FRuntimeResampler`(`Source/Runtime/SignalProcessing/Public/DSP/RuntimeResampler.h:13–16`)는 선형 보간이다. Rocks 런타임도 선형이고, 고품질 리샘플은 쿠킹(3.11.2절)이 맡는다.
- 비율 r = 출력 1 frame당 입력 frame 진행량(= pitch × srcRate / mixRate). 범위 `[MinFrameRatio, MaxFrameRatio]` = [0.01, 100]이고(UE `FRuntimeResampler`와 같음) 밖은 `InvalidArgument`.
- 상태: 입력 위치 `position`(현재 입력 블록의 첫 frame 기준)과 비율은 32.32 고정소수점(int64)이고, 직전 입력 frame(채널마다)을 둔다. double 누적은 블록 기준점이 바뀌면 반올림이 달라져 블록 분할 불변을 지키지 못한다(개정 4에서 고침). `Process`는 다음 출력 위치보다 앞선 입력을 모두 소비한다(소비 수 = ceil(다음 위치)). 그래서 남는 위치는 (−1, 0]이다. 비율 보간 길이 상한은 `MaxRatioInterpFrames` = 2^20 frame이다. 출력 frame n은 `x[i] + (x[i+1] − x[i])·frac`(i = floor(position), frac = position − i)이고 `x[-1]`은 직전 `Process`가 마지막으로 소비한 frame이다(소비가 0인 호출은 바꾸지 않는다).
- `SetFrameRatio(r, interpFrames)`: 비율을 `interpFrames` 출력 frame에 걸쳐 선형으로 옮긴다(UE `SetFrameRatio(InRatio, InNumFramesToInterpolate)`).
- `InputFramesNeeded(outputFrames)`: 그만큼 내려면 필요한 입력 frame 수(보간 진행 중이면 그 경로를 따라 계산). `Process(input, output) -> 소비한 입력 frame 수`. `Process`는 입력 검사를 위해 `InputFramesNeeded`와 같은 경로를 한 번 더 계산한다(블록당 출력 frame 수만큼의 정수 연산, 성능 문제가 보이면 AU1 믹서에서 합친다). 입력이 모자라면 `InvalidArgument`이고 상태를 바꾸지 않는다.
- 블록 분할 불변: 같은 입력·비율이면 출력 블록을 어떻게 나눠도 같은 PCM이다(시험). r = 1이고 위치가 정수면 입력을 그대로 낸다.
- `OutputFramesBefore(inputFrame) -> uint32`(개정 8): 비율 보간 경로를 따라, 출력 위치가 `inputFrame`(소비하지 않은 입력의 첫 frame 기준) 이상이 되기 전까지의 출력 frame 수를 `MaxBlockFrames`까지 센다. 믹서의 끝 판정(3.22.5절)이 쓴다.

#### 3.20.6 패너(`Audio/Panner.h`)

- `ComputeAzimuthDegrees(listenerForward, listenerRight, listenerToSound)`: UE `GetAzimuth`와 같다. 앞·오른쪽 축 내적으로 2D 방향을 만들고 사분면을 따져 0–360°를 만든다(atan2와 같음). UE는 x = 0·y < 0(정왼쪽)에서 90°를 내는 경계 결함이 있어 Rocks는 270°로 바로잡는다. 수평 성분이 없으면(수직) 0°다. 입력은 단위 벡터(cm 좌표계의 방향, 3.13.1절).
- 스피커 방위각 표: 스테레오 FL 270°·FR 90°, 5.1·7.1은 FL 330°·FR 30°·SL 250°·SR 110°·BL 210°·BR 150°. 5.1의 서라운드 쌍은 SL·SR(±110°, ITU-R BS.775)로 고정한다. UE는 장치 채널 마스크를 따르며 Windows 5.1 기본 마스크는 BL·BR(210°·150°)이다(`WindowsMMDeviceInfoCache.cpp:206–215`). 장치가 back 마스크를 보고할 때 SL·SR을 BL·BR 스피커로 내보내는 일은 AU9에서 장치 마스크를 확인하며 정한다. FC·LFE는 방위각 계산에서 뺀다(UE `bAllowCenterChannel3DPanning` 기본 false — `Source/Runtime/Engine/Private/AudioDevice.cpp:374`).
- `ComputeChannelGains(layout, azimuth, nonSpatializedAmount, lfeBleed, Span<float> out)`: 방위각을 감싸는 두 스피커를 찾고 각도 비율 f로 `next = sin(f·π/2)`, `prev = cos(f·π/2)`(0–1로 자름). 채널마다 `Lerp(pan, 1/채널 수, nonSpatializedAmount)`, LFE는 `lfeBleed`, 쌍이 아닌 채널의 pan은 0이다(UE `Get3DChannelMap` OmniDirectional — `AudioMixerDevice.cpp:2983–3125`).
- UE `Get3DChannelMap`의 FC 하한(`VoiceCenterChannelVolume`과 `Max`)은 AU3에서 source 파라미터로 더한다(지금은 0).
- 센터 전용(`centerChannelOnly`): 스테레오는 양쪽 1/√2, 그 밖은 FC = 1.
- 스테레오 원본의 좌우 방위각 spread는 AU3에서 이 함수를 채널마다 두 번 부르는 것으로 한다.

#### 3.20.7 거리 감쇠(`Audio/DistanceAttenuation.h`)

UE `FBaseAttenuationSettings::AttenuationEval`(`Source/Runtime/Engine/Private/Attenuation.cpp:170–258`)과 같은 식이다. 구 모양은 `distance = max(|d| − innerRadius, 0)`(같은 파일 145–149행).
- `falloff = max(falloffDistance, 1)`, `d = distance × distanceScale`, `ε` = `AttenuationEpsilon` = 1e-4(UE `KINDA_SMALL_NUMBER`).
- Linear `1 − d/falloff`, Logarithmic `−0.5·ln(max(d, ε)/falloff)`, Inverse `0.02/(max(d, ε)/falloff)`, LogReverse `d > falloff`면 0, 아니면 `1 + 0.5·ln(max(1 − d/falloff, ε))`, NaturalSound `α = d/falloff`에서 `10^(α·dBAtMax/20)`(Hold는 α를 0–1로 자름, Silent는 α ≥ 1이면 0, Continues는 자르지 않음, `dBAtMax` ≤ 0 — 기본 −60).
- 결과는 0–1로 자른다. `Custom`(커브)은 AU7에서 Curves 모듈 커브로 더한다(지금 `EvaluateDistanceAttenuation`은 `Custom`이면 `Unsupported`).

#### 3.20.8 시험(DSP1)

| 대상 | 확인 |
|---|---|
| 버퍼 | 정렬, 생성 인자 음성, 할당 실패 N회(`NFailAllocator`), `MixAddRamp` 이득 식 |
| gain ramp | 목표 정확 도달, 블록 분할 불변, 0 frame ramp, 램프 중 목표 변경 |
| 1극 필터 | 계수 식(double 기준값 대조, HPF fs/2 clamp), DC·Nyquist 응답 방향, LPF·HPF 계수 보간 경로(정답 대조), ramp 중 재설정, bypass 경계(LPF 19999.99·20000 Hz, HPF 20·20.01 Hz), 다시 켜기, HPF fs/2 이상 1분 Nyquist 유계, 블록 분할 불변 |
| 리샘플러 | r = 1 항등, r = 0.5·2·0.75·44.1/48 위치·값, 블록 분할 불변, 소비 0 호출(r = 0.01, 1 frame씩), 블록 사이 비율 재설정 분할 불변, 비율 보간, 입력 부족 음성, 범위 밖 비율 음성(유한·비유한) |
| 패너 | 방위각 사분면(0·45·90·135·180·225·270·315°)과 UE 경계 결함 자리(정왼쪽·정후면·수직), 정면·후면 1/√2, 5.1·7.1 한 바퀴 equal-power, 정오른쪽 R = 1, 리스너 180° 회전 대칭, 5.1 쌍 선택, 비공간화 비율, 센터 전용, equal-power 합 1 |
| 거리 감쇠 | 모델마다 0·내부 반경·중간·falloff·falloff 밖 값(double 기준값 대조), falloff < 1, NaturalSound 세 모드, Custom 음성 |

### 3.21 AU1 구현 세부 — DSP2 dynamics(개정 6)

UE `Audio::FDynamicsProcessor`(`Source/Runtime/SignalProcessing/Private/DynamicsProcessor.cpp`)와 `FInlineEnvelopeFollower`·`FAttackRelease`(`Public/DSP/EnvelopeFollower.h`, `Private/EnvelopeFollower.cpp`)에 해당한다. 마스터 limiter(3.6절 4단계)가 AU1에서 쓰고, AU7 compressor 효과가 같은 처리기를 쓴다. 헤더는 `Audio/DynamicsProcessor.h`, 클래스는 `DynamicsProcessor`다. UE의 블록 경로(`ProcessAudioDeinterleaveInternal`)를 따른다. UE frame 경로(`ProcessAudioFrame`)는 dB 하한(−160 dB)과 0 dB 판정이 달라 따르지 않는다.

#### 3.21.1 설정(`DynamicsSettings`, 기본값은 UE 생성자 값)

| 필드 | 기본 | 범위(밖이면 `InvalidArgument`) |
|---|---|---|
| `mode` | `Compressor` | `Compressor`·`Limiter`·`Expander`·`Gate`·`UpwardsCompressor` |
| `peakMode` | `Peak` | `Peak`·`MeanSquared`·`RootMeanSquared`(UE `TruePeak`은 envelope follower가 받지 않으므로 없음) |
| `linkMode` | `Disabled` | `Disabled`·`Average`·`Peak` |
| `lookaheadMs` | 10 | [0, `MaxLookaheadMs` = 100] |
| `attackMs`·`releaseMs` | 20·1000 | 0 이상 유한(0이면 계수 0 = 즉시) |
| `thresholdDb` | −6 | 유한 |
| `ratio` | 1 | 유한하고 `MinDynamicsRatio` = 1e-8 이상(UE는 `SMALL_NUMBER`로 올림, Rocks는 거부 — 더 작으면 1/ratio가 넘쳐 이득이 NaN) |
| `kneeBandwidthDb` | 10(half-knee 5) | 0 이상 유한 |
| `inputGainDb`·`outputGainDb` | 0·0 | [−96, 96] dB(`MaxDynamicsGainDb`, UE는 제한 없음 — 약 770 dB 넘으면 선형값이 inf) |
| `analogMode` | true | |
| `analysisWindowMs` | 5 | 0보다 큰 유한(MeanSquared·RootMeanSquared 전용) |

UE의 외부 key(sidechain)·key shelf 필터·key audition은 biquad가 필요해 AU7에서 더한다. 지금 key는 입력 자신이고 key gain도 없다.

#### 3.21.2 식

- 엔벌로프 계수: `coef = exp(−1000·T/(ms·fs))`, T = analog 1.00239343(RC 63.2%) / digital 4.60517019(= ln 100, 99%). ms = 0이면 0. 평균 제곱 창: `α = exp(−1000/(fs·windowMs))`, `β = 1 − α`.
- 정규화: Peak `|x|`, MeanSquared `a = β·x² + α·a`, RootMeanSquared `√a`(같은 a).
- 추종: `diff = v − s`; `v ≤ s`면 `v = attack·diff + s`, 아니면 `v = release·diff + s`.
- key = 입력 × input gain. 채널마다 엔벌로프를 낸 뒤 link: Average는 채널 평균, Peak는 최대, Disabled는 채널별.
- dB 변환: `20·log10(max(v, 10^(−96/20)))`(UE `ArrayMagnitudeToDecibelInPlace(…, −96)`).
- 기울기: Compressor·UpwardsCompressor `1 − 1/ratio`, Limiter 1, Expander `1/ratio − 1`, Gate −1.
- knee: `x0 = threshold − half`, `x1 = min(threshold + half, 0)`, y0 = 0, y1 = 기울기. 분모 절댓값이 1e-8보다 작으면 1e-8. 처리 구간은 `half ≥ 0`이고 `x0 < level < threshold + half`이다(UpwardsCompressor는 UE 식 그대로 `level < x0`와 `level > threshold + half`를 함께 요구해 사실상 거짓). 구간 안 기울기는 `(level − x1)/(x0 − x1)·y0 + (level − x0)/(x1 − x0)·y1`이다.
- 이득 dB = 기울기 × (threshold − level). UpwardsCompressor는 [0, 36 dB]로 자른다(`UpwardsCompressionMaxGain`). 그 밖은 dB가 −1e-8보다 크면 선형 1, 아니면 `10^(dB/20)`.
- 출력: 채널 c = lookahead 지연(`ceil(lookaheadMs·fs/1000)` frame)을 거친 입력 × 이득[c % 이득 채널 수] × output gain × input gain.

#### 3.21.3 수명·실시간 계약

- `TryCreate(sampleRate, channels, settings, Allocator&)`: 지연 링(채널마다 `ceil(100·fs/1000)` + 1 frame — 지연 0–최대를 모두 담는다)과 엔벌로프 scratch(채널 × `MaxBlockFrames`)를 한 번에 할당한다. 할당 실패는 `OutOfMemory`다.
- `SetSettings(settings) -> Result<void>`: 검사한 뒤 계수를 다시 계산한다. 할당하지 않는다. lookahead 길이를 바꾸면 읽기 위치만 옮긴다(UE `SetDelayLengthSamples`처럼 링에 남은 이력을 그대로 쓴다). 엔벌로프·평균 제곱 상태는 유지한다.
- `Process(PlanarView)`: 제자리 처리, `noexcept`, 할당 없음. 채널 수는 생성 때 값과 같아야 한다(assert). frame 수에는 제한이 없고 `MaxBlockFrames`씩 나눠 처리한다(scratch 크기). 기본 생성·이동된 빈 처리기는 아무것도 하지 않는다.
- 비유한 입력: key 계산에서 비유한 표본은 0으로 본다. UE는 거르지 않아 inf 한 표본이 엔벌로프를 NaN에 영구히 가둔다(`v ≤ s`가 늘 거짓). 출력 쪽 비유한 값은 그대로 지나가고 마스터 끝의 NaN·Inf 정리(3.6절 4단계)가 0으로 바꾼다.
- denormal: 무음 꼬리에서 엔벌로프·평균 제곱이 비정규 값에 머문다. UE는 렌더 스레드에 `FScopedFTZFloatMode`를 건다(`AudioMixer.cpp:930`). Rocks도 AudioRT·Inline 렌더 진입에서 FTZ를 켠다(3.22.8절 `RunWithRealtimeAudioFloatingPoint`, DAZ는 UE처럼 켜지 않는다). DSP 함수 자체는 FTZ를 가정하지 않고 결과만 그 환경에서 같다.
- `Reset()`: 엔벌로프·평균 제곱·지연 링을 0으로 지운다.
- 블록 분할 불변: 상태가 표본 순서로만 진행하므로 블록을 어떻게 나눠도 출력이 bit 단위로 같다(시험).
- `GainForLevelDb(levelDb)`: 정적 곡선(이득 선형값)이다. 시험·진단용이다.

#### 3.21.4 시험(DSP2)

| 대상 | 확인 |
|---|---|
| 정적 곡선 | 다섯 모드 × knee 0·10 dB × threshold −20·−2 dB(knee 위 점 0 dB clamp) × 여러 level을 double 기준값(3.21.2 식)과 대조, UpwardsCompressor 36 dB 상한, 임계 아래 이득 정확히 1 |
| 엔벌로프 | 계수 식(analog·digital), 계단 응답(digital 어택 시간에 99%, analog 63.2%), 릴리스 하강 경로, MeanSquared·RootMeanSquared 과도 구간(`x²(1 − α^k)`)과 수렴값, 비유한 입력 뒤 회복 |
| lookahead | 임펄스 지연 = `ceil(ms·fs/1000)`(100 ms는 다음 블록), 0 ms는 지연 없음, 실행 중 길이 줄이기·늘리기 |
| limiter | 어택·lookahead 0의 Peak limiter 출력이 임계 이하, 임계 아래 신호는 그대로(이득 1) |
| link | 스테레오 한쪽만 클 때 Disabled·Average·Peak 이득 |
| 블록 분할 불변 | 무작위 입력·무작위 블록 크기, `MaxBlockFrames`보다 큰 블록, Reset(RootMeanSquared 상태 포함) |
| 음성 | 생성 인자, 설정 범위(모든 필드), 할당 실패 N회 |

### 3.22 AU1 구현 세부 — 믹서 코어(개정 8)

AU1의 나머지를 둘로 나눈다. **Mixer1**(이 절)은 한 스레드에서 도는 믹서 코어와 `OfflineAudioRender`다. **Mixer2**는 스레드 경계의 원자값이다(파라미터 seqlock, stop mailbox, 장치 stop-all epoch, epoch ACK, 시작·terminal 슬롯 원자화 — 3.23절. World별 scene epoch는 AU3). 원자값은 `Platform/Atomics.h`로 만들 수 있고 시험 hook으로 Inline에서 결정적으로 시험할 수 있으므로 Mixer2도 AU1이다. SY7이 필요한 명령 링만 AU2로 옮긴다(3.22.1절).

#### 3.22.1 명령을 인자로 받는 렌더(PR3 조정)

- `AudioMixer::RenderBlock(Span<const AudioRtCommand> commands, Span<float> interleavedOut) -> RenderBlockResult`는 그 블록에 줄 명령을 인자로 받는다. 명령을 큐에서 꺼내고 정렬하는 일은 호출자가 한다. 믹서 코어는 큐 구현과 무관하다. UE `FMixerSourceManager`도 명령 큐 소비(`PumpCommandQueue`)와 source 렌더를 나눠 둔다.
- `commands`는 (`mixFrame`, `sequence`) 오름차순이어야 한다(어기면 assert). `ImmediateMixFrame`은 정렬 전에 호출자가 현재 블록 시작 frame으로 바꾼다(그대로 오면 assert). 미래 예약 명령 뒤에 Immediate가 갇히지 않게 하기 위해서다.
- Inline 모드: `OfflineAudioRender`가 자기 대기 목록을 블록마다 정렬해 넘긴다(3.22.9절).
- 스레드 모드(AU2): SPSC 큐는 FIFO이고 꺼낸 미래 명령을 되돌릴 수 없다. 그래서 AudioRT가 고정 용량 정렬 pending 배열(`AudioRtPendingCapacity` = `AudioRtCommandCapacity`)을 소유하고, 큐에서 꺼낸 명령을 정렬 삽입한 뒤 그 앞부분을 넘긴다. 배열이 차면 큐 소비를 멈추고 센다(제어 쪽은 링 포화와 같게 미룬다).
- 시각 지정 명령과 Mixer2 경로의 우선순위: stop mailbox(시각 없음, 즉시 fade)는 블록 시작에 명령보다 먼저 본다. 파라미터 seqlock 값은 블록 시작에 읽어 목표값으로 두고, 같은 블록의 시각 지정 `SetParams`가 그 뒤 순서로 덮어쓴다.
- 결과: PR3(SY7)은 AU1이 아니라 AU2의 선행이다(6.1·6.2절 표 개정).

#### 3.22.2 설정과 생성

`AudioMixerConfig`:

| 필드 | 기본 | 범위(밖이면 `InvalidArgument`) |
|---|---|---|
| `sampleRate` | 48000 | `IsValidMixSampleRate` |
| `layout` | `Stereo` | `Mono`·`Stereo`(AU1). 다채널은 AU9 |
| `blockFrames`(Q) | `DefaultMixBlockFrames` = 256 | 64·128·256·512·1024 |
| `maxVoices` | `DefaultMaxVoices` = 128 | 1–`MaxMixerVoices`(= 1024) |
| `maxClips` | `DefaultMaxClips` = 256 | 1–`MaxMixerClips`(= 65536) |
| `masterLimiter` | 3.22.7절 프리셋 | `DynamicsSettings` 범위 |
| `masterLimiterEnabled` | true | |

- `AudioMixer::TryCreate(config, Allocator&)`가 한 번에 만드는 것: voice 표, voice별 DSP 상태(리샘플러·LPF·HPF·gain ramp·fade·채널 이득 ramp), 공용 scratch(입력 `MaxBlockFrames` × 2채널, 출력 Q × 2채널 — voice별이 아니다. 1024 voice별이면 약 32 MB), 마스터 버퍼, 마스터 limiter, clip 표.
- 실패는 `OutOfMemory`이고, 그때까지 만든 것을 모두 되돌린다.
- 이후 `RenderBlock`은 할당하지 않는다(시험: 미리 Submit한 `Render`와 `RenderBlock`의 할당 시도 수 0).

#### 3.22.3 clip 표(메모리 PCM)

- `AudioClipDesc`는 소유하지 않는 planar float PCM이다.
  - 채널 포인터: 1–2채널(AU1).
  - frame 수: 1 이상.
  - 원본 rate: [`MinClipSampleRate` = 1000, `MaxClipSampleRate` = 384000].
  - loop 구간 `[loopBegin, loopEnd)`: 둘 다 0이면 clip 전체(UE와 같다), 아니면 `loopBegin < loopEnd ≤ frameCount`.
- `RegisterClip(desc) -> Result<AudioClipId>`(제어 쪽):
  - 비유한 표본이 있으면 `InvalidArgument`다(NaN 하나가 필터 상태를 영구 NaN으로 만들고 마스터 합 전체를 지운다).
  - 표가 차면 `CapacityExceeded`다.
  - id는 `{index, generation}`이다.
- clip 사용 수: `PreparePlay`(3.22.4절)가 voice에 clip을 묶을 때 올리고, 그 voice를 `ReleaseVoice`할 때 내린다.
- `UnregisterClip(id)`: 사용 수가 0보다 크면 `Busy`, 이전 generation이면 `StaleHandle`이다. 해제가 성공한 뒤에만 호출자가 PCM 메모리를 돌려준다(3.5.5절 4단계의 PCM pin). 그래서 제출된 Play가 해제된 메모리를 읽는 일은 없다.

#### 3.22.4 voice·명령·상태 표

- `AcquireVoice() -> Result<AudioVoiceHandle>`(제어 쪽): 빈 슬롯이 없으면 `CapacityExceeded`다.
- `PreparePlay(handle, clipId, startFrame, loop, params) -> Result<AudioRtCommand>`(제어 쪽): handle과 clip을 검사하고(이전 generation은 `StaleHandle`), voice에 clip을 묶은 뒤 `Play` 명령을 만든다. 다음은 `InvalidArgument`다.
  - `startFrame ≥ frameCount`
  - loop인데 `startFrame ≥ loopEnd`
  - 이미 Play를 만든 voice
- `ReleaseVoice(handle)`: 아래 상태 표대로다. 놓을 때 generation을 올리고 clip 사용 수를 내린다. 이전 handle 명령은 무시하고 센다.
- `AudioRtCommand`(고정 크기 POD, 문자열·소유 포인터 없음): 필드는 `type`, `voice`, `mixFrame`, `sequence`, payload다.
  - `Play`: 시작 원본 frame, loop, 초기 파라미터. clip은 voice에 묶인 것이다.
  - `Stop`: `fadeFrames`. 제어 쪽 기본은 `DefaultStopFadeFrames` = 512(UE `au.SourceFadeMin`)이고 0은 그 frame에서 바로 끝이다.
  - `SetParams`: 파라미터 전부와 `rampFrames`.
  - `SetMasterGain`: 이득(선형)과 `rampFrames`.
- 파라미터 `AudioVoiceParams`:
  - `gain`: 선형 [0, `MaxVoiceGain` = 4].
  - `pitch`: [`MinPitch` = 0.4, `MaxPitch` = 2]. UE `GlobalMin/MaxPitchScale` 기본값이고 설정 승격은 AU3.
  - 패닝: `{mode: Direct | Azimuth, azimuthDegrees, nonSpatializedAmount}`.
  - `lowPassCutoffHz`, `highPassCutoffHz`.
  - 거리 감쇠·분류 gain은 제어 쪽이 `gain`에 곱해 넣는다.
  - 범위 검사는 제어 쪽 API(`PreparePlay`·Mixer2·AU3)가 한다. `RenderBlock`은 범위로 자르고 자른 수를 센다(실시간 경로는 실패를 돌려줄 곳이 없다).
- 리샘플 비율: pitch × clip rate / mix rate를 `LinearResampler` 범위 [0.01, 100]으로 자른다.
- ramp 길이는 블록·구간 길이와 무관하다. `SetParams`·`SetMasterGain`은 명령의 `rampFrames`를 쓰고, 제어 쪽 기본은 `DefaultParamRampFrames` = 256이다. 필터 계수 보간도 같은 길이다. 출력 채널 이득은 채널마다 `GainRamp`로 두어 구간·블록을 넘어 이어 간다. 그래서 같은 명령이면 Q가 달라도 같은 출력이다. UE는 블록 단위로 보간한다(7절).
- fade는 파라미터 gain과 따로 곱한다(UE `VolumeFadeSlope` — `AudioMixerSourceManager.cpp:2013–2056`). fade 중 `SetParams`는 fade를 되돌리지 못한다.

상태 표(행 = 슬롯 상태, 열 = 사건):

| 상태 | Play | Stop | SetParams | ReleaseVoice | 자연 끝 |
|---|---|---|---|---|---|
| `Reserved`(Play 없음) | — | 제어 API가 거부(3.23.1절) | 제어 API가 거부 | 성공 → Idle | — |
| `Prepared`(Play 만듦, 미적용) | 적용 → Playing, `startMixFrame` 기록 | 즉시 terminal `Stopped`(fade 없음, UE `StopInternal`) → Finished, 이후 Play 무시 | 저장(뒤 sequence가 이김) | `Busy` | — |
| `Playing` | 무시 | fade 시작(0이면 즉시 끝) | 적용 | `Busy` | terminal `NaturalEnd` |
| `Stopping` | 무시 | 남은 fade보다 일찍 끝날 때만 받아들임(fade 0 = 즉시, steal), 아니면 무시 | 적용(fade는 따로 곱함) | `Busy` | terminal `NaturalEnd` |
| `Finished` | 무시 | 무시 | 무시 | 성공 → Idle(terminal 수거와 ACK ≥ `releaseMixFrame` 뒤 — 3.23.6절) | — |

같은 voice의 자연 끝과 fade 끝은 frame 순서로 먼저 온 쪽 하나만 terminal이다(같은 frame이면 `NaturalEnd` — 3.22.5절). 무시한 명령은 종류별로 센다(Prepared에서 sequence가 더 작아 버린 SetParams 포함). `Azimuth`를 stereo clip에 주면 `PreparePlay`가 `InvalidArgument`로 거부한다(렌더 경로의 `SetParams`면 Direct로 바꾸고 센다). Prepared voice는 `ReleaseVoice`가 `Busy`이므로, 제어 쪽이 Play 제출에 실패했으면 Stop을 보내 Finished로 만든 뒤 수거·해제한다. `SetMasterGain` 이득은 [0, `MaxMasterGain` = 4]이다.

#### 3.22.5 끝 판정(구간 경계와 무관)

- loop 없는 voice의 끝은 리샘플러의 다음 출력 위치가 처음으로 원본 `frameCount` 이상이 되는 출력 frame이다. 그 직전 출력까지는 `x[frameCount…] = 0` 꼬리를 포함해 낸다.
- `endMixFrame`은 그 frame 번호다. 구간·블록 끝과 무관하다.
- 리샘플러에 `OutputFramesBefore(inputFrame) -> uint32`를 더한다(3.20.5절 개정). 비율 보간 경로를 따라, 입력 위치가 `inputFrame`(현재 소비 기준 상대값)에 닿기 전까지의 출력 frame 수를 `MaxBlockFrames`까지 센다.
- fade 끝은 fade 길이 N frame을 낸 다음 frame(= Stop frame + N)이다. k번째 fade 이득은 1 − k/N이라 마지막으로 내는 frame의 이득은 1/N이고 이득 0인 frame은 내지 않는다.
- 자연 끝과 fade 끝이 같은 frame이면 `NaturalEnd`가 이긴다.

#### 3.22.6 블록 처리(3.6절 순서의 AU1 부분)

1. 명령 소비: 블록 `[start, start+Q)`보다 앞선 frame의 명령은 offset 0에서 실행하고 늦은 frame을 센다. 블록 끝 이후 명령에 닿으면 멈춘다. 구간 수(offset 0에서 시작하는 첫 구간 포함)가 `MaxBlockSegments`(= 16)를 넘게 되면 그 명령부터 다음 블록으로 미루고 센다. 미룬 명령은 다음 블록에서 늦은 명령으로도 센다(두 계수는 원인이 다르다). `RenderBlockResult::consumedCommands`는 앞에서부터 소비한 수다.
2. 구간마다(명령 offset으로 나눈 `[a, b)`): 그 offset의 명령을 순서대로 적용한 뒤 `Playing`·`Stopping` voice마다 다음을 한다.
   1. PCM: clip 커서에서 리샘플러가 요구하는 입력을 공용 입력 scratch에 복사한다. loop면 `loopEnd`에서 `loopBegin`으로 이어 붙이고, 끝이면 0을 채운다. 요구량이 scratch를 넘으면 출력을 나눠서 처리한다.
   2. 리샘플(3.20.5절) → LPF·HPF(3.20.4절) → 파라미터 gain ramp × fade(3.20.3절).
   3. 채널 이득: `Direct`의 mono는 양쪽 0.5다(UE 기본 `EMonoChannelUpmixMethod::Linear` — `AudioSettings.h:42–51`). 그래서 `Azimuth` + 비공간화 1(1/채널 수 = 0.5)과 이어진다. stereo는 L→L·R→R이고, 출력이 Mono면 0.707씩 더한다(UE `ToMonoMatrix` — `ChannelMap.cpp:14–18`). `Azimuth`(mono clip만)는 3.20.6절 이득이다. 채널 이득 `GainRamp`를 목표값으로 옮기며 공용 출력 scratch → 마스터 버퍼에 더한다.
   4. 끝 판정(3.22.5절)이면 그 frame까지만 더하고 terminal을 적는다. terminal 기록은 `{reason, endMixFrame}`이다.
3. 마스터: 마스터 gain ramp(구간마다 — 블록 중간의 `SetMasterGain`이 앞 구간을 바꾸지 않게) → 비유한 표본을 0으로 바꾸고 센다(limiter 상태 오염 방지) → limiter(켜져 있으면) → [−1, 1]로 자르고 자른 표본을 센다(UE도 출력에서 자른다 — `AudioMixer.cpp:291`. AU2 int16 변환이 넘치지 않게).
4. 출력: 마스터 planar → `interleavedOut`(교차 float32, Q × 채널 수. 크기가 다르면 assert).
5. mix clock += Q.

제어 쪽 조회: `QueryVoice(handle)`로 상태·`startMixFrame`·terminal을 읽고, `CollectFinished(Span<AudioVoiceTerminal>)`로 수거한다. 계수는 `AudioMixerCounters`(늦은 명령·미룬 명령·무시 명령·자른 파라미터·NaN 대체·clipping)다.

#### 3.22.7 마스터 limiter 프리셋

UE 메인 submix에는 기본 dynamics가 없고 출력 clamp만 있다. Rocks는 3.6절대로 마스터 limiter를 두고, 그 뒤에 clamp도 둔다. 기본값은 다음과 같다.
- `Limiter`, threshold −1 dBFS, knee 0, Peak, link Peak(스테레오 상 유지).
- attack 0, release 100 ms, lookahead 0 ms(지연을 더하지 않는다), digital 시상수.
- 임계를 넘는 표본은 그 표본에서 바로 임계로 내려간다. 과도부 순간 왜곡을 받는 대신 지연이 없다. 설정으로 바꾸거나 끌 수 있다.

#### 3.22.8 부동소수 환경(FTZ)

- UE는 렌더 스레드에 `FScopedFTZFloatMode`(`VectorRegister.h:142`)를 건다. 기존 설정에 FTZ만 더한다.
- Rocks는 compiler가 연산을 제어 레지스터 쓰기 앞뒤로 옮기므로 RAII guard 대신 `RunWithDeterministicFloatingPoint`와 같은 실행 함수 방식을 쓴다. PlatformBase에 `RunWithRealtimeAudioFloatingPoint(function, context)`를 더한다([CoreDataFoundations](CoreDataFoundations.ko.md) 2.1절 개정).
- 이 함수는 결정적 기준 환경(nearest-even, trap 끔)에 FTZ(x64 MXCSR FTZ, ARM64 FPCR FZ)를 더해 실행하고 되돌린다. DAZ는 켜지 않는다(UE와 같다). ARM64 FZ는 입력 비정규도 0으로 본다. 또 x86은 반올림 뒤에, ARM은 반올림 전에 tininess를 판정하므로 최소 정규 수 경계에서 결과가 다를 수 있다. 목적은 같고 결정성 비교는 같은 아키텍처끼리 한다.
- `OfflineAudioRender::Render`가 블록 루프를 이 함수 안에서 돈다. `RenderBlock` 진입은 FTZ가 켜져 있음을 assert한다(시험이 환경 밖에서 직접 부르는 실수를 막는다).

#### 3.22.9 `OfflineAudioRender`

- 믹서 하나와 명령 대기 목록(제어 쪽, `Array`, 할당 허용)을 소유한다.
- `Submit(command)`는 `sequence`를 매긴다.
- `Render(Span<float> interleaved, blockCount)`는 블록마다 다음을 한다. 이 블록 루프는 3.22.8절 환경 안에서 돈다.
  - Immediate를 블록 시작 frame으로 바꾼다.
  - (`mixFrame`, `sequence`)로 정렬해 `RenderBlock`에 넘긴다.
  - 소비한 만큼 지운다.
- 제어 쪽 API(`AcquireVoice`·`PreparePlay`·`RegisterClip`·`QueryVoice` 등)는 믹서 것을 그대로 노출한다. Mixer1에서는 모든 호출이 같은 스레드다.

#### 3.22.10 시험(Mixer1)

| 대상 | 확인 |
|---|---|
| PCM·채널 | mono Direct 0.5·stereo 직결·stereo→Mono 0.707, 정수 fixture 정확 일치, Azimuth 90°는 오른쪽만 |
| 예약 | offset 0·Q−1·Q·Q+1, 같은 frame Play·Stop 순서(sequence), 늦은 명령 offset 0과 늦은 frame 수, `MaxBlockSegments` 초과 미룸, Immediate가 미래 명령 뒤에 갇히지 않음 |
| Q 불변 | 시각을 명시한 명령(Immediate·미룸 제외)이면 Q 64·256·512·1024에서 같은 PCM과 같은 `endMixFrame`(ramp·필터 보간 포함) |
| 리샘플·loop | 44.1→48 kHz 위치, pitch 0.5·2(잘림 계수), loop 경계 impulse 위치, 0/0 loop = 전체, 끝 판정 frame(r = 0.5 꼬리 포함) |
| 상태 표 | 표의 모든 칸(Prepared Stop → 즉시 terminal과 뒤 Play 무시, 반복 Stop, fade 중 SetParams가 끝을 막지 못함, Finished 명령 무시) |
| click | Stop fade 길이와 단조 감소, SetParams ramp 길이 = `rampFrames`, 0 fade 즉시 끝 |
| 마스터 | 마스터 gain ramp, limiter 상한(−1 dBFS), 끈 limiter에서 clamp ±1과 clipping 계수 |
| 수명 | voice·clip 용량, Playing 해제 `Busy`, 이전 generation 명령 무시, Prepared voice의 clip 해제 `Busy`, 비유한 PCM 등록 거부, terminal 한 번 |
| 할당 | 생성 뒤 `RenderBlock`·미리 Submit한 `Render`의 할당 시도 0, 생성 실패 N회 되돌림 |
| 환경 | 렌더 안에서 FTZ 켜짐, 끝나면 원래 값 |

### 3.23 AU1 구현 세부 — Mixer2 스레드 경계(개정 10)

Mixer1(3.22절)은 제어 쪽 API와 `RenderBlock`이 같은 voice 슬롯을 읽고 쓴다. Mixer2는 이 둘을 서로 다른 스레드에서 불러도 데이터 경쟁이 없게 나눈다.
- 경계를 넘는 것은 명령(호출자가 넘기는 배열, 3.22.1절), 원자값(이 절), 출판 순서다.
- 스레드 모드(AU2)에서 명령은 SPSC 큐로 넘어간다. 큐의 생산·소비가 release·acquire 쌍이므로, 명령보다 먼저 쓴 제어 쪽 메모리는 RT가 그 명령을 꺼낸 뒤에 보인다.
- UE와 비교: UE는 오디오 스레드가 렌더 스레드로 명령을 넘긴다.
  - 경로는 `CommandBuffers[2]` 이중 버퍼(`AudioMixerSourceManager.h:461`, 교체는 `CommandBufferIndexCriticalSection` 잠금과 `CommandsProcessedEvent`)와 `TMpscQueue MpscCommandQueue`(h:471)다.
  - 공유 원자 표지(`bStopFadedOut`, `bIsDone`·`bPlaying`, `bSourceBusy` — `AudioMixerSourceVoice.h:154`, `AudioMixerSource.h:190–207·750`)도 쓴다.
  - source id는 `ReleaseSourceId`(cpp:1742–1761)에서 바로 free 목록에 넣고, 재사용 안전은 명령의 FIFO 순서에 기댄다.
  - Rocks는 정렬 pending 배열(3.22.1절)이 FIFO를 깨므로 generation과 ACK로 수명을 나누고, 잠금 없이 계획 §18의 seqlock·mailbox·epoch를 쓴다(7절).

#### 3.23.1 소유 분리

| 상태 | 소유 | 다른 쪽이 보는 방법 |
|---|---|---|
| 제어 voice 슬롯: generation, 제어 상태(`Idle`·`Reserved`·`Submitted`·`Finished`), clip 묶음, 수거 여부 | 제어 | 보지 않는다 |
| RT voice 슬롯: 3.22.4절 상태 표(`Prepared`·`Playing`·`Stopping`·`Finished`), RT generation, 마지막 적용 순번, DSP 상태, 커서 | RT | 보지 않는다 |
| clip 표 항목(desc) | 제어가 쓴다 | RT는 현재 generation의 새 수명에서 Play를 적용할 때만 읽는다(그 Play의 큐 출판이 desc 쓰기 뒤다). 제어는 그 clip을 묶은 voice가 모두 해제된 뒤(3.23.6절)에만 지운다 |
| 파라미터 seqlock | 제어 → RT | 3.23.2절 |
| stop mailbox | 제어 → RT | 3.23.3절 |
| 장치 stop-all epoch | 제어 → RT | 3.23.4절 |
| 시작·terminal 슬롯 | RT → 제어 | 3.23.5절 |
| ACK·계수 스냅샷 | RT → 제어 | 3.23.6절 |

- `Play` 명령이 clip 묶음(clip index·시작 frame·loop)과 제출 시점의 stop-all epoch를 직접 싣는다. RT는 제어 슬롯을 읽지 않는다.
- 순번: 모든 명령과 seqlock 출판은 제어 쪽 단조 순번 하나(`sequence`)를 받는다. 제어 API(`PreparePlay`·`MakeStop`·`MakeSetParams`·`MakeSetMasterGain`·`PublishParams`)가 매긴다. `OfflineAudioRender::Submit`은 받은 순번을 그대로 둔다.
- RT 슬롯은 명령의 generation으로 수명을 안다.
  - 더 오래되었으면(부호 있는 32비트 차이 < 0) 무시하고 stale로 센다.
  - 더 새로우면 새 수명이다. RT 슬롯을 `Prepared`로 초기화한 뒤 그 명령을 상태 표대로 적용한다.
  - 한계: 한 슬롯을 RT가 한 번도 보지 않은 채 2^31번 이상 잡았다 놓으면(Reserved → Release 반복) 비교가 뒤집힌다. 제어 API는 Reserved 해제를 연속 2^30번으로 막지 않는다. 실제 수명 주기에서 닿지 않는 수라 한계로만 적는다.
- 제어 쪽 거부: `Reserved`(Play를 만들지 않은) voice나 stale handle에 대한 `MakeStop`·`MakeSetParams`·`PublishParams`·`RequestStop`은 `InvalidArgument`·`StaleHandle`이다. 3.22.4절 표의 `Reserved` 행 "무시"는 제어 쪽 거부로 바뀐다.
- Play 제출 실패(AU2 큐 포화 등): 제어는 `RequestStop`으로 그 수명을 끝낸다. mailbox는 포화가 없다.

#### 3.23.2 파라미터 seqlock(최신값 슬롯)

- voice마다 슬롯 하나다.
  - `version`(`uint32` 원자값, 홀수 = 쓰는 중)
  - 내용(모두 `uint32` 원자값, 실수는 비트 패턴): generation, 제어 순번(64비트를 하위·상위 두 word로), `AudioVoiceParams`
  - 일반 필드로 두면 동시 읽기·쓰기가 C++ 데이터 경쟁이다.
- 제어 쓰기 `PublishParams(handle, params)`(쓰는 쪽은 제어 하나):
  - `v = version`(relaxed)
  - `version = v + 1`(relaxed) → release fence → 내용 store(relaxed) → `version = v + 2`(release)
- RT 읽기:
  - `v1 = version`(acquire). 홀수면 이번에는 건너뛴다.
  - 내용 load(relaxed) → acquire fence → `v2 = version`(relaxed)
  - `v1 ≠ v2`면 건너뛴다. 재시도하지 않고, 마지막 적용 순번도 바꾸지 않는다.
- 적용 조건: 내용의 generation이 RT 슬롯 generation과 같고, 내용의 순번이 그 voice의 마지막 적용 순번보다 크다.
- 적용:
  - `Playing`·`Stopping`이면 블록 시작에서 `DefaultParamRampFrames` ramp로 적용한다.
  - `Prepared`(Play 대기 중)면 저장 파라미터로 둔다(3.22.4절 "뒤 sequence가 이김"과 같은 규칙).
  - 새 수명의 Play를 적용할 때도 바로 한 번 읽는다. 그래서 `PreparePlay` 뒤 Play가 적용되기 전에 낸 출판이 사라지지 않는다.
- 시각 지정 `SetParams` 명령도 같은 마지막 적용 순번 비교를 거친다. 그래서 두 경로가 섞여도 제어 프로그램 순서대로 최신값이 이긴다. 예: seqlock A(순번 1) → 미래 frame F의 `SetParams` B(2) → seqlock C(3)이면, C가 먼저 적용된 뒤 F에서 B는 오래된 값이라 버리고 센다.
- 시험 hook `AudioMixerTestAccess`로 홀수 version·찢어진 쓰기(v1 ≠ v2)를 주입한다.

#### 3.23.3 stop mailbox

- voice마다 `uint64` 원자값 하나: `{generation:32, fadeFrames:32}`다. 0은 비었다는 뜻이다(generation 0은 무효).
- 제어 쓰기 `RequestStop(handle, fadeFrames)`: 제어는 그 수명에서 낸 fade의 최소값을 기억하고, 그보다 짧은 값일 때만 release store한다. mailbox 값은 줄기만 하므로 RT가 언제 읽어도 가장 이른 끝을 보고, 같은 값이 되돌아오는 ABA도 없다(개정 11 — 마지막 값이 남는 방식이면 `RequestStop(0)` 뒤 `RequestStop(512)`가 steal을 지울 수 있었다).
- RT 읽기: 블록 시작, 명령보다 먼저 acquire load한다. 값이 RT가 마지막으로 본 값과 다르면 처리한다.
  - generation이 RT 슬롯과 같으면 offset 0의 Stop으로 적용한다.
  - generation이 RT 슬롯보다 새로우면 새 수명이다. Play가 아직 큐·pending에 있는데 stop이 먼저 보인 경우로, Prepared에서 즉시 terminal(`Stopped`)이고 뒤의 Play는 무시된다.
  - 더 오래되었으면 무시한다.
- 반복 Stop(명령·mailbox 공통, 3.22.4절 개정): `Stopping`에서 새 Stop은 남은 fade보다 일찍 끝날 때만 받아들인다. fade 0이면 즉시 끝이다(steal). 아니면 무시하고 센다.

#### 3.23.4 장치 stop-all epoch

- `uint64` 원자값 하나: `{epoch:32, fadeFrames:32}`다. 제어 `StopAll(fadeFrames)`가 epoch를 올려 release store한다. 제어는 현재 epoch를 알고 있고, `PreparePlay`가 그 값을 Play에 싣는다.
- RT는 처리한 epoch를 출판한다(RT → 제어). 제어는 아직 처리되지 않은 epoch가 있으면 그 fade와 새 fade 중 짧은 쪽을 낸다. 그래서 `StopAll(0)` 뒤의 `StopAll(1000)`이 0을 지우지 않는다(개정 11).
- stop-all이 이미 끝난 voice나 더 긴 fade로 건너뛴 voice는 명령 무시 계수에 넣지 않는다.
- RT는 블록 시작에 acquire load한다. 마지막으로 처리한 epoch와 다르면 그 시점의 모든 `Playing`·`Stopping` voice에 Stop(fadeFrames)을 적용하고, `Prepared` voice를 즉시 끝낸다(사유 `StoppedAll`).
- 새 수명의 Play가 실은 epoch가 RT가 처리한 epoch보다 작으면, 그 Play는 StopAll 전에 제출된 것이다. 시작하지 않고 즉시 terminal(`StoppedAll`)로 끝낸다. 그래서 큐·pending에 있던 Play도 놓치지 않는다(UE `StopAllSounds`도 대기 중인 sound를 멈춘다).
- World별 scene epoch는 voice에 scene 색인이 생기는 AU3(World 연결)에서 같은 방식으로 더한다.

#### 3.23.5 시작·terminal 슬롯

- voice마다 RT가 쓰는 원자값이다.
  - 시작: `startGeneration`(`uint32`)과 `startMixFrame`(`uint64`). `startMixFrame`을 relaxed store한 뒤 `startGeneration`을 release store한다.
  - terminal: word `{generation:32, reason:8}`와 `endMixFrame`, `releaseMixFrame`(terminal을 게시한 블록의 끝 frame)이다. 두 frame을 relaxed store한 뒤 word를 release store한다.
- 제어 읽기 순서: terminal word(acquire) → 두 frame → start(acquire) → `startMixFrame`.
  - word의 generation이 제어 슬롯 generation과 같을 때만 그 수명의 값이다.
  - 수거해도 슬롯을 지우지 않는다. generation으로 구별한다.
- terminal 사유: `NaturalEnd`, `Stopped`(명령·mailbox stop), `StoppedAll`(stop-all), `StartFailed`(clip 묶음이 비거나 잘못된 Play — 제어 API를 거치지 않은 손 명령).
- RT는 terminal을 게시한 뒤 그 수명의 clip·PCM·DSP 상태를 더 읽지 않는다(Finished 슬롯은 렌더하지 않는다).

#### 3.23.6 ACK·계수 스냅샷과 해제

- RT는 블록을 끝낼 때마다 다음을 출판한다.
  - `completedMixFrame`(= 다음 블록 시작 frame)을 release store한다.
  - 계수(3.22.6절)를 relaxed 원자 스냅샷으로 둔다. 제어는 `CountersSnapshot()`·`CompletedMixFrame()`으로 읽는다.
  - RT 내부 `Counters()`·`MixFrame()`은 RT 스레드나 Inline 렌더 사이에서만 부른다.
- `ReleaseVoice` 조건: 제어 상태가 `Finished`(수거함)이고 `completedMixFrame ≥ releaseMixFrame`이다. 아니면 `Busy`다.
  - 같은 블록 안에서 terminal을 본 제어가 그 블록이 끝나기 전에 해제하지 못하게 하기 위해서다. `endMixFrame`과 비교하면 블록 시작 frame에 끝난 voice가 한 블록 일찍 풀린다.
  - 3.22.4절 표의 `Finished` 해제 조건도 이것으로 바뀐다.
- clip 해제: 사용 수는 voice 해제가 내린다. 사용 수 0이면 그 clip을 쓰던 모든 수명이 위 조건을 지났으므로 RT가 다시 읽지 않는다.
- pending 정리(AU2 스레드 모드): RT가 terminal을 게시하면 그 voice·generation의 pending 항목을 지운다. 먼 미래 Play를 mailbox로 취소해도 pending 칸이 막히지 않는다. Inline `OfflineAudioRender`의 대기 목록은 크기 제한이 없는 `Array`라 정리하지 않는다. 그런 명령은 예약 frame에 stale·무시로 센다.

#### 3.23.7 API 변화

- 제어 쪽(같은 `AudioMixer` 객체의 제어 메서드):
  - `PreparePlay`: clip 묶음·stop-all epoch·순번을 실은 Play
  - `MakeStop(handle, frame, fadeFrames)`, `MakeSetParams(handle, frame, params, rampFrames)`, `MakeSetMasterGain(frame, gain, rampFrames)`: 제어 상태를 검사하고 순번을 매긴다
  - `PublishParams`, `RequestStop`, `StopAll`
  - `QueryVoice`·`CollectFinished`: 시작·terminal 슬롯을 읽는다
  - `CountersSnapshot`, `CompletedMixFrame`
- RT 쪽 `RenderBlock`: 블록 시작에 1 stop-all epoch → 2 stop mailbox → 3 seqlock → 4 명령 순서로 처리한다.
- 손으로 만든 `AudioRtCommand`는 시험 전용이다. clip 묶음이 잘못된 Play는 `StartFailed`로 끝낸다.

#### 3.23.8 시험(Mixer2)

| 대상 | 확인 |
|---|---|
| seqlock | 정상 출판이 다음 블록에 ramp 적용, 홀수·찢어짐 주입 시 이전 값 유지(적용 순번 불변), 같은 순번 재적용 없음, 이전 generation 내용 미적용, Prepared 중 출판이 Play 뒤 적용, seqlock과 시각 지정 SetParams의 순번 비교 |
| stop mailbox | Playing fade, Play가 대기 중일 때 즉시 terminal과 뒤 Play 무시, 반복 stop은 더 이른 끝만(fade 0 steal), 이전 generation mailbox 무시 |
| stop-all | 그 시점 Playing 전부 fade, Prepared 즉시 끝, StopAll 전에 낸 미래 Play(epoch 작음) 즉시 `StoppedAll`, 뒤에 낸 Play는 영향 없음 |
| 시작·terminal | generation 묶음(이전 수명 값 무시), 수거 한 번, `StartFailed` |
| ACK | terminal 게시 블록이 끝나기 전 `ReleaseVoice` `Busy`(블록 시작 frame 즉시 terminal 포함), 뒤 성공, clip 해제 순서 |
| 제어 거부 | Reserved·stale의 Make*·Publish·RequestStop 거부 |
| 두 스레드(범위 한정) | 경계 상태의 정확성은 Inline 주입 시험이 맡는다. 두 `platform::Thread` 시험은 publish·RequestStop·StopAll만 쓴다. 제어는 RT의 ACK(`CompletedMixFrame`)가 정한 frame에 닿을 때까지 출판해 두 스레드가 실제로 겹치게 하고, RT는 블록마다 terminal word를 관찰해 voice마다 한 번만 바뀌는지와 사유를 단언한다(AU1에는 Play를 넘길 큐가 없고, 자체 링은 공용 유틸 중복이라 만들지 않는다). 정해진 반복 수와 원자 gate로 끝나고, RT 스레드는 `RunWithRealtimeAudioFloatingPoint` 안에서 돈다. 단언은 인터리빙과 무관한 불변식만 한다: 적용된 파라미터는 늘 한 출판의 튜플이고, generation마다 terminal은 한 번이다. Windows에는 TSan이 없어 이 시험이 경쟁 부재를 증명하지는 않는다 |

### 3.24 AU5 앞당김 — ADPCM 블록 코덱(개정 12)

사용자 지시 순서(DSP → 믹서 → 코덱 → 임포터)에 따라, 선행작업(PR6 ClientOnly 모듈 유형, PR11 A3/A4 쿠커)이 필요 없는 코덱 부분을 먼저 만든다. 이번 범위는 ADPCM 블록 코덱이다. 청크 표·플랫폼 데이터 헤더·`AudioDecoder` 인터페이스·`AudioDecoderTable`·AssetNugget 키는 쿠커(PR11)와 함께 AU5에서 한다(개정 21: 쿠커 A4c를 앞두고 형식·인터페이스는 3.30절로 앞당겼다. AssetNugget 키는 여전히 A4c 뒤). Opus(`AudioOpus`)는 PR6 뒤다(개정 21: BS2 전까지 일반 Engine 모듈로 두고 등록 방식으로 연결 — 3.30절).

#### 3.24.1 형식(UE `AdpcmAudioDecoder`·`AudioFormatADPCM`과 같은 MS ADPCM)

- 블록: `AdpcmBlockBytes` = 512 byte, 채널 하나다.
  - 머리 `AdpcmPreambleBytes` = 7 byte: 계수 색인(uint8, 0–6), 초기 delta(int16 LE), 두 번째 표본(int16 LE), 첫 표본(int16 LE).
  - 본문은 nibble 쌍이고 상위 nibble이 먼저다.
  - 블록당 `AdpcmFramesPerBlock` = (512 − 7) × 2 + 2 = 1012 frame이다.
- 디코드 식(UE `DecodeNibble`):
  - 예측 = (s1·c1 + s2·c2) / 256이다(C 정수 나눗셈, 0 쪽 버림).
  - 예측에 부호 확장한 nibble × delta를 더하고 [−32768, 32767]로 자른다.
  - s2 = s1, s1 = 결과로 둔다.
  - 다음 delta = max(delta × 적응표[nibble] / 256, 16)이다.
  - 계수표 c1 = {256, 512, 0, 192, 240, 460, 392}, c2 = {0, −256, 0, 64, 0, −208, −232}다.
  - 적응표 = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230}이다.
  - 첫 두 출력은 머리의 첫 표본·두 번째 표본이다.
- 블록 배치(UE와 다름 — 7절): 블록 묶음 k = [채널 0 블록 k][채널 1 블록 k]…로 채널을 블록 단위로 교차한다.
  - UE는 채널마다 모든 블록을 이어 붙인다.
  - Rocks는 청크 경계(3.11.3절)를 블록 묶음 경계에 두어, 청크 하나만으로 모든 채널을 디코드한다.
- 마지막 블록은 남은 frame만 담는다. 인코더는 남는 nibble을 0 표본 입력으로 채운다. UE의 교차 입력 `Encode`(`AudioFormatADPCM.cpp:308`)와 같다. 채널별 배열 `Encode`(:376)는 채널 버퍼 밖을 읽으므로 따르지 않는다. 유효 frame 수는 헤더(플랫폼 데이터)가 정한다.
- 표본: int16이다. 런타임은 float으로 바꾼다(÷ 32768).

#### 3.24.2 인코더(`AudioTools`, Developer)

- 입력: planar float(채널 1–`MaxOutputChannels`)이다.
- int16 변환: round-half-even(×32768, [−32768, 32767]로 자름)이다. 비유한 입력은 `InvalidArgument`다.
- 블록마다 계수 색인 7개를 모두 시험해 제곱 오차 합이 가장 작은 것을 고른다(동률이면 낮은 색인). 오차는 유효 frame만 센다(마지막 블록의 0 채움 구간은 버려지므로). 초기 delta는 UE처럼 적응표[0] = 230이다.
  - UE 인코더는 늘 색인 0을 쓴다("Calculate the optimal starting coefficient" TODO — `AudioFormatADPCM.cpp:247`).
  - 디코더 형식은 같으므로 UE 디코더로도 읽힌다.
- nibble 식은 UE `EncodeNibble`과 같다: 오차 / delta(0 쪽 버림)를 [−8, 7]로 자른다.
- 결정성: 같은 입력이면 같은 byte다(부동소수 연산은 int16 변환 한 번뿐이다).

#### 3.24.3 디코더(`Audio`, 런타임)

- `DecodeAdpcmBlock(block, frames, out)`: 블록 하나를 `frames`(1–1012)만큼 float으로 낸다. 할당하지 않는다.
  - 계수 색인이 6을 넘거나 블록이 7 byte보다 짧으면 `InvalidArgument`다(UE는 로그 뒤 false).
  - 블록 길이가 `frames`에 필요한 nibble보다 짧아도 `InvalidArgument`다.
- `DecodeAdpcmFrames(payload, channels, totalFrames, startFrame, out)`: 블록 묶음 배치에서 `[startFrame, startFrame + out.frameCount)`를 디코드한다. 블록마다 독립이라 임의 위치에서 시작한다(seek). 할당하지 않는다(블록 하나 분량 스택 scratch).
  - 크기 검사는 payload 크기에서 블록 수를 역산해 넘침 없이 한다. 손상된 frame 수가 와도 Fatal이 아니라 `InvalidArgument`다.
  - 호출마다 블록 처음부터 디코드한다(임의 접근용). 상태를 유지하는 스트림 디코더는 `AudioDecoder`와 함께 AU5에서 둔다.
- 손상 데이터 방어: 디코드는 64비트로 계산하고 delta를 `AdpcmMaximumDelta` = 2^20으로 자른다. UE는 같은 입력에서 int32 넘침(UB)이 난다. 유효 스트림의 delta는 표본 범위 근처에서 멈추므로 정상 결과는 UE와 같다.
- 실시간 경로에서 부를 수 있다(무할당·무대기).

#### 3.24.4 시험

| 대상 | 확인 |
|---|---|
| 디코드 | 표(계수·적응)를 시험 안 리터럴과 대조, 무작위 블록을 UE 식 참조 구현과 표본 단위 대조, 머리 두 표본 순서, delta 하한 16, 예측 자르기, 부분 frame(1·2·3·1011), 손상 블록(최대 delta + nibble 8 연속)이 넘치지 않음 |
| 인코드 | 출력 전체를 참조 인코더(리터럴 표·계수 탐색·유효 frame 오차)와 byte 대조(머리·nibble 순서·마지막 블록 채움 포함), 0 아닌 입력의 머리 byte 직접 확인, 사인 왕복 SNR 하한, 계수 선택이 색인 0 고정보다 오차가 작거나 같음, 결정성, 비유한(NaN·Inf) 입력 거부 |
| 임의 접근 | 서로 다른 0 아닌 스테레오에서 블록 경계·블록 안 시작(블록 1 이상 포함)의 디코드가 처음부터 디코드한 결과의 같은 구간과 같음 |
| 음성 | 계수 색인 7, 짧은 블록, payload 크기 불일치, 범위 밖 frame(`startFrame > totalFrames` 포함), 넘침 경계의 frame 수 |

### 3.25 AU4 앞당김 — 원본 디코드 라이브러리: WAV·AIFF(개정 13, 14)

`SoundClipImporter`(A2f `AssetImporter`)와 `SoundClip` 에셋은 AU3(`EngineAudio`)의 `SoundClip` 타입 뒤에 붙인다. 이번 범위는 `AudioTools`의 원본 디코드 함수다. 파일 byte를 받아 planar PCM과 메타데이터를 낸다. ThirdParty 디코더(FLAC·Vorbis·Opus·MP3)는 같은 결과 형식으로 이어서 더한다.

#### 3.25.1 결과 형식(`AudioTools/SourceAudio.h`)

- `DecodedSourceAudio`
  - `channels`(1–8), `sampleRate`, `frames`
  - `sampleFormat`(원본 표본 형식): `UInt8`·`Int16`·`Int24`·`Int32`·`Float32`·`Float64`·손실 코덱
  - `sourceCodec`: `Pcm`·`Flac`·`Vorbis`·`Opus`·`Mp3`
  - `pcm`: planar float32. 채널 하나가 frame 수만큼 이어지고 채널이 차례로 이어진다.
  - `loops`: `smpl` loop 목록 `{start, end(포함하지 않음)}`
  - `cues`: `cue ` 위치 목록 `{id, frame, name}`. name은 `LIST adtl labl`이고 UTF-8 그대로 둔다. 유효하지 않은 UTF-8이면 빈 이름이다. 같은 id의 cue가 여럿이면 모두 같은 이름을 받는다.
  - `ignoredChunks`: 모르는 chunk 수. 임포터의 `IgnoredSourceChunks` 경고에 쓴다.
- float32 변환:
  - 8비트(부호 없음): (v − 128) / 128
  - 16비트: v / 32768
  - 24비트: v / 8388608
  - 32비트 정수: v / 2147483648(double로 계산한 뒤 float로)
  - float64: float로 반올림
  - 24비트 이하 정수는 float32로 손실 없이 표현되므로, 3.10.1절 bulk 저장 형식(int16·int24)으로 정확히 되돌릴 수 있다.
  - float 원본(32·64)의 NaN·무한대, float32로 바꾸면 넘치는 float64는 `InvalidArgument`다. 믹서까지 비유한 값이 가지 않게 임포트에서 막는다(UE는 그대로 받는다).
- 디코더는 시작할 때 결과를 비우고, 실패하면 다시 비운다. 부분 결과(PCM 할당 뒤 실패한 경우 포함)를 남기지 않는다.
- 오류 코드:
  - `InvalidSource`에 대응: `ErrorCode::InvalidArgument`(필수 chunk 없음·크기·길이 불일치, float 비유한 표본)
  - `UnsupportedSourceFormat`에 대응: `ErrorCode::Unsupported`(모르는 부호화, 9채널 이상, rate가 [8000, 192000] 밖)
  - PCM 상한 `MaxSoundClipPcmBytes`(2 GiB) 초과: `ErrorCode::CapacityExceeded`
  - 할당 실패: `OutOfMemory`
  - 진단 코드로 옮기는 일은 임포터가 한다.

#### 3.25.2 WAV(`DecodeWav`, UE `FWaveModInfo::ReadWaveInfo` — `Engine/Private/Audio.cpp`)

- RIFF(LE)·RIFX(BE) `WAVE`다. chunk는 짝수 byte로 맞춘다(홀수 크기 뒤 pad 1 byte).
  - RIFF 크기는 파일 끝으로 자른다. 스트리밍 기록기가 남긴 `0xFFFFFFFF`나 끝 pad 누락도 받는다(UE와 같다). RIFF 크기가 파일보다 작으면 그 크기까지만 읽는다.
  - `data`가 끝을 넘으면 끝까지로 잘라 받는다(UE와 같다). 다른 chunk가 끝을 넘으면 거기서 멈추고 `ignoredChunks`에 센다. 그때까지 `fmt `·`data`가 없으면 `InvalidArgument`다.
- `fmt `(16·18·40 byte)는 다음을 받는다.
  - `WAVE_FORMAT_PCM`(1): 8·16·24·32비트
  - `IEEE_FLOAT`(3): 32·64비트
  - `EXTENSIBLE`(0xFFFE): SubFormat GUID가 PCM이나 IEEE_FLOAT이고, validBits ≤ bitsPerSample. fmt가 40 byte보다 짧으면 `InvalidArgument`, 모르는 GUID는 `Unsupported`다.
    - GUID `{0000tttt-0000-0010-8000-00AA00389B71}`의 Data1(u32)·Data2·Data3(u16)은 파일 끝 방향으로 읽고 Data4 8 byte는 그대로 비교한다. RIFX는 Data1·2·3이 BE다(libsndfile이 그렇게 쓴다).
    - validBits < bitsPerSample이어도 컨테이너 bit 수로 읽는다(아래 bit는 0이다).
  - `blockAlign = channels × bits/8`이어야 한다(아니면 `InvalidArgument`).
  - 그 밖의 부호화(ADPCM·µ-law 등)는 `Unsupported`다(UE도 거부).
- `data`: 크기가 blockAlign의 배수가 아니면 남는 byte를 버린다(UE와 같다).
  - `fmt `보다 먼저 와도 된다.
  - 둘 중 하나가 없거나 frame이 0이면 `InvalidArgument`다.
- `smpl`: loop마다 `{start, end}`다. 파일 값의 end는 포함 위치라 +1로 바꾼다. end가 frame 수를 넘으면 그 loop를 버리고 셈에 넣지 않는다. 반복 방식(`dwType` — 정방향·왕복·역방향)은 보지 않는다(UE도 보지 않는다).
- `cue `: `{id, dwPosition}`. 위치는 `dwPosition`이다(UE `Audio.cpp`와 같다. `dwSampleOffset`은 보지 않는다). frame 수보다 크면 버린다. `LIST`/`adtl`의 `labl`(`{id, 문자열}`)로 이름을 붙인다. 첫 NUL부터 뒤는 떼고, NUL이 없으면 본문 전체가 이름이다.
- `smpl`·`cue `의 개수 필드가 본문보다 크면 본문에 있는 것까지만 읽는다. `labl`이 `LIST` 끝을 넘으면 거기서 멈추고 센다. 메타데이터 오류는 PCM을 막지 않는다.
- 모르는 chunk(`fact`·`bext`·`id3 ` 등 포함)는 건너뛰고 `ignoredChunks`를 센다. 같은 chunk가 둘 이상이면 첫 것을 쓴다.

#### 3.25.3 AIFF·AIFF-C(`DecodeAiff`)

- `FORM`(BE), `AIFF`·`AIFC`다.
  - `COMM`: channels, frames, bits, rate(80비트 IEEE extended → 정수 rate. 소수가 있거나 범위 밖이면 `Unsupported`)
  - AIFC면 압축 type을 받는다: `NONE`(BE 정수), `sowt`(LE 정수), `fl32`·`FL32`(BE float32). 그 밖은 `Unsupported`다.
  - `SSND`: offset·blockSize 머리 뒤 offset byte를 건너뛴 곳부터 PCM이다. 길이가 frames × channels × bytes보다 짧으면 `InvalidArgument`다.
  - FORM 크기와 끝을 넘는 `SSND`는 WAV처럼 파일 끝으로 자른다. 다른 chunk가 끝을 넘으면 멈추고 센다.
  - PCM 상한은 `COMM`의 frame 수로 `SSND`를 보기 전에 판정한다. WAV는 `data` 크기로 frame 수를 얻으므로 상한을 넘으려면 512 MiB 넘는 입력이 든다 — 파일 경로 시험은 AIFF로 한다.
- 정수 bits는 1–32다. 표본은 왼쪽 맞춤 ceil(bits/8) byte다(규격). 아래 bit는 0이므로 컨테이너 크기(8·16·24·32비트)로 읽는다. 12비트는 `Int16`, 20비트는 `Int24`로 보고한다.
- 8비트는 부호 있음이다(WAV와 다르다). 저장 형식은 `Int16`으로 보고한다 — 넓혀도 손실이 없다.
- `MARK`·`INST` loop는 지금 읽지 않는다. 모르는 chunk로 센다(필요해지면 더한다).

#### 3.25.4 시험

| 대상 | 확인 |
|---|---|
| WAV 부호화 | 8·16·24·32비트 정수, float32·64, fmt 18 byte, EXTENSIBLE(PCM·float GUID, RIFF·RIFX), RIFX 16·24비트·float32, 1·2·8채널: 표본 값 정확 일치(손으로 만든 byte) |
| WAV chunk | RIFF·RIFX 각각: 홀수 크기 pad, `fmt `·`data` 순서, 중복 `fmt `·`data`, `smpl` loop(+1 변환·end = frame 수 받음·넘으면 버림·개수 필드 초과), `cue `(dwPosition·frame 수와 같음·개수 필드 초과)·`labl` 이름(NUL 뒤 뗌·NUL 없음·잘못된 UTF-8), 모르는 chunk 계수 |
| 잘린 파일 | RIFF 크기 0xFFFFFFFF, 끝을 넘는 `data` 자름, `data` 뒤 넘치는 메타 chunk 멈춤·계수, `LIST` 안 넘치는 `labl` |
| AIFF | `NONE`·`sowt`·`fl32`·`FL32`, 80비트 rate(44100·48000·22050), 8비트 부호(`Int16`), 24·32비트, 12·20비트 왼쪽 맞춤, 스테레오, SSND offset ≠ 0, 끝을 넘는 SSND |
| 음성 | blockAlign 불일치, 압축 WAV `Unsupported`, 12비트 PCM·16비트 float WAV, EXTENSIBLE 짧은 fmt·모르는 GUID·validBits > bits, float NaN·±무한대·float64 넘침, 9채널, rate 범위 밖, 빈 data, fmt 잘림, AIFF 모르는 압축·소수 rate·rate 4000·9채널·33비트·짧은 SSND·SSND offset 초과·SSND 없음·frame 0·형식 id |
| 상한 | `MaxSoundClipPcmBytes`를 넘는 크기 → `CapacityExceeded`(할당 전, AIFF는 `COMM`만으로) |
| 실패 시 비움 | 앞서 채운 결과에 NaN WAV·짧은 AIFF를 디코드하면 frame·loop·cue·계수가 모두 0 |

### 3.26 AU4 앞당김 — MP3 원본 디코드(개정 15, 16)

3.25절과 같은 결과 형식(`DecodedSourceAudio`)으로 MP3를 디코드한다. ThirdParty `Minimp3`(AD13)를 처음 반입한다.

#### 3.26.1 ThirdParty `Minimp3`(`Engine/Source/ThirdParty/Minimp3`)

- lieff/minimp3 master `afb604c06bc8`(2021-11-30, CC0-1.0). 릴리스·서명이 없어 commit archive SHA-256으로 고정한다. 반입 파일은 `minimp3.h`·`minimp3_ex.h`·`LICENSE`다. 수정 없음.
- `Minimp3Glue.c`가 구현을 한 번 컴파일한다(`MINIMP3_IMPLEMENTATION`, `MINIMP3_FLOAT_OUTPUT`, `MINIMP3_NO_STDIO`, `MINIMP3_ONLY_MP3`). `MINIMP3_ONLY_MP3`는 Layer I·II 디코더를 빼 공격면을 줄인다(어차피 거부한다). `minimp3_ex.h`의 static tag 함수를 Rocks 이름으로 내보낸다.
  - `RocksMinimp3SkipTags`: 앞 ID3v2, 끝 ID3v1·APEv2를 건너뛴다. 빈 입력은 그 전에 `InvalidArgument`로 돌려준다(NULL끼리 빼기 방지).
  - `RocksMinimp3CheckVbrTag`: Xing/Info(LAME) tag를 읽는다. upstream은 tag 필드(TOC 100 byte 포함, side info 뒤 약 120 byte)를 읽은 뒤에야 frame 크기를 보므로, frame을 0으로 채운 버퍼(free-format 상한 2304 + 512 byte)에 복사해 넘긴다. 상한보다 큰 frame은 tag 없음으로 본다.
  - `minimp3_ex`의 load·ex 함수(malloc·realloc 사용)는 부르지 않는다. frame마다 `mp3dec_decode_frame`을 부른다.
  - 한 번에 넘기는 byte 수는 `INT_MAX / 2`까지다. 인자가 int이고 upstream frame 탐색이 `i + 2*k` 같은 int 덧셈을 하므로, 2 GiB 넘는 입력에서 overflow를 막는다.
- 경고 0: clang C17 `-Wall -Wextra -Wpedantic -Werror`, clang-cl `/W4 /WX`(`/Od`·`/O2`)에서 억제 없이 컴파일된다.
- SIMD는 빌드 때 고른다(x86-64 SSE2, ARM64 NEON). 런타임 CPU 탐지가 없어 디코드 결과가 아키텍처마다 고정이다(3.10.1절 결정성).
- `AudioTools`만 Private 의존으로 링크한다(헤더는 이 모듈 밖으로 나가지 않는다).

#### 3.26.2 `DecodeMp3`

- 받는 것: MPEG-1·2·2.5 Layer III. Layer I·II는 `Unsupported`다(minimp3는 디코드하지만 `.mp3` 원본은 Layer III만 받는다. `.mp2`는 범위 밖).
- 순서:
  1. tag 건너뛰기: ID3v2·ID3v1·APEv2
  2. 1차 훑기(`pcm = NULL`, 머리만 읽음): 채널·rate·layer와 채널당 표본 수 합을 센다.
     - 채널·rate가 중간에 바뀌면 `Unsupported`다. 한 `SoundClip`은 형식이 하나다(UE도 libsndfile 변환에서 한 형식으로 받는다).
     - 첫 frame에 Xing/Info tag가 있으면 그 frame은 길이에 넣지 않고 디코드하지 않는다(무음 frame이다. minimp3_ex와 같다). 그 뒤 decoder를 비워, 디코드 단계(새 decoder)와 같은 방식(frame 탐색)으로 다음 frame을 연다.
     - frame당 채널 표본 수(MPEG-1 1152, MPEG-2·2.5 576)를 첫 frame에서 기억한다. rate가 고정이라 stream 안에서 같다.
     - frame이 하나도 없으면 `InvalidArgument`, rate·채널 범위 밖이면 `Unsupported`다.
     - 표본 수 합으로 PCM 상한을 디코드 전에 판정한다(`CapacityExceeded`).
  3. 디코드: 교차 float로 받은 뒤 planar로 옮긴다. 1차 훑기 합만큼 미리 잡는다.
     - frame마다 layer·채널·rate가 1차 값과 같은지 다시 본다. 디코드 단계는 깨진 side info 뒤 frame을 다시 찾으므로(`mp3d_find_frame`: 뒤 머리 최대 10개 연쇄) 1차 훑기(다음 머리 하나)와 다른 frame을 볼 수 있다. 다르면 원본 손상으로 `InvalidArgument`다.
     - main data가 깨지거나 bit reservoir가 모자라 표본을 내지 못한 frame은 frame당 표본 수만큼 무음을 넣어 길이를 지킨다.
  4. gapless(minimp3_ex `mp3dec_load`와 같은 규칙): LAME tag에 frame 수가 있으면
     - delay = encoder delay + 디코더 지연 529, padding = encoder padding − 529(음수면 0)
     - 길이 = min(디코드 − delay, tag frame 수 × frame당 표본 수 − delay − padding). 앞 delay는 언제나 자른다.
     - 그래서 frame 수가 원본 길이와 같다. 잘린 파일은 앞만 자르고 끝은 남기고(tag 길이보다 짧다), tag 길이 뒤에 frame이 더 붙은 파일은 tag 길이에서 끊는다.
     - frame 수가 없는 tag(Xing flags에 frame 수 없음)나 LAME 확장이 없는 tag(delay·padding 0)는 tag frame만 빼고 자르지 않는다. tag가 없으면 자르지 않는다(frame 수 × frame당 표본 수).
     - 자른 뒤 0 frame이면 `InvalidArgument`다.
- 결과: `sampleFormat = Lossy`, `sourceCodec = Mp3`, loop·cue 없음. 실패하면 결과를 비운다(3.25.1절과 같다).
- 시작 부분 bit reservoir가 비어 첫 frame이 표본을 못 내는 일은 처음부터 디코드하면 생기지 않는다. 생기면 위 무음 규칙을 따른다.
- 한계:
  - 깨진 side info 뒤 재동기화로 디코드 단계가 frame을 건너뛰면 1차 훑기보다 짧아질 수 있다(1차 훑기에서 재현할 수 없다).
  - encoder padding이 529보다 작으면 원본보다 그 차이만큼 길다(padding을 0으로 본다. minimp3_ex와 같다).
  - 메모리 정점은 교차 임시 버퍼와 planar 결과를 합쳐 PCM 상한의 두 배(4 GiB)까지다. 도구 전용이라 받는다.
  - stack은 약 40 KB(frame 버퍼 9 KB, `mp3dec_t` 6.7 KB, minimp3 scratch 16 KB)를 쓴다. 임포트 worker stack은 이보다 커야 한다.

#### 3.26.3 시험

입력은 `Tests/Mp3TestFixtures.h`의 byte 배열이다. 만든 방법:

- `Mp3MonoLame`: `ffmpeg -f lavfi -i sine=frequency=440:sample_rate=44100:duration=0.25 -c:a pcm_s16le sine44.wav`, `lame -b 128 --tt Rocks --add-id3v2 sine44.wav`(LAME 4.0). 11 frame × 1152 − delay 576 − padding 1071 = 11025.
- `Mp3StereoNoTag`: `ffmpeg … sine 44100 Hz 0.1 s, pan=stereo|c0=c0|c1=-1*c0, -c:a libmp3lame -b:a 128k -write_xing 0 -id3v2_version 0`
- `Mp2Layer2`: `ffmpeg … -c:a mp2 -b:a 64k -f mp2`
- `Mp3MonoNoTag`: `ffmpeg … sine 44100 Hz 0.05 s, -c:a libmp3lame -b:a 128k -write_xing 0 -id3v2_version 0`(채널 변경 시험용)
- `Mp3Mpeg2Mono16k`·`Mp3Mpeg2Mono22k`: libmp3lame 24 kbps tag 없는 16000 Hz·22050 Hz 0.3 s. rate 변경 시험은 둘을 이어 붙인다.
- APEv2 건너뛰기는 따로 시험하지 않는다. 끝의 tag byte는 건너뛰지 않아도 frame이 아닌 byte로 무시된다.
- ffmpeg(9.1)가 쓰는 LAME tag는 delay가 529 모자라다(ffmpeg 자신도 디코드 결과가 529 표본 어긋난다). 그래서 gapless 기준 파일은 LAME CLI로 만든다.

| 대상 | 확인 |
|---|---|
| gapless | `Mp3MonoLame`: ID3v2·ID3v1 건너뜀, tag frame 제외, frame 수 = 11025, 원본 sine과 정렬(SNR ≥ 15 dB. 529 어긋나면 0 dB 아래) |
| 잘린 파일 | tag frame + audio frame 6개로 자름: 6 × 1152 − 1105(앞만 자르고 끝은 남김) |
| tag 변형 | padding < 529(0으로 봄, 앞만 자름), LAME 확장 없음(자르지 않음), frame 수 flag 없음(−1, 자르지 않음) — 모두 tag frame은 뺌 |
| tag 없음 | `Mp3StereoNoTag`: 2채널, 5 × 1152 frame, 오른쪽 = −왼쪽. `Mp3Mpeg2Mono16k`: MPEG-2 16000 Hz, 11 × 576 frame |
| 무음 채움 | `Mp3StereoNoTag` 세 번째 frame의 main_data_begin을 511로: 길이 5 × 1152 유지, 그 frame 자리는 0 |
| 고정값 | `Mp3MonoLame` 디코드 결과 float bit의 SHA-256 — x86-64 고정값(Debug·Release 같음). ARM64(NEON) 고정값은 macOS 기기에서 잰 뒤 넣는다(그때까지는 두 번 디코드가 같은지만 본다) |
| 거부 | Layer II, rate 변경(16000 → 22050), 채널 변경(stereo → mono) `Unsupported`. 빈 입력·0 byte·ID3v2만·머리 4 byte `InvalidArgument`. 실패 시 비움(코덱·형식도 처음 값) |
| 상한 | MPEG-2.5 8 kHz 8 kbps mono frame(72 byte)을 상한 + 1 frame만큼(약 67 MB) 이어 붙임: 1차 훑기만으로 `CapacityExceeded` |
| 견고성 | 앞부분 길이 37 byte 간격 전부(받으면 길이 ≤ 원본), byte 8개 무작위 교체 200회(멈춤·범위 밖 읽기 없음, 받으면 표본 유한) |

### 3.27 AU4 앞당김 — FLAC 원본 디코드(개정 17, 18)

3.25절과 같은 결과 형식으로 FLAC을 디코드한다. ThirdParty `Flac`(libFLAC, AD13)을 반입한다.

#### 3.27.1 ThirdParty `Flac`(`Engine/Source/ThirdParty/Flac`)

- libFLAC 1.5.0(BSD-3-Clause, `COPYING.Xiph`). 공식 릴리스 tarball이고, GitHub 릴리스와 downloads.xiph.org 두 곳의 SHA-256이 같다.
- 반입 범위는 stream decoder에 필요한 35개 파일이다: 공개 decoder 헤더, share compat 헤더, decoder 원본과 private 헤더, Windows UTF-8 파일 wrapper, 라이선스. encoder·Ogg FLAC·SIMD intrinsics·도구·시험·문서는 들이지 않는다.
- `config.h` 없이 빌드한다. configure 결과는 manifest define이다.
  - little endian(모든 Rocks 대상), 64비트 bit reader word
  - Ogg FLAC 없음(`FLAC__HAS_OGG=0`)
  - x86·NEON intrinsics 없음, `FLAC__NO_ASM`
  - 무손실이고 decoder가 정수만 쓰므로 디코드 결과가 모든 아키텍처에서 같다.
- glue:
  - `FlacFormat.c`: `PACKAGE_VERSION`(version 문자열)을 정의하고 `format.c`를 컴파일한다.
  - `FlacWindowsFileIo.c`: Windows에서만 `src/share/win_utf8_io/win_utf8_io.c`를 컴파일한다. 플랫폼 선택은 manifest `platformDefines`(Windows: `ROCKS_FLAC_WIN_UTF8_IO=1`)에 두고 glue는 그 매크로만 본다(엔진 소유 소스는 native backend 밖에서 OS 매크로로 분기하지 않는다). `compat.h`가 Windows에서 `flac_fopen`을 그 파일의 `fopen_utf8`로 바꾸고, file 기반 init 함수가 이를 참조한다. 그 파일은 `windows.h`의 `malloc` 선언에 기대는데 Rocks 빌드는 `WIN32_LEAN_AND_MEAN`이라 `<stdlib.h>`를 먼저 include한다.
- 경고 0을 위한 수정(`Rocks.patch`, manifest `modifications`). Microsoft CRT가 deprecated로 표시한 호출 두 곳이다. `_CRT_SECURE_NO_WARNINGS`는 쓰지 않는다([컴파일러 진단](CompilerDiagnostics.ko.md)).
  - `stream_decoder.c`: `file_length_callback_`의 `fileno`를 Windows에서 `_fileno`로(같은 파일의 stdin 경로가 이미 그렇게 한다).
  - `win_utf8_io.c`: `_wfopen`을 `_wfsopen(…, _SH_DENYNO)`로. 공유 방식이 같다.
- 원본에 있는 진단 지시문(`format.c`·`lpc.c`의 MSVC `#pragma warning`, `compat.h`의 `#pragma message`)은 `pragmaDirectives`에 기록한다(byte 그대로인 파일이다).
- `AudioTools`만 Private 의존으로 링크한다. libFLAC 내부 할당은 malloc이다(도구 전용이라 받는다).
- Debug(NDEBUG 없음)에서는 libFLAC이 손상 입력에 stderr로 진단 문장을 쓰고 `FLAC__ASSERT`가 CRT assert가 된다. upstream 동작이고 도구 전용이라 받는다(manifest notes).
- Linux 도구 빌드가 생기면 glibc `-std=c17`에서 `fileno`가 숨겨지므로 `_POSIX_C_SOURCE=200809L` platform define이 필요하다.

#### 3.27.2 `DecodeFlac`

- 메모리에서 stream callback으로 디코드한다(`FLAC__stream_decoder_init_stream`, read·eof만 주고 seek·tell·length는 주지 않는다). file 기반 init 함수는 부르지 않는다.
- STREAMINFO:
  - 없거나 채널 0·bit 수가 4–32 밖이면 `InvalidArgument`
  - 9채널 이상·rate가 [8000, 192000] 밖이면 `Unsupported`
  - STREAMINFO가 둘이면 `InvalidArgument`다(libFLAC은 뒤 값을 쓰므로 모호하다).
  - 길이(total samples)를 알면 디코드 전에 PCM 상한을 판정하고 교차 버퍼를 미리 잡는다. 미리 잡는 양은 16M 표본(64 MiB)까지다. STREAMINFO 길이는 CRC가 없는 숫자 하나라, 손상·조작된 작은 파일이 큰 버퍼를 잡지 않게 한다. 나머지는 쌓으며 늘린다. 길이를 모르면(파이프로 쓴 파일, 0) 쌓는 동안 상한을 본다.
- frame마다 채널·bit 수·rate가 STREAMINFO와 같아야 한다. 다르면 `Unsupported`다(한 `SoundClip`은 형식이 하나다).
- frame이 이어져야 한다: frame 머리의 시작 표본 번호가 지금까지 디코드한 frame 수와 같아야 한다(다르면 `InvalidArgument`).
- 오류 callback:
  - `LOST_SYNC`는 표시만 한다. 그 뒤 audio가 오면 `InvalidArgument`다. libFLAC은 frame 안 손상(subframe 머리 bit, wasted bits, predictor order, residual 해석 오류)도 CRC 비교 전에 `LOST_SYNC`로 보고하고, 빠진 구간을 무음 frame으로 채워 길이를 맞추기 때문이다(그 뒤의 `MISSING_FRAME`은 이미 오류를 보냈다는 이유로 나가지 않는다). 마지막 frame 뒤 쓰레기(끝의 ID3v1 등)는 뒤따르는 audio가 없으므로 받는다.
  - 그 밖(`FRAME_CRC_MISMATCH`·`BAD_HEADER`·`UNPARSEABLE_STREAM`·`BAD_METADATA`·`OUT_OF_BOUNDS`·`MISSING_FRAME`)은 `InvalidArgument`다. libFLAC은 이때 무음을 넣고 계속하지만, 무손실 원본을 조용히 바꿔 임포트하지 않는다(MP3는 손실 원본이라 무음 채움으로 길이를 지키는 것과 다르다).
- MD5 확인을 켠다. STREAMINFO MD5가 0이 아니면 `FLAC__stream_decoder_finish`가 디코드 결과와 맞춰 보고, 다르면 `InvalidArgument`다.
- 길이를 알면 디코드한 frame 수가 같아야 한다(잘린 파일 `InvalidArgument`). 더 길어도 `InvalidArgument`다.
- 앞 ID3v2는 libFLAC이 건너뛴다.
- init 실패는 할당 실패면 `OutOfMemory`, 그 밖(내부 오류)은 `InvalidArgument`다.
- 변환: 정수 표본 / 2^(bits−1). 저장 형식은 bit 수로 정한다: 16 이하 `Int16`(8비트 이하도 넓혀 보고), 24 이하 `Int24`, 그 위 `Int32`. 12·20비트 같은 값도 float32에서 정확하고, 넓힌 저장 형식으로 정확히 되돌릴 수 있다.
- loop·cue는 읽지 않는다(`CUESHEET`·foreign `smpl` 메타데이터는 필요해지면 더한다). `ignoredChunks`는 0이다.
- 결과: `sourceCodec = Flac`. 실패하면 결과를 비운다.
- 한계:
  - 무결성 수단은 frame CRC(머리 CRC-8, frame CRC-16), STREAMINFO 길이, MD5뿐이다. 길이·MD5가 모두 0인 파일(파이프로 쓴 파일)은 frame 경계에서 잘리면 오류 없이 짧게 받는다(마지막 frame 중간에서 잘리면 그 frame은 libFLAC이 버린다).
  - ID3v2 바로 뒤가 `fLaC`가 아니면(선언 크기 뒤 padding, ID3v2.4 footer) libFLAC 1.5.0이 메타데이터를 찾지 못해 `InvalidArgument`다(`find_metadata_`가 ID3 건너뛰기 뒤 상태를 되돌리지 않는다. upstream 동작). ID3 없이 `fLaC` 앞에 쓰레기가 있는 파일은 받는다(첫 frame 앞의 `LOST_SYNC`는 표시하지 않는다).
  - 첫 frame의 시작 표본 번호가 0이 아닌 stream(라이브 stream을 잘라낸 파일)은 frame 연속 검사로 `InvalidArgument`다.
  - 끝의 ID3v2·APEv2 binary(그림 등)에 `FF F8`·`FF F9`가 있으면 frame 머리로 해석하다 CRC-8 불일치(`BAD_HEADER`)로 거부된다. 드문 경우라 받는다.
  - 메모리 정점은 교차 임시 버퍼와 planar 결과를 합친 값이다. 길이를 모르면 Array가 늘어나는 순간 옛 버퍼와 새 버퍼가 함께 있어 PCM 상한의 약 2.4배까지 갈 수 있다(MP3도 같은 구조).

#### 3.27.3 시험

입력은 `Tests/FlacTestFixtures.h`의 byte 배열이다. 만드는 방법:

- 정수 패턴 x = frame × 2654435761 + channel × 40503(mod 2^32)의 위 bits를 부호 있는 값으로 쓴 WAV(16비트 stereo 44100 Hz 1000 frame, 24비트 mono 96000 Hz 600 frame, 16비트 mono 48000 Hz 500 frame)
- `ffmpeg -fflags +bitexact -flags:a +bitexact -i s16.wav -c:a flac -frame_size 256 s16.flac`(24비트도 같다), 길이·MD5 없는 판은 `-c:a flac -frame_size 128 -f flac -`로 파이프에 쓴다
- 생성기가 frame 머리 CRC-8로 frame 시작과 첫 subframe 머리 위치를 확인해 `<이름>FrameOffsets`·`<이름>SubframeOffsets`로 적는다(손상 시험이 쓴다)
- 무손실이라 시험이 같은 식으로 기대값을 만든다

| 대상 | 확인 |
|---|---|
| 정확 일치 | 16비트 stereo(`Int16`, 4 frame), 24비트 mono(`Int24`, 3 frame), 파이프로 쓴 16비트 mono(길이·MD5 0, 4 frame): 표본 전부 패턴과 같음 |
| tag | 앞 ID3v2·끝 ID3v1(128 byte)을 붙여도 받고 결과가 같음 |
| 거부 | FLAC 아님·빈 입력·`fLaC`만 `InvalidArgument`. audio byte 하나 바꿈(CRC 불일치) `InvalidArgument`. 길이를 아는 파일의 끝 100 byte를 자름 `InvalidArgument`. STREAMINFO rate 4000 Hz `Unsupported`. 실패 시 비움 |
| STREAMINFO | MD5만 바꿈(finish 실패) `InvalidArgument`. MD5 0 + 마지막 frame 제거(길이 불일치) `InvalidArgument`. MD5 0 + 길이 500(길이 초과) `InvalidArgument`. 길이 36 bit 최대값 `CapacityExceeded`(디코드 전). bit 수 24로(frame과 형식 불일치) `Unsupported` |
| 손상 은폐 | 파이프 파일 가운데 frame의 subframe 머리 bit를 켬(`LOST_SYNC` + libFLAC 무음 채움) `InvalidArgument` |
| 한계 | 파이프 파일을 frame 경계에서 자르면 짧게 받고 앞부분은 패턴과 같음 |
| 견고성 | 길이를 모르는 파일의 앞부분 41 byte 간격 전부(받으면 패턴의 앞부분과 같음), audio 영역 byte 4개 무작위 교체 200회(멈춤·범위 밖 읽기 없음, 받으면 표본 유한) |

ffmpeg flac 파일은 메타데이터 뒤에 8 KiB PADDING이 있어 손상 시험은 파일 끝쪽 2 KiB(audio frame)만 바꾼다.

### 3.28 AU4 앞당김 — Ogg Vorbis 원본 디코드(개정 18)

3.25절과 같은 결과 형식으로 Ogg Vorbis를 디코드한다. ThirdParty `Ogg`(libogg)·`Vorbis`(libvorbis + vorbisfile, AD13)를 반입한다. `Ogg`는 Opus 파일 디코드(opusfile)도 쓴다.

#### 3.28.1 ThirdParty `Ogg`·`Vorbis`

- libogg 1.3.6, libvorbis 1.3.7(둘 다 BSD-3-Clause). 공식 릴리스 tarball이고 downloads.xiph.org의 SHA256SUMS와 SHA-256이 같다.
- `Ogg`: `bitwise.c`·`framing.c`와 헤더, `COPYING`. `os_types.h`가 Windows(MSVC·clang-cl)와 macOS에서 정수 형식을 스스로 정의하므로 configure가 만드는 `config_types.h`는 들이지 않는다.
- `Vorbis`: libvorbis·vorbisfile 원본에서 encoder 설정(`vorbisenc.c`와 mode·codebook 표)을 뺀 것. `block.c`가 공용 dsp 설정에서 분석 쪽(`psy.c`·`envelope.c`·`bitrate.c`)을 참조하므로 그 파일들은 컴파일한다.
  - libvorbis가 `ogg/ogg.h`를 include하므로 `Vorbis.Build.cs`가 `Ogg` include 경로를 더한다(ThirdParty 모듈 사이 의존이 없어 규칙 `Configure`를 덮어쓴다). 소비 모듈은 `Vorbis`·`Ogg`를 이 순서로 링크한다(왼쪽부터 푸는 정적 링커 대비).
  - include 경로는 `include`만 내보낸다. `lib` 안 헤더는 같은 폴더 따옴표 include라 경로가 필요 없고, `os.h`·`misc.h` 같은 흔한 이름이 소비 모듈로 새지 않는다.
  - `lookup.c`는 들이지만 컴파일하지 않는다(`FLOAT_LOOKUP`·`INT_LOOKUP`이 없으면 정의가 없다).
- Linux 도구 빌드가 생기면: `Ogg`에 `config_types.h`, `Vorbis`에 `HAVE_ALLOCA_H`(glibc `-std=c17`은 `alloca`를 선언하지 않는다)가 필요하다.
  - 소비 define `OV_EXCLUDE_STATIC_CALLBACKS`(라이브러리 자체 guard): `vorbisfile.h`가 include하는 C++ 단위마다 쓰지 않는 stdio callback 함수를 넣지 않게 한다.
- 경고 0을 위한 수정(`Vorbis/Rocks.patch`, manifest `modifications`):
  - `info.c`: comment 문자열을 `strcpy`·`strcat` 대신 길이를 아는 `memcpy`로 만든다(CRT deprecated). 결과 문자열은 같다.
  - `floor1.c`: encoder floor 맞춤(`fit_line`)에서 더하기만 하고 읽지 않는 지역 합 `y2b`를 지운다(`-Wunused-but-set-variable`).
  - `vorbisfile.c`: Windows `ov_fopen`의 `fopen`을 `_fsopen(…, _SH_DENYNO)`로(CRT deprecated, 공유 방식 같음), `host_is_big_endian`에 `(void)` 원형(`-Wstrict-prototypes`), `ov_read_filter`의 `fpu`를 쓴 것으로 표시(ARM64에서 `os.h`의 반올림 매크로가 비어 `-Wunused-variable`).
- Vorbis 디코드는 SIMD·런타임 CPU 분기가 없는 float 계산이라 아키텍처·컴파일 설정마다 결과가 고정이다(3.10.1절).

#### 3.28.2 `DecodeVorbis`

- 디코드 전에 page 구조를 확인한다(`OggValidation`, Opus 파일 디코드도 쓴다). vorbisfile은 CRC가 틀린 page를 건너뛰고, link 끝 page나 다음 link의 BOS page가 빠져도(BOS가 빠지면 그 link를 다중화된 다른 stream으로 보고 통째로 건너뛴다) 오류 없이 짧게 디코드하기 때문이다.
  - 원본 전체를 64 KiB씩 libogg sync에 넣어 `ogg_sync_pageseek`로 page를 나눈다. 건너뛴 byte(앞뒤 쓰레기, CRC 불일치)와 끝의 덜 찬 page는 `InvalidArgument`다.
  - logical stream마다 BOS page로 시작하고 page 번호가 이어지며 EOS page로 끝나야 한다(빠진 page·잘림은 `InvalidArgument`).
  - stream은 차례로 이어져야 한다. 한 stream이 열린 채 다른 BOS가 오면(다중화) `Unsupported`, 이미 나온 serial이 다시 오면(`cat a.ogg a.ogg`) 규격 위반이라 `InvalidArgument`다.
- vorbisfile을 read callback만 주어 순차(non-seekable)로 연다(`ov_open_callbacks`, seek·tell·close 없음).
  - link마다 audio page가 하나(EOS page)뿐인 연결 파일을 seekable로 열면 vorbisfile 1.3.7이 첫 link를 통째로 건너뛴다(시험 파일로 확인. 여러 page짜리 link는 seekable로도 모두 나온다). 순차 모드에서는 어느 경우도 모든 link가 나온다. 순차 모드라 `ov_pcm_total`로 길이를 미리 알 수 없다.
- 첫 link 형식: 채널 0·정보 없음 `InvalidArgument`, 9채널 이상·rate 범위 밖 `Unsupported`.
- `ov_read_float`로 읽어 교차 버퍼에 쌓으며 PCM 상한을 본다(`CapacityExceeded`). 끝에 planar로 옮긴다.
  - 읽을 때마다 지금 link의 채널·rate가 첫 link와 같은지 본다. 다르면 `Unsupported`다.
  - `OV_HOLE`: 순차 모드의 vorbisfile은 link 경계를 지날 때도 한 번 돌려준다(`_fetch_headers` 뒤 이미 넣은 마지막 header page를 다시 넣어 page 번호가 어긋난다. 규격에 맞는 stream에서는 결정적이다). 표시만 하고, 다음 읽기가 새 link에서 나오면 경계로 본다. 같은 link에서 나오거나 바로 stream이 끝나면 page 손상·누락이라 `InvalidArgument`다. 연이은 `OV_HOLE`과 그 밖 음수도 `InvalidArgument`다. 손실 원본이지만 Ogg page에 CRC가 있어 손상을 정확히 알므로 메우지 않고 거부한다(MP3의 무음 채움과 다르다).
  - 비유한 표본(손상된 setup·packet)은 `InvalidArgument`다.
- 길이는 granule 기준이다. vorbisfile이 마지막 page granule로 끝을 자르므로 frame 수가 원본 길이와 같다.
- 채널 순서: Vorbis 순서를 3.3절 내부 순서로 바꾸며 planar로 옮긴다(표는 3.29.2절).
- 결과: `sampleFormat = Lossy`, `sourceCodec = Vorbis`, loop·cue 없음(`LOOPSTART` 같은 comment는 읽지 않는다. UE도 읽지 않는다). 실패하면 결과를 비운다.
- 한계: 메모리 정점은 FLAC과 같다. Ogg page CRC·번호·BOS·EOS로 손상·누락·잘림을 모두 잡으므로 page 단위 무결성 한계는 없다.

#### 3.28.3 시험

입력은 `Tests/VorbisTestFixtures.h`의 byte 배열이다(`<이름>PageOffsets`는 page 시작 위치). ffmpeg lavfi sine·pink noise(`anoisesrc`, 여러 page가 나오게)를 s16le로 받아 libvorbis 1.3.7 reference 인코더로 만들었다(`examples/encoder_example.c`와 같은 흐름: `vorbis_encode_init_vbr`, header를 따로 flush, serial 1001–1004). ffmpeg libvorbis 인코더는 쓰지 않는다: ffmpeg 9.1 Ogg muxer는 header와 audio를 한 page에 섞고, ffmpeg 디코드는 끝을 128 표본 덜 낸다.

| 대상 | 확인 |
|---|---|
| 길이·정렬 | stereo 44100 Hz: frame 수 = 마지막 granule 11025, 원본 sine과 정렬(SNR ≥ 20 dB), 오른쪽 = −왼쪽. mono 22050 Hz 2205 frame |
| 연결 stream | 같은 형식 link 둘(4410 + 2205 = 6615), audio page가 둘씩인 잡음 link 둘(33075 + 22050, link 경계 `OV_HOLE`을 지남). rate가 다른 link·채널이 다른 link `Unsupported` |
| page 무결성 | 첫 link 가운데 audio page·첫 link EOS page·둘째 link BOS page 한 byte 손상, page 중간 잘림, page 경계 잘림(EOS 없음), page 하나 누락, 같은 serial 재연결, 앞·뒤 쓰레기: 모두 `InvalidArgument`. 다중화(두 BOS가 연달아): `Unsupported` |
| 거부 | Ogg 아님·빈 입력·첫 page 머리만 `InvalidArgument`. audio page 한 byte 손상 `InvalidArgument`. 실패 시 비움 |
| 견고성 | 앞부분 43 byte 간격 전부(받으면 길이 ≤ 원본), byte 4개 무작위 교체 200회(멈춤·범위 밖 읽기 없음, 받으면 표본 유한) |

### 3.29 AU4 앞당김 — Ogg Opus 원본 디코드(개정 19, 20)

3.25절과 같은 결과 형식으로 Ogg Opus를 디코드한다. ThirdParty `Opus`(libopus)·`OpusFile`(opusfile, AD13)을 반입한다. 클라이언트 런타임 Opus 코덱(`AudioOpus`, PR6 대기)과 쿠킹용 Opus 인코더도 나중에 같은 `Opus`를 링크한다.

#### 3.29.1 ThirdParty `Opus`·`OpusFile`

- libopus 1.6.1, opusfile 0.12(둘 다 BSD-3-Clause). 공식 릴리스 tarball이고 downloads.xiph.org의 SHA256SUMS.txt와 SHA-256이 같다.
- `Opus`: float build. `celt_sources.mk`·`silk_sources.mk`·`opus_sources.mk`의 기본·float 원본 137개와 그 헤더, `COPYING`. SIMD 원본·RTCD(런타임 CPU 탐지)·DNN(deep PLC·LACE)은 들이지 않는다.
  - define은 upstream CMake 기본을 따른다: `OPUS_BUILD`, `VAR_ARRAYS`(alloca 대신 C99 VLA), `HAVE_LRINTF`·`HAVE_LRINT`, `ENABLE_HARDENING`(release에서도 켜는 값싼 불변식 검사).
  - `OPUS_X86_MAY_HAVE_*`·`OPUS_HAVE_RTCD`가 없어 사칙연산 경로는 아키텍처·컴파일 설정마다 고정이다. 다만 float build(`FLOAT_APPROX` 없음)는 디코드에서 플랫폼 libm을 부른다(`denormalise_bands`의 `exp`, `exp_rotation`의 `cos`, 머리 gain의 `exp`). UCRT·Apple libm·glibc의 double ulp 차이가 float 반올림을 넘는 일은 드물지만 긴 원본에서는 0이 아니므로, 디코드 결과 고정값은 아키텍처·OS마다다(3.10.1절의 "기기마다 같다"는 같은 OS·아키텍처 안에서 성립한다). `FLOAT_APPROX`만으로는 `cos`가 남아 해결되지 않는다. Vorbis도 같은 성격이다.
  - `ENABLE_HARDENING`의 불변식 검사(`celt_assert`·`VALIDATE_CELT_DECODER` 등)가 실패하면 `celt_fatal`이 stderr에 쓰고 `abort()`한다. bitstream으로는 닿지 않게 설계된 불변식이고 upstream fuzzing도 이 설정으로 돈다(끄면 같은 결함이 메모리 손상이 된다). 결함이 있으면 임포트가 Log·`Result` 없이 프로세스를 끝낸다는 것이 한계다.
  - stack: `VAR_ARRAYS`와 multistream 버퍼로 디코드 한 번에 약 100 KB를 쓴다. 임포트 worker stack은 이보다 커야 한다.
  - 내부 include 경로(`celt`·`silk`·`silk/float`·`src`)는 `Opus.Build.cs`가 라이브러리 빌드에만 더한다. manifest `includeDirectories`는 `include`만 두어 `debug.h`·`errors.h`·`main.h` 같은 흔한 이름이 소비 모듈로 새지 않는다.
  - 수정 없음. 원본의 지시문(`#pragma intrinsic`, `#pragma GCC poison`, 최적화 안내 `#pragma message`)은 `pragmaDirectives`에 기록한다.
- `OpusFile`: `opusfile.h`와 디코드 원본(`info.c`·`internal.c`·`opusfile.c`·`stream.c`), `COPYING`. HTTP·인증서(`http.c`·`wincerts.c`)는 들이지 않고 `OP_DISABLE_HTTP`를 정의한다. `OpusFile.Build.cs`가 `Ogg`·`Opus` include 경로를 더한다.
  - 경고 0을 위한 수정(`OpusFile/Rocks.patch`):
    - `opusfile.c` 9곳·`stream.c` 3곳·`info.c` 2곳: clang이 요구하는 괄호를 더한다(`-Wshift-op-parentheses`·`-Wbitwise-op-parentheses`·`-Wlogical-op-parentheses`). C 우선순위와 결과는 그대로다. `stream.c`의 Windows 파일 이름 UTF-8 변환에서 넷째 byte를 `|` 대신 `&`로 합치는 upstream 코드(버그로 보인다)도 동작을 바꾸지 않는다(AudioTools는 메모리 stream만 연다).
    - `stream.c`: Rocks 빌드가 명령줄에서 `WIN32_LEAN_AND_MEAN`을 정의하므로 정의되지 않았을 때만 정의한다(`-Wmacro-redefined`).
    - `stream.c`: Microsoft CRT deprecated 호출을 바꾼다 — `_wfopen` → `_wfsopen(…, _SH_DENYNO)`(공유 같음), `fdopen` → Windows `_fdopen`, `_wfreopen` → `_wfreopen_s`(공유 없음). `internal.h`의 `#pragma warning(disable:4996)`이 clang-cl에서 이 경고를 가리고 있었다(지시문을 지운 사본 컴파일 시험 `Native.ThirdPartyDirectivesHideNoWarnings`로 확인).
  - 원본 지시문(`opusfile.h`의 visibility push/pop, `internal.h`의 GCC `-Wparentheses` 억제·MSVC 4554·4996)은 byte 그대로인 파일이라 `pragmaDirectives`에 기록한다. 괄호와 CRT 호출을 고쳤으므로 가리는 경고가 없다(GCC 억제는 `__GNUC__`가 있는 macOS clang에서만 켜진다. clang-cl은 `#pragma warning(disable:4996)`을 `-Wdeprecated-declarations`로 받아들인다).
- 소비 모듈은 `OpusFile`·`Opus`·`Ogg`를 이 순서로 링크한다.

#### 3.29.2 `DecodeOpus`

- page 구조를 먼저 확인한다(3.28.2절 `OggValidation`).
- 열기 전에 OpusHead를 본다: mapping family가 0·1이 아니거나(2·3 ambisonics, 255 discrete) 채널이 9개 이상이면 `Unsupported`다. opusfile은 이런 머리도 `OP_EBADHEADER`(머리 손상)로 돌려주기 때문이다. OpusHead 표지가 없으면 opusfile에 맡긴다.
- `op_open_memory`로 연다(메모리 stream이라 seekable). opusfile 오류는 `OP_EFAULT`(할당 실패) `OutOfMemory`, `OP_EIMPL`·`OP_EVERSION` `Unsupported`, 그 밖(`OP_ENOTFORMAT` 등) `InvalidArgument`로 옮긴다. 읽는 중 오류도 같다.
- link마다 채널 수가 같아야 한다(다르면 `Unsupported`). seekable 모드의 opusfile은 새 link의 BOS에서 serial을 다시 맞추고 link 시작에서 hole을 무시하므로, 정상 연결 stream에서 `OP_HOLE`이 나오지 않는다.
- 채널 순서: mapping family 0·1은 Vorbis 순서다. 디코드 직후 3.3절 내부 순서로 바꾼다(아래 "다채널 순서").
- 출력 rate는 언제나 48000 Hz다. 머리의 원본 입력 rate는 참고값일 뿐이라 쓰지 않는다(재표본화하지 않는다). OpusHead output gain은 opusfile 기본(`OP_HEADER_GAIN`)대로 적용한다(RFC 7845).
- 길이: `op_pcm_total(-1)`(granule 기준, pre-skip·끝 자르기 반영, link 합)을 열 때 읽어 디코드 전에 PCM 상한을 판정하고 교차 버퍼를 미리 잡는다(16M 표본까지, FLAC과 같다). 끝에 디코드한 frame 수가 이 값과 같아야 한다. opusfile은 TOC가 잘못된 packet(길이 0 이하)을 오류 없이 버리므로, 길이 대조가 그런 손상을 잡는다.
- `op_read_float`로 120 ms(5760 표본 × 채널)씩 읽어 쌓는다. 음수와 비유한 표본은 위 오류 대응을 따른다(`InvalidArgument`가 대부분).
- 결과: `sampleFormat = Lossy`, `sourceCodec = Opus`, `sampleRate = 48000`, loop·cue 없음. 실패하면 결과를 비운다.
- 한계: 메모리 정점은 Vorbis와 같다.

다채널 순서(Vorbis·Opus 공통, `DeinterleaveVorbisOrder`): Vorbis I 규격의 채널 순서를 3.3절 내부 순서(FL, FR, FC, LFE, SL, SR, BL, BR)로 바꾼다. `DecodedSourceAudio`에는 레이아웃 필드가 없고, 채널 수가 배치를 정한다.

| 채널 수 | Vorbis 순서 | 출력(내부 순서) |
|---|---|---|
| 1 | M | M |
| 2 | L, R | FL, FR |
| 3 | L, C, R | FL, FR, FC |
| 4 | FL, FR, RL, RR | FL, FR, SL, SR |
| 5 | FL, C, FR, RL, RR | FL, FR, FC, SL, SR |
| 6 | FL, C, FR, RL, RR, LFE | FL, FR, FC, LFE, SL, SR |
| 7 | FL, C, FR, SL, SR, RC, LFE | FL, FR, FC, LFE, SL, SR, BC |
| 8 | FL, C, FR, SL, SR, RL, RR, LFE | FL, FR, FC, LFE, SL, SR, BL, BR |

4–6채널의 rear 쌍은 5.1 내부 배치(앞 여섯)에 맞춰 SL·SR로 둔다. 7채널(6.1)의 BC는 `ChannelLayout`에 없어 맨 뒤에 둔다(임포터가 다룬다). WAV·AIFF·FLAC은 파일 순서(WAVE 채널 마스크 순서)를 그대로 내고, 내부 순서로 옮기는 일은 임포터가 채널 마스크로 한다.

#### 3.29.3 시험

입력은 `Tests/OpusTestFixtures.h`의 byte 배열이다(`<이름>PageOffsets`는 page 시작 위치). ffmpeg 9.1 libopus 인코더(`-fflags +bitexact -flags:a +bitexact`)로 lavfi sine·pink noise에서 만들었다. ffmpeg Ogg Opus는 OpusHead·OpusTags가 각각 page 하나이고 granule이 pre-skip 312를 포함한다(마지막 granule − 312 = 원본 길이로 확인).

| 대상 | 확인 |
|---|---|
| 길이·정렬 | stereo 48000 Hz 0.25 s: frame 수 12000(마지막 granule 12312 − pre-skip), 원본 sine과 정렬(SNR ≥ 15 dB), 오른쪽 = −왼쪽. 5.1(mapping family 1) 6채널 2400 frame |
| 채널 순서 | 채널마다 다른 sine(FL 300, FR 500, FC 700, LFE 60, BL 900, BR 1100 Hz)을 ffmpeg 5.1로 넣은 파일: 내부 순서 채널마다 제 주파수가 가장 강함(Goertzel). Vorbis도 같은 시험(`VorbisOrder51`) |
| 고정값 | stereo 디코드 결과 SHA-256 — x86-64 Windows(UCRT) 값(Debug·Release 같음). 그 밖은 두 번 디코드가 같은지만 본다(libm 차이). Vorbis도 같은 시험 |
| 연결 stream | 잡음 link 둘(72000 + 48000). 채널 수가 다른 link(stereo + mono) `Unsupported` |
| page 무결성 | 첫 link 가운데 audio page·EOS page·둘째 link BOS page 한 byte 손상, page 중간·경계 잘림: `InvalidArgument` |
| 지원 안 함 | OpusHead mapping family 2·255, 9채널(page CRC 다시 계산): `Unsupported`. 올바른 Ogg Vorbis: `InvalidArgument`(`OP_ENOTFORMAT`) |
| packet 손상 | audio page 본문 byte 3개를 바꾸고 page CRC를 다시 계산 200회: 멈춤·abort 없음, 받으면 길이 원본과 같고 표본 유한, 아니면 `InvalidArgument` |
| 거부 | Ogg 아님·빈 입력 `InvalidArgument`, OpusHead 표지 손상(CRC 그대로 — page 검사에서) `InvalidArgument`, 실패 시 비움 |
| 견고성 | 앞부분 29 byte 간격 전부 `InvalidArgument`(page 중간이면 page 검사, page 경계면 EOS 없음), byte 하나 무작위 교체 100회(값이 같으면 성공, 다르면 `InvalidArgument`) |

### 3.30 AU5 앞당김 — 코덱 계층 C1: 플랫폼 데이터·청크 표·인코더/디코더 인터페이스(개정 21, 22)

3.24절은 청크 표·플랫폼 데이터 헤더·`AudioDecoder`·`AudioDecoderTable`을 쿠커(PR11)와 함께 하기로 했다. 에셋 쿠커가 A4a·A4b까지 들어왔고 A4c(CookProcessor 표·키)가 다음이라, 쿠커와 맞물리는 부분(쿠킹 처리기·AssetNugget 키·sidecar 게시)만 남기고 형식과 코덱 계층을 앞당긴다. 이 절이 3.11.1절 인터페이스와 3.11.3절 머리 목록을 대체한다(개정 22 설계 교차 검증 반영). 단계:

| 단계 | 범위 |
|---|---|
| C1(이 절) | `Audio`: 플랫폼 데이터 형식·검증, `AudioDecoder`·`AudioDecoderTable`, PCM16·ADPCM 디코더, 전체 디코드 도우미. `AudioTools`: `AudioEncoder`, PCM16·ADPCM 인코더, 청크 나누기(pre-roll 복제)·플랫폼 데이터 쓰기 |
| C2 | 쿠킹 리샘플러: UE와 같이 libsamplerate `SRC_SINC_BEST_QUALITY`(UE `AudioPlatformConfiguration/Private/AudioResampler.cpp`, `AudioDerivedData.cpp:1597` `ResampleWaveData`) — ThirdParty 반입, 판을 너겟 키에 |
| C3 | Opus 인코더(`AudioTools`, multistream family 1)·Opus 디코더(`AudioOpus`, 아래 등록 방식). codec 머리 = mapping family 1, stream 수, coupled 수, mapping 표, output gain 0. pre-skip은 머리 encoder delay와 같아야 한다 |
| AU5 나머지 | 쿠킹 처리기·AssetNugget 키(A4c 뒤), sidecar 게시·청크 캐시(AU6) |

모듈 경계(빌드 세션 조언, BuildTargets BD25): Runtime 모듈은 ClientOnly 모듈에 링크로 의존할 수 없다. 그래서 `Audio`(Runtime)는 코덱 구현 모듈을 링크하지 않고 `AudioDecoderTable`로 조회만 한다(3.30.2절 등록 경로). `AudioOpus`는 BS2(모듈 유형)가 들어올 때까지 일반 Engine 모듈로 둔다. 그 뒤 유형만 ClientOnly로 바꾼다. `AudioTools`(Developer)가 Opus 왕복 시험에 `AudioOpus`를 링크해도 되는지(Developer → ClientOnly, BD25)는 C3 전에 BuildTargets 3.3절 표로 확인한다.

#### 3.30.1 플랫폼 데이터 형식(`Audio/AudioPlatformData.h`, little endian)

byte 배치는 `[메타 = 머리 + codec 머리 + 청크 표 + 정렬 0][payload 영역]`이다.

- 메타와 payload를 나눠 둘 수 있다(3.11.5절). sidecar = payload 영역 byte 그대로(위치 기준 불변), 패키지 = 메타 + 선택으로 payload 영역 앞부분 [0, 청크 0 끝). 위치·SHA가 나누기 전후로 같으므로 쿠킹 처리기는 byte를 다시 쓰지 않는다. Server 쿠킹은 메타만 둔다.
- 머리(`AudioPlatformDataHeaderBytes` = 80):

| 위치 | 크기 | 필드 |
|---|---|---|
| 0 | 4 | magic `RAPD` |
| 4 | 2 | 형식 판 `AudioPlatformDataVersion` = 1 |
| 6 | 1 | codec id(`AudioCodecId`: Pcm16 = 1, Adpcm = 2, Opus = 3 — 저장 값이라 번호를 바꾸지 않는다) |
| 7 | 1 | 채널 수(1–8) |
| 8 | 4 | bitstream 판(codec 단위 framing·bitstream 판. 디코더가 확인한다. PCM16 1, ADPCM 1, Opus 1). 인코더 판은 머리에 넣지 않고 너겟 키에만 넣는다 |
| 12 | 4 | 인코딩 rate(Hz) |
| 16 | 8 | 유효 frame 수 |
| 24 | 8 | loop 시작(유효 frame 기준) |
| 32 | 8 | loop 끝(포함하지 않음). 0이면 쿠킹된 loop 구간이 없고, looping 재생은 [0, 유효 frame)을 쓴다. looping 여부는 `SoundClip`·재생이 정한다 |
| 40 | 4 | encoder delay(codec frame) |
| 44 | 4 | 끝 padding(codec frame) |
| 48 | 4 | 청크 수 |
| 52 | 4 | codec 머리 byte 수(8의 배수) |
| 56 | 4 | 채널 마스크(아래) |
| 60 | 4 | 0 |
| 64 | 8 | payload 영역 byte 수 |
| 72 | 8 | 0 |

- 채널 마스크: bit 0 FL, 1 FR, 2 FC, 3 LFE, 4 SL, 5 SR, 6 BL, 7 BR, 8 BC. 켜진 bit 수 = 채널 수이고, data 채널 i는 i번째로 켜진 bit의 자리다(bit 순서 = 3.3절 내부 순서, BC는 끝). mono는 FC다. 같은 채널 수의 다른 배치(4채널 Quad FL FR SL SR와 3.1 FL FR FC LFE)를 가린다. 쿠커가 원본 채널 마스크·디코더 순서 표(3.29.2절)에서 정한다.
- 로딩 동작·첫 청크 상주는 머리에 두지 않는다. 재생 정책이라 바꿔도 다시 인코드할 필요가 없으므로 `SoundClip` 쿠킹 필드로 두고 너겟 키에서 뺀다(3.11.4절 고침). cue도 `SoundClip` 쿠킹 필드(유효 frame 기준 위치)다.
- codec frame 시간축: 디코더가 내는 frame은 codec 시간축이다. 유효 frame f = codec frame (f + encoder delay)다. codec frame 합 = delay + 유효 frame + padding이다.
- 청크 표(청크마다 `AudioChunkEntryBytes` = 64):

| 위치 | 크기 | 필드 |
|---|---|---|
| 0 | 8 | payload 위치(payload 영역 기준) |
| 8 | 4 | payload byte 수 |
| 12 | 4 | flags(bit 0 = 독립 디코드 시작 가능. C1은 모든 청크가 1이어야 한다. 다른 bit는 0) |
| 16 | 8 | 첫 codec frame |
| 24 | 4 | codec frame 수(pre-roll 제외) |
| 28 | 4 | pre-roll codec frame 수(payload 앞부분을 디코드해 버리는 양) |
| 32 | 32 | payload SHA-256 |

- 정렬: 메타 끝은 0으로 채워 16 byte 배수로 맞춘다(payload 영역 시작 정렬). 청크 payload 위치는 `AudioChunkAlignment` = 16의 배수이고 증가만 한다. 사이 빈틈은 0이다(writer가 채운다).
- 단위 framing: 청크 payload는 codec 단위(PCM16 frame 묶음, ADPCM 블록 묶음, Opus 패킷)의 연속이고, 단위 경계는 byte만으로 알 수 있어야 한다(codec이 정한다). PCM16은 `Pcm16UnitFrames`(= 1024) frame × 채널 × 2 byte(청크 마지막 단위만 짧다), ADPCM은 채널 × 512 byte, Opus는 `[u16 LE 길이][multistream 패킷]`이다.
- 검증(`ParseAudioPlatformData(bytes)`: 읽기 전용 view. bytes = 메타 + payload 영역 앞부분(없어도 된다)):
  - magic·형식 판, 0이어야 할 칸(머리 60·72, 메타 정렬 채움)이 다르면 `InvalidArgument`.
  - codec id 0은 `InvalidArgument`, 표 범위(`MaxAudioCodecs` = 8) 이상은 이 판이 모르는 codec이라 `Unsupported`다. 범위 안의 id는 형식 규칙만 보고(rate 규칙은 아는 codec에만), 디코더 유무는 표가 정한다(`NotFound`). 그래서 시험 전용 codec(범위 안의 빈 id)도 형식 시험에 쓸 수 있다.
  - 채널 1–8, 채널 마스크의 켜진 bit 수 = 채널 수, bit 9 이상 0. rate: PCM16·ADPCM은 [8000, 192000], Opus는 48000.
  - 유효 frame > 0. delay + 유효 + padding이 넘치지 않고 청크 frame 수 합과 같다.
  - loop: 끝이 0이면 시작도 0, 아니면 시작 < 끝 ≤ 유효 frame.
  - codec 머리 byte 수는 8의 배수. 청크 수 ≥ 1, 첫 codec frame이 0부터 빈틈없이 이어진다. codec frame 수 > 0. pre-roll ≤ 첫 codec frame(청크 0은 0). flags = 1.
  - 청크 0의 위치 = 0, 위치는 16 배수이고 앞 청크 끝 이상, payload 구간이 payload 영역 안.
  - 메타 byte 수 = 정렬(80 + codec 머리 + 64 × 청크 수)와, 주어진 bytes ≥ 메타. 메타 뒤 byte 수(있는 payload)는 payload 영역 이하다. view는 어떤 청크의 payload가 있는지 알려 준다(`ChunkPayload(index)` — 없으면 빈 Span).
  - 덧셈·곱셈 넘침은 모두 검사한다(청크 수 × 64 등).
  - codec별 검사(codec 머리 크기, PCM16·ADPCM은 pre-roll 0·codec 머리 0·청크 byte와 frame 수 관계)는 디코더 `Open`·`BeginChunk`가 한다.
- 청크 SHA 확인 `VerifyAudioChunk(chunk, payload)`: 다르면 `InvalidArgument`. 청크 캐시가 sidecar에서 읽을 때와 도구(전체 디코드 도우미)가 부른다. 패키지 안 메타·상주 청크는 패키지 SHA(manifest)가 덮으므로 재생 때마다 부르지 않는다.

#### 3.30.2 디코더(`Audio/AudioDecoder.h`)

3.11.1절의 `Decode(chunk, out)`·`SeekToChunk`는 청크를 한 번에 풀거나 청크 머리에서만 시작해 voice PCM 링(3.12.3절)·청크 가운데 시작(`PreparePlayback(startFrame)`, 청크 가운데의 loop 시작, loop 머리 PCM 뒤를 잇는 위치)에 맞지 않는다. 그래서 청크를 열고 단위씩 내는 모양으로 고친다:

```cpp
class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;
    // 머리를 받아 codec 머리를 확인하고 상태·scratch를 모두 잡는다(이 뒤 BeginChunk·DecodeNext는 할당하지 않는다).
    virtual Result<void> Open(const AudioPlatformDataHeader& header, Span<const uint8> codecHeader) noexcept = 0;
    // 청크를 startCodecFrame(∈ [첫 codec frame, 끝))부터 디코드할 준비를 한다. 상태를 비우고, payload 단위 경계와 크기를 먼저
    // 검사하고, pre-roll과 startCodecFrame 앞 frame을 디코드해 버린다(내지 않는다). payload는 청크를 다 낼 때까지 살아 있어야 한다.
    virtual Result<void> BeginChunk(const AudioChunkEntry& chunk, Span<const uint8> payload, uint64 startCodecFrame) noexcept = 0;
    // 다음 단위를 planar로 쓴다(시작 단위는 startCodecFrame부터의 나머지). 쓴 codec frame 수, 청크 끝이면 0.
    virtual Result<uint32> DecodeNext(PlanarView out) noexcept = 0;
    // DecodeNext 한 번이 쓸 수 있는 최대 frame 수(out 용량 하한). PCM16 1024, ADPCM 1012, Opus 5760.
    virtual uint32 MaxFramesPerCall() const noexcept = 0;
};
```

- 청크 경계 규칙: 순차 재생도 청크마다 상태를 비우고 pre-roll부터 디코드한다(앞 청크 상태를 이어 쓰지 않는다). 그래서 처음부터 재생·seek 시작·전체 디코드 도우미가 같은 표본을 낸다. Opus는 80 ms pre-roll 뒤에도 연속 디코드와 bit가 같지 않으므로, 기준은 "청크별 디코드"다.
- 청크 가운데 시작: PCM16·ADPCM은 단위로 바로 건너뛴다. Opus는 길이 접두를 훑어 (startCodecFrame − pre-roll 요구량) 이전의 패킷부터 디코드한다. 그 지점이 청크 머리 앞이면 복제 pre-roll 구간부터 쓴다.
- `out` 채널 수가 머리 채널 수와 다르거나 frame 용량이 `MaxFramesPerCall()`보다 작으면 `InvalidArgument`, `BeginChunk` 전·`Open` 전 호출은 `InvalidArgument`다.
- 손상 판정: 단위 경계·크기(PCM16·ADPCM byte 수, Opus 길이 접두 합)는 `BeginChunk`에서 먼저 검사한다(이미 낸 PCM이 voice에 나간 뒤 실패하지 않게). `DecodeNext`의 실패는 실제 디코드 오류뿐이다.
- `AudioDecoderTable`: codec id(1–`MaxAudioCodecs` − 1, `MaxAudioCodecs` = 8)마다 factory(`Result<UniquePtr<AudioDecoder>> (*)(Allocator&)`) 하나다. `Register`(이미 있으면 `AlreadyExists`, 범위 밖 `InvalidArgument`), `Create(codec, allocator)`(없으면 `NotFound`, factory 실패는 그 오류). 전역 표·전역 생성자는 없다.
- 등록 경로(backend와 같은 방식, AD24):
  - 내장(PCM16·ADPCM): `RegisterBuiltInAudioDecoders(table)`.
  - 코덱 모듈(`AudioOpus`): 모듈 `Startup`에서 `HostServices`에 디코더 factory 서비스를 등록한다. 표를 만드는 쪽(AU3 `AudioEngineSubsystem`, 에디터 미리듣기·AD25 변환 도구, 시험)은 내장을 넣은 뒤 `HostServices`에서 찾은 factory를 넣는다.
  - Modular 빌드에서 factory·디코더 vtable이 `AudioOpus` 안에 있으므로 `AudioOpus`는 `SupportsUnload` = false다(디코더가 남은 채 내려가지 않게).
- 수명: 디코더는 제어 쪽이 만들고 디코드 worker가 쓰며(3.12.3절), voice 회수 ACK 뒤 제어 쪽이 파괴한다. AudioRT는 디코더를 만지지 않는다.
- 전체 디코드 도우미 `DecodeAudioPlatformData(view, table, allocator, DecodedAudioPcm& out)`: payload가 모두 있어야 한다(없으면 `InvalidArgument`). 할당 전에 모든 청크의 SHA 확인과 `BeginChunk`(단위 경계·크기)를 먼저 돌려 손상된 frame 수로 큰 버퍼를 잡지 않는다. 그 뒤 청크마다 `BeginChunk(첫 frame)` → `DecodeNext` 반복, delay·padding을 잘라 유효 frame만 planar로 남긴다(`DecodedAudioPcm` = 채널 수·frame 수·planar float `Array`. `PlanarBuffer`는 block 크기 상한이라 쓰지 않는다). 에디터 미리듣기·시험·도구용이다.
- 내장 디코더 `Open`은 codec·채널 1–8·rate [8000, 192000]·codec 머리 없음을 보고(검증하지 않은 머리로 열어도 `PlanarView` 범위 밖을 쓰지 않게), 실패하면 앞 상태(열린 청크)를 비운다. bitstream 판이 다르면 `Unsupported`다.
- PCM16 디코더: 단위 = 1024 frame 교차 int16 LE(마지막 단위는 청크 frame 수로 안다). 값 × `Pcm16SampleScale`(1/32768).
- ADPCM 디코더: 단위 = 블록 묶음(채널 × 512 byte, 3.24.1절). 마지막 청크의 마지막 묶음만 1012보다 적은 frame을 담는다(디코더는 청크 frame 수로 묶음 수를 정하므로 중간 청크의 짧은 묶음도 받는다 — 청크마다 독립이라 해가 없다). `DecodeAdpcmFrames`를 묶음 단위로 부른다.

#### 3.30.3 인코더와 청크 나누기(`AudioTools/AudioEncoder.h`·`AudioPlatformDataWriter.h`)

```cpp
struct EncodedAudio {           // 인코더 출력: codec 단위 목록
    AudioCodecId codec; uint32 bitstreamVersion; uint32 encoderVersion;   // encoderVersion은 너겟 키용(머리에 넣지 않는다)
    uint32 channels; uint32 channelMask; uint32 sampleRate;
    uint64 frames; uint32 encoderDelay; uint32 endPadding;
    uint32 preRollFrames;       // 청크 앞에 둘 최소 pre-roll codec frame(Opus 80 ms = 3840, 그 밖 0)
    Array<uint8> codecHeader; Array<uint8> payload; Array<EncodedAudioUnit> units;   // 단위 = {payload 위치, byte 수, codec frame 수}, 단위 byte는 3.30.1절 framing
};
class AudioEncoder {
public:
    virtual ~AudioEncoder() = default;
    virtual const AudioCodecInfo& Info() const noexcept = 0;   // codec id, 이름, 인코더 판, bitstream 판, rate 범위, 최대 채널
    virtual Result<void> Encode(PlanarConstView pcm, uint32 sampleRate, uint32 channelMask, const AudioEncodeSettings& settings,
                                EncodedAudio& out) noexcept = 0;
};
Result<void> WriteAudioPlatformData(const EncodedAudio& encoded, const AudioPlatformDataOptions& options, Array<uint8>& out) noexcept;
```

- `AudioEncodeSettings`: 품질(codec별 뜻 — Opus, C3). PCM16·ADPCM은 쓰지 않는다. 입력에 비유한 표본이 있으면 `InvalidArgument`다.
- `AudioPlatformDataOptions`: 목표 청크 byte(`TargetChunkBytes` = 256 KiB 기본), 첫 청크 목표 byte(기본 = 목표 — UE `ZerothChunkSize`·`MaxLengthOfFirstAudioChunkInFrames`, `AudioDerivedData.cpp:800–864`에 해당, 상주 첫 청크를 작게), loop 구간(유효 frame 기준).
- 청크 나누기:
  - 단위를 차례로 넣다가 청크 자기 단위 byte가 목표를 넘게 되면 새 청크를 연다(한 청크에 단위 하나는 늘 들어간다 — 단위 하나가 목표보다 크면 그 청크만 목표를 넘는다).
  - 청크 k > 0은 스트림 순서로 바로 앞 단위들을 거꾸로 모아 frame 합이 `preRollFrames` 이상이 될 때까지 복제해 payload 앞에 둔다(청크 경계와 상관없이 앞 청크들을 넘어 모으되 codec frame 0 앞으로는 넘어가지 않는다). 그 frame 합이 청크 pre-roll이다. 복제분은 목표 크기 계산에 넣지 않는다.
  - payload 위치는 청크 순서대로 16 byte 정렬로 이어 붙인다(빈틈 0). SHA-256은 청크 payload(복제분 포함) 전체다.
- writer는 아는 codec의 불변식도 본다: PCM16·ADPCM에 pre-roll이나 codec 머리가 있으면 `InvalidArgument`. 청크 하나가 4 GiB(청크 표 32비트 칸)를 넘으면 `CapacityExceeded`다.
- PCM16 인코더: 단위 = 1024 frame 교차 int16(마지막은 남은 frame). 양자화는 `QuantizePcm16`(×32768, round-half-even, 자르기 — 3.24.2절 `AdpcmQuantize`를 이 이름으로 바꾸고 ADPCM도 쓴다).
- ADPCM 인코더: `EncodeAdpcm`(3.24.2절) 결과를 블록 묶음 단위로 나눈다. 마지막 단위의 codec frame 수는 남은 frame이다(padding 0).
- 결정성: 같은 입력·설정은 같은 byte다(시험, golden 해시).

#### 3.30.4 시험

| 대상 | 확인 |
|---|---|
| 형식 왕복 | PCM16·ADPCM: 쓰기 → 검증 → 전체 디코드. PCM16은 양자화 결과와 표본 일치, ADPCM은 `DecodeAdpcmFrames` 결과와 일치. 같은 입력 두 번 같은 byte, 고정 입력의 golden SHA-256(형식이 우연히 바뀌면 실패) |
| 청크 | 작은 목표 크기로 여러 청크: 경계가 단위 경계, 단위 하나가 목표보다 큰 경우, 첫 청크 목표 따로, 16 정렬·빈틈 0, 첫 codec frame 연속, 청크 하나씩 따로 디코드해 이어 붙인 결과 = 전체 디코드 |
| 청크 가운데 시작 | 모든 codec·청크에서 여러 startCodecFrame: 낸 표본 = 전체 디코드의 같은 구간 |
| pre-roll | 시험 전용 codec(상태 기억이 정확히 N frame인 단위, pre-roll 요구 N)을 표에 등록: 청크 k만으로·청크 가운데서 디코드한 결과가 기준과 같음, pre-roll이 앞 청크 여럿을 넘어 모임(작은 목표), codec frame 0 앞으로 넘지 않음, 복제 byte가 SHA에 들어감, delay·padding이 있는 경우 전체 디코드가 앞뒤를 정확히 자름 |
| 메타·payload 분리 | 메타만(Server)·메타 + 청크 0(패키지)·전체: 검증 통과, 있는 청크만 payload가 보임, 전체 디코드는 payload가 다 있어야 함 |
| 검증 음성 | magic·판·0 칸·codec id(모름 `Unsupported`)·채널 0·9·마스크(bit 수 불일치, bit 9 이상)·rate 범위·유효 frame 0·delay+padding 불일치·loop(시작 ≥ 끝, 끝 > 유효, 끝 0인데 시작 ≠ 0)·codec 머리 8 배수 아님·청크 0개·frame 빈틈·pre-roll > 첫 frame·청크 0 pre-roll·flags·청크 0 위치 ≠ 0·정렬·위치 감소·payload 범위 밖·메타 크기·덧셈 넘침(청크 수 최대값, 첫 frame 넘침): 칸마다 하나씩 |
| 청크 손상 | SHA 불일치 `InvalidArgument`, PCM16·ADPCM byte 수 불일치(`BeginChunk`에서), PCM16·ADPCM의 pre-roll·codec 머리 ≠ 0 |
| 견고성 | 올바른 blob의 byte 무작위 교체·앞부분 자르기: 멈추거나 범위 밖을 읽지 않음(검증·디코드 어디서든 오류 또는 성공) |
| 표 | 등록 중복 `AlreadyExists`, 범위 밖 `InvalidArgument`, 없는 codec `NotFound`, factory 실패 전달, 내장 등록 둘 |
| 디코더 계약 | out 채널 수 불일치·용량 부족 `InvalidArgument`, `Open`·`BeginChunk` 전 호출, 청크 끝 뒤 0, startCodecFrame 범위 밖 |
| 인코더 | 비유한 입력 `InvalidArgument`, 채널 0·9, 채널 마스크 bit 수 불일치 |

### 3.31 AU5 앞당김 — 코덱 계층 C2: 쿠킹 리샘플러(개정 23, 24)

Opus 플랫폼 데이터는 48 kHz만 쓰므로(3.11.2절) 다른 rate 원본은 쿠킹 때 바꾼다. 런타임 리샘플러(3.20절 선형)와 달리 band-limited 고품질 필터를 쓴다.

#### 3.31.1 ThirdParty `SampleRate`(`Engine/Source/ThirdParty/SampleRate`)

- UE와 같은 라이브러리다: UE `ResampleWaveData`(`Runtime/Engine/Private/AudioDerivedData.cpp:1597`)는 `Audio::Resample`(`Runtime/AudioPlatformConfiguration/Private/AudioResampler.cpp`)로 libsamplerate `SRC_SINC_BEST_QUALITY`를 부른다.
- libsamplerate 0.2.2(BSD-2-Clause). GitHub 릴리스 tarball이고, 릴리스의 분리 서명(`.sig`)이 libsndfile 관리자 키(David Seifert, 주 키 31D9 5CAB 6D80 D262 244A 1750 A476 20E8 01E4 7E95)로 확인된다.
- 반입: `samplerate.h`, `src/` 원본(`samplerate.c`·`src_linear.c`·`src_sinc.c`·`src_zoh.c`)과 헤더(`common.h`·계수 표 셋), `COPYING`. 시험·예제·Octave 도구는 들이지 않는다.
- `config.h` 없이 define으로 빌드한다: 세 sinc 변환기 켬, `HAVE_STDINT_H`·`HAVE_STDBOOL_H`·`HAVE_LRINT`·`HAVE_LRINTF`, little endian, `CPU_CLIPS_*` = 0. `PACKAGE`·`VERSION` 문자열은 glue `SampleRateVersion.c`가 정의하고 `samplerate.c`를 컴파일한다(FLAC `FlacFormat.c`와 같은 방식).
- 수정 없음. 경고 0(clang C17 `-Wall -Wextra -Wpedantic`, clang-cl `/W4`)이다. 계수 표 헤더(`high_qual_coeffs.h`·`mid_qual_coeffs.h`)의 MSVC `#pragma warning(push/disable: 4305/pop)`은 byte 그대로인 원본 지시문이라 `pragmaDirectives`에 기록한다(지시문을 지운 사본도 경고 0 — `Native.ThirdPartyDirectivesHideNoWarnings`).
- 결정성: sinc 경로는 계수 표와 사칙연산·`lrint`만 쓰고 libm 초월 함수를 부르지 않는다. 그래서 같은 입력이면 OS와 상관없이 같은 아키텍처에서 같은 byte다(`-ffp-contract=off`, 최적화가 연산 순서를 바꾸지 않는다). 전제: 부동소수점 반올림 모드가 기본(가장 가까운 값)이고 FTZ·DAZ가 꺼져 있다. 쿠킹은 AudioRT(FTZ를 켜는 실행 함수, 3.22절) 밖의 도구 스레드에서 돈다.
- `AudioTools`만 Private 의존으로 링크한다. 내부 할당은 malloc이다(도구 전용).

#### 3.31.2 `ResampleForCooking`(`AudioTools/CookResampler.h`)

```cpp
inline constexpr uint32 CookResamplerVersion = 1;   // libsamplerate 0.2.2 SRC_SINC_BEST_QUALITY — 너겟 키에 넣는다
Result<uint64> CookResampledFrames(uint64 frames, uint32 sourceRate, uint32 targetRate) noexcept;      // round-half-even(frames × target / source), 0이면 1
Result<uint64> CookResampledPosition(uint64 position, uint32 sourceRate, uint32 targetRate) noexcept;  // round-half-even(position × target / source)
Result<void> ResampleForCooking(const PlanarConstView& in, uint32 sourceRate, uint32 targetRate, Allocator& allocator, DecodedAudioPcm& out) noexcept;
```

사용 예:

```cpp
DecodedAudioPcm resampled(allocator);
if (auto result = ResampleForCooking(source.View(), 44100, 48000, allocator, resampled); !result) { return result; }
auto loopStart = CookResampledPosition(sourceLoopStart, 44100, 48000);   // Result<uint64>
```

- 출력 frame 수 = `CookResampledFrames`(가까운 정수, 동률은 짝수 — loop·cue 위치 변환과 같은 규칙, 3.11.2절). 0이 되면 1이다.
- 위치·길이 변환은 rate가 [8000, 192000] 밖이거나 값이 2^32를 넘으면 `InvalidArgument`다(× rate가 64비트 안에 든다). `PlanarConstView::frameCount`가 `uint32`이므로 리샘플 입력은 이 범위 안이다.
- 끝 frame: libsamplerate는 `end_of_input` 뒤 대략 floor(N × 비율) 근처에서 멈추고 그 경계가 채널 수에 따라 다르다. 그래서 입력 뒤에 0을 ceil(source / target) + 1 frame 명시적으로 붙여 넣는다. 원본 끝 뒤를 무음으로 보는 것이고, 출력 수보다 많이 나온 frame은 자른다. 모자라면(libsamplerate 동작이 바뀐 것) `Unsupported`다. 뒤를 0으로 채우는 방식은 버렸다 — 끝 frame이 원본과 상관없이 0이 된다.
- 블록 처리: 입력을 4096 frame씩 교차 float 블록으로 만들어 `src_process`에 넣고(`src_new`/`src_delete`는 RAII), 출력 블록(4096 × 24 + 16 frame)을 바로 planar 결과로 옮긴다. libsamplerate의 frame·표본 수 칸(`long`·`int`, Windows 32비트)이 채널 수·길이와 상관없이 넘치지 않고, 원본 전체 교차 사본을 만들지 않는다. 진행이 없으면(입력도 출력도 0) `Unsupported`다.
- 필터 지연은 libsamplerate가 맞춘다(출력 frame 0 = 원본 시각 0).
- 같은 rate면 필터를 거치지 않고 그대로 복사한다.
- loop 경계: 리샘플은 원본 전체를 한 번에 하고 loop 구간을 따로 감싸지 않는다. loop 끝 근처 표본은 loop 시작이 아니라 뒤 원본(끝이면 무음)을 보고 필터된다. UE `ResampleWaveData`도 같다. 경계 맞춤이 필요해지면 쿠킹 처리기(AU5 나머지)가 정한다.
- 거부와 오류: 채널 9 이상이거나 `in`이 `out`의 기존 버퍼와 겹치면 `InvalidArgument`이고 `out`은 그대로 둔다(비우면 입력이 사라진다). 그 밖에는 `out`을 먼저 비우고, rate [8000, 192000] 밖·채널 0·frame 0·비유한 표본은 `InvalidArgument`, 결과 PCM이 `MaxSoundClipPcmBytes`(2 GiB, 원본 디코드 상한)를 넘으면 할당 전에 `CapacityExceeded`, 할당 실패는 `OutOfMemory`, libsamplerate 오류는 `Unsupported`(할당 실패 `SRC_ERR_MALLOC_FAILED`만 `OutOfMemory`)다. 실패하면 `out`을 비운다.
- 결과는 `DecodedAudioPcm`(채널 수·rate·frame 수·planar float, 3.30.2절)을 그대로 쓴다.
- loop 길이가 0이 되면 1 frame으로 둔다(3.11.2절)는 쿠킹 처리기(AU5 나머지)가 맡는다.

#### 3.31.3 시험

| 대상 | 확인 |
|---|---|
| 길이 | 8000·22050·32000·44100·96000·192000 → 48000과 48000 → 44100: 출력 frame 수 = round-half-even, 동률 경계(짝수로), 같은 rate 복사(byte 같음), 1 frame 입력(크게 내릴 때도 1 frame, 0 아님) |
| 정렬·품질 | 1 kHz·2 kHz sine(채널별): 해석적 sine과 비교해 가장자리(출력 기준 256 × ceil(target / source) frame)를 뺀 구간 SNR ≥ 90 dB, 위상 정렬(출력 frame 0 = 시각 0), 4096 frame 블록 여러 개 |
| 대역 제한 | 96000 → 48000에 30 kHz sine: 출력 에너지가 입력의 −80 dB 아래(앨리어싱 없음) |
| 끝 frame | DC 입력(1·2·441·4095·4096·4097·10000 frame, 8000·44100·192000 → 48000, 채널 1–8)의 결과 = 뒤에 무음 2048 frame을 붙인 입력 결과의 앞부분(허용 오차 10^−6), 끝 frame 0 아님 |
| 채널 경로 | libsamplerate 채널 수별 경로(1·2·4·6·그 밖): 같은 신호 2–8채널 결과 = 1채널 결과(허용 오차 10^−6) |
| 거부 | rate 범위 밖·채널 0·채널 9·frame 0·비유한 표본 `InvalidArgument`(실패 뒤 out 빔), 겹침 `InvalidArgument`(out 그대로), 8000 → 192000 8채널 2,800,000 frame `CapacityExceeded`(할당 전) |
| 결정성 | 같은 입력 두 번 같은 byte, golden SHA(정수로 만든 입력) — x86-64 값을 단언하고 ARM64는 두 번 같은지만 본다(사칙연산뿐이라 같을 것으로 보지만 macOS에서 잰 뒤 넣는다) |
| 위치 변환 | `CookResampledPosition`·`CookResampledFrames` 경계값(0, 2^32, 동률)과 거부(rate 범위 밖, 2^32 초과) |

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
| AD26 | 장치 콜백 → AudioRT 깨우기는 잠금 없는 단일 대기자 신호(`RealtimeWakeSignal`: Windows `WaitOnAddress`, Android futex, macOS Mach semaphore) | 권장안 |
| AD27 | 리버브 bypass·없음으로 전환해도 꼬리 에너지가 임계 아래로 머문 뒤에 끄고 이력을 지운다(UE는 즉시 지움) | 권장안 |
| AD28 | Opus 청크는 앞 청크 끝 80 ms 패킷을 복제해 독립 디코드, looping clip은 loop 머리 PCM을 미리 디코드해 둔다 | 권장안 |

## 6. 단계

### 6.1 선행작업

| ID | 선행작업 | 상태(2026-11-22) | 할 일 | 막는 단계 |
|---|---|---|---|---|
| PR1 | 이 스펙 | 개정 1 | 교차 검증 반영 | AU1 이상 |
| PR2 | 스레드 우선순위 | 구현(`Highest`) | 없음. 실시간 등급은 AU9 측정 뒤 PlatformLayer 개정 | — |
| PR3 | Core 동시 컨테이너 SY7(`BoundedSpscQueue`·`FixedBlockPool`·`IndexedFreeList`) | 설계(선행 SY0·SY1·SY2) | SY0→SY1→SY2→SY7 구현 | AU2(개정 8: 믹서가 명령을 인자로 받아 AU1에서 옮김 — 3.22.1절) |
| PR21 | 무잠금 깨우기 신호 `RealtimeWakeSignal`(AD26) | 설계(Synchronization 개정 3 9.6절, D34, SY12) | SY0 → SY12 구현 | AU2 |
| PR4 | `SerialTaskChain`·`WorkerScheduler`·`TaskLane::Audio` | 구현 | 없음 | — |
| PR5 | ThirdParty 반입 | 절차 구현, 오디오 라이브러리 없음 | miniaudio(AU2), libFLAC·libvorbis·libogg·libopus·libopusfile·minimp3(AU4), 런타임 libopus(AU5) | AU2, AU4, AU5 |
| PR6 | 모듈 유형 `ClientOnly`, Server 대상 오디오 제외 | 설계 | BuildTargets 구현 | AU2(서버 빌드 확인은 AU3) |
| PR7 | `RunTraits::audioOutput`·`audioMuted` | 설계(AppInfo 개정 4, 10.2절) | AppInfo AP1 구현 또는 AU3 임시 판정 | AU3 |
| PR8 | Engine 종료 재시도 게이트 | 설계(EngineRuntime 6절, CrashHandlingAndLaunch 개정 13 20절) | 구현(Engine `Shutdown`의 `Busy` 처리·호스트 재시도) | AU3 |
| PR9 | 앱 수명 이벤트(활성·비활성·백그라운드·오디오 포커스) | 데스크톱 창 포커스는 구현(PlatformWindow), 앱 수준 이벤트는 설계(PlatformLayer "앱 수명 이벤트" 절) | Windows·macOS 활성 이벤트는 AU3과, Android는 AU9와 함께 구현 | AU9 |
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
| AU1 | `Audio` 모듈: 형식·채널 레이아웃·PCM 버퍼, voice pool, 명령(인자 전달)·파라미터 seqlock·stop mailbox·장치 stop-all epoch·ACK·시작/terminal 슬롯(Mixer2, scene epoch는 AU3), mix clock·블록 안 예약, 리샘플러, source LPF/HPF, gain ramp, 2D·equal-power 패너, 거리 곡선, 마스터 limiter, `OfflineAudioRender`, 메모리 PCM clip | PR1 | 3.19절 PCM·예약·리샘플·click·수명·seqlock 시험(Inline), AudioRT 할당 0 |
| AU2 | `AudioOutputBackend`, AudioRT 스레드·명령 링(`BoundedSpscQueue`)·정렬 pending 배열(3.22.1절)·출력 링(칸 수 계산)·`RealtimeWakeSignal`, `NullAudioOutput`, ThirdParty `Miniaudio`, `AudioMiniaudio`(Windows 먼저)와 `HostServices` 등록 | AU1, PR3, PR5, PR6, PR21 | Sandbox 시험음 재생, 협상값·실제 스레드 우선순위·underrun 기록, Q ≠ 콜백 크기에서 underrun 0, 장치 없음·열기 실패·종료 자원 반환. macOS·Android는 실행한 것만 판정 |
| AU3 | `EngineAudio`: `AudioEngineSubsystem`·`AudioWorldSubsystem`·제어 pipe·`AudioDeviceContext`, `SoundSource`·`SoundClip`(메모리 PCM)·`AttenuationSettings`(거리), `AudioComponent`, 재생 API·결과, 리스너, CVar(비활성 창 볼륨 포함), 무음 실행·서버, 종료 drain, Tryout 정리 계약 | AU2, PR7, PR8 | Sandbox에서 키 입력으로 UI음·움직이는 부착 루프, 좌우 방향, 맵 교체·컴포넌트 파괴·GC 뒤 stale voice·lease 0, 서버 빌드에 `AudioMiniaudio` 미링크 |
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
| 렌더 깨우기 | `FEvent` | 잠금 없는 `RealtimeWakeSignal` | 장치 콜백과 AudioRT의 우선순위 역전 방지 |
| 리버브 bypass 전환 끝 | early·late 이력 즉시 지움(꼬리 잘림) | 꼬리 에너지가 임계 아래로 머문 뒤 지움 | 꼬리 보존 |
| 블록 안 offset | Quartz 예약만 | 모든 시작·정지 | 결정적 시각과 시험 |
| 리버브 보간 진행률 | 벽시계(`FApp::GetCurrentTime`), 게임 틱 단위 | mix clock frame, 블록 단위 | 오프라인 렌더 결정성 |
| Android | OpenSL ES만 | AAudio 우선(miniaudio), 없으면 OpenSL ES | AAudio가 현재 권장 API |
| 실행 판정 | `FApp::CanEverRenderAudio` 정적 지역 변수 | `RunTraits` 값 | AppInfo 설계(명령줄 고정 시점 명시) |
| 오류 | `check`·로그 | `Result`, 원래 OS 오류 코드 보존 | Rocks 원칙 |
| 렌더 명령 경로·슬롯 재사용 | 오디오 스레드 → 렌더 스레드 이중 버퍼(잠금 교체)·MPSC 큐, source id는 FIFO 순서에 기대 바로 재사용 | 정렬 pending 배열 + 잠금 없는 seqlock·mailbox·epoch, generation·ACK로 재사용(3.23절) | 예약 명령 정렬이 FIFO를 깨므로, 대기 금지 |
| ADPCM 블록 배치·계수 | 채널마다 블록을 끝까지 이어 붙임, 인코더 계수 색인 0 고정 | 블록 묶음마다 채널 교차, 블록마다 7개 계수 중 오차 최소 | 청크 독립 디코드(3.11.3절), 음질 — 디코더 형식은 같다 |
| 파라미터 ramp | 블록 단위 보간(블록 크기에 따라 길이가 다름) | 명령의 `rampFrames`(기본 256), 채널 이득도 frame 단위 ramp | Q와 무관한 같은 출력(3.22.4절) |
| 마스터 출력 | dynamics 없음, 출력 clamp ±1 | limiter(−1 dBFS, 지연 0) + clamp ±1 | 3.6절 limiter, AU2 int16 변환 |
| 렌더 FP 환경 | 기존 설정 + FTZ(`FScopedFTZFloatMode`) | 결정적 기준(nearest-even·trap 끔) + FTZ, 실행 함수 방식 | compiler 재배치 방지, 결정성 |
| dynamics 계수·지연 계산 | float(`FMath::Exp`, `CeilToInt(ms·fs/1000.0f)`) | double 계산 뒤 float(계수 ulp 차이, 정수 경계에서 지연 1 frame 차이 가능 — 예: 0.020833334 ms에서 UE 1, Rocks 2) | 결정적 기준값 대조 |
| dynamics 비유한 key | 거르지 않음(엔벌로프가 NaN에 갇힘) | 0으로 봄, ratio < 1e-8·gain ±96 dB 밖 거부 | 마스터 limiter 무력화 방지 |
| 런타임 리샘플러 위치 | 16.16 고정소수점(`RuntimeResampler.cpp:17`) | 32.32 고정소수점(3.20.5절) | 비율·위치 정밀도(16.16은 비율 오차 2^-17), 블록 분할과 무관한 bit 동일 출력(결정적 시험) |
| 방위각 경계 | 정왼쪽(x = 0·y < 0) 90°, 정후면(x < 0·y = 0) 0°, 수직(수평 성분 0) 90°(경계 결함) | 270°·180°·0°(atan2와 같음, 수직은 정면) | 결함 수정, 다른 방향은 같음 |
| 5.1 서라운드 쌍 | 장치 마스크(Windows 기본 BL·BR 210°·150°) | SL·SR 250°·110° 고정, 장치 마스크 대응은 AU9 | ITU-R BS.775 배치, 내부 순서 고정(3.3절) |
| HPF 계수 | `FastTan` 근사 | `PlatformMath::Tan` | 정확한 기준값 대조 |
| 다채널 분리 임포트 | `_L`·`_R` 파일 묶음 임포트 | 받지 않음 | 수요 생기면 |

## 8. 다른 문서 개정

개정 2(2026-11-22)에서 아래를 반영했다. 남은 것은 표의 "남음" 칸이다.

| 문서 | 고친 것 | 남음 |
|---|---|---|
| [엔진 구현 계획](EngineImplementationPlan.ko.md) 개정 1.17 | 18절: 전용 AudioRT 스레드 + 출력 링(AD2), 이 문서 링크, 표지 1.17 항목 | — |
| [동기화](Synchronization.ko.md) 개정 3 | 9.6절 `RealtimeWakeSignal`, D34, 단계 SY12(AD26). D27(AudioRT SPSC 입력)은 그대로 | — |
| [AppInfo·명령줄](AppInfoCommandLine.ko.md) 개정 4 | `RunTraits::audioOutput`·`audioMuted`, `--no-sound`·`--mute-audio`·`--allow-commandlet-audio`, 8.6절 볼륨 CVar | — |
| [엔진 런타임](EngineRuntime/06-Engine.ko.md) 6절 | 종료 게이트(`Busy`면 멈춤·이어 부르기, AD23) | — |
| [크래시 처리·실행](CrashHandlingAndLaunch.ko.md) 개정 13 | 20절 호스트 재시도·Fatal | — |
| [플랫폼 계층](PlatformLayer.ko.md) | "앱 수명 이벤트" 절(PR9) | — |
| [빌드 타깃](BuildTargets/README.ko.md) 개정 20 | 오디오 모듈 유형·Server 쿠킹 payload 제외(AD24) | — |
| [에셋 시스템](AssetSystem.ko.md) | 진단 `IgnoredSourceChunks`, `ReadyMask`와 오디오 준비 티켓의 관계(AD18) | 오디오 쿠킹 처리기·sidecar 청크 게시 세부는 A4 설계 때(AU5·AU6 전) |
| [Sparks 엔진 통합](SparksEngineIntegration.ko.md) 개정 2 | SE-G7 오디오 의미 | `Engine.sparkbind.json` 스키마 변경(`Play2D`)은 AU3b 코드와 함께 |
| [Trace 시스템](TraceSystem.ko.md) | — | `Audio`·`AudioMixer` 채널(AU8 전) |

## 9. 범위 밖과 채택하지 않은 UE 요소

### 9.1 채택하지 않은 UE 요소

- Bink Audio·RAD Audio 코덱, Vorbis 런타임 디코더(AD6).
- libsndfile(AD13).
- 레거시 `FIOSAudioDevice`, XAudio2 backend.
- `EAudioDeviceScope`의 장치 참조 계수 핸들(`FAudioDeviceHandle`) 공개 — Rocks는 `AudioEngineSubsystem`이 소유하고 World는 id만 쓴다.
- 다채널 `_L`·`_R` 분리 파일 임포트.
- `USynthComponent`(procedural은 `SoundGenerator` source 입력으로 — AU10). Synthesis 플러그인의 신스 컴포넌트 5종(`UGranularSynth`·`UModularSynthComponent`·`USynthComponentMonoWaveTable`·`USynthComponentToneGenerator`·`USynthSamplePlayer`)도 같은 길로, 신스를 `SoundGenerator` 구현으로 만들어 `AudioComponent`가 재생한다([컴포넌트 도입](ComponentCatalog/09-Plugins.ko.md) 9.5절).

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
- 개정 2(2026-11-22): 8절 다른 문서 개정을 반영했다(계획 1.17, 동기화 3, AppInfo 4, 엔진 런타임 6절, 크래시·실행 13, 플랫폼 계층 앱 수명 이벤트, 빌드 타깃 20, 에셋 시스템, Sparks 통합 2). 깨우기 신호 이름을 `RealtimeWakeSignal`(동기화 9.6절)로 맞췄다. PR9는 데스크톱 포커스가 PlatformWindow에 있어 AU3이 아니라 AU9만 막는다.
- 개정 3(2026-11-22): 3.20절 AU1 구현 세부(DSP1 — 형식·planar 버퍼·gain ramp·1극 필터·선형 리샘플러·패너·거리 감쇠, UE 5.8 식 대조)를 더했다. 3.13.2절 패닝을 UE `Get3DChannelMap`의 방위각 각도 비율 방식으로 고쳤다(개정 2까지의 right 축 투영 식은 UE와 결과가 달랐다).
- 개정 4(2026-11-22): DSP1 구현을 반영했다. 리샘플러 위치·비율은 블록 분할 불변을 위해 32.32 고정소수점이다(double 누적 버림). 소비 규칙은 ceil(다음 위치)이다. 정왼쪽 방위각은 UE 경계 결함(90°)을 따르지 않고 270°다. HPF fc ≥ fs/2는 a0 = 1이다. `Custom` 감쇠는 `Unsupported`다. 시험 등록 위치는 EngineTests다.
- 개정 5(2026-11-22): DSP1 리뷰 반영. 필수 — HPF fc ≥ fs/2의 a0 = 1(극 −1, 상태 발산)을 UE처럼 fs/2 − 1 Hz clamp로 고쳤다. 권장 — HPF bypass를 UE와 같은 20 Hz(+ KINDA_SMALL)로, 리샘플러 비율 범위를 UE와 같은 [0.01, 100]으로 맞췄다. 7절의 UE 리샘플러 서술(16.16 고정소수점)·방위각 경계 차이(정후면·수직)·5.1 서라운드 쌍 선택을 기록했다. gain ramp 목표 유한 계약, `VoiceCenterChannelVolume`은 AU3, 시험 표를 보강했다.
- 개정 6(2026-11-22): 3.21절 AU1 DSP2 dynamics 처리기(UE `FDynamicsProcessor` 블록 경로 대응 — 설정·식·실시간 계약·시험)를 더했다. 3.20절의 DSP2 범위를 효과 표(3.14절)에 맞춰 dynamics로 줄였다(biquad EQ·plate 리버브는 AU7).
- 개정 7(2026-11-22): DSP2 리뷰 반영. 권장 — 비유한 key를 0으로 보고(UE는 엔벌로프가 NaN에 갇힘), ratio 하한 1e-8·input/output gain ±96 dB를 두고, denormal은 AudioRT·Inline 렌더 진입의 FTZ·DAZ로 정했다(Inline 믹서 계약). 참고 — 큰 블록 분할 처리, 빈 처리기 no-op, 링 크기 문구(+1), UE float 계산 차이(7절), 시험(릴리스·평균 제곱 과도·knee clamp·lookahead 늘리기·Reset 평균 제곱·설정 전 필드)을 보강했다.
- 개정 8(2026-11-22): 3.22절 AU1 믹서 코어(Mixer1·Mixer2 범위, 명령 인자 전달 `RenderBlock`, clip 표·사용 수, voice 상태 표, Q 불변 ramp, 구간과 무관한 끝 판정, 마스터 limiter 프리셋·clamp, FTZ 실행 함수, `OfflineAudioRender`)을 더했다. 초안 교차 검증 1회 반영(필수 6 — Q 불변, 끝 판정, Immediate 정렬, 상태 표, clip 해제 경쟁, 6.2 충돌; 권장 — UE mono 2D 0.5·stereo→mono 0.707, FTZ만(DAZ 아님), 출력 clamp, 공용 scratch, 비유한 PCM 거부, 스레드 모드 pending 배열, Mixer2를 AU1에 남김; 참고 — loop 0/0, pitch [0.4, 2], 할당 시험 대상, 기본 stop fade 512). PR3은 AU2의 선행으로 옮겼다. 3.20.5절에 `OutputFramesBefore`를 더했다.
- 개정 9(2026-11-22): Mixer1 구현 리뷰 반영. fade 끝 = Stop + N과 자연 끝 동점 규칙(3.22.5절), 무시한 명령의 종류별 계수와 Prepared 회수 절차, stereo clip `Azimuth`의 `PreparePlay` 거부, `MaxMasterGain`, 구간 수 문구와 미룸·늦음 이중 계수, 마스터 경로에서 비유한 정리를 limiter 앞으로(3.22.6절).
- 개정 10(2026-11-22): 3.23절 AU1 Mixer2 스레드 경계(제어·RT 슬롯 소유 분리, 공통 순번, generation 묶은 seqlock, stop mailbox, Play에 실은 stop-all epoch, 시작·terminal 슬롯과 `releaseMixFrame` ACK, 계수 스냅샷, pending 정리, 범위 한정 두 스레드 시험)을 더했다. 초안 교차 검증 1회 반영(필수 5 — 새 수명 seqlock 소실, ACK 한 블록 어긋남, stop-all이 대기 Play를 놓침, pending 막힘, RT 계수 경쟁 읽기). 3.22.4절 상태 표의 Reserved 거부·반복 Stop(더 이른 끝만)·Finished 해제 조건, 3.5.3절 ACK 문구, 7절 UE 명령 경로 비교를 고쳤다.
- 개정 11(2026-11-23): Mixer2 구현 리뷰 반영. 필수 — mailbox는 마지막 값이 아니라 그 수명의 최소 fade만 쓴다(스케줄링에 따라 steal이 사라지던 문제·ABA), stop-all은 RT가 처리한 epoch를 출판하고 제어가 미처리 epoch의 짧은 fade를 유지한다. 권장 — 두 스레드 시험을 ACK gate·terminal 관찰로 강화, 처리 순서·Stopping·Prepared stop-all·짧은 fade·stale 거부·clip 해제 시험, seqlock·계수 칸 static_assert. 참고 — seqlock 순번 64비트 문구, stop-all 건너뜀은 무시 계수에서 뺀다.
- 개정 12(2026-11-23): 3.24절 AU5 앞당김 — ADPCM 블록 코덱(UE MS ADPCM 형식·식, 채널 블록 교차 배치, 계수 탐색 인코더, 임의 접근 디코드). 청크 표·`AudioDecoder` 인터페이스·쿠킹은 PR11 뒤, Opus는 PR6 뒤. 구현 리뷰 반영: 넘침 없는 크기 검사, delta 상한·64비트 디코드, 유효 frame 오차, 리터럴 표·byte 대조 시험.
- 개정 13(2026-11-23): 3.25절 AU4 앞당김 — 원본 디코드 라이브러리(결과 형식 `DecodedSourceAudio`, WAV RIFF/RIFX·EXTENSIBLE·smpl·cue·labl, AIFF·AIFF-C NONE·sowt·fl32). `SoundClipImporter`·`SoundClip` 에셋은 AU3 뒤.
