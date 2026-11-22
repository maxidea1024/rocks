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
| LC5c 표준 API | 부록 B의 언어 함수·속성(`CALLL` — §6a): Math·벡터·quat·Rotator·Transform·mat4·문자열·Format·Parse·Array·Map·Set·Result·Log·Assert·Random. 시간·대기(`Delay`·`NextTick`·`All`·`Race`)·에셋·Tag·TagSet·InputAction·Text·CVars·Settings·Class는 host가 필요하므로 LC5d·LC6 | 함수마다 양성·음성(fault) corpus |
| LC5d1 링크·host 기본 | 링크 단계(`VmLinkedModule` — 언어 함수·native·필드·문맥·리터럴 참조를 로드 때 모두 묶고 못 묶으면 거부), `VmHost`: 객체 handle 생존(`ISSOME.R`·`EQ.R`·`NE.R`), 필드(`GETF`·`SETF`), 문맥 이름(`GETCTX`), native 호출(`CALLN`), 리터럴(`NEWLIT`), 컴파일러의 native·필드 서술자 | SparksTestHost의 schema 기반 가짜 host로 `.run`의 객체 인자·host 호출 기록 golden, 링크 거부 시험 |
| LC5d2 가상·모듈 사이·엔진 타입 | `CALLV`(host가 대상 결정 — native 또는 Sparks 함수), 다른 모듈 함수(`CALL` ImportedFunction — 링크가 host의 모듈 표로 푼다, frame마다 모듈), `CAST`(`as?` — host 타입 판정), 엔진 struct의 `NEWS`(host 배치·기본값), 엔진 enum·`Name`·`Tag`의 `TOSTR`(host 이름 표), `TagSet`·`Text`의 0 값(null이 빈 값 — 6.5절) | 여러 모듈 사례·가상 호출·엔진 타입 corpus |

대기(`AWAIT`)·이벤트(`EMIT`·`AWAITEV`)·RPC 송신·유지 자원 해제(`RELEASE`)의 실행 의미는 LC6이다. LC5에서 VM은 이 명령을 만나면 `ContractViolation` fault로 실행을 끝낸다(LC6 전의 자리 — 결정적이다). 실행 corpus는 그 명령이 없는 동기 함수만 쓴다.

## 2. 값과 frame

### 2.1 bank 레지스터

- **Scalar:** `uint64` 원시 값(BC §2 표 — 정수는 그 타입 범위의 부호·0 확장, 실수는 double 비트, `float32`는 float로 반올림한 double).
- **Ref:** `uint64` handle = (index << 32) | generation. 0은 null. 해석·수명 검사는 host(§6)가 한다 — VM은 handle을 해석하지 않는다.
- **Buf:** `Buffer*`(§2.2) 또는 null. 레지스터 복사는 참조계수 증가, 덮어쓰기·frame 해제는 감소다(VM이 frame을 떠날 때 Buf 레지스터를 모두 놓는다).

### 2.2 값 버퍼(LC5b)

