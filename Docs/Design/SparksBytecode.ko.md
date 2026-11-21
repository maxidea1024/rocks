# Sparks typed IR·bytecode·.sparkc

기준일: 2026-11-22 · 개정 2(LC4a 구현 반영) · 상위 문서: [Sparks 언어](SparksLanguage.ko.md)(이하 SL) §11·§12.1–§12.3·18.2절 LC4

이 문서는 언어 코어 LC4(typed IR, bank 할당, 32비트 명령, `.sparkc`, verifier, 안정 ID, interface·body hash, 덤프)의 설계 정본이다. 실행(interpreter·값 버퍼·fault)은 LC5, 실행 모델(instance·hook·큐·async)은 LC6이 이 형식을 읽는다. SL은 요구(레지스터 VM, 3 bank, 타입별 opcode, 32비트 고정 명령, verifier, 결정적 덤프)를 정하고, 이 문서는 그 형식을 정한다.

## 1. 하위 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| LC4a typed IR | 검사한 구문 트리 → typed IR(값 표현 분해, 제어 흐름, 호출·필드·이벤트·대기·`try`·`?`), IR 덤프 `.ir` golden. 컴파일 단계 `Codegen`(SL §13.1 `stage codegen`, 기본 단계) | corpus `Codegen` 영역 사례의 IR golden, 진단 없는 모든 corpus 사례가 IR로 내려감(Codegen이 기본 단계라 corpus 전체가 내림을 거친다) |
| LC4b bytecode | bank 할당(선형 스캔), 32비트 명령 인코딩, 상수 표, bytecode 덤프 `.bc` golden | 같은 입력 → 같은 바이트, 덤프 golden |
| LC4c `.sparkc`·verifier | 파일 쓰기·읽기, verifier(구조·타입·바인딩·문맥), 재덤프 일치 | 읽어 다시 쓴 덤프가 컴파일 직후 덤프와 바이트 단위로 같다, 변조 시험의 거부 |
| LC4d ID·hash | 패키지 ID manifest(`.rocksids.json` 두 번째 발급자, SE-R8 파서 확장), interface hash·body hash, 재컴파일 범위 | 재컴파일 범위 시험, ids merge-check 통과 |

## 2. 값 표현(LC4a)

VM 레지스터는 bank 셋이다(SL §11): **Scalar**(태그 없는 64비트), **Ref**(엔진 객체·script instance handle + generation, 64비트), **Buf**(참조계수 COW 버퍼 handle). IR은 Sparks 값 하나를 원시 값(slot)의 묶음으로 내린다.

| Sparks 타입 | bank | slot 수 | 표현 |
|---|---|---|---|
| `bool` | Scalar | 1 | 0·1 |
| 정수 타입(`int8`–`uint64`, `int`) | Scalar | 1 | 부호 있는 타입은 부호 확장, 부호 없는 타입은 0 확장. 연산은 그 타입의 범위로 overflow 검사(fault) |
| `float32`·`float64`(`float`) | Scalar | 1 | IEEE double 비트. `float32` 연산은 결과를 float32로 반올림해 다시 double로 둔다 |
| enum(Sparks·엔진)·`Clock` | Scalar | 1 | case 번호(선언 순, 0부터). 저장은 이름(§15) — 번호는 실행 표현 |
| `vec2`·`vec3`·`vec4` | Scalar | 2·3·4 | 성분 double |
| `quat`·`Rotator`·`Color` | Scalar | 4·3·4 | `quat`(X,Y,Z,W), `Rotator`(Pitch,Yaw,Roll — 도), `Color`(R,G,B,A) |
| `Transform` | Scalar | 10 | Location 3, Rotation quat 4, Scale 3 |
| `Tag`·`Row<T>` | Scalar | 1 | 사전 이름 id(실행 중 Name 번호 — 쿡 GUID는 `.sparkc` imports의 표) |
| `Soft<T>` | Scalar | 2 | AssetId 128비트(lo, hi) |
| `string`·`Text`·struct(Sparks·엔진)·`Array`·`Map`·`Set`·`TagSet`·`Result<T>`(`T`가 Scalar 밖) | Buf | 1 | COW 버퍼(LC5 값 버퍼) |
| 엔진 객체·Sparks class·interface·로드한 에셋 값·유지 자원·`LoadRequest<T>` | Ref | 1 | handle(index 32·generation 32). 0은 null |
| `T?`(Scalar 표현) | Scalar | 1 + T | 앞 slot이 있음 표지(0·1), 없으면 뒤 slot은 0 |
| `InputAction`·`CVarRef<T>` | Ref | 1 | 실행 중 handle |
| `T?`(Ref·Buf 표현) | 같은 bank | 1 | null handle 0 |
| `mat4` | Scalar | 16 | 행 우선 성분 double(기본값 단위 행렬 — 대각 slot 0·5·10·15가 1) |
| `Class<T>` | Scalar | 1 | 사전 이름 id(클래스 경로) |
| 대기 handle(`async fn`·`Delay`·`NextTick`·`All`·`Race` 호출의 결과) | Ref | 1 | 실행 중 대기 객체. Sparks 타입이 아니며 덤프는 `wait`다 |
| `Result<T>`(`T`가 Scalar 표현이거나 `void`) | Scalar | 1 + T | 앞 slot이 오류 코드(0 성공, 아니면 `Error` case 번호 + 1) |

