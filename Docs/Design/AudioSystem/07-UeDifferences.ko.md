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
| 합성 사운드 에셋 | Sound Cue(`USoundNode` 트리)와 MetaSounds 둘 다 | `SoundGraph`(MetaSounds 대응) 하나 | UE도 새 작업은 MetaSounds가 주력이고 Sound Cue 기능(랜덤·레이어·switch·지연·loop)은 그래프 노드로 덮인다. 두 체계를 따로 유지하지 않는다(AD29) |