- `VmBuffer` 머리: 참조계수(u32), 종류(문자열·Array·Map·Set·struct·Result·TagSet), 원소 배치(bank·slot 수 — 컨테이너 표지와 같은 모양), 원소 수, 논리 크기(예산 계수용 byte). 내용은 머리 바로 뒤의 연속 배열이다(문자열은 UTF-8 byte, Array는 원소마다 slot 묶음 — Buf 원소는 `VmBuffer*`).
- **수명:** 레지스터 복사(`MOV.B`·창으로의 이동)는 참조계수 +1, 덮어쓰기는 이전 값 −1(0이면 해제 — 원소의 Buf도 재귀적으로 놓는다. 값 struct는 순환이 없으므로(S0426) 깊이는 타입 깊이로 유한하고, 해제는 명시 stack으로 돌아 재귀 깊이에 기대지 않는다). frame을 내릴 때(`RET`) 반환 값 밖의 Buf 레지스터를, fault·실행 끝에는 모든 frame의 Buf 레지스터를 놓는다. 해제는 실패하지 않는다(SL §9 소유 범위 연산).
- **ledger와 예산:** 실행 문맥마다 살아 있는 버퍼 수와 할당 byte를 센다(할당 +, 해제 −). 버퍼 하나는 정해진 머리 64 byte(`VmBufferHeaderBytes` — 플랫폼마다 실제 머리 크기가 달라도 같은 계수) + 8 byte 정렬한 내용(컨테이너는 용량만큼의 원소와 해시 색인)이다. AOT도 같은 머리 크기·증가 정책(아래 컨테이너)으로 센다(SL §12.5 — 계수는 VM·AOT가 같다). 새 버퍼가 메모리 예산(`VmLimits::memoryBytes` — 기본 16 MiB, SL §9.3 instance 예산의 LC5 자리)을 넘기거나 단일 버퍼가 16,777,216 원소 또는 64 MiB를 넘으면 그 명령 자리의 `BudgetExceeded`다(할당 위치를 보고 — SL §8.3). 할당자 실패(OS 메모리 부족)는 fault가 아니라 `Call`의 OutOfMemory다. 호출이 끝나 host가 반환 값을 놓으면 ledger는 0이어야 한다(시험이 확인한다).
- **문자열:** byte 열(UTF-8 — 컴파일러 출력과 host 입력이 UTF-8이다. VM은 다시 검사하지 않는다). `LOADSTR`는 상수 문자열의 새 버퍼를 만든다(실행마다 — 모듈 상수 버퍼 공유는 측정 뒤). `CONCAT A B C`는 창의 Buf B..B+C(조각 — 모두 문자열)를 이어 새 버퍼를 A에 둔다. 비교 `EQ.STR`·`LT.STR`…는 byte 순(SL §3.2), `string?`의 null은 null끼리만 같고 순서 비교에서 null은 어떤 문자열보다 작다(컴파일러는 `string?` 순서 비교를 내지 않지만 정해 둔다). `EQ.B`·`NE.B`는 문자열 값 비교다(컨테이너·struct의 `==`는 언어에 없다 — 아래).
- **0 값의 남은 자리:** `TagSet`·`Text`의 0 값은 null 버퍼(빈 값 — 6.5절), 엔진 struct 필드의 schema 기본값은 host가 `NEWS` 때 채운다(LC5d2).
- **`TOSTR` 표기(SL §3.2 보간):** `bool`은 `true`·`false`, 정수는 10진, 실수는 다시 읽으면 같은 값이 되는 가장 짧은 10진(Core 서식 — `float32`는 float 값으로)이고 고정 소수점 표기에 소수점이 없으면 `.0`을 붙인다(`1.0`·`-0.0`·`1e+38`), 수학 타입은 성분을 `, `로 이은 괄호(`(1.0, 2.0, 3.0)` — `Transform`은 Location·Rotation·Scale 묶음 셋의 괄호), 이름·`Tag`는 LC5d(host의 이름 표), enum은 LC5b3(TYPE 구역의 case 이름 — Sparks enum과 언어 enum `Clock`, 엔진 enum은 host 이름 표라 LC5d까지 `ContractViolation`).
- **COW:** 쓰는 명령(`SETE`·`SETSF`·`APPEND`·`PUT`·`CALL.MUT`)은 참조계수가 1보다 크면 먼저 복제하고 그 레지스터를 새 버퍼로 바꾼다(이전 버퍼는 감소). 순환은 생기지 않는다(closure·참조형 값이 없다 — SL §4.1).
- **컨테이너(LC5b2):** 원소 배열은 삽입 순서이고 원소 하나는 key slot 묶음 뒤 값 slot 묶음이다(Array는 값만, Set은 원소를 key 자리에, Map은 둘 다). Buf slot은 원소 버퍼의 참조 하나를 가진다. 배치(bank·slot 수)는 명령의 컨테이너 표지에서 정하며 처음 원소를 넣는 명령(`APPEND`·`PUT`·`SETE`)이 버퍼에 적는다 — 빈 컨테이너는 배치가 없어도 된다(읽기는 없음·0을 돌려준다). Map·Set은 원소 배열 뒤에 열린 주소 해시 색인(원소 번호 + 1, 크기는 용량 2배 이상의 2의 거듭제곱)을 두고 key는 Core `HashStringBytes`(문자열 byte)·`MixHash`(slot 비트)로 흩는다 — 순회 순서는 색인과 무관하게 삽입 순이다(SL §6.3 결정적 순회). 용량은 2배로 늘리되(처음 4) 단일 버퍼 상한(원소 수·byte — 색인 포함)에 들어가는 만큼으로 자르고, 필요한 원소 수가 그 상한을 넘을 때만 `BudgetExceeded`다. 늘릴 때 원소는 옮기고(참조계수 그대로) 색인은 다시 만든다. 해시는 씨앗이 없다: 미리 만든 충돌 key가 들어오면 찾기 하나가 O(n)이고 명령어 예산은 이를 1로 센다(알려진 한계 — 실행 문맥 씨앗이나 탐사 수 계수는 버퍼가 실행 문맥 사이를 오가는 LC6에서 정한다).
- **컨테이너 명령:** `NEWA`·`NEWSET`·`NEWM`은 빈 버퍼. `APPEND A B`는 Array 끝에 넣고 Set은 이미 있으면 넣지 않는다. `PUT A B C`·Map의 `SETE A B C`는 key B의 값을 C로 바꾸거나 끝에 넣는다(SL §4 "Map의 `m[k] = v`는 삽입 또는 교체"). Array의 `SETE`는 번호가 범위 밖(음수 포함)이면 `IndexOutOfRange`. `GETE A B C`는 `T?`다: Array 번호가 범위 밖이거나 Map에 key가 없으면 없음(Scalar는 표지 0과 0 slot, Ref 0, Buf null). `LEN A B`는 원소 수(null은 0). `GETKEY`·`GETVAL A B C`는 삽입 순 C번 원소의 key·값(Array는 값)이고 범위 밖이면 `IndexOutOfRange`다(컴파일러는 `LEN` 아래 번호만 낸다). 쓰는 명령은 COW다(공유된 버퍼는 먼저 복제 — 원소 버퍼는 참조 +1).
- **실행 중 배치 대조(LC5b 리뷰):** 표지·필드 참조와 실제 버퍼 모양은 정적 verifier가 볼 수 없으므로 VM이 명령마다 본다: 컨테이너는 배치가 정해진 버퍼에서 표지의 묶음(bank·slot)이 버퍼 배치와 같아야 하고(`GETVAL`은 Set에, `GETKEY`는 Array에 쓸 수 없다, Array 번호는 Scalar 1 slot), struct 필드는 버퍼의 자기 bank 구역 안(Buf 필드는 끝의 Buf slot, 그 밖은 앞쪽)이어야 한다. 어긋나면 `ContractViolation`이다 — 버퍼 밖 읽기·쓰기나 값 비트를 포인터로 쓰는 일이 없다. verifier는 Ref·Buf 묶음(표지·`TRY.B`·`OK`)을 slot 하나로 제한한다.
- **struct(LC5b3):** `NEWS A Bx`는 TYPE의 struct 배치(bank별 slot 수)로 모든 slot이 0·null인 버퍼를 만든다(기본값은 컴파일러가 `SETSF`로 채운다 — SparksBytecode 5절 TYPE 구역). TYPE이 없는 Type 참조(엔진 schema struct·`TagSet` — host 배치, LC5d)의 `NEWS`는 `ContractViolation` 자리표시다. `GETSF A B C`는 struct B의 필드를 A로(Buf는 참조 +1), `SETSF A B C`는 struct A의 필드를 B로 바꾼다(COW — 공유된 struct는 먼저 복제). null struct(`T?`의 없음)에 대한 필드 명령은 `ContractViolation`이다(컴파일러는 꺼낸 뒤에만 낸다).
- **Buf `Result`(LC5b3):** `Result<T>`의 T가 Scalar 밖(Buf·Ref)이면 버퍼 하나다: slot 0이 오류 코드(0 성공, 아니면 `Error` case + 1), 그 뒤가 값(값 bank·slot은 버퍼 머리 — Buf 값은 원소 버퍼). `OK A B`는 값 B로 성공, `ERR A B`는 Scalar B의 `Error` case로 실패(값 없음), `TRY.B`는 성공이면 값을 A로 아니면 실패 블록, `RETERR.B`·`RETERR.S`는 받은 오류를 이 함수의 반환 모양(Scalar Result 묶음이나 Buf Result)으로 돌려주고, `ENDEXEC.B`는 오류 코드로 실행을 끝낸다.
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

