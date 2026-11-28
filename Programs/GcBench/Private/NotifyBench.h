#pragma once

#include <Base/Types.h>

// 변경 통지 비용 벤치다(Docs/Design/ChangeNotification.ko.md CN-D12). Registry 하나에 통지 대상
// 필드 둘(Notify Int32 — 수동 함수표)인 타입의 객체를 만들고 네 가지를 잰다(ns/연산, 측정 구간의
// 할당 수):
// - 구독자 없는 통지: 객체 objects개를 돌며 NotifyIndex(슬롯 통지 bit 하나로 끝나야 한다 — 할당 0)
// - 구독자 있음·그 번호 비트 꺼짐: 대상 1을 구독한 객체에 대상 0 통지(목록을 훑지 않는다)
// - 구독자 n명 발행: 대상 0 구독자 1·8·64명에 통지(통지 한 번과 구독자 호출 한 번의 비용)
// - 구독·해제: 한 객체에 TrySubscribe 직후 Unsubscribe
// - 등록된 write(FieldWriteOperation): 구독자 없음, 구독자 하나·바뀐 값(사전 read·비교·통지),
//   구독자 하나·같은 값(사전 read·비교만)
// - 통지 묶음 열고 닫기(경로 변경 연산마다의 비용)
// 반환값은 0이 성공이다.
//
// 사용 예:
//   if (const int status = gc_bench::RunNotifyBench(1000000, 1024); status) {
//       return status;
//   }
namespace gc_bench {

int RunNotifyBench(rocks::uint64 iterations, rocks::uint32 objects) noexcept;

} // namespace gc_bench