- struct를 Buf로 두는 이유: 필드 수가 정해지지 않은 값 타입을 레지스터 묶음으로 펼치면 호출 경계·컨테이너 원소·필드 저장의 표현이 둘이 된다. 수학 타입만 성분 수가 고정·작고 연산이 잦아 Scalar로 펼친다(D1).
- slot 묶음은 IR에서 "값 묶음(value tuple)"으로 다루고, bytecode에서는 연속 레지스터에 둔다(§4).

## 3. typed IR(LC4a)

함수 하나는 기본 블록 배열이다. 블록은 명령 열과 끝 명령(분기·반환·대기) 하나다. 명령은 3-주소 형식이며 피연산자는 bank가 정해진 가상 레지스터(`s12`·`r3`·`b4`)와 상수(`k`)다. IR은 SSA가 아니다(지역 이름 하나가 가상 레지스터 묶음 하나 — 대입은 그 레지스터를 덮어쓴다). 이유는 LC5 interpreter·LC9 C++ 생성이 모두 변수 단위 코드를 바로 내고, 최적화는 LC4 범위가 아니기 때문이다(D2).

### 3.1 명령 군

| 군 | 명령(대표) | 비고 |
|---|---|---|
| 이동·상수 | `MOV.S/R/B`, `LOADK.I64/F64`, `LOADNULL.R/B`, `LOADBOOL` | 묶음은 slot마다 한 명령 |
| 정수 | `ADD/SUB/MUL/DIV/MOD/NEG.<w>` (`w` = I8…I64·U8…U64) | 그 타입 범위 밖이면 `IntegerOverflow` fault, 0 나눗셈 `DivisionByZero` |
| 실수 | `ADD/SUB/MUL/DIV/NEG.F32/F64`, `MIN/MAX/ABS…`(부록 B Math) | 결과가 유한하지 않으면 `NonFinite` fault. FMA 축약 금지 |
| 비교·논리 | `EQ/NE/LT/LE.<w>/F64/STR/REF`, `NOT` | `&&`·`||`·`??`는 분기로 내린다 |
| 변환 | `CONV.<from>.<to>` | 범위 검사·반올림 규칙은 SL §3.10 변환 |
| 제어 | `JMP`, `BR cond, then, else`, `SWITCH`(정수·enum 표), `RET` | `for`·`while`·`switch`·`?? return`를 블록으로 |
| optional | `ISSOME.S/R/B`, `UNWRAP.S`(묶음의 뒤 slot) | `?.`·`if (let)`·`??` |
| 필드 | `GETF`·`SETF`(자기·다른 객체, FieldId·slot 번호), `GETSF`·`SETSF`(struct Buf 안) | `SETF`는 Changed 후보 기록(SL §5.3)·`@net` 권한 런타임 검사 |
| 호출 | `CALL fn`(사용자 함수 — 모듈 안 번호·import 표), `CALLV`(open·interface — slot 표), `CALLN`(native FunctionId), `CALLL`(부록 B 언어 함수 번호) | 인자·결과는 연속 레지스터(§4) |
| 이벤트 | `EMIT ev, args`, `EMITX target, ev, args`(확장 이벤트) | 발행 큐(LC6) |
| 대기 | `AWAIT wait`(대기 값), `AWAITCALL fn`(async 함수), `MKWAIT…`(Delay·NextTick·이벤트·로드 요청) | `AWAIT`는 블록 끝 명령. 재개 블록과 살아 있는 레지스터 목록을 둔다(SL §11 "await 지점별 레지스터 형태") |
| Result | `TRY r, fail`(오류면 함수에서 그 오류로 반환) | `try`·`try await` |
| 생성 | `NEWS`(struct Buf), `NEWC`(component 생성), `NEWA/NEWM/NEWSET`(컨테이너) | |
| 문자열 | `CONCAT`(보간 조각), `TOSTR.<w>` | |
| 자원 | `RELEASE r`(지역 유지 자원의 블록 끝·대입 해제 — SL §7.1 규칙 2) | 블록을 떠나는 모든 경로에 내린다(D5) |

### 3.1a 구현 형식(LC4a 확정)

