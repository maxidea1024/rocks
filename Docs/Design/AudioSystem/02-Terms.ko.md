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
| `USoundCue`·`USoundNode*` | 대응 없음 | 합성 사운드는 `SoundGraph`만 쓴다(AD29, 7절) |
| `USoundBase` | `SoundSource`(추상) | `SoundClip`·`SoundGraph`의 공통 기반 |
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
| MetaSounds | `SoundGraph` | 합성·절차 사운드 그래프(AU10, M7) |
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
