# Sparks VM

기준일: 2026-11-22 · 개정 1 · 상위 문서: [Sparks 언어](SparksLanguage.ko.md)(이하 SL) §8.3·§9·§11·18.2절 LC5, [Sparks bytecode](SparksBytecode.ko.md)(이하 BC)

이 문서는 언어 코어 LC5(VM 핵심 — interpreter, 값 버퍼, 숫자 검사와 fault, 예산, 부록 B 표준 API, host interface 1차)의 설계 정본이다. 실행 모델(instance·hook·큐·async — LC6)은 이 VM 위에 얹는다. 형식(opcode·명령 모양·`.sparkc`)은 BC가 정본이고, 이 문서는 그 형식을 실행하는 방법을 정한다.

## 1. 하위 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| LC5a interpreter 핵심 | 실행 문맥(VM stack·frame·호출 창), Scalar 명령 전부(이동·상수·정수·실수·비교·변환·분기·switch·합성 곱), 같은 모듈 `CALL`·`RET`, `TRY`·`RETERR`·`ENDEXEC`·`FAULT`, 숫자 fault(§3), 명령어·호출 깊이 예산(§4), 실행 corpus(`.run`) | `Run` 영역 사례의 결과·fault·예산 계수 golden |
| LC5b1 버퍼·문자열 | 값 버퍼 기반(참조계수·ledger·메모리 예산·단일 버퍼 상한 — §2.2), Buf 레지스터의 수명(복사·덮어쓰기·frame 해제·fault 해제), 문자열 명령(`LOADSTR`·`CONCAT`·`TOSTR`(enum 제외)·`EQ.STR`…`LE.STR`·문자열의 `EQ.B`·`NE.B`), host의 문자열 인자·반환, `.run`의 `string` | 종료 시 ledger 0(반환 값을 놓은 뒤), 문자열 corpus golden, 할당 실패·예산 시험 |
| LC5b2 컨테이너 | Array·Map·Set(`NEWA`·`NEWSET`·`NEWM`·`APPEND`·`PUT`·`GETE`·`SETE`·`LEN`·`GETKEY`·`GETVAL`, COW·해시 색인), 경로 쓰기 원소의 존재 검사(`FAULT 3`), `.run`의 컨테이너 표기 | 컨테이너 corpus golden, COW 공유·해제 시험 |
| LC5b3 struct·Result | `.sparkc` 형식 버전 2의 `TYPE` 구역(struct 필드 배치·상수 기본값 원형, enum case 이름 — BC §5), `NEWS`·`GETSF`·`SETSF`, enum `TOSTR`, Buf `Result`(`OK`·`ERR`·`TRY.B`·`RETERR.B`·`ENDEXEC.B`) | struct·Result corpus golden |
| LC5c 표준 API | 부록 B의 언어 함수(`CALLL` — Math·문자열·컨테이너·Format·Parse·Log·Assert·Random·Tag) | 함수마다 양성·음성(fault) corpus |
| LC5d host interface 1차 | `IHost`: 객체 handle 검증(generation), native 호출(`CALLN` — 인자 창, 결과), native 필드(`GETF`·`SETF`), 문맥 이름(`GETCTX`), 리터럴 해석(`NEWLIT`), `CALLV` 대상 결정 | SparksTestHost의 가짜 host로 엔진 schema 함수·필드 corpus |

대기(`AWAIT`)·이벤트(`EMIT`·`AWAITEV`)·RPC 송신·유지 자원 해제(`RELEASE`)의 실행 의미는 LC6이다. LC5에서 VM은 이 명령을 만나면 `ContractViolation` fault로 실행을 끝낸다(LC6 전의 자리 — 결정적이다). 실행 corpus는 그 명령이 없는 동기 함수만 쓴다.

## 2. 값과 frame

### 2.1 bank 레지스터

- **Scalar:** `uint64` 원시 값(BC §2 표 — 정수는 그 타입 범위의 부호·0 확장, 실수는 double 비트, `float32`는 float로 반올림한 double).
- **Ref:** `uint64` handle = (index << 32) | generation. 0은 null. 해석·수명 검사는 host(§6)가 한다 — VM은 handle을 해석하지 않는다.
- **Buf:** `Buffer*`(§2.2) 또는 null. 레지스터 복사는 참조계수 증가, 덮어쓰기·frame 해제는 감소다(VM이 frame을 떠날 때 Buf 레지스터를 모두 놓는다).

### 2.2 값 버퍼(LC5b)

