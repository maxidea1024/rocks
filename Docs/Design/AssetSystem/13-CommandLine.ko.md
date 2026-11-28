## 13. 명령행(Programs/Commandlet)

레지스트리 이름은 단일 토큰이라 모든 명령을 하이픈으로 이은 한 단어로 등록한다(`asset-<대상>-<동사>` 또는 `asset-<동사>`). 미커밋 `import-texture`는 `asset-import`로 대체한다.

```
asset-import <path> <source> [--cube-layout=<배치>|--volume-tile=<W>x<H>] [설정…]
asset-import <path> (--cube=<cube 지정>|--array=<파일 목록>|--cube-array=<cube 지정>;…|--volume=<파일 목록>) [설정…]
asset-import <path> --source.<역할>=<파일>… [--importer=<이름>] [설정…]
asset-reimport <path> [--source=<파일> | --source.<역할>=<파일>…] [설정…]
asset-new <Type> <path> [<속성>=<값>…]
asset-set <path> <속성>=<값> [<속성>=<값>…]
asset-list [--type=<Type>] [--under=<path>]
asset-info <path|AssetId> [--json] [--all | --import]
asset-move <path|디렉터리> <새 path>
asset-duplicate <path> <새 path>
asset-delete <path> [--force]
asset-fix-duplicate-id <path>
asset-index-rebuild
asset-index-verify
asset-alias-list
asset-alias-remove <옛 path>
asset-alias-prune
asset-orphans-list
asset-orphans-delete <path>
asset-cook --profile=<id> [<path>…] [--verify-nuggets] [--verify-nuggets-source=local|remote] [--json]
asset-cook --engine --profile=<id> [--verify-nuggets] [--json] [--wait=<초>]   (A4f — 프로젝트 없이 엔진 셰이더 라이브러리만, 쿠킹 루트 Engine/Saved/Cooked/<id>, 잠금은 Engine/Saved/Locks/Cooked-<id>.lock, --project·경로 인자와 함께 쓰면 사용법 오류 — 10.3절 7항)
asset-load-cooked --profile=<id> [<AssetId|경로 힌트>] [--verify-sidecars]
nugget-gc --max-bytes=<n> --unused-days=<n>   (Rockpile backend면 `rockpile gc`로 위임 — --max-bytes → maxBytes, --unused-days → maxUnusedDays)
공통 옵션: --project=<dir>(기본: 현재 디렉터리에서 위로 *.rocksproject 탐색), --wait=<초>(쓰기 잠금 대기, 기본 0)
```

문법:

