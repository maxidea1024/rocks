#pragma once

#include <Base/Types.h>

namespace rocks::platform {

// 경과 시간·deadline 비교용 단조 시계다. UTC나 영속 timestamp로 사용하지 않는다.
// 반환 단위는 ns지만 실제 해상도·변환 정밀도까지 1ns임을 보장하지 않는다.
// MonotonicTicks와 같은 원천을 읽어 ns로 바꾼 값이다(MonotonicTicks 설명 참조).
// OS 조회 실패나 표현 범위 초과는 잘못된 시간 대신 Fatal로 종료한다.
uint64 MonotonicNanoseconds() noexcept;

// 단조 시계의 원시 tick이다(UE FPlatformTime::Cycles64 대응). 계측(trace)처럼 호출 비용이 중요하고
// 단위 변환을 나중에 몰아서 하는 곳에 쓴다. 경과 시간·deadline 계산은 MonotonicNanoseconds를 쓴다.
// - 원천: Windows QueryPerformanceCounter, macOS·iOS mach_absolute_time, Linux·Android
//   clock_gettime(CLOCK_MONOTONIC)의 ns 값.
// - MonotonicNanoseconds와 같은 원천이다. 같은 시점에 읽은 두 값은
//   MonotonicNanoseconds() == ticks / 주파수 × 10^9(소수 버림)의 관계를 지킨다.
// - 0의 기준 시점은 정하지 않는다(부팅 시각 등 플랫폼마다 다르다). 값끼리 빼서만 쓴다.
// - 절전 중 시간이 tick에 들어가는지는 원천을 따른다(Windows는 들어가고, Apple·Linux 계열은
//   빠진다). 프로세스 사이에서는 같은 기기 안에서만 비교한다.
// 같은 스레드의 연속 호출은 감소하지 않는다. OS 조회 실패는 Fatal이다.
//
// 사용 예:
//   const uint64 start = MonotonicTicks();
//   DoWork();
//   const uint64 elapsed = MonotonicTicks() - start;
//   const double seconds = double(elapsed) / double(MonotonicTickFrequency());
uint64 MonotonicTicks() noexcept;

// MonotonicTicks의 초당 tick 수다. 0이 아니고 프로세스 수명 동안 바뀌지 않는다.
// Windows는 QueryPerformanceFrequency, Apple은 mach_timebase_info에서 구한 10^9 × denom / numer
// (정수로 나누어떨어지지 않는 timebase는 반올림한다), Linux·Android는 10^9다.
// OS 조회 실패나 표현할 수 없는 주파수는 Fatal이다.
uint64 MonotonicTickFrequency() noexcept;

// 현재 UTC 시각을 0001-01-01 00:00:00 UTC부터의 100ns tick으로 반환한다(rocks::DateTime 표현).
// 벽시계이므로 사용자·NTP 조정으로 뒤로 갈 수 있다. 경과 시간·deadline에는 MonotonicNanoseconds를
// 쓴다. 윤초는 반영하지 않는다(OS가 돌려주는 POSIX/Windows 시각 그대로). OS 조회 실패나 표현
// 범위(0001-01-01–9999-12-31) 밖이면 Fatal이다.
int64 UtcTicks() noexcept;

// UtcTicks와 같은 값을 ticks에 쓰고 true를 돌려준다. OS 조회 실패나 표현 범위 밖이면 Fatal 대신
// false이고 ticks는 바꾸지 않는다. 실패해도 멈추면 안 되는 곳(로그 출력 스레드의 UTC 환산)이 쓴다.
bool TryUtcTicks(int64& ticks) noexcept;
// 현재 native thread의 식별 값이다. 종료된 thread의 ID는 OS가 재사용할 수 있으므로
// 장기 보존하는 작업·객체의 generation이나 프로세스 간 식별자를 대체하지 않는다.
uint64 CurrentThreadId() noexcept;

} // namespace rocks::platform
