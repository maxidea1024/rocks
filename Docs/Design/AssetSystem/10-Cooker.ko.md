## 10. Cooker

- `TargetProfile`(엔진 `Engine/Config/TargetProfiles/<id>.json` + 프로젝트 재정의): id, 그래픽 backend, 형식 지원 표(비압축, 블록 압축 지원 = 이번에는 없음), LOD 그룹 표(12.4.5절), bulk 정렬. 엔진 기본: `Windows`, `Android`(형식 표만, 그래픽 backend 후속). JSON은 리플렉션 공용 codec `ReflectionText`(struct 멤버 병합, Map은 선택 `MergeByKey` — ReflectionExtensions 10.4절)로 읽고, 프로젝트 재정의를 그룹 단위로 덮는 방식은 A4가 고른다.
- `CookSettings`(`Config/CookSettings.json`): AlwaysCook·NeverCook(경로·디렉터리), PrimaryAsset 규칙(타입 → 디렉터리), `CookSoftReferences`. 같은 codec으로 읽는다. bundle은 규칙이 아니라 PrimaryAsset의 Soft 참조 필드 지정자에서 온다(저장 때 색인 태그 — 10.2절 결정 9, 2026-11-23 교차 검증으로 고침).
- 대상: 전체 쿠킹은 (AlwaysCook ∪ PrimaryAsset) → Hard 전이 ∪ (설정 시) Soft 전이. 부분 쿠킹은 (인자 경로) → Hard 전이 ∪ (설정 시) Soft 전이 ∪ 역방향 Build 폐포(원본 Texture2D를 고치면 그것을 묶는 Texture2DArray도 다시 쿠킹) ∪ 기존 매니페스트에서 cookKey가 달라진 항목. 부분 쿠킹은 에디터에서 사라진 에셋의 매니페스트 항목을 지우고, 그것을 하드 참조하는 남은 항목은 `MissingDependency`로 실패시킨다. 공통: EditorOnly 제외, NeverCook에 걸리면 `NeverCookReferenced`. 규칙에도 참조에도 없는 에셋은 쿠킹하지 않는다.
- 처리: 타입별 `CookProcessor`(에셋 너겟 사용)가 에디터 객체 → 쿠킹 객체·bulk. 쿠킹 데이터는 같은 타입에서 EditorOnly 필드를 뺀 것이다(EditorOnly 필드는 런타임 바이너리에 있고 쿠킹 데이터에 기록·로드되지 않는다). 항목마다 **cookKey**를 계산한다. 지역 입력 = (에디터 패키지 콘텐츠 해시, 논리 경로(경로 힌트·디렉터리 기반 NeverCook·PrimaryAsset 판정이 바뀐다), 그 항목에 적용된 CookSettings 판정(PrimaryAsset·bundle), 처리기 build key들, 쿠커 버전, 쿠킹 패키지 formatVersion·섹션 schemaVersion 집합, 타입 스키마 버전 표 SHA-256, 패키지 export 구체 클래스별 (TypeId, 클래스 기본값 지문) 정렬 목록(A5 — base 기본값은 파생 레코드에 이미 들어 있다, RuntimeClassMetadata '클래스 기본값 결정'), 프로파일 출력 설정 SHA-256). 엔진 버전은 지역 입력이 아니다 — 쿠커 출력 의미가 바뀌면 쿠커 버전(엔진 버전과 독립된 정수)을 올린다([ReflectionFoundations](../ReflectionFoundations.ko.md) 8.2절). 하드 참조 순환을 위해 SCC 단위로 계산한다: SCC key = SHA-256(구성원 지역 입력을 AssetId 순으로, SCC 밖 하드 의존 튜플 (구성원 AssetId, 의존 AssetId, 그 cookKey)의 정렬 목록), cookKey = SHA-256(SCC key, AssetId). 순환이 없어도 같은 식이다.
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
- 릴리스 콘텐츠 서명: 쿠킹 매니페스트에 서명한 봉투 `CookedManifest.rsig`, 로더 검증, `content-sign` 도구는 [콘텐츠 서명](../ContentSigning.ko.md) 설계를 A1 구현 때 함께 넣는다(Release는 매니페스트 항목과 실행 파일 안 데이터만 읽는다).

### 10.1 구현에서 정한 점(A1b — 쿠킹 매니페스트 v1)

v1은 A4f에서 v2(backend 표·비에셋 산출물 표 — 10.3절 4항)로 바뀐다. 아래 v1 규칙은 v2에서도 그대로이고 v2는 칸을 더한다.

10절 매니페스트 v1 서술에서 비어 있던 곳을 다음처럼 정했다. 애매한 곳은 UE 5.8 IoStore(container TOC `FIoStoreTocHeader`·`FIoStoreTocEntryMeta`, container header `FIoContainerHeader`의 패키지별 import 목록·soft 참조 목록)와 asset registry 쿠킹 산출물(PrimaryAsset·청크 표시)을 따랐고, 이름은 Rocks식이다(대응표는 아래 끝).

1. **물리 배치**: 모든 정수는 little-endian, 문자열은 `u64 길이 + UTF-8 byte`(종단 NUL 없음 — Serialization `ArchiveWriter`와 같은 표기), Guid는 high·low u64다. 패딩이 없어 같은 모델이면 같은 byte다(결정성).
   ```
   magic "RMAN"(4) | formatVersion u32 = 1 | entryCount u32
   | profileId string | profileSettingsSha256 (32) | cookSettingsSha256 (32) | cookerVersion u32
   | cookedByEngineVersion: major u32, minor u32, patch u32
   | entry × entryCount
   | crc32 u32 (앞의 모든 byte — 파일 끝과 정확히 맞는다)
   entry = assetId Guid | typeId Guid | pathHint string | packageSha256 (32) | packageBytes u64
         | bulkRawBytes u64 | cookKey (32) | flags u32 | bundleCount u32 | bundle × n
         | dependencyCount u32 | { assetId Guid, kind u32 } × m
   bundle = name string | memberCount u32 | member assetId Guid × k
   ```
   UE TOC처럼 고정 폭 표와 따로 둔 문자열 표로 나누지 않는다 — 항목 수가 패키지 수(수만)라 한 번 읽어 모델로 푸는 비용이 작고, 로더는 모델(정렬된 항목 배열)을 이진 탐색한다. 헤더 크기 칸도 없다(형식이 바뀌면 formatVersion을 올린다).
2. **판정 순서와 진단**: reader는 두 단계다. (1) 구조: 길이(최소 크기 이상·파일 상한 이하) → magic → formatVersion(1이 아니면 `UnsupportedVersion` / `Unsupported` — 더 새 것과 옛 것을 구분하지 않는다, 매니페스트는 같은 쿠커 빌드가 다시 만든다) → 파일 CRC32 → 헤더 칸 읽기(개수 칸 상한·남은 byte, 문자열 길이·UTF-8, 엔진 버전 u16 범위) → 항목마다 칸 읽기(bundle·의존 개수 칸 상한·남은 byte, 문자열 길이·UTF-8) → 끝 위치 = crc 앞. (2) 칸 규칙: 다 읽은 모델을 `ValidateCookedManifest`가 헤더 → 항목(번호 순, 항목 안은 assetId 순서 → typeId → 경로 → 패키지 크기 → flags → bundle → 의존) → 항목 사이(경로 caseless 유일 → Hard 대상 존재) 순서로 본다. 검증·writer는 (2)만 본다. 그래서 위반이 여럿이면 reader와 검증·writer의 진단이 다를 수 있다 — 예: 항목 0의 정의되지 않은 flags와 항목 1의 bundle 개수 칸 상한 초과가 함께 있으면 reader는 (1)에서 항목 1 BundleCount, 검증·writer는 (2)에서 항목 0 Flags다(`CookedManifest_JudgementOrder`의 우선순위 표 — 길이·magic·버전·CRC·남는 byte·항목 사이·병합·패키지 대조의 겹친 위반). 위반이 하나면 세 경로의 진단이 같다. 그 밖의 위반은 모두 `CorruptPackage` / `InvalidArgument`이고 `AssetDiagnostic::section`에 `CookedManifestField`(위반한 칸), `detail`에 항목 번호(헤더 칸이면 `CookedManifestNoEntry`), `asset`에 그 항목 AssetId(읽었으면), `related`에 의존 대상(의존 칸이면)을 둔다. 할당 실패는 진단 None과 그 오류다. CRC를 내용보다 먼저 보므로 우발 손상은 칸 판정 전에 잡힌다(의도적 변조는 서명 봉투 — ContentSigning — 가 막는다).
3. **상한**(이름 있는 상수): 파일 `CookedManifestMaxFileBytes` = 1 GiB, 항목 `CookedManifestMaxEntries` = 2²⁰, 항목당 bundle `CookedManifestMaxBundles` = 256, 항목당 의존 `CookedManifestMaxDependencies` = 65536, 프로파일 id·경로 힌트 길이는 각각 4 KiB(패키지 문자열 상한)·256 byte(4.2절). 개수 칸은 남은 byte로 담을 수 있는 최소 항목 수보다 크면 읽기 전에 거부한다(작은 파일이 큰 할당을 요구하지 못한다).
4. **헤더 칸**: profileId는 비어 있지 않은 UTF-8(패키지 Summary profileId와 같은 규칙), cookedByEngineVersion은 u16 범위(진단용 — 병합·로드 판정에 쓰지 않는다, 10절). 문자열 칸(profileId·경로 힌트·bundle)의 잘못된 UTF-8은 reader의 문자열 읽기가 그 칸의 손상으로 거부한다. 모델의 `String`은 유효한 UTF-8만 담으므로(Core 불변식) 검증 함수는 UTF-8을 다시 보지 않는다.
5. **항목 칸**:
   - assetId: null 아님, 항목 사이 엄격 오름차순(Guid (high, low) 부호 없는 순 — 중복 없음).
   - typeId: null 아님(대표 에셋 export 타입 — 8항 `CheckCookedPackage`).
   - pathHint: 4.2절 논리 경로 — 앞 `/`, 구성 요소 둘 이상(마운트 + 이름), 구성 요소마다 Core `ValidateNameComponent`, 전체 ≤ 256 byte. 경로 힌트는 caseless 키(`TryMakeNameComponentKey`를 구성 요소마다 '/'로 이은 것)로 항목 사이 유일하다(`FindByPath`가 한 항목을 찾는다). 마운트 이름의 허용 목록(`/Game` 등)은 ProjectLayout(A2) 몫이라 보지 않는다.
   - packageBytes: 패키지 헤더 64 byte 이상, 패키지 상한 64 GiB 이하.
   - bulkRawBytes: 패키지 BulkIndex rawBytes 합(10절 항목 표에 더한 칸). 로더(A1c)가 `PackageLoadOptions::maxBulkRawBytes`를 이 값으로 정한다(5.7절 11 — 패키지마다 정확한 해제 상한이라 손상된 rawBytes가 큰 할당을 요구하지 못한다). 8항 `CheckCookedPackage`가 같은지 본다. UE TOC의 청크 압축 해제 크기 표(`FIoOffsetAndLength`)에 대응한다.
   - flags: bit0 = PrimaryAsset(`CookedManifestFlags::PrimaryAsset`), 그 밖의 비트는 0.
   - bundles: PrimaryAsset 항목만 가진다(PrimaryAsset이 아니면 0개 — UE에서 bundle은 PrimaryAsset 데이터다). 이름은 `ValidateNameComponent`, 항목 안에서 byte 엄격 오름차순이고 caseless 키로도 중복이 없다.
   - bundle 구성원(A1d 개정, 2026-11-20 사용자 결정 — UE `FAssetBundleData`의 bundle별 에셋 목록 대응): bundle마다 그 bundle을 로드할 때 함께 로드할 에셋 목록이다(계획 §24.2 "bundle은 의미별 dependency 집합"). 구성원은 AssetId 엄격 오름차순, null·자기 자신 금지, 그 항목의 런타임 의존 대상(Hard·Soft 어느 쪽이든)이어야 하고 매니페스트 항목이어야 한다(항목 사이 규칙 — 없으면 `CorruptPackage` Bundle 칸, related = 구성원). 상한 `CookedManifestMaxBundleMembers` = 65536(의존 상한과 같다). 구성원이 0개인 bundle도 쓸 수 있다(대표 에셋만 로드). Hard 대상 구성원은 어차피 함께 로드되지만 무해하므로 허용한다(쿠커가 필드의 bundle 지정만 보고 모으기 때문). 수집: PrimaryAsset의 Soft 참조 필드에 붙은 bundle 지정자(리플렉션 `AssetBundles="…"` — UE `meta=(AssetBundles="…")` 대응, 계약)를 저장 때 타입 hook Tags로 색인 태그에 남기고 쿠커(A4)가 그 태그를 읽어 모으며(10.2절 결정 9 — UE가 `FAssetBundleData`를 AssetRegistry 태그로 남기는 것과 같다), bundle 구성원은 `CookSoftReferences`와 무관하게 쿠킹한다(UE `ModifyCookReferences`와 같다 — 구성원은 매니페스트 항목이어야 하므로). 구조 판정(2): bundle 하나의 최소 byte는 이름 길이 칸 8 + 구성원 개수 칸 4이고, 구성원 개수는 `CookedManifestMaxBundleMembers` 이하이면서 남은 byte / 16 이하여야 한다(작은 파일이 큰 할당을 요구하지 못하게 — 3과 같은 규칙). 새 진단 칸 `BundleMemberCount`(개수 칸)·`BundleMember`(구성원 — 순서·null·자기 자신·의존 대상 아님·매니페스트에 없음, related = 구성원). 칸 규칙 순서: 항목 안에서는 의존 검증 뒤에 구성원이 의존 대상인지 보고, 매니페스트에 있는지는 항목 사이 규칙(6)에서 본다. 형식이 바뀌었으나 v1은 아직 배포 전이라 formatVersion은 1 그대로이고(하위 호환을 두지 않는다), 14.1.3 대조표와 golden byte를 다시 만든다.
   - dependencies: 런타임 의존(Hard·Soft — kind 값은 패키지 `PackageDependencyKind`의 1·2), (assetId, kind) 엄격 오름차순, assetId null·자기 자신 금지. 같은 대상에 Hard와 Soft가 함께 있을 수 있다(패키지 Dependencies와 같다).
6. **항목 사이 규칙**: Hard 의존 대상은 매니페스트에 있어야 한다(쿠킹은 Hard 전이 폐포다 — 없으면 `CorruptPackage`, related = 대상). Soft 대상은 없어도 된다(`CookSoftReferences`가 꺼져 있으면 쿠킹하지 않는다). 순환(SCC)은 허용한다 — 로드 단위 계산은 로더(A1c)가 이 Hard 간선으로 한다.
7. **writer**(`WriteCookedManifest`): 모델을 6까지 같은 규칙으로 검증한 뒤(위반은 쓰지 않고 `InvalidArgument` + 같은 진단 — reader가 거부할 byte를 쓰지 않는다) 결정적으로 직렬화한다. 출력이 `CookedManifestMaxFileBytes`(CRC 칸 포함 파일 전체)를 넘으면 `CapacityExceeded`(진단 None, out 그대로)다 — 처음 구현은 상한에서 CRC 4 byte를 한 번 더 빼 1 GiB − 4까지만 썼다. 상한을 인자로 받는 내부 함수(`package_detail::WriteCookedManifestWithLimit`)로 경계를 시험한다. 모델은 이미 정렬되어 있어야 한다(writer가 정렬하지 않는다 — 쿠커가 정렬해 넘기고, 순서 위반은 오류로 드러난다). 출력은 메모리 버퍼이고 게시(임시 파일·Flush·Replace)는 쿠커(A4) 몫이다(10절 게시 3).
8. **대조 함수**:
   - `CheckCookedManifestProfile`(로더 시작): 매니페스트 profileId ≠ 실행 프로파일이면 `IncompatibleCook` / `Unsupported`.
   - `CheckCookedManifestMerge`(부분 쿠킹 병합): profileId·profileSettings·cookSettings·cookerVersion이 모두 같아야 하고, 아니면 `IncompatibleCook` / `Unsupported`(section = 다른 칸 — 처음 다른 칸 하나).
   - `CheckCookedPackage`(로더가 패키지를 연 뒤 — A1c가 파일 쪽 `CheckCookedPackageFile`과 내용 쪽 `CheckCookedPackageContents`로 나눴고 `CheckCookedPackage`는 둘을 차례로 부른다, 11.6절 3): 파일 길이 = packageBytes, SHA-256 = packageSha256, 그다음 `ReadPackage` 결과와 — 헤더 종류 쿠킹, assetId, 대표 에셋 타입(export 1) = typeId, BulkIndex rawBytes 합 = bulkRawBytes(합이 u64를 넘으면 — 호출자가 손상된 내용을 넘긴 경우 — 2⁶⁴로 감은 값이 같아도 불일치), Summary profileId·profileSettings·cookerVersion = 매니페스트 헤더, 패키지 Dependencies(Hard·Soft, (asset, kind) 순서) = 매니페스트 의존 배열(같은 순서로 하나씩 — 쿠킹 패키지에는 다른 종류가 없다). 다르면 `CorruptPackage` / `InvalidArgument`, section = `CookedManifestField`의 그 칸, detail = `CookedManifestNoEntry`, asset = 그 에셋, 의존이면 related = 처음 다른 자리의 의존 대상(매니페스트 쪽이 있으면 그것, 없으면 패키지 쪽). 크기·SHA-256은 `ReadPackage` 전에 본다(바뀐 파일을 해석하지 않는다).
   - `AppendCookedPackagePath`: 4.2절 파일 이름 `Packages/<a>/<A>-<h>.rpkg`(쿠킹 루트 기준)을 만든다.
