#pragma once

#include <Base/Types.h>

namespace rocks::platform {

// 경과 시간·deadline 비교용 단조 시계다. UTC나 영속 timestamp로 사용하지 않는다.
// 반환 단위는 ns지만 실제 해상도·변환 정밀도까지 1ns임을 보장하지 않는다.
// OS 조회 실패나 표현 범위 초과는 잘못된 시간 대신 Fatal로 종료한다.
uint64 MonotonicNanoseconds() noexcept;

// 현재 UTC 시각을 0001-01-01 00:00:00 UTC부터의 100ns tick으로 반환한다(rocks::DateTime 표현).
// 벽시계이므로 사용자·NTP 조정으로 뒤로 갈 수 있다. 경과 시간·deadline에는 MonotonicNanoseconds를
// 쓴다. 윤초는 반영하지 않는다(OS가 돌려주는 POSIX/Windows 시각 그대로). OS 조회 실패나 표현
// 범위(0001-01-01–9999-12-31) 밖이면 Fatal이다.
int64 UtcTicks() noexcept;
// 현재 native thread의 식별 값이다. 종료된 thread의 ID는 OS가 재사용할 수 있으므로
// 장기 보존하는 작업·객체의 generation이나 프로세스 간 식별자를 대체하지 않는다.
uint64 CurrentThreadId() noexcept;

} // namespace rocks::platform
