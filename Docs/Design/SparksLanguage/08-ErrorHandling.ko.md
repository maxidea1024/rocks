## 8. 오류 처리

### 8.1 컴파일 진단

진단 예:

~~~text
UI/HealthBar.spark:14:14: error S0412: 'Target' may be null.
        let hp = Target.Current;
                 ^^^^^^
  'Target' has type 'Health?'. Another actor reads as null once it is destroyed.
  fix: if (let health = Target) { let hp = health.Current; … }

Characters/PlayerCharacter.spark:42:5: error S0731: Cannot assign to a function result.
        GetTransform().Location.Z = 1.0;
        ^^^^^^^^^^^^^^
  GetTransform() returns a copy, so assigning to it never changes the original.
  fix: RelativeLocation.Z = 1.0;
~~~

- **형식(SD37):** `경로:줄:열: error S코드: 메시지` 다음에 원문 줄·표시·설명·`fix:` 수정 예를 둔다. 위치가 앞에 있어 VS Code problem matcher와 clang 형식 도구가 그대로 읽는다. 경고는 없으므로 종류는 항상 `error`다. CLI 종료 코드는 성공 0, 컴파일 오류 1, 사용법 오류 2다(AppInfoCommandLine 14.4절).
- **언어:** 컴파일러·VM·엔진이 내는 진단과 fault 보고는 영어다(실행 중 메시지 영어 규칙). LSP는 locale 옵션으로 지역화된 설명을 덧붙일 수 있으며 코드·위치·수정 예는 바뀌지 않는다. 사용자가 쓰는 `Log` 메시지와 `Assert` 문구의 언어는 사용자 콘텐츠이므로 프로젝트 정책이다.
- **코드 대역:** S01xx 어휘, S02xx 구문, S03xx 이름 해석, S04xx null·타입, S05xx 숫자·상수식, S06xx 실행 문맥·효과, S07xx 대입·`let`, S08xx 이벤트·수명 hook·재진입, S09xx 유지 자원·로드, S10xx 저장·ID·호환성, S11xx 패키지·reload·backend. 코드는 한 번 발급하면 의미를 바꾸지 않는다. 엔진 공통 진단 코드 등록부(RMC#### 메타 컴파일러, UI validate 코드와 함께)는 CompilerDiagnostics 문서에 둔다(§17.2).
- **LevelScript 코드(초안 1.4):** S0413 `LevelScript`의 `@edit` 객체 필드에 `?`가 없다(§3.2), S0801 `LevelScript`가 물리 hook을 선언했다, S0802 `LevelScript`가 `let` Collider component 필드를 선언했다. 판정 규칙은 §3.12, 메시지와 수정 예는 [LevelScript](../LevelScript.ko.md) 4.9절에 있다.
- 진단은 원인 하나를 작성한 코드 그대로 보이고 수정 예를 포함한다. 한 원인에서 파생된 연쇄 오류는 표시하지 않는다.[R23] 수정 예는 이름 규칙(§3.1)을 지키도록 생성한다. 지역 이름은 멤버 이름이 아니라 타입 이름에서 도출한다(`Target: Health?` → `health`). 유사 이름 제시는 Core `EditDistance`와 이름 붙은 문턱 상수를 쓴다.
- **위치:** 줄과 열은 1부터 센다. 열은 그 줄의 처음부터 센 Unicode code point 수이며 tab도 1이다(바이트 수가 아니다 — 주석·문자열에 한글이 있어도 열이 편집기와 맞는다). 언어 서버는 LSP가 요구하는 UTF-16 단위로 바꿔 보낸다. 줄 끝은 LF이고 LF 바로 앞의 CR은 줄 끝의 일부다 — 그래서 CRLF의 CR·LF 위치는 LF만 쓴 파일의 LF 위치와 같은 열(줄 끝)이다. BOM 안의 위치는 1줄 1열이다.
- **출력 세부:** 첫 줄 `경로:줄:열: error S코드: 메시지`, 둘째 줄은 원문 줄을 공백 4칸 들여 보이고(원문 줄의 들여쓰기도 그대로 보인다), 셋째 줄은 공백 4 + (열 − 1)칸 뒤에 `^`를 둔다. `^`의 수는 범위가 그 줄 안에 있는 code point 수이며 최소 1이고, 범위가 줄 끝을 넘으면 그 줄 끝에서 자른다(여러 줄 범위는 첫 줄만 표시, BOM 안에서 시작하는 범위는 BOM 뒤부터 센다). 그 뒤 설명 줄들과 `fix:` 줄은 공백 2칸 들여 쓴다(설명의 빈 줄은 들여쓰지 않는다). 설명이나 수정 예가 없으면 그 줄을 생략한다. 메시지와 수정 예는 한 줄이며(첫 줄을 problem matcher가 읽는다) 설명만 여러 줄일 수 있다. 원문 위치가 없는 진단(파일 전체 — 크기 초과 등)은 첫 줄이 `경로: error S코드: 메시지`이고 원문 줄·밑줄 없이 설명·수정 예를 쓴다. 원문 줄을 보일 때 tab·CR 같은 C0 제어 문자, DEL, C1 제어 문자(U+0080–U+009F), 표시 순서를 바꾸는 bidi 제어 문자(U+061C, U+200E·U+200F, U+202A–U+202E, U+2066–U+2069)는 공백 한 칸으로, 잘못된 UTF-8 byte는 U+FFFD로 바꾼다(열 계산과 맞고, 터미널을 흐트러뜨리거나 원문을 다르게 보이게 하지 않게). 동아시아 넓은 문자는 터미널에서 두 칸을 차지해 `^`가 어긋날 수 있다(표시 한계, 위치 값은 정확하다).
- **순서:** 한 번의 컴파일이 낸 진단은 파일 경로 byte 순 → (같은 경로의 서로 다른 파일이면 더한 순서) → 파일 전체 진단 먼저 → 시작 위치 → 코드 → 낸 순서로 정렬해 낸다. 같은 입력이면 출력이 바이트 단위로 같다.
- **원문 검사(S01xx 앞부분, LC0):** 컴파일러는 원문을 받으면 먼저 다음을 검사한다. 원문 처음의 UTF-8 BOM(EF BB BF)은 허용하고 무시한다(편집기가 붙이는 경우가 많다). 원문 크기 상한은 16 MiB(`MaxSourceBytes`)다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0101 | `invalid-utf8` | UTF-8이 아닌 바이트 열(overlong·surrogate·U+10FFFF 초과·잘린 sequence). 위치는 처음 잘못된 sequence의 첫 byte(예: `E1 80 5A`면 `E1`)이며 파일당 하나만 낸다 | 파일을 UTF-8로 다시 저장 |
| S0102 | `lone-carriage-return` | LF가 뒤따르지 않는 CR. 줄 끝 해석이 편집기마다 달라지므로 오류다. 처음 하나만 내고, S0101이 있으면 검사하지 않는다. 홀로 선 CR은 줄을 나누지 않는다 | 줄 끝을 LF 또는 CRLF로 |
| S0103 | `source-too-large` | 원문이 16 MiB를 넘는다 | 파일을 나눈다 |

  S0101이나 S0103이 나면 그 파일의 뒤 단계(어휘 분석 이후)를 실행하지 않는다.
- **어휘 분석(S01xx 나머지, LC1):** 원문을 토큰으로 나눈다. 공백은 space·tab·LF·CR이다. 오류가 나도 끝까지 토큰을 만들어(그 자리는 건너뛴다) 구문 분석이 이어지게 한다. 연쇄 오류를 막기 위해 같은 줄에서 같은 코드는 한 번만 낸다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0104 | `invalid-character` | 문자열·주석 밖에서 쓸 수 없는 문자(ASCII 밖 문자, `'` `#` `\` `` ` `` `~` `^` `$`, 제어 문자, 단독 `&`·`\|`). 단독 `&`·`\|`는 비트 연산자가 없다고 설명한다 | 문자 삭제, 논리 연산은 `&&`·`\|\|` |
| S0105 | `underscore-in-identifier` | 식별자 안의 `_`(`move_speed`, `_x`). 단독 `_`는 따로 토큰이다 | PascalCase·lowerCamel로(`moveSpeed`) |
| S0106 | `unterminated-block-comment` | `/*`가 닫히지 않았다. 위치는 `/*` | `*/` 추가. 블록 주석은 중첩되지 않는다 |
| S0107 | `unterminated-string` | 일반 문자열은 줄 끝이나 파일 끝까지, verbatim 문자열은 파일 끝까지 닫는 `"`가 없다. 위치는 여는 `"`(verbatim은 `@`) | 같은 줄에서 `"`로 닫는다(verbatim은 `"`로 닫는다) |
| S0108 | `invalid-escape` | `\"` `\\` `\n` `\t` `\$` `\u{…}` 밖의 escape, `\u{}`의 자리 수가 0이거나 6을 넘음, surrogate·U+10FFFF 초과 | 허용 escape 목록 제시 |
| S0109 | `integer-literal-too-large` | 정수 리터럴이 쓰는 자리의 타입에 들어가지 않는다(`int`면 int64 — 단항 `-`가 붙은 9223372036854775808만 허용, `uint8`이면 0–255, 부호 없는 자리의 음수). 타입을 아는 검사 단계가 판정한다 | 범위 안의 값이나 더 넓은 타입 |
| S0110 | `malformed-number` | `1.`, 숫자가 없는 지수(`1e`·`1e+`), 숫자가 없는 접두사(`0x`·`0b`), 2진에 쓸 수 없는 숫자(`0b102`), 잘못된 자리의 `_`(`1_`·`1_.5`·`1._5`·`1e_5`), 숫자 바로 뒤의 영문자(`12abc`, 접미사는 S0114) | `1.0`·`1.0e3`·`0xFF`·`1_000` |
| S0114 | `numeric-suffix` | C# 숫자 접미사(`1L`·`1u`·`1.5f`·`1.5d`·`1.5m`) | 접미사를 지운다. 실수가 필요하면 `1.0` |
| S0111 | `string-in-interpolation` | `${ }` 안의 문자열 리터럴 | 지역 변수로 먼저 받는다 |
| S0112 | `range-operator` | `..`(범위는 `..<` 하나다, SD16) | `a..<b` |
| S0113 | `float-literal-out-of-range` | 실수 리터럴이 유한한 double로 표현되지 않는다(`1e400`). 너무 작은 값은 0 또는 비정규 수로 허용 | 범위 안의 값 |

- **토큰:** 식별자, 단독 `_`, 키워드(부록 A — `true`·`false`·`null` 포함), 정수·실수 리터럴, 문자열(시작·조각·`${`·`}`·끝으로 나뉜다), verbatim 문자열(`@"…"` 전체가 한 토큰), 문서 주석 `///`, 구두점 `{ } ( ) [ ] ; , . ?. ? : = == != < <= > >= + - * / % += -= *= /= ! && || ?? ..< @ as?`. `as?`는 `as` 바로 뒤에 공백 없이 `?`가 올 때 한 토큰이다(그 밖의 `as`는 import 별칭 키워드). `>=`는 한 토큰이며 타입 인자를 닫는 자리에서 구문 분석이 둘로 나눈다(§3.6 — 토큰이 `=` 한 글자로 바뀌고 `>`는 타입 인자 노드의 범위에만 남는다). 토큰은 원문 위치만 가지며 값(정수·실수·escape를 푼 문자열)은 구문 분석이 원문에서 다시 읽는다. 어휘 오류 자리에는 Invalid 토큰을 남긴다: 잘못된 문자(그 문자 범위), 닫히지 않은 문자열(닫는 토큰 바로 앞, 빈 범위), 닫히지 않은 verbatim 문자열(그 토큰 뒤, 빈 범위), 닫히지 않은 블록 주석(`/*` 범위). 구문 분석은 Invalid 토큰에서 진단을 더 내지 않는다(아래 "복구").
- **문서 주석:** `///` 바로 뒤가 `/`가 아니면 문서 주석 토큰이고(`////`는 일반 주석), 놓인 자리 검사는 구문 분석이 한다(§3.1).
- **구문 분석(S02xx, LC1):** 토큰을 §3.6 문법의 구문 트리로 만든다. 구문 트리는 원문 위치를 가진 노드 배열이며(노드 배치는 `SparksCompiler/SyntaxTree.h`), 괄호식도 노드로 남겨 formatter와 진단이 원문 모양을 알 수 있게 한다. 문법에 있지만 의미가 제한되는 것(이름 지정 인자를 쓸 수 있는 호출, 함수 결과에 대입 S0731, 프로필별 문법 등)은 구문 분석이 받아들이고 뒤 단계가 판정한다. 노드 종류마다 자식의 자리와 수가 정해져 있고 구문 오류가 있어도 같다(빠진 자리는 Error 노드). 부모 노드의 범위는 자식 범위를 모두 포함하고 형제의 시작 위치는 순서대로다. 구문 트리 덤프(`AppendSyntaxDump`, SD58)는 이 트리를 한 줄에 노드 하나씩 종류·flag·토큰 글자·범위로 적은 결정적 텍스트이며(형식은 `SyntaxTree.h`) corpus의 `.ast` golden(§13.1)이 쓴다.
- **복구(원인 하나에 진단 하나, LC1b에서 확정):**
  - 구문 오류를 내면 복구 상태가 되고, 복구 상태에서는 진단을 더 내지 않는다. 분석은 문법대로 이어 가되 기대한 토큰이 없으면 소비하지 않고 빠진 부분은 Error 노드로 채운다.
  - 문장·멤버·선언 하나를 마쳤을 때 복구 상태이면, 그 단위가 `;`나 `}`로 끝났으면 거기가 동기화 지점이므로 상태만 푼다. 아니면 동기화 지점까지 진단 없이 건너뛴다. 문장은 `;`(소비)·`}`·문장을 시작하는 키워드(`var` `let` `if` `for` `while` `switch` `return` `emit` `break` `continue` `try` `await`), 멤버는 `;`(소비)·`}`·멤버를 시작하는 토큰(`var` `let` `fn` `async` `override` `open` `event` `on` `@`), 선언은 선언을 시작하는 토큰(`import` `class` `struct` `enum` `interface` `event` `const` `fn` `async` `override` `open` `@`)이 동기화 지점이다. 최상위의 `;`·`}`는 그 자체가 오류이므로 건너뛴다. 건너뛸 때 `{ … }`는 짝을 맞춰 통째로 건너뛴다. switch 본문은 `case`·`default`·`}`, enum 본문은 `,`·`}`에서 맞춘다.
  - 본문(블록·클래스·struct·interface·enum·switch)을 여는 `{`를 읽으면 복구 상태가 풀린다. 머리의 오류(`if (x {`)와 본문 안의 오류는 원인이 다르기 때문이다. 머리를 읽고 난 바로 그 자리의 `{`만 본문으로 읽는다 — 머리 오류로 다른 토큰이 남아 있으면(`while (a b) { … }`) 그 본문은 동기화 규칙대로 `{ … }` 통째로 건너뛴다.
  - 파일 끝에서는 복구 상태가 풀리지 않는다. 빠진 `}`·`)`를 같은 원인으로 다시 알리지 않기 위해서다(오류 없이 파일 끝에 닿으면 빠진 `}`는 S0201 하나이며, 여러 겹이 빠져도 하나다).
  - 같은 위치(시작 위치)에는 구문 진단(S02xx)을 하나만 낸다(S0215 제외). 동기화가 오류 난 토큰에서 멈춘 뒤 그 토큰에서 시작한 다음 단위가 같은 자리에서 또 틀리는 경우(`interface` 다음 줄의 `import`, interface 본문의 `override`)다.
  - 선언·멤버·문장을 시작할 수 없는 토큰에서 난 오류(S0203·S0204·S0205)는 그 토큰을 Error 노드로 소비한 뒤 동기화한다(같은 자리에서 같은 진단이 되풀이되지 않게). 본문을 닫는 `}`와 파일 끝은 소비하지 않는다. 그 앞의 문서 주석·attribute는 트리에 남기지 않는다. 이렇게 오류로 소비한 `;`·`}`는 동기화 지점이 아니다(`} x`도 진단 하나).
  - 첫 선언 전(import 단계)의 잡토큰은 S0203이고 import 목록에 Error 노드로 남는다. S0210은 첫 선언(선언을 시작하는 토큰에서 시작한 단위) 뒤의 import다.
  - 이름·타입이 와야 할 자리(S0206·S0207 — 선언 이름, 매개변수·binder, 타입, `.`·`?.` 뒤 멤버 이름)에 앞 토큰과 같은 줄의 키워드가 있으면 이름 자리에 잘못 쓴 것으로 보고 그 키워드를 소비한다(`var class = 1;`의 `class`가 클래스 선언으로, `x.if`의 `if`가 if 문으로 다시 읽히지 않게). 다른 줄이거나 이미 복구 중이면 새 구문의 시작일 수 있으므로 남긴다. attribute 이름 자리(`@class`)는 예외로 소비하지 않는다 — 바로 뒤가 선언 키워드일 수 있기 때문이다.
  - S0213은 `??`와 그 뒤 키워드를 오류의 일부로 소비한다(`return`을 새 문장으로 읽지 않게).
  - 건너뛸 것이 없는 오류는 그 자리에서 내고 복구 상태를 만들지 않는다: `@` 뒤 공백(S0207), S0208, S0209(사슬당 한 번, 왼쪽 결합으로 이어 읽는다), S0210(그 import는 트리에 넣지 않는다), S0211, S0212, S0214(버린 attribute는 트리에 넣지 않는다 — interface 메서드의 것은 남긴다), S0217(case 뒤따름은 switch당 한 번, 두 번째 `default`는 트리에 넣지 않는다), S0218(리터럴당 한 번), S0219, 빈 문장 `;`(S0205), 선언 사이의 `;`(S0203). 복구 중에는 이것들도 내지 않는다.
  - 버린 attribute(S0214) 뒤에 그 자리(문장·매개변수)를 시작할 토큰이 없으면 S0214 하나만 낸다(`fn F() { @edit }`).
  - try·await 값 뒤에 이항 연산자나 `as?`가 이어지면(`let x = await F() + 1;`) 그 키워드 위치에 S0212를 내고 복구 상태가 된다(§3.3 — 지역 변수로 먼저 받는다). 식 안에 잘못 놓인 try·await는 S0212를 그 자리에서 내고 뒤 연산은 이어 읽는다.
  - 현재 토큰이 어휘 오류 자리면 구문 오류를 내지 않고 복구 상태만 된다. 어휘 오류 자리는 Invalid 토큰, 또는 시작 위치에 어휘 진단(S01xx)이 있는 토큰이다(보간 안의 문자열, `_`가 든 식별자 등 — 어휘 분석이 같은 줄의 같은 코드를 한 번만 내므로 그렇게 빠진 토큰은 해당하지 않는다).
  - 문서 주석: 연속한 `///` 줄은 사이에 빈 줄·일반 주석이 있어도 하나의 문서 주석이며, 그다음 토큰이 선언·멤버·enum case를 시작해야 한다. 문서 주석은 줄을 시작해야 한다 — 코드 뒤 같은 줄의 `///`(`var x = 1; /// 설명`)는 다음 선언을 설명하지 않으므로 S0208이다. 문서 주석은 attribute보다 앞에 둔다(`/// 설명` → `@edit` → 선언). S0208은 붙을 자리가 아닌 문서 주석마다(줄마다) 낸다. 복구 중 건너뛴 문서 주석은 알리지 않는다.
- **진단 위치(`.expect`가 비교하는 줄·열):** S0201·S0202·S0203·S0204·S0205·S0206·S0207·S0216은 그 자리의 토큰(파일 끝이면 끝 위치), S0207의 `@` 뒤 공백은 이름 토큰, S0208은 그 `///` 줄, S0209는 두 번째 비교·같음 연산자, S0210은 `import`, S0211은 대입 왼쪽 식의 시작, S0212는 그 `try`·`await`, S0213은 `??`, S0214는 attribute 목록의 처음(`@`), S0215는 넘은 자리의 토큰(트리 깊이면 그 노드의 시작), S0217은 `case` 또는 두 번째 `default`, S0218은 처음으로 종류가 다른 원소의 시작, S0219는 enum 이름이다. 기반 목록·`as?` 대상의 타입 인자·`?`(S0201)는 그 `<`·`?`다.
- **깊이 상한(S0215):**
  - 재귀하는 구문의 중첩은 128단계까지다(`MaxSyntaxNesting`). 단계는 문장 하나, 식 하나(괄호·대괄호·호출 인자·인덱스·보간 안의 식마다 새로 하나), 단항 연산자 하나, `??`의 오른쪽 하나, `try`·`await` 하나, 타입 하나(타입 인자마다 새로 하나), `else if` 하나마다 늘어난다. 넘으면 넘은 자리의 토큰에 S0215를 (복구 중이어도) 내고 그 파일의 나머지는 분석하지 않는다. 악의적이거나 생성된 원문이 컴파일러 stack을 넘치게 하지 못하게 하는 방어다(한 단계가 parser 프레임 여러 개를 쓰므로 Debug 빌드의 기본 stack 1 MiB 안에 들도록 정했다).
  - 구문 트리 전체 깊이는 1024단계까지다(`MaxSyntaxTreeDepth`, 뿌리 File이 1단계, 연산자 사슬 `a + b + c`·`a.b.c`의 연산자도 한 단계씩). 넘으면 끝까지 분석한 뒤 가장 깊은 경로에서 1025단계 노드의 위치에 S0215를 낸다(가장 깊은 자식이 여럿이면 자식 순서에서 앞의 것).
  - S0215가 없는 트리는 뒤 단계(검사·IR 생성·formatter·LSP)가 재귀로 걸어도 된다. 대신 뒤 단계는 재귀 경로의 frame을 작게 둔다: 재귀하는 함수에는 하위 결과 값만 두고 판정·진단 문장(StringBuilder)·임시 값은 재귀가 끝난 뒤 부르는 함수에 둔다(LC2c 타입 검사는 Debug에서 단계당 약 0.5 KiB — 시험 `SparksTypes_DeepChainsStayWithinTheStack`이 1000단계 식 연쇄와 문장 중첩을 돌린다). S0215가 난 파일은 뒤 단계로 넘기지 않는다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0201 | `expected-token` | 특정 토큰이 와야 할 자리에 다른 것이 있다(`;`·`)`·`}`·`:`·`=` 등). 메시지에 기대한 토큰과 실제 토큰을 쓴다 | 그 토큰을 넣는다 |
| S0202 | `expected-expression` | 식이 와야 할 자리 | 값이나 식을 쓴다 |
| S0203 | `expected-declaration` | 파일 최상위에 선언이 아닌 것 | `class`·`struct`·`enum`·`interface`·`event`·`const`·`fn` |
| S0204 | `expected-member` | 클래스·struct·interface·enum 본문에 멤버가 아닌 것 | `var`·`let`·`fn`·`event`·`on`(종류마다 다르다) |
| S0205 | `expected-statement` | 블록 안에 문장이 아닌 것 | 문장을 쓴다 |
| S0206 | `expected-type` | 타입이 와야 할 자리 | `int`·`Health?`·`Array<Enemy>` |
| S0207 | `expected-name` | 이름(식별자)이 와야 할 자리. `@` 바로 뒤에 공백이 있는 경우 포함 | 이름을 쓴다 |
| S0208 | `misplaced-doc-comment` | `///`가 선언·멤버·enum case 바로 앞이 아니다(§3.1) | `//`로 바꾼다 |
| S0209 | `chained-comparison` | 결합하지 않는 비교·같음 연산자를 잇달아 썼다(`a < b < c`, `a == b == c`) | `a < b && b < c` |
| S0210 | `import-after-declaration` | 선언 뒤의 `import` | import를 파일 처음으로 옮긴다 |
| S0211 | `invalid-assignment-target` | `=`·복합 대입의 왼쪽 맨 바깥이 이름·멤버·원소가 아니다(`a + b = 1`, `F() = 1`, `(a) = 1`, `this = x`). 경로 안의 호출 결과·`?.`·`this.`는 S0731(§3.6) | 대입할 변수나 필드를 쓴다 |
| S0212 | `misplaced-await` | `await`·`try`가 문장 수준(문장 전체, 지역 선언의 초기값, 단순 대입의 우변, `return`)이 아닌 자리, 복합 대입의 우변, 그 값 뒤에 이항 연산자·`as?`가 이어짐(`await F() + 1`), `await try` 순서(§3.3) | 지역 변수로 먼저 받는다 |
| S0213 | `misplaced-exit` | `?? return`이 지역 선언의 초기값 밖에 있거나, `?? break`·`?? continue`(SD39) | `let x = e ?? return;` |
| S0214 | `misplaced-attribute` | 문장 앞처럼 attribute를 둘 수 없는 자리 | attribute를 지운다 |
| S0215 | `nesting-too-deep` | 재귀 구문의 중첩이 128단계를 넘거나(그 파일의 나머지는 분석하지 않는다) 구문 트리 깊이가 1024단계를 넘는다(위 "깊이 상한") | 지역 변수나 함수로 나눈다 |
| S0216 | `range-outside-for` | `..<`가 `for` 머리 밖에 있다(§3.3) | `for (i in a..<b)` |
| S0217 | `misplaced-default` | `switch`의 `default`가 마지막이 아니거나 두 번 나온다 | `default`를 맨 끝에 하나만 |
| S0218 | `mixed-collection-literal` | 컬렉션 리터럴에 값 원소와 `키: 값` 원소가 섞였다(`[1, "a": 2]`) | Array는 값만, Map은 `키: 값`만 |
| S0219 | `empty-enum` | case가 없는 enum(`enum E { }`). 이름이 빠진 enum에는 내지 않고(S0207 하나), case 자리가 하나라도 있으면(그 이름이 빠져도) 내지 않는다 | case를 하나 이상 쓴다 |

- **문법 세부(모호함 없애기):**
  - 빈 문장(`;` 하나)과 단독 블록(`{ … }`을 문장으로 쓰기)은 없다(S0205). 블록은 `if`·`for`·`while`·`switch`·함수·handler의 몸통에만 온다.
  - `if`·`for`·`while`의 몸통은 항상 블록이다(`if (x) return;`은 S0201 — 수정 예는 중괄호로 감싸기).
  - 함수 수식어 순서는 `open`·`override` 중 하나, 그다음 `async`, 그다음 `fn`이다(다른 순서는 S0201).
  - attribute를 둘 수 있는 자리는 최상위 선언, 클래스·struct 멤버, enum case뿐이다. interface 메서드·문장·매개변수 앞은 S0214다.
  - 클래스의 기반 목록은 `Path`다(타입 인자·`?` 없음). 첫 기반은 엔진 타입 또는 Sparks 클래스, 나머지는 interface(full)이며 그 판정은 뒤 단계다. `as?`의 대상도 `Path`다. 둘 다 타입 인자·`?`를 붙이면 그 자리에 S0201을 내고, 그 타입 인자·`?`는 읽고 버린다(본문·나머지 식은 그대로 읽는다).
  - `as`는 import 별칭에만 쓴다. 식 안의 하향 변환은 `as?` 하나뿐이다(`x as T`는 S0201).
  - 최상위 선언 뒤의 `;`(`class A { };`)는 S0203이다.
  - 목록 끝의 쉼표는 enum case 목록만 받는다. 인자·매개변수·컬렉션 리터럴·case 값 목록의 끝 쉼표는 다음 원소가 빠진 것이다(S0202·S0207).
  - `try`와 `await`를 함께 쓸 때는 `try await F()` 순서다(`await try F()`는 S0212, 순서를 고치라는 문장).
  - 식 안의 `?? return`·`?? break`·`?? continue`는 `??`의 오른쪽 피연산자로 읽지 않는다(지역 선언의 Exit이거나 S0213).
- **이름 해석(S03xx, LC2):** import·이름·명명 규칙이다. S0301–S0305는 import 해석(§3.4 "import 해석", LC2a), S0306–S0325는 선언·이름·타입 표기 해석(§3.9, LC2b)이며 위치와 연쇄 방지 규칙은 §3.9에 있다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0301 | `unknown-module` | import 경로가 현재 패키지·의존 패키지의 모듈이 아니다. 첫 구간이 의존 패키지 이름뿐이면(`import Weapons;`) 패키지는 모듈이 아니라고, 자기 패키지 이름으로 시작하면 패키지 이름 없이 import한다고 설명한다 | 경로를 고친다(비슷한 모듈 이름 제시) |
| S0302 | `undeclared-dependency` | import 경로의 첫 구간이 이 컴파일의 다른 패키지인데 manifest `dependencies`에 없다 | `dependencies`에 그 패키지를 더한다 |
| S0303 | `duplicate-import` | 같은 모듈을 다시 import했다(별칭 유무와 무관, 두 번째 줄부터 줄마다) | 한 줄을 지운다 |
| S0304 | `self-import` | 자기 모듈을 import했다(줄마다) | 지운다 |
| S0305 | `rocks-import` | `Rocks` 패키지를 import했다(엔진 타입·prelude는 자동으로 보인다, SD60) | 지운다 |
| S0306 | `naming-rule` | 이름의 첫 글자가 그 종류의 규칙(§3.1 표 — 타입·멤버·enum case·최상위 선언·별칭은 대문자, 매개변수·지역 이름·binder는 소문자)과 다르다 | 첫 글자를 바꾼 이름 |
| S0307 | `reserved-name` | 사용자 선언 이름이 예약 이름(부록 A — 내장 타입·prelude·핵심 엔진 타입·검증 리터럴·문맥 이름·RPC 접두·`Changed`)이다 | 다른 이름 |
| S0308 | `hook-name` | 수명 클래스의 멤버 이름이 hook 이름·`On`+hook·Unity hook 이름이다 | `on Start()`처럼 hook으로 |
| S0309 | `duplicate-declaration` | 같은 선언 공간에 같은 이름(대소문자 무시)이 다시 나온다(둘째부터) | 한쪽 이름을 바꾼다 |
| S0310 | `name-conflict` | 선언 이름이 다른 출처의 보이는 이름과 겹친다(§3.9 비교 규칙 — 엔진 타입, 최상위 fn·const, import한 선언, 문맥 이름, 둘러싼 지역 이름, Sparks 멤버) | 다른 이름 |
| S0311 | `import-conflict` | import가 도입한 이름이 앞선 import·현재 파일의 최상위 선언과 같거나 별칭이 그것들·엔진 타입과 같다(import 줄당 하나) | 별칭을 붙이거나 이름을 바꾼다 |
| S0312 | `member-type-name` | class·struct 멤버 이름이 보이는 타입 이름과 같다(`var Health: Health?`) | 멤버 이름을 바꾼다(`HealthComp`) |
| S0313 | `hides-inherited-member` | 멤버 이름이 상속 멤버와 같다(`override fn` 예외) | 다른 이름, 재정의 가능하면 `override fn` |
| S0314 | `override-without-target` | `override fn`이 재정의할 상속 함수가 없다(같은 이름이 함수가 아니거나 재정의할 수 없는 경우 포함) | `override`를 지우거나 이름을 고친다 |
| S0315 | `override-signature-mismatch` | `override fn`의 매개변수 타입·수·반환 타입·`async`가 대상과 다르다 | 대상의 시그니처로 |
| S0316 | `misplaced-open` | `open fn`이 `open class` 밖에 있다 | `open`을 지우거나 class에 `open`을 붙인다 |
| S0317 | `unknown-name` | 식의 이름을 찾지 못했다 | import 추가 또는 비슷한 이름 |
| S0318 | `unknown-type` | 타입 이름을 찾지 못했다(별칭 아닌 한정 경로 `Weapons.Sword` 포함) | import 추가, 짧은 이름, 비슷한 이름 |
| S0319 | `not-a-type` | 타입 자리의 이름이 타입이 아니다(prelude 값·문맥 이름·const·fn·멤버 등) | 그 대상의 타입을 쓴다 |
| S0320 | `type-argument-count` | 제네릭 타입의 타입 인자 수가 틀리거나 제네릭이 아닌 타입에 타입 인자가 있다 | `Map<string, int>`처럼 정한 수로 |
| S0321 | `context-name-unavailable` | 그 위치에서 쓸 수 없는 문맥 이름(§3.9 — struct·최상위 fn·필드 초기값의 `World`, `WorldService`의 `Input`, 서버 RPC 밖의 `Caller`) | 문맥 이름이 있는 class 함수로 옮긴다 |
| S0322 | `invalid-base` | 첫 기반이 상속할 수 있는 엔진 타입(`scriptExtensible`)이나 `open class`가 아니거나, 나머지 기반이 interface가 아니거나 거듭된다 | 기반 타입을 고친다 |
| S0323 | `inheritance-cycle` | Sparks 클래스 상속이 순환한다(순환 하나에 하나) | 순환을 끊는다 |
| S0324 | `interface-not-implemented` | class가 기반 interface의 메서드를 같은 이름·시그니처로 구현하지 않았다 | 그 시그니처의 `fn`을 더한다 |
| S0325 | `invalid-service` | manifest `services` 항목이 같은 패키지 모듈의 `WorldService` 파생 최상위 class가 아니다 | 모듈 경로와 class 이름을 고친다 |
| S0326 | `unknown-member` | `.`·`?.` 뒤 멤버나 이름 지정 인자의 필드를 찾지 못했다(LC2c, §3.10) | 비슷한 멤버 이름 |

- **null·타입(S04xx, LC2c)·숫자·상수식(S05xx, LC2c):** 규칙과 위치는 §3.10이다. 위치는 그 식(또는 피연산자·인자·멤버 이름)의 시작이며, S0409는 함수 본문의 닫는 `}`다. S0109(`integer-literal-too-large`)는 어휘 대역 번호이지만 타입을 아는 이 단계가 낸다. S0413은 LevelScript(§8.1 위)이며 LC3에서 검사한다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0401 | `type-mismatch` | 값의 타입이 자리의 타입에 대입 가능하지 않다(§3.10 "대입 가능"), `?` 타입 인자, `Result<T?>` | 변환하거나 맞는 타입의 값 |
| S0402 | `not-callable` | 함수가 아닌 것을 불렀다, 생성할 수 없는 타입(`constructible: false`, Actor 등 참조 타입, `mat4`) | 함수 이름이나 엔진 생성 API |
| S0403 | `argument-count` | 인자 수가 매개변수 수와 다르다 | 시그니처대로 |
| S0404 | `argument-form` | 함수 호출의 이름 지정 인자, 생성의 위치 인자, 이름 중복, 기본값 없는 필드를 빠뜨림 | 함수는 위치 인자, 생성은 `Field: 값` |
| S0405 | `not-a-value` | 타입 이름·이름 공간·함수·검증 리터럴 이름·`void` 결과를 값으로 썼다, 반환 타입·`Result<void>` 밖의 `void`·`void?` 표기 | 값을 쓴다 |
| S0406 | `invalid-operator` | 연산자가 그 피연산자 타입에 없다(문자열 `+` 포함) | 보간·맞는 타입 |
| S0407 | `not-comparable` | `==`·`!=`를 쓸 수 없는 타입, `?` 없는 값과 `null`, `==`를 쓸 수 없는 Map key·Set 원소 | `Math.NearlyEqual` 등 |
| S0408 | `condition-not-bool` | 조건·논리 연산의 피연산자가 `bool`이 아니다 | `x != null`·`x ?? false` |
| S0409 | `missing-return` | 반환 타입이 있는 함수가 끝나는 문장으로 끝나지 않는다 | `return`을 더한다 |
| S0410 | `return-value` | `void` 함수의 `return 값`, 반환 타입이 있는 함수의 `return;` | 값을 지우거나 더한다 |
| S0411 | `type-argument-rule` | 제네릭 함수의 타입 인자가 없거나 수가 틀리거나 제약을 지키지 않는다, `Soft<T>`·`Class<T>`의 T가 맞지 않는다 | `Find<Health>()` |
| S0412 | `may-be-null` | `T?` 값에 `.`·인덱스를 썼다, `T?` 컬렉션을 `for`로 돌았다 | `?.`, `if (let)`, `??` |
| S0414 | `not-optional` | `?.`·`??`·`if (let)`의 대상이 `T?`가 아니다, `if (let)`의 `bool?` | 그 연산을 지운다 |
| S0415 | `switch-coverage` | enum switch가 case를 빠뜨렸거나 `default`가 있다, enum이 아닌·엔진 `Error` switch에 `default`가 없다 | 빠진 case나 `default` |
| S0416 | `duplicate-case` | 같은 값의 case가 다시 나온다 | 한 case로 |
| S0417 | `no-expected-type` | 기대 타입이 필요한 `[]`·Map 리터럴·`null`·`Asset(...)`·`Row(...)`·`CVar(...)`에 기대 타입이 없다 | 타입을 적는다 |
| S0418 | `mixed-collection-types` | 기대 타입 없는 배열 리터럴의 원소 타입이 다르다 | 타입을 적거나 맞춘다 |
| S0419 | `interpolation-type` | 보간할 수 없는 타입(§3.3) | 그 값의 표시용 문자열 |
| S0420 | `invalid-cast` | `as?`의 피연산자·대상이 맞지 않거나 올림 변환이다 | `as?`를 지운다 |
| S0421 | `await-try-operand` | `await`의 대상이 대기가 아니다, `try`의 대상이 `Result`가 아니다 | 그 키워드를 지운다 |
| S0422 | `unawaited` | 대기(`async fn` 호출·`Delay`·이벤트 등)를 `await` 없이 값으로 썼다, `All`·`Race`의 인자가 결과 없는 대기나 지역 변수의 로드 요청이 아니다(§6.1 SD41) | `await`, 지역 변수 |
| S0423 | `this-misuse` | struct·최상위 fn·const의 `this`, 자기 멤버의 `this.X` | 이름만 쓴다 |
| S0424 | `validated-literal-form` | 검증 리터럴의 첫 인자가 보간 없는 문자열 리터럴이 아니다 | `Tag("A.B")` |
| S0425 | `static-instance-mismatch` | 타입 이름으로 인스턴스 멤버를, 인스턴스로 static 함수를 불렀다 | 맞는 쪽으로 |
| S0426 | `recursive-struct` | struct가 필드로 자신을 값으로 담는다(직접·다른 struct를 거쳐, 순환 하나에 하나) | 컨테이너에 담거나 class로 |
| S0427 | `initializer-too-deep` | 필드·const 초기값이 다른 초기값을 따라가는 연쇄가 64단계를 넘는다(이름 의존은 미리 정하므로 값의 멤버 등으로 이어진 연쇄) | 중간 값을 const·지역으로 나눈다 |
| S0428 | `object-field-needs-optional` | `?` 없는 객체 타입 필드가 §3.11의 허용 자리(component 생성 `let` 필드, Component·Actor 파생 class의 `@edit` 필드)가 아니다 | `T?` |
| S0429 | `struct-default-cycle` | struct 필드 초기값이 생성하는 struct를 따라 같은 struct로 돌아온다(기본값이 끝없다 — §3.11) | 필드를 빈 값(`[]`)으로 시작하고 함수에서 채운다 |
| S0501 | `integer-division` | 정수(상수 포함)끼리의 `/`, 정수 변수의 `/=` | `Math.Div(a, b)`, `float(a) / float(b)` |
| S0502 | `float-to-int-conversion` | 실수 → 정수 타입 변환 호출 | `Math.FloorToInt` 등 |
| S0503 | `constant-overflow` | 타입 없는 정수 상수식이 −2^63..2^64−1을 벗어난다, 정해진 상수 식이 그 타입의 범위를 벗어난다 | 값을 줄인다 |
| S0504 | `constant-division-by-zero` | `%`의 오른쪽, `Math.Div`의 둘째 인자가 값 0인 정수 상수다 | 0이 아닌 값 |
| S0505 | `non-finite-constant` | 실수 상수식이 NaN·Inf다 | 값을 고친다 |
| S0506 | `unsigned-negation` | 부호 없는 정수 타입의 단항 `-` | 부호 있는 타입 |
| S0507 | `mixed-numeric-types` | 다른 숫자 타입끼리 계산·비교했다 | `float(x)` 등으로 맞춘다 |
| S0508 | `invalid-conversion` | 숫자 변환 호출의 인자가 숫자가 아니다, `bool`·`string` 변환 | `Parse.Int` 등 |
| S0509 | `float-constant-in-integer` | 실수 상수를 정수 자리에 썼다 | 정수 상수나 `Math.RoundToInt` |
| S0510 | `not-constant` | const 초기값이 상수식이 아니다, 필드 초기값이 상수식·component 생성이 아니다(§3.11) | 상수식으로, 실행 중 값은 `on Start()`에서 |
| S0511 | `const-cycle` | const 초기값이 순환한다(순환 하나에 하나) | 순환을 끊는다 |
| S0512 | `const-type` | const의 타입이 §3.11의 목록에 없다 | `let` 필드나 함수로 |

- **대입·`let`·미사용(S07xx, LC2d):** 규칙과 위치는 §3.11이다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0701 | `let-write` | `let`·const 이름·필드에 대입, `let` 값 타입의 멤버·원소 쓰기와 변경 메서드 호출 | `var`로 선언 |
| S0702 | `parameter-write` | 인자·반복 변수·`if (let)` binding을 바꾼다 | 지역 `var`에 복사해 바꾼다 |
| S0703 | `foreign-field-write` | 다른 객체의 Sparks 필드를 쓴다 | 그 class의 함수를 부른다 |
| S0704 | `read-only-property` | 엔진의 읽기 전용 필드·부록 B `let` 속성을 쓴다 | 쓰기 API를 쓴다 |
| S0705 | `struct-self-write` | struct 함수가 자기 필드를 쓴다 | 바꾼 값을 돌려준다 |
| S0706 | `not-assignable` | 문맥 이름·타입 이름·이름 공간·함수·enum case에 대입 | 변수에 대입 |
| S0710 | `unused-local` | 지역 변수·binding·반복 변수를 어디에서도 읽지 않는다 | 지우거나 `_` |
| S0711 | `unread-store` | 지역 `var`에 쓴 값을 그 뒤 어느 경로에서도 읽지 않는다(사본 수정 포함) | 원래 경로에 쓰거나 지운다 |
| S0712 | `unused-parameter` | 본문에서 읽지 않는 인자(시그니처가 정해지지 않은 함수의 `_` 포함) | 지우거나 정해진 자리면 `_` |
| S0713 | `discarded-result` | 식 문장의 값이 `void`가 아니다 | 값을 쓰거나 `_ = ...` |
| S0714 | `unused-import` | import한 모듈의 선언을 쓰지 않는다 | import를 지운다 |
| S0731 | `assign-to-call-result` | 대입·변경 경로에 호출 결과(그 뒤가 참조 타입 값을 지나지 않을 때)나 `?.`가 있다 | 지역 변수에 받아 바꾼 뒤 되돌려 쓴다 |

- **권한·RPC·저장 타입(S10xx, LC3d):** 규칙과 위치는 §3.15다. 저장 호환성(코드 대역의 나머지)은 LC4에서 정한다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S1001 | `not-saveable` | `@save` 필드의 타입이 저장 가능 타입이 아니다 | `Soft<T>`·값 타입 |
| S1002 | `not-replicable` | `@net` 필드·RPC 매개변수가 복제 타입이 아니다, RPC 반환 타입이 `void`가 아니다 | `Soft<T>`·객체 참조 |
| S1003 | `net-write-outside-authority` | 권한 구역 밖의 `@net` 필드 쓰기(직접·호출) | `if (Owner.HasAuthority) { ... }` |
| S1004 | `rpc-call` | `@rpc` 함수를 접두 없이·다른 대상 접두로 부른다, `@rpc`가 아닌 함수에 접두 | `Server.F(...)` |
| S1005 | `data-id-lost` | 패키지 ID manifest의 활성 `type`·`field`·`subobject`·`enumCase` 항목이 현재 선언에도 `@formerly`에도 없다(저장·복제·편집 데이터가 연결을 잃는다 — [SparksBytecode](../SparksBytecode.ko.md) 6.1절, LC4d). 위치는 manifest의 그 `name` 값 | 새 이름에 `@formerly("<옛 이름>")`(모듈 이동이면 전체 옛 키)을 붙이거나, `rocks ids rename`으로 옮기거나, `rocks ids retire`로 은퇴시킨다. field와 subobject 사이로 형태만 바뀌었으면 형태를 되돌리거나 은퇴·데이터 이전 |
| S1006 | `invalid-id-manifest` | 패키지 ID manifest(`<패키지>.rocksids.json`)가 공유 형식(`schema: 1`, 항목의 kind·name·id·aliases·retired — 다른 키 없음)이 아니거나, 다른 패키지의 이름·alias, 같은 이름·ID 둘, 주인 type 없는 활성 멤버를 담는다 | 도구로 다시 만든다(손으로 고치지 않는다) |
| S1007 | `id-name-reserved` | 선언의 이름 키가 ID manifest의 alias(다른 ID의 옛 이름)이거나 은퇴한 이름이다([SparksBytecode](../SparksBytecode.ko.md) 6.1절). 위치는 선언 이름 | 다른 이름을 쓴다 |
| S1008 | `id-claimed-twice` | `@formerly`가 가리키는 ID를 다른 선언이 이미 쓴다(옛 이름이 아직 선언돼 있거나 두 `@formerly`가 같은 이름). 위치는 `@formerly`를 단 선언 이름 | `@formerly`를 빼거나 다른 선언의 이름을 먼저 바꾼다 |

- **유지 자원·로드(S09xx, LC3c):** 규칙과 위치는 §3.14다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0901 | `discarded-resource` | 유지 자원·로드 요청을 버린다(식 문장·`_ =`) | 지역 변수나 필드에 담는다 |
| S0902 | `resource-placement` | 담는 자리(§3.14)가 아닌 곳에서 획득한다, 로드 요청을 필드에 담는다 | `let x = ...`로 받는다 |
| S0903 | `resource-moved` | 유지 자원을 담은 이름을 옮기거나 복사한다 | 담긴 이름의 멤버를 호출한다 |
| S0904 | `resource-declaration` | 유지 자원 타입을 담을 수 없는 선언·타입 자리 | `var X: T?` 필드나 지역 변수 |
| S0905 | `resource-without-wait` | 지역 유지 자원 뒤 블록 끝까지 `await`가 없다 | 필드에 담거나 `await Delay(...)` |

- **실행 문맥·효과·attribute(S06xx, LC3b):** 규칙과 위치는 §3.13이다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0601 | `await-outside-async` | `async`가 아닌 함수의 `await` | `async fn`으로 선언 |
| S0602 | `context-effect` | 문맥(hook·handler·문맥 attribute 함수)이 금지하는 효과를 직접 또는 호출로 쓴다 | 다른 hook으로 옮긴다 |
| S0603 | `while-without-wait` | 대기할 수 없는 본문의 `while`, 회차마다 `await`가 없는 경로 | 반복 안에서 `await NextTick();` |
| S0620 | `unknown-attribute` | 표에 없는 attribute 이름 | 비슷한 이름 |
| S0621 | `attribute-not-allowed` | attribute를 그 선언에 쓸 수 없다, 문맥 attribute가 둘 | attribute를 옮기거나 지운다 |
| S0622 | `duplicate-attribute` | 한 선언에 같은 attribute가 둘 | 하나를 지운다 |
| S0623 | `attribute-argument` | attribute 인자의 수·이름·값이 표와 다르다 | 표의 인자 |

- **이벤트·수명 hook·재진입(S08xx, LC3a):** 규칙과 위치는 §3.12다. S0413(LevelScript의 `?` 없는 `@edit` 객체 필드)도 LC3a가 판정한다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0413 | `level-script-required-object` | LevelScript의 `@edit` 객체 필드에 `?`가 없다 | `T?` |
| S0801 | `level-script-physics-hook` | LevelScript가 물리 hook을 선언했다 | 트리거 Actor의 `TriggerEntered` 등을 받는다 |
| S0802 | `level-script-collider` | LevelScript가 `let` Collider component 필드를 선언했다 | 트리거 Actor를 배치한다 |
| S0803 | `unknown-hook` | 구간 하나짜리 `on X()`의 X가 hook 이름이 아니다 | 비슷한 hook 이름, 자기 이벤트면 함수를 직접 부른다 |
| S0804 | `hook-unavailable` | 수명 클래스가 아닌 곳의 handler, 물리 hook을 받을 수 없는 class의 물리 hook | 수명 타입에서 파생한다 |
| S0805 | `hook-signature` | hook 인자가 정해진 목록의 앞부분이 아니다(수·타입) | `on Tick(dt: float)` 등 |
| S0806 | `duplicate-handler` | 같은 class의 같은 hook, class 계층의 같은 `on` 경로 | 하나로 합친다 |
| S0807 | `unknown-event-source` | `on`·`emit` 경로의 원천을 해석하지 못한다 | 비슷한 필드 이름 |
| S0808 | `unknown-event` | 원천에 그 이벤트가 없다 | 비슷한 이벤트 이름 |
| S0809 | `changed-unavailable` | `Changed`가 없는 필드의 `Changed`, `Changed`라는 이벤트 선언 | `var` 필드, 다른 이벤트 이름 |
| S0810 | `handler-signature` | handler 인자가 이벤트 인자의 앞부분이 아니다(수·타입) | 이벤트 인자 타입 그대로 |
| S0811 | `emit-not-allowed` | 이 class가 선언하지 않은 멤버 이벤트·엔진 이벤트의 `emit` | 선언한 class의 함수를 부른다 |
| S0812 | `event-call` | 식에서 이벤트를 호출 형태로 썼다 | `emit X.E(...)` 또는 `await X.E` |
| S0813 | `extension-event-target` | 확장 이벤트의 대상이 `Actor`·`World`가 아니다 | `event Actor.X(...)` |
| S0814 | `duplicate-extension-event` | 확장 이벤트 이름이 대상 타입의 이벤트·다른 확장 이벤트와 같다 | 다른 이름 |
| S0815 | `extension-event-not-imported` | 확장 이벤트를 선언한 모듈을 직접 import하지 않았다 | 그 모듈의 import |
| S0816 | `missing-reentrancy-policy` | `await`가 있는 `on` 경로 handler·물리 hook에 재진입 표시가 없다 | `@skipWhileBusy` 또는 `@restart` |
| S0817 | `misplaced-reentrancy-policy` | 재진입 표시가 둘이거나 `await` 없는 handler·수명 hook·함수에 있다 | 표시를 지운다 |

- **패키지(S11xx, LC2a):** manifest·모듈 경로·패키지 의존·binding schema 파일이다. 위치 규칙은 §3.4와 §10.2 "binding schema 파일 형식"에 있다. 코드 대역의 나머지(reload·backend)는 해당 마일스톤에서 더한다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S1101 | `manifest-syntax` | manifest가 JSON5 객체 하나가 아니다(문법 오류, 중복 key, 최상위가 객체가 아님) | JSON5 문법을 고친다 |
| S1102 | `unknown-manifest-key` | 알 수 없는 key | 그 key를 지운다(비슷한 key 제시) |
| S1103 | `missing-manifest-key` | 필수 key(`name`·`sparks`·`profile`)가 없다 | 그 key를 더한다 |
| S1104 | `invalid-manifest-value` | 값의 종류·형식이 틀리다(이름 규칙, `profile` 값, 배열 원소, 중복, 자기 자신·`Rocks` 의존, 길이) | 표(§3.4)의 형식대로 |
| S1105 | `unsupported-language-version` | `sparks`가 이 컴파일러가 아는 언어 버전이 아니다(지금은 1뿐) | `sparks: 1` |
| S1106 | `missing-dependency` | 의존 패키지가 이 컴파일에 없다(이름이 없는 패키지가 있으면 내지 않는다) | 패키지 이름을 고치거나 패키지를 더한다 |
| S1107 | `dependency-cycle` | 패키지 사이 의존이 순환한다(강하게 연결된 묶음 하나에 하나, 메시지에 순환 경로) | 순환을 끊는다 |
| S1108 | `invalid-module-path` | 모듈 경로가 규칙에 맞지 않는다(구간 이름, `.spark` 확장자) | 대문자로 시작하는 영문자·숫자 이름으로 |
| S1109 | `module-path-case-conflict` | 대소문자만 다른 두 경로(처음 철자가 아닌 쪽의 모듈 파일마다) | 한쪽 이름을 바꾼다 |
| S1110 | `module-folder-conflict` | 같은 폴더의 `X.spark`와 폴더 `X/` | 한쪽 이름을 바꾼다 |
| S1111 | `package-folder-conflict` | 의존 패키지 이름(과 `Rocks`)이 현재 패키지의 최상위 폴더·최상위 모듈 이름과 같다 | 폴더 이름을 바꾼다 |
| S1112 | `duplicate-package` | 한 컴파일에 같은 이름(ASCII 대소문자만 다른 것 포함)의 패키지가 둘 있다 | 한쪽 이름을 바꾼다 |
| S1113 | `full-profile-only` | `profile: "core"` 패키지에서 full 문법(§1.3·§3.11)을 썼다 | `profile: "full"` 또는 core 대안 |
| S1120 | `invalid-binding-schema` | binding schema 파일이 §10.2 형식이 아니다(문법, key, kind가 허용하지 않는 key, 값, 이름 규칙·키워드, ID 표기·중복, 타입 표기 문법, 중복 이름, 파일 사이 타입 이름·ID·모듈 중복) | 생성기를 다시 돌린다(손으로 고치지 않는다) |
| S1150 | `function-too-large` | 함수가 bytecode 한계를 넘는다(bank마다 동시에 쓰는 값 256 slot, 명령 32,767개 — [SparksBytecode](../SparksBytecode.ko.md) 4.2절, LC4b). 위치는 함수 이름 | 함수를 작은 함수로 나눈다 |

- 사전 대조 리터럴(`Tag`·`Action`·`CVar`)이 사전에서 이름이 바뀐 경우 사전의 redirect(`@formerly`와 같은 역할)를 따라 자동 변환하고 빠른 수정으로 소스를 고친다. redirect도 없으면 오류다.

### 8.2 Result와 try

- 조회 실패는 `T?`, 작업 실패는 `Result<T>`다. 결과를 사용하지 않으면 오류다. 직접 처리할 때는 `r.Value`·`r.Error`를 확인한다.
- `Result<T>`를 반환하는 함수는 성공을 `return 값;`, 실패를 `return Error.NotFound;`처럼 쓴다. `Error`는 엔진이 정한 닫힌 enum이며(case 목록은 부록 B) 사용자 정의 오류 타입은 없다(SD38).
- `try`는 실패하면 실행을 더 진행하지 않는다. `Result`를 반환하는 함수에서는 실패를 호출자에게 전달한다. 그 밖의 함수와 handler에서는 현재 실행(Execution 트리 전체)을 EndExec로 끝내고 위치별로 1회 진단을 기록한다. `try` 실패는 fault가 아니므로 격리 집계와 Tryout 정지에 포함하지 않는다. 예외로 hook이 절반만 실행된 채 남지 않도록 `Awake`·`Enable`·`Start` 안의 `try` 실패는 그 hook의 fault다(SD43).
- 스크립트에 노출하는 API 이름에는 `Try` 접두를 쓰지 않는다. `try` 키워드("실패하면 여기서 끝")와 반대 의미로 읽히기 때문이다. `Result`를 돌려주는 C++ `TryX`는 Sparks에서 자동으로 `X`로 노출한다(§10.3, SD44).

### 8.3 fault와 발생 위치별 처리

| fault | 원인 |
|---|---|
| `IndexOutOfRange` | 범위 밖 원소 쓰기, Map에 없는 키의 복합 대입·경로 쓰기(읽기는 `T?`) |
| `IntegerOverflow`, `DivideByZero` | 정수 overflow, `Math.Div`·`%`의 0, 범위 밖 숫자 변환, `float(i)`의 정밀도 손실 |
| `NonFinite` | NaN·Inf를 만든 연산, float32 범위 밖 값의 전달 |
| `BudgetExceeded`, `CascadeLimit` | 명령어·호출 깊이·native stack·메모리·Spawn 수 예산, 발행 큐 상한(스크립트 발행), 이벤트 연쇄 깊이 상한·연속 이월. 메모리 예산 초과는 할당 위치를 보고 |
| `AssertFailed` | 모든 빌드에서 검사하며 조건식은 Pure 문맥. `@edit(Min, Max)` 범위 밖 쓰기 포함 |
| `ContractViolation` | native API의 계약 위반(파괴 지연 미준수, PendingDestroy 대상의 쓰기 호출 등), 요청 결과를 두 번 꺼냄 |

| 발생 위치 | 처리 |
|---|---|
| 필드 초기값·Awake·spawn 중 Enable | spawn 실패(`Result`), 미공개 graph rollback(전체 계획 §5.3), 큐에 들어갔지만 전달되지 않은 emit 폐기, 획득한 자원은 Detach로 해제(SD43) |
| 재활성화의 Enable, Start | 해당 instance 격리 |
| Tick 계열 | 해당 instance의 Tick·FixedTick·LateTick을 그 클래스 모듈의 새 revision이 게시될 때까지 중지. Disable·Enable로는 해제되지 않음 |
| 그 밖의 handler·실행 | 그 실행 트리만 중단하고 해제. 같은 instance에서 직전 600 프레임(프레임 번호 기준, 일시 정지·디버거 정지 중 프레임 제외, 경계 포함) 안에 세 번째 fault가 기록되면 격리(SD42). 한 실행 트리의 fault는 1회로 센다 |
| `on Validate`·`@editorButton`·`@cheat` | §8.4 편집기 경계 |
| 원격 client가 유발(RPC 처리 중) | instance·session에 영향 없음, 해당 connection에 집계(임계 초과 시 끊기, §8.4) |

- **격리:** Disable hook을 실행하지 않고 모든 실행을 EndExec하며 유지 자원 해제와 연결 key 폐기를 수행하고 값 필드는 검사용으로 보존한다. admission은 그 instance가 파괴될 때의 Destroy hook 1회만 허용한다. 다시 활성화할 수 없고, 그 instance가 파괴될 때(component는 소유 Actor 파괴, WorldService는 World 종료, WidgetController는 widget 제거) Destroy hook을 실행한 뒤 Detach한다(§5.1 종료 경로 표). 필수 `@edit` 참조 대상이 제거된 component도 같은 방식으로 격리한다(§4.3).
- **예산:** 엔진이 실행 조각을 시작할 때마다(호출·재개) 명령어 수 예산을 새로 부여하고 그 조각과 조각이 동기 호출한 함수가 공유한다. 예산은 플랫폼 allocator가 아닌 논리 단위(명령어 수, 원소 수 × 원소 크기)로 계산하므로 모든 플랫폼·빌드·재시뮬레이션에서 결과가 같다.
- **보고:** fault는 (모듈 hash, 함수, 위치, 종류)별로 1회 상세 보고하고 이후 횟수만 집계한다. Tryout은 새 fault 위치에서 일시 정지한다. 출시 빌드는 hash·위치를 기록해 오프라인에서 소스 위치로 변환한다.
- 격리는 Sparks 쪽 상태이며 World는 모른다. 스크립트 bridge는 격리된 instance에 대한 엔진의 `SetEnabled(true)`·OnEnable을 무시하고, Tick 중지는 `SetTickEnabled(false)`로 구현하므로 Disable·Enable로 풀리지 않는다. 이 bridge 계약을 ER 7절에 추가한다(§17.2).
- 개발·출시 빌드와 두 backend(VM·AOT)는 같은 검사를 수행한다. 게임 실행에서 이미 엔진에 요청한 외부 효과는 되돌리지 않는다(전체 계획 §23.4). 편집기 문맥은 §8.4를 따른다.

### 8.4 오류 방어 경계

사용자 코드의 어떤 실수도 process·편집기·다른 instance·저장 데이터를 망가뜨리지 않는 것이 목표다(SD32). 범위는 컴파일 → 로드 → 실행 → 편집기 → 출시 순으로 좁혀 막는다.

| 경계 | 규칙 |
|---|---|
| 컴파일 | 오류가 있으면 실행 중인 게임은 마지막 성공 revision을 유지한다. 오류가 있는 상태로 Tryout을 시작하면 마지막 성공 revision으로 실행할지 묻고, 성공 revision이 없으면 그 패키지의 클래스를 데이터 보존 placeholder로 둔다. 쿡은 실패한다 |
| 로드 | `.sparkc`가 verifier에서 거부되거나 interface·binding hash가 맞지 않으면 그 모듈의 클래스를 쓰는 prefab은 원래 필드를 보존하는 placeholder component로 로드하고 게임은 계속 실행한다(§15의 placeholder 규칙 확장) |
| 실행 | fault는 실행 트리 → instance 순으로만 번진다(§8.3). VM 내부 불변식 위반(verifier를 통과한 bytecode의 VM 버그)만 엔진 버그로 크래시 보고하며 사용자 코드 원인은 모두 fault다 |
| native 경계 | 스크립트에 노출하는 C++ 함수는 호출자 입력(인자·대상 상태)으로 Fatal·`check`·assert하지 않고 `Result`나 `ContractViolation`으로 돌려준다. binding thunk는 schema가 선언한 전제조건(non-null·범위·lane·권한·PendingDestroy 여부)을 호출 전에 모두 검사한다. 노출 함수의 인자 검증에 Fatal을 쓰면 policy 검사로 막는다(binding schema `NoFatalOnInput`, §10.2). Borrow는 thunk 안에서만 얻고 fault로 빠져나갈 때도 RAII로 반환한다 |
| 권한 | `@net` 필드 쓰기는 권한 구역(§14)에서만 컴파일된다. 실행 시 권한이 없으면 쓰기를 무시하고 위치별 1회 보고한다(fault 아님 — 클라이언트 연출이 멈추지 않게) |
| 폭주 | 무한 반복은 명령어 예산, 깊은 재귀는 호출 깊이 256과 native stack 검사, 대량 생성은 조각당 1,024·단계당 8,192 Spawn 상한, 이벤트 폭은 전달 수 상한과 연속 이월 검사(§5.4), 로그 폭주는 호출 위치별 초당 10회 상한(초과분은 횟수만 집계)으로 막는다. Tryout에서는 같은 생성 위치의 살아 있는 Actor가 100,000을 넘으면 일시 정지하고 보고한다 |
| 편집기 | `on Validate` fault는 그 instance의 미리보기를 새 revision까지 끄고 에셋을 바꾸지 않는다. `@editorButton`·`@cheat` 실행의 fault·`try` 실패는 Undo transaction 전체를 취소하므로 에셋이 반쯤 바뀐 채 저장되지 않는다. "외부 효과를 되돌리지 않는다"(§8.3)는 게임 실행에만 적용한다 |
| 저장 데이터 | 저장 로드는 RPC와 같은 길이·값 검사를 거치고 범위 밖 값은 고쳐 보고한다(§15). 변환할 수 없는 값은 기본값으로 두고 보고하며 로드를 실패시키지 않는다 |
| 복구 | 개발 빌드에서는 격리된 instance를 그 클래스 모듈의 새 revision으로 재생성하는 편집기 명령을 제공한다(§12.4 instance 재생성 경로). 출시 빌드는 엔진 이벤트 `World.ScriptFaulted(instance, kind)`를 발행하므로 C++·WorldService가 checkpoint 복귀·메뉴 복귀로 softlock을 피할 수 있다 |
| 원격 | 원격 client가 유발한 fault는 그 connection에만 집계하고, 60초 안에 100회를 넘으면 connection을 끊는다(값은 프로젝트 설정) |
