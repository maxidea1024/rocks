#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <TraceAnalysis/TraceModel.h>

namespace rocks {

// report 형식의 판이다. JSON 배치(키·순서)가 바뀌면 올린다.
inline constexpr uint32 TraceReportVersion = 1;

// 모델의 결정적 JSON report를 out 뒤에 붙인다(Docs/Design/TraceSystem.ko.md 10절 TR4,
// `trace-report`). 같은 입력 byte에서 늘 같은 byte를 낸다. 키 순서는 고정이고, 배열은 스레드
// id·이름·id 순으로 정렬하며, 시간은 정수 ns다(실수 서식을 쓰지 않는다).
//
// 담는 것:
// - session: 세션 GUID, tick 주파수
// - summary: 잘림·정의 일부 없음·중간에 붙음(앞부분 없음)·손실 수·손실 구간·버린 정의·
//   이름 정의 누락·읽지 못한 스트림·시작 없는 끝·끝 없는 구간·순서 번호 구멍·표지 수, 채널별 손실
// - threads: 스레드별 구간 이름의 횟수·합계·p50/p95/p99(nearest-rank, 끝난 구간만)·끝 없음 수
// - tasks: 수·실행·실패·취소, 임계 경로(앞 → 뒤 task id, 실행 시간 합, 첫 만듦부터 마지막 완료까지)
// - counters: 이름 순, 표본 수, 마지막 값(정수 카운터) 또는 마지막 값 bit(실수 카운터, 16진)
// 이름 정의가 없는 구간은 "<unknown:id>"로 쓴다.
// 실패: 메모리 부족 OutOfMemory.
Result<void> WriteTraceReportJson(const TraceModel& model, Array<uint8>& out) noexcept;

} // namespace rocks