- **명령:** `IrInstruction{op, sub, a, b, c, extra, span}` 하나가 3.1절 군의 명령 하나다. 타입별 opcode(`ADD.I32` 등)는 IR에서 목적 레지스터의 Scalar 해석으로 정하고, 이름은 LC4b의 bytecode 선택에서 붙인다. 묶음 연산(벡터 산술·이동)은 slot마다 명령 하나로 펼친다. 비교는 묶음 전체를 한 명령으로 비교한다(slot 수를 extra에).
- **인자 목록:** 호출·생성·발행·문자열 잇기의 인자는 연속 레지스터가 아니라 함수의 피연산자 표(`operands`)의 구간이다. 연속 레지스터 요구(4절 인자 규약)는 LC4b 할당이 이동을 넣어 맞춘다. 생성 `NEW`는 sub로 해석한다: struct·component는 (필드 심볼, 값) 쌍(빠진 필드는 실행 중 기본값), 검증 리터럴(`Tag`·`Row`·`Asset`·`Loc`…)은 (인자 이름 글자, 값) 쌍, 컬렉션은 원소(Map은 key·값 차례), `Result`(Buf 표현)는 값 하나(`NEW.OK`·`NEW.ERROR`).
- **대기:** `AWAIT`는 블록 끝 명령이며 대기 b(대기 handle, 로드 요청)나 (발행자 b, 이벤트 extra)를 기다리고, 재개하면 결과를 a에 받고 블록 c로 간다. 결과 타입은 피연산자가 정한다(async 함수는 반환 타입, `LoadRequest<T>`는 `Result<T>`, 그 밖은 없음 — `try await`의 Try 노드 타입이 꺼낸 `T`라 따로 구한다).
- **Result:** Scalar 표현은 앞 slot(오류 코드)을 직접 쓴다 — 성공 0, 실패 `Error` case 번호 + 1(정해진 오류는 코드를 바로 싣는다). `TRY`는 끝 명령이다: 성공이면 값을 꺼내 성공 블록으로, 실패면 실패 블록으로 간다. 실패 블록은 살아 있는 지역 유지 자원을 모두 해제하고(D5) `RETURN.ERROR`로 같은 오류를 이 함수의 Result 타입으로 반환한다.
- **문자열:** 보간 없는 문자열은 `LOADSTR` 하나, 보간은 조각(`LOADSTR.PIECE` — escape를 풀지 않은 원문 조각, LC4b가 상수 표에 넣을 때 푼다)과 값의 문자열 변환(`CONVERT`)을 `CONCAT`으로 잇는다. 상수(`const`)와 verbatim은 리터럴 원문 그대로다.
- **값 경로 쓰기:** 대입 대상은 값 경로(지역 → 필드·성분 slot·원소)로 왼쪽부터 한 번 평가한다(객체·key의 식은 한 번만 — 부작용이 두 번 나지 않는다). 그다음 오른쪽 값을 계산하고(SL §3.3 왼쪽에서 오른쪽), 경로 끝에 쓴 뒤 값 타입(struct Buf·수학 타입 slot·컨테이너)을 지난 단계는 바깥 값에 차례로 다시 쓴다(`Info.Offset.X = 1` → Info를 읽고 Offset slot을 바꿔 Info에 다시 `SETF`, `a[k] = v` → `SETE` 뒤 a를 다시 쓴다). 객체 참조(Ref)에서 경로가 새로 시작한다. 복합 대입은 같은 경로의 현재 값을 읽어 계산한다.
- **값을 바꾸는 메서드:** 언어 메서드 중 경로를 바꾸는 것(`Add`·`Remove`… SL §3.11)은 `CALL.MUT`다. 수신자 묶음은 in/out 인자이며(호출이 바뀐 값을 그 레지스터에 남긴다) 수신자 경로에 다시 쓴다.
- **유지 자원 필드:** 유지 자원 타입 필드의 `SETF`는 실행 중 이전 값을 해제하고 저장한다(필드 타입이 정한다 — SL §7.1 "획득 → 해제 → 저장", 오른쪽이 먼저 획득한다). 지역 `var`는 IR이 `RELEASE` 후 이동한다. `if (let s = 획득())`의 binding은 then 블록을 떠나는 모든 경로에서 해제한다.
- **const:** 접힌 값(숫자·bool·문자열·enum)은 상수로 싣고, 접지 않는 정해진 값(수학 값·`Tag`·`Math.Radians(...)`)은 const 초기값 식을 쓴 자리에서 다시 내린다(명령 위치는 쓴 자리). 숫자 상수식(`300 - 100`, 타입 없는 상수)은 검사기처럼 한 번에 접어 그 식의 타입으로 싣는다(SD66 — 연산마다 내리면 중간 값이 자리 타입의 범위를 넘는다). float32 상수는 float로 반올림한 double이다.
- **optional:** `T? == T`는 optional이 아닌 쪽을 `T?`로 올려 묶음 비교한다. Scalar `T?`의 "없으면 뒤 slot 0" 불변식은 값을 만드는 명령(`LOADNULL` 대응 0 채우기, `GETF`·`GETE`·`CALL`의 결과 — 실행 중 host·VM)이 지킨다. `?.`로 읽은 멤버·함수 결과가 이미 `T?`면 그 값을 그대로 쓴다. `await a?.F()`는 a가 있을 때만 부르고 기다리며 결과는 `R?`다. 이 표현(`Tag?` = 있음 표지 + Name)은 실행 표현이고, 저장 표현(없음 = None Name, §15)은 저장 형식이 바꾼다.
- **타입 인자:** 명시한 타입 인자가 있는 호출(`Find<Lamp>()`)은 그 타입 목록을 호출에 싣는다(native 호출이 받는다).
- **반복:** `for`는 숨은 번호를 0(범위는 시작)부터 끝 전까지 올리고 회차마다 이름에 값을 둔다. 컨테이너는 시작 때 잡은 값을 돈다(`LEN`·`GETE`, Map·Set은 `GETKEY`·`GETVALUE`). 범위 끝은 한 번만 계산한다.
- **분기 조건:** `if`·`while`·`&&`·`||`의 조건은 값을 만들지 않고 단락 분기로 내린다(`!`는 갈래를 바꾼다). 값 자리의 `&&`·`||`는 결과 레지스터에 0·1을 둔다.
- **switch:** case 값이 모두 정수·enum 상수면 표(`SWITCH`), 아니면(문자열 등) 차례 비교다. default가 없고 어느 case에도 맞지 않으면 switch 뒤로 간다.
- **본문 끝:** 반환 타입이 있는 함수의 끝(도달하지 않는다 — S0409)은 `FAULT 1`(Unreachable), void·`Result<void>`는 반환이다. 위치는 본문의 닫는 `}`다.
- **깊은 식:** 왼쪽으로 깊은 이항·`&&`·`||` 사슬은 척추를 모아 반복으로 내려 재귀 깊이가 사슬 길이에 비례하지 않는다(트리 깊이 상한 1024의 사슬을 Debug 기본 stack에서 내린다 — 시험 `SparksCodegen_DeepChainsStayWithinTheStack`).
- **내리는 범위:** 진단이 하나라도 있는 컴파일은 어느 모듈도 내리지 않는다(타입·심볼이 정해지지 않은 자리가 남는다). 필드 초기값(클래스 기본값 레코드)은 LC4c `TYPE` 구역과 함께 내린다.
- **구조 검사:** 내린 함수마다 불변식을 본다(내용 있는 블록은 끝 명령으로 닫힌다, 분기 대상은 내용 있는 블록, 레지스터·피연산자 번호가 범위 안, 이동의 bank와 비교의 묶음 크기가 맞다). 어기면 컴파일러 결함이므로 Fatal이다(corpus 전체가 이 검사를 거친다).
- **값 크기:** 값 하나는 18 slot까지다(`Result<mat4>?` = mat4 16 + 오류 코드 + 있음 표지, `Result<T?>`·`T??`는 타입 오류).