9. **로더 API 염두**: 매니페스트 모델은 AssetId 오름차순 배열이고 `Find(AssetId)`(이진 탐색)를 둔다. 경로 힌트 조회 색인(`FindByPath`)과 Hard SCC 계산은 그것을 쓰는 로더·AssetManager(A1c)가 만든다(여기서는 그 입력 — 경로 힌트 caseless 유일성, Hard 대상 존재 — 만 보장한다). 서명 봉투(ContentSigning CS1)는 같은 버퍼를 검증한 뒤 이 reader에 그 매니페스트 byte span을 넘긴다(reader는 파일을 열지 않는다).

| Rocks | UE 5.8 |
|---|---|
| `CookedManifest`(rman) | IoStore container TOC(`.utoc`) + container header(`FIoContainerHeader`) + 쿠킹 asset registry |
| entry assetId·packageSha256·packageBytes | `FIoChunkId`·`FIoStoreTocEntryMeta::ChunkHash`·`FIoOffsetAndLength` |
| bulkRawBytes | TOC 압축 블록 표가 주는 청크 해제 크기 |
| dependencies Hard·Soft | `FFilePackageStoreEntry::ImportedPackages`·`FIoContainerHeaderSoftPackageReferences` |
| primaryAsset·bundles | asset registry의 PrimaryAssetId·`FAssetBundleData` |
| 파일 CRC32 | (TOC는 서명·해시로) — 서명은 Rocks `CookedManifest.rsig` |

### 10.2 A4 구현 세부(2026-11-23 — 권장안으로 확정, 교차 검증 1회: 필수 4·권장 7 반영)

10절을 구체화한다. 애매한 곳은 UE 5.8 Cook(`UCookOnTheFlyServer`·`FPackageData`·`UAssetManager` PrimaryAsset 규칙·`FAssetBundleData`·증분 쿠킹의 TargetDomain 키)을 따르고 이름은 Rocks식이다.

1. **모듈** `Engine/Source/Developer/AssetCooker`(Developer — 에디터·도구만). 의존 `AssetDatabase`(색인·EditorPackageSet)·`AssetRuntime`(WritePackage·CookedManifest)·`AssetNugget`(키 writer·SCC 키·NuggetCache)·`ReflectionText`(설정 codec)·`ObjectRuntime`. 명령은 `AssetCommandlets`의 `asset-cook`.
2. **TargetProfile**(리플렉션 struct): `id`(String — 파일 이름과 같아야 함), `graphicsBackend`(String — A4는 기록만, A4f에서 `graphicsBackends`·`shaderFormats`로 바뀌고 struct·병합·구조 검증은 새 모듈 `TargetProfiles`로 옮긴다 — 10.3절 1항), `bulkCodec`(enum None·Zstd, 기본 None — 10절), `bulkAlignment`(u32, 0이면 쿠킹 기본 4096). 텍스처 칸(형식 지원 표·LOD 그룹 — LOD 그룹 Map은 `MergeByKey`)은 A5가 더한다. 엔진 파일 `Engine/Config/TargetProfiles/<id>.json`(엔진 기본 `Windows`·`Android`, A4f에서 `Mac`을 더한다 — 10.3절 1항) 위에 프로젝트 `Config/TargetProfiles/<id>.json`을 ReflectionText `ImportJson`으로 덮는다 — 의미는 codec 기본: struct는 재귀 멤버 병합, 배열은 통째 교체(10절의 "그룹 단위"를 이것으로 확정). 엔진 파일이 없는 id는 `InvalidSetting`(엔진 플랫폼 목록이 기준 — 프로젝트만의 프로파일은 받지 않는다). **프로파일 출력 설정 SHA-256** = 병합한 TargetProfile을 `NuggetKeyWriter`의 `WriteReflected`로 흘린 SHA-256(필드 이름·타입·값 — 칸을 더하면 바뀐다).
3. **CookSettings**(리플렉션 struct, `Config/CookSettings.json`, 없으면 기본값): `alwaysCook`·`neverCook`(Array<String> — 논리 경로 `/Game/A/B` 또는 디렉터리 `/Game/A/`(끝 `/`)), `primaryAssets`(Array<`PrimaryAssetRule{type: 클래스 도구 이름, directories: Array<String>}`>), `cookSoftReferences`(bool, 기본 false), `cookDevelopmentShaders`(bool, 기본 false — A4f, 10.3절 2항). 경로는 LogicalPath 규칙으로 검증(위반 `InvalidSetting`). **CookSettings SHA-256**도 같은 방식. bundle은 규칙이 아니라 필드 지정자에서 온다(결정 9 — 10절 문장을 고친다).
4. **대상 계산**(색인만 — 패키지를 열지 않는다, UE가 AssetRegistry로 쿠킹 대상을 세는 것과 같다):
   - 뿌리: 전체 쿠킹은 alwaysCook이 가리키는 에셋(디렉터리는 재귀) ∪ PrimaryAsset(규칙 타입 또는 그 파생 타입이며 규칙 디렉터리 아래). 부분 쿠킹은 인자 경로(에셋·디렉터리).
   - 폐포: Hard 의존(+ cookSoftReferences면 Soft) + bundle 구성원(결정 9 — 색인 태그, cookSoftReferences와 무관). EditorOnly·Build·ImportOwner 간선은 따라가지 않는다.
   - NeverCook: 뿌리면 뺀다. alwaysCook과 NeverCook에 함께 걸리면 NeverCook이 이기고 경고 `NeverCookReferenced`(UE와 같은 우선순위). 다른 대상이 Hard로 참조하면 `NeverCookReferenced`(오류), Soft로 참조하면 진단 없이 대상에서 빠진다. 의존 목록에는 남는다 — 매니페스트 의존은 쿠킹 패키지 Dependencies와 같아야 하고(로더 대조 11.6절 3), 그 대상은 매니페스트 항목이 없어 로드 요청이 NotFound다(UE도 쿠킹되지 않은 soft 참조를 남기고 로드에서 실패시킨다 — A4c2 리뷰로 고침).
   - EditorOnly 에셋(대표 타입이 EditorOnly 클래스)은 대상이 아니다. Hard로 참조하면 `NeverCookReferenced`와 같은 오류 규칙.
   - 색인에 없는 Hard 의존은 `MissingDependency`(쿠킹 실패), Soft는 경고 `MissingDependency`(의존 목록에는 남는다 — 위와 같은 이유).
   - 부분 쿠킹: 위에 역방향 Build 폐포(대상을 Build로 의존하는 에셋 — 색인 Referencers(Build), 전이)와 기존 매니페스트 항목 중 다시 쿠킹할 것(결정 8)을 더한다. 에디터에서 사라진 에셋의 매니페스트 항목은 빼고, 남은 항목이 그것을 Hard로 참조하면 `MissingDependency`.
5. **키 계산**(패키지를 열지 않는다 — UE 증분 쿠킹 TargetDomain 키처럼 로드 없이 판정):
   - **콘텐츠 해시** = 에디터 패키지 파일 전체 SHA-256(패키지 지문은 섹션 CRC32 표의 SHA라 충돌이 CRC 수준이어서 쓰지 않는다). 색인이 지연 계산해 항목과 색인 캐시에 둔다(`AssetIndex::ContentHash(id)` — 크기·수정 시각이 같으면 캐시 값, 다르면 다시 읽음 — 바뀐 파일만 다시 해시, 색인 캐시 형식 버전을 올린다).
   - **처리기 build key** = 9절 너겟 키: 공통 머리(`WriteNuggetKeyHeader(writer, {처리기 id, 처리기 버전}, projectGeneration)`) + 처리기 설정 키(색인 정보만으로 — 기본 처리기는 없음, 설정이 패키지 안에 있으면 콘텐츠 해시가 이미 담는다) + Build 의존 튜플 (역할, AssetId, 그 의존의 build key). Build 그래프에 순환이 있을 수 있으므로 **Build SCC**로 계산한다(`NuggetSccKey` — 구성원 지역 키 = 공통 머리 + 설정 + 콘텐츠 해시, 외부 튜플 = Build 의존의 구성원 key; build key = `NuggetMemberKey`).
   - **cookKey 지역 입력** = 공통 머리(`{CookerProcessorGuid, cookerVersion}`) + 콘텐츠 해시 + 논리 경로 키 + CookSettings 판정(PrimaryAsset 여부·bundle 이름과 구성원) + 처리기 build key + 쿠킹 패키지 formatVersion·섹션 schemaVersion 집합 + **타입 표 지문** + 프로파일 출력 설정 SHA-256. 클래스 기본값은 A5c가 export 타입마다 (TypeId, 상태, cooked 지문) 목록으로 더한다(RuntimeClassMetadata 'A5c 지문 연결' — 레코드 실패 타입은 `InvalidClassDefaults`).
   - **타입 표 지문** = 등록된 리플렉션 타입 전체의 (TypeId, 스키마 버전) 정렬 목록 SHA-256이다. Summary 타입 표는 export 클래스만 담아 필드 안 struct·enum·InstancedStruct 내용 타입의 스키마 변경을 놓치고, InstancedStruct 내용 타입은 실행 중에 정해져 스키마로 폐포를 셀 수 없다 — 그래서 보수적으로 전체 표를 쓴다(어느 타입의 버전이 바뀌어도 전체가 다시 쿠킹된다 — 안전 쪽, 범위를 줄이는 것은 저장된 타입 참조를 색인에 두는 후속).
   - **cookKey** = Hard 그래프 SCC(대상 사이, Tarjan — AssetId 순 결정적): SCC key = `NuggetSccKey(구성원 {AssetId, 지역 키}, 외부 튜플 {구성원, "Hard", 의존, 의존 cookKey})`, cookKey = `NuggetMemberKey(SCC key, AssetId)`. 순서: Build SCC(build key) → Hard SCC 역위상(cookKey).
6. **CookProcessor 표**: TypeId → `CookProcessor{정체성 NuggetProcessorId(처리기 Guid·버전 Guid — 너겟 키 공통 머리와 같다), WriteSettingsKey(writer, 색인 항목)(선택 — 색인 정보만), Cook(context)}`. 대표 에셋 타입에서 상속 사슬을 따라 가장 가까운 등록 처리기 하나(AssetTypeHookTable과 같은 규칙). 기본 처리기(Asset 루트): 에디터 객체 그래프를 그대로 `WritePackage(kind=Cooked, profile 값, bulkCodec·bulkAlignment, bulkSource=에디터 패키지 store)`로 쓴다(EditorOnly 필드는 writer가 뺀다). 처리기는 `CookContext`로 NuggetCache·프로파일·진단 sink를 받는다(텍스처 처리기 A5가 너겟을 쓴다).
7. **쿠킹 실행**: 대상을 Hard SCC 역위상(의존 먼저, 같은 깊이는 AssetId 순)으로 처리한다. 열린 패키지는 "남은 참조자 수"로 관리해 그것을 Hard로 의존하는 대상이 모두 쿠킹되면 닫는다(공통 의존을 대상마다 다시 열지 않는다 — 열기 횟수 ≤ 대상 수 + 의존 수, 시험이 카운터로 본다). 메모리 상한(`CookOpenPackageBytes`)을 넘으면 참조자가 남은 패키지도 닫고 나중에 다시 연다. 처리기는 메모리 sink에 쓰고 패키지 SHA-256·크기·bulk raw 합을 얻는다. 오류는 에셋별로 모아 끝까지 진행하고(보고서), 하나라도 있으면 게시하지 않는다(10절). 병렬은 후속(결정성은 정렬된 순회로 지킨다). 에디터 프로세스 안 쿠킹(서비스 API)은 dirty 패키지가 열려 있으면 `Busy`로 거부한다(쿠킹은 디스크 상태 — Commandlet에는 열린 패키지가 없다). 쿠킹은 에셋 DB 잠금을 쥔다.
8. **부분 쿠킹 재사용**: 기존 매니페스트 항목은 cookKey가 같고 `Packages/`의 그 파일이 있으며 크기·SHA-256이 매니페스트와 같을 때만 재사용한다. 파일이 없거나 다르면 다시 쿠킹한다(사용자가 쿠킹 디렉터리를 일부 지운 경우 — 시나리오 27 손상된 게시 파일의 재게시). 재사용하는 것은 **패키지 byte와 그 byte에서 나오는 칸**(SHA-256·크기·bulk raw 합·대표 타입·의존 목록 — 의존 목록은 쿠킹 패키지 Dependencies 그대로다)뿐이다. 매니페스트 항목의 byte 밖 칸(PrimaryAsset flags·bundle·경로 힌트)은 재사용 항목도 이번 판정(A4b 대상·제외 목록·색인)으로 늘 다시 만든다(A4c1 리뷰 필수 — cookKey는 byte 입력의 키라 판정을 다 담지 않는다. 판정 입력이 늘어도 키를 같이 늘릴 필요가 없다).
9. **bundle**: 리플렉션 지정자 `AssetBundles="a,b"`(Soft 참조 필드 — UE `meta=(AssetBundles=…)`)가 있는 PrimaryAsset은 **저장 때** 타입 hook Tags(A2c)로 bundle 구성원을 색인 태그에 남긴다(UE가 `FAssetBundleData`를 AssetRegistry 태그로 남기는 것과 같다). 쿠커는 태그만 읽는다(2차 열기 없음). MetaCompiler 지정자와 필드 메타는 리플렉션 쪽 요구다(ReflectionExtensions 문서에 적고 리플렉션 담당과 조정한다 — A4e).
10. **게시**(10절 1–3 그대로): `<Project>/Saved/Cooked/<Profile>/.staging/<guid>.tmp` 기록·Flush → `Packages/<a>/<이름>`에 `Move(no-replace)` → 있으면 크기·SHA-256 대조(같으면 임시 삭제, 다르면 `CorruptEntry` 경고 + `Replace`) → 바뀐 디렉터리 안쪽부터 `SyncDirectory` → 매니페스트를 `.staging`에 쓰고 Flush → `Replace`. 전체 쿠킹은 그 뒤 새 매니페스트가 가리키지 않는 `Packages/` 파일을 지운다(못 지우면 경고). 부분 쿠킹은 기존 매니페스트와 병합(`CheckCookedManifestMerge` — 다르면 `IncompatibleCook`)하고 파일은 지우지 않는다. 기존 매니페스트가 없으면 부분 쿠킹은 전체 쿠킹과 같다.
11. **보고서**: `Saved/Cooked/<Profile>/CookReport.json`(결정적 — 진단을 (assetId, 코드) 순), 도구 빌드 ID·엔진 버전(진단용 — 10절), 대상 수·재사용 수·쓴 byte. 명령은 사람이 읽는 줄도 낸다.
12. **명령** `asset-cook --profile=<id> [<path>...] [--verify-nuggets] [--verify-nuggets-source=local|remote] [--json]`(13절 문법 — A4e1에서 13절 형태로 맞췄다): 경로가 없으면 전체 쿠킹. `--verify-nuggets`는 처리기가 너겟 적중을 쓰지 않고 다시 만들어 적중 결과와 byte를 대조한다(다르면 `NuggetMismatch` 오류 — A4e가 진단 코드를 등록, 9.1 결정 16). A4의 기본 처리기는 너겟을 쓰지 않아 verify는 늘 같다 — 실질 시험은 A5 텍스처 처리기 때다. 엔진 전용 쿠킹 `asset-cook --engine --profile=<id> [--json]`(프로젝트 없이 엔진 셰이더 라이브러리만)은 A4f가 더한다(10.3절 7항).

