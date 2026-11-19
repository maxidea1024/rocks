#pragma once

#include <Core/Memory/Allocator.h>

// 할당자 작은 벤치다(Docs/Design/GarbageCollectionScale.ko.md 18.2절 — gate A3·A4·A5의 측정).
// 그래프·GC 없이 할당자 하나의 할당·반환만 잰다.
namespace gc_bench {

// allocator로 네 가지를 재어 한 줄씩 출력한다. name은 출력 표지다(NUL 종료).
// - 쌍: 크기 16·64·256·1024·8192의 할당 직후 반환(단일 스레드, ns/쌍)
// - 묶음: 64 byte 블록 4096개를 할당한 뒤 같은 순서로 반환(단일 스레드, ns/연산)
// - 스레드 사이: 한 스레드가 256개를 할당해 넘기면 다른 스레드가 반환한다(명시 handoff, ns/쌍).
//   같은 흐름을 한 스레드에서 한 값과 함께 낸다.
// - 동시: 1개·8개 스레드가 각자 할당·반환(64 byte, 창 64개)을 되풀이한 처리량(백만 쌍/초)
// 반환값은 0이 성공이다. allocator는 여러 스레드의 동시 호출을 지원해야 한다.
//
// 사용 예:
//   SystemAllocator system;
//   if (const int status = gc_bench::RunAllocatorMicroBench(system, "SystemAllocator"); status) {
//       return status;
//   }
int RunAllocatorMicroBench(rocks::Allocator& allocator, const char* name) noexcept;

} // namespace gc_bench