## 6a. 언어 함수(LC5c)

- **호출 모양:** 부록 B의 함수·메서드·속성(`string.Length`·`Array.Count`·`Result.Value` 같은 언어 필드 읽기 포함)은 모두 `CALLL Bx`다(속성은 수신자 하나의 함수). 인자는 일반 호출처럼 bank별 창에 놓고(수신자 먼저) 결과는 창 0번부터 받는다. 값을 바꾸는 메서드(`Array.Add` 등 — 컴파일러의 `CALL.MUT`)는 바뀐 수신자를 그 bank 창 0번에 돌려주고 같은 bank의 결과는 그 뒤에 둔다(SparksBytecode 4.3절). 상수(`Math.Pi`)는 컴파일러가 접는다.
- **참조와 서술자:** `LanguageFunction` 참조의 이름은 `<주인>.<멤버>`(`Math.Min`·`string.Slice`·`Array.Add`, 최상위 prelude 함수는 이름만 — `Assert`), 리터럴 인자 표 자리(`arguments`)는 피연산자 서술자 문자열이다: `<타입>,<타입>,…><결과 타입>`(수신자 먼저, 결과가 없으면 `void`). 타입 글자: `b` bool, `i8` `i16` `i32` `i64` `u8` `u16` `u32` `u64` `f32` `f64`, `e` enum, `n` 이름(Tag), `v2` `v3` `v4` `q` `r`(Rotator) `c`(Color) `t`(Transform) `m`(mat4), `s` string, `x` 객체·handle(Ref), `st` struct, `A<…>` `M<…,…>` `S<…>` 컨테이너, `R<…>` Result, `?<…>` 선택. 예: int8 `Math.Min`은 `i8,i8>i8`, `Array<vec3>.Add`는 `A<v3>,v3>void`. 오버로드(`$N` 등)와 원소 타입이 서술자로 정해지므로 VM은 이름과 서술자로 구현을 고르고 창의 bank·slot을 계산한다.
- **묶기:** 링크(§6.1)가 모듈의 `LanguageFunction` 참조를 모두 묶는다(이름 → 구현 표, 서술자 → 피연산자 bank·slot·Scalar 종류). 구현마다 받는 서술자 모양(패턴 — 서술자 문법에 자리표시 `I` 정수·`F` 실수·`N` 숫자·`V` 벡터·`O` 정렬 가능(정수·실수·문자열·enum)·`T`·`U` 아무 타입, 같은 글자는 같은 타입)이 있고 이름과 모양이 모두 맞는 구현을 고른다. 서술자 노드의 slot 수는 32 이하이고(Optional·Result 묶음 포함) `z`의 Ref·Buf는 1 slot이다. `CALLL`의 창(할당 수부터 frame 끝)이 피연산자와 결과(값을 바꾸는 메서드는 수신자 뒤)를 담는지도 본다. 모르는 이름·모양·상한 위반·창 부족은 링크 거부다(손상된 `.sparkc`가 구현의 고정 배열이나 창 밖을 건드리지 않게 — LC5c 리뷰). 대기·host 언어 함수(`Delay`·`NextTick`·`NextFixedTick`·`All`·`Race`, `Time.`·`Assets.`·`Tag.`·`TagSet.`·`InputAction.`·`Text.`·`CVars.`·`Settings.`·`Class.` 등)는 묶음 없이 링크하고 부르면 `ContractViolation`이다(LC6·연동 — V5).
- **의미와 fault:** 부록 B 표를 따른다(SL §17). 정수 `Math.Abs`·`Math.Div`·`*ToInt`의 범위 밖은 `IntegerOverflow`, `Math.Div`·`InverseLerp`의 0은 `DivideByZero`, 실수 결과의 NaN·Inf는 `NonFinite`, `Math.Clamp`의 `lo > hi`·`Assert` 실패·`Format.Fixed`의 `decimals` 범위 밖은 `AssertFailed`, `string.Slice`의 범위 밖·code point 경계 아님과 `Array.Insert`·`RemoveAt`의 범위 밖은 `IndexOutOfRange`다. 초월 함수는 Core Math 함수를 부른다(VM·AOT 같은 함수 — SL §12.5). `Random`은 실행 문맥의 결정적 생성기(Core `RandomStream` — 씨앗은 `VmLimits::randomSeed`, LC6이 instance·session에서 정한다)다. `Log`는 host 로그 대신 실행 문맥의 기록 목록에 쌓는다(LC5d가 host 로그로 잇는다 — `.run`은 결과 줄 뒤에 기록을 보인다). 기록 할당이 실패하면 그 기록은 빠지고 실행은 계속된다(로그는 결과를 바꾸지 않는다) — 빠진 수는 `VmExecution::DroppedLogs`로 보이고 `.run` 실행기는 이를 할당 실패로 돌려준다(golden이 거짓으로 달라지지 않게).
- **세부 의미(부록 B가 정하지 않은 자리 — LC5c 확정):** `Format.Fixed`는 이진 값을 정확히 반올림하고 동률은 짝수 쪽이다(C `printf("%.*f")`·UE `FString::Printf`와 같음 — `2.5`는 `"2"`, `-0.5`는 `"-0"`). `Format.Padded`는 부호를 포함한 폭이 되게 앞을 0으로 채우고(`-42`·5 → `"-0042"`) 폭은 0–32, 밖은 `AssertFailed`다. `string.Split`의 빈 구분자는 code point마다 나누고 빈 조각을 남긴다(Go `strings.Split` — `""`는 `[""]`). `string.Find`·`Contains`의 빈 찾기 글자는 0·true다. `Parse.Int`는 부호(`+`·`-`) 선택 뒤 10진 숫자만(`_`·공백·16진 없음, int 범위 밖은 없음), `Parse.Float`는 부호 선택 뒤 `숫자[.숫자]`·`.숫자`와 지수 선택(`1.`·`inf`·`nan`·유한하지 않은 값은 없음)이다 — Core 파서보다 좁으므로 문법을 먼저 본다. `Array.Sort`는 오름차순이다(정수·실수 값 순, 문자열 byte 순, enum case 번호 순 — 원소가 slot 하나인 값뿐이라 같은 값끼리 구분할 수 없으므로 안정성은 결과에 드러나지 않는다). `Array.Remove`는 처음 같은 원소 하나를 지운다. `Map.Keys`·`Values`는 삽입 순의 새 Array다. 원소를 지운 Map·Set은 뒤 원소를 당기고 색인을 다시 만든다(삽입 순 유지 — 지운 key를 다시 넣으면 끝에 붙는다). `Random.Int`는 64비트 출력의 거부 표본으로 `[min, maxExclusive)`에서 균등하게 고르고, `Random.Float`는 `min + (max − min) × u`(`u`는 `[0, 1)` 53비트 — `float32` 결과는 float로 반올림하므로 `max`가 나올 수 있다), `min > max`는 `AssertFailed`다. `Random.Chance(p)`는 `u < p`(0 이하는 늘 false, 1 이상은 늘 true). `Result.Value`·`Error`는 성공이면 값·없음, 실패면 없음·case다. 컨테이너 메서드는 수신자 버퍼의 종류와(배치가 정해졌으면) 배치를 서술자와 대조하고 다르면 `ContractViolation`이다(host가 다른 배치의 컨테이너를 넘겨도 버퍼 밖을 쓰지 않는다). `Clear`는 원소 버퍼를 한꺼번에 놓고 색인을 한 번 비우며, `Set.Add`는 새 원소 하나만 색인에 넣는다(O(1) 평균). 객체 원소의 `Contains`·`IndexOf`·`Remove`는 죽은 handle을 null로 비교한다(Map·Set의 객체 key는 handle 비트로 흩으므로 죽은 handle도 다른 key로 남는다 — 알려진 한계, 객체 key 정리는 LC6). `Log` 글자는 실행 하나에 1 MiB까지이고 넘는 기록은 빠진 기록이다. `Assert` 실패는 message를 `error` 수준 로그로 남긴 뒤 fault를 낸다.