하위 단계(각각 14절 완료 조건 넷):

| 단계 | 범위 | 시험 |
|---|---|---|
| A4a | 모듈·TargetProfile·CookSettings(읽기·병합·검증·SHA-256)·엔진 기본 프로파일 파일 | 병합·거부·SHA 안정·칸 변경 시 SHA 변경 |
| A4b | 대상 계산(전체·부분·NeverCook·AlwaysCook∩NeverCook·Missing·Soft 설정·역방향 Build·사라진 항목·bundle 태그 읽기)·색인 콘텐츠 해시(지연·캐시) | 그래프 경우마다 대상·진단, 해시 캐시 무효화 |
| A4c1 | 기반 둘(TypeRegistry 타입 목록·Core SCC)·CookProcessor 표·키(build key Build SCC·cookKey Hard SCC·타입 표 지문) | 순환 키(시나리오 28)·Build 원본만 바꾸면 배열 cookKey 바뀜·입력마다 키 바뀜·결정성 |
| A4c2 | 기본 처리기·쿠킹 실행(출력 sink, 열기 관리) | EditorOnly 필드 부재(11)·두 번 같은 byte(13)·원본 없이 쿠킹(2)·열기 횟수·에셋별 오류 계속 |
| A4d1 | 게시기(staging·no-replace 이동·있으면 대조·CorruptEntry 교체·디렉터리 동기화·매니페스트 원자 교체·전체 정리·부분 병합·IncompatibleCook)·재사용 판정(cookKey·파일 크기·SHA-256)·재사용 항목의 byte 밖 칸 재계산(결정 8) | 시나리오 22·27·재사용 대상의 판정 칸이 바뀌면 매니페스트가 바뀜·실패 시 게시본 불변 |
| A4d2 | 보고서(CookReport.json)·쿠킹 전체 흐름(설정 → 대상 → 키 → 재사용 → 실행 → 게시, 오류 진단이 있으면 게시하지 않음) | 시나리오 13(전체 두 번 같은 byte·두 번째는 전부 재사용)·보고서 결정성 |
| A4e | `asset-cook` 명령·`--verify-nuggets`(NuggetMismatch 등록)·리플렉션 요구 둘(필드 지정자 `AssetBundles`·클래스 표지 `EditorOnly` — 리플렉션 담당과 조정)·저장 때 bundle 태그·EditorOnly 에셋 규칙(결정 4) | 명령 출력·verify(A4는 늘 같음)·bundle 구성원·EditorOnly 에셋 |

- A4b 구현 세부(2026-11-23 — 권장안으로 확정, 결정 4·5의 콘텐츠 해시를 구체화):
  - EditorOnly 에셋 규칙(결정 4)은 A4e로 옮긴다 — 클래스 표지 `EditorOnly`가 리플렉션에 아직 없다(`TypeFlags`에 없음). 필드 지정자 `AssetBundles`와 같은 리플렉션 요구라 함께 다룬다.
  - **콘텐츠 해시**(AssetDatabase): `AssetIndexEntry`에 `contentHash`·`contentHashBytes`·`contentHashTicks`·`hasContentHash`를 둔다. `AssetIndex::ContentHash(files, id)`는 Content 파일을 Stat해 크기·수정 시각이 저장된 값과 같으면 저장된 해시를, 아니면 64 KiB씩 흘려 읽어 SHA-256을 계산해 그 Stat 값과 함께 둔다(바뀐 파일만 다시 해시 — 권장 1). 읽기 전후 Stat이 다르면 `Busy`(해시하는 동안 바뀜 — 쿠킹은 그 에셋 오류). 색인 캐시가 네 칸을 보존한다(캐시 schemaVersion 3 — 캐시 항목은 지문으로 재사용되므로 해시는 Stat 대조로만 믿는다). 오류: 없음 NotFound, 같은 id 둘 이상 AlreadyExists, 파일 IO 오류, 할당 실패.
  - **API**(AssetCooker `CookTargets.h`): `ComputeCookTargets(request, out)`. 요청 = 색인·TypeRegistry·CookSettings·부분 쿠킹 경로 목록(비면 전체 쿠킹)·기존 매니페스트(부분, 없으면 null). 결과 = 대상(AssetId 순 — 각각 PrimaryAsset 여부·bundle 목록), 제외 목록(Soft로만 닿은 NeverCook 에셋 — 매니페스트 의존에서 뺄 것), 사라진 항목(기존 매니페스트에 있고 색인에 없음), 진단 목록. 진단은 `CookDiagnostic{code, warning, asset, related, setting}`다 — 같은 코드가 경고·오류로 쓰인다(NeverCookReferenced·MissingDependency·InvalidPath)라 `AssetDiagnostic`(코드별 고정 수준)이 아닌 따로 둔 표지다. 함수 오류는 색인 조회 오류·할당 실패뿐이고 규칙 위반은 진단이다(쿠킹은 오류 진단이 하나라도 있으면 게시하지 않는다 — 10절).
  - 경로 판정은 비교 키로 한다: 에셋 경로는 키 일치, 디렉터리는 키 접두(`<dir key>/`, 마운트 디렉터리는 모든 항목).
  - 규칙(판정 순서):
    1. 뿌리: 전체 쿠킹 = alwaysCook(에셋은 FindByPath, 디렉터리는 접두) ∪ PrimaryAsset 규칙(ListByType(type, types — 파생 포함) 중 디렉터리 하나 아래). 부분 쿠킹 = 인자 경로(같은 판정). 아무것도 가리키지 않는 alwaysCook 항목은 경고 `InvalidPath`(setting = 순번), 인자 경로는 오류 `InvalidPath`.
    2. NeverCook 뿌리는 뺀다. alwaysCook·인자 경로가 직접 가리킨 에셋이면 경고 `NeverCookReferenced`(UE와 같은 우선순위), PrimaryAsset 규칙으로만 닿았으면 진단 없이 뺀다.
    3. 폐포: 대상마다 색인 의존을 본다 — Hard는 늘, Soft는 cookSoftReferences일 때, PrimaryAsset이면 bundle 구성원(태그 `Bundle.<이름>` = 구성원 AssetId 표준 문자열, 구성원마다 태그 하나 — A4e가 쓴다)도 Hard와 같은 규칙. 의존이 색인에 없으면 Hard·bundle은 오류·Soft는 경고 `MissingDependency`(asset = 참조자, related = 의존), 같은 id가 둘 이상이면 오류 `DuplicateAssetId`, NeverCook이면 Hard·bundle은 오류 `NeverCookReferenced`·Soft는 제외 목록. bundle 태그 값이 AssetId가 아니면 오류 `CorruptPackage`(asset = PrimaryAsset). EditorOnly·Build·ImportOwner 간선은 따라가지 않는다.
    4. 부분 쿠킹: 인자 뿌리와 역방향으로 더한 에셋을 Build로 의존하는 에셋(색인 Referencers(Build))을 고정점까지 더한다(NeverCook은 진단 없이 빼고, 더한 에셋도 3의 폐포를 따른다). Hard·Soft·bundle 폐포로만 들어온 에셋에는 적용하지 않는다 — 10절의 의도는 인자 쪽 편집(원본 Texture2D를 고치면 그것을 묶는 Texture2DArray도)이고, 폐포 에셋까지 넓히면 많이 쓰이는 원본 하나가 그것을 묶는 에셋 전부를 다시 쿠킹한다(리뷰 권장, 2026-11-23). 폐포로 먼저 들어온 에셋도 뒤에 역방향으로 닿으면 넓힌다. 기존 매니페스트 항목은 색인에 고유하게 있고 NeverCook이 아니면 뿌리로 더한다(폐포만 따르고 역방향 Build는 넓히지 않는다 — 10절 "기존 매니페스트에서 cookKey가 달라진 항목"을 다시 쿠킹하려고 키를 계산한다, 같으면 A4d가 재사용한다 — A4d1 리뷰로 더함). 색인에 없거나 NeverCook이 된 항목은 사라진 항목이고(다시 게시하지 않는다), 같은 id가 둘 이상이면 오류 `DuplicateAssetId`(asset = 그 id)다. 이번에 다시 쿠킹하지 않는 남은 항목(같은 id 항목뿐이다)이 사라진 항목을 Hard로 의존하면 오류 `MissingDependency`(asset = 남은 항목). 다시 쿠킹하는 항목은 옛 매니페스트 의존이 아니라 색인 의존으로 3에서 이미 판정했다(리뷰 필수 — 지운 참조를 옛 의존으로 오류 내지 않는다). 기존 항목 중 다시 쿠킹할 것(결정 8)은 키가 필요하므로 A4c·A4d가 정한다.
    5. PrimaryAsset 여부는 규칙에 맞는지로 정한다(폐포로 들어와도 규칙에 맞으면 PrimaryAsset). bundle은 PrimaryAsset에만 있다(이름 순, 구성원 AssetId 순).
  - 결정성: 대상·제외·사라진 항목은 AssetId(high, low) 순, 진단은 (asset, code, related, setting, 오류 먼저) 순이다(같은 의존의 Hard·Soft 누락은 칸이 같고 수준만 다르다).
- A4c 구현 세부(2026-11-23 — 권장안으로 확정, 결정 5–7을 구체화. A4c1·A4c2로 나눈다):
  - **기반 보강 둘**(우회하지 않는다):
    1. `TypeRegistry::ListTypeVersions(out)`(ObjectRuntime): 등록된 모든 타입의 (TypeId, 종류, 스키마 버전)을 TypeId 순으로 채운다. 등록·해제가 소유 스레드 전용이므로 이 함수도 소유 스레드 전용이다(다른 스레드는 WrongThread). 그래서 개수를 세고, 잠금 밖에서 예약한 뒤 복사하는 사이에 목록이 바뀌지 않는다(할당기 콜백을 잠금 안에서 부르지 않는 규칙). 종료 뒤에는 ShuttingDown이다. enum은 스키마 버전이 없어 0이다 — 저장 wire가 I64 값이라 enum 스키마를 바꿔도 쿠킹 byte가 바뀌지 않는다.
    2. Core `Core/Graph/StrongComponents.h`의 `algo::ForEachStrongComponent`: 반복 Tarjan이고 CSR 그래프를 받는다. SparksCompiler 비공개 구현을 공용으로 올린 것이고 알고리즘은 같다. 의존을 먼저 알린다. 기존 사본 셋은 모두 공용으로 바꿨다: SparksCompiler(f476bffd)·UnrealExchange(12ad179c)는 각 세션이, AssetLoader는 에셋 세션이 A4c2 뒤에 바꿨다(새 항목을 모아 지역 CSR 그래프를 만들고 root부터 돌린다 — 완료 순서는 같다).
  - **처리기 정체성**: 버전은 Guid다(`NuggetProcessorId` — 너겟 키 공통 머리와 같다). 결정 6의 `version u32`를 이것으로 고친다. 출력 의미가 바뀌면 버전 Guid를 바꾼다.
  - **CookProcessor**(AssetCooker `CookProcessor.h`) 인터페이스: `Identity()`(NuggetProcessorId), `WriteSettingsKey(writer, 색인 항목)`(기본은 아무것도 쓰지 않음 — 색인 정보만 쓴다), `Cook(context)`(A4c2).
  - **CookProcessorTable**: 생성 인자는 TypeRegistry와 기본 처리기다. `Register(type, processor)`는 0 type이면 InvalidArgument, 같은 type이면 AlreadyExists다. 처리기는 빌림이라 표보다 오래 살아야 한다. `Find(type)`는 type부터 base 사슬로 올라가 가장 가까운 등록 처리기를 주고, 없으면 기본 처리기를 준다(AssetTypeHookTable과 같은 규칙). 타입 조회 오류는 그대로 돌려준다.
  - **키 API**(AssetCooker `CookKeys.h`): `ComputeCookKeys(request, out)`.
    - 요청: 색인(비const — ContentHash가 해시를 둔다), 파일 시스템, TypeRegistry, 처리기 표, A4b 결과(`CookTargetSet`), 프로파일 출력 설정 SHA-256, 프로젝트 세대 Guid, 쿠커 버전(u32 — 매니페스트 `cookerVersion`과 같다).
    - 결과: 대상마다(대상 순) {asset, contentHash, buildKey, cookKey, hasKey}와 진단이다. 함수 오류는 TypeRegistry 오류와 할당 실패뿐이고, 에셋 문제는 진단이다.
  - `CookDiagnostic`에 `error`(ErrorCode, 기본 None) 칸을 더한다. 콘텐츠 해시 실패는 진단 코드가 MissingFile(파일 없음)이거나 None이고, 그 오류를 이 칸에 둔다.
  - **Build 키**(결정 5 그대로 — 9절 너겟 키):
    - 영역 = 대상 ∪ 대상에서 Build 간선으로 닿는 에셋(전이). Build 의존이 색인에 없으면 오류 MissingDependency, 같은 id면 오류 DuplicateAssetId다(asset = 참조자, related = 의존).
    - 지역 키 = 공통 머리(처리기 정체성, 프로젝트 세대) + 처리기 설정 키 + 콘텐츠 해시.
    - Build SCC(AssetId 순 노드·간선 — 결정적)마다 `NuggetSccKey(구성원, 외부 튜플 {구성원, "Build", 의존, 의존 build key})`를 구하고, build key = `NuggetMemberKey`.
  - **cookKey**:
    - 지역 키 = 공통 머리({`CookerProcessorGuid`, `CookerKeyVersion`}, 프로젝트 세대) + 쿠커 버전 + 콘텐츠 해시 + 논리 경로 키 + PrimaryAsset 여부 + bundle(이름·구성원) + 이 대상의 제외된 Soft 의존(AssetId 순) + build key + `PackageFormatVersion` + `PackageSectionSchemaVersion` + 타입 표 지문 + 프로파일 출력 설정 SHA-256 + (A5c) export 타입별 클래스 기본값 (TypeId, 상태, cooked 지문) 목록.
    - CookSettings SHA-256 전체는 넣지 않는다 — 판정 결과만 넣는다. 그래서 alwaysCook을 더해도 기존 대상의 키는 바뀌지 않는다. 제외된 Soft 의존은 byte에도 매니페스트에도 영향이 없지만 판정 입력으로 보수적으로 남긴다(적중만 줄어든다).
    - Hard SCC는 대상 사이 Hard 간선으로 만든다(대상 밖 Hard 의존은 A4b가 이미 오류로 보고했다 — 간선에서 뺀다). 외부 튜플은 {구성원, "Hard", 의존, 의존 cookKey}다.
  - **타입 표 지문** = `ListTypeVersions` 목록(개수, 그리고 칸마다 TypeId·종류·버전)을 NuggetKeyWriter로 흘린 SHA-256이다.
  - **키 실패 전파**: 콘텐츠 해시를 못 구했거나 Build 의존이 없는 에셋은 키가 없다(hasKey 거짓). 그 에셋에 Build·Hard로 기대는 에셋도 키가 없고, 진단은 원인에 하나만 남긴다. 쿠킹은 오류 진단이 있으면 게시하지 않는다.
  - **실행**(A4c2, `CookPackages`):
    - 순서는 Hard SCC 역위상이다(의존 먼저, SCC 안은 AssetId 순 — Tarjan 완료 순이 결정적이다).
    - 패키지 열기는 호출자의 EditorPackageSet으로 한다. 시작 때 dirty 패키지가 있으면 Busy다.
    - 열린 집합 관리(결정 7을 고친다): 대상을 열고 쿠킹한다(이미 열린 의존은 그대로 잇는다). 이번 쿠킹이 연 패키지 수가 상한 `CookMaxOpenPackages`(요청 칸, 기본 4096)를 넘으면 시작 때 열린 것만 남기고 모두 닫는다(`CloseOpened(before)`). 끝에도 닫는다.
    - 상한 판정을 byte 대신 패키지 수로 하는 근거: 결정적이고 O(1)이다. UE도 쿠킹 중 메모리 압박에서 GC로 한꺼번에 내린다. 참조자 수로 하나씩 닫는 방식은 EditorOnly 의존 묶음과 닫기 순서 규칙(참조되는 패키지는 Busy) 때문에 버린다. 열기 횟수 ≤ 대상 수 + 의존 수(상한을 넘지 않을 때)이고, 시험이 파일 열기 계수로 본다.
    - 처리기는 `CookContext`(Registry·접근 문맥·패키지 handle·색인 항목·프로파일·출력 sink·allocator, 선택 NuggetCache)를 받는다.
  - 매니페스트 항목은 재사용 여부와 상관없이 byte 밖 칸을 이번 판정으로 만든다(결정 8). 의존 목록은 byte에서 나온다(재사용 항목은 기존 `Packages/` 파일을 읽어 얻는다).
    - 기본 처리기는 `WritePackage(kind=Cooked, profileId, profileSettings, cookerVersion, bulkCodec, bulkAlignment(0이면 4096), bulkSource=열린 패키지 store)`다.
    - 결과(`CookedPackage`)는 쓴 byte·SHA-256·크기·bulk raw 합·대표 타입·경로 힌트·flags·bundle·의존이다. 쓴 byte를 다시 읽어 검증하고, 의존 = 쿠킹 패키지 Dependencies 그대로다. 만든 항목은 로더 대조(`CheckCookedPackage`)로 확인한다.
    - 결과는 출력 sink(`CookOutputSink::Put`)로 하나씩 넘긴다(A4d가 바로 staging에 쓴다 — 전체를 메모리에 모으지 않는다). 에셋별 오류(열기·처리기·검증)는 진단으로 모으고 끝까지 진행한다.