- `VmBuffer` 머리: 참조계수(u32), 종류(문자열·Array·Map·Set·struct·Result·TagSet), 원소 배치(bank·slot 수 — 컨테이너 표지와 같은 모양), 원소 수, 논리 크기(예산 계수용 byte). 내용은 머리 바로 뒤의 연속 배열이다(문자열은 UTF-8 byte, Array는 원소마다 slot 묶음 — Buf 원소는 `VmBuffer*`).
- **수명:** 레지스터 복사(`MOV.B`·창으로의 이동)는 참조계수 +1, 덮어쓰기는 이전 값 −1(0이면 해제 — 원소의 Buf도 재귀적으로 놓는다. 값 struct는 순환이 없으므로(S0426) 깊이는 타입 깊이로 유한하고, 해제는 명시 stack으로 돌아 재귀 깊이에 기대지 않는다). frame을 내릴 때(`RET`) 반환 값 밖의 Buf 레지스터를, fault·실행 끝에는 모든 frame의 Buf 레지스터를 놓는다. 해제는 실패하지 않는다(SL §9 소유 범위 연산).
- **ledger와 예산:** 실행 문맥마다 살아 있는 버퍼 수와 논리 byte를 센다(할당 +, 해제 −). 새 버퍼가 메모리 예산(`VmLimits::memoryBytes` — 기본 16 MiB, SL §9.3 instance 예산의 LC5 자리)을 넘기거나 단일 버퍼가 16,777,216 원소 또는 64 MiB를 넘으면 그 명령 자리의 `BudgetExceeded`다(할당 위치를 보고 — SL §8.3). 할당자 실패(OS 메모리 부족)는 fault가 아니라 `Call`의 OutOfMemory다. 호출이 끝나 host가 반환 값을 놓으면 ledger는 0이어야 한다(시험이 확인한다).
- **문자열:** byte 열(UTF-8 — 컴파일러 출력과 host 입력이 UTF-8이다. VM은 다시 검사하지 않는다). `LOADSTR`는 상수 문자열의 새 버퍼를 만든다(실행마다 — 모듈 상수 버퍼 공유는 측정 뒤). `CONCAT A B C`는 창의 Buf B..B+C(조각 — 모두 문자열)를 이어 새 버퍼를 A에 둔다. 비교 `EQ.STR`·`LT.STR`…는 byte 순(SL §3.2), `string?`의 null은 null끼리만 같고 순서 비교에서 null은 어떤 문자열보다 작다(컴파일러는 `string?` 순서 비교를 내지 않지만 정해 둔다). `EQ.B`·`NE.B`는 같은 종류의 값 비교다(LC5b1은 문자열, 컨테이너·struct는 LC5b2·LC5b3).
- **`TOSTR` 표기(SL §3.2 보간):** `bool`은 `true`·`false`, 정수는 10진, 실수는 다시 읽으면 같은 값이 되는 가장 짧은 10진(Core 서식 — `float32`는 float 값으로)이고 고정 소수점 표기에 소수점이 없으면 `.0`을 붙인다(`1.0`·`-0.0`·`1e+38`), 수학 타입은 성분을 `, `로 이은 괄호(`(1.0, 2.0, 3.0)` — `Transform`은 Location·Rotation·Scale 묶음 셋의 괄호), 이름·`Tag`는 LC5d(host의 이름 표), enum은 LC5b3(TYPE 구역의 case 이름).
- **COW:** 쓰는 명령(`SETE`·`SETSF`·`APPEND`·`PUT`·`CALL.MUT`)은 참조계수가 1보다 크면 먼저 복제하고 그 레지스터를 새 버퍼로 바꾼다(이전 버퍼는 감소). 순환은 생기지 않는다(closure·참조형 값이 없다 — SL §4.1).
- **컨테이너(LC5b2):** 원소 배열은 삽입 순서이고 원소 하나는 key slot 묶음 뒤 값 slot 묶음이다(Array는 값만, Set은 원소를 key 자리에, Map은 둘 다). Buf slot은 원소 버퍼의 참조 하나를 가진다. 배치(bank·slot 수)는 명령의 컨테이너 표지에서 정하며 처음 원소를 넣는 명령(`APPEND`·`PUT`·`SETE`)이 버퍼에 적는다 — 빈 컨테이너는 배치가 없어도 된다(읽기는 없음·0을 돌려준다). Map·Set은 원소 배열 뒤에 열린 주소 해시 색인(원소 번호 + 1, 크기는 용량 2배 이상의 2의 거듭제곱)을 두고 key는 Core `HashStringBytes`(문자열 byte)·`MixHash`(slot 비트)로 흩는다 — 순회 순서는 색인과 무관하게 삽입 순이다(SL §6.3 결정적 순회). 용량은 2배로 늘리고(처음 4) 늘릴 때 원소는 옮기고(참조계수 그대로) 색인은 다시 만든다.
- **컨테이너 명령:** `NEWA`·`NEWSET`·`NEWM`은 빈 버퍼. `APPEND A B`는 Array 끝에 넣고 Set은 이미 있으면 넣지 않는다. `PUT A B C`·Map의 `SETE A B C`는 key B의 값을 C로 바꾸거나 끝에 넣는다(SL §4 "Map의 `m[k] = v`는 삽입 또는 교체"). Array의 `SETE`는 번호가 범위 밖(음수 포함)이면 `IndexOutOfRange`. `GETE A B C`는 `T?`다: Array 번호가 범위 밖이거나 Map에 key가 없으면 없음(Scalar는 표지 0과 0 slot, Ref 0, Buf null). `LEN A B`는 원소 수(null은 0). `GETKEY`·`GETVAL A B C`는 삽입 순 C번 원소의 key·값(Array는 값)이고 범위 밖이면 `IndexOutOfRange`다(컴파일러는 `LEN` 아래 번호만 낸다). 쓰는 명령은 COW다(공유된 버퍼는 먼저 복제 — 원소 버퍼는 참조 +1).
- **경로 쓰기의 원소(컴파일러):** 원소 읽기는 `T?`라 검사기가 원소를 거친 경로 쓰기(`m[k].X = 1`·`grid[i][j] = v`)를 S0412로, 원소의 복합 대입을 S0406으로 막는다 — 코드는 `if (let row = grid[i]) { … grid[i] = changed; }`로 쓴다. IR은 그래도 쓰기 경로로 원소를 읽으면 있음 검사와 실패 블록의 `FAULT 3`(`IndexOutOfRange` — SL §4 "복합 대입과 경로 쓰기는 키가 없으면 fault")을 낸다(규칙이 넓어져도 0 값이 끼어들지 않는 안전장치, SparksBytecode 3.1a절).
- 컨테이너·struct의 `==`는 언어에 없다(SL §3.3 — 오류). `EQ.B`·`NE.B`는 문자열만 만나며 그 밖의 종류는 `ContractViolation`이다.