## 6. host interface(LC5d)

### 6.1 링크(LC5d1)

- **링크 단계:** `LinkModule(module, host, out, reason)`가 verifier를 통과한 모듈의 참조 표를 처음부터 끝까지 한 번 묶어 `VmLinkedModule`(모듈 + 참조 번호별 묶음)을 만든다. `VmExecution::Call`은 링크에 성공한 모듈만 받는다(실패했거나 링크하지 않은 `VmLinkedModule`은 InvalidArgument). 못 묶은 참조가 하나라도 있으면 모듈 전체를 거부하고 `VmLinkError`(참조 번호·종류·이름·사유)를 돌려준다(SL §12 "로드" — 실행 중에 처음 부를 때 발견하는 일이 없다). 언어 함수 묶기(§6a)도 여기로 옮긴다(실행 문맥의 첫 호출 묶기 cache는 없앤다 — 모르는 이름·서술자는 실행 중 `ContractViolation`이 아니라 링크 거부다).
- **묶는 참조:** `LanguageFunction`(VM 구현 표), `NativeFunction`·`Field`·`Context`·`Literal`(host `Bind`), `Function`(같은 모듈 — verifier가 이미 봄), `StructField`·TYPE이 있는 `Type`(TYPE 구역 — 묶을 것 없음). 그 밖(`ImportedFunction`·`VirtualFunction`·TYPE 없는 `Type`·`Event`·`RpcFunction`)은 LC5d2·LC6이 묶을 때까지 묶음 없음으로 두고, 그 명령은 지금처럼 실행 중 `ContractViolation`이다(V5).
- **서술자 대조:** 컴파일러는 `NativeFunction` 참조의 `arguments`에 언어 함수와 같은 문법의 피연산자 서술자(§6a — 수신자 먼저, 결과)를, `Field` 참조에는 필드 타입 하나의 서술자(`>v3` — 결과 자리에 필드 타입)를 싣는다. host는 자기 schema의 시그니처를 같은 문법으로 만들어 글자로 비교하고 다르면 묶기를 거부한다(schema와 모듈이 어긋나면 로드에서 막는다 — BC §5의 "native 시그니처 hash"를 대신한다. 글자 비교라 진단이 읽힌다).
- **필드 명령 대조:** 링크는 함수 명령을 한 번 훑어 `GETF`·`SETF`의 bank·slot 수(C)가 묶은 필드의 배치와 같은지 본다(다르면 거부). 실행 중에는 다시 보지 않는다.
- **host 묶음:** `VmHost::Bind(const VmBindRequest&)`는 요청(참조 종류, 이름, 서술자, 리터럴 글자·이름 지정 인자 표)을 받아 `VmHostBinding`(host가 정하는 64비트 번호, 필드·리터럴·문맥 결과의 bank·slot 수)을 돌려준다. 실패는 `Result` 오류와 사유 글자다. 같은 host로 링크한 모듈만 그 host로 실행한다(실행 문맥은 링크된 모듈의 host를 쓴다).

