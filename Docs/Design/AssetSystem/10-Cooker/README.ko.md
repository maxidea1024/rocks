## 10. Cooker

장별 파일(2026-11-25 — 500줄을 넘어 폴더로 옮겼다, 내용 변경 없음):

- [10.1 쿠킹 매니페스트 v1 구현에서 정한 점(A1b)](10.1-ManifestV1.ko.md)
- [10.2 A4 구현 세부](10.2-A4Details.ko.md)
- [10.3 A4f 셰이더 라이브러리 연동 설계](10.3-ShaderLibrary.ko.md)
- [10.3.1 A4f 구현 세부](10.3.1-A4fDetails.ko.md)

- `TargetProfile`(엔진 `Engine/Config/TargetProfiles/<id>.json` + 프로젝트 재정의): id, 그래픽 backend, 형식 지원 표(비압축, 블록 압축 지원 = 이번에는 없음), LOD 그룹 표(12.4.5절), bulk 정렬. 엔진 기본: `Windows`, `Android`(형식 표만, 그래픽 backend 후속). JSON은 리플렉션 공용 codec `ReflectionText`(struct 멤버 병합, Map은 선택 `MergeByKey` — ReflectionExtensions 10.4절)로 읽고, 프로젝트 재정의를 그룹 단위로 덮는 방식은 A4가 고른다.
- `CookSettings`(`Config/CookSettings.json`): AlwaysCook·NeverCook(경로·디렉터리), PrimaryAsset 규칙(타입 → 디렉터리), `CookSoftReferences`. 같은 codec으로 읽는다. bundle은 규칙이 아니라 PrimaryAsset의 Soft 참조 필드 지정자에서 온다(저장 때 색인 태그 — 10.2절 결정 9, 2026-11-23 교차 검증으로 고침).
- 대상: 전체 쿠킹은 (AlwaysCook ∪ PrimaryAsset) → Hard 전이 ∪ (설정 시) Soft 전이. 부분 쿠킹은 (인자 경로) → Hard 전이 ∪ (설정 시) Soft 전이 ∪ 역방향 Build 폐포(원본 Texture2D를 고치면 그것을 묶는 Texture2DArray도 다시 쿠킹) ∪ 기존 매니페스트에서 cookKey가 달라진 항목. 부분 쿠킹은 에디터에서 사라진 에셋의 매니페스트 항목을 지우고, 그것을 하드 참조하는 남은 항목은 `MissingDependency`로 실패시킨다. 공통: EditorOnly 제외, NeverCook에 걸리면 `NeverCookReferenced`. 규칙에도 참조에도 없는 에셋은 쿠킹하지 않는다.
- 처리: 타입별 `CookProcessor`(에셋 너겟 사용)가 에디터 객체 → 쿠킹 객체·bulk. 쿠킹 데이터는 같은 타입에서 EditorOnly 필드를 뺀 것이다(EditorOnly 필드는 런타임 바이너리에 있고 쿠킹 데이터에 기록·로드되지 않는다). 항목마다 **cookKey**를 계산한다. 지역 입력 = (에디터 패키지 콘텐츠 해시, 논리 경로(경로 힌트·디렉터리 기반 NeverCook·PrimaryAsset 판정이 바뀐다), 그 항목에 적용된 CookSettings 판정(PrimaryAsset·bundle), 처리기 build key들, 쿠커 버전, 쿠킹 패키지 formatVersion·섹션 schemaVersion 집합, 타입 스키마 버전 표 SHA-256, 패키지 export 구체 클래스별 (TypeId, 클래스 기본값 지문) 정렬 목록(A5 — base 기본값은 파생 레코드에 이미 들어 있다, RuntimeClassMetadata '클래스 기본값 결정'), 프로파일 출력 설정 SHA-256). 엔진 버전은 지역 입력이 아니다 — 쿠커 출력 의미가 바뀌면 쿠커 버전(엔진 버전과 독립된 정수)을 올린다([ReflectionFoundations](../../ReflectionFoundations.ko.md) 8.2절). 하드 참조 순환을 위해 SCC 단위로 계산한다: SCC key = SHA-256(구성원 지역 입력을 AssetId 순으로, SCC 밖 하드 의존 튜플 (구성원 AssetId, 의존 AssetId, 그 cookKey)의 정렬 목록), cookKey = SHA-256(SCC key, AssetId). 순환이 없어도 같은 식이다.
- 출력: 쿠킹 루트 `<Project>/Saved/Cooked/<Profile>/` 아래 `CookedManifest.rman`과 `Packages/`. 패키지 파일 이름은 내용 주소(4.2절)라 게시된 파일을 덮어쓰지 않는다.
- 게시:
  1. 각 패키지를 `.staging/`에 기록·Flush한 뒤 `Move(no-replace)`로 `Packages/<a>/`에 놓는다. 같은 이름이 이미 있으면 크기·SHA-256을 확인해 같으면 임시 파일만 지우고, 다르면(손상) `CorruptEntry` 경고 후 `Replace`로 바꾼다.
  2. 바뀐 `Packages/<a>/`와 `Packages/`, 그리고 이번 게시에서 새로 만든 상위 디렉터리(첫 쿠킹이면 쿠킹 루트 — 그 안의 `Packages` 항목)를 안쪽부터 `FileSystem::SyncDirectory`(A0-P)로 동기화한다 — 매니페스트보다 패키지 이름이 먼저 매체에 남아야 한다. POSIX는 디렉터리 fsync(macOS `F_FULLFSYNC`), Windows는 항목 추가 권한으로 연 디렉터리의 `FlushFileBuffers`다(관리자 권한 불필요. 로그 순서에 기대지 않으므로 `Packages`가 다른 volume을 가리키는 link여도 순서가 지켜진다). 디렉터리 동기화를 지원하지 않는 FS는 그 FS의 보장이 전부라 성공이다. `NotDurable`은 게시를 멈추고(매니페스트를 바꾸지 않는다) 오류로 보고한다. `Unsupported`(wrapper 기본 구현)도 오류다.
  3. 매니페스트를 임시 파일에 써서 Flush하고 `Replace`로 한 번에 바꾼다(POSIX는 쿠킹 루트도 동기화된다).
  매니페스트 교체가 유일한 전환점이므로 reader는 이전 매니페스트(이전 파일이 모두 있음) 또는 새 매니페스트(새 파일이 모두 있음)만 본다. 실패하면 매니페스트는 그대로이고 놓인 새 파일은 다음 전체 쿠킹이 지운다.
