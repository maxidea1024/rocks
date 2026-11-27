# 작업 진행 보고와 취소 — 15. 한계와 후속

[목차](README.ko.md)

## 15. 한계와 후속

1. **사용자 확인(Prompt).** UE `EAsyncTaskNotificationState::Prompt`(계속/취소 묻기)는 두지 않았다. 필요한 기능이 생기면 더한다.
2. **남은 시간 추정.** 두지 않았다. 받는 쪽이 스냅샷 이력으로 할 수 있다.
3. **멈춤 감지.** 스냅샷의 마지막 진행 시각으로 받는 쪽이 "응답 없음"을 보일 수 있다. 기준 시간은 에디터 단계에서 정한다.
4. **여러 자식 프로세스.** 하위 작업 트리는 자식마다 따로 붙는다. 프로세스 사이 합계 표시는 받는 쪽 몫이다.
5. **비용 수치(PG2 측정, 2026-11-27).** `Programs/ProgressBench`(Release, Windows 11 개발 기기, 3회 평균의 범위, 묶음 전체 시간 ÷ 호출 수):

   | 측정 | 값 | 요구 9 |
   |---|---|---|
   | 바인딩 없는 구간 생성 + `EnterStep` + 소멸 | 5.0–5.7 ns | 모듈 경계 호출·thread-local 읽기만 — 충족 |
   | 바인딩 없는 열린 구간의 `EnterStep` | 1.5 ns | 충족 |
   | Primary 아래 문구 없는 `EnterStep`(seqlock 쓰기) | 6.4–7.3 ns | 할당·잠금 없음 — 충족(시험 `Progress_BoundStepsWithoutMessagesDoNotAllocate`) |
   | Primary 아래 구간 생성 + `EnterStep` + 소멸(trace 꺼짐) | 28–30 ns | 할당·잠금 없음 |
   | 작업 수준 계수 `Add` | 3.7–4.9 ns | 원자 더하기 |
   | 스냅샷(살아 있는 작업 1·64·1024개, 같은 객체 재사용) | 0.09–0.12 µs·3.7–4.4 µs·70–79 µs | 받는 쪽 비용(hub 잠금 안) |

   스냅샷은 작업마다 제목·문구 `Text::Clone`(노드 잠금 + 참조 계수)을 하므로 작업 수에 비례한다. 1024개를 0.2 s마다 당겨도 0.04% 수준이다. 수치는 보장이 아니라 이 기기·구성의 측정값이다.
6. **Format 문구 재지역화.** 9.2절대로 Format·Integer 이력 문구는 자식에서 부모로 원문만 넘어간다.
7. **가장 바깥 구간 여럿.** 5.5절대로 둘째 가장 바깥 구간의 진행은 보이는 값 고정 때문에 앞 구간 끝 값을 넘기 전까지 보이지 않는다.
8. **취소 전달 지연.** 자식 취소는 최대 `ChildControlPollInterval`만큼 늦다. P1 `Process`에 교차 스레드 깨우기가 생기면 polling을 없앤다.
9. **Windows pipe 표준 입력 대기.** 익명 pipe는 기다릴 수 있는 handle이 아니라 시간 제한 읽기가 5 ms씩 쉬며 남은 byte를 확인한다. 부모 제어 수신 스레드는 자식 명령이 사는 동안 초당 수십 번 깬다(가벼운 확인 한 번). 겹친 I/O pipe로 만들면 없앨 수 있지만 부모가 만든 pipe 종류를 자식이 고를 수 없다.
10. **손자 명령의 취소.** 부모 cancel 줄은 그 순간의 자식에만 간다. 자식이 손자 명령을 띄우면 손자에게 넘기는 것은 자식의 `ChildProgressReader`가 한다(사슬).
11. **같은 로더의 Engine 둘.** 등록소 호스트 서비스는 먼저 시작한 Engine 것 하나다(8장 PG5 연결). 그 Engine이 먼저 끝나면 엔진 묶음 모듈의 source는 남은 Engine의 등록소에 없고, 먼저 끝난 Engine의 등록소 `Shutdown`이 그 source를 닫았으므로(`CountedPendingWork` token은 되돌리지 않는다) 남은 Engine에서 그 종류의 새 일은 바로 취소된다(예: Development 공급자의 첫 만들기·재컴파일). Engine 여럿을 한 로더로 오래 돌리는 호스트가 생기면 서비스를 Engine마다가 아니라 로더 수명의 등록소 하나로 바꾼다.
12. **명령의 Ctrl-C.** `RocksCommandlet`은 Ctrl-C를 종료 요청 표시로 받지 않는다(OS 기본 동작으로 바로 끝난다 — 다른 명령의 동작을 바꾸지 않으려고). 셰이더 워커는 입력 pipe EOF로 스스로 정리된다(ShaderSystem 11절 6항). 명령이 Ctrl-C에서 정상 경로 `Cancelled`를 내야 하면 `CommandProgressDesc::terminationRequested`와 Launch `handleTerminationRequests`를 그 명령에 켠다.