### 6.2 실행 중 host 호출(LC5d1)

```text
class VmHost {
  virtual Result<VmHostBinding> Bind(const VmBindRequest& request, StringBuilderBase& reason) = 0;
  virtual bool IsAlive(uint64 handle) = 0;                       // 0은 부르지 않는다
  virtual VmFaultKind GetContext(VmHostCall& call) = 0;          // Ref 결과 하나
  virtual VmFaultKind GetField(VmHostCall& call, uint64 object) = 0;
  virtual VmFaultKind SetField(VmHostCall& call, uint64 object) = 0;
  virtual VmFaultKind CallNative(VmHostCall& call) = 0;
  virtual VmFaultKind MakeLiteral(VmHostCall& call) = 0;
};
```

- **`VmHostCall`:** 묶음 번호, bank별 창(시작 포인터와 크기 — `CALLN`·`NEWLIT`은 frame base + `registers[bank]`부터 frame 끝까지, `GETF`·`SETF`·`GETCTX`는 대상 레지스터 묶음), 실행 문맥(버퍼를 만들고 놓는 도구 — `NewString`·`NewContainer`·`Retain`·`Release`·`Put`), 할당자 오류 자리. 결과는 결과 bank 창 0번부터다(§6a와 같은 규칙 — 값을 바꾸는 수신자는 없다). Buf 결과는 host가 만든 버퍼의 참조를 넘겨 `Put`으로 둔다(이전 값은 놓는다). host는 창 크기 밖을 쓰지 않는다(넘으면 `ContractViolation`을 돌려준다).
- **반환:** `VmFaultKind::None`이 성공이다. native 계약 위반은 `ContractViolation`(SL §8.4 — Fatal 금지), 할당자 실패는 `call.error = OutOfMemory`(실행은 `Call`의 OutOfMemory로 끝난다). 그 밖의 fault 종류도 돌려줄 수 있다(`IndexOutOfRange` 등 — 위치는 그 명령).
- **handle 생존(SL §4.1 약한 참조):** `ISSOME.R`은 0이 아니고 `IsAlive`인 handle만 있음이다(host 없이 링크한 모듈에서는 모든 handle이 없음으로 읽힌다 — 객체는 host만 만든다). `EQ.R`·`NE.R`은 죽은 handle을 0으로 바꿔 비교한다(파괴된 객체는 null과 같다). 레지스터의 handle 비트는 바꾸지 않는다(읽을 때 판정).
- **`GETF`·`SETF`:** 대상 객체가 0이거나 죽었으면 `ContractViolation`이다(컴파일러는 꺼낸 참조에만 내고 파괴는 단계 끝까지 미뤄지므로 동기 실행 안에서는 생기지 않는다 — 생기면 host·컴파일러 버그다). `GETF`의 결과 묶음은 레지스터 A부터(Buf는 host가 넘긴 참조, Ref는 handle — 생존은 쓸 때 판정), `SETF`의 값은 레지스터 B부터 읽기 전용으로 보인다. host가 Buf 값을 보관하면 `Retain`으로 참조를 더하고 바꾸거나 객체를 없앨 때 `Release`한다(그 버퍼는 실행 문맥 ledger에 살아 있는 것으로 남는다 — 아래 V12). `Changed` 후보 기록·`@net` 권한 검사는 host 일이다(LC6·EI7).
- **`GETCTX A Bx`:** host가 문맥 객체 handle을 Ref A에 둔다(`World`·`Time`… — 늘 살아 있는 객체).
- **`CALLN Bx`:** 인자는 bank별 창(수신자 먼저 — Ref 창 0번), 결과는 결과 bank 창 0번부터. 링크는 native 서술자의 피연산자·결과가 창에 들어가는지 본다(`CALLL`과 같은 규칙). 수신자의 생존·non-null 같은 schema 전제조건은 host thunk가 보고 위반은 `ContractViolation`이다(SL §8.4). 대기를 만드는 native(`async`)는 LC6이다(지금 링크는 받되 결과가 대기 handle `w`면 실행 중 `ContractViolation`).
- **`NEWLIT A`:** 결과는 레지스터 A(결과 bank는 EXTRA — 링크가 묶음의 bank와 같은지, A부터 결과 묶음이 frame 안인지, 이름 지정 인자가 있으면 창이 결과를 담는지 본다)다. 이름 지정 인자가 없으면 host는 레지스터 A 묶음을 결과 창으로 받고, 있으면 인자는 호출 창에 있고 결과를 결과 bank 창 0번에 두면 VM이 A로 옮긴다. host는 리터럴 글자를 묶을 때 미리 검사·변환해 둘 수 있다(`Tag`·`Soft<T>`의 이름).