- A4d 구현 세부(2026-11-23 — 권장안으로 확정, 결정 8·10·11을 구체화. A4d1·A4d2로 나눈다):
  - **공용 원자 쓰기**: AssetDatabase의 비공개 `WriteFileAtomically`(`<디렉터리>/<이름>.<guid>.tmp` → Flush → Replace)를 공개 API로 올려 매니페스트·보고서 쓰기에 그대로 쓴다(중복 구현 금지). 결정 10의 "매니페스트를 `.staging`에 쓰고 Replace"는 같은 볼륨의 임시 파일 → Replace라는 뜻이고, 임시 파일은 대상 옆에 둔다.
  - **게시 배치**: `<Saved>/Cooked/<프로파일>/` 아래에 `CookedManifest.rman`·`CookReport.json`·`Packages/<a>/<A>-<h>.rpkg`(`AppendCookedPackagePath` — 이름에 SHA-256이 들어간다)·`.staging/`을 둔다.
  - **게시기**(AssetCooker `CookPublish.h`, `CookPublisher` — `CookOutputSink`):
    - `Begin(lock)`: 잠금은 이 프로젝트의 쓰기 잠금이어야 한다(InvalidArgument). 쿠킹 루트까지의 디렉터리(`Saved`·`Saved/Cooked`·루트·`Packages`) 중 없던 것은 그 부모를 기억해 Finish가 동기화한다(10절 게시 2 — 첫 쿠킹이면 새 루트의 항목이 매니페스트보다 먼저 매체에 남아야 한다, A4d1 리뷰로 더함). `.staging`·`Packages`를 만들고, `.staging` 안의 파일을 모두 지운다(잠금을 쥔 쿠커만 쓰는 자리라 남은 것은 모두 고아다 — 못 지우면 경고).
    - `Put`: `.staging/<guid>.tmp`에 쓰고 Flush·Close한 뒤 대상 경로로 `Move(no-replace)`한다. 대상이 이미 있으면 크기·SHA-256을 흘려 읽어 대조한다. 같으면 임시 파일을 지우고, 다르면 경고 `CorruptEntry` 뒤 `Replace`한다(시나리오 27). 바뀐 디렉터리를 기억하고 매니페스트 항목을 만든다.
    - `Reuse(기존 항목, 대상)`: byte에서 나오는 칸(SHA-256·크기·bulk raw·대표 타입·의존·cookKey)은 기존 항목에서, byte 밖 칸(경로 힌트·flags·bundle)은 이번 판정에서 가져와 항목을 만든다(결정 8).
    - `Finish`: 바뀐 디렉터리를 안쪽부터 `SyncDirectory`한다(NotDurable은 경고). 매니페스트를 만든다(머리 = 프로파일 id·출력 설정·CookSettings SHA-256·쿠커 버전·쿠킹 엔진 버전, 항목은 AssetId 순). 부분 쿠킹은 기존 매니페스트와 병합한다: 기존 항목 중 이번 항목도 사라진 항목(A4b)도 아닌 것을 더하고, `CheckCookedManifestMerge`가 다르면 `IncompatibleCook` 오류로 게시하지 않는다. 그 뒤 `WriteCookedManifest`(검증 포함)와 원자 교체를 한다. 전체 쿠킹은 그 뒤 새 매니페스트가 가리키지 않는 `Packages/` 파일을 지운다(못 지우면 경고, 빈 디렉터리는 남겨도 된다).
    - 실패하면 `Finish`를 부르지 않는다. 이미 옮긴 패키지 파일은 이름이 내용 해시라 기존 매니페스트가 가리키는 파일을 덮지 않는다(같은 이름이면 같은 내용 — 대조로 확인). 그래서 게시본(매니페스트 + 그것이 가리키는 파일)은 그대로다. 남은 새 파일은 다음 전체 쿠킹이 지운다.
  - **재사용 판정**(`PlanCookReuse`): 기존 매니페스트가 있고 머리의 프로파일 id·출력 설정·쿠커 버전이 이번과 같을 때만 본다(다르면 아무것도 재사용하지 않는다). 대상마다 키가 있고, 기존 항목의 cookKey가 같고, 그 파일이 있으며 크기·SHA-256이 같으면 재사용한다. 파일 대조는 흘려 읽는 SHA-256이다(시나리오 27 — 손상·삭제된 파일은 다시 쿠킹한다).
    - 전체 쿠킹도 재사용한다(UE 증분 쿠킹처럼 — cookKey가 판정을 담으므로 CookSettings가 달라도 안전하다). 부분 쿠킹의 병합 조건(CookSettings SHA-256까지 같음)은 게시 때 따로 본다.
    - 부분 쿠킹은 살아 있는 기존 항목을 모두 대상에 넣으므로(A4b 규칙 4) 키가 같으면 재사용, 다르면 다시 쿠킹한다. 그래서 게시기 병합이 그대로 남기는 기존 항목은 같은 id 항목(오류 — 게시하지 않음)뿐이고, 병합 복사는 방어적이다.
    - 실행기는 재사용 대상을 건너뛴다(`CookRunRequest`의 재사용 표지 — 열지 않는다).
  - **보고서**(A4d2, 결정 11): 리플렉션 struct `CookReport`(도구 빌드 ID·엔진 버전·프로파일·대상·쿠킹·재사용 수·쓴 byte·진단 목록)를 ReflectionText `ExportJson`으로 쓴다(결정적 — 진단은 `CookDiagnosticBefore` 순). 게시 성공·실패 모두 쓴다(실패도 원인을 남긴다).
  - **전체 흐름**(A4d2, `CookProject`): 설정 읽기(A4a) → 대상(A4b) → 키(A4c1) → 재사용 판정 → 실행(A4c2, 출력 = 게시기) → 오류 진단이 하나라도 있으면(대상·키·실행) 게시하지 않고 보고서만 → 아니면 Finish.
    - 색인은 호출자가 쿠킹 직전에 갱신해 넘긴다(A4b 콘텐츠 해시 한계 — Rebuild 또는 감시 갱신. 에디터 안에서는 열린 패키지가 색인 항목을 가리키므로 흐름이 스스로 Rebuild하지 않는다).
    - 에디터 프로세스 안 쿠킹은 dirty 패키지가 있으면 Busy다(결정 7 — 실행기가 본다).
- A4e 구현 세부(2026-11-23 — 권장안으로 확정, 결정 4·9·12를 구체화. A4e1·A4e2로 나눈다):
  - A4e1 = `asset-cook` 명령·`--verify-nuggets`·`NuggetMismatch` 등록. A4e2 = 리플렉션 요구 둘(필드 지정자 `AssetBundles`·클래스 표지 `EditorOnly`)·저장 때 bundle 태그·EditorOnly 에셋 규칙.
  - **명령 자리**: AssetCommandlets `AssetCookCommandlet.h`의 `RunAssetCookCommand`(문맥 `AssetCommandContext` — 다른 에셋 명령과 같다). AssetCommandlets가 AssetCooker에 의존한다. 제품 실행 파일(Commandlet 프로그램)이 등록한다.
  - **문법**(13절): `asset-cook --profile=<id> [<path>…] [--verify-nuggets] [--verify-nuggets-source=local|remote] [--json] [--project=<dir>] [--wait=<초>]`. 값 옵션 두 번·빈 값·모르는 옵션·`--profile` 없음은 사용법 오류 2다. `--verify-nuggets-source`는 `--verify-nuggets` 없이 쓰면 사용법 오류다(효과가 없는 옵션을 조용히 받지 않는다).
  - **흐름**:
    1. 세션을 쓰기로 연다(`OpenSession` — 잠금을 잡은 뒤 색인을 만든다. 그래서 색인은 쿠킹 직전의 디스크다 — A4b 콘텐츠 해시 한계).
    2. AssetCooker 모듈을 시작해 쿠킹 설정·보고서 struct를 세션 TypeRegistry에 등록한다(세션 등록기가 붙어 있으므로 시작하는 모듈의 타입이 들어간다 — ModuleLoader 3.11절. 이미 시작했으면 붙일 때 이미 들어갔다).
    3. 설정: 엔진 설정 디렉터리 = `FindEngineDirectory`(HostConfig) + `/Config`, 프로젝트 설정 디렉터리 = layout의 Config. `ReadTargetProfile`·`ReadCookSettings`(codec 문맥은 세션 패키지 집합의 Registry) 뒤 두 SHA-256(`HashTargetProfile`·`HashCookSettings`)을 만든다. 거부는 실패 줄(아래 출력) 뒤 1이다.
    4. 너겟 캐시: `ReadNuggetCacheSettings` → `NuggetCache::Open`(시계는 `NuggetSystemClock`). 프로젝트 세대는 캐시의 `ProjectGeneration()`이다(너겟 키 공통 머리와 같은 값 — 결정 5). required가 아닌데 열지 못해 꺼졌으면 경고 줄 하나(`<오류>: <로컬 경로>: warning NuggetCacheDisabled`)를 쓰고 계속한다. 설정 거부·required 열기 실패는 실패 줄 뒤 1이다. 명령 끝에서 `Shutdown`한다.
    5. `CookProject`(A4d2 — 처리기 표는 기본 처리기뿐이다. 타입별 처리기는 그 단계가 이 표에 더한다 — A5 텍스처).
  - **verify**(결정 12, 9.1절 결정 16):
    - `CookProjectRequest`·`CookRunRequest`·`CookContext`에 `verifyNuggets`(bool)를 둔다. 처리기는 이것이 켜져 있으면 너겟 적중을 쓰지 않고 다시 만든 결과와 적중 결과를 byte로 대조하고, 다르면 진단 `NuggetMismatch`(오류 — error는 InvalidArgument)로 실패한다(실행기가 에셋 오류로 모은다 — 게시하지 않음).
    - verify 쿠킹은 재사용하지 않는다(재사용 대상은 처리기를 거치지 않으므로 대조가 비게 된다 — UE `-VerifyDDC`도 모든 대상을 다시 만든다). `PlanCookReuse`를 부르지 않는다.
    - `--verify-nuggets-source=local`(기본)은 로컬 backend 적중을 대조한다. `remote`는 Rockpile backend가 생기기 전까지 실행 실패 `Unsupported`(1)다(문법은 13절대로 받는다).
    - `AssetDiagnosticCode::NuggetMismatch`를 등록한다(오류 수준 — 마지막 열거자 뒤에 더한다).
    - A4의 기본 처리기는 너겟을 쓰지 않아 verify 결과가 늘 같다. 흐름 시험은 시험 처리기(verify면 NuggetMismatch)로 진단이 보고서·명령 출력까지 가는지 본다. 실질 대조는 A5 텍스처 처리기다.
  - **출력**:
    - 진단은 표준 오류에 `asset-cook: <코드>: <대상>: <error|warning>[ <원인 오류>][ (related <대상>)]` 한 줄씩(보고서와 같은 순). 코드는 진단 코드 이름, 없으면 원인 오류 이름이다. 대상은 에셋이면 색인의 논리 경로(색인에 고유하게 없으면 AssetId 표준 문자열), 에셋이 없는 경로 진단(InvalidPath·DuplicatePath)은 부분 쿠킹이면 그 인자 경로·전체 쿠킹이면 `alwaysCook[<순번>]`, 그 밖은 쿠킹 루트다.
    - 요약은 표준 출력에 `asset-cook: <프로파일> <published|not published> targets <n> cooked <n> reused <n> bytes <n>[ partial]` 한 줄이다. `--json`이면 요약 대신 쓴 보고서(CookReport.json) 내용을 그대로 쓴다(진단 줄은 표준 오류에 그대로 쓴다).
    - 종료 코드: 게시 0(경고만 있어도), 오류 진단으로 게시하지 않음 1, 실행 실패 1(실패 줄), 사용법 2. 할당 실패는 명령 오류(OutOfMemory)다.
  - **A4e2a — 리플렉션 요구 둘**(ReflectionFoundations 2.2·2.3·2.6절에 행으로 적는다 — 리플렉션 쪽 문서가 정본):
    - **클래스 표지 `ROCKS_CLASS(EditorOnly)`** = `TypeFlags::EditorOnly`(32). 쿠킹 데이터에 없는 클래스다. WorldOnly처럼 **상속된다**(MetaCompiler가 base에서 더하고, TypeRegistry는 EditorOnly base의 파생이 플래그를 갖는지 검사한다 — 수동 등록 대비). UE에는 UHT 클래스 지정자가 없고 `UObject::IsEditorOnly()` 재정의(가상 — 파생이 물려받는다)와 편집기 모듈 클래스가 같은 뜻이다(UE 5.8 `SavePackage`·쿠커가 `IsEditorOnlyObject`로 거른다). Rocks는 판정을 실행 코드가 아니라 선언으로 둔다(패키지를 열지 않는 대상 계산이 색인의 대표 타입만으로 판정해야 한다 — 결정 5). WorldOnly·Abstract와 함께 쓸 수 있다.
    - **필드 지정자 `ROCKS_PROPERTY(AssetBundles="a,b")`**(UE `meta=(AssetBundles="a,b")`): 쉼표 목록, 이름마다 ASCII 식별자(`[A-Za-z_][A-Za-z0-9_]*`, 1–`AssetBundleNameMaxBytes`(64) byte), 목록 안 중복 금지(대소문자 구분 — 태그 키가 `Bundle.<이름>`이다), 이름 수 1–`AssetBundleMaxNames`(16). 위반은 RMC2212. 쓸 수 있는 필드: 값(Array·StaticArray·Set·Optional의 원소, Map의 키 또는 값 포함)이 SoftObjectRef이거나 struct이거나 InstancedStruct인 필드(InstancedStruct는 내용 struct를 따라가므로 사슬 제한에 든다). 그 밖은 RMC2202다 — `AssetId` 값 필드는 참조 간선이 아니라(쿠킹 패키지 Dependencies에 남지 않는다) bundle 구성원이 될 수 없고(구성원은 그 항목의 런타임 의존이어야 한다 — 10.1절 5), EditorOnly 필드는 쿠킹 패키지에서 빠지므로 막는다(교차 검증 필수). 바깥 필드가 EditorOnly인 struct 멤버의 목록은 막지 않는다 — 같은 struct를 다른 자리에도 쓸 수 있고 Registry 수동 등록은 바깥 문맥을 모른다. 그 경로는 태그 수집(A4e2b)이 건너뛴다(A4e2a 리뷰 권장). struct 쪽은 UE처럼 **사슬 제한**이다 — 소프트 참조의 실효 bundle은 대표 에셋에서 그 값까지 경로에 있는 `AssetBundles`들의 교집합이고(UE `InitializeAssetBundlesFromMetadata_Recursive`가 속성 사슬의 집합을 교차한다), 경로에 하나도 없으면 어느 bundle에도 들지 않는다. 교집합이 비면 들지 않는다 — UE는 중간 교집합이 비면 더 깊은 메타가 집합을 다시 채운다("비었으면 초기화, 아니면 교차"). Rocks는 엄격한 교집합으로 둔다(바깥 목록이 늘 제한이 되어 일관된다 — UE와 다른 점).
    - 스키마: `FieldSchema::assetBundles`(정규 텍스트 — 쉼표로 이은 이름, 비면 없음). 저장 때만 읽는 편집 정보라 UiToolkit 7.3 편집 메타데이터 블록(CU1)이 생기면 그리로 옮긴다(그때 `ROCKS_WITH_EDITOR_DATA=0` 빌드에서 빠진다). Registry는 깊게 복사하고 등록 때 같은 규칙(이름 문법·수·중복·필드 종류)을 다시 본다(수동 등록 대비 — InvalidArgument). 상속 필드 일치 비교에 넣는다.
    - 버린 안: bundle을 타입 hook(A2c)으로 타입마다 손으로 쓰기 — UE와 다르고 필드를 더할 때마다 hook을 고쳐야 한다. 필드 플래그 비트 하나 + 별도 표 — 이름 목록을 담을 곳이 결국 필요하다.
  - **A4e2b — 저장 때 bundle 태그·EditorOnly 규칙**:
    - **bundle 태그**: 에디터 패키지 저장(`EditorPackageSet` — Tags를 저장마다 다시 만드는 자리)이 타입 hook Tags 뒤에 대표 에셋의 값을 공용 순회(`reflection_text::VisitValues` — 출력 제외는 Report와 같다, Transient만 뺀다)로 돌며, 경로의 어느 단계든 EditorOnly인 필드는 건너뛰고(쿠킹 패키지 쓰기와 같은 제외 — 수동 등록 스키마도 막는다, 교차 검증 필수) 실효 bundle이 있는 null 아닌 소프트 참조마다 태그 `Bundle.<이름>` = 대상 AssetId 표준 문자열을 더한다(같은 쌍은 sink가 합친다). 모든 에셋에 쓴다(PrimaryAsset 여부는 쿠킹 설정이 정하므로 저장 때 알 수 없다 — 쿠커는 PrimaryAsset의 태그만 읽는다, A4b 규칙 3). InstancedStruct 내용도 따라간다(순회가 내용 struct를 준다). 하위 객체(UE `CPF_InstancedReference`·`IncludeAssetBundles`)는 따라가지 않는다 — 인스턴스 하위 객체 참조 표지가 리플렉션에 없다(생기면 더한다). 소프트 참조의 객체 경로(하위 객체)는 버리고 패키지 AssetId만 쓴다(UE도 `GetLongPackageName`으로 패키지를 본다).
    - 공용 순회의 필드 이벤트에 그 필드의 스키마를 함께 넘긴다(`ValueVisitor::Field(name, field)` — 순회를 따로 만들지 않는다, 순회 사용처 둘을 함께 고친다).
    - **EditorOnly 에셋 규칙**(결정 4): 대표 타입이 EditorOnly인 에셋은 NeverCook과 같은 규칙이다 — 뿌리면 빼고(alwaysCook·인자 경로가 직접 가리키면 경고 `NeverCookReferenced`, PrimaryAsset 규칙으로만 닿았으면 진단 없음), Hard·bundle 참조는 오류 `NeverCookReferenced`, Soft는 제외 목록. 대표 타입의 플래그는 TypeRegistry로 본다. 색인의 대표 타입은 이 프로세스에 로드되지 않은 모듈의 타입일 수 있으므로(교차 검증 필수) 미등록(NotFound)은 EditorOnly가 아닌 것으로 두고 판정을 키 단계에 맡긴다 — 키 단계가 그 에셋에 `UnsupportedVersion`(error NotFound) 오류를 내므로 쿠킹은 게시되지 않고 진단은 한 번이다. 그 밖의 조회 오류(ShuttingDown 등)는 함수 오류다.
    - bundle 구성원의 엄격도(Hard와 같은 오류 규칙 — 없거나 NeverCook·EditorOnly인 구성원은 오류)는 의도한 결정이다(교차 검증 권장 확인): 매니페스트 bundle 구성원은 그 항목의 런타임 의존이어야 하고(10.1절 5) 런타임 bundle 로드가 실패하지 않아야 한다. UE는 bundle 데이터를 AssetRegistry에 기록만 하고 쿠킹을 막지 않는다(차이).
    - **쿠킹 패키지 안 EditorOnly 객체**: 쿠킹 패키지 쓰기는 EditorOnly 클래스 객체(상속 포함)를 저장 불가 객체처럼 뺀다 — export 집합에서 그 부분 트리를 빼고, 가리키는 참조는 null + `ExternalReference` 진단(value 5 = EditorOnly 클래스 — 쓰기 실패가 아니라 기록 진단, Transient 클래스 참조와 같다)이다. UE `SavePackage`가 쿠킹 때 편집기 전용 객체를 export·import에서 빼고 그 참조를 null로 쓰는 것과 같다(쿠킹 실패가 아님). UE의 `HasNonEditorOnlyReferences()`(편집기 전용 객체를 남기는 선택 장치) 대응은 없다(차이 — 요구가 생기면 클래스 표지로 더한다). 저장 정책에 저장 불가 마스크를 두고(기본 = Transient·Deprecated, 쿠킹 쓰기 = + EditorOnly) 에디터 패키지 저장은 그대로다.
