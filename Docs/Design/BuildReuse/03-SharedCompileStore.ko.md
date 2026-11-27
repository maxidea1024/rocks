## 3. 공유 compile 결과 저장소(2순위)

### 3.1 목표와 범위

목표:
- 새 작업 사본의 첫 빌드와, 같은 작업 사본의 다른 타깃 빌드가 이미 다른 곳에서 만든 같은 입력의 compile 출력을 받는다.
- **틀린 출력을 내지 않는다.** 지역 증분과 같은 판정(선언 입력 + 의존 기록 검증)을 거친 출력만 쓴다. 받은 출력은 byte 해시로 검증한다.
- 저장소가 없거나, 깨졌거나, 바쁘거나, 꺼져 있어도 빌드는 지금처럼 컴파일한다. 저장소 때문에 빌드가 실패하거나 기다리지 않는다.
- 여러 세션의 여러 프로세스가 동시에 읽고 써도 안전하다. 전역 잠금으로 빌드를 줄 세우지 않는다.

범위:
- 대상 action: compile(단독·묶음·공개 헤더 검사·생성 소스), ISPC, 메타 action(2장 — 3.7절). PCH·archive·link·BuildInfo 생성 TU·`--timing` 실행은 넣지 않는다(BR17·BR18).
- 대상 플랫폼: Windows clang-cl 먼저. macOS·Android·Asan·Coverage·Release(ThinLTO)는 결정성 시험(5.1절 T2)을 통과할 때까지 경로 결속 action이다(BR15).
- 범위 밖: 원격·분산 실행, 기계 사이 공유.

### 3.2 저장소 배치

- 위치: Windows `%LOCALAPPDATA%\Rocks\out\CompileStore\`, macOS `~/Library/Caches/Rocks/CompileStore/`(OS가 지워도 다시 컴파일하면 된다 — ThirdParty 저장소와 달리 받을 원본이 없는 재생성물이다). `ROCKS_COMPILE_STORE_DIR`로 바꾼다(시험·다른 드라이브). 툴체인이 이미 `%LOCALAPPDATA%\Rocks\out\toolchains`에 있고 모든 작업 사본이 그것을 쓴다(실측: 301개 폴더의 툴체인 지문이 같다).
- 사용자 단위다. 같은 사용자의 프로세스만 쓰므로 신뢰 경계는 작업 사본과 같다.
- 구조(`v1`은 저장소 형식 판이다):

```
CompileStore/v1/
  entries/<p0p1>/<1차 key 64자>/<후보 key 64자>.entry
  blobs/<b0b1>/<SHA-256 64자>.blob
  tmp/<Guid>.tmp
  collect.lock        (정리 전용 try-lock, 지우지 않는다)
  collect.stamp       (마지막 정리 시각)