### 6.3 SparksTestHost의 가짜 host(LC5d1)

- schema(`*.sparkbind.json`)로 만든다: 객체는 번호(`#1`…)와 타입 이름, generation, 필드 값 표다. 묶기는 schema의 함수·필드·문맥·리터럴 이름과 서술자를 대조한다(못 찾으면 거부).
- 필드: 처음 읽으면 0 값이다. 단 non-null 객체 타입 필드(`World.Audio`)는 처음 읽을 때 그 타입의 새 객체를 만든다(엔진의 하위 객체처럼 — 결정적 번호). 쓴 값은 사례 끝까지 남는다(`.run` 줄 사이에도 — host 상태는 사례 하나에 하나).
- native: 이름별 일반 동작 — 객체(또는 `T?` 객체) 결과는 그 타입의 새 객체, 숫자·bool은 0 값, 문자열은 빈 글자, `void`는 없음. `RequestDestroy`는 수신자를 바로 죽인다(generation 증가 — 파괴 지연은 LC6이 World bridge에서 한다. 여기서는 생존 판정을 시험한다). 호출마다 `  host <이름>(<인자>) -> <결과>` 기록을 남긴다.
- 문맥 이름은 사례마다 하나의 객체(처음 쓸 때 만든다). 리터럴 `Tag`·`Soft<T>`는 글자를 이름 표에 넣고 그 번호(Scalar)를 돌려준다(빈 글자는 거부).

### 6.5 모듈 사이·가상 호출과 엔진 타입(LC5d2)