- A4e2b 상태: **구현·코드 리뷰 충족(빌드·시험 실행 대기 — 사용자 지시로 빌드는 화면 마일스톤 뒤 한 번에)** — 리뷰 1차 미충족(Opaque 필드 저장 실패·시험 태그 수) → 반영 충족(2026-11-23) — 대조표 14.1.77절, coverage 14.1.78절(빌드 때 잰다). 구현에서 정한 것:
  - 수집기는 AssetDatabase `AssetBundleTags.h`의 `CollectAssetBundleTags(registry, access, object, allocator, sink)`다. 공용 순회의 이벤트로 경로 필드 단계(목록·EditorOnly 표지·열린 묶음 수)를 쌓고, 소프트 참조 값·Map 소프트 키마다 단계 목록의 교집합으로 태그를 만든다. 단계는 묶음이 모두 닫히거나 단일 값이 끝나면 내린다.
  - 사전 판정: 대표 타입(과 대상 struct 멤버, 재귀)이 bundle 목록도 InstancedStruct도 없으면 순회하지 않는다(저장마다 모든 에셋을 순회하지 않는다 — 큰 메시 에셋 저장 비용). 거짓으로 판정한 struct id는 기억해 같은 struct를 다시 보지 않는다(리뷰 권장).
  - 순회 옵션 `skipFieldsWithoutText`(ReflectionText `TextCodecOptions`)로 텍스트 꼴이 없는 필드(접근기 없는 Opaque)를 건너뛴다 — 그런 필드가 있는 에셋도 저장된다(리뷰 필수 — 처음 구현은 순회가 Unsupported로 멈춰 그 에셋 저장이 실패했다). 텍스트 출력·너겟 키 기록은 이 옵션을 쓰지 않는다.
  - 공용 순회의 필드 이벤트는 `Field(name, field, flags)`로 바뀌었다(필드 스키마와 유효 플래그 — 바깥 EditorOnly 전파 포함). 사용처는 너겟 키 기록(`NuggetReflected` — 이름만 쓴다)과 순회 시험이다.
  - 태그 키 접두는 AssetRuntime `PackageTags.h`의 `AssetBundleTagPrefix`로 옮겼다(쓰는 쪽 AssetDatabase와 읽는 쪽 AssetCooker가 같이 쓴다 — 전의 `CookBundleTagPrefix`는 없앴다).
  - 쿠커의 EditorOnly 판정은 NeverCook 판정(`IsNever`)에 합쳤다 — NeverCook이 쓰이는 모든 자리(뿌리·폐포·역방향 Build·기존 매니페스트 항목)가 같은 규칙이다. 대표 타입 0(대표 없음)은 판정하지 않는다.
  - 그래프 저장 정책에 `unsaveableTypeFlags`(기본 Transient·Deprecated)를 두고 `IsUnsaveableType(lease, mask)`가 받는다. 패키지 writer는 쿠킹이면 EditorOnly를 더한 같은 mask로 export 집합과 그래프 저장을 함께 거른다(둘이 어긋나면 저장이 집합 안 저장 불가 객체로 InvalidArgument가 된다).
- A4e2a 상태: **구현** — 대조표 14.1.75절, coverage 14.1.76절. 구현에서 정한 것:
  - 상속 클래스 비트를 `InheritedTypeFlags`(WorldOnly·Transient·Deprecated·EditorOnly — MetaSchema `TypeSchema.h`)로 묶었다. TypeRegistry의 파생 검사와 손 등록 파생(`NativeClass` — 리플렉션 조상 플래그 상속)이 함께 쓴다. 전에는 손 등록 파생이 WorldOnly만 물려받아 Transient·Deprecated 조상의 손 등록 파생은 등록이 거부됐다(같은 상수로 고쳤다).
  - bundle 목록 판정은 런타임 `IsAssetBundleList`(constexpr)와 MetaCompiler `Specifiers.CanonicalAssetBundles`가 같은 규칙(이름 문법·길이·수·중복)이다. MetaCompiler는 이름 앞뒤 공백을 떼어 정규 텍스트로 만들고(UE `ParseIntoArrayWS`처럼 공백을 받는다), 런타임·descriptor는 정규 텍스트만 받는다.
  - 생성 `FieldSchema` 초기화에서 bundle 목록은 마지막 칸이라, 목록이 있는 단일 값 필드는 element·maxElements·key를 빈 값으로 채워 쓴다(목록이 없는 필드의 생성 텍스트는 그대로 — 모든 생성 소스가 다시 생성되지 않는다).
  - MetaCompiler descriptor 형식을 3으로 올렸다(필드 `assetBundles`). 이전 형식은 거부한다(설치형 SDK도 다시 만든다).
  - EditorOnly struct 필드 안 멤버의 목록은 받는다(리뷰 권장 — 처음에는 MetaCompiler만 막았으나 Registry가 같은 규칙을 볼 수 없고 그 struct를 어느 EditorOnly 필드에도 못 쓰게 해 과했다). EditorOnly 경로와 InstancedStruct 내용은 수집기(A4e2b)가 건너뛴다.
  - 손 등록 파생(`NativeClass`)이 리플렉션 조상의 Transient·Deprecated·EditorOnly도 물려받게 바뀐 것은 결함 수정이다(시험 `NativeReflectedBase_InheritsTransientFromReflectedBase`).
- A4e1 상태: **완료**(리뷰어 판정 충족, 2026-11-23) — 대조표 14.1.73절, coverage 14.1.74절. 리뷰 반영(필수 하나 — 경로 진단 대상은 인자 유무가 아니라 실제 쿠킹 모드(`result.partial`)로 고른다. 기존 매니페스트가 없거나 깨져 전체 쿠킹으로 바뀌면 순번은 alwaysCook 순번이다 — 인자 배열을 그 순번으로 읽으면 범위 밖일 수 있었다). 구현에서 정한 것:
  - 명령 시험은 EngineTests가 명령 함수를 이 프로세스에서 부른다(시험 타입 모듈·시험 임포터로 에셋을 만든다). 제품 실행 파일 시험(Rocks.Commandlet.Tests)은 실제 모듈 로더가 쿠커 모듈을 시작하고 실행 파일 위쪽 `Engine/Config` 프로파일로 쿠킹하는지 본다.
  - 명령 시험 공용 도구(출력 수집·시험 임포터 등록·디스크 프로젝트)는 `AssetCommandlets/Tests/AssetCommandletTestSupport.h`로 옮겼다(임포트 명령 시험과 같이 쓴다 — 동작 불변).
  - 설정 실패 줄은 `<코드>: <파일>: <칸 설명> (<원인 오류>)`이다. 파일 밖 판정(id 형식·합친 bulkAlignment)은 `TargetProfiles/<id>`, 칸 없는 거부는 엔진 프로파일 파일이 없는 것(`the file does not exist`), 칸 없는 실패는 읽기 오류(`cannot read` — 코드 자리에 원인 오류)다. 값 대입 진단은 그 앞에 `<TextDiagnostic 코드>: <경로>: <설명>` 줄로 쓴다. 너겟 설정 실패도 같은 모양이다(`<프로젝트>/Config/AssetNuggets.json`).
  - 부분 쿠킹의 조기 병합 판정(A4d2 권장)은 명령에서 `IncompatibleCook: <쿠킹 루트>: error Unsupported` 줄과 `not published … partial` 요약이 된다(게시본 규칙과 다른 CookSettings로 부분 쿠킹하면 먼저 전체 쿠킹해야 한다).
  - verify 쿠킹은 매니페스트 byte가 같다(기본 처리기 — 재사용과 다시 쿠킹이 같은 byte, 시나리오 13과 같은 근거).
- A4d2 상태: **구현** — 대조표 14.1.71절, coverage 14.1.72절. 구현에서 정한 것:
  - API는 AssetCooker `CookProject.h`의 `CookProject(request, result)`와 리플렉션 struct `CookReport`·`CookReportDiagnostic`이다(진단 줄은 코드·수준·AssetId 표준 문자열·순번·오류 이름).
  - 보고서는 `ExportJson(TextUse::Report)`과 원자 교체로 쓴다. 보고서 struct의 codec 문맥은 집합의 Registry다. AssetCooker 타입을 등록하지 않았으면 보고서 쓰기의 타입 조회 오류(NotFound)다.
  - 보고서는 흐름이 끝까지 오면 늘 쓴다(게시 성공·규칙 위반 실패 모두). 함수 오류(IO·잠금·할당 등)로 멈추면 쓰지 않는다.
  - 기존 매니페스트:
    - 읽기는 Core 공용 `ReadWholeFile`(`Core/FileRead.h` — 상한 CookedManifestMaxFileBytes)로 한다. A4a CookConfig의 사본을 이것으로 바꿨다. Config·AssetDatabase의 비공개 사본은 후속이다(Config는 다른 세션 몫).
    - 깨졌으면(크기 상한 초과 포함 — `CorruptPackage` / CapacityExceeded) 경고 뒤 없는 것으로 보고 전체 쿠킹한다(사용자가 지우기 전까지 쿠킹이 멈추지 않게 — 리뷰 권장). 읽기 IO 오류는 함수 오류다. 상한은 요청 칸 `manifestLimitBytes`(기본 CookedManifestMaxFileBytes — 시험이 낮춘다)다.
    - 부분 쿠킹은 병합 호환(`CheckCookedManifestMerge` — 프로파일·출력 설정·CookSettings·쿠커 버전)을 대상 계산 전에 본다. 다르면 쿠킹하지 않고 오류 `IncompatibleCook`과 보고서로 끝낸다(헛된 쿠킹을 막는다 — 리뷰 권장). 게시기 Finish의 같은 대조는 방어적으로 남는다.
  - 기존 매니페스트가 없으면 인자가 있어도 전체 쿠킹이다(결정 10 — partial 거짓). 재사용 판정은 전체·부분 모두 기존 매니페스트로 한다.
  - 진단은 대상·키·실행·게시 순으로 모아 `CookDiagnosticBefore`로 정렬한다. 오류가 하나라도 있으면 Finish를 부르지 않는다.
  - 비용(리뷰 권장): 부분 쿠킹도 살아 있는 기존 항목을 모두 재사용 판정에 넣으므로 `PlanCookReuse`가 매번 게시된 패키지 전부를 흘려 해시한다(쿠킹 결과 byte에 비례 — 시나리오 27의 같은 크기 손상 감지와의 균형). 후속 두 길: (a) 지난 게시 때 패키지 파일의 (크기·수정 시각)을 기록해 같으면 해시를 건너뜀, (b) 전체 SHA 대조를 `--verify` 같은 선택으로 둠.
  - JSON 내보내기와 `JsonWriter::Finish`는 한 갈래로 본다(내보내기가 성공하면 Finish의 실패는 끝 줄바꿈 append의 할당 실패뿐이다).