```

- 후보 key = SHA-256(정규화 의존 목록과 내용 해시, include 검증 기록). 같은 입력으로 같은 결과를 낸 두 프로세스는 같은 파일 이름을 만든다.
- 항목 파일은 JSON이다: 형식 판, 1차 key(echo), 정규화 의존 목록(경로, 내용 해시), include 검증 기록(3.3.3절), 출력 목록(출력 순번, blob 해시, 크기), 컴파일러 stdout·stderr(재생용 — 경로는 1차 key와 같은 규칙으로 `<R>`·`<O>`로 정규화해 두고 복원 때 지금 경로로 되돌린다), 본문 SHA-256. 만든 사용자 이름·작업 사본 경로 같은 개인 정보는 넣지 않는다.

### 3.3 key

#### 3.3.1 1차 key

지역 선언 입력 key(`NativeAction.Key`)와 같은 항목을 같은 순서로 담되 경로를 정규화한다.

- 머리: 저장소 형식 판, **compile 의미 판**(ActionGraph의 인자 의미가 바뀌면 올리는 상수), action 종류, 도구 경로와 툴체인 key(`Fingerprints.Toolchain` — 컴파일러·archiver·linker·`FingerprintPaths`·런타임 파일 내용, keyed 환경).
- 인자 전체: FP 인자(`-ffp-contract=off` 등), 정의(`/D`), 언어·최적화·CRT·sanitizer·coverage·ThinLTO 인자를 하나도 빼지 않는다. 빼는 것은 실행 때 바뀌는 자리 표시(`{OUTPUTn}`·`{DEPFILE}`)뿐이고, 자리 표시는 그대로 key에 든다.
- identity(`ContextFingerprint`·구성·최적화·구성원 목록), 입력 파일 내용(소스·묶음 파일·정의 헤더·리플렉션 생성 소스), 선언 추가 입력 내용(ISPC 생성 헤더 등).
- 소비 PCH: `.pch` 출력 해시 대신 **PCH action의 1차 key**를 넣는다(입력 전파 — UE `PropagateInputs`와 같은 생각). PCH에 든 헤더 내용은 소비 TU의 의존 기록에 들어 있어 3.3.3절이 검증한다(clang depfile은 PCH 입력 헤더를 함께 적는다 — material·wt-ut5 기록으로 확인, UnityBuildPch 4.1절). PCH의 탐색 디렉터리 목록(순서 포함)도 소비 key에 넣는다: PCH 안 헤더는 PCH를 만들 때의 탐색 순서로 풀렸으므로, 소비 TU의 정밀 include 검증은 그 헤더들에 대해 PCH의 목록으로 본다. 소비 TU와 PCH의 탐색 목록이 다르면(앞부분이 같지 않으면) 그 소비 TU는 저장소를 쓰지 않는다(조회·게시 모두 — 지역 증분만).
- 헤더 이름 목록은 1차 key에 넣지 않는다. 3.3.3절이 대신한다.
- 경로 결속 action(BR15)이면 정규화 전 작업 사본 루트를 더한다(그 action은 같은 작업 사본의 타깃 사이에서만 공유된다).

#### 3.3.2 정규화

- 경로 토큰에서 접두 둘만 바꾼다: 출력 폴더(`<out>`) → `<O>`, 작업 사본 루트 → `<R>`. 긴 접두를 먼저 본다(출력 폴더는 루트 아래다). 비교는 디스크 철자(`PathSpelling.OnDisk` — UnityBuildPch 4.4절) 기준이다.
- 그 밖의 절대 경로(툴체인, `%LOCALAPPDATA%\Rocks\ThirdParty`, Windows SDK)는 그대로 둔다. 기계 공용 경로라 그대로 같다.
- 인자 안에 섞인 경로(`/I<경로>`·`/FI<경로>`·`-fmacro-prefix-map=<경로>=`·`-I` 다음 인자)도 같은 규칙이다. 접두 다음 글자가 구분자일 때만 바꾼다(`C:\work\rocks2`가 `C:\work\rocks`로 바뀌지 않게).
- 정규화한 1차 key가 다른 두 action은 절대 같은 항목을 보지 않는다. 정규화가 실수로 두 다른 입력을 같게 만들 수 있는 유일한 길은 `<O>`다: 출력 폴더 경로가 결과 byte에 의미로 들어가면 안 된다. 그래서 출력 폴더 경로가 코드에 남는 길을 막는다(3.4.1절 `-fmacro-prefix-map=<out>\=` 추가). 디버그 정보에만 남는 것은 허용한다(3.4.2절 한계).

#### 3.3.3 후보 검증(2단)과 정밀 include 검증

조회는 `entries/<p0p1>/<1차 key>/` 아래 후보를 이름 순으로 본다(후보는 적다 — BR20). 후보 하나가 맞으려면 다음이 모두 참이다.

1. **의존 내용**: 후보의 의존 목록(정규화 경로)을 지금 작업 사본 경로로 되돌려, 파일마다 내용 해시가 같다(`BuildInputs.Content` 메모를 쓴다 — 한 빌드에서 파일마다 한 번 읽는다).
2. **include 해석이 같다**(정밀 include 검증): TU의 탐색 디렉터리 목록 S[0]…S[n](명령줄 `/I` 순서, 정규화)은 1차 key에 있다. 연 파일 D마다, D를 담는 **모든** 탐색 디렉터리 S[k]와 상대 이름 X = D − S[k]에 대해 (a) 앞선 탐색 디렉터리 S[j](j<k) **모두**에 X가 없고, (b) 어떤 의존 파일의 디렉터리(따옴표 include는 include 스택의 디렉터리를 먼저 본다 — clang-cl의 MSVC 호환 규칙)에도 X가 없어야 한다. D가 그 의존 파일 디렉터리에서 직접 찾아졌을 수 있으면 (b)의 그 디렉터리는 뺀다. 후보를 줄이는 최적화(첫 일치에서 멈춤 등)는 하지 않는다. 조회는 빌드가 만든 디렉터리 inventory로 하고 디스크를 다시 읽지 않는다.
   - inventory는 **확장자와 무관하게 모든 파일 이름**을 담는다. 지금 header 확장자만 담는 열거(`Fingerprints.HeaderExtensions` — `.h`·`.inl`·`.c` 등)는 `.def`, 확장자 없는 이름, include되는 `.cpp`를 놓친다(native 이름 목록에도 같은 빈틈이 있다 — SCS1이 함께 고친다). 이름 비교는 Windows·macOS에서 대소문자를 무시하고 Linux에서 구분한다(파일 시스템 규칙과 같게).
   - reparse point(symlink·junction)인 디렉터리는 받은 경로의 이름으로 본다(해석하지 않는다 — UnityBuildPch 4.4절과 같다). 탐색 디렉터리 안에 reparse point가 있어 같은 실제 파일이 두 이름으로 보이면 두 이름을 모두 inventory에 둔다. 연 파일 D의 철자가 어느 탐색 디렉터리로도 설명되지 않으면(링크를 거쳐 다른 곳으로 풀렸거나, `..`로 벗어났거나) 그 TU는 이름 목록 방식으로 떨어진다.
3. **`__has_include` 기록**: 기록할 때(컴파일 성공 뒤) 의존 파일에서 `__has_include(<…>)`·`__has_include("…")`의 글자 그대로인 이름을 뽑아, 이름마다 탐색 디렉터리별 존재 여부를 항목에 적는다. 검증 때 같은 이름의 존재 여부가 모두 같아야 한다. 다음이 의존 파일에 하나라도 있으면 그 TU는 **이름 목록 방식**(지금 규칙 — 탐색 디렉터리의 이름 목록 해시를 항목에 적고 비교)으로 떨어진다: 매크로로 만든 이름(`__has_include(MACRO)`), `__has_include_next`, `#include_next`, `#embed`·`__has_embed`, 위 2의 설명할 수 없는 경로. 실측: 엔진 원본의 `__has_include`는 `Base/PlacementNew.h`의 `<vcruntime_new.h>` 하나와 셰이더 전처리기 시험 둘이고 `#include_next`는 없다. 서드파티 5곳(Zstd·ICU·Miniaudio·OpenEXR)은 글자 그대로인 이름이다.
4. 툴체인 소유 디렉터리(툴체인 `OwnedRoots`·`FingerprintPaths`·keyed `INCLUDE`의 SDK·CRT)는 탐색 목록과 검증에서 뺀다. 툴체인 key가 대신한다(UnityBuildPch 4.1절과 같다).