- **모듈 사이 호출:** `LinkModule`은 `ImportedFunction` 참조를 묶지 않고 둔다. 한 실행 단위의 모듈을 모두 링크한 뒤 `LinkImports(modules, reason)`가 참조 이름(`<import 경로>:Owner.Name`)의 경로로 모듈(`module.path`)을, 이름으로 함수를 찾아 (링크된 모듈, 함수 번호)로 묶는다(순환 import가 있어도 된다 — 모두 링크한 뒤 푼다). 컴파일러는 `ImportedFunction` 참조에도 서술자(같은 문법 — 수신자 `x` 먼저)를 싣고 링크는 대상 함수의 함수 표 모양(bank별 매개변수 수·반환 bank·slot 수)과 대조한다(어긋나면 거부). 풀지 못한 import가 있는 모듈은 `Call`이 InvalidArgument로 막는다(import를 푼 뒤에만 실행).
- **frame의 모듈:** frame은 자기 링크된 모듈을 가진다. 다른 모듈 함수를 부르면 그 모듈의 명령·상수·참조 표로 바꿔 실행하고 `RET`이 부른 쪽 모듈로 돌아온다. 창 규칙은 같은 모듈 `CALL`과 같다(부르는 쪽 frame의 할당 수 위 — 대상 함수의 매개변수가 창에 들어가는지는 실행 중에 본다: 들어가지 않으면 `ContractViolation`).
- **`CALLV Bx`:** `VirtualFunction` 참조(이름 `Owner.Method`, 서술자)는 host가 묶는다. 실행 중 VM은 수신자(Ref 창 0번)를 `VmHost::ResolveVirtual`에 넘기고 host는 대상을 돌려준다: native(묶음 번호 — `CallNative`처럼 부른다) 또는 Sparks 함수(링크된 모듈, 함수 번호 — `CALL`처럼 frame을 만든다). 수신자가 죽었거나 0이면 `ContractViolation`(컴파일러는 꺼낸 참조에만 낸다). host는 Sparks 클래스의 override 표(클래스 → 메서드 → 함수)를 가진다(EI2 ScriptRuntime이 타입 등록으로 만든다 — 가짜 host는 Compilation의 클래스·기반 사슬에서 만든다).
- **`CAST A B` + Type:** TYPE이 없는 Type 참조(엔진 타입·Sparks 클래스·interface)는 host가 묶고, 실행 중 `VmHost::IsA(handle, 묶음)`이 참이면 A ← B, 아니면 0이다(죽은 handle은 0).
- **엔진 struct:** TYPE이 없는 struct Type 참조는 host가 bank별 slot 수를 돌려주고(`VmHostBinding::structSlots`), `NEWS`는 그 배치의 0·null 버퍼를 만든 뒤 `VmHost::InitializeStruct`가 schema 기본값을 채운다(창은 버퍼 안 bank별 구역). 엔진 struct의 `StructField` 참조(보조 번호 없음)는 host가 (bank, slot 수, bank 구역 안 위치)로 묶고 `GETSF`·`SETSF`는 그 위치를 쓴다(링크가 필드 명령의 bank·slot 수를 대조).
- **이름 표기:** 엔진 enum의 `TOSTR`(TYPE 없는 Type 참조)·`Name`·`Tag`의 `TOSTR`는 `VmHost::AppendName(묶음 또는 0, 값, out)`이 글자를 붙인다(이름 표는 host — Tag는 이름 번호, 엔진 enum은 case 번호).
- **묶기 범위:** TYPE 없는 Type·StructField 참조는 host가 있을 때만 묶는다(컨테이너 원소 타입처럼 실행 중 host가 필요 없는 Type 참조도 있다 — host 없이 링크하면 그 참조를 쓰는 `NEWS`·`CAST`·`TOSTR`·`GETSF`는 실행 중 `ContractViolation`, `CAST`는 없음). `VirtualFunction`은 host가 없으면 링크 거부다. 엔진 struct의 필드 명령은 링크가 host 필드 배치(bank·slot 수)와 대조한다. 엔진 struct의 Buf 필드는 null로 시작한다(schema 기본값의 Buf 필드는 연동 EI5).
- **`TagSet()`:** 언어 타입 `TagSet`의 생성(`NEWS` — Type 이름 `TagSet`)은 null(빈 값)을 둔다.
- **`TagSet`·`Text`의 0 값:** null 버퍼가 빈 값이다(SL §3.2 "빈 TagSet·빈 Text") — 그 타입의 연산(LC6·EI5 host 함수)이 null을 빈 값으로 받는다. 따로 만들지 않는다.

### 6.4 결정(LC5d — 권장안 확정)

- **V12 host가 보관한 버퍼:** 버퍼는 만든 실행 문맥의 ledger에 남는다(host가 Retain한 동안 살아 있음 — 원소 버퍼도). `.run` 실행기는 host가 버퍼를 쥐지 않은 줄에서는 ledger 0을, 쥔 줄에서는 "살아 있는 버퍼 ≥ 쥔 버퍼 수"를 보고, 사례 끝에 host가 모두 놓은 뒤 0을 본다. 실행 문맥 사이로 버퍼를 옮기는 규칙(instance 필드가 여러 실행에 걸쳐 사는 경우)은 LC6이 정한다. 대안(host가 보관할 때 깊은 복사)은 큰 컨테이너 필드마다 복사 비용이 들고 COW 공유의 이점을 잃는다.
- **V13 링크 시 전부 묶기:** 대안(처음 실행할 때 묶기)은 드문 경로의 schema 불일치를 늦게 발견한다 — SL §12의 "로드 거부"와 어긋난다.
- **V14 서술자 글자 대조:** 대안(시그니처 hash)은 작지만 불일치 사유를 보일 수 없다. 서술자는 모듈 문자열 표에 한 번 들어가므로 크기 부담이 작다.
- **V15 죽은 handle 판정:** 읽을 때(`ISSOME.R`·`EQ.R`) host에 묻는다. 대안(레지스터에 넣을 때 0으로 바꿈)은 같은 실행 안에서 파괴가 일어날 때(LC6 이후 즉시 파괴 경로) 이미 레지스터에 있는 handle을 놓친다.

## 7. 실행 corpus(`.run`)