### 2.3 실행 문맥과 frame

- **실행 문맥(`VmExecution`):** bank마다 하나의 레지스터 stack(연속 배열)과 frame stack. frame은 (함수, bank별 base, pc, 반환 대상)이다.
- **호출 창(BC §4.1):** 부르는 함수의 frame bank base + `registers[bank]`가 부르는 함수의 base다. 인자는 이미 창에 있고, 부르는 함수 frame은 `frame[bank]` 크기만큼 stack을 늘린다(늘린 slot은 0·null로 채운다 — 초기화 전 읽기가 정해진 값을 보게).
- **반환:** `RET A B C`는 callee의 bank C 레지스터 A..A+B를 callee base(= 창 0번)로 옮기고 frame을 내린다. 부르는 쪽은 창에서 결과를 옮긴다(BC §4.1).
- stack 상한은 호출 깊이 256(§4)과 bank마다 최대 frame 합이다(깊이 × 256 slot).

## 3. 숫자 검사와 fault

- 정수 산술(`ADD`…`MOD`·`NEG` `.I8`–`.U64`): 결과가 그 타입 범위 밖이면 `IntegerOverflow`, `DIV`·`MOD`의 0은 `DivideByZero`, 부호 있는 `MIN / -1`은 `IntegerOverflow`(`MIN % -1`은 0 — 결과가 범위 안이다, SL §3.3 "범위를 넘으면"). `DIV`(`Math.Div`)·`MOD`(`%`)는 내림 방식이다(SL §3.3 — `a == Math.Div(a, b) * b + a % b`, 나머지는 오른쪽 부호: `-7 % 2 == 1`, `7 % -2 == -1`). 컴파일러의 상수 접기(검사기·IR)도 같다. 결과가 범위 밖이면 대상 레지스터를 바꾸지 않는다.
- 실수 산술(`.F32`·`.F64`): 결과가 NaN·Inf면 `NonFinite`. F32는 float로 계산한다(입력이 float 값이므로 double로 계산해 float로 반올림한 것과 같다), float 범위 밖이면 `NonFinite`. FMA 축약 없음(빌드 규칙이 모든 C++ 번역 단위에 `-ffp-contract=off`를 준다 — §7). `Call`은 실행을 `platform::RunWithDeterministicFloatingPoint` 안에서 한다(nearest-even, FTZ·DAZ 끔 — 스레드 환경이 바뀌어 있어도 같은 결과). 묶음 실수 비교 `EQN.F`는 비트로 판정한다(비트가 같고 NaN이 아님, 또는 둘 다 ±0 — IEEE 같음과 같고 있음 표지 slot에도 안전).
- `CONV`: 정수 → 정수는 대상 범위 밖이면 `IntegerOverflow`, 정수 → 실수는 정확히 표현되지 않으면(2^53 초과 등) `IntegerOverflow`, `F32 → F64`는 그대로, `F64 → F32`는 최근접 짝수, 범위 밖 `NonFinite`. 실수 → 정수와 숫자가 아닌 종류(bool·enum·이름·표지·AssetId)의 `CONV`는 컴파일러가 내지 않으므로 `Unreachable`이다(`Math.*ToInt` — LC5c).
- **합성 곱:** `MUL.QUAT`·`MUL.MAT4`는 Core `Quaternion<double>`·`Matrix4<double>`의 `operator*`(UE `FQuat`·`FMatrix`와 같은 누적 순서)를 쓴다. 성분이 유한하지 않으면 `NonFinite`.
- **fault 기록:** 종류(SL §8.3 표 + VM의 `Unreachable`(`FAULT 1`·정의하지 않은 `CONV`)·`HookTryFailed`(`FAULT 2` — SD43)), 함수 번호, 명령 주소, 원문 위치(BC `locations`). fault는 실행 문맥 전체를 끝낸다(frame을 모두 내리며 Buf를 놓는다). 같은 (모듈 hash, 함수, 주소, 종류)의 보고 횟수 집계는 LC6 실행 모델이 한다.
- `FAULT 1`(도달하지 않는 경로 — BC §3.1a)은 `ContractViolation`이 아니라 VM 불변식 위반이다(verifier를 통과한 컴파일러 출력에서는 나오지 않는다 — 나오면 엔진 버그로 보고, SL §8.4 "실행").

