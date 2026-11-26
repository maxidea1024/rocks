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
| AD25 | 에디터·Tryout은 쿠킹하지 않은 clip을 재생할 때 실행 플랫폼 데이터를 만든다(UE DDC 방식). AU4는 원본 bulk → 재생 PCM 직접 변환(원본 정밀도 — PCM16 플랫폼 데이터와 다르다, 개정 33), AU5·AU6 뒤 AssetNugget 경유 플랫폼 데이터로 | 권장안 |
| AD26 | 장치 콜백 → AudioRT 깨우기는 잠금 없는 단일 대기자 신호(`RealtimeWakeSignal`: Windows `WaitOnAddress`, Android futex, macOS Mach semaphore) | 권장안 |
| AD27 | 리버브 bypass·없음으로 전환해도 꼬리 에너지가 임계 아래로 머문 뒤에 끄고 이력을 지운다(UE는 즉시 지움) | 권장안 |
| AD28 | Opus 청크는 앞 청크 끝 320 ms(개정 27, 처음 80 ms) 패킷을 복제해 독립 디코드, looping clip은 loop 머리 PCM을 미리 디코드해 둔다 | 권장안 |
| AD29 | Sound Cue 대응(`SoundEvent`)을 두지 않는다. 랜덤·레이어·switch·지연·loop 같은 합성은 MetaSounds 대응 `SoundGraph`의 노드로만 만든다 | 사용자 결정(2026-11-25) |
