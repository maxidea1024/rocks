## 13. 언어 서버와 편집기

LSP 서버 `sparks-lsp`를 VS Code 확장과 Rocks Studio Script Editor가 공유한다.[R15] CLI·Rocks Build·언어 서버·런타임 eval이 같은 컴파일러 라이브러리를 사용하므로 같은 입력에서 같은 진단이 도출된다(전체 계획 §23.4). 서버는 엔진 없이 binding schema와 사전 파일만으로 동작하고 별도 process이며 문서 버전별 arena를 사용한다. 디버깅은 DAP adapter가 VM 디버거에 연결한다.[R16][R17]

자동완성 위치(`|` = 커서):

~~~text
class DoorLogic : ScriptableComponent {
    @edit var RequiredKey = Tag("Item.|");        // 태그 사전
    on |                                           // Awake, Start, Tick, TriggerEnter, Owner.Interacted, State.Changed …
    on Target.|                                    // Target 타입의 이벤트와 필드 Changed
    fn Ring() {
        World.Audio.PlayAt(Asset("/Game/Audio/|"), Owner.Location);   // SoundAsset 에셋만
    }
}
~~~

- 리터럴 자동완성은 컴파일 검증과 같은 사전을 사용한다. Unity 이름(`Update`, `FixedUpdate`, `LateUpdate`, `OnEnable`, `GetComponent`)을 입력하면 Rocks 이름(`Tick`, `FixedTick`, `LateTick`, `Enable`, `Find<T>`)을 제시한다. core 패키지에서는 full 후보를 표시하지 않는다.
- hover는 타입·단위·문서 주석·native 선언 위치·하드 의존 크기·bundle·이벤트의 발행 측을 표시한다. 빠른 수정: `?` 추가, `?? return`, 누락 import, handler 골격(`@save`·`@net` 필드의 Changed 골격은 `on Enable`에 같은 반영 호출을 함께 생성), 재진입 정책 표시(`@skipWhileBusy`·`@restart`), `Soft`→`try await X.Load()`, `@formerly`, 이름 충돌 시 이름 변경.
- **편집기 명령 채널(LevelScript LD14):** 편집기(Rocks Studio)는 `sparks-lsp`에 LSP `workspace/executeCommand`(서버 명령 `sparks.levelScript.addReference`·`sparks.levelScript.addEvent`)로 "레벨 스크립트에 참조 추가"·"이벤트 추가"를 요청하고, 서버는 넣을 위치를 정해 `workspace/applyEdit`로 VS Code·Script Editor에 편집을 보낸다(서버는 파일을 직접 쓰지 않는다). 편집기는 새 revision의 컴파일 완료 통지를 받은 뒤 필드 값을 지정한다. "레벨 스크립트 열기"의 생성 경로 `<패키지>/Levels/<레벨 경로 이름>.spark`와 이름 변환 규칙은 [LevelScript](../LevelScript.ko.md) 4.8절이다.
- 도입 단계: L1 진단·의미 강조, L2 자동완성(멤버·hook·attribute·리터럴·경로)·hover·정의 이동·빠른 수정·서식·추론 타입 표시, L3 ID를 유지하는 rename·참조 찾기·VS Code 디버거.
- **디버거(DAP, L3):** `await`를 넘는 step(같은 실행의 다음 조각으로), 조건 breakpoint(Pure 문맥으로 평가), watch는 고정된 버퍼를 읽음(§9.4), breakpoint 중 저장은 재개 후 첫 reload 경계에 적용(§12.4), Tryout의 여러 process 동시 연결, pause 중 큐 상한(§9.4), 출시 빌드의 오프라인 위치 변환(§8.3·§12.5).

### 13.1 시험 작성

