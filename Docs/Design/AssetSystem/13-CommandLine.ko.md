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
asset-cook --engine --profile=<id> [--json]   (A4f — 프로젝트 없이 엔진 셰이더 라이브러리만, 쿠킹 루트 Engine/Saved/Cooked/<id>, 잠금은 Engine/Saved/Locks/Cooked-<id>.lock, --project·경로 인자와 함께 쓰면 사용법 오류 — 10.3절 7항)
asset-load-cooked --profile=<id> <AssetId|경로 힌트>
nugget-gc --max-bytes=<n> --unused-days=<n>   (Rockpile backend면 `rockpile gc`로 위임 — --max-bytes → maxBytes, --unused-days → maxUnusedDays)
공통 옵션: --project=<dir>(기본: 현재 디렉터리에서 위로 *.rocksproject 탐색), --wait=<초>(쓰기 잠금 대기, 기본 0)
```

문법:

- 옵션은 값이 있는 `--<이름>=<값>`과 값 없는 bool 플래그 `--<이름>`(`--force`·`--verify-nuggets`) 두 형태다. 공백으로 나눈 값·짧은 옵션은 없다.
- 설정(`[설정…]`)은 `--<속성>=<값>`, `asset-new`·`asset-set`의 속성은 `<속성>=<값>`이다. 값 표기·해석은 리플렉션 공용 codec `ReflectionText`가 맡는다([리플렉션 B9](../ReflectionExtensions.ko.md) 10절 — 대입은 Prepare로 모두 검증한 뒤 한꺼번에 게시하고, 오류는 모두 모아 보고한다). `<Type>`은 타입 도구 이름(한정하지 않은 C++ 이름, 충돌하면 `Name="…"` — ReflectionExtensions 5.2절)이다. 속성 이름은 `ROCKS_PROPERTY(Name="…")`로 노출한 이름(텍스처는 UE C++ 이름, 대소문자 무시)이고 구조체 필드는 `<속성>.<필드>`(예: `SourceColorSettings.ColorSpace=sRGB`)다. 모르는 이름은 `UnknownProperty`.
- 값: enum은 열거자 이름(대소문자 무시, UE 접두사는 붙여도 되고 빼도 된다 — 12.4절), 정수·실수는 10진, bool은 `on`·`off`·`true`·`false`, 문자열은 그대로, 참조는 논리 경로 또는 36자 AssetId(`none` = null), 배열은 쉼표 목록(빈 배열은 빈 값). 리플렉션 B9가 더하는 표기(ReflectionExtensions 10.4절): Map은 `k:v` 쉼표 목록(처음 나오는 escape되지 않은 `:`에서 나눔), Flags enum은 `A+B`, Map 항목·Set 원소는 경로 key 단계 `<속성>[키]`(특수 문자가 있을 때만 큰따옴표), struct 기본값은 `{}`, Optional unset은 `--unset=<경로 목록>`, Map 키·Set 원소 제거는 `--erase=<경로 목록>`(목록 옵션이라 같은 옵션 두 번에 해당하지 않는다), `none`은 참조·클래스 참조·InstancedStruct의 null 전용이다. 대입은 `[...]`·따옴표 밖의 첫 `=`에서 나눈다. `asset-info` 기본 출력은 `asset-set`에 그대로 넣을 수 있는(값 보존) `<경로>=<값>` 줄이고, `--json`은 JSON, `--all`은 보고용 전체 출력이다(ReflectionExtensions 10.4절).
- 목록 구분자는 `,`, `--cube-array`의 cube 구분자는 `;`, `--cube`의 면 이름 구분자는 `:`(앞 2자 `px`…`nz`만 면 이름으로 본다). 값 안의 `\,`·`\;`만 이스케이프(→ `,`·`;` — 리플렉션 B9가 Map 키 안의 `\:`와, 경로 key 따옴표 안의 `\\`·`\"`·`\n`·`\r`·`\t`(UE `ParseQuotedString`의 escape 가운데 다섯)를 더한다 — ReflectionExtensions 1.5절)이고 그 밖의 `\`는 글자 그대로다(Windows 경로 `C:\Art\a.png`·UNC `\\server\share\a.png`는 그대로 쓴다). 논리 경로는 경로 정책상 `,`·`;`·`\`를 가질 수 없다. `;`가 든 인자는 셸 구분자이므로 따옴표로 감싼다.
- 위치 인자 원본은 하나뿐이다. `--cube-layout`·`--volume-tile`은 위치 인자 원본과 함께, `--cube`·`--array`·`--cube-array`·`--volume`은 위치 인자 원본 없이 쓴다.
- `asset-reimport`의 원본 재지정: 원본이 하나면 `--source=<파일>`, 여럿이면 역할마다 `--source.<역할>=<파일>`(역할은 5.4절: `px`…`nz`, slice `0`…, CubeArray `<i>.<면>`). 기록에 없는 역할은 `InvalidImportOption`.
- 종료 코드: 0 성공(경고만 있어도 0), 1 실행 실패(진단은 표준 오류에 `<진단 코드>: <path>: <설명>` 한 줄씩 — 임포터가 거부한 옵션 값은 `InvalidImportOption`), 2 사용법 오류(모르는 명령·옵션, 같은 옵션 두 번, 값이 필요한 옵션에 값 없음, 위치 인자 수·조합 오류).
- 쓰기 잠금(8절)을 잡는 명령: `asset-import`·`asset-reimport`·`asset-new`·`asset-set`·`asset-move`·`asset-duplicate`·`asset-delete`·`asset-fix-duplicate-id`·`asset-alias-remove`·`asset-alias-prune`·`asset-orphans-delete`·`asset-index-rebuild`(캐시 기록)·`asset-cook`(쿠킹 동안 에디터 패키지가 바뀌지 않게, 쿠킹 출력 보호). 나머지는 잠금 없이 읽는다(캐시는 읽기만).

서비스 API(`AssetService`, AssetDatabase)는 명령과 1:1 함수이며 에디터가 나중에 같은 함수를 쓴다. A2 명령은 그 서비스 API가 생기는 하위 단계에서 함께 더한다(7.1절 4 표).
