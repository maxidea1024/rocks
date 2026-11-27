# 작업 진행 보고와 취소 — 9. 받는 쪽

[목차](README.ko.md)

## 9. 받는 쪽

### 9.1 명령줄 보고기(`ConsoleProgressReporter`)

- 사람이 읽는 줄을 Log API로 낸다. 로그 범주 이름은 `"Progress"`(변수 `LogProgress`), 문구는 원문(9.2절). 로그는 stderr로 가므로 stdout JSONL과 섞이지 않는다.
- `Pump(uint64 nowNanoseconds)`를 펌프 주인이 명시 호출한다. 시작·끝 사건은 Pump 때 바로, 진행 줄은 `DefaultConsoleProgressIntervalNanoseconds` = 2 s마다 최상위 작업별 한 줄: 계수 `Cooking packages: 1,234 / 5,000 (24%) - Maps/Arena`, 비율 `Building mesh: 41% - Welding vertices`, 불확정 `Loading: in progress`. 계수 줄의 퍼센트는 그 줄의 완료/총량이다(하위 작업을 섞은 보이는 비율 `D`를 쓰면 같은 줄의 수와 어긋난다), 비율 줄은 `D`다. 시작 줄은 `<제목>: started`, 끝 줄은 `<제목>: done|failed|cancelled|abandoned`(사유가 있으면 ` (<사유>)`). 실패·취소·Abandoned 끝과 "사건 누락" 줄은 Warning, 나머지는 Info 수준(Rocks 로그에 Display 수준이 없다 — UE Display 대응). `Hidden` 작업은 시작·끝만. 첫 Pump는 진행 줄 시계만 맞추고(사건 cursor 0 = 지금부터라 그 전에 시작한 작업의 시작 줄은 없다) 그 뒤 간격마다 낸다. 줄 출력은 `ConsoleProgressReporterDesc::write`로 바꿀 수 있다(시험).
- **펌프 주인.** 명령의 주 스레드 반복이 부른다. 한 호출로 오래 막는 명령은 `ProgressPumpThread`(Progress 모듈 — 전용 스레드가 간격 대기 후 `Tick` = 펌프 + 종료 요청 확인, `Stop`은 마지막 Tick 한 번 뒤 회수)를 쓴다. 시험은 스레드 없이 `Tick`을 직접 부른다. 펌프 주인은 매 Pump에 `IsTerminationRequested()`(Launch)를 보고 처음 보았을 때 `RequestCancelAllTopLevel()`을 부른다. Progress 모듈은 Launch에 의존하지 않으므로 이 확인은 명령 쪽 glue(`ProgressPumpThread`에 함수로 주입)가 한다.
- 명령은 기본으로 켜고 `--no-progress`로 끈다(AppInfoCommandLine `--kebab` 형식).

### 9.2 원문과 지역화 id

- 원문: `ResolveText`에 invalid `CatalogSnapshot`을 넘겨 fallback 원문을 얻는다(`Localization/Text.h` — invalid snapshot은 원문으로 fallback). 이 원문은 source culture 원문이다.
- 지역화 id: `TryGetPersistentForm`이 Invariant·Localized 형태에서 `TextId`(Guid)와 `sourceRevision`을 준다. Format·Integer 이력은 `Unsupported`이므로 id 없이 원문만 보낸다 — 이런 문구는 부모 쪽에서 다시 지역화할 수 없다(한계 15.6).

### 9.3 기계 판독 줄(`ProgressJsonlWriter`)

EngineImplementationPlan 28.4절의 "stdout은 선택 JSONL/progress protocol"을 이 절이 정한다. `--progress=jsonl`이면 stdout에 한 줄 JSON을 쓴다. 이 모드에서 명령의 다른 stdout 출력(예: `shader-compile`의 결과 이름)은 stderr 로그나 파일로 보낸다.

