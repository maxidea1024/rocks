## 6. 단계

### 6.1 선행작업

| ID | 선행작업 | 상태(2026-11-22) | 할 일 | 막는 단계 |
|---|---|---|---|---|
| PR1 | 이 스펙 | 개정 1 | 교차 검증 반영 | AU1 이상 |
| PR2 | 스레드 우선순위 | 구현(`Highest`) | 없음. 실시간 등급은 AU9 측정 뒤 PlatformLayer 개정 | — |
| PR3 | Core 동시 컨테이너 SY7(`BoundedSpscQueue`·`FixedBlockPool`·`IndexedFreeList`) | 완료(46dd9f81, 2026-11-25) | SY0→SY1→SY2→SY7 구현 | AU2(개정 8: 믹서가 명령을 인자로 받아 AU1에서 옮김 — 3.22.1절) |
| PR21 | 무잠금 깨우기 신호 `RealtimeWakeSignal`(AD26) | 완료(SY12, 46dd9f81) | SY0 → SY12 구현 | AU2 |
| PR4 | `SerialTaskChain`·`WorkerScheduler`·`TaskLane::Audio` | 구현 | 없음 | — |
| PR5 | ThirdParty 반입 | 절차 구현, 오디오 라이브러리 없음 | miniaudio(AU2), libFLAC·libvorbis·libogg·libopus·libopusfile·minimp3(AU4), 런타임 libopus(AU5) | AU2, AU4, AU5 |
| PR6 | 모듈 유형 `ClientOnly`, Server 대상 오디오 제외 | 완료(BS2 0d9c3a8b) | BuildTargets 구현 | AU2(서버 빌드 확인은 AU3) |
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
| AU2 | (개정 28: AU2a Audio 모듈 장치 경로 → AU2b miniaudio·`AudioMiniaudio`, 3.33절. 개정 31: AU2b 세부 3.34절) `AudioOutputBackend`, AudioRT 스레드·명령 링(`BoundedSpscQueue`)·정렬 pending 배열(3.22.1절)·출력 링(칸 수 계산)·`RealtimeWakeSignal`, `NullAudioOutput`, ThirdParty `Miniaudio`, `AudioMiniaudio`(Windows 먼저)와 `HostServices` 등록 | AU1, PR3, PR5, PR6, PR21 | Sandbox 시험음 재생, 협상값·실제 스레드 우선순위·underrun 기록, Q ≠ 콜백 크기에서 underrun 0, 장치 없음·열기 실패·종료 자원 반환. macOS·Android는 실행한 것만 판정 |
| AU3 | `EngineAudio`: `AudioEngineSubsystem`·`AudioWorldSubsystem`·제어 pipe·`AudioDeviceContext`, `SoundSource`·`SoundClip`(메모리 PCM)·`AttenuationSettings`(거리), `AudioComponent`, 재생 API·결과, 리스너, CVar(비활성 창 볼륨 포함), 무음 실행·서버, 종료 drain, Tryout 정리 계약 | AU2, PR7, PR8 | Sandbox에서 키 입력으로 UI음·움직이는 부착 루프, 좌우 방향, 맵 교체·컴포넌트 파괴·GC 뒤 stale voice·lease 0, 서버 빌드에 `AudioMiniaudio` 미링크 |
| AU3b | Sparks 바인딩 | AU3, PR18 | Sparks 시험: 재생·정지·`LoopHandle` 수명, 서버 `Ignored` |
| AU4 | `AudioTools`: `SoundClipImporter`, WAV·AIFF 파서, FLAC·Vorbis·Opus·MP3 디코드(SIMD 고정), ThirdParty 디코더, 에디터 재생용 PCM 플랫폼 데이터 변환(AD25) | AU3, PR10, PR5 | 3.19절 임포트 시험, 디코드 SHA 고정값, 재생 중 재임포트 시 새 재생부터 새 revision |
| AU5 | `AudioEncoder`·`AudioDecoder`·`AudioDecoderTable`, ADPCM·Opus 인코더·디코더, `AudioOpus`, 플랫폼 데이터·청크 표·플랫폼 재정의, 쿠킹 처리기·AssetNugget 키 | AU4, PR11, PR5 | 같은 입력 같은 byte, 설정 변경 새 키, 디코드 허용오차, 손상 payload 거부 |
| AU6 | 로딩 동작(`Inherited` 포함), `AudioChunkCache`, sidecar 게시, 디코드 worker·PCM 링, seek·loop·loop 머리 PCM, 준비 티켓, 굶음 정책 | AU5, PR12 | 3분 곡 첫 음 전 전체 read 없음, 100회 seek 메모리 상한, 늦은 완료 폐기, Opus loop 이음매 불연속 없음 |
| AU7 | `ConcurrencyGroup`·해결 규칙, voice 상한·steal, 가상화, `SoundCategory`·`MixSnapshot`, `MixBus` graph·send·plan 교체, EQ·compressor·plate 리버브·delay, 리버브 존(3.15절), 감쇠 나머지(모양·공기 흡수·거리별 send), 차폐 목표값 입력 | AU3(AU6과 병행 가능), 차폐 완성은 PR16 | 3.19절 동시 발음·가상화·리버브 존 시험, 그래프 교체 시 꼬리 보존 |
| AU8 | trace 채널·카운터, 재생 추적, 콘솔 명령, WAV 캡처. 3D 시각화·`AudioInspector`는 PR17 뒤 같은 단계의 후반(AU8b) | AU3(AU8b는 PR17) | "왜 안 들리나"를 `audio.playback`으로 재현, trace-report 집계 |
| AU9 | 장치 교체·rate 변경·앱 수명·모바일 세션, 다채널 출력·downmix, 품질 프로필, 1시간 부하, 실시간 등급 판단, 병렬 렌더 판단 | AU2, PR9, PR13, PR15, PR19 | 3.18절 목표, 콜백 종료 뒤 접근 0, 실행 플랫폼별 결과 기록 |
| AU10 | `BeatClock`, 모듈레이션, `SoundGraph`(M7 — 합성 사운드는 이것 하나, AD29), `SignalBus`, 공간화·soundfield 확장 인터페이스(HRTF·ambisonics), convolution 리버브, 지역화 대사, 에디터 미리듣기. 마이크 입력은 capture backend 설계를 이 문서 개정으로 먼저 정한다 | AU6, AU7, AU8, PR17, PR20 | 기능별 CPU·메모리 예산 측정, 그래프 교체·꼬리 보존, 오프라인 재현 |

단계마다 리뷰어 1명이 교차 검증하고, 스펙·문서 관점은 묶음 끝에 본다.