### 3.2 IR 덤프(`.ir`)

결정적 영어 텍스트다(`Compilation::AppendIrDump`). 함수·handler마다(선언 순, 사이에 빈 줄) 머리 줄 `function Owner.Name(매개변수 레지스터)`·`handler Owner.on 경로(…)`, 가상 레지스터 표(값 묶음마다 한 줄: `  s7..s9 w: vec3 [F64 F64 F64]` — 레지스터 번호는 bank 글자 `s`·`r`·`b`와 함수 안 번호, 이름은 지역 이름, Scalar는 slot 해석), 블록마다 `L<번호>:`와 명령 한 줄씩(`    s4 = ADD.I64 s0, s3    ; 2:12`)을 쓴다. 원문 위치는 그 명령을 낸 식·문장의 시작(줄:열)이다. 문맥(LC3b 효과 집합)은 LC4c `.sparkc`의 함수 표에 둔다. corpus `.ir` golden은 `Codegen` 영역 사례가 쓴다.

## 4. bank 할당과 명령 인코딩(LC4b)

bytecode 형식(opcode 번호·명령 모양·함수 표·상수 표)은 새 모듈 `SparksVm`(SL §18.1 — Core만 의존)의 공개 헤더가 정본이고, 컴파일러(`SparksCompiler`)가 LC4부터 그것에 의존해 명령을 만든다. 실행(LC5)과 verifier(LC4c)도 같은 정의를 쓴다.

### 4.1 frame과 호출 창(LC4b 확정)

- **frame:** 함수 하나의 frame은 bank마다 레지스터 배열이다(Scalar·Ref·Buf 각각 0–255). 매개변수는 선언 순서로 bank마다 0번부터(값 묶음은 연속), class 본문의 `this`는 Ref 0번(struct 메서드의 `this`는 Buf 0번)이고 나머지 같은 bank 매개변수가 그 뒤다. 반환값은 bank의 0번부터 돌려준다. 함수 표는 반환 값의 bank·slot 수를 둔다(`RETERR`가 그 모양의 Result를 만든다).
- **호출 창(Lua 방식 register window):** 호출은 부르는 쪽 frame의 bank마다 맨 위 구간(창 base — 할당된 레지스터 수)에 인자를 놓고 `CALL`한다. 부르는 함수의 frame은 창 base에서 시작한다(창이 곧 부르는 쪽의 0번). 반환값은 창 base로 돌아오고, 부르는 쪽이 결과 레지스터로 옮긴다. 창은 할당에 참여하지 않으며 frame 크기는 `할당한 수 + 가장 큰 창`이다(bank마다 256 이하, 넘으면 S1150).
- **창 쓰는 명령:** `CALL`·`EMIT`·`NEWLIT`(이름 지정 인자)·`CONCAT`은 인자 묶음을 창에 둔다(IR의 피연산자 목록 → 창으로 이동, bank마다 차례로). 수신자는 그 bank 창의 첫 칸이다. 구조체·컬렉션 생성은 창 없이 `NEWS`·`NEWA` 뒤 필드·원소마다 `SETSF`·`APPEND`·`PUT`이다.