- A4d1 상태: **구현** — 대조표 14.1.69절, coverage 14.1.70절. 구현에서 정한 것:
  - 공용으로 올린 것:
    - AssetDatabase `AtomicFileWrite.h`의 `WriteFileAtomically`를 비공개에서 공개로 옮겼다(namespace rocks, export).
    - Core `Encoding/Sha256File.h`의 `HashFileSha256(file, buffer, digest, bytes)`를 새로 두었다. 연 파일을 조각으로 흘려 읽는다. 색인 ContentHash·게시 대조·재사용 판정이 쓴다(임포트 원본 대조·셰이더 도구·너겟의 기존 사본은 기대 크기 대조 등 계약이 달라 이번에 바꾸지 않았다).
  - 시험 대역 `MemoryFileSystem`에 `SyncDirectory`(고장 `faults.sync`, 호출 수 `syncs`)를 더했다. 기본 구현은 Unsupported라 게시가 늘 실패했다.
  - staging 임시 이름은 `.staging/<대상 파일 이름>.tmp`다(Guid 없음). `.staging`은 잠금을 쥔 쿠커만 쓰고 Begin이 비운다. 한 쿠킹에 같은 패키지는 한 번만 넣으므로 겹치지 않는다. 열기는 CreateAlways다.
  - bundle 구성원은 그 항목의 의존이어야 한다(AS 10.1절 5 — `ValidateBundleMembers`). `AssetBundles`가 Soft 참조 필드라 정상 입력은 늘 맞는다. 어기면 Finish의 매니페스트 검증 오류로 게시하지 않는다.
  - 게시하지 않는 오류 진단(IncompatibleCook·매니페스트 검증 위반)은 함수 성공 + 오류 진단이다(규칙 위반은 진단). 진단 push의 할당 실패는 OutOfMemory다. 경고 push의 할당 실패도 OutOfMemory다 — 처음 구현은 버렸으나 A4d2 할당 실패 시험이 "주입했는데 성공"으로 잡았다(경고를 조용히 잃지 않는다).
  - 정리는 `Packages/<a>/<파일>` 두 단계만 훑는다. 그 밖의 모양(최상위 파일·링크)은 쿠커가 만든 것이 아니라 두고, 빈 shard 디렉터리도 남긴다.
  - 실행기의 재사용 표지는 `CookRunRequest::reuse`다(표지 수가 대상 수와 다르면 InvalidArgument).
  - 첫 쿠킹 동기화(리뷰 필수): Begin이 `DirectoryExists`로 없던 상위 디렉터리를 찾아 그 부모를 동기화 목록에 넣는다(확인 실패는 오류). 시험 대역 `MemoryFileSystem`에 호출 기록(`recordJournal`·`journal` — 성공한 SyncDirectory·Replace)을 더해 "모든 동기화가 매니페스트 교체보다 먼저"를 순서로 본다.
  - 부분 쿠킹의 기존 항목(리뷰 필수): 게시기가 아니라 대상 계산(A4b 규칙 4)에서 막는다. 살아 있는 기존 항목이 모두 키 계산·재사용 판정을 거치므로 cookKey가 달라진 항목은 다시 쿠킹된다.
  - 후속(리뷰 권장): `WriteFileAtomically`는 에셋 DB와 무관한 범용 기능이라 Core·PlatformBase 파일 유틸로 옮긴다. 해시 루프 사본 중 `CheckImportSource`(AssetSourceRoots — 크기 비교 뒤 흘려 해시)는 `HashFileSha256`으로 바꿀 수 있다(줄어든 파일의 진단이 IO 오류에서 SourceChanged로 바뀌는지 시험과 함께). `NuggetFile`의 범위 reader 검증은 File이 아니라 계약이 달라 남긴다. ShaderToolLoader는 다른 세션 몫이다.
- A4c2 상태: **구현** — 대조표 14.1.67절, coverage 14.1.68절. 리뷰 반영(2026-11-23, 필수 하나·권장 둘 — 매니페스트 의존 거르기 제거·결정 4·8 문장, 로더 대조 자기 검증, cookKey 제외 Soft 근거). 구현에서 정한 것:
  - 기본 처리기는 `CookProcessor::Cook`의 기본 구현이다(에디터 그래프를 그대로 쿠킹 패키지로). `DefaultCookProcessor`는 정체성(`DefaultCookProcessorId`)만 정한다. 특화 처리기(A5 텍스처)는 Cook을 재정의한다. CookProcessor는 export 클래스다(Modular 빌드에서 기본 Cook을 시험 처리기가 물려받는다).
  - API는 AssetCooker `CookRun.h`의 `CookPackages(request, result)`다.
    - 결과(`CookRunResult`): 진단, 쿠킹 수, 대상 Open 호출 수, 상한으로 한꺼번에 닫은 횟수.
    - 출력: `CookOutputSink::Put(CookedPackage)` — span은 Put 동안만 유효하다.
  - 에셋별 진단: 읽기 전용 패키지(더 새 타입·커스텀 버전 — 지금 스키마로 쓰면 모르는 데이터를 잃는다) `UnsupportedVersion`, 열기 실패는 Open의 진단 코드·오류, null로 이은 import는 `MissingDependency`(related = 첫 미해결 대상), 처리기 실패는 그 진단 코드·오류, 쿠킹 byte 검증 실패는 `CorruptPackage`(error = 읽기 오류). 열기·처리기·검증의 할당 실패는 진단이 아니라 OutOfMemory로 멈춘다(규칙 위반으로 바꾸지 않는다).
  - 멈춰도 이번 쿠킹이 연 패키지는 닫는다. 닫기 자체가 할당에 실패하면 패키지가 열린 채 남고 OutOfMemory다(`EditorPackageSet::CloseOpened`는 할당 실패면 아무것도 닫지 않는다 — 호출자가 시작 때 목록으로 다시 닫는다).
  - 쿠킹 순서의 Hard 간선은 대상 사이만이다. 대상 밖 Hard 의존은 A4b 오류다.
  - 키가 없는 대상은 진단 없이 건너뛴다(원인 진단은 키 단계).
  - 매니페스트 의존은 쿠킹 패키지 Dependencies 그대로다(로더가 같은지 대조한다 — 처음 구현은 제외·색인에 없는 Soft를 빼서 로드 대조에서 거부될 결함이었다, 리뷰 필수). bulk raw 합은 BulkIndex rawBytes 합이다. 만든 항목은 `CheckCookedPackage`로 자기 검증한다(처리기가 프로파일 값·대표 타입을 다르게 쓰면 `CorruptPackage` — 리뷰 권장).
- A4c1 상태: **구현** — 대조표 14.1.65절, coverage 14.1.66절. 구현에서 정한 것:
  - `GuidLess`를 AssetRuntime `PackageReader.h`에서 Base `Guid.h`로 옮겼다. ObjectRuntime(ListTypeVersions 정렬)도 쓰므로 공용 자리에 둔다. 이름이 같아 기존 사용처는 그대로다.
  - 진단 순서는 `CookTargets.h`의 `CookDiagnosticBefore`((asset, code, related, setting, 오류 먼저, 원인 오류)) 하나다. 대상 계산·키·이후 보고서가 같은 순서를 쓴다.
  - 키 계산의 영역 표(노드·실패 표지·id 표)는 색인 항목 수만큼 미리 잡는다(영역 ⊆ 색인의 고유 항목). 그래서 노드 넣기는 할당하지 않는다. 진단 push의 할당 실패는 A4b처럼 기록해 두고 끝에서 돌려준다.
  - cookKey 지역 입력 기록은 실패하지 않는다(문자열은 LogicalPath·PackageTags가 검증한 UTF-8이고, 해시 기록기는 할당하지 않는다). 불변식으로 단언한다. 처리기 설정 키의 writer 오류는 처리기 계약 위반이라 함수 오류로 돌려준다.
  - 대상이 아닌 영역 노드로 가는 Hard 간선(A4b가 이미 오류로 보고한 NeverCook 참조 등)은 Hard 그래프에서 뺀다. 그런 대상의 키는 그 간선 없이 정해진다(어차피 게시하지 않는다).
  - 키 진단은 모두 오류다(`CookKeySet::HasErrors` = 진단 있음).
  - 대표 타입의 처리기 조회 실패는 7.1절 8의 진단 모델을 따른다: 이 프로세스에 등록되지 않은 타입(NotFound)은 `UnsupportedVersion`, 등록됐지만 클래스가 아닌 종류(TypeMismatch)는 `TypeMismatch`다(error 칸에 조회 오류 — 리뷰 권장).
  - 타입 표 지문은 프로세스가 로드한 모듈 구성에 달린다. 에디터 안 쿠킹과 Commandlet 쿠킹처럼 로드한 모듈이 다르면 지문이 달라 서로의 매니페스트를 재사용하지 못한다(안전 쪽 — 적중만 잃는다). 범위를 줄이는 후속은 저장된 타입 참조를 색인에 두는 것이다(결정 5).
  - cookKey의 판정 입력(PrimaryAsset·bundle·제외 Soft)은 byte에 영향을 주지 않지만 결정 5대로 보수적으로 넣는다. 매니페스트의 판정 칸은 결정 8대로 매번 다시 만든다.
  - SCC 묶음 표지는 Build·Hard 두 단계에서 번호를 이어 쓴다(다시 비우지 않는다).
  - 리뷰 반영(2026-11-23, 필수 하나·권장 셋): 재사용은 패키지 byte만이고 매니페스트의 byte 밖 칸은 매번 다시 만든다(결정 8 — 시험은 A4d), 미등록 타입 진단을 UnsupportedVersion으로, 타입 표 지문의 모듈 구성 의존 기록, 결정 6 본문의 처리기 버전을 Guid로.
- A4b 상태: **구현** — 대조표 14.1.63절, coverage 14.1.64절. 구현에서 정한 것:
  - 콘텐츠 해시는 `AssetIndexContentHash.cpp`(조회는 `FindById`를 그대로 써서 NotFound·AlreadyExists가 같다). 색인 캐시 schemaVersion 3은 readOnly 뒤에 표지(u8, 0·1 밖은 캐시 없음)·digest·크기(u64)·시각(u64)을 둔다.
  - 뿌리 진단을 더 정했다: 에셋 경로가 같은 경로 키 파일 둘 이상을 가리키면 오류 `DuplicatePath`(asset null, setting = 순번 — 어느 것도 뿌리로 고르지 않는다), 같은 id 파일은 alwaysCook·인자 경로·디렉터리 목록·PrimaryAsset 규칙 어디서 닿아도 오류 `DuplicateAssetId`(asset = 그 id), 역방향 Build 참조자가 같은 id면 오류 `DuplicateAssetId`(asset = 대상, related = 참조자 id). 디렉터리가 가리킨 NeverCook 항목도 "직접 가리킴"이라 경고다.
  - 대상 큐는 색인 항목 포인터다(고유 항목만 들어온다 — 의존·태그를 다시 찾지 않는다). 진단·대상·제외 목록의 할당 실패는 기록해 두고 계속한 뒤 끝에서 OutOfMemory로 돌려준다(호출마다 분기하지 않는다). bundle 구성원은 태그 순서((키, 값) 정렬·중복 없음)를 써서 마지막 bundle에 붙이고 AssetId 순으로 정렬한다.
  - 비교는 `GuidLess`(AssetRuntime)를 쓴다(따로 만든 비교 함수 없음).
  - PrimaryAsset 타입 조회: 없는 타입(NotFound)만 설정 위반 InvalidArgument이고, 그 밖(종료 뒤 ShuttingDown·lease 포화 Overflow)은 TypeRegistry 오류 그대로다.
  - 리뷰 반영(2026-11-23, 필수 하나·권장 넷): 다시 쿠킹하는 기존 항목의 옛 의존 판정 제거(규칙 4), 역방향 Build 범위를 인자 뿌리·역방향 에셋으로 한정(규칙 4), 진단 정렬에 수준, TypeRegistry 오류 보존, 아래 Stat 한계 기록.
  - 콘텐츠 해시의 한계: 크기·수정 시각이 같으면 저장된 해시를 믿으므로, 같은 크기로 다시 쓰면서 수정 시각을 보존한 파일(`cp -p`·`rsync -t`·압축 해제·거친 시각 해상도의 파일 시스템)은 낡은 해시가 남는다. 플랫폼 층 `FileInfo`에 변경 시각(ctime)·파일 id가 아직 없어 더 대조하지 못한다 — 생기면 대조에 더한다. 쿠킹은 이 한계를 줄이려고 A4c 실행 단계에서 쿠킹 전에 색인을 갱신해(감시 갱신 또는 Rebuild) 색인 의존과 파일 내용을 같은 시점으로 맞춘다.
- A4a 상태: **구현** — 대조표 14.1.61절, coverage 14.1.62절. 구현에서 정한 것:
  - 모듈 `Engine/Source/Developer/AssetCooker`(`CookConfig.h` — `TargetProfile`·`PrimaryAssetRule`·`CookSettings`·`CookBulkCodec` 리플렉션 타입, `ReadTargetProfile`·`ReadCookSettings`·`HashTargetProfile`·`HashCookSettings`·`ClassifyCookPath`). 엔진 기본 파일 `Engine/Config/TargetProfiles/Windows.json`(graphicsBackend D3D12)·`Android.json`(빈 backend — 후속).
  - PrimaryAsset 규칙은 `{type, directories}`다(UE `FPrimaryAssetTypeInfo` — 디렉터리는 하나 이상, 빈 목록은 거부, 진단은 규칙 순번 index와 디렉터리 순번 item). 처음 구현은 "codec이 배열 원소 안의 배열에 대입하지 못한다"고 보고 `{type, directory}`로 바꿨으나 틀린 가정이었다 — 그 제약은 Config의 배열 연산 문법(`+=` 등)의 것이고 ImportJson은 원소 안 배열을 통째 교체로 받는다(리뷰 필수 — 확인 없이 스키마를 바꾸지 않는다).
  - 리뷰 반영(필수 둘·권장 둘): `directories` 배열 복원, `WriteReflected` 실패 갈래는 문자열 칸 읽기(FieldValue)가 할당하므로 도달 가능 — 할당 실패 시험에 규칙 SHA-256을 더해 덮었다(처음 근거 "할당하지 않는다"는 틀렸다), 할당 실패 시험의 끝 상태를 기준(시스템 allocator)과 칸·SHA-256까지 대조, 주입했으면 적어도 하나가 실패(삼킨 실패 없음).
  - 프로파일 id는 `[A-Za-z0-9_]` 1–64 byte(파일 이름 — 경로 문자가 들어가지 않게)이고 읽기가 미리 채운다(파일에서 생략 가능, 다르면 거부 — 대소문자 구분). 읽을 때마다 기본값에서 시작한다.
  - 거부는 `InvalidArgument` + `CookConfigFailure{code InvalidSetting, file, field, index, line, column}`, codec 진단은 호출자의 `TextDiagnostics`에 남는다. 읽기·할당 오류는 code None + 그 오류(규칙 위반과 구분 — 경로 판정 `ClassifyCookPath`도 할당 실패를 `OutOfMemory`로 돌려준다, 처음 구현은 규칙 위반으로 바꿨다).
  - 경로: 에셋은 LogicalPath 규칙, 디렉터리는 끝 '/'를 뗀 것이 마운트(`/Game/` — 대소문자 무시)이거나 논리 경로다. 마운트 없는 `/Game`(끝 '/' 없음)은 거부(에셋 경로에는 이름이 필요하다).
  - 설정 파일 상한 `CookConfigMaxFileBytes` 1 MiB.