- `@test fn`(full 또는 패키지 manifest의 test 패키지)은 headless World fixture에서 실행한다. 시험 시계는 `NextTick`·`Delay`를 결정적으로 진행하며 실제 시간에 기대지 않는다(sleep 금지). `Assert`가 실패 판정이다.
- CLI `sparks test <패키지>`가 실행하고 결과는 진단과 같은 형식으로 보고한다. 같은 corpus를 VM·AOT 차등 시험(G6)에도 쓴다.
- **conformance corpus:** `Conformance/<영역>/<사례>.spark`와 `.expect`. `.expect`에는 진단 코드·줄·열, 또는 실행 trace(전달 순서·fault 종류·위치 ID·ledger counter)를 적는다. 본문의 "오류다" 문장과 SD마다 음성 사례를 1개 이상 둔다. 영역은 진단 코드 대역(§8.1)과 같다.
- **`.expect` 형식(LC0):** UTF-8 텍스트(처음의 BOM은 무시, UTF-8이 아니면 형식 오류)이며 한 줄에 지시 하나다. 단어는 공백이나 tab으로 나누고, 숫자는 0–9만 쓴다(`+` 부호 없음). 빈 줄과 `#`로 시작하는 줄은 주석이다. 사례 이름(stem)에는 점이 들어가도 되며 `.spark`와 `.expect`는 stem으로 짝을 맞춘다.
  - `error S0412 14:14` — 이 위치에 이 코드의 진단이 나야 한다. 여러 줄이면 §8.1의 순서대로 모두 일치해야 하며 빠지거나 더 나온 진단은 실패다.
  - `error S0103` — 위치를 적지 않으면 원문 위치가 없는 진단(파일 전체)이다.
  - `clean` — 진단이 없어야 한다. `error`와 함께 쓸 수 없다.
  - `stage syntax`·`stage imports`·`stage names`·`stage types`·`stage flow`·`stage rules`·`stage codegen` — 그 단계까지만 컴파일한다(`syntax`는 manifest·schema·모듈 경로·의존·어휘·구문 분석, `imports`는 거기에 import 해석, `names`는 거기에 이름 해석, `types`는 거기에 타입 검사 — LC2c, `flow`는 거기에 흐름·대입·미사용·상수식·필드·프로필 검사 — LC2d §3.11, `rules`는 거기에 문맥·수명 규칙 — LC3 §3.12–, `codegen`은 거기에 진단 없는 컴파일의 typed IR 내림 — LC4, [SparksBytecode](../SparksBytecode.ko.md)). 사례는 `.ir`(typed IR 덤프 golden — 진단 없는 사례만)도 둘 수 있다. 한 번만 쓰며 없으면 전체 단계다. 구문 사례(`Lexical`·`Syntax`·`SyntaxErrors` 영역)와 이름 사례(`Names` 영역)가 뒤 단계 규칙(이름·타입·미사용)에 묶이지 않게 한다(LC2b·LC2c). 단계만으로는 기대 지시가 아니다(`error`나 `clean`이 함께 있어야 한다).
  - 지시가 하나도 없는 `.expect`, 알 수 없는 지시, `.expect`가 없는 `.spark`, `.spark`가 없는 `.expect`는 corpus 오류로 실패한다(빈 기대값이 조용히 통과하지 않게 한다).
  - 실행 결과 지시(`result`·`fault`·`trace`)는 해당 마일스톤(LC5·LC6)에서 더한다.