- 전체 쿠킹(`asset-cook --profile=X`)은 매니페스트를 바꾼 뒤 새 매니페스트가 가리키지 않는 패키지 파일을 지운다(이전 매니페스트로 이미 실행 중인 게임은 그 파일을 `NotFound`로 받는다 — 개발용 쿠킹 디렉터리의 한계이며 배포본은 바뀌지 않는다). 부분 쿠킹은 대상만 다시 만들고 기존 매니페스트에 병합하며 패키지 파일은 지우지 않는다(사라진 에셋의 매니페스트 항목만 뺀다). 병합은 기존 매니페스트의 프로파일 id·프로파일 출력 설정 SHA-256·CookSettings SHA-256·쿠커 버전이 모두 같을 때만 하고, 다르면 `IncompatibleCook`(전체 쿠킹 필요)이다. 처리기·포맷·타입 스키마·툴체인 변화는 항목별 cookKey가 잡는다(위 지역 입력).
- 매니페스트 v1(`CookedManifest.rman`): 헤더 `{ "RMAN", 버전 1, 항목 수, 프로파일 id, 프로파일 출력 설정 SHA-256, CookSettings SHA-256, 쿠커 버전, cookedByEngineVersion(주·부·패치 — 진단용, 병합·로드 판정에 쓰지 않음) }` + 항목 `{ assetId, typeId, 경로 힌트, 패키지 SHA-256, 패키지 크기, bulkRawBytes(BulkIndex rawBytes 합 — 10.1절 5), cookKey, PrimaryAsset 표시, bundle 목록, 런타임 의존 목록 { assetId, kind(Hard·Soft) } }`(AssetId 정렬) + 파일 전체 CRC32. 손상은 `CorruptPackage`, 버전 불일치는 `UnsupportedVersion`. 로더는 시작할 때 매니페스트 프로파일 id가 실행 플랫폼의 프로파일과 같은지 확인하고(다르면 `IncompatibleCook`), 패키지를 열 때 크기·SHA-256을 먼저 보고 그다음 패키지 종류·assetId·대표 에셋 타입·bulk raw 합·Summary 프로파일 칸·Dependencies(Hard·Soft)를 매니페스트와 대조한다(다르면 `CorruptPackage` — 10.1절 8 `CheckCookedPackage`). SCC 계산은 매니페스트의 Hard 의존으로 로드 전에 한다.
- 도구 빌드 ID·엔진 버전: 빌드 시스템이 쿠커·에디터 도구 실행 파일마다 주입하는 값이며 진단·추적용이다(병합 조건이 아니다). 쿠킹 보고서(엔진 버전은 CookedManifest 헤더의 cookedByEngineVersion에도)에 기록하고 쿠킹 패키지 Summary·cookKey에는 넣지 않는다 — Summary에 있으면 구성 사이 쿠킹 byte 동일(시나리오 13·25)이 깨지고 쿠킹 패키지 이름(내용 주소)이 엔진 빌드마다 바뀐다.
- 런타임 쿠킹 루트: 도구(`asset-load-cooked`)는 `<Project>/Saved/Cooked/<Profile>/`. 게임은 `EngineAssets`가 부팅 CVar `asset.cooked_root`(설정 파일 또는 `--set=asset.cooked_root=<dir>`, 비면 실행 파일 디렉터리의 `Cooked/`)를 `AssetManager`에 넘긴다(11.8절 3). 쿠킹 결과를 배포 위치로 옮기는 패키징 단계는 후속이다.
- 쿠킹 bulk codec: 프로파일 설정(None·Zstd, 기본 None — 후속 스트리밍의 mip 단위 접근을 위해 entry 단위 압축).
- 결정성(범위: 같은 도구 빌드에서 호스트 무관하게 같은 바이트): 정렬된 순회, 병렬 결과 정렬 후 기록, ID는 에디터 값 복사, 패딩 0. 이미지 처리 모듈은 SIMD 분기 없이 스칼라 경로만 쓰고, 초월 함수(pow·exp2·log2·sin)와 sRGB·감마 변환은 CRT를 쓰지 않는 Core 결정적 수학(A0-C)으로 하며 쿠킹 작업 동안 부동소수점 환경 guard(A0-C)를 건다. 현재 Core `LinearColor::ToSRGB`는 CRT `pow`를 써서 호스트마다 결과가 다를 수 있으므로(Color.h 주석) A0-C에서 결정적 수학의 표·함수로 바꾼다(런타임과 쿠킹의 sRGB 구현을 하나로). 결정적 수학 버전은 너겟 키와 cookKey에 들어간다. 시험: 결정적 수학의 기준값 표, CRT 미사용 링크 검사, 에셋 너겟 빈/찬 상태 출력 비교.
- 오류: 에셋별 원인·경로를 모아 보고, 하나라도 실패하면 게시하지 않는다.
- 릴리스 콘텐츠 서명: 쿠킹 매니페스트에 서명한 봉투 `CookedManifest.rsig`, 로더 검증, `content-sign` 도구는 [콘텐츠 서명](../../ContentSigning.ko.md) 설계를 A1 구현 때 함께 넣는다(Release는 매니페스트 항목과 실행 파일 안 데이터만 읽는다).
