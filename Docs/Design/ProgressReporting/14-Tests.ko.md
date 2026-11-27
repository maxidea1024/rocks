# 작업 진행 보고와 취소 — 14. 시험

[목차](README.ko.md)

## 14. 시험

시간 운에 기대지 않는다. 시계는 주입하고(가짜 시계를 시험이 직접 전진), 스레드 순서는 명시 gate로 만든다. 단언은 결정적인 최종 상태와 사건 순서에 한다.

PG2 시험 파일: `Core/Source/Runtime/Progress/Tests/ProgressScopeTests.cpp`(3·4·5·6·8·14, 문구·`ProgressParallelFor`)·`ProgressHubTests.cpp`(7·9·10·11·12·13). 11의 `Text::Clone` 실패는 노드 참조 계수 넘침뿐이라 직접 만들지 않고, 할당 실패를 `NFailAllocator`로 `Begin`의 모든 할당 지점에서 본다. 11의 "켜기 전"은 header-only 시험 접근점(`Private/ProgressTestAccess.h`)이 닫힌 hub를 되돌려 본다.

1. 연결 취소: 부모·자식·다단, 깊이 상한, 기본 부모 token, Source 쪽 조회도 사슬을 봄.
2. `CheckCancelled`가 `Cancelled`를 돌려줌.
3. 중첩 구간 비율: 3단 중첩, 가중치 다른 구간, 총량 0 범위, 남은 양 채움 — UE `FSlowTaskStack` 곱셈 합성과 같은 값(손 계산 표).
4. 입력 검사: 음수·NaN 작업량, 총량 초과 `EnterStep`, 깊이 상한 — 값과 Warning 1회.
5. 바인딩 없는 스레드의 구간은 아무 일도 안 하고 할당이 없다(할당 계수기).
6. 바인딩: Primary 둘 → `AlreadyExists`, 스택 순서, Secondary 구간 무시, `ProgressBindingSuspend`, 끝난 작업 → `StaleHandle`, handle을 다른 스레드에서 `Finish`해도 바인딩이 남지 않음.
7. 계수: 여러 스레드 `Add`(gate로 순서 고정), `AddTotal`로 총량 증가 시 `D` 단조, `done > total` 순간 값 자르기.
8. 구간 계수: 그 구간 몫만 채움, 구간이 끝난 뒤 `Add` 무시, 둘째 계수 → `AlreadyExists`.
9. 하위 작업 합성: `selfWeight`·`weightInParent`, 끝난 하위 작업 1, 늦게 붙는 하위 작업에도 `D` 단조, 자기 보고 없는 부모의 `w₀ = 0`.
10. 수명: `Finish` 없이 소멸 → `Abandoned` + Warning, 끝난 뒤 보고 no-op, `Shutdown` 뒤 handle·계수 no-op, 부모 끝 → 하위 `Abandoned`가 먼저 기록됨.
11. 오류 코드 표(5.2절) 각 줄.
12. 사건 기록: 두 소비자가 각자 cursor로 같은 사건을 받음, 고리 넘침 → `lostEvents`, 첫 호출 cursor 0.
13. 취소: `RequestCancel` → 작업·하위 작업 token, `RequestCancelAllTopLevel`, 취소 불가 → `Unsupported`.
14. UI 펌프 hook: 가짜 시계로 0.2 s 경계(직전·정확히·직후), 등록 스레드만 펌프, 펌프 안 재진입 없음.
15. 명령줄 보고기: 가짜 시계로 2 s 경계, 시작·끝 줄, `Hidden`, 실패·취소 줄 수준, 종료 요청 표시 → `RequestCancelAllTopLevel` 한 번.
16. JSONL golden(각 사건·모양·id 있음/없음·`lost`), 변화 없는 작업 건너뜀, 긴 문구 UTF-8 경계 자르기.
17. 원문 추출: Localized·Invariant·Format 문구.
18. 자식 읽기: 정상 왕복, 깨진 줄 무시, 버전 불일치 `Unsupported`, 대응표 상한, 비정상 종료 → `Failed`, end 없는 정상 종료 → `Abandoned`.
19. 자식 취소: 부모 취소 → `cancel` 줄 → 자식 `Cancelled`·종료 코드 130.
20. `--parent-control`에서 stdin EOF → 취소, `--parent-control` 없으면 닫힌 stdin이어도 계속.
21. 강제 종료 경로(hook로 자식이 취소를 무시).
22. 등록소: 합계, `RunBefore` 순서, 순환 → 등록 수락·의존 무시·Error 로그, 없는 이름 무시.
23. 소유 스레드 밖 호출 → `WrongThread`(`RemainingCount` 제외).
24. `FinishAll`이 사이사이 `ApplyCompleted`를 끼우고 끝에 남은 수 0, 진행 작업과 하위 계수.
25. `FinishFor`·`CancelFor`가 모든 source에 같은 목록, `Shutdown` 뒤 `Unregister`, 호출 중 `Unregister` → `Busy`.
26. `ApplyCompleted` 매개변수 전달(시간 예산·Tryout 전용).