- **패키지 사례(LC2a):** 사례는 파일 하나(`<사례>.spark`) 또는 폴더 하나(`<사례>/`)다.
  - 파일 사례는 manifest가 `{ name: "Test", sparks: 1, profile: "full" }`인 패키지의 모듈 하나(모듈 경로 `Main` — 사례 이름에는 점이 들어갈 수 있으므로 사례 이름과 무관하다)로 컴파일한다. 진단 경로는 `<영역>/<사례>.spark`이고 `.expect`의 `error` 지시에는 경로를 쓰지 않는다.
  - 폴더 사례는 폴더 바로 아래에 `SparksPackage.json5`가 있으면 그 폴더가 패키지 하나이고, 없으면 바로 아래 폴더 하나하나가 패키지다(각자 `SparksPackage.json5` 필수). 패키지 안에는 `.spark`와 `SparksPackage.json5`(루트)만 둔다. `.expect`는 폴더와 같은 stem으로 영역에 두며 `error` 지시에 사례 폴더 기준 경로(`/` 구분)를 쓴다: `error S0301 Game/Combat/Score.spark 3:8`, 파일 전체 진단은 `error S1108 Game/combat.spark`. 진단 경로도 사례 폴더 기준이다. 그 밖의 파일, 파일이 하나도 없는 사례 폴더, manifest 없는 패키지 폴더, 패키지 루트가 아닌 곳의 `SparksPackage.json5`는 corpus 오류다(모듈이 없는 패키지 — manifest만 있는 폴더 — 는 쓸 수 있다).
  - 모든 사례는 시험용 가짜 엔진 schema(`SparksTestHost/Schema/*.sparkbind.json`, 패키지 `Rocks`)를 함께 컴파일한다. 진단 경로는 `Schema/<파일 이름>`이다.
  - `.ast`(구문 트리 golden)는 파일 사례에만 둔다(폴더 사례 옆의 `.ast`는 corpus 오류).
- **이름 해석 golden(LC2b, SD58):** 사례 옆에 `<사례>.names`가 있으면 이름 해석 덤프(`AppendNameDump`, 형식은 `Compilation.h`)가 파일 내용과 바이트 단위로 같아야 한다. 파일 사례는 그 모듈의 덤프, 폴더 사례는 이름 해석을 한 모듈마다 `== <사례 폴더 기준 경로>` 줄과 그 덤프를 경로 byte 순으로 이은 것이다. 판정·실패 보고는 `.ast`와 같다.
- **식 타입 golden(LC2c, SD58):** 사례 옆에 `<사례>.types`가 있으면 식 타입 덤프(`AppendTypeDump`, §3.10)가 파일 내용과 바이트 단위로 같아야 한다. 묶는 방식(파일·폴더 사례)과 판정·실패 보고는 `.names`와 같고, 타입 검사를 하지 않은 사례(`stage`가 `types`보다 앞)에 `.types`가 있으면 실패다.
- **구문 트리 golden(LC1b, SD58):** 사례 옆에 `<사례>.ast`가 있으면 그 사례의 구문 트리 덤프(`AppendSyntaxDump`, 형식은 `SyntaxTree.h`)가 파일 내용과 바이트 단위로 같아야 한다(끝의 LF 포함). `.ast`는 선택이며 `.spark` 없이 있으면 corpus 오류이고, 원문 검사가 막아(S0101·S0103) 트리가 없는 사례에 `.ast`가 있어도 실패다. 진단(`.expect`)과 트리는 따로 판정한다. 다르면 첫 다른 줄과 실제 트리 전체를 보인다(검토 뒤 golden 갱신에 쓴다). 사례 실행은 컴파일(LC2a부터 manifest·schema·모듈 경로·어휘·구문 분석·import 해석, 마일스톤마다 늘어난다)이다. IR·bytecode 덤프 golden도 같은 방식(`.ir`·`.bc` — `.bc`는 `.sparkc` 왕복도 본다)이다. 실행 golden `.run`(LC5 — [SparksVm](../SparksVm.ko.md) 7절)은 진단 없는 파일 사례의 최상위 함수 호출을 줄마다 적고, 실행기가 `.sparkc` 왕복을 거친 모듈을 VM으로 실행해 결과(값·fault 종류와 위치·실행 끝)와 명령 수를 다시 써서 비교한다.
- corpus는 `Engine/Source/Developer/SparksTestHost/Conformance`에 두고 시험이 실행 중 엔진 디렉터리 탐색으로 찾는다. 저장소가 줄 끝을 LF로 정규화하므로 CRLF·잘못된 UTF-8·BOM처럼 바이트가 중요한 사례는 C++ 시험에 직접 넣는다.
- 제안 목표(미측정): 파일 1,000개 기준 자동완성 p95 100 ms, 편집 후 진단 p95 300 ms. 검증: 후보 golden test, prefix fuzz, CLI와의 진단 동등성, rename 후 저장 데이터 보존.
