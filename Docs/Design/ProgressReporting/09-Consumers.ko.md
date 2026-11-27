# 작업 진행 보고와 취소 — 9. 받는 쪽

[목차](README.ko.md)

## 9. 받는 쪽

### 9.1 명령줄 보고기(`ConsoleProgressReporter`)

- 사람이 읽는 줄을 Log API로 낸다. 로그 범주 이름은 `"Progress"`(변수 `LogProgress`), 문구는 원문(9.2절). 로그는 stderr로 가므로 stdout JSONL과 섞이지 않는다.
- `Pump(uint64 nowNanoseconds)`를 펌프 주인이 명시 호출한다. 시작·끝 사건은 Pump 때 바로, 진행 줄은 `kDefaultConsoleProgressInterval` = 2 s마다 최상위 작업별 한 줄: `Cooking packages: 1,234 / 5,000 (24%) - Maps/Arena`. 실패·취소·Abandoned 끝은 Warning, 나머지는 Display 수준. `Hidden` 작업은 시작·끝만.
- **펌프 주인.** 명령의 주 스레드 반복이 부른다. 한 호출로 오래 막는 명령은 `ProgressPumpThread`(Progress 모듈 — 전용 스레드가 간격 대기 후 `Pump`·종료 요청 확인)를 쓴다. 펌프 주인은 매 Pump에 `IsTerminationRequested()`(Launch)를 보고 처음 보았을 때 `RequestCancelAllTopLevel()`을 부른다. Progress 모듈은 Launch에 의존하지 않으므로 이 확인은 명령 쪽 glue(`ProgressPumpThread`에 함수로 주입)가 한다.
- 명령은 기본으로 켜고 `--no-progress`로 끈다(AppInfoCommandLine `--kebab` 형식).

### 9.2 원문과 지역화 id

- 원문: `ResolveText`에 invalid `CatalogSnapshot`을 넘겨 fallback 원문을 얻는다(`Localization/Text.h` — invalid snapshot은 원문으로 fallback). 이 원문은 source culture 원문이다.
- 지역화 id: `TryGetPersistentForm`이 Invariant·Localized 형태에서 `TextId`(Guid)와 `sourceRevision`을 준다. Format·Integer 이력은 `Unsupported`이므로 id 없이 원문만 보낸다 — 이런 문구는 부모 쪽에서 다시 지역화할 수 없다(한계 15.6).

### 9.3 기계 판독 줄(`ProgressJsonlWriter`)

EngineImplementationPlan 28.4절의 "stdout은 선택 JSONL/progress protocol"을 이 절이 정한다. `--progress=jsonl`이면 stdout에 한 줄 JSON을 쓴다. 이 모드에서 명령의 다른 stdout 출력(예: `shader-compile`의 결과 이름)은 stderr 로그나 파일로 보낸다.

```text
{"v":1,"e":"begin","id":7,"parent":0,"title":"Cooking packages","titleId":"<guid>","titleRev":3,"pres":"background","cancel":true}
{"v":1,"e":"progress","id":7,"shape":"count","frac":0.2468,"done":1234,"total":5000,"msg":"Maps/Arena","msgId":"","msgRev":0,"cancelRequested":false}
{"v":1,"e":"progress","id":9,"shape":"fraction","frac":0.4125,"msg":"Welding vertices","msgId":"<guid>","msgRev":1,"cancelRequested":false}
{"v":1,"e":"end","id":7,"outcome":"succeeded","detail":"","detailId":"","detailRev":0}
{"v":1,"e":"lost","count":12}
```

- `outcome`: `succeeded`·`failed`·`cancelled`·`abandoned`. `shape`: `indeterminate`·`fraction`·`count`(`done`·`total`은 count만). id 필드가 빈 문자열이면 원문만 있다.
- `begin`·`end`·`lost`는 Pump 때 바로 쓰고, `progress`는 `kDefaultJsonlProgressInterval` = 0.2 s마다 쓰되 직전과 같은 작업은 건너뛴다.
- 줄 최대 길이 `kMaxProgressJsonlLineBytes` = 64 KiB. 넘는 문구는 잘라 쓴다(UTF-8 경계에서).
- 읽는 쪽은 `v`가 다르면 `Unsupported`로 거부하고 그 자식의 진행을 끊는다(자식 프로세스 자체는 계속).

### 9.4 에디터(나중 — UiToolkit·에디터 단계)

계약만 정한다.
- `Blocking` 작업이 `modalDelaySeconds` 뒤에도 살아 있으면 진행 창(깊이별 문구 줄, 최상위 막대, 취소 단추). 여러 막는 작업은 하나의 창에 쌓는다(UE와 같다).
- `Background` 작업은 알림(제목·막대·취소, 끝나면 결과 표시 후 사라짐, 실패는 남김 — UE `bKeepOpenOnFailure`).
- 대기 작업 등록소 합계는 상태 표시줄.
- UI 스레드는 프레임마다 스냅샷을 당기고, 막는 작업 중에는 UI 펌프 hook으로 그린다.