### 4.2 할당

- 생성(`NEWS` 뒤 `SETSF`…)과 합성 곱처럼 결과를 먼저 쓰고 피연산자를 읽는 명령은 결과 묶음을 읽기 위치부터 살려(early clobber) 피연산자와 다른 레지스터에 둔다. await 지점 생존 표는 `emit`의 암묵 대상 `this`도 읽기로 센다.
- 가상 레지스터 묶음 하나(값 하나)가 실제 레지스터 연속 구간 하나다. 생존은 가상 레지스터 slot마다 블록 단위로 구하고(역방향 고정점), 묶음의 구간은 블록 배치 순서의 명령 번호(읽기 2k·쓰기 2k + 1)로 slot 구간들을 합친 것이다(블록 입구·출구에서 살아 있으면 그 경계까지). 도달하지 않는 블록은 내지 않는다. 구간은 배치 순서의 연속 구간이라 실제 생존의 상위 집합이다(await 지점 표도 같다 — 보수적).
- bank마다 구간 시작 순으로 선형 스캔해 끝난 구간의 레지스터를 다시 쓴다(묶음 크기만큼 연속으로 빈 곳 중 가장 낮은 번호). 매개변수·`this`는 고정 번호다. 결정적이다(같은 IR → 같은 번호).
- bank 레지스터 256개를 넘거나 함수 명령이 32,767개를 넘으면(분기 거리 16비트) S1150 "function is too large"다(위치는 함수 이름).

### 4.3 명령 형식

32비트 고정. `op:8 | A:8 | B:8 | C:8`(ABC), `op:8 | A:8 | Bx:16`(ABx — 상수·표 번호), `op:8 | A:8 | sBx:16`(AsBx — 분기 거리, 명령 단위, 다음 명령 기준). 피연산자가 더 필요한 명령은 바로 뒤에 `EXTRA Ax:24` 단어를 하나 붙인다(필드·타입·함수 번호, 컨테이너 bank 표지 — 덤프는 한 줄로 쓴다). opcode 번호는 bytecode 형식 버전에 묶인다(`SparksVm/Opcode.h`가 정본, 덤프는 이름만 쓴다).

