## 1. 현재 상태

| 항목 | 현재 Rocks | 문제 |
|---|---|---|
| 오디오 코드 | 없다. 오디오 모듈·장치 backend·디코더·인코더·ThirdParty 오디오 라이브러리·사운드 에셋 클래스·임포터가 없다 | — |
| 설계 계약 | 계획 §18(1673–1706행): AudioRT 금지 목록, `AudioVoiceHandle{index,generation}`, `AudioCommand{sampleTime,voice,op,data}`, `RenderAudioBlock(float*, frames) noexcept`, 불변 graph plan 교체·epoch ACK, 48 kHz·256/512 frame, underrun 0·callback p99.9 ≤ 블록 50%. 마일스톤 M4 기본 오디오, M7 audio graph(§30.1) | 콜백에서 직접 믹스하는 모양으로 적혀 있다. UE 구조(전용 렌더 스레드 + 출력 링)와 다르다(3.2절, AD2) |
| Task | `TaskLane::Audio`, `DedicatedLane`(FIFO 작업 lane, Thread·Inline 모드), `SerialTaskChain`(`Core/Source/Runtime/Task`) | `DedicatedLane::Post`는 `Function<void()>` 작업 노드를 받는다. 블록마다 작업을 게시하는 모양은 AudioRT의 무할당 계약과 맞지 않는다(3.2절) |
| 스레드 우선순위 | `ThreadPriority { Lowest, BelowNormal, Normal, AboveNormal, Highest }`(`Core/Source/Runtime/PlatformBase/Public/Platform/Thread.h:23`). Windows `Highest` = `THREAD_PRIORITY_HIGHEST`(`Private/Win64/Concurrency.cpp:232`) | UE 오디오 렌더 스레드도 `TPri_Highest`다(3.2절). 실시간 등급은 없지만 UE와 같은 수준은 있다 |
| 동시 컨테이너 | `BoundedMpscQueue`(구현). `BoundedSpscQueue`·`FixedBlockPool`·`IndexedFreeList`는 [동기화](../Synchronization.ko.md) SY7 설계만 | `BoundedMpscQueue`는 생산자가 예약 슬롯을 게시하기 전에 선점되면 소비자 `TryPop`이 실패할 수 있다. AudioRT 입력은 SPSC여야 한다(Synchronization D27) |
| 깨우기 수단 | `ManualResetEvent`·`Semaphore`는 Signal·Release 안에서 `Mutex`를 잡는다(`Core/Source/Runtime/PlatformBase/Public/Platform/Sync.h:86–108`, `Private/Common/Sync.cpp:61–68`). SY0 `WaiterSignal` 설계는 Windows `WaitOnAddress`, Android futex, macOS `pthread_mutex_t`+`pthread_cond_t`(Synchronization 350행) | 장치 콜백이 AudioRT를 깨울 때 잠금을 공유하면 우선순위 역전이 생긴다. 무잠금 깨우기가 없다(3.2절, PR21) |
| 엔진 수명 | `EngineSubsystem`의 `Initialize`·`OnFrameBegin`·`OnFrameEnd`·`BeginShutdown`·`CompleteInFlightWork`·`Deinitialize`(`Engine/Source/Runtime/Engine/Public/Engine/Engine.h:46–70`). 종료 2단계 주석에 "audio epoch" 대기 | `CompleteInFlightWork` 실패는 `KeepFirst`로 누적되고 정리가 이어진다. 장치 콜백이 살아 있는 채 소유자가 풀릴 수 있다(3.8.6절) |
| 명령줄 | AppInfo 개정 4가 `RunTraits::audioOutput`·`audioMuted`와 `--no-sound`·`--mute-audio`를 설계했다(미구현 — AP1) | 판정 값이 아직 코드에 없다(3.8.4절) |
| 설정 | 콘솔 영역 `"audio"`(`Core/Source/Runtime/Config/Public/Config/ConsoleSettings.h:66`), `audio.master.volume` 시험 사용 | 오디오 CVar가 없다 |
| 빌드 대상 | [빌드 타깃](../BuildTargets/README.ko.md) 59–63행에 모듈 유형 `ClientOnly`. 69행은 Server의 기능 축소 범위를 네트워크 단계 스펙에 넘긴다 | BS2(0d9c3a8b)로 빌드 코드에 `ClientOnly`가 생겼고 `AudioOpus`가 쓴다(C3). 오디오 모듈의 Server 범위는 이 문서가 정한다(AD24) |
| 에셋 | A1 구현, A2a–A2e 구현, A2f 임포트 프레임워크는 A2f1a 구현(진단 등록·원본 루트·원본 읽기·임포터 표)·A2f1b·A2f2 설계(`AssetImporter`·`AssetImporterTable`, AssetSystem 7.1절 18), A3 AssetNugget·A4 AssetCooker 설계. `ReadyMask { Cpu, Render }`(`Engine/Source/Runtime/AssetRuntime/Public/AssetRuntime/AssetManager.h:36`) | 오디오 재생 구간이 준비됐는지 표현할 수단이 없다(3.12.4절) |
| Sparks | 스키마 `Engine/Source/Developer/SparksTestHost/Schema/Engine.sparkbind.json`에 `SoundAsset`·`AudioSystem.PlayAt/StartLoop`·`World.Audio`. [Sparks 엔진 통합](../SparksEngineIntegration.ko.md) SE-G7·EI5는 모듈 없음으로 대기 | — |
| policy | `PolicyChecks.WindowsMacroLeak.cs`가 `xaudio2.h`·`mmdeviceapi.h`·`audioclient.h`를 Windows 매크로 누출 헤더로 안다 | 장치 헤더는 backend 모듈의 Private에만 둔다 |
