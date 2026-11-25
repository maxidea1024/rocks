## 9. 범위 밖과 채택하지 않은 UE 요소

### 9.1 채택하지 않은 UE 요소

- Bink Audio·RAD Audio 코덱, Vorbis 런타임 디코더(AD6).
- libsndfile(AD13).
- 레거시 `FIOSAudioDevice`, XAudio2 backend.
- `EAudioDeviceScope`의 장치 참조 계수 핸들(`FAudioDeviceHandle`) 공개 — Rocks는 `AudioEngineSubsystem`이 소유하고 World는 id만 쓴다.
- 다채널 `_L`·`_R` 분리 파일 임포트.
- `USynthComponent`(procedural은 `SoundGenerator` source 입력으로 — AU10). Synthesis 플러그인의 신스 컴포넌트 5종(`UGranularSynth`·`UModularSynthComponent`·`USynthComponentMonoWaveTable`·`USynthComponentToneGenerator`·`USynthSamplePlayer`)도 같은 길로, 신스를 `SoundGenerator` 구현으로 만들어 `AudioComponent`가 재생한다([컴포넌트 도입](../ComponentCatalog/09-Plugins.ko.md) 9.5절).

### 9.2 범위 밖

- 콘솔 플랫폼 backend·코덱(UE 클론에도 없다).
- AudioLink(외부 오디오 미들웨어 연결), VoIP·음성 채팅(네트워크 단계 뒤).
- UE 실험 기능: Subsonic, `IAudioMixerGeneratorSource`, AudioChannelAgnosticCore, Harmonix.
- Interchange 오디오 번역기.
- 형상 기반 자동 음향(레이 트레이싱 울림 계산). 필요하면 AU10 공간화 확장 위 후속 항목.
- 오디오 분석(AudioSynesthesia) 기능.