- 옵션은 값이 있는 `--<이름>=<값>`과 값 없는 bool 플래그 `--<이름>`(`--force`·`--verify-nuggets`) 두 형태다. 공백으로 나눈 값·짧은 옵션은 없다.
- 설정(`[설정…]`)은 `--<속성>=<값>`, `asset-new`·`asset-set`의 속성은 `<속성>=<값>`이다. 값 표기·해석은 리플렉션 공용 codec `ReflectionText`가 맡는다([리플렉션 B9](../ReflectionExtensions.ko.md) 10절 — 대입은 Prepare로 모두 검증한 뒤 한꺼번에 게시하고, 오류는 모두 모아 보고한다). `<Type>`은 타입 도구 이름(한정하지 않은 C++ 이름, 충돌하면 `Name="…"` — ReflectionExtensions 5.2절)이다. 속성 이름은 `ROCKS_PROPERTY(Name="…")`로 노출한 이름(텍스처는 UE C++ 이름, 대소문자 무시)이고 구조체 필드는 `<속성>.<필드>`(예: `SourceColorSettings.ColorSpace=sRGB`)다. 모르는 이름은 `UnknownProperty`.
- 값: enum은 열거자 이름(대소문자 무시, UE 접두사는 붙여도 되고 빼도 된다 — 12.4절), 정수·실수는 10진, bool은 `on`·`off`·`true`·`false`, 문자열은 그대로, 참조는 논리 경로 또는 36자 AssetId(`none` = null), 배열은 쉼표 목록(빈 배열은 빈 값). 리플렉션 B9가 더하는 표기(ReflectionExtensions 10.4절): Map은 `k:v` 쉼표 목록(처음 나오는 escape되지 않은 `:`에서 나눔), Flags enum은 `A+B`, Map 항목·Set 원소는 경로 key 단계 `<속성>[키]`(특수 문자가 있을 때만 큰따옴표), struct 기본값은 `{}`, Optional unset은 `--unset=<경로 목록>`, Map 키·Set 원소 제거는 `--erase=<경로 목록>`(목록 옵션이라 같은 옵션 두 번에 해당하지 않는다), `none`은 참조·클래스 참조·InstancedStruct의 null 전용이다. 대입은 `[...]`·따옴표 밖의 첫 `=`에서 나눈다. `asset-info` 기본 출력은 `asset-set`에 그대로 넣을 수 있는(값 보존) `<경로>=<값>` 줄이고, `--json`은 JSON, `--all`은 보고용 전체 출력이다(ReflectionExtensions 10.4절).
- 목록 구분자는 `,`, `--cube-array`의 cube 구분자는 `;`, `--cube`의 면 이름 구분자는 `:`(앞 2자 `px`…`nz`만 면 이름으로 본다). 값 안의 `\,`·`\;`만 이스케이프(→ `,`·`;` — 리플렉션 B9가 Map 키 안의 `\:`와, 경로 key 따옴표 안의 `\\`·`\"`·`\n`·`\r`·`\t`(UE `ParseQuotedString`의 escape 가운데 다섯)를 더한다 — ReflectionExtensions 1.5절)이고 그 밖의 `\`는 글자 그대로다(Windows 경로 `C:\Art\a.png`·UNC `\\server\share\a.png`는 그대로 쓴다). 논리 경로는 경로 정책상 `,`·`;`·`\`를 가질 수 없다. `;`가 든 인자는 셸 구분자이므로 따옴표로 감싼다.
- 위치 인자 원본은 하나뿐이다. `--cube-layout`·`--volume-tile`은 위치 인자 원본과 함께, `--cube`·`--array`·`--cube-array`·`--volume`은 위치 인자 원본 없이 쓴다.
- `asset-reimport`의 원본 재지정: 원본이 하나면 `--source=<파일>`, 여럿이면 역할마다 `--source.<역할>=<파일>`(역할은 5.4절: `px`…`nz`, slice `slice:0`…, CubeArray `cube:<i>.<면>`). 기록에 없는 역할은 `InvalidImportOption`.
- 종료 코드: 0 성공(경고만 있어도 0), 1 실행 실패(진단은 표준 오류에 `<진단 코드>: <path>: <설명>` 한 줄씩 — 임포터가 거부한 옵션 값은 `InvalidImportOption`), 2 사용법 오류(모르는 명령·옵션, 같은 옵션 두 번, 값이 필요한 옵션에 값 없음, 위치 인자 수·조합 오류).
- 쓰기 잠금(8절)을 잡는 명령: `asset-import`·`asset-reimport`·`asset-new`·`asset-set`·`asset-move`·`asset-duplicate`·`asset-delete`·`asset-fix-duplicate-id`·`asset-alias-remove`·`asset-alias-prune`·`asset-orphans-delete`·`asset-index-rebuild`(캐시 기록)·`asset-cook`(쿠킹 동안 에디터 패키지가 바뀌지 않게, 쿠킹 출력 보호). 나머지는 잠금 없이 읽는다(캐시는 읽기만).

제품 명령 실행 파일의 에셋 타입 모듈은 Animation·Mesh·Texture이고, `asset-cook`의 처리기는 텍스처(12.14절)·정적 메시·스킨 메시·애니메이션 클립([메시·애니메이션 에셋](../MeshAnimationAssets/13-Stages.ko.md) 13.1절 1)이며 그 밖의 타입은 기본 처리기다. 타입 hook은 텍스처(설정 정규화·원본 재구성)와 애니메이션(스켈레톤 Build 의존)이다.

서비스 API(`AssetService`, AssetDatabase)는 명령과 1:1 함수이며 에디터가 나중에 같은 함수를 쓴다. A2 명령은 그 서비스 API가 생기는 하위 단계에서 함께 더한다(7.1절 4 표).

### 13.1 A6 구현 세부(2026-11-25 — 착수 때 정함)

A6은 남은 명령 둘(`asset-load-cooked`·`nugget-gc`)과 문서다. 검증 보고(14절 결과 기록)는 빌드·시험 체인을 돌 수 있을 때(화면 마일스톤 뒤 — 사용자 결정 2026-11-23) 한다. 그때까지 14절에는 시나리오별 시험 이름만 대조표(14.1.x)로 둔다.

**`asset-load-cooked --profile=<id> [<AssetId|경로 힌트>] [--project=<dir>] [--verify-sidecars]`**(쿠킹 결과 확인 도구 — UE `-cookonthefly`가 아닌 단발 로드. 대상은 `--verify-sidecars`가 없으면 필수, sidecar 검증은 20.5 7):
1. 프로젝트를 연다. 잠금은 없다(읽기만 — 다른 쿠커의 게시와 겹치면 로드가 패키지 대조 `CorruptPackage`로 실패할 수 있다, 한계).
2. 쿠킹 루트는 `<Saved>/Cooked/<id>`다. 매니페스트는 `CookedManifest.rman`을 읽는다. 서명 봉투는 있어도 무시한다(있는지도 보지 않는다) — 도구는 신뢰 목록이 없고, 서명 검증은 `content-sign`·EngineAssets 몫이다. 한계: EngineAssets Release는 봉투가 필수라 이 도구의 성공이 Release 로드 성공을 뜻하지 않는다(2026-11-25 교차 검증 1).
3. 로더 구성은 EngineAssets와 같다(11.8절 4):
   - 에셋 타입 모듈(`AssetCommandTypes`).
   - `AssetManager`(에셋 객체 Registry를 만든다).
   - `AsyncIoContext` + `AsyncCookedPackageIo`.
   - `WorkerScheduler` + `WorkerSchedulerLoaderWorker`(worker 1개 — 도구라 고정, 이름 있는 상수).
   - 실행 프로파일은 `--profile` 값이다(매니페스트와 다르면 `IncompatibleCook`).
4. 대상:
   - `/`로 시작하면 경로 힌트다(로더 `FindByPath` — caseless). 없으면 `asset-load-cooked: NotFound: <대상>: error NotFound` 줄, 종료 코드 1이다.
   - 그 밖은 36자 소문자 AssetId다(`AssetId::TryParse` — 다른 명령과 같다). 문법 위반은 사용법 오류 2다.
   - `--profile`은 TargetProfiles id 문법(`[A-Za-z0-9_]`, 1–64 byte)이다 — 쿠킹 루트 경로 조각이라 위반은 사용법 오류 2다(코드 리뷰).
5. `LoadBlocking`(site `"asset-load-cooked"`):
   - 성공하면 `asset-load-cooked: <id> loaded <AssetId> (<경로 힌트>) packages <공개 패키지 수>` 한 줄, 종료 코드 0이다. 공개 패키지 수는 로더 상태(`AssetManager::Describe` → `loader.loadedPackages`)다 — 대상과 그 Hard 의존 폐포의 패키지다(요청이 하나뿐이라 그 둘이 같다).
   - 실패하면 `<진단 코드 또는 오류 이름>: <대상>: error <원인 오류>`(related가 있으면 `(related <AssetId>)`), 종료 코드 1이다.
   - 끝에 AssetManager Shutdown → scheduler·IO 종료(EngineAssets 6절 순서)를 한다.
6. 쿠킹 루트·매니페스트 읽기 실패는 `<오류>: <루트>: cannot read the cooked manifest`, 매니페스트 손상은 그 진단 코드 줄이다.

**`nugget-gc --max-bytes=<n> --unused-days=<n> [--project=<dir>]`**(9.1절 9·16):
1. 두 옵션 모두 필수다(값은 10진 정수 — `--max-bytes`는 u64, `--unused-days`는 u32). 빠지거나 문법 위반이면 사용법 오류 2다.
2. 프로젝트 너겟 설정(`ReadNuggetCacheSettings`)을 읽는다.
   - backend `off`면 `nugget-gc: the nugget cache is off` 한 줄, 종료 코드 0이다.
   - `rockpile`이면 `Unsupported: rockpile: the Rockpile nugget backend is not available` 줄, 종료 코드 1이다(Rockpile이 생기면 `rockpile gc`로 위임 — 13절).
   - `auto`·`filesystem`은 로컬 backend다.
3. 잠금은 없다. 너겟 GC는 여러 프로세스가 동시에 써도 무해하다(9.1절 9).
4. `NuggetFileSystemBackend(allocator, files, ResolveNuggetLocalPath, NuggetSystemClock)`를 `Open`한다.
   - Open 오류(루트를 만들 수 없음 등)는 `<오류>: <루트>: cannot open the nugget cache` 줄, 종료 코드 1이다.
   - Open은 성공했지만 `IsReadOnly()`(tmp를 만들 수 없는 공유 루트 등)면 `Unsupported: <루트>: the nugget cache is read-only` 줄, 종료 코드 1이다(읽기 전용 전용 오류 코드는 없다).
   - backend `Collect`도 읽기 전용이면 Unsupported다(Put과 대칭 — 9.1절 9, 2026-11-25 교차 검증 5).
5. `Collect({maxBytes, unusedDays})`가 끝나면 `nugget-gc: removed <n> (<bytes> bytes) kept <n> (<bytes> bytes) skipped <n> temporaries <n>` 한 줄, 종료 코드 0이다. 건너뛴 항목이 있어도 0이다(경고 성격 — 다음 GC가 다시 본다). `temporaries`는 Collect가 지운 지난 임시 파일 수다(Open이 먼저 지운 것은 세지 않는다 — 대개 0).
   - `--unused-days`는 u32 전체를 받는다. tick으로 바꿀 때 넘치는 값(`NuggetCollectMaxUnusedDays` 초과 — 약 1천만 일)은 backend가 "나이 제한 없음"으로 포화한다(9.1절 9 — 부호 있는 곱의 넘침을 막는다, 교차 검증 4).
6. Collect 오류(루트 열거)는 `<오류>: <루트>: cannot collect` 줄, 종료 코드 1이다.

**문서**: 13절 명령 목록의 `asset-cook --engine` 줄에 `--verify-nuggets`·`--wait`를 더한다(A4f4). 명령마다의 시험은 14.1.115 대조표다.

**A6 시험**(14.1.115):
- `asset-load-cooked`:
  - 쿠킹한 프로젝트에서 AssetId·경로 힌트(대소문자 다르게)로 로드 성공 줄을 낸다.
  - 실패: 없는 경로 NotFound, 다른 프로필 IncompatibleCook, 매니페스트 없음, 패키지 손상 CorruptPackage.
  - 사용법 오류: 대상 없음·둘, 잘못된 AssetId, 프로필 없음.
- `nugget-gc`:
  - 너겟을 쓴 뒤 `--max-bytes=0`이면 모두 지운다(결과 줄 — 바이트 상한이 결정적으로 보장한다, 실제 시계와 수정 시각 비교에 기대지 않는다).
  - 큰 상한(`--max-bytes`·`--unused-days=4294967295` — 일수 포화)이면 남긴다.
  - off·rockpile 설정 줄, 읽기 전용 루트 줄, 사용법 오류(빠짐·문법·두 번).
- backend: `Collect` 일수 포화(u32 최댓값 → 아무것도 나이로 지우지 않음), 읽기 전용 `Collect` Unsupported.