```text
{"v":1,"e":"begin","id":7,"parent":0,"title":"Cooking packages","titleId":"<guid>","titleRev":3,"pres":"background","cancel":true,"weight":1.0,"selfWeight":1.0}
{"v":1,"e":"progress","id":7,"shape":"count","frac":0.2468,"done":1234,"total":5000,"msg":"Maps/Arena","msgId":"","msgRev":0,"self":0.2468,"cancelRequested":false}
{"v":1,"e":"progress","id":9,"shape":"fraction","frac":0.4125,"msg":"Welding vertices","msgId":"<guid>","msgRev":1,"cancelRequested":false}
{"v":1,"e":"end","id":7,"outcome":"succeeded","detail":"","detailId":"","detailRev":0}
{"v":1,"e":"lost","count":12}
```

- `frac`는 보이는 비율 `D`(표시용 — 하위 작업 합성 포함)다. `self`는 자기 진행 `S`이고 그 작업이 자기 구간·계수를 보고했을 때만 있다. begin의 `weight`·`selfWeight`는 합성 가중치다. 받는 쪽(10.1절)은 `self`·가중치로 같은 트리를 다시 합성한다 — `frac`를 자기 진행으로 쓰면 하위 작업이 두 번 섞인다(PG3 리뷰). 새 칸은 더한 것뿐이라 판은 1 그대로다(읽는 쪽은 모르는 칸을 넘긴다).
- 부모 begin은 언제나 자식 begin보다 먼저 쓴다. 사건이 유실되면(`lost`) begin을 쓴 작업 가운데 스냅샷에서 사라진 것에 `abandoned` end를 보충한다(end를 잃어도 받는 쪽 하위 작업이 남지 않게).
- `outcome`: `succeeded`·`failed`·`cancelled`·`abandoned`. `shape`: `indeterminate`·`fraction`·`count`(`done`·`total`은 count만). id 필드가 빈 문자열이면 원문만 있다.
- `begin`·`end`·`lost`는 Pump 때 바로 쓰고, `progress`는 `DefaultJsonlProgressInterval` = 0.2 s마다 쓰되 직전과 같은 작업은 건너뛴다.
- 줄 최대 길이 `MaxProgressJsonlLineBytes` = 64 KiB. 문구 칸 하나는 `MaxProgressJsonlTextBytes` = 8 KiB까지 UTF-8 경계에서 자른다(최악의 이스케이프 6배여도 줄 상한 안 — 한 줄에 문구 칸은 하나다).
- `ProgressJsonlWriter`(PG3): `Pump`(사건 줄 바로, progress는 간격마다 바뀐 작업만)와 `Flush`(간격 무시 — 명령이 끝나기 직전). 스냅샷에 처음 보이는 작업은 begin 사건을 놓쳤어도(writer보다 먼저 시작) begin 줄을 먼저 쓴다 — 받는 쪽이 모르는 id를 받지 않게. progress의 `msg`는 구간 문구 → 작업 문구(제목은 begin 줄에 있다). 기본 출력은 표준 출력이고 줄 하나를 한 번에 쓴다(줄이 섞이지 않게).
- 읽는 쪽은 `v`가 다르면 `Unsupported`로 거부하고 그 자식의 진행을 끊는다(자식 프로세스 자체는 계속).

### 9.4 에디터(나중 — UiToolkit·에디터 단계)

계약만 정한다.
- `Blocking` 작업이 `modalDelaySeconds` 뒤에도 살아 있으면 진행 창(깊이별 문구 줄, 최상위 막대, 취소 단추). 여러 막는 작업은 하나의 창에 쌓는다(UE와 같다).
- `Background` 작업은 알림(제목·막대·취소, 끝나면 결과 표시 후 사라짐, 실패는 남김 — UE `bKeepOpenOnFailure`).
- 대기 작업 등록소 합계는 상태 표시줄.
- UI 스레드는 프레임마다 스냅샷을 당기고, 막는 작업 중에는 UI 펌프 hook으로 그린다.