| 군 | opcode | 피연산자 |
|---|---|---|
| 이동 | `MOV.S` `MOV.R` `MOV.B` | A ← B |
| 상수 | `LOADI` | A ← sBx(부호 있는 16비트 정수) |
| | `LOADK` | A ← Scalar 상수 표[Bx](64비트 — 정수·double 비트·이름 id) |
| | `LOADSTR` | Buf A ← 문자열 표[Bx] |
| | `LOADNULL.R` `LOADNULL.B` | A ← null |
| 정수 | `ADD` `SUB` `MUL` `DIV` `MOD` `.I8`–`.I64`·`.U8`–`.U64`, `NEG.I8`–`.I64` | A ← B op C(그 타입 범위 밖이면 `IntegerOverflow`, 0 나눗셈 `DivisionByZero`) |
| 실수 | `ADD` `SUB` `MUL` `DIV` `NEG` `.F32`·`.F64` | 결과가 유한하지 않으면 `NonFinite`. F32는 결과를 float로 반올림 |
| 합성 | `MUL.QUAT` `MUL.MAT4` A B C | quat·mat4 곱(UE `FQuat`·`FMatrix`의 `operator*`와 같은 순서, A·B·C는 묶음 첫 레지스터). 성분별 곱이 아니다 |
| 논리 | `NOT` | A ← !B |
| 비교 | `EQ` `NE` `LT` `LE` `.I`(부호 있는 정수·bool·enum·이름 — 64비트 확장값), `LT.U` `LE.U`, `EQ` `NE` `LT` `LE` `.F`, `EQ.STR` `NE.STR` `LT.STR` `LE.STR`(UTF-8 byte 순 — `string?` 포함), `EQ.R` `NE.R`(handle), `EQ.B` `NE.B`(Buf 값 비교 — 같음만) | A ← B cmp C. `>`·`>=`는 B·C를 바꾼 `LT`·`LE`다 |
| | `EQN` `NEN` + EXTRA(slot 수), `EQN.F` `NEN.F` | Scalar 묶음 비교. 성분이 모두 실수(와 있음 표지)면 `.F`(실수 비교 — -0.0 == 0.0), 그 밖(`int?`·`Soft<T>`의 AssetId)은 비트 비교 |
| 변환 | `CONV` | A ← B, C = (원래 Scalar 종류 << 4) \| 목적 종류(범위 밖은 `ConversionOutOfRange`) |
| | `TOSTR` + EXTRA(표시 종류·enum 타입 번호) | Buf A ← Scalar 묶음 B의 문자열(보간 규칙 SL §3.2) |
| | `CAST` + EXTRA(타입 번호) | Ref A ← B가 그 타입이면 B, 아니면 null |
| 제어 | `JMP` sBx, `BRT` A sBx, `BRF` A sBx | 분기 거리는 다음 명령 기준 |
| | `SWITCH` A Bx | switch 표[Bx](값 → 명령 주소, 기본 주소) |
| | `RET` A B C | bank C의 A부터 B개를 0번부터로 돌려주고 반환(B = 0이면 값 없음) |
| optional | `ISSOME.R` `ISSOME.B` | A ← B가 null이 아님(Scalar `T?`는 표지 slot을 `MOV.S`로 읽는다) |
| 필드 | `GETF.S/R/B` A B + EXTRA(필드 번호) | A ← 객체 B(Ref)의 필드(묶음은 필드 타입 slot 수만큼) |
| | `SETF.S/R/B` A B + EXTRA(필드 번호) | 객체 A의 필드 ← B(Changed 후보·`@net` 권한 실행 중 검사) |
| | `GETSF.S/R/B` `SETSF.S/R/B` + EXTRA | struct 값(Buf)의 필드 읽기·쓰기(COW) |
| | `GETCTX` A Bx | Ref A ← 문맥 이름 표[Bx](`World`·`Time`…) |
| 컨테이너 | `GETE` `SETE` `LEN` `GETKEY` `GETVAL` + EXTRA(key bank·원소 bank·원소 slot 수) | `GETE`는 `T?`를 돌려준다(SL §3.10 인덱스) |
| 호출 | `CALL` Bx(모듈 함수 표 — 사용자 함수), `CALLV` Bx(open·interface — 실행 중 대상 결정), `CALLN` Bx(import 표의 native 함수), `CALLL` Bx(언어 함수 — 부록 B 번호), `CALLRPC` Bx | 창 base는 함수 표의 bank별 할당 수(`registers`)다. 대기를 만드는 함수의 결과는 Ref 창 0번의 대기 handle. `CALL.MUT`(값을 바꾸는 언어 메서드)는 수신자를 창 0번에서 돌려받고 같은 bank의 결과는 그 다음 자리에서 받는다 |
| 이벤트 | `EMIT` Bx(이벤트 표) | 대상은 Ref 창 0번(자기 객체면 this), 인자는 그 뒤 |
| 대기 | `AWAIT` A B + EXTRA(결과 bank·slot 수) | 대기 handle B로 정지, 재개하면 결과를 A에. 블록 끝 명령이며 재개는 다음 명령 |
| | `AWAITEV` B + EXTRA(이벤트 번호) | 발행자 B의 이벤트를 기다린다 |
| Result | `TRY.S` A B C(slot 수) `TRY.B` A B C(값 bank << 5 \| slot 수) + EXTRA(부호 있는 24비트 거리) | 성공이면 값을 A에 두고 다음 명령, 오류면 EXTRA 거리(EXTRA 다음 단어 기준)의 실패 블록으로 |
| | `RETERR.S` A B(slot 수) `RETERR.B` A | Result의 오류를 이 함수의 Result로 반환 |
| 생성 | `NEWS` A Bx(struct 타입 — 기본값), `NEWC` A Bx(component), `NEWA`·`NEWSET` A Bx(원소 타입), `NEWM` A Bx(Map 타입), `APPEND` A B, `PUT` A B C, `NEWLIT` A + EXTRA(리터럴 참조 16비트 \| 인자 수 6비트 << 16 \| 결과 bank << 22), `OK` A B + EXTRA(값 bank·slot), `ERR` A B(`Error` case 번호 — Scalar Result의 코드 case + 1은 VM이 만든다) | 구조체 생성은 `NEWS` 뒤에 필드마다 `SETSF`. 생성 결과 레지스터는 피연산자 값과 겹치지 않는다(할당이 결과를 읽기 위치부터 살린다 — early clobber). 리터럴 참조의 이름은 결과 타입(`Tag`·`Soft<DataAsset>`·하드 참조 `DataAsset`…), 글자는 보조 번호, 이름 지정 인자(`Loc`)는 인자 표 `이름=bank 글자;…`(창에 놓는 순서) |
| 문자열 | `CONCAT` A B C | Buf A ← 창의 B부터 C개 조각 |
| 자원 | `RELEASE` A | Ref A의 유지 자원을 해제(null이면 아무것도 안 함) |
| fault | `FAULT` Ax | fault 종류 Ax |

- **참조 이름:** 다른 모듈의 Sparks 선언(함수·필드·타입·이벤트)은 `<import 경로>:` 접두로 같은 이름의 다른 모듈 선언과 구별한다(LC4d가 안정 ID를 더한다). 명시한 타입 인자가 있는 호출은 참조 이름이 `Owner.Name<T>`이고 보조 번호가 첫 타입 인자의 Type 참조다.
- **별칭:** 한 명령의 결과 레지스터가 읽는 레지스터와 같을 수 있다(`AWAIT`·`TRY.S`·`GETE`·산술). VM은 피연산자를 모두 읽은 뒤 쓰고, 묶음 복사는 앞에서부터 한다. `CONCAT`의 조각은 모두 Buf(문자열)다.
- **표 번호 한도:** Bx 16비트 자리(상수·문자열·참조·switch 표)는 65,536개까지다. 넘으면 S1150이다.

