#pragma once

namespace rocks::test {

// platform::Thread를 거치지 않고 OS API로 바로 만든 스레드에서 entry(context)를 부르고, 그 스레드가
// 완전히 끝날 때까지(스레드 종료 정리 포함) 기다린다(Windows CreateThread, POSIX pthread_create).
// 엔진 밖에서 만든 스레드(OS 스레드 풀, 외부 라이브러리)에서도 스레드 종료 hook이 도는지 볼 때
// 쓴다. 스레드를 만들지 못하거나 기다리지 못하면 false다.
//
// 사용 예:
//   void Work(void* context) noexcept { static_cast<Counter*>(context)->Touch(); }
//   REQUIRE(test::RunOnOsThread(Work, &counter));
bool RunOnOsThread(void (*entry)(void*) noexcept, void* context) noexcept;

} // namespace rocks::test