### 10.3 A4f 셰이더 라이브러리 연동 설계(2026-11-24, 개정 2 — 교차 검증 승인, ShaderSystem 개정 32 SSD10–SSD15·SSD24의 에셋 쪽, 교차 검증 1차 반영)

렌더 세션(SH2 — [ShaderSystem](../ShaderSystem/12-FormatDefinitionsCooking.ko.md) 12.3.1절, 14절 개정 요구)이 에셋 쪽에 요구한 것을 정한다. 셰이더 쪽은 `CookShaderLibraries`와 아래 단계 구현만 맡고(ShaderSystem 16.1절 범위), 이 절의 TargetProfile 칸·매니페스트 v2·비에셋 산출물 단계·`--engine`은 에셋 세션이 구현한다(A4f). 하위호환은 두지 않는다(옛 프로필·v1 매니페스트는 다시 쿠킹한다). 애매한 곳은 UE 5.8과 같게 했다 — UE는 엔진 전역 셰이더 라이브러리("Global")도 프로젝트 쿠킹이 만들고(ShaderCodeLibrary.cpp:2272) backend별 형식 목록을 플랫폼 설정에 둔다(`D3D12TargetedShaderFormats` — WindowsDynamicRHI.cpp:624–626).

**구현 시점**: A5(A5g–A5k) 뒤, A6 전(사용자 결정 2026-11-24). SH2d(셰이더 쪽 마지막 단계 — 순서 SH2a → b → c → e → f → d)보다 먼저 끝나고, 끝나면 렌더 세션(rocks-6b)에 알린다.

**렌더 세션과 맞춘 것(요구 원문과 다른 곳 — 2026-11-24 rocks-6b가 셋 다 받아들임, ShaderSystem 다음 개정에 반영)**: (a) SSD10의 "쿠커가 형식 정의 디렉터리와 대조"를 비에셋 산출물 단계(셰이더 쪽) 시작 때 대조로 옮겼다(1항 — AssetCooker가 ShaderCompiler에 의존하지 않게). (b) `graphicsBackends`가 빈 프로필은 셰이더 없는 프로필이다(1항). 셰이더 쪽 런타임 규칙: 매니페스트 backendCount 0이면 그래픽 장치·렌더러를 만들지 않고(UE 전용 서버의 NullDrv처럼), 그 실행에서 장치 생성을 요청하면 `Unsupported`("this cook has no shader formats")다 — 셰이더 라이브러리를 찾지 않으므로 `CorruptPackage`도 없다. (c) macOS 프로필 id는 Rocks·UE 플랫폼 이름 `Mac`이다(SSD10·SSD24 문구를 고친다).

1. **TargetProfile 칸**(SSD10 — 10.2절 2항의 `graphicsBackend`를 바꾼다):
   - `graphicsBackends`(`Array<String>`): 런타임이 시도하는 backend 순서. 병합은 ReflectionText 기본(배열 통째 교체).
   - `shaderFormats`(`Map<String, Array<String>>`): backend → 선호 순서 형식 id 목록. 병합은 **Replace**(프로젝트 파일에 있으면 Map 전체를 바꾼다 — 키별 병합을 쓰지 않는다). backend 목록과 형식 목록이 반쪽만 덮여 엇갈리지 않게 한다. 리플렉션 필드 지정자로 Map 병합 의미를 Replace로 둔다(10.2절 2항 LOD 그룹 Map의 `MergeByKey`와 같은 방식 — 지정자 이름은 리플렉션 쪽 기존 것을 쓴다, 구현 때 확인).
   - 두 칸은 프로파일 출력 설정 SHA-256에 들어간다(`WriteReflected` — 칸이 바뀌면 전체를 다시 쿠킹한다).
   - 구조 검증(`ReadTargetProfile` — 위반 `InvalidSetting`, `CookConfigFailure` 필드·index): backend 이름이 고정 어휘(`D3D12`·`D3D11`·`Vulkan`·`Metal`·`OpenGL` — 구현 여부와 무관) 밖, `graphicsBackends` 중복, 목록의 backend에 형식 목록이 없거나 비었음, 목록에 없는 backend 키, 형식 id 문법(`[A-Z0-9_]` 1–64 byte) 위반, 프로필 전체에서 형식 id 중복, backend 수가 `CookedManifestMaxBackends`(8) 초과, backend 하나의 형식 수가 `CookedManifestMaxFormatsPerBackend`(16) 초과(4항 매니페스트 상한과 같은 상수 — 통과한 프로필은 늘 쓸 수 있는 매니페스트를 만든다). `graphicsBackends`가 비어 있으면 셰이더 없는 프로필이다(형식 Map도 비어야 한다 — 비에셋 산출물 단계가 셰이더 라이브러리를 만들지 않는다).
   - 형식 정의 대조(형식 id가 형식 정의 디렉터리에 있는지·자리만 둔 형식·형식 정의의 backend가 키와 같은지·`GLES31_PC` 금지)는 형식 정의를 아는 비에셋 산출물 단계(3항 — 셰이더 쪽 구현)가 단계 시작 때 한다(위반은 같은 `InvalidSetting`, 진단 sink로 — 알림 (a)).
   - 엔진 파일(`Engine/Config/TargetProfiles/`): `Windows`(`graphicsBackends` [`D3D12`, `D3D11`, `Vulkan`], `shaderFormats` {`D3D12`: [`D3D12_SM6`], `D3D11`: [`D3D11_SM5`], `Vulkan`: [`VULKAN_SM5`]}), `Android`(`OpenGL`: [`GLES31_ANDROID`] — SH6에서 `Vulkan`: [`VULKAN_ANDROID`]을 앞에 더한다), `Mac`(새 파일 — `Metal`: [`METAL_SM5`]). 프로필 id는 Rocks 플랫폼 이름과 같다(`Mac` — HostSettings 플랫폼 층 폴더, 빌드 `NativePlatform.MacOS => "Mac"`, 11.8절 `asset.runtime_profile` 기본값, UE 5.8 플랫폼 이름 — 알림 (c)).
   - **모듈 분리**: TargetProfile 리플렉션 struct·엔진/프로젝트 병합·구조 검증(`ReadTargetProfile`)·호스트 플랫폼 프로필 조회(`ReadHostTargetProfile` — Windows 호스트 `Windows`, macOS 호스트 `Mac`, 프로젝트 재정의를 합친 결과, SSD24)를 새 Developer 모듈 `TargetProfiles`(Engine/Source/Developer/TargetProfiles — 의존 Core·ObjectRuntime·ReflectionText·AssetRuntime(매니페스트 상한 상수 `CookedManifestMaxBackends`·`CookedManifestMaxFormatsPerBackend` — 가벼운 런타임 모듈))로 옮긴다. 위반 형식은 이 모듈의 `TargetProfileFailure`(code InvalidSetting·file·field·index·line·column — AssetCooker `CookConfigFailure`가 그대로 옮겨 담는다)다. 엔진·프로젝트 config 경로는 호출자가 넘긴다(`ReadTargetProfile(engineDir, projectDir 또는 없음, id)` — ProjectLayout에 의존하지 않는다, 쿠커·셰이더 도구가 자기 경로 해석으로 채운다). `shader-compile`과 Development 실행 파일의 셰이더 공급자(SSD16)가 AssetCooker·에셋 DB를 링크하지 않고 같은 함수를 쓴다(출처는 하나). 프로파일 출력 설정 SHA-256(`HashTargetProfile` — 너겟 키 writer 의존)과 CookSettings는 AssetCooker에 남는다.
2. **CookSettings**: `cookDevelopmentShaders`(bool, 기본 false — SSD11). 참이면 비에셋 산출물 단계가 개발 전용 퍼뮤테이션을 라이브러리에 넣는다. CookSettings SHA-256에 들어간다(`WriteReflected` 그대로).
3. **비에셋 산출물 단계**(SSD11): 에셋 쿠킹(10.2절 7항) 뒤, 게시(10.2절 10항) 전에 실행한다.
   - AssetCooker가 interface를 둔다: `CookArtifactStage{종류 CookArtifactKind, 정체성 NuggetProcessorId, Run(const CookArtifactRequest&, CookArtifactSink&)}`. 요청은 병합한 TargetProfile·CookSettings·쿠킹 루트·staging 디렉터리·NuggetCache·진단 sink·엔진 전용 여부·이전 매니페스트의 같은 종류 artifact 항목 중 파일 대조를 통과한 것(재사용 후보 — 6항)이다. sink는 artifact 항목(이름·형식 id·cookKey)과 그 staging 파일 경로, 또는 재사용할 이전 항목, 그리고 보고서 절을 받는다. **크기·SHA-256은 AssetCooker가 staging 파일에서 직접 계산한다**(단계가 보고한 값으로 내용 주소 이름을 짓지 않는다 — 재사용 항목은 6항 대조).
   - 등록: 단계를 구현하는 모듈이 **모듈 시작 hook**(ModuleLoader — `ROCKS_IMPLEMENT_MODULE`의 모듈 구현 시작 함수)에서 AssetCooker의 단계 표(`CookArtifactStageTable` — 종류별 하나, 같은 종류 두 번 등록은 `AlreadyExists`)에 명시적으로 넣고, 종료 hook에서 뺀다. 정적 초기화로 등록하지 않는다(정적 라이브러리 링크에서 참조 없는 등록 객체가 버려질 수 있다). `asset-cook`을 실행하는 Commandlet은 ShaderCompiler 모듈을 링크하고 로드한다(모듈 표 — ModuleLoader 3절). 의존 방향은 ShaderCompiler → AssetCooker interface이고 AssetCooker → ShaderCompiler 의존은 없다.
   - 프로필의 `graphicsBackends`가 비어 있지 않은데 `ShaderLibrary` 단계가 등록되지 않았으면 쿠킹 실패 `UnsupportedCapability`(셰이더 없는 쿠킹 결과를 게시하지 않는다 — 런타임이 `IncompatibleCook`으로 늦게 실패하지 않게).
   - 단계의 job 실패(컴파일 오류·상한)는 에셋 오류처럼 보고서에 모으고 하나라도 있으면 게시하지 않는다(10절). 이 호스트가 만들 수 없는 형식(macOS의 DXBC 등)은 `UnsupportedCapability` 진단으로 쿠킹 실패다(셰이더 쪽 판정).
   - 순서: 단계는 에셋 쿠킹 뒤에 돈다 — 머티리얼 셰이더 항목은 머티리얼 쿠킹(에셋 처리기)이 남긴 입력을 `Project` 라이브러리에 더한다(머티리얼 시스템, 후속).
4. **매니페스트 v2**(SSD13 — 10.1절 v1을 대체, formatVersion 2, v1 reader는 두지 않는다 — v1 파일은 `UnsupportedVersion`):
   ```
   magic "RMAN" | formatVersion u32 = 2 | entryCount u32
   | profileId string | profileSettingsSha256 (32) | cookSettingsSha256 (32) | cookerVersion u32
   | cookedByEngineVersion: major u32, minor u32, patch u32
   | backendCount u32 | backend × backendCount
   | entry × entryCount
   | artifactCount u32 | artifact × artifactCount
   | crc32 u32
   backend  = name string | formatCount u32 | formatId string × formatCount
   artifact = kind u8 | name string | formatId string | bytes u64 | sha256 (32) | cookKey (32)
   ```
   - backend 표는 쿠킹한 프로필의 `graphicsBackends` 순서와 backend마다 `shaderFormats` 순서다(런타임 형식 선택 SSD15가 실행 파일 설정이 아니라 쿠킹 결과로 고른다). 셰이더 없는 프로필은 backendCount 0.
   - artifact 파일 경로는 칸이 아니라 계산한다: `AppendCookedArtifactPath`가 `Shaders/<이름>-<formatId>-<sha256 앞 16 hex 소문자>.rshl`(SSD12 내용 주소 — 패키지의 `AppendCookedPackagePath`와 같은 방식)을 만든다.
   - **판정 순서**(10.1절 2항에 더한다): (1) 구조 — … 엔진 버전 → backend 개수 칸(상한·남은 byte로 담을 최소 크기 — backend 하나 최소 = 이름 길이 칸 8 + 형식 개수 칸 4) → backend마다 칸(형식 개수 칸 상한·남은 byte, 문자열 길이·UTF-8) → 항목(v1 그대로) → artifact 개수 칸(상한·남은 byte — artifact 하나 최소 = 1 + 8 + 8 + 8 + 32 + 32) → artifact마다 칸 → 끝 위치 = crc 앞. (2) 칸 규칙(`ValidateCookedManifest`) — 헤더 → backend 표(번호 순: 이름 어휘 → 이름 중복 → 형식 수 ≥ 1 → 형식 id 문법 → 형식 id 전역 중복) → 항목 → 항목 사이 → artifact 표(번호 순: kind → 이름 → formatId가 backend 표에 있음 → bytes → (kind, 이름, formatId) 엄격 오름차순). 이 순서를 `CookedManifest_JudgementOrder`의 우선순위 표에 더한다(backend·artifact 위반과 기존 위반이 겹친 경우).
   - 진단(`CorruptPackage`): 새 `CookedManifestField` 칸 `Backend`·`ShaderFormat`(detail = backend 번호), `ArtifactKind`·`ArtifactName`·`ArtifactFormat`·`ArtifactBytes`·`ArtifactOrder`(detail = artifact 번호), 개수 칸 `BackendCount`·`FormatCount`·`ArtifactCount`(detail = backend 번호 또는 `CookedManifestNoEntry`), 런타임 파일 대조 칸 `ArtifactSha256`(8항 — `ArtifactBytes`와 함께).
   - 칸 규칙: kind는 알려진 값(`CookArtifactKind::ShaderLibrary` = 1), 이름은 `Engine`·`Project`, formatId는 backend 표에 있는 형식, bytes는 1 이상 `CookedArtifactMaxBytes`(4 GiB) 이하, 표는 (kind, 이름, formatId) 엄격 오름차순. "엔진 전용 쿠킹은 `Engine` 항목만"은 매니페스트로 판정하지 않는다(엔진 전용 표시가 없다 — 쿠커 불변식으로 시험한다).
   - 상한(이름 있는 상수): `CookedManifestMaxBackends` 8, `CookedManifestMaxFormatsPerBackend` 16, `CookedManifestMaxArtifacts` 4096.
   - `CheckCookedManifestMerge`는 그대로다 — backend 표는 프로파일 출력 설정 SHA-256에 이미 담긴다. 병합은 artifact 표를 (kind, 이름, formatId)로 합친다(이번 쿠킹이 만든 항목이 이긴다).
   - golden byte·14.1.3 대조표·로더 시험의 픽스처를 v2로 다시 만든다.