성질:
- 1–3은 지역 증분 판정이 잡던 경우(새 shadow header, `__has_include` 변화, 삭제)를 모두 잡는다. 삭제는 1이 잡는다(연 파일이 없다). 새 shadow header는 2가, 존재 판정 변화는 3이 잡는다. 연 적이 없고 이름으로 찾지도 않은 헤더가 생기는 것은 결과에 영향이 없으므로 hit다.
- **지역 증분도 같은 규칙으로 바꾼다**(BR11 — UnityBuildPch 4.1절 개정). 그러면 1.5절의 과한 재컴파일이 없어지고, 지역 기록과 저장소 항목이 한 가지 의존 기록 형식(형식 2, 3.3.4절)을 쓴다.

#### 3.3.4 의존 기록 형식 2

지금 `rocks-dependency-record 1`(줄마다 해시와 절대 경로)에 include 검증 기록을 더한다: 탐색 디렉터리 목록(이미 인자에 있으므로 해시만), `__has_include` 이름과 디렉터리별 존재 비트, 이름 목록 방식으로 떨어졌는지 표지. 저장소 항목에는 경로를 정규화해 같은 내용을 넣는다. 형식이 다르면 miss다(지금 규칙과 같다).

### 3.4 경로 독립 object(결정성)

#### 3.4.1 인자 변경

