#pragma once

#include <Audio/AudioDecoder.h>

// 쿠킹된 Opus 플랫폼 데이터의 런타임 디코더다(Docs/Design/AudioSystem.ko.md 3.32.4절). Audio는 이
// 모듈을 링크하지 않고, 디코더 표를 만드는 쪽(AU3 AudioEngineSubsystem, 도구, 시험)이 등록한다.
//
// 사용 예:
//   AudioDecoderTable table;
//   RegisterBuiltInAudioDecoders(table);
//   RegisterOpusAudioDecoder(table);
//   DecodedAudioPcm pcm(allocator);
//   DecodeAudioPlatformData(view, table, allocator, pcm);
namespace rocks::audio {

// codec Opus 디코더 factory를 넣는다. 이미 있으면 AlreadyExists다.
//
// 디코더 동작:
// - Open: codec이 Opus가 아니면 InvalidArgument, bitstream 판 ≠ 1이면 Unsupported(다른 규칙보다
//   먼저 본다), 채널 1–8·rate 48000·codec 머리(3.32.2절)·채널 마스크 = 기본 배치가 아니면
//   InvalidArgument다. libopus 상태와 scratch를 여기서만 할당한다(할당 실패 OutOfMemory). 실패하면
//   앞 상태(열린 청크)를 남기지 않는다.
// - BeginChunk(시작·seek): 길이 접두·패킷마다 960 frame(첫 stream TOC)·패킷 수·frame 수를 먼저
//   검사하고(InvalidArgument), 상태를 비운 뒤 시작 패킷 320 ms(16 패킷, OpusPreRollFrames) 앞
//   (payload 머리 앞이면 머리)부터 디코드해 startCodecFrame 앞을 버린다. 시작 패킷이 스트림 패킷
//   0–16이면 연속 디코드와 bit까지 같고, 그 밖은 pre-roll로 수렴한 근사다(측정 약 91 dB, 3.32.4절).
//   같은 시작 위치는 언제나 같은 표본을 낸다.
// - ContinueChunk(순차 재생): 앞 청크를 끝까지 냈고(DecodeNext가 0) 새 청크 첫 frame = 앞 청크
//   끝일 때만 상태를 이어 열고 복제 pre-roll 패킷은 건너뛴다. 그래서 처음부터 순차로 낸 결과는
//   청크를 어떻게 나눴든 청크 하나로 디코드한 결과와 bit까지 같다. 조건이 어긋나거나 payload가
//   손상이면 InvalidArgument이고 열린 청크도 닫는다.
// - DecodeNext: 패킷 하나(960 frame, 시작 패킷은 나머지)를 내부 채널 순서 planar로 쓴다. 손상
//   패킷은 InvalidArgument이고 청크를 닫는다(다시 BeginChunk해야 한다).
// - MaxFramesPerCall = 960이다. 디코드 중 힙 할당은 없지만 libopus가 스택 가변 배열을 쓴다.
ROCKS_AUDIOOPUS_API Result<void> RegisterOpusAudioDecoder(AudioDecoderTable& table) noexcept;

} // namespace rocks::audio