### 4.4 블록 배치와 분기

블록은 IR 순서로 놓는다. `BRANCH c, T, E`는 다음 블록이 T면 `BRF c → E`, E면 `BRT c → T`, 둘 다 아니면 `BRT c → T` 다음 `JMP E`다. 다음 블록으로 가는 `JUMP`는 내지 않는다. 도달하지 않는 블록(진입·분기 대상이 아닌 블록)은 내지 않는다.

### 4.5 상수 표

모듈 하나에 Scalar 상수(64비트, 처음 나온 순)·문자열(escape를 푼 UTF-8 — 보간 조각과 리터럴 원문을 여기서 푼다)·리터럴(`Tag`·`Row`·`Asset`·`Loc`·`CVar`·`Action`의 종류와 이름)·함수·필드·타입·이벤트 참조 표를 둔다. 같은 값은 한 번만 넣는다. `-32768`–`32767`의 정수는 `LOADI`로 상수 표를 쓰지 않는다.

### 4.6 bytecode 덤프(`.bc`)

결정적 ASCII 영어 텍스트(`AppendBytecodeDump` — 모듈만 읽으므로 `.sparkc`에서 읽은 모듈도 같은 글자를 낸다). 머리(`module "경로"`, `compiler "버전"`, `profile core|full`, `content`·`interface` hash 16진), 상수 표(`k<번호> = 0x<16진 64비트>`), 문자열 표(`t<번호> = "…"` — 제어 문자와 ASCII 밖 byte는 `\xHH`), 참조 표(`ref<번호> = 종류 이름 [보조]`), 함수마다 머리 줄 `function 이름  scalar=할당/frame ref=… buf=… params=s/r/b returns <bank>x<slot>`, await 지점(`await 0003  live s0..s2 r0`), switch 표(`switch<번호> [값: 주소, …] else 주소`), 명령마다 `  0004  ADD.I64  s4, s0, s3    ; 2:12` — 주소(4자리 10진), opcode 이름, 피연산자(레지스터 `s`·`r`·`b` + 번호, 상수 `k<번호>`·문자열 `t<번호>`, 분기 대상 주소, 참조 `ref<번호>`, 컨테이너 표지 `[값 bank·slot key bank·slot]`), 원문 위치. EXTRA 단어는 앞 명령 줄에 합쳐 쓴다(주소는 2 늘어난다). corpus `.bc` golden은 `Codegen` 영역 사례가 쓴다.

## 5. `.sparkc`와 verifier(LC4c)

- **형식(LC4c 확정, 형식 버전 1):** little-endian. 머리: magic `"SPKC"`, 형식 버전(u32 — `BytecodeFormatVersion`), compiler version 문자열(u32 길이 + UTF-8), 모듈 import 경로 문자열, content hash(원문 UTF-8의 SHA-256 32바이트), interface hash(SHA-256 32바이트 — LC4d가 채우고 그 전에는 0), profile(u8 — manifest `profile`: 0 core, 1 full). 이어 길이 접두 구역(4바이트 tag + u32 길이 + 내용)이 정해진 순서로 하나씩 온다: `KCON`(Scalar 상수 u64들, 문자열 — 길이 + byte), `REFS`(참조 — 종류 u8, 이름 문자열 번호 u32, 보조 번호 u32), `FUNC`(함수 표 — 이름·명령 구간·bank별 할당 수·frame 크기·매개변수 수·switch·await 구간, 명령 단어, switch 표와 case, await 지점과 생존 구간), `DBUG`(분리 디버그 — 명령 단어마다 줄·열). 모르는 tag, 순서 어긋남, 구역 길이와 내용의 불일치, 파일 끝 뒤의 byte는 거부다. `TYPE`(class·struct·enum·이벤트·필드의 TypeId·FieldId·초기값)·`IMPT`(import 모듈의 interface hash, native FunctionId·시그니처 hash, Tag·Action·Row 이름)는 안정 ID(LC4d)·필드 초기값과 함께 형식 버전 2에서 더한다. 쓰기는 Core `ByteWriter`, 읽기는 `ByteReader`다.
- **결정성:** 같은 입력 → 같은 바이트. 표의 순서는 처음 나온 순서·선언 순서이고 해시 표 순회를 쓰지 않는다. 읽어 다시 쓴 파일과 그 bytecode 덤프는 원본과 byte 단위로 같다(시험: corpus의 `.bc` 사례마다 쓰기 → 읽기 → 덤프 비교).
- **verifier(로드 시 선형 1회, SL §11 — `VerifyBytecode`):** 구조를 본다 — 표 번호(함수 이름·참조 이름·리터럴 글자·같은 모듈 함수 번호), 함수 명령 구간이 명령 배열 안, 원문 위치 표의 길이, bank별 할당 수 ≤ frame 크기 ≤ 256, 매개변수 수 ≤ 할당 수. 명령마다: 알려진 opcode, EXTRA가 필요한 명령 뒤에 EXTRA가 있고 그 밖에는 EXTRA가 홀로 오지 않음, 레지스터 피연산자가 그 bank frame 안(묶음 수를 아는 명령 — `RET`·`RETERR`·`CONCAT`·`EQN` — 은 구간 전체), 상수·문자열·참조 번호가 표 안이고 참조 종류가 명령과 맞음(`CALL` → 함수, `CALLN` → native, `EMIT`·`AWAITEV` → 이벤트, 필드 명령 → 필드, 생성·`CAST` → 타입, `GETCTX` → 문맥, `NEWLIT` → 리터럴), 분기·`TRY` 실패 거리·switch case·await 주소가 함수 안의 명령 시작(EXTRA 단어가 아님), `CONV` 종류 번호·컨테이너 표지·표시 종류가 범위 안, 함수의 마지막 명령이 끝 명령(`JMP`·`RET`·`RETERR`·`FAULT`). 생존 구간은 할당 수 안. 거부는 모듈 단위이며 사유(`BytecodeLoadError`)와 함수·주소를 돌려준다(SL §12 표 "로드"). 타입(Scalar 표현 종류·초기화 전 읽기 — 블록 단위 dataflow)·바인딩(native 시그니처 hash)·문맥(대기 금지 문맥의 `AWAIT`) 검사는 TYPE·IMPT 구역과 함수 문맥 표가 생기는 형식 버전 2(LC4d·LC6)에서 더한다.
- **컴파일 직후 검사:** 컴파일러는 내린 모듈마다 verifier를 돌린다(거부는 컴파일러 결함 — Fatal). 그래서 corpus 전체가 verifier를 거친다.
- 변조 시험: magic·버전·구역 tag·구역 길이·명령 1바이트(opcode·레지스터)·상수·참조 번호·분기 거리를 바꾼 파일이 정해진 사유로 거부된다.