Windows clang-cl compile에 다음을 더한다(탐침 7.3절: 두 루트·두 stage 디렉터리에서 object byte가 같아졌다).

| 인자 | 없애는 것 | UE 근거 |
|---|---|---|
| `/Brepro` | COFF timestamp | `VCToolChain.cs:797–800`(clang은 늘 `/Brepro`) |
| `-Xclang -gno-codeview-command-line` | `LF_BUILDINFO`의 컴파일러 경로·명령줄 | `VCToolChain.cs:564`(VFS 모드) |
| `/clang:-fdebug-prefix-map=<루트>=.` | 줄 표·타입의 소스 절대 경로 | VFS 모드의 경로 일관화 |
| `/clang:-fdebug-compilation-dir=.` | 작업 디렉터리 기록 | `VCToolChain.cs:560` |
| `-object-file-name=<object 파일 이름>` | `S_OBJNAME`의 stage 경로 | (UE는 stage 경로가 없다) |
| `/clang:-fmacro-prefix-map=<out>\=<고정 철자>\` | 생성 파일 `__FILE__`의 프리셋 이름 | (없음) |

- `-fmacro-prefix-map=<루트>\=`는 그대로 둔다(로그·assert 위치가 지금과 같다). `-ffile-prefix-map`으로 합치지 않는다 — 그러면 `__FILE__`이 `.\Core\…`로 바뀐다.
- 출력 폴더용 매크로 매핑은 루트 매핑보다 먼저 맞아야 한다. clang이 여러 매핑 중 무엇을 고르는지(먼저 준 것·가장 긴 것·마지막 것)는 SCS2에서 시험으로 정하고(T2-3) 그 결과에 맞춰 순서를 둔다. 고정 철자는 `out\`로 하고 생성 헤더의 `__FILE__`은 `out\Generated\…`가 된다.
- object 파일 이름: 지금은 절대 소스 경로의 해시라서(`ObjectOf` — `Fingerprints.Strings([file])[..20]`) 작업 사본마다 다르다. **루트 기준 상대 경로(출력 폴더 아래 파일은 `<O>` 기준)의 해시로 바꾼다.** 그러면 action Id도 작업 사본 사이에 같아진다. 이 이름 변경, 위 인자 추가, 의존 기록 형식 2(SCS1)는 모두 모든 action을 한 번 다시 컴파일하게 하므로 **한 변경(한 병합)으로 묶어** 전체 재컴파일을 한 번만 일으킨다(하위 호환은 두지 않는다).
- link: `/pdbsourcepath:<루트>`를 더한다. lld-link가 PDB의 상대 소스 경로를 링크한 작업 사본의 절대 경로로 바꾼다(탐침: 다른 루트의 object를 링크한 PDB에 링크한 쪽 경로가 들어갔다). link는 저장하지 않으므로 작업 사본마다 다르다.
- 도구 빌드 ID(UnityBuildPch 8.7절)는 물리 인자를 정규화해 만들므로 새 인자를 정규화 규칙에 더한다. compile DB·IntelliSense 인자 변환(`IntelliSenseArguments`)은 새 인자를 뺀다.
- macOS·Android(clang++): `-fdebug-prefix-map=<루트>=.`, `-fdebug-compilation-dir=.`, 출력 폴더 매크로 매핑. Mach-O·ELF에는 timestamp가 없다. 결정성 시험(T2) 통과 전에는 경로 결속이다.
- Coverage: `-fcoverage-compilation-dir=.`(UE VFS 모드와 같다 — `UBT/Platform/Windows/VCToolChain.cs:559`)와 `-fcoverage-prefix-map=<루트>=.`를 검토한다. llvm-cov가 작업 사본 루트에서 상대 경로를 풀 수 있는지 T2에서 확인하고, 확인 전에는 경로 결속이다.
- 여러 `-f*-prefix-map`이 함께 맞을 때 clang이 무엇을 고르는지(먼저 준 것·가장 긴 것·마지막 것)는 T2-3에서 정하고, 그 결과로 인자 순서를 정한다(출력 폴더 매핑이 루트 매핑보다 먼저 맞아야 한다).

#### 3.4.2 디버깅 영향과 한계

- 실행 파일의 PDB 줄 표는 `/pdbsourcepath`로 링크한 작업 사본의 절대 경로가 된다. 다른 작업 사본에서 받은 object도 지금 작업 사본의 소스로 중단점·단계 실행이 된다. 내용이 같은 것은 의존 검증이 보장한다.
- 타입 레코드의 소스 줄(`LF_UDT_SRC_LINE`)은 상대 경로(`.\…`)로 남는다(탐침). 디버거의 정의로 이동은 소스 검색 경로가 필요할 수 있다.
- 출력 폴더 아래 생성 파일(리플렉션 생성 헤더·소스, 묶음 파일)의 디버그 경로는 object를 만든 프리셋의 출력 폴더(`out\<프리셋>\…`)를 가리킨다. 다른 프리셋에서 받은 object면 그 폴더가 없을 수 있다. 내용은 같다(의존 검증).
- `LF_BUILDINFO`가 없어지면 PDB에서 컴파일 명령을 읽는 도구(hot patch 류)가 동작하지 않는다. Rocks는 hot patch가 범위 밖이다(UnityBuildPch 3절).
- 크래시 덤프 심볼화는 PDB 줄 표를 쓰므로 영향이 없다.

### 3.5 실행 흐름과 IncrementalStore의 관계

action 하나의 판정 순서(`NativeBuildEngine`·`Executor`):

1. **지역 판정**: 지금과 같다(`IncrementalStore.IsCurrent` — 선언 입력 key, 출력, 의존 기록). hit면 끝.
2. **저장소 조회**(대상 action, 저장소 켬): 1차 key를 계산한다(지역 key와 같은 토큰을 한 번 모으고 정규화본을 따로 해시한다). 후보 검증(3.3.3절)이 맞는 후보가 있으면 blob을 action stage(`<out>/.native-tmp/<guid>/`)로 **복사**하며 SHA-256을 잰다. 해시가 항목과 다르면 그 blob과 항목을 지우고 miss다. 의존 기록은 후보의 정규화 목록을 지금 경로로 되돌려 stage에 쓴다. stdout·stderr를 재생한다. 그다음은 실행 성공과 같은 경로다: `PublishOutputs`로 출력을 게시하고 `IncrementalStore.Publish`가 지역 기록을 남긴다. 다음 빌드는 1에서 hit다.
3. **실행**: miss면 지금처럼 컴파일한다. 성공하면 출력 게시·지역 기록 뒤 그 action을 **게시 대기 목록**에 넣는다. 저장소 게시는 빌드 끝에 한다: native 엔진의 안정 검사(시작·끝 InputSnapshot, 툴체인 key, 의존 기록 밖 파일 — `NativeBuildEngine.cs:137`)를 통과한 빌드에서만 대기 목록을 꺼내고, 게시 직전에 그 action의 의존 파일을 다시 해시해 기록과 같은 것만 게시한다. 불안정한 빌드는 아무것도 게시하지 않는다. 게시 실패는 진단만 남긴다.

- PCH action은 저장소를 쓰지 않으므로 계획대로 지역에서 만든다. 소비 TU가 모두 저장소에서 왔으면 PCH를 만들 필요가 없지만, 지금은 PCH가 소비 TU보다 먼저 돈다. "소비자가 실행될 때만 PCH를 만든다"는 후속 개선이다(BR18).
- 빌드 증거(`build-evidence.json`)의 원본 안정 검사(빌드 전후 원본 digest가 다르면 종료 1)는 그대로다. 저장소는 원본 트리에 쓰지 않고, 받은 출력도 지역 의존 검증을 거친다.
- action 보고(`action-report.json`)와 요약 줄에 `restored` 수와 miss 원인 수(항목 없음·의존 불일치·include 검증 불일치·blob 없음·손상·저장소 꺼짐·IO 오류), 받은 byte, 저장소에 쓴 시간을 더한다. `Native build: PASS; executed=…; cached=…; restored=…`.

### 3.6 동시성·손상·정리

#### 3.6.1 게시

- blob 먼저, 항목 나중이다. 항목이 가리키는 blob은 게시 시점에 늘 있다.
- blob: `tmp/<Guid>.tmp`에 쓰고 닫은 뒤 해시를 다시 확인하고 `File.Move(…, overwrite: false)`로 `blobs/<b0b1>/<해시>.blob`에 옮긴다. 대상이 이미 있으면 같은 내용이므로(이름이 내용 해시) tmp를 지우고 성공이다. 이동이 AccessDenied(삭제 대기 중인 같은 이름)면 이번 게시를 건너뛴다.
- 항목: 같은 방식으로 `<후보 key>.entry`에 옮긴다. 이미 있으면 같은 후보다 — tmp를 지우고, 기존 항목과 blob의 사용 시각을 갱신한 뒤 성공이다(정리가 방금 다시 만든 결과를 지우지 않게). 두 세션이 같은 TU를 동시에 컴파일하면 둘 다 컴파일하고 하나만 남는다. **서로 기다리게 하는 "진행 중" 표지는 두지 않는다**(BR21 — UI 세션 요구: 잠금·다른 프로세스 대기로 "Process/pipe deadline expired"가 나면 안 된다).
- 저장소에서 받은 action은 다시 게시하지 않는다(사용 시각만 갱신한다, 3.6.3절).

#### 3.6.2 읽기와 손상

- 항목·blob은 이동으로만 나타나므로 반쯤 쓴 파일을 읽지 않는다. 읽기는 `FileShare.Read | FileShare.Delete`로 열어 정리가 동시에 지워도 읽기가 끝까지 간다(Windows delete-pending).
- 항목: 크기 상한(1 MiB), JSON 해석, 본문 SHA-256, echo된 1차 key가 모두 맞아야 쓴다. 하나라도 틀리면 그 항목을 지우고 다음 후보로 간다.
- blob: 복사하며 잰 해시가 이름과 다르면 지우고 miss다. 크기가 항목과 다르면 열지 않고 miss다.
- 저장소 IO 오류(디스크 가득, 권한, 공유 위반)는 모두 miss·게시 생략이다. 빌드 실패로 올리지 않는다. 같은 빌드에서 IO 오류가 연달아 나면(기본 32회) 그 빌드의 남은 action은 저장소를 끈다(진단 한 줄).
- 중간에 프로세스가 죽으면 `tmp`의 파일만 남는다. 정리가 24시간 지난 `.tmp`를 지운다.

#### 3.6.3 사용 시각과 정리

- 사용 시각은 파일 수정 시각이다. hit한 항목과 blob의 시각을 갱신한다. 쓰기 비용을 줄이려고 시각이 1시간보다 오래된 것만 갱신한다.
- 정리(`Collect`)는 **개수로 정한 증분 정리**다: 빌드가 끝날 때 `collect.lock`을 기다리지 않고 잡을 수 있으면 shard(`<p0p1>`·`<b0b1>` 디렉터리) 하나를 정리하고, 다음 정리할 shard 번호와 합계 추정치를 `collect.stamp`에 남긴다. 한 번에 한 shard라 빌드 끝의 비용이 작고 일정하다. 256번이면 한 바퀴다. 못 잡으면 건너뛴다(다른 프로세스가 정리 중). 잠금 파일은 지우지 않는다([에셋 너겟](../AssetSystem/09-AssetNuggets.ko.md) 9.1절 결정 8과 같은 이유).
- shard 하나의 절차: 그 shard의 항목·blob 크기를 다시 세어 합계 추정치를 고친다. 추정치가 상한(기본 32 GiB, `ROCKS_COMPILE_STORE_MAX_GB`)을 넘으면 그 shard에서 사용 시각이 오래된 항목부터, 어느 항목도 가리키지 않는 blob(1시간보다 오래된 것만 — 막 게시해 항목이 아직 없는 blob을 남긴다)을 오래된 것부터 지운다(shard마다 상한/256의 90%까지). 1차 key 하나의 후보가 상한(8개)을 넘으면 오래된 후보를 지운다. 오래 쓰지 않은 것(기본 14일)도 지운다. 시각 규칙은 주입한 시계(`TimeProvider`)로 판단해 시험이 시간을 기다리지 않는다.
- 정리 중 다른 프로세스가 같은 항목을 읽거나 게시해도 무해하다: 지운 뒤 게시는 새로 생기고, 읽기는 blob이 없으면 miss와 항목 삭제다.
- 규모(실측): Commandlet Debug 한 번의 object가 2,872개·491 MiB다. 32 GiB면 서로 다른 입력의 전체 빌드 60번쯤을 담는다. C: 여유 공간은 523 GB다.

### 3.7 메타 결과 공유(SCS5)

- 메타 action(2.2절)의 기록은 native compile과 같은 구조(선언 입력 key + 의존 기록 + 출력 해시)다. 그래서 같은 저장소에 같은 방식으로 넣는다: 1차 key는 2.3절의 key를 3.3.2절 규칙으로 정규화한 것이고, 후보는 정규화한 의존 기록(열거·존재 확인 포함)이며, blob은 생성 파일·descriptor·binding schema다.
- 경로 독립인지 따로 확인해야 한다. 생성 헤더·생성 소스는 루트 기준 include 철자라 절대 경로가 없다(1.8절 실측). 그러나 **identity inventory는 원본 절대 경로를 담는다** — 저장할 때 `<R>`로 정규화하고 복원할 때 지금 루트로 되돌리거나, 저장소에서 빼고 복원 뒤 그 모듈의 `ids verify`만 다시 돈다. 권장은 정규화다(descriptor·binding schema에도 같은 검사를 T5-9에서 한다).
- 도구 ID는 도구 디렉터리 내용이다(2.3절). 작업 사본마다 따로 빌드한 MetaCompiler의 어셈블리·PDB가 빌드 경로를 담으면 작업 사본 사이 도구 ID가 달라 공유되지 않는다. 그래서 도구 프로젝트를 경로 독립으로 빌드한다: `Deterministic`(.NET SDK 기본), `ContinuousIntegrationBuild=true`, `PathMap=$(RepoRoot)=/_/`(`Programs/Directory.Build.props`). T5-9가 두 루트에서 빌드한 도구 byte를 비교한다.
- 새 작업 사본의 첫 빌드는 지역 기록이 없어 메타 단계 전체(512초 — 1.2절)를 돈다. 가까운 commit의 다른 작업 사본이 같은 입력으로 만든 모듈은 이 저장소에서 받는다. 받은 모듈은 지역 기록도 남겨 다음 빌드부터는 지역 hit다. 저장소 게시는 메타 단계의 안정 검사(2.6절)를 통과한 실행에서만 한다(B3와 같은 규칙).
- `meta-ids-set`은 출력이 없어 저장소에 넣지 않는다(검사 자체가 수 초다).

### 3.8 시험 환경

- 시험 하네스(Rocks.Build.Tests·IntegrationTests·ToolTests·CI)의 기본은 저장소 **off** 또는 시험마다 만든 임시 `ROCKS_COMPILE_STORE_DIR`다. 사용자의 실제 저장소(`%LOCALAPPDATA%\Rocks\out\CompileStore`)를 시험이 읽거나 쓰지 않는다.
- CI 작업 흐름(`.github/workflows/foundation.yml` 등 빌드를 부르는 모든 작업 흐름)에 `ROCKS_COMPILE_STORE=off`와 `ROCKS_META_INCREMENTAL`의 값을 명시한다(8.1절).