- `Conformance/Run/<사례>.spark`와 `<사례>.run`. 최상위 함수만 부른다(LC6 전에는 instance가 없다). 줄 형식은 아래 "형식"이다.
- test host는 사례를 컴파일한 뒤(진단 없어야 함) `.sparkc` 왕복을 거친 모듈을 VM에 올려 호출하고, 실제 결과를 기대와 비교한다(다르면 실제 줄을 보인다 — `.ir`·`.bc`와 같은 golden 흐름).
- 실수 연산은 C++ 컴파일러의 FMA 축약을 끈 번역 단위에서 한다(`-ffp-contract=off` — 빌드 규칙(Rocks.Build ActionGraph)이 모든 번역 단위에 준다, SL §12.5).
- **형식:** `.run`은 UTF-8, 줄마다 호출 하나 `<함수>(<인자>, …) -> <결과> (<명령 수> steps)`, 빈 줄과 `#` 줄은 주석. 실행기는 `->` 앞(호출)만 읽어 실행하고 줄 전체를 실제 결과로 다시 써서 파일과 byte 단위로 비교한다(`->` 없는 줄도 호출로 읽는다 — 새 호출을 적고 golden을 만든다). 값 표기는 매개변수·반환 타입(Compilation의 시그니처)을 따른다: 정수 10진, `float`·`float32`는 최단 왕복 10진(`float32`는 float 값으로), `bool`은 `true`·`false`, 수학 타입은 `(1.0, 2.0, 3.0)`, Scalar `T?`는 `none` 또는 값, Scalar `Result<T>`는 `ok(값)`·`ok`(void)·`error(<case 번호>)`, enum은 case 이름, `void`. 결과는 그 값, `fault <종류> at <줄>:<열>`, `ended error(<case 번호>)`(ENDEXEC). `Class.Method(#this, …)`는 클래스 메서드를 가짜 host 객체 `#this`를 수신자로 부른다(최상위 함수는 객체를 바꾸거나 상태를 바꾸는 엔진 함수를 부를 수 없으므로 host 경로는 메서드로 본다 — instance의 필드 기본값·수명은 LC6, 가짜 host의 필드는 0에서 시작한다). 객체 인자·결과는 `#n`(죽은 handle은 `dead#n`, 없음은 `none`)이고, native 호출은 결과 줄 뒤에 `  host <이름>(<인자>) -> <결과>` 줄로 보인다(실행기가 다시 쓴다). 인자 자리의 문자열·컨테이너는 결과 표기와 같다(Array `[a, b]`, Set `{a, b}`, Map `{k: v}` — 원소는 원소 타입대로, 중첩 가능, host가 `VmExecution::NewContainer`로 만든다). 호출이 남긴 `Log.*` 기록은 결과 줄 바로 뒤에 `  log <수준> "<메시지>"`(수준 `info`·`warning`·`error`·`debug`, 메시지는 문자열 표기의 escape) 줄로 하나씩 보인다 — 실행기는 이 줄을 읽지 않고 다시 쓴다.

## 8. 결정 항목(권장안 — 2026-11-22 확정)

- **V1 값 표현:** Ref는 host handle(64비트), Buf는 참조계수 포인터. 대안(모든 값을 tagged union 한 bank)은 SL §11의 3 bank 결정과 어긋난다.
- **V2 새 frame 초기화:** 늘린 레지스터를 0·null로 채운다. 대안(초기화하지 않음)은 verifier의 "초기화 전 읽기" 검사(형식 버전 2) 전까지 결정성을 잃는다.
- **V3 fault 범위:** fault는 실행 문맥 전체를 끝낸다(LC5). instance 격리·Tick 중지 등 위치별 처리는 LC6.
- **V4 실행 corpus:** 최상위 함수만 호출. instance·hook이 필요한 시험은 LC6의 시나리오 golden.
- **V6 문자열 버퍼:** `LOADSTR`마다 새 버퍼. 대안(모듈 상수 문자열을 불변 공유 버퍼로 미리 만듦)은 할당이 줄지만 ledger·수명이 모듈 수명과 섞인다 — 측정 뒤 바꾼다.
- **V10 struct 기본값:** 생성 자리에서 컴파일러가 내린다. 대안(TYPE 구역에 상수 원형을 두고 `NEWS`가 복사)은 상수 struct·컨테이너 직렬화 형식이 더 필요하고, 대안(struct마다 기본값 함수를 두고 부름)은 다른 모듈 함수 호출(LC5d)에 묶인다.
- **V9 컨테이너 배치:** 첫 쓰기 명령의 표지에서 정한다. 대안(`NEWA`의 Type 참조에 배치를 실음)은 참조 표의 뜻이 늘어나고 형식 버전 2의 TYPE 구역과 겹친다 — 원소가 없는 컨테이너는 배치가 필요 없다.
- **V7 host의 Buf 값:** host는 `VmExecution::NewString`·`NewContainer`(원소 slot 묶음과 배치 표지 — 같은 key는 InvalidArgument, Buf 원소는 빌려 준다)로 만든 버퍼를 인자로 빌려 주고(호출이 참조를 더한다) 자기 참조는 `Release`로 놓는다. 반환된 Buf 값은 `ReturnedBuffers`로 읽고 다음 `Call`이나 `ReleaseReturned`가 놓는다.
- **V8 Scalar 인자 정규형:** `Call`의 Scalar 인자는 정규형(정수 확장, bool 0·1, float32 값, 유한 실수)을 전제로 한다. 검사는 LC5d host 경계가 함수 표의 매개변수 종류로 한다(`.run` 실행기는 표기를 읽을 때 범위·유한성을 본다).
- **V11 언어 함수 묶기:** 서술자를 참조 표에 싣고 실행 문맥이 처음 부를 때 묶는다. 대안(언어 함수마다 opcode)은 opcode 표가 수백 개로 늘고 형식 버전이 함수 추가마다 바뀐다. 대안(이름에 타입을 붙여 구현 표를 모든 조합으로 둠)은 오버로드·원소 타입 조합이 폭발한다.
- **V5 미구현 명령:** 하위 단계가 아직 구현하지 않은 명령(LC5a의 Buf·필드·컨테이너·호출 종류, LC6의 대기·이벤트)은 `ContractViolation` fault로 실행을 끝낸다(결정적 자리표시 — 그 단계에서 실제 의미로 바꾼다). `EQ.R`·`NE.R`·`ISSOME.R`은 LC5d 전까지 handle 원시 비교다(파괴된 객체를 null로 읽는 비교는 host `IsAlive`를 거친다).
