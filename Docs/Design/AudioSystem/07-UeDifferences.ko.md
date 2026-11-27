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
| 파일 loop 구간 | `USoundWave` looping은 늘 파형 전체, `smpl` loop는 cue point(loop region)로만 남고 믹서가 쓰지 않는다 | looping이면 사용자 `loopOverride` > 파일 loop > 전체(개정 33, 3.36.2절) | intro + loop 음악을 그래프 없이 |
| 원본 bulk 정밀도 | 16비트 교차 WAV byte | 디코드 PCM planar, Int16·Int24·Float32(3.10.1절) | 24비트·float 원본 정밀도 유지 |
| 합성 사운드 에셋 | Sound Cue(`USoundNode` 트리)와 MetaSounds 둘 다 | `SoundGraph`(MetaSounds 대응) 하나 | UE도 새 작업은 MetaSounds가 주력이고 Sound Cue 기능(랜덤·레이어·switch·지연·loop)은 그래프 노드로 덮인다. 두 체계를 따로 유지하지 않는다(AD29) |
| Opus 플랫폼 rate | `bResampleForDevice`면 Opus도 등급 rate(8·12·16·24·48 kHz 중)로 인코드 | Opus 플랫폼 데이터는 늘 48 kHz, 등급 rate는 PCM16·ADPCM만 | 디코더·청크 규칙을 rate 하나로 둔다(3.32절), byte는 품질 배수로 줄인다(3.39.3절) |
| 짧은 loop 디코드 | non-streaming은 길이와 상관없이 `DTYPE_RealTime`(`AudioDevice.cpp:7027`), loop는 디코더 seek | loop 길이가 1 s(`StreamMinLoopSeconds`) 미만인 looping clip은 로드 때 통째 디코드(3.40.3절) | 스트림 디코더가 바퀴마다 loop 시작을 다시 열고 pre-roll을 디코드하므로(3.40.2절) 짧은 loop의 비용 상한 |
| voice 상한 동률 | 볼륨 가중 우선순위 `Sort`(안정 정렬 아님 — 동률 순서 미정) | 동률은 먼저 수락한 재생이 voice를 갖는다(3.41.1절) | 결정적 출력·시험 |
| SeekRestart 위치 | voice가 있던 wave에서만 기록(`AudioVirtualLoop.cpp:129–152`) — 수락 때부터 가상이던 loop는 처음부터, 상한으로 빠졌던 loop는 낡은 `SeekRestartTimes`(`SoundWave.cpp:3720–3733`) | 언제나 논리 커서(pitch 적분)에서, `ResumeFadeFrames` 256 fade-in(3.41.1·3.41.3절). Restart는 UE처럼 `startFrame`부터 | 정식 기능, 가운데 시작의 끊김 방지(UE 믹서 초기 ramp 여부는 미확인) |
| 동시 발음 재시작 간격·ducking·steal fade 시간 | 실시간(`FPlatformTime`)·게임 delta | mix clock frame(3.41.2절) | 오프라인 렌더·Inline 시험 결정성 |
| 동시 발음 그룹 목록 순서 | `RemoveSwap`으로 지워 순서가 섞임 | 수락 순을 지킴(3.41.2절) | `StopOldest` 동률·cull 동률이 결정적 |
| 2D 재생의 동시 발음 거리 | ActiveSound Transform 위치로 잼 | 위치가 없어 0(3.41.2절) | NonSpatial 재생은 위치를 갖지 않는다 |
| 기본 동시 발음 | `UAudioSettings::DefaultSoundConcurrency`(BaseEngine.ini에서는 비어 있음) | 두지 않음 — 그룹 없는 사운드는 제한 없음(3.41.2절) | 필요해지면 프로젝트 설정 개정 |
| voice 상한 단위 | render cost 합(`au.EnableRelativeRenderCostVoiceLimit` 1, wave 1.0) | 개수(3.41.1절) | wave만 있는 동안 같다. SoundGraph(AU10)에서 다시 본다 |
| 여유 부족 때 정지 중 voice 끊기 | `StopNow`(fade 없음 — `AudioDevice.cpp:4377–4446`), 동률이면 배열 앞쪽, 정지 중 voice가 없으면 loop 아닌 voice·아무 voice | 64 frame fade(`MinStopFadeFrames`), 동률이면 먼저 수락한 재생, 정지 중 voice만(스트림 슬롯 부족이면 스트림 clip을 쥔 것만 — 3.41.1절) | 끊김 방지·결정성, 자리 안 활성 voice는 상한 정렬이 지킴 |
| 최대 가청 거리의 거리 배수 | 없음(`GetMaxFalloffDistance`) | `falloffDistance ÷ distanceScale`(3.41.3절) | Rocks 감쇠 평가의 `distanceScale` 칸과 같은 끝 |
| 분류 값의 계산 단위 | 장치에 등록된 분류 전체를 프레임마다 에셋 값으로 되돌려 계층·믹스를 적용(공유 값 — 에셋 변경이 재생 중 소리에도 바로 닿는다) | 재생마다 수락 때의 분류 사슬 스냅숏으로 같은 식을 계산(3.43.2절) | 스냅숏 원칙(3.35.3절) — 에셋을 고치면 새 재생부터. 결과는 같은 사슬이면 같다 |
| 분류 없는 재생 | 프로젝트 `DefaultSoundClass`(설정이 비면 분류 없음) | 엔진 기본 분류(`SetDefaultCategory`, 호스트가 단다) 또는 분류 없음(3.43.1절) | 프로젝트 설정 로드는 호스트 몫이다 |
| 분류별 사용자 볼륨 | 엔진 기능 없음(사용자 믹스 `SetSoundMixClassOverride` 관행) | CVar 넷(`audio.volume.music`·`effects`·`dialogue`·`interface`)을 분류 `userVolume`으로 곱함(3.43.3절) | 게임마다 같은 관행을 엔진이 준다. 믹스가 사용자 값을 바꾸지 않는다 |
| always-play 동률 | 모두 `FLT_MAX/4`라 불안정 정렬이 순서를 정함 | 먼저 수락한 쪽(3.43.4절) | 결정적 출력·시험 |
| 수락 직후의 분류 always-play | wave instance 전이라 첫 동시 발음 평가에서 보이지 않음(`ActiveSound.cpp:1908–1929`) | 수락 때부터 사슬 값으로 봄(3.43.4절) | 사슬 스냅숏이 수락 때 이미 있다. 첫 평가만 다르다 |
| 믹스 다시 걸기(fade-out 중) | `duration ≥ 0`은 `FIS = now`·FIE를 고치지 않아 바로 t = 1, `duration < 0`은 `1 − t`로 역전(`AudioDevice.cpp:2744–2763`) — 레벨이 튄다 | 지금 레벨에서 이어 fade-in(`FIS = now − t × fadeIn`, 3.43.7절) | 튀는 것은 UE 결함으로 본다 |
| fade-out 중 믹스를 base로 | 바로 t = 1 | 다시 걸기 규칙(연속, 3.43.7절) | 같은 이유 |
| override·분류 거리 배수 fade 시간 | device delta(0.5 s로 자름 — `AudioDevice.cpp:7394–7398`) | mix clock(자르지 않음 — 3.43.7절) | 오프라인 렌더·Inline 시험 결정성. 끊긴 프레임에서만 다르다 |
| 차폐 질의 시점 | 기본은 매 검사 동기 trace(`au.UseAsyncOcclusionTraces` 0 — `ActiveSound.cpp:25–27·1602`) | 첫 검사만 Play 때 동기, 이후는 제어 쪽 요청 → 게임 쪽 공급자 → 명령 왕복(3.42.7절) | 제어 쪽은 게임 스레드에서 질의할 수 없다. 결과가 한 프레임 이상 늦다 |
| 첫 차폐 검사의 focus 볼륨 조건 | 첫 검사도 parse 안이라 focus 볼륨 뒤 `Volume > 0`일 때만(`ActiveSound.cpp:2178`) | Play 때 게임 쪽에서 하므로 focus 볼륨을 보지 않는다(3.42.8절) | 게임 쪽은 focus 계수를 모른다. 이후 검사는 같은 조건이다 |
| focus 첫 갱신 보간(`InitializeFocusFactorOnFirstUpdate` 0) | 첫 parse의 게임 delta만큼 움직인다 | 첫 갱신이 수락과 같은 묶음이라 dt 0 — 다음 묶음부터 움직인다(3.42.8절) | 수락과 첫 갱신을 같은 묶음에서 한다. 한 묶음 차이다 |
| 차폐 보간 시간 | `FDynamicParameter`가 게임 delta로 진행, 값은 한 프레임 늦게 읽음 | mix frame으로 세고 그 묶음 값을 씀(3.42.7절) | 오프라인 렌더·Inline 시험 결정성 |
| focus 보간 dt | 게임 delta | 묶음 사이 mix frame(3.42.5절) | 같은 이유 |
| 스테레오 spread의 영거리 방위각 | L 90°·R 270°(`AudioMixerSource.cpp:2187–2188` — 좌우가 바뀜) | L 270°·R 90°(3.42.6절) | 좌우를 맞춘다. 비공간화 반경이 0이면 이 거리에서 출력이 다르다 |
| Cone 원점에서의 방향 | 0 벡터 정규화 → `acos(0)` = 90° | 원뿔 안(θ = 0)으로 본다(3.42.2절) | 정의되지 않은 방향 — 원뿔 소리가 원점에서 끊기지 않게 |
| 공기 흡수 거리 구간 | Min > Max도 받음(로그 사상이 깨짐 — `Dsp.h:337–345`) | `InvalidArgument`(3.42.1절) | 잘못된 설정을 에셋 검사에서 막는다 |
| 빈 retrigger 그룹 | 비어도 영구히 남음 | 간격이 지나면 지움(3.41.2절) | 동작은 같고 표가 쌓이지 않음 |
| cull로 가상이 된 loop의 그룹 멤버십 | 원래 ActiveSound가 steal fade를 마칠 때까지 그룹에 남음(가상 사본은 그룹 밖 — `SoundConcurrency.cpp:1066–1104`) | 바로 그룹에서 빠짐, voice는 분리 목록에서 fade(3.41.2절) | 분리 voice는 재생 기록이 아니라 그룹 링크를 쥐지 않는다. fade 동안 cull 수가 하나 적다 |
| 가상 강제 검사 범위 | 리스너 이동이 크면 장치의 모든 가상 loop(`AudioDevice.cpp:3479–3488`) | 크게 움직인 리스너의 scene 재생만(3.41.3절) | World마다 리스너가 따로라 다른 World 재생을 볼 이유가 없다 |
| 빼앗겨 가상이 된 loop의 fade 중 voice | 원래 ActiveSound가 fade 중에도 공간 갱신 | 떼어 낼 때의 파라미터에 고정, scene 해제 때 짧게 끊음(3.41.3절) | 분리 voice는 기록이 없다. fade는 길어야 `voiceStealReleaseTime` |
| 살아 있는 재생 수 | 제한 없음(ActiveSound 배열) | `MaxPlaybacks` 1024(voice 없는·가상 포함), 그룹 8개·활성 그룹 1024 | 고정 용량 표(3.35.4절) |
| 정지 fade 중 voice의 경쟁 | stopping이면 `v × P − 101`로 정렬에 남음 | 정지·steal fade 중인 재생은 경쟁하지 않고 voice를 끝까지 쥠, 상한 수에 세지 않음(3.41.1절) | 별도 stopping 풀(UE `NumStoppingSources`)과 같은 자리를 믹서 여유 voice가 받음 |
| 스트림 디코드 지연 | 렌더 스레드가 디코드 작업 완료를 기다림(`AudioMixerSourceBuffer.cpp:550` `EnsureCompletion`) — 늦으면 렌더가 멈춤 | AudioRT는 기다리지 않음 — 링이 모자라면 그 voice만 무음(굶음)이고 이어 재생(3.40.1절) | AudioRT 무대기 계약(3.2절), 한 스트림의 지연이 전체 출력을 멈추지 않게 |