5. **게시와 정리**(SSD12 — 10.2절 10항과 같은 규칙): 단계가 staging에 쓴 파일을 AssetCooker가 크기·SHA-256으로 이름 지어 `Shaders/<이름>`으로 `Move(no-replace)` → 있으면 크기·SHA-256 대조(같으면 임시 삭제, 다르면 `CorruptEntry` 경고 + `Replace`) → `SyncDirectory` → 매니페스트 `Replace`(전환점은 매니페스트 하나 — 패키지와 같은 거래). 전체 쿠킹은 새 매니페스트가 가리키지 않는 `Shaders/` 파일을 지우고(못 지우면 경고) 부분 쿠킹은 지우지 않는다. 패키징(설치 SDK 포함)은 쿠킹 루트에서 매니페스트와 매니페스트가 가리키는 파일만 복사한다(`.staging`·보고서는 복사하지 않는다).
6. **부분 쿠킹 재사용**(10.2절 8항 규칙 그대로): 흐름은 한 방향이다: AssetCooker가 `Run` 전에 이전 매니페스트의 artifact 항목마다 `Shaders/`의 그 파일 크기·SHA-256이 매니페스트와 같은지 대조하고, 맞는 항목만 요청에 넣는다. 단계는 cookKey(셰이더 쪽 정의 — 라이브러리 형식 버전·형식 정의 SHA-256·셰이더 타입 집합·항목 표·job 키 목록, SSD13)를 계산해 받은 항목 중 같은 (kind, 이름, formatId)·같은 cookKey인 것만 재사용으로 sink에 넘기고 나머지는 새로 만든다(job 키 계산은 늘 하고 너겟 적중이면 컴파일하지 않는다).
7. **엔진 전용 쿠킹 `asset-cook --engine --profile=<id>`**(SSD11 — 10.2절 12항·13절 문법):
   - 프로젝트 없이 실행한다(`--project`·경로 인자와 함께 쓰면 사용법 오류). 에셋 대상은 없다(entryCount 0) — 비에셋 산출물 단계만 엔진 전용 요청으로 돌아 `Engine` 라이브러리만 만든다.
   - 쿠킹 루트 `Engine/Saved/Cooked/<Profile>/`, 너겟 root는 엔진 폴더(셰이더 쪽 `ResolveShaderNuggetRoot`), 프로필은 엔진 파일만(프로젝트 재정의 없음), CookSettings는 `Engine/Config/CookSettings.json`(있으면 — 엔진 시험이 개발 전용 퍼뮤테이션을 쿠킹 결과로 쓸 수 있게, 없으면 기본값).
   - 잠금·색인: 에셋 DB를 열지 않는다(색인 없음 — 대상이 없다). 동시 쿠킹을 막는 쓰기 잠금은 쿠킹 루트 밖의 잠금 파일(`<Saved>/Locks/Cooked-<Profile>.lock` — 엔진 전용은 `Engine/Saved/Locks/`, 8절 에셋 DB 잠금과 같은 플랫폼 잠금 원시, `--wait` 의미도 같다)이다. 프로젝트 쿠킹도 같은 쿠킹 루트 잠금을 쥔다. 잠금 순서는 에셋 DB 잠금 → 쿠킹 루트 잠금으로 고정한다.
   - 엔진만 쓰는 실행(Sandbox·엔진 시험·설치 SDK의 엔진 전용 실행)은 `asset.cooked_root`를 엔진 쿠킹 루트로 둔다(11.8절). 설치 SDK 패키징은 그 결과를 5항 규칙으로 복사한다.
8. **런타임 열기**(SSD14 — 11절 로더 시작): 로더 시작이 매니페스트를 연 뒤 모델의 backend 표와 artifact 표를 공개한다(`CookedManifest::Backends()`·`Artifacts()`·`FindArtifact(kind, 이름, formatId)`). RenderBase 셰이더 집합이 실행 장치에 맞는 형식(SSD15 — 셰이더 쪽)의 `Engine`·`Project` 항목을 찾아 연다. 파일 크기·SHA-256 대조는 패키지와 같은 방식의 새 함수 `CheckCookedArtifactFile`(다르면 `CorruptPackage`, section = `ArtifactBytes`·`ArtifactSha256`)이다. 항목이 없으면 셰이더 쪽이 `IncompatibleCook`이다.
9. **손상 코드**(SSD12·SSD14): 게시 때 같은 이름의 다른 파일 `CorruptEntry`(경고), 런타임 크기·SHA-256 불일치 `CorruptPackage` — 새 코드를 만들지 않는다.

| Rocks | UE 5.8 |
|---|---|
| TargetProfile `graphicsBackends`·`shaderFormats` | 플랫폼 설정 `D3D12TargetedShaderFormats` 등 backend별 형식 목록 |
| 비에셋 산출물 단계(`CookArtifactStage`) | 쿠커가 ShaderCodeLibrary를 열고 저장하는 단계(`UCookOnTheFlyServer`) |
| `Shaders/<이름>-<형식>-<hash>.rshl` | `ShaderArchive-<Library>-<Format>-<Platform>.ushaderbytecode` |
| `asset-cook --engine` | (UE는 프로젝트 쿠킹이 Global 라이브러리를 만든다 — 엔진 전용 실행용 Rocks 추가) |

**시험(A4f — 14.1절 대조표는 구현 때)**: TargetProfile 구조 검증 각 위반·상한·Replace 병합(프로젝트가 한 backend만 적어도 Map 전체 교체)·SHA 변화·호스트 프로필(`TargetProfiles` 모듈 단독 — AssetCooker 없이), CookSettings `cookDevelopmentShaders` SHA, 단계 표 등록(시작 hook·중복 `AlreadyExists`)·미등록 `UnsupportedCapability`·셰이더 없는 프로필, 시험 단계(가짜 `CookArtifactStage`)로 게시·크기·SHA-256 직접 계산·`CorruptEntry`·전체 쿠킹 정리·부분 쿠킹 재사용(cookKey·파일 대조·불일치 시 다시 만들기)·job 실패 시 게시 안 함, 매니페스트 v2 왕복·golden·각 칸 손상·판정 순서 표·상한·v1 `UnsupportedVersion`·병합·`AppendCookedArtifactPath`, `--engine`(프로젝트 없음·잠금 위치·잠금 순서·`Engine` 항목만 — 쿠커 불변식·`--project` 함께 사용 오류·엔진 CookSettings), 로더 `FindArtifact`·`CheckCookedArtifactFile` 불일치 `CorruptPackage`.

#### 10.3.1 A4f 구현 세부(2026-11-25 — 착수 때 정함, 교차 검증 1차 반영)

**나눔**: 넷으로 나누고 차례로 구현한다.
- **A4f1** 필드 플래그 `TextMapMergeByKey`, 새 모듈 `TargetProfiles`(TargetProfile 옮기기·셰이더 칸·구조 검증·`Mac`·호스트 프로필).
- **A4f2** 매니페스트 v2(backend 표·artifact 표·`AppendCookedArtifactPath`·판정 순서·`CheckCookedManifestMerge`의 artifact 표 병합), 로더 `Backends`·`Artifacts`·`FindArtifact`·`CheckCookedArtifactFile`.
- **A4f3** 비에셋 산출물 단계(`CookArtifactStage`·표·요청·sink), 게시·정리·부분 쿠킹 재사용, `cookDevelopmentShaders`, 미등록 `UnsupportedCapability`.
- **A4f4** `asset-cook --engine`, 쿠킹 루트 잠금, 엔진 전용 실행의 `asset.cooked_root` 기본값(11.8절). 완료 조건에 렌더 세션(rocks-6b) 알림을 넣는다(10.3절 "구현 시점").
- 후속: 10.3절 5항의 패키징 규칙(매니페스트가 가리키는 파일만 복사)은 패키징 도구가 생길 때 한다.

**A4f1 결정**:
1. **Map 병합 필드 플래그**(1항의 "리플렉션 필드 지정자" — 기존 지정자가 없어 새로 둔다):
   - `FieldFlags::TextMapMergeByKey`(새 비트 256)와 지정자 `ROCKS_PROPERTY(TextMapMergeByKey)`다. Map 필드에만 쓴다(그 밖은 MetaCompiler `RMC2202` — AssetBundles와 같은 필드 종류 진단).
   - 의미: 그 필드의 JSON import 기본 병합이 키별이다(`options.mapMerge`가 Replace여도). 옵션 MergeByKey는 지금처럼 모든 Map을 덮는다. 플래그를 끄는 옵션은 두지 않는다 — 플래그 단 Map은 어느 ImportJson 경로에서도 키별이다(Config 런타임 변경 import가 일부러 Replace로 부르는 경로 — ConfigRuntimeChanges.cpp 359 — 에서도. 그 경로에 플래그 단 Map을 쓰는 Config 타입이 생기면 그 타입의 뜻으로 키별이 맞다).
   - 플래그는 바깥 필드로 전파하지 않는다(`PropagateFieldFlags`의 inherited에 넣지 않는다 — 자기 Map만, 안쪽 struct의 Map은 자기 플래그를 본다).
   - 닿는 곳(`TextExportTransient` 경로와 같다): TypeSchema.h `FieldFlags`, ObjectRuntime TypeRegistryRegister.cpp `KnownFieldFlags`(빠뜨리면 등록 InvalidArgument), MetaCompiler `Specifiers.cs`(이름 목록·파싱)·`SchemaModel.cs`(비트)·`SchemaValidator.cs`(RMC2202 — Map이 아니면 거부)·`ModuleDescriptor.cs`(descriptor 쓰기·받는 이름·읽기 — 빠뜨리면 다른 모듈이 import할 때 플래그를 잃는다), ReflectionText `TextJsonImport.cpp`(merge = 옵션 MergeByKey 또는 플래그), 문서 ReflectionFoundations 6.4절 플래그 표·ReflectionExtensions 10.4절 import 의미와 Map 행.
   - TargetProfile 읽기는 기본 옵션(Replace)으로 바꾸고 `textureLodGroups`에 플래그를 단다. `shaderFormats`는 플래그 없이 Replace다(1항). 12.14절의 "한계: 옵션이 import 전체에 걸린다"는 이것으로 풀린다.
   - UE 대응: UE는 배열 하나의 키 병합을 ini에서 선언한다 — `BaseDeviceProfiles.ini` 3–4행 `[DeviceProfile]` `*TextureLODGroups=Group`(ConfigCacheIni ArrayOfStructKeys, ConfigCacheIni.cpp 911·966 — 같은 Group 항목은 덮고 새 항목은 더한다). 필드마다 병합 의미를 고르는 같은 구조를 Rocks는 리플렉션 지정자로 둔다.
   - 버린 대안: `shaderFormats`를 struct 배열(`[{backend, formats}]`)로 두기 — 배열은 늘 통째 교체라 플래그가 필요 없고 "목록에 없는 backend 키" 검증도 사라지지만, 렌더 세션과 합의한 칸 모양(SSD10 `graphicsBackends` + `shaderFormats` Map)을 바꾼다. 합의 비용이 더 커서 플래그 안을 택한다.
2. **TargetProfiles 모듈**(1항 모듈 분리):
   - 위치: `Engine/Source/Developer/TargetProfiles`.
   - 의존: Core·ObjectRuntime·ReflectionText·AssetRuntime(매니페스트 상한 상수), 그리고 Texture(A5j의 `textureLodGroups`가 Texture enum을 쓴다)·HostConfig(4번)를 더한다(1항 목록에 더한다).
   - 옮기는 것:
     - `TargetProfile`·`TextureLodGroup`·`TextureLodMinMagFilter`·`TextureLodMipFilter`·`CookBulkCodec`, `FindTextureLodGroup`.
     - `ReadTargetProfile`(새 문맥 `TargetProfileContext{allocator, files, registry, access, types, engineConfigDirectory, projectConfigDirectory(비면 없음)}`).
     - `TargetProfileFailure`·`TargetProfileField`·`TargetProfileFile`(None·EngineProfile·ProjectProfile).
     - JSON 층 읽기 도우미(크기 상한·JSON 문법·대입 진단·층 — 지금 AssetCooker `ApplyJsonFile`)를 공개 도우미 `ApplyConfigJsonFile`로 옮기고 AssetCooker `ReadCookSettings`가 다시 쓴다(두 모듈에 같은 도우미를 두지 않는다).
   - 실패 진단: `CookConfigField`·`CookConfigFile`은 AssetCooker에 남는다(CookSettings 칸 — AlwaysCook·NeverCook·PrimaryAsset·CookSettings 파일 — 이 섞여 있다). `TargetProfileField`·`TargetProfileFile`은 같은 이름의 `CookConfigField`·`CookConfigFile` 값과 1:1로 대응하고, AssetCooker가 `CookConfigFailure`로 변환해 담는다(대응은 static_assert 표로 묶는다). `AssetCookCommandlet` `AppendConfigField`에 새 칸(GraphicsBackends·ShaderFormats) 문구를 더한다.
   - 리플렉션 안정 id는 AssetCooker.rocksids.json의 항목을 TargetProfiles.rocksids.json으로 **옮긴다**(같은 Guid — 새로 만들지 않는다).
   - AssetCooker는 TargetProfiles에 public 의존하고 `HashTargetProfile`·CookSettings를 둔다. A5j1에서 더한 AssetCooker의 직접 Texture 의존은 TargetProfiles public 의존으로 대체한다.
   - 비용: `shader-compile`·SSD16 공급자도 TargetProfiles를 거쳐 Texture 런타임 모듈을 링크한다(엔진 실행 파일은 이미 가진다 — 비용은 셰이더 도구 쪽뿐). `TargetProfileContext`가 Registry·TypeRegistry를 요구하므로 셰이더 도구는 프로필을 읽으려고 ObjectRegistry 하나를 만든다(ReflectionText import 때문 — 피할 수 없다).
   - rocksids 이동은 한 커밋에서 한다(두 파일에 같은 Guid가 동시에 있지 않게). 같은 Guid라 TypeId와 `HashTargetProfile`의 필드 기록이 그대로이고, 프로필 SHA는 새 칸만큼만 바뀐다.
3. **셰이더 칸**: `graphicsBackend`를 지우고(하위호환 없음, 안정 id는 retired로 남긴다) `graphicsBackends`·`shaderFormats`를 둔다. 리플렉션 Map 값은 컨테이너를 담지 못하므로 `shaderFormats`는 `OrderedMap<String, ShaderFormatList{formats: Array<String>}>`다 — JSON은 `{"D3D12": {"formats": ["D3D12_SM6"]}}`(1항의 `Map<String, Array<String>>`과 뜻은 같고 한 층 감싼다, A4f 끝 알림에 넣는다). 1항 구조 검증의 위반은 새 칸 `TargetProfileField::GraphicsBackends`(index = 번호)·`ShaderFormats`(index = backend 번호, 형식 id 위반은 그 backend 번호)다.
4. **호스트 프로필** `ReadHostTargetProfile(context, platform, out, diagnostics, failure)`: platform은 호출자가 넘긴 HostSettings 플랫폼 이름 재정의다(HostSettings.h 57 — 비면 HostConfig `HostPlatformName()`, Windows·Mac·Android). 그 이름으로 `ReadTargetProfile`을 부른다. 그래서 의존에 HostConfig를 더한다(가벼운 Core 모듈). Linux 호스트(BS6 보류)는 엔진 프로필 파일이 없어 엔진 파일 없음 진단(`InvalidSetting`·NotFound)이다.
5. 엔진 파일 `Mac.json`(1항 값 — LOD 그룹 27행은 Windows와 같고, norm16 true), Windows·Android의 셰이더 칸(1항 값).

**A4f1 시험**:
- 플래그: MetaCompiler 수용·Map 아닌 필드 `RMC2202`, 키별 병합·플래그 없는 Map의 Replace·옵션 MergeByKey 전체 적용.
- TargetProfiles 단독(AssetCooker 없이): 프로필 병합·구조 검증 위반마다(어휘·중복·형식 목록 없음·키 남음·형식 id 문법·전역 중복·상한 8·16)·Replace(프로젝트가 한 backend만 적으면 Map 전체 교체)·셰이더 없는 프로필(형식 Map도 빔), 호스트 프로필, 엔진 파일 셋(Windows·Android·Mac).
- AssetCooker: SHA-256이 새 칸을 포함, `CookConfigFailure` 변환(대응 표 전부), `ReadCookSettings`가 공개 도우미로 읽기, 명령 문구(GraphicsBackends·ShaderFormats).
- 호스트 프로필: HostSettings 재정의 이름을 따름, 빈 재정의는 HostPlatformName().
