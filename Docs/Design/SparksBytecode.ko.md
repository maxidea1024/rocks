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
- **Result:** Scalar 표현은 앞 slot(오류 코드)을 직접 쓴다 — 성공 0, 실패 `Error` case 번호 + 1. `TRY`는 오류 코드가 0이 아니면 같은 오류로 반환한다.
- **문자열:** 보간 없는 문자열은 `LOADSTR` 하나, 보간은 조각(`LOADSTR.PIECE` — escape를 풀지 않은 원문 조각, LC4b가 상수 표에 넣을 때 푼다)과 값의 문자열 변환(`CONVERT`)을 `CONCAT`으로 잇는다. 상수(`const`)와 verbatim은 리터럴 원문 그대로다.
- **값 경로 쓰기:** 값 타입(struct Buf·수학 타입 slot) 경로의 대입은 바깥 값을 읽어 바꾼 뒤 바깥 경로에 다시 쓴다(`Info.Offset.X = 1` → Info를 읽고 Offset slot을 바꿔 Info에 다시 `SETF`). 컨테이너 원소 대입(`a[i] = v`)도 같다(COW).
- **반복:** `for`는 숨은 번호를 0(범위는 시작)부터 끝 전까지 올리고 회차마다 이름에 값을 둔다. 컨테이너는 시작 때 잡은 값을 돈다(`LEN`·`GETE`, Map·Set은 `GETKEY`·`GETVALUE`). 범위 끝은 한 번만 계산한다.
- **분기 조건:** `if`·`while`·`&&`·`||`의 조건은 값을 만들지 않고 단락 분기로 내린다(`!`는 갈래를 바꾼다). 값 자리의 `&&`·`||`는 결과 레지스터에 0·1을 둔다.
- **switch:** case 값이 모두 정수·enum 상수면 표(`SWITCH`), 아니면(문자열 등) 차례 비교다. default가 없고 어느 case에도 맞지 않으면 switch 뒤로 간다.
- **본문 끝:** 반환 타입이 있는 함수의 끝(도달하지 않는다 — S0409)은 `FAULT 1`(Unreachable), void·`Result<void>`는 반환이다. 위치는 본문의 닫는 `}`다.
- **깊은 식:** 왼쪽으로 깊은 이항·`&&`·`||` 사슬은 척추를 모아 반복으로 내려 재귀 깊이가 사슬 길이에 비례하지 않는다(트리 깊이 상한 1024의 사슬을 Debug 기본 stack에서 내린다 — 시험 `SparksCodegen_DeepChainsStayWithinTheStack`).
- **내리는 범위:** 진단이 하나라도 있는 컴파일은 어느 모듈도 내리지 않는다(타입·심볼이 정해지지 않은 자리가 남는다). 필드 초기값(클래스 기본값 레코드)은 LC4c `TYPE` 구역과 함께 내린다.

### 3.2 IR 덤프(`.ir`)

결정적 영어 텍스트다(`Compilation::AppendIrDump`). 함수·handler마다(선언 순, 사이에 빈 줄) 머리 줄 `function Owner.Name(매개변수 레지스터)`·`handler Owner.on 경로(…)`, 가상 레지스터 표(값 묶음마다 한 줄: `  s7..s9 w: vec3 [F64 F64 F64]` — 레지스터 번호는 bank 글자 `s`·`r`·`b`와 함수 안 번호, 이름은 지역 이름, Scalar는 slot 해석), 블록마다 `L<번호>:`와 명령 한 줄씩(`    s4 = ADD.I64 s0, s3    ; 2:12`)을 쓴다. 원문 위치는 그 명령을 낸 식·문장의 시작(줄:열)이다. 문맥(LC3b 효과 집합)은 LC4c `.sparkc`의 함수 표에 둔다. corpus `.ir` golden은 `Codegen` 영역 사례가 쓴다.

## 4. bank 할당과 명령 인코딩(LC4b)

- **할당:** 함수마다 bank별로 가상 레지스터를 실제 레지스터 번호에 놓는다. 살아 있는 구간(블록 순서의 선형 번호)으로 선형 스캔하고, 값 묶음·호출 인자·결과는 연속 번호를 요구한다. bank마다 레지스터는 256개까지이고(8비트 피연산자), 넘으면 컴파일 오류 S1201(새 코드 — "함수가 너무 크다")이다(D3).
- **인자 규약:** 호출은 bank마다 연속 구간 `[base, base+n)`에 인자를 두고, 결과는 같은 base에 돌려받는다(Lua 방식). `this`는 Ref 0번이다.
- **명령 32비트:** `op:8 | A:8 | B:8 | C:8`(ABC), `op:8 | A:8 | Bx:16`(상수·필드·함수 번호), `op:8 | sAx:24`(분기 — 부호 있는 명령 단위 거리). 피연산자가 범위를 넘는 경우(상수 65,536개 초과)는 `EXTRA` 접두 명령이 위 16비트를 준다.
- opcode 번호는 bytecode 형식 버전에 묶인다(번호 표는 구현의 `Opcode.h`가 정본, 덤프는 이름만 쓴다).
- **상수 표:** 함수마다가 아니라 모듈 하나에 Scalar 상수(64비트)·문자열(UTF-8)·AssetId·이름 표를 둔다(중복 제거, 처음 나온 순서).