## 6. 안정 ID·hash(LC4d)

- **ID manifest:** 패키지마다 `<패키지>.rocksids.json`. Sparks 컴파일러가 두 번째 발급자이고 이름 키는 `<패키지>/<모듈 경로>.<선언>[.<멤버>]`, kind는 `type`·`field`·`subobject`·`function`·`rpc`·`enumCase`다(SL §12.2). 메타 컴파일러 `StableIdManifest.cs`의 kind·이름 검사를 넓힌다(SE-R8 파서 부분).
- **발급:** 개발 빌드의 컴파일이 새 선언에 ID를 준다(입력 중 미완성 선언 제외 — 구문 오류가 있는 모듈은 발급하지 않는다). 데이터 연결 ID(`@save`·`@net`·`@edit` 필드, 소유 component, enum case)의 소실은 오류 S1005(새 코드), `@formerly`는 aliases로 기록한다.
- **interface hash:** 모듈의 공개 선언(이름·시그니처·필드 타입·const 값·이벤트·함수별 효과 집합 — SL §12.3)을 정규 순서로 직렬화한 SHA-256. **body hash:** 함수 본문의 IR 덤프 바이트 SHA-256.
- **재컴파일 범위:** 본문만 바뀌면 그 함수만, interface hash가 바뀌면 import한 모듈도 다시 검사한다.

## 7. 결정 항목(권장안 — 2026-11-22 확정)

- **D1 값 표현:** 수학 타입은 Scalar 펼침, struct·문자열·컨테이너는 Buf(COW). 대안(모든 값 타입을 Buf)은 수학 연산마다 할당이 생기고, 대안(struct도 펼침)은 표현이 둘이 된다.
- **D2 IR 형식:** 비SSA 3-주소, 지역 이름 = 가상 레지스터 묶음. 최적화는 범위 밖이므로 SSA 구성·해체 비용이 이득이 없다.
- **D3 레지스터 상한:** bank마다 256(8비트), 함수 명령 32,767개(16비트 분기 거리). 넘으면 컴파일 오류(S1150). 대안(16비트 피연산자)은 명령이 64비트가 되어 SL §11의 32비트 고정 명령과 어긋난다.
- **D7 호출 규약:** bank별 register window(Lua 방식). 대안(호출마다 인자 배열 할당)은 호출당 할당이 생기고, 대안(모든 값을 한 bank)은 SL §11의 3 bank 결정과 어긋난다.
- **D4 상수 표 범위:** 모듈 하나. 함수마다 두면 본문 reload가 쉬우나 중복이 많다 — 본문 reload(LC7)는 함수 단위 상수 구간을 따로 두지 않고 모듈 상수 표를 덧붙이는 방식으로 한다.
- **D5 유지 자원 해제:** 컴파일러가 블록을 떠나는 모든 경로에 `RELEASE`를 낸다(실행 중 범위 추적 없이). fault·취소는 LC6 실행이 frame의 유지 자원 표로 해제한다.
- **D6 hash:** 영속 식별자는 Core `Sha256`(FNV `HashStringBytes`는 실행 중 분포용이라 쓰지 않는다).
