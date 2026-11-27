# 작업 진행 보고와 취소 — 10. 프로세스 경계

[목차](README.ko.md)

## 10. 프로세스 경계

### 10.1 자식이 보내는 진행

- 자식 프로세스(쿠킹 프로세스·`shader-compile` 명령·외부 도구 어댑터)는 `--progress=jsonl`로 9.3절 줄을 stdout에 쓴다.
- 부모는 `ChildProgressReader`로 읽어 자기 작업 아래 하위 작업으로 다시 만든다. 자식 id → 부모 쪽 id 대응표 상한은 `MaxChildProgressTasks` = 4096이고, 넘는 하위 작업은 만들지 않고(그 진행은 부모 쪽에서 안 보임) 처음 한 번 Warning.
- 깨진 줄은 Warning을 한 번 남기고 무시한다. 자식이 끝나면 남은 하위 작업을 받은 결과로, 결과가 없으면 `Failed`(비정상 종료) 또는 `Abandoned`(정상 종료인데 end가 없음)로 닫는다.
- **pipe 소유.** P1 `Process`는 한 객체를 한 번에 한 스레드만 다루고 교차 스레드 깨우기가 없다(`PlatformBase/Process.h` 스레드 계약). 그래서 `ChildProgressReader`는 Process를 소유한 한 스레드가 돌린다: `Read(timeout = ChildControlPollInterval)`(0.1 s)를 반복하고, 반복 사이에 부모 token을 보아 취소면 stdin에 `cancel` 줄을 한 번 쓴다(10.2절). 셰이더 워커 채널과 같은 방식이다.
- **stderr.** 자식 stderr는 부모 stderr를 상속하거나 파일로 보낸다. stdout 하나만 오래 읽으므로 stderr를 pipe로 열어 두면 교착이 난다(`Process.h` 경고). `mergeErrorIntoOutput`은 쓰지 않는다(JSONL과 섞인다).

### 10.2 부모 제어 모드와 취소

- 자식은 `--parent-control`일 때만 stdin을 제어 채널로 쓴다. `ParentControlListener`(전용 스레드)가 줄을 읽고:
  - `cancel` → `RequestCancelAllTopLevel()`. 자식은 정상 경로로 `Cancelled`를 내고 종료 코드 130(EngineImplementationPlan 28.4절 "사용자 중단")으로 끝난다.
  - EOF(부모가 사라짐) → 같은 취소. 셰이더 워커의 "입력 EOF면 자식을 정리하고 끝남"과 같은 규칙이다.
  - 모르는 줄 → Warning 후 무시.
- `--parent-control` 없이 실행한 명령(CI가 stdin을 닫고 띄우는 경우 등)은 stdin을 보지 않는다. EOF로 취소되지 않는다.
- 부모는 취소 뒤 호출 기능이 정한 제한 시간 안에 자식이 안 끝나면 강제 종료한다. 시험은 hook으로 자식이 취소를 무시하게 만들어 강제 종료 경로를 결정적으로 검사한다.

### 10.3 셰이더 워커(ShaderSystem SH1d가 따를 것)

- 셰이더 워커는 JSONL을 쓰지 않는다. 기존 제어 pipe 줄 프로토콜에 job 하나가 끝날 때마다 `job <seq> <index>` 줄을 더한다. 워커 채널은 지금 모르는 줄을 받으면 워커를 끝내므로(ShaderSystem 11.1절 SC4·SC6 채널 규칙), **ShaderSystem 스펙을 먼저 개정**해 메시지 버전을 올리고 채널 파서가 이 줄을 받게 한다.
- 관리자의 계수: `job` 줄이 오면 그 묶음의 잠정 완료 수를 올린다. 묶음이 끝나면 잠정 값을 실제 결과(성공·실패·재시도로 돌림)로 바꾼다. 묶음이 실패해 이미 받은 `job` 줄의 결과가 사라지면 그 job은 재시도나 실패로 다시 세고, 잠정 값이 줄어도 보이는 비율은 hub의 고정 규칙(5.5절 4)으로 뒤로 가지 않는다.
- 이 방식으로 진행은 묶음이 아니라 job 단위로 오른다 — UE(묶음 단위)보다 촘촘하다. 결과 자체는 지금처럼 묶음 출력 파일로 받는다.