### 4.1 bytecode 덤프(`.bc`)

SL §11의 bytecode 예시 모양이다: 모듈 머리(format·compiler version·content hash·interface hash), 상수 표, 타입·필드·이벤트 표, 함수마다 bank 크기·문맥·await 지점별 살아 있는 레지스터, 명령마다 `주소 OPCODE 피연산자 ; 줄:열`. 분기 대상은 주소, 상수는 `k<번호>`와 값 주석이다.

## 5. `.sparkc`와 verifier(LC4c)

- **형식:** little-endian. 머리 `"SPKC"`·형식 버전(u32)·compiler version 문자열·모듈 import 경로·content hash(SHA-256 32바이트)·interface hash(SHA-256)·profile. 이어 길이 접두 구역(section): `IMPT`(import 모듈과 그 interface hash, native FunctionId·시그니처 hash, Tag·Action·Row 이름), `TYPE`(class·struct·enum·이벤트·필드 — TypeId·FieldId·초기값), `FUNC`(함수: 시그니처, bank 크기, 문맥, 명령, await 지점 표, 예외 없는 원문 위치 표), `KCON`(상수), `DBUG`(분리 디버그 — 원문 위치·지역 이름). 쓰기는 Core `ByteWriter`, 읽기는 `ByteReader`다.
- **결정성:** 같은 입력 → 같은 바이트. 표의 순서는 선언 순서(모듈 안), 해시 표 순회를 쓰지 않는다.
- **verifier(로드 시 선형 1회, SL §11):** 구조(구역 길이·명령 수·분기 대상이 함수 안·피연산자가 bank 크기 안·상수·필드·함수 번호 범위), 타입(각 명령의 피연산자 bank·Scalar 표현 종류, 대기 지점 레지스터 형태와 재개 블록의 일치, 초기화 전 읽기 없음 — 블록 단위 dataflow), 바인딩(native FunctionId·시그니처 hash가 실행 중 schema와 같음, 자원 정책), 문맥(함수 문맥이 금지하는 명령 — `AWAIT`가 대기 금지 문맥에 없음 등). 거부는 모듈 단위다(SL §12 표 "로드").
- 변조 시험: 명령 1바이트·구역 길이·상수 번호를 바꾼 파일이 정해진 사유로 거부된다.

## 6. 안정 ID·hash(LC4d)

- **ID manifest:** 패키지마다 `<패키지>.rocksids.json`. Sparks 컴파일러가 두 번째 발급자이고 이름 키는 `<패키지>/<모듈 경로>.<선언>[.<멤버>]`, kind는 `type`·`field`·`subobject`·`function`·`rpc`·`enumCase`다(SL §12.2). 메타 컴파일러 `StableIdManifest.cs`의 kind·이름 검사를 넓힌다(SE-R8 파서 부분).
- **발급:** 개발 빌드의 컴파일이 새 선언에 ID를 준다(입력 중 미완성 선언 제외 — 구문 오류가 있는 모듈은 발급하지 않는다). 데이터 연결 ID(`@save`·`@net`·`@edit` 필드, 소유 component, enum case)의 소실은 오류 S1005(새 코드), `@formerly`는 aliases로 기록한다.
- **interface hash:** 모듈의 공개 선언(이름·시그니처·필드 타입·const 값·이벤트·함수별 효과 집합 — SL §12.3)을 정규 순서로 직렬화한 SHA-256. **body hash:** 함수 본문의 IR 덤프 바이트 SHA-256.
- **재컴파일 범위:** 본문만 바뀌면 그 함수만, interface hash가 바뀌면 import한 모듈도 다시 검사한다.

## 7. 결정 항목(권장안 — 2026-11-22 확정)

- **D1 값 표현:** 수학 타입은 Scalar 펼침, struct·문자열·컨테이너는 Buf(COW). 대안(모든 값 타입을 Buf)은 수학 연산마다 할당이 생기고, 대안(struct도 펼침)은 표현이 둘이 된다.
- **D2 IR 형식:** 비SSA 3-주소, 지역 이름 = 가상 레지스터 묶음. 최적화는 범위 밖이므로 SSA 구성·해체 비용이 이득이 없다.
- **D3 레지스터 상한:** bank마다 256(8비트). 넘으면 컴파일 오류(S1201). 대안(16비트 피연산자)은 명령이 64비트가 되어 SL §11의 32비트 고정 명령과 어긋난다.
- **D4 상수 표 범위:** 모듈 하나. 함수마다 두면 본문 reload가 쉬우나 중복이 많다 — 본문 reload(LC7)는 함수 단위 상수 구간을 따로 두지 않고 모듈 상수 표를 덧붙이는 방식으로 한다.
- **D5 유지 자원 해제:** 컴파일러가 블록을 떠나는 모든 경로에 `RELEASE`를 낸다(실행 중 범위 추적 없이). fault·취소는 LC6 실행이 frame의 유지 자원 표로 해제한다.
- **D6 hash:** 영속 식별자는 Core `Sha256`(FNV `HashStringBytes`는 실행 중 분포용이라 쓰지 않는다).
