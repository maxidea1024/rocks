## 4. 결정 요약

- 모듈: `Audio`(믹서·DSP·backend 인터페이스) / `AudioMiniaudio`(장치, ClientOnly) / `AudioOpus` / `EngineAudio`(Object 세계) / `AudioTools`(임포트·쿠킹). 의존은 단방향.
- 스레드: Game → 제어 pipe(`SerialTaskChain`, UE 오디오 pipe) → AudioRT 전용 스레드(`Highest`) → 출력 링(최소 2칸, 협상한 콜백 크기로 계산) → 장치 콜백(복사와 무잠금 깨우기만).
- AudioRT는 무할당·무잠금·무대기(깨우기 신호 제외). 결정적 시험은 Inline 모드 오프라인 렌더.
- 장치 IO는 miniaudio 저수준 API, 그 위 책임은 모두 Rocks.
- 원본은 무손실·손실 모두 받아 PCM으로 저장(사용자 결정), 24비트·float 정밀도 유지. 런타임 코덱은 PCM·ADPCM·Opus. 에디터는 재생 때 실행 플랫폼 데이터를 만든다.
- 재생 핸들과 voice 핸들 분리, 동시 발음 그룹과 voice 상한 분리, 가상화 4모드(UE 그대로).
- 리버브 존은 UE처럼 리버브 하나 파라미터 보간, 진행률은 mix clock.
- 서버·`--no-sound`는 장치 없이 `Ignored`, 게임 진행은 완료 통지에 기대지 않는다.
- 합성 사운드는 MetaSounds 대응 `SoundGraph` 하나, Sound Cue 대응은 두지 않는다(사용자 결정).