## 4. 예산(LC5a·LC5b)

- **명령어 수:** 실행 조각을 시작할 때 예산(기본 1,000,000 — SL §9.3)을 받고 명령마다 1 줄인다(EXTRA 단어는 세지 않는다). 예산을 다 쓴 뒤의 명령은 실행하지 않고 그 명령 자리의 `BudgetExceeded`다(쓴 명령 수 = 예산). 동기 호출은 같은 예산을 나눈다. 계수는 VM·AOT가 같다(SL §12.5).
- **호출 깊이:** frame 256 초과는 `BudgetExceeded`(자리는 그 `CALL`).
- **메모리(LC5b):** 논리 크기(원소 수 × 원소 크기 + 머리) 합을 실행 문맥의 예산(`VmLimits::memoryBytes`, 기본 16 MiB — instance·session 예산은 LC6)과 비교한다. 단일 버퍼 상한 16,777,216 원소 또는 64 MiB(§2.2).
- 결과에 쓴 명령어 수를 돌려준다(실행 corpus가 계수 golden으로 본다).

## 5. dispatch

`switch` 기반 단일 루프(SL §11 — computed goto·tail call은 측정 뒤). 명령 하나는 32비트 단어를 읽어 opcode로 분기하고, EXTRA가 있는 명령은 다음 단어를 함께 읽는다. 모듈 로드 때 verifier를 통과한 모듈만 실행하므로(BC §5) dispatch는 범위 검사를 다시 하지 않는다(디버그 빌드는 assert).

## 6. host interface 1차(LC5d)

```text
class IVmHost {
  // 객체 handle 해석·수명: generation이 맞지 않으면 null로 본다(약한 참조 — SL §4.1).
  virtual bool IsAlive(uint64 handle);
  // native 함수·필드·문맥 이름·리터럴: 모듈 참조 표(BytecodeRef)의 이름으로 묶는다(LC4d 뒤에는 안정 ID).
  virtual Result<NativeBinding> Bind(const BytecodeRef& ref, StringView name);
  virtual VmFault CallNative(NativeBinding, VmWindow window);  // 인자는 창, 결과는 창 0번
  virtual VmFault GetField(NativeBinding, uint64 object, VmSlots out);
  virtual VmFault SetField(NativeBinding, uint64 object, VmSlotsView value);
  ...
};
```

- 모듈 로드 때 참조 표의 native 이름을 모두 묶는다(못 묶으면 로드 거부 — SL §12 "로드"). 실행 중에는 묶은 번호로 부른다.
- native 계약 위반은 `ContractViolation` fault다(호출자 입력으로 Fatal하지 않는다 — SL §8.4 native 경계).
- SparksTestHost는 가짜 host(엔진 schema의 객체·필드·함수를 메모리 표로 흉내)를 둔다.

