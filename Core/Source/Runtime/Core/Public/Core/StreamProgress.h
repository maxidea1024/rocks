#pragma once

#include <Base/Types.h>

namespace rocks {

// 역압이 있는 byte 스트림 한 번의 호출 결과다. 압축(Compression/CompressionStream.h)과
// 암호(Crypto) 스트림이 같은 타입을 쓰므로, 호출자는 두 스트림을 중간 버퍼 하나로 잇는다.
//
// - consumed: 이번 호출에서 소비한 입력 byte 수다. 출력이 모자라면(역압) 입력보다 작을 수
//   있으며, 남은 입력은 다음 호출에 다시 넘긴다.
// - produced: 이번 호출에서 출력 Span 앞쪽에 쓴 byte 수다.
// - complete: 뜻은 API마다 다르다(예: Finish 계열은 "더 낼 출력이 없다", 해제 스트림은
//   "스트림 경계에 도달"). 각 함수 설명을 따른다.
//
// 사용 예(입력을 모두 흘려보내는 반복):
//   Span<const uint8> pending = input;
//   while (!pending.IsEmpty()) {
//       auto step = stream.TryProcess(pending, output);
//       if (!step) { return step.Error(); }
//       Consume(output.Left(step.Value().produced));
//       pending = pending.RightChop(step.Value().consumed);
//   }
struct StreamProgress {
    usize consumed = 0;
    usize produced = 0;
    bool complete = false;
};

} // namespace rocks