## 7. 실행 corpus(`.run`)

- `Conformance/Run/<사례>.spark`와 `<사례>.run`. 최상위 함수만 부른다(LC6 전에는 instance가 없다). 줄 형식은 아래 "형식"이다.
- test host는 사례를 컴파일한 뒤(진단 없어야 함) `.sparkc` 왕복을 거친 모듈을 VM에 올려 호출하고, 실제 결과를 기대와 비교한다(다르면 실제 줄을 보인다 — `.ir`·`.bc`와 같은 golden 흐름).
- 실수 연산은 C++ 컴파일러의 FMA 축약을 끈 번역 단위에서 한다(`-ffp-contract=off` — 빌드 규칙(Rocks.Build ActionGraph)이 모든 번역 단위에 준다, SL §12.5).
- **형식:** `.run`은 UTF-8, 줄마다 호출 하나 `<함수>(<인자>, …) -> <결과> (<명령 수> steps)`, 빈 줄과 `#` 줄은 주석. 실행기는 `->` 앞(호출)만 읽어 실행하고 줄 전체를 실제 결과로 다시 써서 파일과 byte 단위로 비교한다(`->` 없는 줄도 호출로 읽는다 — 새 호출을 적고 golden을 만든다). 값 표기는 매개변수·반환 타입(Compilation의 시그니처)을 따른다: 정수 10진, `float`·`float32`는 최단 왕복 10진(`float32`는 float 값으로), `bool`은 `true`·`false`, 수학 타입은 `(1.0, 2.0, 3.0)`, Scalar `T?`는 `none` 또는 값, Scalar `Result<T>`는 `ok(값)`·`ok`(void)·`error(<case 번호>)`, enum은 case 이름, `void`. 결과는 그 값, `fault <종류> at <줄>:<열>`, `ended error(<case 번호>)`(ENDEXEC).

## 8. 결정 항목(권장안 — 2026-11-22 확정)

- **V1 값 표현:** Ref는 host handle(64비트), Buf는 참조계수 포인터. 대안(모든 값을 tagged union 한 bank)은 SL §11의 3 bank 결정과 어긋난다.
- **V2 새 frame 초기화:** 늘린 레지스터를 0·null로 채운다. 대안(초기화하지 않음)은 verifier의 "초기화 전 읽기" 검사(형식 버전 2) 전까지 결정성을 잃는다.
- **V3 fault 범위:** fault는 실행 문맥 전체를 끝낸다(LC5). instance 격리·Tick 중지 등 위치별 처리는 LC6.
- **V4 실행 corpus:** 최상위 함수만 호출. instance·hook이 필요한 시험은 LC6의 시나리오 golden.
- **V6 문자열 버퍼:** `LOADSTR`마다 새 버퍼. 대안(모듈 상수 문자열을 불변 공유 버퍼로 미리 만듦)은 할당이 줄지만 ledger·수명이 모듈 수명과 섞인다 — 측정 뒤 바꾼다.
- **V9 컨테이너 배치:** 첫 쓰기 명령의 표지에서 정한다. 대안(`NEWA`의 Type 참조에 배치를 실음)은 참조 표의 뜻이 늘어나고 형식 버전 2의 TYPE 구역과 겹친다 — 원소가 없는 컨테이너는 배치가 필요 없다.
- **V7 host의 Buf 값:** host는 `VmExecution::NewString`으로 만든 버퍼를 인자로 빌려 주고(호출이 참조를 더한다) 자기 참조는 `Release`로 놓는다. 반환된 Buf 값은 `ReturnedBuffers`로 읽고 다음 `Call`이나 `ReleaseReturned`가 놓는다.
- **V8 Scalar 인자 정규형:** `Call`의 Scalar 인자는 정규형(정수 확장, bool 0·1, float32 값, 유한 실수)을 전제로 한다. 검사는 LC5d host 경계가 함수 표의 매개변수 종류로 한다(`.run` 실행기는 표기를 읽을 때 범위·유한성을 본다).
- **V5 미구현 명령:** 하위 단계가 아직 구현하지 않은 명령(LC5a의 Buf·필드·컨테이너·호출 종류, LC6의 대기·이벤트)은 `ContractViolation` fault로 실행을 끝낸다(결정적 자리표시 — 그 단계에서 실제 의미로 바꾼다). `EQ.R`·`NE.R`·`ISSOME.R`은 LC5d 전까지 handle 원시 비교다(파괴된 객체를 null로 읽는 비교는 host `IsAlive`를 거친다).
