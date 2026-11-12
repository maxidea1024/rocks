# 에셋 시스템 설계

기준일: 2026-11-12, 개정 7(개정 6 최종 검증 반영: 저장 쪽 값 치환·SCC 키 튜플·Commit 간선 보고·Windows 디렉터리 동기화·PreserveBorder 정의). 원본 파일 임포트부터 쿠킹된 런타임 데이터 로드까지 에셋의 전체 수명주기와 첫 수직 단면(PNG → Texture Asset → 설정 수정·재임포트 → 플랫폼별 쿠킹 → 런타임 로드)을 정한다. [엔진 구현 계획](EngineImplementationPlan.ko.md)의 결정을 따른다: §5.2(Outer·객체 경로), §5.3(instancing), §6.1(참조 종류), §7(직렬화·스키마 변경), §9(에셋 파이프라인·로딩·패키지·재임포트·텍스처 스트리밍), §24.2(PrimaryAsset·bundle). 계획을 바꾸는 곳은 17절에 모으고 계획 문서도 같이 고친다. 에디터 UI 연동은 범위 밖이며(사용자 결정) 서비스 API와 명령행 도구로 같은 기능을 제공한다.

원칙: 기반이 부족하면 우회하지 않고 기반을 보강한다(사용자 지시). 동작하지 않는 기능을 완성처럼 표시하지 않는다. 상태 표기: **구현**(이번 범위에서 동작·시험), **계약**(API·스키마·포맷이 있고 호출하거나 기본값이 아닌 값을 주면 명시 오류 — `Unsupported`·`UnsupportedSetting`·`UnsupportedCapability` — 를 반환), **후속**(이번 범위에 API도 없음).

## 1. 단계와 완료 기준

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| A0-P | PlatformBase: `DirectoryWatcher`, `File::Flush`, `FileSystem::Replace`, `FileLock`, `SetModifiedTime`, Windows 긴 경로(구현됨), `FileSystem::SyncDirectory`(쿠킹 게시용 디렉터리 동기화 — POSIX fsync(macOS F_FULLFSYNC), Windows는 항목 추가 권한(FILE_ADD_FILE, 관리자 불필요)으로 연 디렉터리 FlushFileBuffers, 동기화 실패는 NotDurable, wrapper 기본 Unsupported, 구현됨. `Replace`도 Windows에서 부모 디렉터리를 같은 방식으로 flush한다), `RunWithDeterministicFloatingPoint`(쿠킹 결정성용 부동소수점 환경 확인·실행·복원, 구현됨 — CoreDataFoundations 2.1절) | 기능별 시험(Windows 실행·Android 빌드, macOS 미검증 명시) |
| A0-C | Core: SHA-256(구현됨), Unicode(착수 시 최신 버전 — 2026-11 기준 18.0.0. NFC·NFD·simple case folding·General_Category, UCD 생성기), Default_Ignorable, 결정적 수학(기본 연산만 쓰는 exp2·log2·pow·SinPi·sin, 정확한 8비트 sRGB 표, 부동소수점 환경 guard), CRT 없는 숫자 변환(정확한 파서·최단 표기), JSON 읽기·쓰기(RFC 8259), `RecordList`(`WireType::RecordList`), 데이터 전용 ThirdParty 종류(C1) — 상세와 단계(C1–C6)는 [CoreDataFoundations](CoreDataFoundations.ko.md) | NIST·NormalizationTest·CaseFolding·JSONTestSuite·숫자 변환 대조·결정적 수학 기준값·RecordList 시험 |
| A0-B | 빌드·MetaCompiler(5.6절, 상세 [ReflectionFoundations](ReflectionFoundations.ko.md) B1–B8): 모듈 소유 메타데이터·모듈별 identity 명세·단위 간 import, `ROCKS_ENUM`, `ROCKS_STRUCT`(값 구조체), 배열 필드(`Array<T>`·`ObjectRefArray<T>`), `SoftObjectRef<T>` 필드, `EditorOnly`·`Required`·`Version=N`·`Name="…"` 지정자, `Sha256Digest` 값 종류, `ROCKS_CLASS(WorldOnly)`(그 밖 클래스의 `AssetLease` 멤버 거부), `AssetId` 값 종류, `ROCKS_NATIVE` 비영속 멤버, 필드 codec thunk·문맥, 툴체인 지문·도구 빌드 ID 주입, World 타입 리플렉션 이전 | 기존 시험 + 새 fixture(단계별 완료 기준은 상세 설계 11절) |
| A0-O | ObjectRuntime·ObjectSerialization: Outer·Name, `Discard`(`RequestDestroy` + Outer·Name 표에서 즉시 제거, 메모리는 GC), 패키지 사이 간선 관찰자 `ReferenceObserver`(큐 전달), Registry 정책(패키지 없는 객체·WorldOnly 타입 거부), 다중 root `Commit`, graph v2(호출자가 준 저장 집합·순서 = localId, 집합 밖 참조는 외부 참조 resolver 또는 거부, 참조 값 표기(5.1절), 노드 타입은 export 표 기준, EditorOnly 제외, `prepare` hook(로드)·저장 쪽 값 치환 hook(bulkId 재매핑 등), Commit 때 패키지 사이 간선 다중도 보고, 외부 참조 목록), 한도(객체 4096 = export 상한, 외부 참조 65536, 필드 누계 1,048,576, 배열 원소 누계 4,194,304, 문자열 4 KiB, 그래프 바이트 64 MiB, 작업량 8 × (필드 + 원소)) | 그래프 왕복·외부 참조·마이그레이션·Discard·관찰자 시험 |
| A0-G | GraphicsTypes·Graphics: 차원(2D·Cube·3D·2DArray·CubeArray)·형식(R8·RG8·RGBA8·R16·RG16·RGBA16·R16F·RG16F·RGBA16F·R32F·RG32F·RGBA32F·BGRA8, sRGB 변형)·view swizzle(D3D12 `Shader4ComponentMapping`, OpenGL `TEXTURE_SWIZZLE_RGBA`)·샘플러 W축 주소·필터 5종(Point·Bilinear·Trilinear·AnisotropicPoint·AnisotropicLinear — 기존 `Nearest`·`Anisotropic`의 이름 변경 포함), 쿠킹 데이터 샘플러 D3D12·OpenGL | self-test 확장 |
| A1 | AssetRuntime: 패키지 포맷 v1, `Package`·`Asset`, 에셋 객체 Registry, 쿠킹 매니페스트(`CookedManifest.rman`), 로더(worker IO·압축 해제, Game 스레드 객체 생성, SCC), `AssetManager`(scope·PrimaryAsset·bundle), `CancelToken`·`TaskLane`·`LoadPriority`, Core ErrorCode 추가(`UnsupportedCapability`·`AlreadyRegistered`·`ScopeClosed`), EngineAssets 모듈 | 포맷·SCC·수명·scope 시험 |
| A2 | AssetDatabase: `ProjectLayout`, 경로 정책, 인덱스(검색·의존·별칭·진단·캐시·감시), 저장·dirty, 이동·복제·삭제, 임포트 프레임워크 | 인덱스·이동·동시 쓰기 시험 |
| A3 | BuildCache | 적중·무효화·손상·동시 기록·GC 시험 |
| A4 | AssetCooker | 원본 없이 쿠킹·재현성·부분 쿠킹 시험 |
| A5 | 텍스처: `Texture` 계열, `TextureSource`, ImageCore(BMP·TGA·PNG·PSD), TextureTools(임포터·빌드·쿠킹 처리기) | 14절 텍스처 시나리오 |
| A6 | 명령행·문서·검증 보고 | 14절 결과 기록 |

## 2. 사전 조사(2026-11-12)

| 영역 | 있는 것 | 부족한 것 → 보강 |
|---|---|---|
| 객체 | `ObjectRegistry`(ContextId 문맥)·`RootRef`·`ObjectRef`(같은 Registry 안에서만)·`WeakRef`·`Borrow`(다른 문맥 Registry는 WrongContext), `ConstructionContext`(스테이징한 객체를 한 번에 공개, RootRef는 첫 객체에만), `TypeId = Guid` | Outer·Name, 다중 root → A0-O |
| 리플렉션 | MetaCompiler: 스칼라·Guid·String·`ObjectRef`, 모든 비정적 필드 리플렉션 필수, 상속 base·참조 대상은 같은 생성 단위, 스키마 버전 1 고정, 타깃 소유 메타데이터·명세 | → A0-B([ReflectionFoundations](ReflectionFoundations.ko.md)) |
| 직렬화 | Archive·Record(TLV, 반복 필드 없음), `SaveObjectGraph/LoadObjectGraph`(단일 root, 내부 Commit, 마이그레이션 콜백, maxObjects 1024) | → A0-O, `RecordList` → A0-C |
| 비동기 | `WorkerScheduler`(DAG·취소), `AsyncReadFile/AsyncWriteFile`(IOCP, `Flush`) | 로더가 사용(11절) |
| 파일 | `FileSystem`, `CreateNew`, no-replace Move, `Path`, Win64는 `FILE_SHARE_DELETE` 열기 | → A0-P, VFS(APK) → 후속 |
| 해시·텍스트 | CRC32, `NameKey`, UTF-8 검증·`CodePointRange` | SHA-256·Unicode·JSON → A0-C |
| 프로세스 | 없음 | 임포트 worker process는 17절 |
| 그래픽 | Format RGBA8 2종, 2D·mip·샘플러 | → A0-G |
| 도구 | `CommandletRegistry`, `Programs/Commandlet` | `asset-*` → A6 |
| 기존 에셋 코드(미커밋) | AssetRuntime(단일 `.rasset`), AssetTools(BMP·TGA·PSD·PNG 디코더 — PNG는 zlib 대기, 설정·mip 빌드) | 대체. 디코더 → ImageCore, mip → TextureTools |
| 외부 라이브러리 | Core/ThirdParty(zlib·LZ4·Zstd), Core `Compression` 모듈 | PNG zlib·bulk Zstd에 사용. 이미지 디코더는 자체 구현 |

## 3. 개념과 모듈

| 개념 | 모듈·타입 | 상태 |
|---|---|---|
| Source File | 원본 루트(4.5절) | 구현 |
| Asset | AssetRuntime `Asset : Object`(Outer = `Package`), A5 `Texture` 계열 | 구현 |
| Asset Package | AssetRuntime `Package : Object`, `PackageWriter`·`PackageReader` | 구현 |
| Importer | AssetDatabase `Importer`, TextureTools `TextureImporter` | 구현(텍스처) |
| Import Metadata | 에디터 패키지 `ImportRecord` 섹션 | 구현 |
| Asset Registry(인덱스) | AssetDatabase `AssetIndex` | 구현 |
| 빌드 캐시 | BuildCache | 구현(로컬), 공유·원격 backend 계약 |
| Cooker | AssetCooker | 구현(비압축 형식), 블록 압축 계약 |
| Runtime Asset Loader | AssetRuntime `AssetLoader` | 구현(CPU ready), `Render` ready 계약 |
| Asset Manager | AssetRuntime `AssetManager` | 구현(scope·PrimaryAsset·bundle·즉시 해제), 예산 LRU 해제 후속 |

이름 구분: 에셋 객체가 사는 `ObjectRegistry`는 **에셋 객체 Registry**, 에디터 패키지 인덱스는 **AssetIndex**(UE AssetRegistry 대응), 쿠킹 결과의 런타임 목록은 **쿠킹 매니페스트**(`CookedManifest.rman`, 10절)라 부른다.

의존 방향(→는 의존):

```
Programs/Commandlet → TextureTools → { AssetCooker, AssetDatabase, BuildCache, ImageCore, Texture }
AssetCooker → { AssetDatabase, BuildCache }        AssetDatabase → AssetRuntime
Texture(Runtime) → { AssetRuntime, GraphicsTypes }       ImageCore(Developer) → { Core, Compression }
AssetRuntime → { ObjectSerialization, ObjectRuntime, Serialization, Task, Compression }
EngineAssets(Runtime, 신규) → { Engine, AssetRuntime }   — AssetManager를 EngineSubsystem으로 붙이는 연결 모듈
```

AssetRuntime은 Engine을 모른다. 도구는 Engine을 링크하지 않고 `AssetManager`를 직접 소유한다. 게임은 `EngineAssets`가 AssetManager를 `EngineSubsystem`으로 등록하고 프레임 단계(로드 결과 전달·에셋 객체 Registry GC 한 조각)를 연결한다. World는 AssetRuntime에 의존하지 않는다(6.3절).

## 4. 식별과 경로

### 4.1 Asset ID와 객체 이름

- `AssetId`(128비트 Guid, 무작위 v4)는 대표 에셋과 그 패키지의 영속 ID다(패키지당 대표 에셋 하나). 생성 때 발급, 저장·이동·이름 변경·재임포트 후 유지, 복제 시 새로 발급. `AssetId`·`SoftObjectRef<T>` 값 타입은 Core(ObjectRuntime)에 둔다(리플렉션이 필드 타입으로 인식).
- 객체 이름: 대표 에셋은 빈 이름, 하위 객체는 같은 Outer 안에서 유일한 이름(계획 §5.2)이며 규칙은 경로 구성 요소(4.2절)와 같다(`.`·`/` 불가). 패키지 안 객체 경로는 Outer 체인 이름을 `.`로 이은 것이다(예: 대표 에셋 아래 `Body` 아래 `Socket` = `Body.Socket`). 하위 객체는 `<AssetId, 객체 경로>`로 식별한다. 하위 객체 이름 변경 안정성은 SubobjectGuid(계획 §5.3, 후속).
- 중복 ID는 덮어쓰지 않고 `DuplicateAssetId`로 보고하며 그 ID 조회는 오류를 반환한다. 해결은 `asset-fix-duplicate-id <path>`(지정한 쪽에 새 ID)로만 한다.

### 4.2 논리 경로

- 형식 `/<Mount>/<Dir>/.../<Name>`. `/Game` = 프로젝트 `Content/`, `/Engine` = 엔진 콘텐츠(후속). 하위 객체 경로는 `<PackagePath>.<객체 경로>`(예: `/Game/Hero.Body.Socket`).
- **진실의 원천은 파일 위치 하나다.** 에디터 패키지는 경로를 저장하지 않고 대표 에셋 이름은 파일 이름에서 유도한다. 쿠킹 매니페스트(10절)는 경로 힌트를 담으며 런타임의 경로 조회(`FindByPath`)는 이 힌트로 AssetId를 찾는 편의 기능이다(영속 참조는 항상 ID).
- 정규화: 구분자 `/`만(입력의 `\`는 거부), 앞 `/` 필수, 끝 `/`·빈 구성 요소·`.`·`..` 금지. 문자열은 NFC로 정규화하며 비교 키는 `TryMakeCaselessKey` = NFC(simple case folding(NFD(x)))다(Unicode §3.13 정준 caseless 일치의 simple folding 판, 버전은 A0-C). 파일 열거 이름도 NFC로 바꿔 비교한다(macOS NFD 대응). NTFS는 자체 대문자 표($UpCase)를 쓰므로 키가 다른데 NTFS가 같게 보는 드문 이름 쌍은 저장 시 `CaseCollision` 진단으로 잡는다.
- 구성 요소: 1–64 byte UTF-8(NFC). 허용: General_Category L*·N*·M*(단, 첫 문자는 L*·N*), `_`, `-`. 거부: Default_Ignorable_Code_Point(보이지 않는 문자 — L*·M*이어도), 그 밖의 문자(공백·제어·구두점·기호 포함 — 따라서 `.`·`,`·`;`·`\`도 없다), Windows 예약 이름(대소문자 무시 `CON PRN AUX NUL COM0-9 LPT0-9`, `COM¹²³ LPT¹²³`).
- 길이: 논리 경로 ≤ 256 byte. `ProjectLayout`이 OS 경로(임시 접미사 포함) 길이를 검사한다. Windows 긴 경로는 PlatformBase가 지원한다(A0-P).
- 정렬: 이름·경로의 결정적 정렬은 NFC UTF-8 byte 순, Guid는 (high, low) 부호 없는 수치 순이다(5·10절의 모든 정렬).
- 파일: 에디터 `<Project>/Content/<path>.rasset`, 쿠킹 `<쿠킹 루트>/Packages/<a>/<A>-<h>.rpkg`. A = AssetId의 36자 소문자 표기(Core Guid `{}` 서식, RFC 9562), a = A의 앞 2자, h = 패키지 SHA-256의 앞 16자(소문자 hex). 내용 주소 이름이라 같은 이름은 같은 내용이다(10절 게시).

### 4.3 경로 별칭

- 사람이 쓴 텍스트 경로는 별칭 표 "옛 경로 → AssetId"로 풀린다(계획 §9.1). 이동·이름 변경 때 추가하고 풀릴 때마다 `AliasResolved` 경고. 위치: `Config/AssetAliases.json`(버전 관리 대상, 팀 공유, 원자적 기록, 항목 정렬로 병합 충돌 최소화). 패키지 재검색으로는 복구되지 않는 사용자 데이터이므로 버전 관리가 백업이다(계획 문서의 "재검색으로 복원되는 캐시" 표현은 17절에서 고친다).
- 만료: 항목은 생성 UTC와 만료 일수(기본 90, 설정)를 가진다. `asset-alias-prune`이 만료 항목을 지우며, 지우기 전 텍스트 참조 검색(`Config/`·스크립트)에서 쓰인 곳을 보고한다. `asset-alias-list`·`asset-alias-remove` 제공.

### 4.4 작업

| 작업 | 방법 | 상태 |
|---|---|---|
| 이동·이름 변경(디렉터리 포함) | 잠금 → 대상 없음 확인 → `Move(no-replace)`(디렉터리면 디렉터리 한 번, 대소문자만 바꾸는 이름 변경 허용) → 별칭 추가(디렉터리면 안의 모든 패키지) → 인덱스 갱신. ID·참조 무변경 | 구현 |
| 복제 | 새 경로에 새 ID로 기록(하위 객체·내부 참조 재매핑, 계획 §5.3) | 구현 |
| 삭제 | 참조자가 있으면 거부, `--force`면 삭제 후 참조자에 `MissingDependency` | 구현 |
| 중복 ID 해결 | `asset-fix-duplicate-id` | 구현 |

읽기 전용으로 연 패키지(5.3절)는 저장·쿠킹·복제·ID 수정을 모두 `NewerVersion`으로 거부한다(모르는 내용을 잃는다).

### 4.5 원본 경로

원본은 이름 있는 원본 루트 기준 상대 경로(`{Project}/SourceArt/Checker.png`)로 기록한다. 루트 목록은 `Config/AssetSources.json`(이름 → 경로, 기본 `Project`). 프로젝트 밖 원본은 절대 경로로 기록하고 `AbsoluteSourcePath` 경고. 재임포트는 기록한 경로를 현재 루트로 풀고, 없으면 `MissingSource`로 실패·기존 보존. 재지정은 `--source=`(원본 하나) 또는 `--source.<역할>=`(원본 여러 개, 역할은 ImportRecord `sources[].role`)로 한다(13절).

### 4.6 진단

| 진단 | 뜻 |
|---|---|
| `DuplicateAssetId`, `DuplicatePath`, `CaseCollision`, `InvalidPath`, `MissingFile`, `UnregisteredFile` | 식별·경로 |
| `TypeMismatch`, `MissingDependency`(Hard·Soft), `UnsupportedCycle`, `UnpackagedReference`(패키지에 속하지 않은 객체를 저장 그래프가 참조) | 참조 |
| `CorruptPackage`, `UnsupportedVersion`, `NewerVersion`, `ExternalModification`, `NotDurable`(경고 — 교체는 됐고 디렉터리 동기화만 실패, 5.5절) | 패키지·저장 |
| `MissingSource`, `SourceChanged`, `InvalidSource`(원본 해석 실패), `UnsupportedSourceFormat`(지원하지 않는 원본 변형, 예: PSD CMYK), `LossySource`, `NonSrgbSource`, `IgnoredSourceChannels`(경고), `AbsoluteSourcePath` | 원본 |
| `ImporterOutdated`, `InvalidImportOption`, `OrphanedOutput`, `UnknownProperty`(모르는 속성 이름), `UnsupportedSetting`(계약 설정의 기본값이 아닌 값), `InvalidSetting`(모순된 조합·숨김 값), `SrgbIgnored`(경고 — 선형 설정·float 출력의 SRGB), `PreserveBorderWithWrap`(경고) | 임포트·설정 |
| `CorruptEntry`(빌드 캐시 항목·게시 패키지 파일 손상), `IncompatibleCook`(부분 쿠킹 병합 불가·런타임 프로파일 불일치), `NeverCookReferenced` | 빌드 캐시·쿠킹 |
| `AliasResolved`, `RootLost`(감시 루트 소실) | 그 밖 |

## 5. 패키지 포맷 v1

### 5.1 헤더·섹션 표·물리 배치

모든 정수 little-endian, Guid는 high·low u64(Archive 규칙). 첫 64 byte 가운데 magic·formatVersion·headerBytes·flags(offset 0–11)와 offset 60의 headerCrc 위치는 모든 포맷 버전에서 고정이다(버전 판정 전에 headerCrc와 flags를 검사하므로).

```
offset size 필드
0      4    magic "RPKG"
4      2    formatVersion (1)
6      2    headerBytes (64)
8      4    flags: Editor(1) | Cooked(2) 중 정확히 하나
12     4    sectionCount (1–32)
16     8    sectionTableOffset (8 정렬)
24     8    fileBytes = sectionTableOffset + 32 × sectionCount + 4  (Stat 크기와 같아야 한다)
32     16   assetId
48     4    bulkAlignment (2의 거듭제곱, 8–65536; 에디터 64, 쿠킹 = 프로파일 값, 기본 4096)
52     8    reserved (0)
60     4    headerCrc = CRC32(byte 0–59)
섹션 표(파일 끝): sectionCount × { u32 kind, u32 flags, u64 offset, u64 bytes, u32 crc32, u32 schemaVersion } + u32 tableCrc
```

- 섹션 flags: `Required`(1) — 이 섹션을 모르는 reader는 패키지를 해석할 수 없다. `PerEntryChecksum`(2) — 섹션 표의 crc32 칸은 0이고 entry별 CRC로 검증(Bulk). 정의되지 않은 비트는 `CorruptPackage`. 알려진 kind의 flags는 5.2절 표로 고정이다.
- 물리 순서는 kind 오름차순으로 고정한다: Summary(1) → Exports(2) → ObjectImports(3) → Dependencies(4) → Objects(5) → ImportRecord(6) → Tags(7) → Bulk(8) → BulkIndex(9) → 섹션 표. BulkIndex를 Bulk 뒤에 두어 스트리밍 기록 중 확정되는 압축 크기·CRC를 마지막에 쓴다. 빈 섹션 규칙은 5.2절 표(같은 내용 → 같은 바이트).
- 검증 순서와 진단:
  1. 크기 ≥ 64, magic, headerCrc, flags(Editor·Cooked 중 정확히 하나, 정의되지 않은 비트 없음 — 버전 판정이 이 값에 달렸다). 실패는 `CorruptPackage`(버전 필드를 믿을 수 없다).
  2. `formatVersion`: 5.3절 규칙(`NewerVersion`·`UnsupportedVersion`).
  3. 나머지 헤더·섹션 표: `headerBytes` = 64, bulkAlignment 범위, 필수 섹션(5.2절 "항상 있음")의 존재와 생략 규칙이 있는 섹션의 비어 있지 않음, `fileBytes` 식과 Stat 크기 일치, tableCrc, kind 중복 금지, 물리 순서 = kind 순서, 각 섹션 `offset ≥ 64`·`offset ≤ sectionTableOffset`·`bytes ≤ sectionTableOffset − offset`, offset은 8(Bulk는 bulkAlignment) 정렬, 겹침 금지, 패딩은 정렬 공간뿐이며 0. 위반은 `CorruptPackage`.
  4. 섹션별: 모르는 kind는 Required면 에디터 `NewerVersion`·쿠킹 `UnsupportedVersion`이다. Required가 아니면 에디터는 그 섹션을 건너뛰고 읽기 전용으로 열고 쿠킹은 `UnsupportedVersion`이다. 아는 kind의 flags가 표와 다르면 `CorruptPackage`, `schemaVersion`은 5.3절 규칙. 섹션 crc32(PerEntryChecksum 제외)·내용 검증 위반은 `CorruptPackage`.
- 상한: 파일 64 GiB, 비-Bulk 섹션 64 MiB, export 4096, object import 65536, 문자열 4 KiB. 위반은 `CorruptPackage`.
- export와 참조 표기:
  - export 집합은 Outer 체인이 이 패키지에 닿는 모든 객체다(Outer 표에서 정한다 — 참조 순회로 정하지 않는다). export index는 1부터(0 = 패키지 자신). 저장 순서는 대표 에셋, 이후 Outer 깊이 우선이며 같은 Outer 안은 이름 순(4.2절 정렬)이다. writer가 이 순서를 정해 graph v2에 넘기고 graph v2의 localId k = export k다(A0-O).
  - export 표의 유일성: export 1이 대표 에셋이고 그것만 Primary 표시·빈 이름·outerIndex 0을 가진다. 나머지 export는 outerIndex ≥ 1(자기 index보다 작다)이고 같은 Outer 안에서 이름이 유일하다. 위반은 `CorruptPackage`.
  - object import 표의 유일성: outerImportIndex가 0이 아니면 자기 index보다 작고 같은 assetId의 항목을 가리켜야 하며, (assetId, 객체 경로)는 중복되지 않는다. 위반은 `CorruptPackage`.
  - 패키지 밖 객체로의 참조는 object import다. 다른 패키지에 속하면 import, 어떤 패키지에도 속하지 않으면 저장을 `UnpackagedReference`로 거부한다.
  - Objects 섹션의 참조 값(u32): 0 = null, 1–0x7FFFFFFF = export index, 0x80000000 | i = object import index i(1부터). export 수·import 수를 넘는 값과 0x80000000(i = 0)은 `CorruptPackage`. Exports의 outerIndex도 export index다(0 = 패키지).
  - 객체 타입의 원천은 Exports.typeId 하나다. graph v2 노드는 타입을 따로 쓰지 않고 export를 가리키며, 모든 export의 typeId는 Summary의 타입 버전 표에 있어야 한다(없으면 `CorruptPackage`).
- CRC32는 우발적 손상 검출용이며, 콘텐츠 동일성은 SHA-256, 변조 방지는 범위 밖이다.

### 5.2 섹션 내용

| kind | 내용 | flags | 빈 경우 | Editor | Cooked |
|---|---|---|---|---|---|
| Summary(1) | Record: 타입 스키마 버전 표(TypeId → 버전, TypeId 정렬), 커스텀 버전(Guid → u32), 도구 빌드 ID(10절), Cooked면 프로파일 id·프로파일 출력 설정 SHA-256·쿠커 버전 | Required | 항상 있음 | ○ | ○ |
| Exports(2) | `{ name(대표는 빈 문자열), typeId, outerIndex, flags(Primary·EditorOnly) }` | Required | 항상 있음 | ○ | ○(EditorOnly 제외) |
| ObjectImports(3) | `{ assetId, outerImportIndex(0 = 그 패키지의 대표 에셋을 Outer로 하는 경로의 시작), name(빈 이름 = 대표 에셋), typeId }`. 깊은 하위 객체는 outer import를 거쳐 객체 경로를 이룬다(UE import 표와 같은 방식). (assetId, 객체 경로) 순 정렬, 중복 금지 | Required | 생략 | ○ | ○ |
| Dependencies(4) | `{ assetId, kind(Hard·Soft·EditorOnly·Build·ImportOwner), expectedTypeId, required }` (assetId, kind) 정렬·중복 제거. Hard·Soft·EditorOnly는 저장 결과의 외부 참조 목록(A0-O)에서, Build는 저장 전에 호출한 `DependencyDeclarer`(16.2절)가 준 목록에서, ImportOwner는 ImportRecord `ownerAssetId`(자기 자신이 아니면)에서 만든다. 로드 때 Hard·Soft·EditorOnly는 Objects 디코딩 결과와, ImportOwner는 ImportRecord와 대조하며(Build는 대조하지 않음) 불일치는 `CorruptPackage` | Required | 생략 | ○ | ○(EditorOnly·Build·ImportOwner 제외) |
| Objects(5) | ObjectSerialization graph v2 바이트 | Required | 항상 있음 | ○ | ○ |
| ImportRecord(6) | Import Metadata(5.4절) | — | 생략(임포트하지 않은 에셋) | ○ | × |
| Tags(7) | 인덱스 검색 태그 Record(타입이 정한 키→값, 키 정렬) | — | 생략 | ○ | × |
| Bulk(8) | entry 본문(bulkAlignment 정렬, PerEntryChecksum) | Required·PerEntryChecksum | 생략(entry 없음) | ○ | ○ |
| BulkIndex(9) | `{ bulkId(1부터 연속), flags(EditorOnly), offset(Bulk 섹션 시작 기준), storedBytes, rawBytes, codec(None=0·Zstd=1), crc32(저장 바이트), sha256(raw) }`. 검증: bulkId 순이며 offset도 증가, entry는 Bulk 섹션 안에서 겹치지 않고 entry 사이에는 bulkAlignment 정렬 패딩(0)만 있다, codec None이면 storedBytes = rawBytes, 쿠킹 패키지의 EditorOnly 표시는 0, Objects의 모든 `BulkDataRef`가 가리키는 bulkId가 있고 rawBytes·SHA-256이 같다 | Required | Bulk와 함께 생략 | ○ | ○ |

### 5.3 버전 축

| 축 | 위치 | 에디터 패키지 | 쿠킹 패키지 |
|---|---|---|---|
| 컨테이너 | `formatVersion` | 현재보다 낮으면 읽으며 마이그레이션, 높으면 `NewerVersion`(해석할 수 없는 컨테이너라 열지 않음) | 정확히 일치, 아니면 `UnsupportedVersion`(다시 쿠킹) |
| 섹션 | `schemaVersion` | 낮으면 마이그레이션, 높으면 그 섹션이 Required면 `NewerVersion`, 아니면 건너뛰고 읽기 전용 | 정확히 일치 |
| 타입 데이터 | Summary 타입·커스텀 버전, 마이그레이션 콜백 | 낮으면 마이그레이션, 높거나 모르는 필드가 있으면 읽기 전용으로 열고 저장은 `NewerVersion` 거부. 이 모드에서는 Dependencies 대조를 부분집합 검사로 바꾼다(모르는 필드 안의 참조는 디코딩 결과에 없으므로 "디코딩 결과 ⊆ Dependencies"만 본다) | 정확히 일치 |

버전을 올릴 때 옛 fixture 3개 이상을 읽고 다시 저장하는 시험을 둔다(계획 §7).

### 5.4 Record 목록과 Import Metadata

목록은 Record의 `WireType::RecordList` 필드다(payload = `u32 count + count × (u64 length + 중첩 Record)`, 빈 목록도 count 0으로 기록 — A0-C, Serialization 모듈).

```
ImportRecord (스키마 Guid, 버전 1)
  importerId: Guid, importerName: string, importerVersion: u32
  sources: RecordList[ { role, root, relativePath, sha256(32B), bytes } ]
  options: Record(임포터별 스키마·버전 — 디코딩·원본 변환 옵션만)
  outputs: RecordList[ { outputKey, assetId, typeId } ]
  ownerAssetId: Guid   — 다중 출력의 Import 기록 소유 패키지. 자기 자신이면 전체 기록, 아니면 sources·options·outputs는 비고 소유자를 가리킨다(Dependencies에 ImportOwner로 남아 소유자 삭제를 막는다)
```

원본 역할(`role`): 2D `source`, Cube 면 `px`·`nx`·`py`·`ny`·`pz`·`nz`, 2DArray·Volume slice `0`·`1`…, CubeArray `<i>.<면>`. 옵션 경계: 디코딩·원본 변환 옵션 = ImportRecord.options(바뀌면 재임포트), 빌드·쿠킹 설정 = 에셋 저장 속성(바뀌면 재빌드, 빌드 캐시 키 변경).

### 5.5 저장

1. 잠금(8절). 대상이 있으면 로드 때 기록한 지문(헤더 + 섹션 표 SHA-256)과 비교, 다르면 `ExternalModification`.
2. 같은 디렉터리에 `<name>.rasset.<guid 36자>.tmp`를 `CreateNew`.
3. Objects를 먼저 메모리에 직렬화한다(그 결과의 외부 참조 목록이 있어야 ObjectImports·Dependencies를 만든다). 그다음 섹션을 물리 순서대로 스트리밍 기록하며 CRC를 증분 계산한다. bulk는 블록 단위로 압축·CRC·SHA-256을 계산하며, 원본은 객체가 메모리에 가진 raw 데이터이거나 아직 읽지 않은 entry면 이전 패키지 파일의 열린 핸들(6.1절)이다. 이전 파일에서 옮기는 entry는 저장 crc32와 raw SHA-256을 검증하며 다르면 저장을 `CorruptPackage`로 실패시킨다(손상이 새 CRC로 세탁되지 않게). bulkId는 저장마다 객체·필드 순서로 1부터 다시 매기고 writer가 (옛 id → 새 id) 재매핑 표를 만든다. BulkIndex·섹션 표를 쓰고 offset 0으로 돌아가 헤더를 쓴다. 메모리 피크(객체와 이미 상주한 raw 데이터 제외)는 Objects 바이트 + 다른 비-Bulk 섹션 하나 + bulk 블록 하나다(비-Bulk 섹션은 각각 64 MiB 상한).
4. `File::Flush` → Close.
5. 고정 크기 버퍼로 다시 읽어 헤더·표·섹션·entry CRC를 검증한다(writer 버그·짧은 쓰기 검출. 페이지 캐시를 읽으므로 매체 손상 검출이 목적은 아니다).
6. `FileSystem::Replace(temp, target)`(구현, `Platform/File.h` 계약). Windows: `SetFileInformationByHandle(FileRenameInfoEx, REPLACE_IF_EXISTS | POSIX_SEMANTICS)`(다른 쪽이 `FILE_SHARE_DELETE`로 연 대상도 교체 — 이전 파일의 열린 핸들도 계속 유효하다). 이 정보 클래스가 없는 볼륨(FAT·일부 네트워크 FS)은 `MoveFileExW(REPLACE_EXISTING)`로 대신하며, 그 볼륨에서 열린 대상은 `Busy`·`AccessDenied`로 끝나고 대상은 바뀌지 않는다. `ReplaceFileW`는 쓰지 않는다(이전 대상의 속성·스트림을 합치고, `ERROR_UNABLE_TO_MOVE_REPLACEMENT`에서 대상을 지운 채 남긴다). Windows의 이름 변경 매체 기록은 NTFS 메타데이터 로그에 맡긴다(반환 직후 전원 차단 시 이전 상태일 수 있다). POSIX: 부모 디렉터리를 먼저 열고 `rename` 후 그 디렉터리를 `fsync`(macOS `F_FULLFSYNC`). 오류: 원본이 일반 파일이 아니거나 대상이 디렉터리·link면 `WrongType`, 대상이 읽기 전용이거나 권한이 없으면 `AccessDenied`, 공유 위반이면 `Busy`.
   교체 직전에 임시 파일의 읽기 핸들을 열고 패키지가 쥔 이전 파일 핸들을 닫는다(POSIX 의미 rename이 없는 볼륨의 `MoveFileExW`는 자기 핸들이 열려 있으면 실패하므로 모든 경로에서 같은 순서로 한다). 교체가 실패하면 이전 경로를 다시 열고 지문이 로드 때와 같은지 확인한다(다르면 이후 저장은 `ExternalModification`).
7. 실패하면 임시 파일을 지우고 보고(기존 보존). `NotDurable`(POSIX 디렉터리 동기화만 실패)은 교체가 이미 보이는 상태이므로 임시 파일 정리 없이 저장 성공으로 처리하고 `NotDurable` 경고를 남긴다. 성공(경고 포함) 후: 저장 세대와 무관하게 메모리의 모든 `BulkDataRef`를 재매핑 표(옛 id·SHA-256 → 새 id)로 바꾸고 패키지의 bulk 핸들·BulkIndex를 새 파일(6번에서 연 핸들)로 다시 잇는다(아직 읽지 않은 entry도 새 파일에 있다). 직렬화를 시작한 세대 = 현재 세대일 때만 dirty를 지운다(저장 중 편집이 있었으면 dirty 유지 — 편집으로 새로 생긴 raw 데이터는 메모리에 있으므로 다음 저장이 쓴다). 마지막으로 인덱스를 갱신. 재매핑은 저장 직렬화의 값 치환 hook(A0-O)이 직렬화 바이트에 새 id를 쓰고, 같은 옛 id를 가리키는 여러 참조는 같은 새 id로 바꾼다.
8. 잠금 아래에서 발견한 `<name>.rasset.<guid 36자>.tmp` 형태의 파일만 고아로 지운다. 재검색·감시는 `.tmp`를 무시. 교체는 이름만 바꾸므로 새 파일은 이전 대상의 ACL·속성·대체 스트림을 승계하지 않는다(두 플랫폼, 두 경로 모두).

### 5.6 리플렉션·빌드 보강(A0-B 상세)

상세 설계와 단계(B1–B8)는 [리플렉션·빌드 보강 설계](ReflectionFoundations.ko.md)다. 요약:

- 메타데이터 소유를 타깃에서 모듈로(`NativeModuleInput.Metadata`·`ModuleDirectory`, 모듈별 명세 `<ModuleDir>/<Module>.rocksids.json`, 원본 철자를 따르는 생성 include를 모듈의 public·private include로 전파). 모듈 의존 순서로 모듈마다 한 번 실행하고, descriptor(`<Module>.rocksmeta.json`)를 `--import`해 다른 모듈의 타입을 쓴다. 명세 사이 ID 중복 검사, ID 기준으로 합친 이력·병합 검사(`ids move`), 다중 명세 원자 게시. `TargetRules.Metadata`·`IdentityManifest` 제거, fixture는 `ReflectionTestFixtures` 모듈.
- 값 종류: `ROCKS_ENUM`(열거자 `Name`·`DisplayName`·`Hidden`, 플래그 enum, 미선언 값 거부·로드 정책), `ROCKS_STRUCT`(중첩 Record, 참조 struct는 배열 원소 불가, struct 버전·마이그레이션, Core 수학 타입은 `Binds`), `Array<T>`(단일 값 원소는 `ValueArray` wire, struct 원소는 `RecordList`)·`ObjectRefArray<T>`, `AssetId`, `SoftObjectRef<T>`(AssetId + 객체 경로), `Sha256Digest`.
- 지정자: `Version=N`·`WorldOnly`(상속)·`EditorOnly`·`Required`(저장·쿠킹에서 거부)·`Name="…"`(도구 이름, 텍스처 설정은 UE C++ 이름, 12.4절)·`MaxElements`. `ROCKS_NATIVE`(비영속 멤버, 강한 참조를 재귀적으로 금지, `AssetLease`처럼 WorldOnly 전용 형질을 선언한 타입은 WorldOnly 클래스에만 — 에셋 객체 Registry는 WorldOnly 타입 생성을 거부한다, 11.4절). 영속 데이터는 모두 리플렉션 필드다 — `BulkDataRef`·`TextureSource`·`TexturePlatformData`도 `ROCKS_STRUCT`이므로 네이티브 직렬화 hook을 두지 않는다. `completeFieldLayout`은 "모든 영속 필드와 강한 간선을 기술"이다.
- 필드 codec: 필드별 생성 encode·두 단계 decode thunk와 문맥(참조 wire 매개변수, Soft 참조 수집기, 유효 플래그, struct 관찰·치환 hook, 진단, decode 정책) — graph v2(A0-O)와 패키지(A1)가 같은 thunk를 쓴다.
- 부동소수점: 빌드 전체가 이미 `-ffp-contract=off`이고 fast-math를 쓰지 않으므로 모듈별 모델은 두지 않는다(clang-cl의 `/fp:precise`는 contract=on을 뜻하고 `/WX`에서 옵션 충돌 경고가 오류가 되므로 쓰지 않는다). 결정적 수학 모듈의 fmadd·`long double`·외부 심볼(`llvm-nm`) 검사는 C3에서 모든 구성에 구현되었다([CoreDataFoundations](CoreDataFoundations.ko.md) 2절). `-fno-math-errno`는 clang-cl이 그 형태를 받지 않아 쓰지 않는다.
- 빌드가 주입하는 상수: 툴체인 지문(빌드 캐시 키, 구성·호스트 경로 제외)과 도구 빌드 ID(Summary·AssetIndex 캐시 키 진단용, 입력 내용 해시 기반)를 Rocks.Build가 실행 파일마다 생성 번역 단위로 넣는다.
- 엔진 World 타입(Level·Actor·Component)은 `ROCKS_CLASS(WorldOnly)`로 옮긴다(B8).
## 6. 에셋 타입·객체 통합·참조

### 6.1 객체 통합

- 로드된 패키지·에셋은 `AssetManager`가 소유하는 **에셋 객체 Registry**(고유 ContextId, 엔진 TypeRegistry 공유)에 있다. World Registry와 분리한다(여러 World·PIE 공유, 에셋끼리 ObjectRef 가능). 접근은 `AssetManager::With`(에셋 문맥 ObjectAccessScope를 중첩해 연다).
- `Package : Object`: `ROCKS_PROPERTY(Transient, ReadOnly) AssetId id`, 플래그, export 목록(`ROCKS_PROPERTY(Transient) ObjectRefArray<Object>` — 강한 간선), 하드 의존 패키지 목록(`ROCKS_PROPERTY(Transient) ObjectRefArray<Package>` — 하드 의존 보존), bulk 접근자(`ROCKS_NATIVE`: 에디터 패키지 파일의 열린 핸들 + BulkIndex. 지연 읽기를 하며 저장 성공 후 새 파일로 다시 잇는다, 5.5절). `Asset : Object`(`ROCKS_CLASS(Abstract)`)의 Outer는 Package. inner → outer 강한 간선(계획 §5.2).
- 저장 속성은 `ROCKS_PROPERTY`. 대용량 데이터는 `ROCKS_STRUCT BulkDataRef`(bulkId 1부터·rawBytes·`Sha256Digest`) 필드이고 내용은 Bulk 섹션. 쿠킹 플랫폼 데이터의 mip별 entry도 bulkId로 가리킨다(파일 offset은 BulkIndex만 안다).
- 대표 에셋의 AssetId는 `ROCKS_PROPERTY(Transient, ReadOnly) AssetId`이며 로드 때 `prepare` hook이 Commit 전에 설정한다.
- 에셋 객체 Registry의 모든 객체는 Package이거나 Outer 체인이 Package에 닿는다(Registry 정책 — 패키지 없는 객체 생성은 거부한다). 그래서 모든 하드 간선에는 소유 패키지가 있다.
- 패키지 사이 하드 간선의 변경: 에셋 객체 Registry는 서로 다른 패키지에 속한 객체 사이의 `ObjectRef` 설정·해제를 관찰자(A0-O `ReferenceObserver`)로 알린다. 규칙:
  - 의존 계수는 (소유 패키지, 대상 패키지) 쌍의 간선 다중도다. 필드 둘이 같은 대상을 가리키면 2이고 하나를 지워도 1이 남는다.
  - 관찰하지 않는 간선: 공개 전 객체(`ConstructionContext` 안 — Commit할 때 Registry가 공개하는 객체들의 패키지 사이 간선 다중도를 한 번에 보고한다. 로드·임포트·`asset-new`·복제가 모두 이 경로다), `DestroyRequested` 소유자의 간선 제거(Discard 정리 — Discard가 나가는 계수를 한 번만 반납한다), Package의 하드 의존 목록(`ObjectRefArray<Package>` — AssetManager가 직접 유지하는 거울이다).
  - 관찰자는 이벤트를 큐에 넣기만 하고(콜백 안에서 Registry를 다시 부르지 않는다), `AssetLoader::Pump`가 Closing 처리 전에 큐를 비워 계수·Package 의존 목록·dirty를 갱신한다.
  - 새 간선이 상주 단위 사이에 순환을 만들면 두 단위(와 순환 경로의 단위)를 하나로 합친다(union-find, 11.4절). 합친 단위는 Discard될 때까지 나누지 않는다(보수 규칙 — 계수가 서로를 붙잡지 않는다).
  따라서 도구·에디터 서비스가 바꾼 하드 간선도 수명과 저장에 반영된다.
- GC: 에셋 객체 Registry의 증분 GC는 EngineAssets가 프레임 끝에 한 조각, 도구는 명시 `Collect`. GC는 열린 `ConstructionContext`가 없을 때만 진행한다(11.2절). 종료: World 종료 → lease·scope 해제 → 에셋 객체 Registry Shutdown.

### 6.2 참조 종류

| 종류 | 필드 | 저장 | 로드 | 쿠킹 | 누락 | 타입 불일치 | 순환 |
|---|---|---|---|---|---|---|---|
| Hard | `ObjectRef<T>` / `ObjectRefArray<T>`(다른 패키지 객체) | ObjectImports + Dependencies(Hard) | 같은 로드 작업에서 먼저 준비, 실패 시 참조자도 실패 | 전이 포함 | 저장·`set` 거부, 로드 `MissingDependency` | 저장 거부, 로드 실패 | SCC 함께 로드·상주(11.3·11.4절) |
| Soft | `SoftObjectRef<T>` / 배열 | Dependencies(Soft) | 자동 로드 안 함 | 기본 포함(`CookSoftReferences`) | 저장 경고(`Required`면 거부), 로드 요청 NotFound | `RequestAsset` TypeMismatch | 허용 |
| EditorOnly | `EditorOnly` 표시 Hard·Soft 필드 | Dependencies(EditorOnly) | 런타임 로더 무시 | 제외, 쿠킹 데이터에서 필드 제외 | 에디터 진단만 | 에디터 진단만 | 허용 |
| Build | `DependencyDeclarer`가 선언(16.2절) | Dependencies(Build) | — | 산출물 선행 조건·빌드 캐시 키 포함 | 쿠킹 실패 | 쿠킹 실패 | 지원 SCC는 한 단위, 아니면 `UnsupportedCycle` |
| ImportOwner | ImportRecord `ownerAssetId`(다중 출력의 소유 패키지) | Dependencies(ImportOwner) | 런타임 로더 무시 | 제외 | 에디터 진단(`MissingDependency`) | — | 허용 |

동적으로 로드하는 에셋은 `CookSettings`의 AlwaysCook·PrimaryAsset 규칙에 명시한다.

### 6.3 World·게임플레이에서의 참조

World 객체는 에셋 객체 Registry 밖에 있어 `ObjectRef<에셋>`을 걸 수 없다. World 객체의 영속 참조는 `SoftObjectRef<T>`(필드), 런타임 보유는 `AssetLease`(`ROCKS_NATIVE` 멤버)다. "하드" 의미(함께 로드·쿠킹)는 그 World 데이터를 담는 레벨·prefab 패키지(후속)의 Dependencies(Hard)로 표현한다. 에셋이 다른 에셋을 런타임에 붙잡아야 하면 lease가 아니라 Hard 참조를 쓴다(11.4절). 계획 §7.1의 `ObjectRef<MaterialAsset>` 예시는 이 규칙으로 바꾼다(17절).

## 7. AssetIndex(에셋 레지스트리)

- 패키지 파일이 유일한 진실이다. 헤더·섹션 표·Summary·Exports·ObjectImports·Dependencies·ImportRecord·Tags만 읽는다.
- 조회: ID·경로 키·타입(상속 포함)·디렉터리·태그, 정방향·역방향 의존(종류별), 별칭.
- 일관성: 프로세스 로컬 최종 일관 인덱스. 쓰기 결정은 잠금 아래에서 디스크를 다시 확인.
- 갱신: 자기 명령은 즉시. 외부 변경은 `DirectoryWatcher`(계약은 `Platform/DirectoryWatch.h`): 감시 시작 → 검색 → 쌓인 이벤트 적용. 이벤트 종류를 믿지 않고 경로를 다시 Stat·범위 읽기(레벨 트리거). `Overflow(path)`는 그 하위 트리 재검색, `RootRemoved`는 감시를 닫고 `RootLost` 진단·1초 간격으로 루트 재출현을 확인해 다시 연다. 쓰는 중이라 `CorruptPackage`인 파일은 크기·시각이 두 번 연속 같아질 때까지 최대 10초 기다린 뒤 다시 읽고, 그래도 손상이면 진단으로 남긴다.
- 캐시: `Intermediate/AssetIndex.cache`(형식·도구 빌드 ID 키, 원자적 기록, 잠금 보유 프로세스만 기록 — 읽기 명령은 캐시를 읽기만). 파일별 헤더+섹션 표 SHA-256과 요약. 시작 시 범위(약 1 KiB)만 읽어 비교, 다르면 그 파일만 다시 읽음. 범위 읽기는 WorkerScheduler 병렬.
- `Verify()`: 4.6절 진단 보고만.
- 원본 감시: 원본 SHA-256이 ImportRecord와 다르면 `SourceChanged`. 자동 재임포트는 기본 꺼짐, 켜면 debounce(500 ms) 후 재임포트, 실패 시 기존 보존·진단(계획 §9.4).

## 8. 동시성

- 쓰기 잠금: `Intermediate/AssetDatabase.lock`을 `FileLock`(Windows `LockFileEx` + 삭제 공유 없는 열기, POSIX `flock` + 잠근 뒤 dev/ino 재확인)으로 잡은 프로세스만 에셋·별칭 표·인덱스 캐시·쿠킹 출력을 바꾼다. 잠금을 잡는 명령은 13절 목록이며 서비스 API도 같은 규칙이다. 쿠킹도 이 잠금을 쥐므로(에디터 패키지가 쿠킹 중 바뀌지 않게) 같은 프로젝트의 쿠킹은 프로파일과 무관하게 직렬화되고 쿠킹 출력에 따로 잠금을 두지 않는다. 못 잡으면 `Busy`(`--wait=<초>`).
- 빌드 캐시는 내용 주소 저장소라 잠금 없이 동시 기록(9절).

## 9. 빌드 캐시

- 위치: `<BuildCache>` = `Config/BuildCache.json`의 `localPath`(기본 `<Project>/Intermediate/BuildCache/`). 프로젝트 사이 공유 경로도 쓸 수 있다(내용 주소라 안전하다).
- 빌드 캐시는 "캐시 키 → 가공 결과" 저장소다. 같은 에셋이라도 설정·대상 플랫폼이 다르면 결과가 다르므로(예: Windows용·Android용 `T_Wall`은 다른 키) **결과에 영향을 주는 입력은 빠짐없이 키에 들어가야 한다.** 빠진 입력은 오래된 결과를 돌려주는 버그가 되고, 불필요한 입력(경로·시각·locale)은 적중률만 떨어뜨린다(계획 §9.1).
- 키: 처리기 `BuildKey(KeyWriter&)`가 고정 순서·타입 태그로 기록한 바이트의 SHA-256. 모든 처리기에 공통으로 들어가는 것: `"RocksBuildCache" 형식 버전`, 처리기 Guid·처리기 버전 Guid, 결과를 만드는 **인코더·외부 라이브러리의 이름·버전·설정**(블록 압축 인코더 등 — 비압축이면 없음), **결정적 수학 버전**(A0-C), **툴체인 지문**(컴파일러 id·버전·대상 아키텍처·부동소수점 모델 — 빌드가 상수로 주입), 에셋 TypeId, 입력 bulk SHA-256, 결과에 영향을 주는 설정 전체(기본값 포함, float는 bit 패턴, NaN 단일화, `CompressionCacheId` 포함), 대상 프로파일 id와 출력 설정 전체, 의존 입력 (역할, AssetId, 그 의존 에셋의 같은 처리기 build key) 정렬 목록 — 의존의 설정·전이 Build 의존 변화까지 키에 들어간다(Merkle). 경로·시각·locale 제외.
- Build 의존 순환(지원 SCC, 6.2절)에서는 서로의 key가 서로를 포함해 값이 정해지지 않으므로 SCC 단위로 계산한다: SCC key = SHA-256(구성원의 지역 입력(위 항목 중 의존을 뺀 것)을 AssetId 순으로, SCC 밖 의존 튜플 (구성원 AssetId, 역할, 의존 AssetId, 그 key)의 정렬 목록), 구성원 key = SHA-256(SCC key, AssetId). 순환이 없어도 이 식 하나만 쓴다(구성원 하나인 SCC — 값 체계를 하나로 둔다).
- 텍스처 키의 구성(위 공통 항목의 텍스처 대응):

  | 입력 | 키에 넣는 값 |
  |---|---|
  | 원본 정체성 | TextureSource의 bulk SHA-256·형식·크기·slice·mip·layer 수(묶음 원본이면 각 원본 에셋의 build key) |
  | 텍스처 종류 | TypeId(Texture2D·Cube·2DArray·CubeArray·Volume) |
  | 색 공간 | `SRGB`·`bUseLegacyGamma`·`SourceColorSettings` 전체 |
  | mip 생성 | `MipGenSettings`·`bPreserveBorder`·`bNormalizeNormals`·`PowerOfTwoMode`·`PaddingColor`·`bPadWithBorderColor`·`ResizeDuringBuildX/Y`·`LODBias`·`MaxTextureSize`·`bFlipGreenChannel`과 프로파일 LOD 그룹 행 |
  | 압축 설정 | `CompressionSettings`·`CompressionNoAlpha`·`CompressionForceAlpha`·`CompressionNone`·`CompressionCacheId`(계약 설정도 값 그대로 — 기본값이 아니면 이미 거부되지만 키는 스키마 전체를 쓴다) |
  | 주소·샘플러 | `AddressX/Y/Z`·`AddressMode`·Border 색(Lanczos 경계 처리가 결과를 바꾼다), 그룹·텍스처에서 정한 Filter·MaxAniso(쿠킹 데이터의 샘플러) |
  | 대상 플랫폼 | 프로파일 id·형식 지원 표·bulk 정렬·codec |
  | 대상 픽셀 형식 | 12.4.3절 규칙으로 확정한 쿠킹 형식·sRGB 표시·swizzle(설정·프로파일에서 유도되지만 유도 규칙의 버그를 막기 위해 결과 형식도 넣는다) |
  | 처리기 버전 | 텍스처 처리기 Guid·버전 Guid |
  | 인코더 버전 | 블록 압축 인코더 라이브러리 이름·버전·품질 설정(비압축 단계에는 없음) |
- 값: `<BuildCache>/<key[0..2]>/<key>.rbc` = `{ "RBCH", 버전, key, 부분 수, 부분별(크기·SHA-256), 본문 }`(텍스처는 mip·slice별 부분).
- 읽기: 검증 실패 → `CorruptEntry` 진단·삭제·미적중(경합으로 정상 항목을 지워도 재생성되므로 무해).
- 쓰기: `<BuildCache>/tmp/<guid>` 기록·Flush → `Move(no-replace)`. 대상이 있으면 성공(게시 항목 불변). Put 실패는 치명적이지 않다. 전원 차단 뒤의 손상 항목은 읽기 검증이 거른다.
- 정리: 적중 시 `SetModifiedTime`, `cache-gc --max-bytes --unused-days`, `tmp` 24시간 경과 삭제.
- 인터페이스: `BuildCacheBackend { Get, Put, Remove, Touch }`, 계층 `BuildCache`(공유·원격은 계약). `--verify-cache`(적중 결과 = 재생성 결과 대조).
- 원본 bulk는 에디터 패키지가 권위 저장소(계획 §9.4)이므로 빌드 캐시를 지워도 손실 없음. Virtual Asset(원본 bulk 분리 저장소)은 후속(17절).

## 10. Cooker

- `TargetProfile`(엔진 `Engine/Config/TargetProfiles/<id>.json` + 프로젝트 재정의): id, 그래픽 backend, 형식 지원 표(비압축, 블록 압축 지원 = 이번에는 없음), LOD 그룹 표(12.4.5절), bulk 정렬. 엔진 기본: `Windows`, `Android`(형식 표만, 그래픽 backend 후속).
- `CookSettings`(`Config/CookSettings.json`): AlwaysCook·NeverCook(경로·디렉터리), PrimaryAsset 규칙(타입 → 디렉터리·bundle 목록), `CookSoftReferences`.
- 대상: 전체 쿠킹은 (AlwaysCook ∪ PrimaryAsset) → Hard 전이 ∪ (설정 시) Soft 전이. 부분 쿠킹은 (인자 경로) → Hard 전이 ∪ (설정 시) Soft 전이 ∪ 역방향 Build 폐포(원본 Texture2D를 고치면 그것을 묶는 Texture2DArray도 다시 쿠킹) ∪ 기존 매니페스트에서 cookKey가 달라진 항목. 부분 쿠킹은 에디터에서 사라진 에셋의 매니페스트 항목을 지우고, 그것을 하드 참조하는 남은 항목은 `MissingDependency`로 실패시킨다. 공통: EditorOnly 제외, NeverCook에 걸리면 `NeverCookReferenced`. 규칙에도 참조에도 없는 에셋은 쿠킹하지 않는다.
- 처리: 타입별 `CookProcessor`(빌드 캐시 사용)가 에디터 객체 → 쿠킹 객체·bulk. 쿠킹 데이터는 같은 타입에서 EditorOnly 필드를 뺀 것이다(EditorOnly 필드는 런타임 바이너리에 있고 쿠킹 데이터에 기록·로드되지 않는다). 항목마다 **cookKey**를 계산한다. 지역 입력 = (에디터 패키지 콘텐츠 해시, 논리 경로(경로 힌트·디렉터리 기반 NeverCook·PrimaryAsset 판정이 바뀐다), 그 항목에 적용된 CookSettings 판정(PrimaryAsset·bundle), 처리기 build key들, 쿠커 버전, 쿠킹 패키지 formatVersion·섹션 schemaVersion 집합, 타입 스키마 버전 표 SHA-256, 프로파일 출력 설정 SHA-256). 하드 참조 순환을 위해 SCC 단위로 계산한다: SCC key = SHA-256(구성원 지역 입력을 AssetId 순으로, SCC 밖 하드 의존 튜플 (구성원 AssetId, 의존 AssetId, 그 cookKey)의 정렬 목록), cookKey = SHA-256(SCC key, AssetId). 순환이 없어도 같은 식이다.
- 출력: 쿠킹 루트 `<Project>/Saved/Cooked/<Profile>/` 아래 `CookedManifest.rman`과 `Packages/`. 패키지 파일 이름은 내용 주소(4.2절)라 게시된 파일을 덮어쓰지 않는다.
- 게시:
  1. 각 패키지를 `.staging/`에 기록·Flush한 뒤 `Move(no-replace)`로 `Packages/<a>/`에 놓는다. 같은 이름이 이미 있으면 크기·SHA-256을 확인해 같으면 임시 파일만 지우고, 다르면(손상) `CorruptEntry` 경고 후 `Replace`로 바꾼다.
  2. 바뀐 `Packages/<a>/`와 `Packages/`, 그리고 이번 게시에서 새로 만든 상위 디렉터리(첫 쿠킹이면 쿠킹 루트 — 그 안의 `Packages` 항목)를 안쪽부터 `FileSystem::SyncDirectory`(A0-P)로 동기화한다 — 매니페스트보다 패키지 이름이 먼저 매체에 남아야 한다. POSIX는 디렉터리 fsync(macOS `F_FULLFSYNC`), Windows는 항목 추가 권한으로 연 디렉터리의 `FlushFileBuffers`다(관리자 권한 불필요. 로그 순서에 기대지 않으므로 `Packages`가 다른 volume을 가리키는 link여도 순서가 지켜진다). 디렉터리 동기화를 지원하지 않는 FS는 그 FS의 보장이 전부라 성공이다. `NotDurable`은 게시를 멈추고(매니페스트를 바꾸지 않는다) 오류로 보고한다. `Unsupported`(wrapper 기본 구현)도 오류다.
  3. 매니페스트를 임시 파일에 써서 Flush하고 `Replace`로 한 번에 바꾼다(POSIX는 쿠킹 루트도 동기화된다).
  매니페스트 교체가 유일한 전환점이므로 reader는 이전 매니페스트(이전 파일이 모두 있음) 또는 새 매니페스트(새 파일이 모두 있음)만 본다. 실패하면 매니페스트는 그대로이고 놓인 새 파일은 다음 전체 쿠킹이 지운다.
- 전체 쿠킹(`asset-cook --profile=X`)은 매니페스트를 바꾼 뒤 새 매니페스트가 가리키지 않는 패키지 파일을 지운다(이전 매니페스트로 이미 실행 중인 게임은 그 파일을 `NotFound`로 받는다 — 개발용 쿠킹 디렉터리의 한계이며 배포본은 바뀌지 않는다). 부분 쿠킹은 대상만 다시 만들고 기존 매니페스트에 병합하며 패키지 파일은 지우지 않는다(사라진 에셋의 매니페스트 항목만 뺀다). 병합은 기존 매니페스트의 프로파일 id·프로파일 출력 설정 SHA-256·CookSettings SHA-256·쿠커 버전이 모두 같을 때만 하고, 다르면 `IncompatibleCook`(전체 쿠킹 필요)이다. 처리기·포맷·타입 스키마·툴체인 변화는 항목별 cookKey가 잡는다(위 지역 입력).
- 매니페스트 v1(`CookedManifest.rman`): 헤더 `{ "RMAN", 버전 1, 항목 수, 프로파일 id, 프로파일 출력 설정 SHA-256, CookSettings SHA-256, 쿠커 버전 }` + 항목 `{ assetId, typeId, 경로 힌트, 패키지 SHA-256, 패키지 크기, cookKey, PrimaryAsset 표시, bundle 목록, 런타임 의존 목록 { assetId, kind(Hard·Soft) } }`(AssetId 정렬) + 파일 전체 CRC32. 손상은 `CorruptPackage`, 버전 불일치는 `UnsupportedVersion`. 로더는 시작할 때 매니페스트 프로파일 id가 실행 플랫폼의 프로파일과 같은지 확인하고(다르면 `IncompatibleCook`), 패키지를 열 때 크기·SHA-256과 패키지 Dependencies(Hard·Soft)를 매니페스트와 대조한다(다르면 `CorruptPackage`). SCC 계산은 매니페스트의 Hard 의존으로 로드 전에 한다.
- 도구 빌드 ID(Summary): 빌드 시스템이 쿠커·에디터 도구 실행 파일의 입력 지문으로 주입하는 값이며 진단·추적용이다(병합 조건이 아니다).
- 런타임 쿠킹 루트: 도구(`asset-load-cooked`)는 `<Project>/Saved/Cooked/<Profile>/`. 게임은 `EngineAssets`가 명령행 `-CookedRoot=<dir>`(없으면 실행 파일 디렉터리의 `Cooked/`)를 `AssetManager`에 넘긴다. 쿠킹 결과를 배포 위치로 옮기는 패키징 단계는 후속이다.
- 쿠킹 bulk codec: 프로파일 설정(None·Zstd, 기본 None — 후속 스트리밍의 mip 단위 접근을 위해 entry 단위 압축).
- 결정성(범위: 같은 도구 빌드에서 호스트 무관하게 같은 바이트): 정렬된 순회, 병렬 결과 정렬 후 기록, ID는 에디터 값 복사, 패딩 0. 이미지 처리 모듈은 SIMD 분기 없이 스칼라 경로만 쓰고, 초월 함수(pow·exp2·log2·sin)와 sRGB·감마 변환은 CRT를 쓰지 않는 Core 결정적 수학(A0-C)으로 하며 쿠킹 작업 동안 부동소수점 환경 guard(A0-C)를 건다. 현재 Core `LinearColor::ToSRGB`는 CRT `pow`를 써서 호스트마다 결과가 다를 수 있으므로(Color.h 주석) A0-C에서 결정적 수학의 표·함수로 바꾼다(런타임과 쿠킹의 sRGB 구현을 하나로). 결정적 수학 버전은 빌드 캐시 키와 cookKey에 들어간다. 시험: 결정적 수학의 기준값 표, CRT 미사용 링크 검사, 빌드 캐시 빈/찬 상태 출력 비교.
- 오류: 에셋별 원인·경로를 모아 보고, 하나라도 실패하면 게시하지 않는다.

## 11. 런타임 로딩과 수명(계획 §9.2 계약)

### 11.1 API

```cpp
struct AssetRequest { AssetId id; TypeId expectedType; LoadPriority priority; ReadyMask required; CancelToken cancel; AssetScope* scope; };
Result<LoadTicket> RequestAsset(const AssetRequest&);
Result<void> OnReady(LoadTicket&, TaskLane lane, Function<void(Result<AssetLease>)>&& callback);
Result<AssetLease> LoadBlocking(const AssetRequest&);
```

- LoadTicket = 요청 관심, AssetLease = 상주 수명(별도 계수). 동시 요청은 합류, caller별 취소는 독립, callback은 caller별 정확히 한 번 terminal. `OnReady` 중복 등록은 `AlreadyRegistered`, 등록 실패 시 callback 미등록·소유권 caller 유지(계획 §9.2).
- `ReadyMask`: `Cpu` 구현, `Render` 계약(`UnsupportedCapability`).
- `TaskLane`: `Game` 구현, `Worker`·`Render` 계약(이번 엔진은 Game 스레드 하나라 전달 대상이 없다 — `Unsupported`).
- `AssetScope`: PIE·World 수명 범위. `Revoke()`는 그 scope의 신규 접근·발급을 막고 진행 중 callback에 `ScopeClosed`를 한 번 전달, lease를 회수한다(다른 scope의 같은 에셋 lease는 유지).
- `LoadBlocking`: 도구·테스트·엔진 초기화 전용이다. AssetRuntime은 Engine을 모르므로 `EngineAssets`가 "프레임 실행 중" 판정 함수를 `AssetManager`에 주입하고, 그 구간에서 호출하면 `WrongContext`다(로딩 화면은 프레임 밖 초기화 단계 또는 `RequestAsset`을 쓴다).
- `AssetLease`의 해제(소멸)는 즉시 계수를 바꾸지 않고 `AssetLoader::Pump`의 해제 큐로 넘긴다(GC 회수 중의 소멸자에서도 안전하다). 큐 항목은 lease를 발급할 때 미리 할당해 lease 안에 두므로 noexcept 소멸자에서 할당 실패가 없다. scope `Revoke`로 이미 회수된 lease의 소멸은 아무것도 하지 않는다(이중 감소 없음).

### 11.2 단계와 스레드

Requested → ResolveManifest(`CookedManifest.rman`, 하드 의존 SCC 계산) → IO(`AsyncReadFile`) → Verify(크기·CRC·SHA-256)/Decompress(`WorkerScheduler`) → DeserializeNative(worker, 바이트 → 중간 표현) → ConstructObjects(Game, `ConstructionContext`) → Fixup(import 연결) → PostLoad(Game) → Ready. Game 단계는 `AssetLoader::Pump()`(EngineAssets 프레임 단계, 도구는 대기 루프)에서 예산(시간·건수) 안에서 진행한다. 한 로드 단위(SCC)의 ConstructObjects·Fixup·공개는 한 Pump 호출 안에서 끝내고(예산은 단위 사이에서 검사), PostLoad만 Pump 사이로 나눌 수 있다. 그래서 GC는 열린 `ConstructionContext`를 만나지 않는다. 실패 시 생성 객체·root를 안전 시점에 정리한다.

### 11.3 순환

하드 참조 그래프의 강연결 성분(Tarjan)을 한 로드 단위로 allocate-all → deserialize-all → fixup-all → publish(계획 §9.2). 다른 패키지의 PostLoad를 동기 대기하지 않는다.

### 11.4 수명

- 상주 단위는 하드 참조 SCC다(순환이 없으면 패키지 하나). 로드 시점의 SCC는 쿠킹 매니페스트의 Hard 의존(런타임) 또는 AssetIndex의 Hard 의존(에디터 패키지를 여는 도구)으로 계산하고, 실행 중 생긴 순환은 6.1절 규칙으로 단위를 합친다. 단위 U의 보유 수 = U에 속한 패키지들에 대한 요청 관심 + lease + 로드 작업의 내부 보유 + 다른 단위의 패키지에서 U의 패키지로 오는 하드 간선 다중도의 합이다(U 내부 간선은 세지 않는다 — 순환이 서로를 붙잡아 영구 상주하지 않는다).
- 의존 보유는 로드 중에도 쥔다: 로드 작업은 ResolveManifest에서 찾은 하드 의존 단위마다 내부 보유를 1 걸고, 완료 시 Fixup이 만든 실제 간선 다중도로 바꾸며 실패·취소 시 놓는다(의존 단위가 Fixup 도중 닫히지 않는다).
- 보유 수가 0이 되면 단위는 Closing 대기 목록에 오른다(Resident의 하위 상태). 다음 `AssetLoader::Pump`(어떤 `With` 범위도 열려 있지 않은 시점)가 처리하기 전에 보유 수가 다시 1 이상이 되면 — 같은 ID 요청이든, 다른 로드 작업의 의존 보유든, 새 간선이든 — 목록에서 빼고 되살린다(revive). 이미 Discard된 단위가 필요해지면 새 로드 작업으로 시작한다. Pump가 처리하면 RootRef를 놓고 단위의 모든 패키지·export를 `Discard`한다(A0-O: `RequestDestroy` + Outer·Name 표에서 즉시 제거. 이후 이름 조회는 실패하고 WeakRef `TryPin`은 `Destroyed`를 반환한다). Discard가 끝나면 Closing이 완료되고 이름이 비므로 같은 ID의 새 요청은 기다리지 않고 새 로드 작업으로 시작한다. 메모리는 이후 GC가 수거한다. Closing 전이와 Discard는 한 Pump 호출 안에서 Game 스레드가 이어서 하므로 그 사이에 발급이 끼지 않는다(계획 §9.2의 "발급이 먼저면 unload 대기, Closing이 먼저면 그 기록의 발급 실패"와 같다 — 새 요청은 새 로드 작업이다).
- "살아 있는 다른 에셋이 Discard된 객체를 가리키지 않는다"는 불변식의 근거: 에셋 객체 Registry의 모든 객체는 패키지에 속하고(6.1절) 패키지 사이 하드 간선은 모두 다중도로 세어지며(동적 변경 포함) 단위의 보유 수가 0일 때만 Discard한다. Discard는 그 단위에서 나가는 간선 계수를 한 번만 반납한다. 패키지에 들어갈 수 있는 클래스는 `AssetLease`를 멤버로 가질 수 없어(`WorldOnly` 규칙, 5.6절) Soft 순환이 lease로 서로를 붙잡는 누수도 없다.
- 정책: 즉시 해제(기본), PrimaryAsset bundle 로드는 bundle lease를 쥔다. 예산 LRU 해제는 후속. 텍스처 mip residency는 Object 수명과 별도(계획 §9.5), 이번에는 전체 mip 상주.

## 12. 텍스처

### 12.1 타입 구성

- `Texture : Asset`(공통 설정, 12.4.1절), 차원별 `Texture2D`·`TextureCube`·`Texture2DArray`·`TextureCubeArray`·`VolumeTexture`(UE 5.6과 같은 다섯 클래스, 고유 설정은 12.4.2절).
- 에디터 데이터 `TextureSource`(`ROCKS_STRUCT`, `ROCKS_PROPERTY(EditorOnly)` 필드): 형식·크기·slice 수·mip 수·layer 수·블록 목록·`BulkDataRef`(Zstd, raw SHA-256). 리플렉션으로 저장하므로 네이티브 직렬화 경로가 없다(5.6절). **원본 저장 형식은 디코딩한 정규 픽셀**이다(원본 파일 바이트는 저장하지 않는다 — UE는 PNG·JPEG 바이트를 그대로 둘 수 있다, 12.4.4절). 근거: 재임포트 없이 재빌드, 디코더 버전과 무관한 동일 입력, 빌드 경로에 원본 포맷 코드가 없음. 원본 파일 SHA-256은 ImportRecord에 있다.
- 플랫폼 데이터 `TexturePlatformData`(`ROCKS_STRUCT`, 쿠킹 데이터): dimension·format·sRGB·**swizzle**(출력 R·G·B·A 각각이 형식의 R·G·B·A·0·1 중 무엇인지 — 렌더러가 view swizzle로 적용, A0-G)·크기·mip 수·slice 수·mip(·slice)별 `{ 크기, BulkDataRef }`·resident tail mip 수(계획 §9.5)·**샘플러**(텍스처·LOD 그룹에서 정한 Filter·MaxAniso·축별 주소·Border 색 — 렌더러가 그대로 쓴다).
- Texture2DArray·TextureCubeArray·VolumeTexture는 다른 에셋을 원본으로 묶을 수 있다(UE 5.6 `SourceTextures`·`Source2DTexture` — Build 의존, 16.2절). 원본 파일 여러 개로 직접 임포트하는 경로도 있다(12.3절).
- UE의 다른 텍스처 계열(렌더 타깃 에셋 `TextureRenderTarget*`, IES `TextureLightProfile`, `RuntimeVirtualTexture`, 미디어·동적 텍스처)은 후속이다(18절).

### 12.2 원본 형식

| 원본 | TextureSource |
|---|---|
| PNG 회색 1·2·4비트 | G8(비트 복제로 확장: v × 255 / (2^b − 1)) |
| PNG·PSD 회색 8비트, TGA 회색(형식 3·11) 8비트 | G8 |
| PNG·PSD 회색 16비트 | G16 |
| PNG·PSD 회색+알파 8비트, PNG 회색 1–8비트 + tRNS | GA8(tRNS: 지정 값과 같은 texel은 알파 0, 나머지 255) |
| PNG·PSD 회색+알파 16비트, PNG 회색 16비트 + tRNS | GA16 |
| PNG 팔레트 1·2·4·8비트(PLTE·tRNS), BMP 1·4·8비트 색표(BI_RGB·BI_RLE4·BI_RLE8), TGA 색표(형식 1·9), PSD Indexed | RGBA8(tRNS에 없는 항목은 알파 255) |
| PSD Bitmap(1비트) | G8(0 → 255 흰색, 1 → 0 검정 — PSD 규약) |
| PNG 색 8비트(RGB·RGBA·RGB+tRNS), PSD RGB 8비트, BMP 16·24·32비트, TGA 15·16·24·32비트(형식 2·10) | RGBA8(5·6비트 성분은 비트 복제로 확장) |
| PNG 색 16비트(RGB·RGBA·RGB+tRNS), PSD RGB 16비트 | RGBA16 |
| PSD 32비트 float RGB·RGBA | RGBA32F |
| PSD 32비트 float 회색, 회색+알파 | R32F, RGBA32F(회색을 r = g = b로) |
| Radiance HDR(RGBE)·무손실 OpenEXR | RGBA16F·RGBA32F·R16F·R32F — 형식은 구현, 디코더는 후속 |
| JPEG, 손실 WebP, 손실 압축 EXR(B44·B44A·DWAA·DWAB, 32비트 float 채널의 PXR24) | `LossySource`로 거부(사용자 결정: 무손실만) |

원본별 규칙:

- BMP: 16비트 BI_RGB는 555(최상위 비트 무시), 32비트 BI_RGB는 4번째 byte가 예약값이므로 알파 255로 읽는다. BI_BITFIELDS·BI_ALPHABITFIELDS는 마스크를 따르며, 알파 마스크는 BI_ALPHABITFIELDS 또는 V4·V5 헤더의 알파 마스크가 0이 아닐 때만 쓴다(BI_RGB에서는 V5의 알파 마스크도 무시). 마스크는 연속 비트여야 하고 서로 겹치지 않아야 하며(아니면 `InvalidSource`), 폭 w의 성분은 v × 255 / (2^w − 1)을 round-half-up해 8비트로 만든다(w > 8도 RGBA8로 줄인다). BI_JPEG·BI_PNG는 `UnsupportedSourceFormat`.
- TGA: image descriptor의 알파 비트 수(bit 0–3)가 0이면 4번째 성분·16비트의 attribute 비트를 무시한다(알파 255). TGA 2.0 확장 영역의 attributes type이 0–2(알파 없음·미정의·무시)면 무시, 3이면 일반 알파, 4(premultiplied)면 성분을 8비트로 확장한 뒤 straight 알파로 되돌린다(min(255, round-half-up(c × 255 / a)), 알파 0이면 색 0).
- PSD: 색 모드 RGB·Grayscale·Indexed·Bitmap을 받고 CMYK·Lab·Multichannel·Duotone은 `UnsupportedSourceFormat`. 병합(composite) 이미지 데이터를 쓴다 — image resource 0x0421(Version Info)의 hasRealMergedData가 false면(Maximize Compatibility를 끄고 저장) 레이어 합성은 지원하지 않으므로 `UnsupportedSourceFormat`. Indexed는 색표 크기를 resource 0x0416(색 수)으로, 투명 항목을 0x0417(투명 index)로 정한다(그 항목은 알파 0). 레이어·마스크 정보의 레이어 수가 음수일 때만 첫 추가 채널이 병합 결과의 투명도이므로 알파로 쓰고(PSD 규약), 그 밖의 추가 채널(저장된 선택 영역·spot 채널)은 `IgnoredSourceChannels` 경고와 함께 버린다.
- float 원본(PSD 32비트 등)은 디코딩 직후 NaN을 0으로, ±Inf를 ±float 최대 유한값으로 바꾼다(이후 필터·mip에서 번지지 않게).
- PNG 범위는 15절.

색 해석과 출력 부호화(정수 원본의 색 채널에만 적용, 알파는 항상 선형 값 그대로):

1. 원본 디코딩: `SourceColorSettings.EncodingOverride`가 None이 아니면 그 부호화(구현: `Linear`·`sRGB`, 나머지는 계약)로, None이면 `SRGB = on`일 때 `bUseLegacyGamma ? x^2.2 : sRGB 구간 함수`, off일 때 선형으로 읽는다. float 원본은 EncodingOverride가 없으면 선형이다(SRGB는 출력에만 영향).
2. 모든 처리(크기 조정·mip·필터)는 선형 float에서 한다(12.5절).
3. 출력 부호화(`SRGB = on`이고 4번에서 무시되지 않을 때): 출력 형식이 8비트이면 sRGB 구간 함수로 부호화한다(GPU는 구간 함수로 디코딩하므로 `bUseLegacyGamma`는 원본 디코딩에만 쓰인다). 쿠킹 형식에 sRGB 변형이 있으면(RGBA8) 그것을 쓰고, 없으면(R8·RG8) 12.4.3절의 swizzle을 데이터에 먼저 적용한 뒤 RGBA8 sRGB로 확장하고 swizzle은 항등으로 기록한다(예: GA8 Default → (g, g, g, a), Grayscale → (g, g, g, 1)). 16비트 정수 출력(R16·RG16·RGBA16)은 sRGB 변형이 없으므로 선형화해 같은 채널 수의 16F(R16F·RG16F·RGBA16F)로 저장하고 sRGB 표시를 하지 않는다(16F는 1.0 근처 약 11비트 정밀도로 16비트 원본보다 낮다 — sRGB 16비트 텍스처의 대가로 문서화한다).
4. `SRGB` 값은 저장된 값만으로 해석한다. 다음 경우 빌드는 SRGB를 off로 보고 `SrgbIgnored` 경고를 낸다: CompressionSettings가 선형 데이터 종류(12.4.3절 SRGB 열이 "무시")일 때, float 원본이 float 출력으로 갈 때(부호화할 8비트 출력이 없다). 명령 편의: CompressionSettings를 선형 종류로 바꾸거나 float 원본을 임포트하는 명령은 같은 명령에서 SRGB를 주지 않았으면 SRGB를 off로 함께 저장한다(UE도 선형 종류에서 SRGB를 끈다) — 저장 값이 깨끗하게 유지된다.
5. 양자화: 정수 출력은 [0, 1]로 자른 뒤 round-half-up(unorm8 = floor(v × 255 + 0.5), unorm16은 65535). float 출력: NaN → 0, +Inf·−Inf → 그 형식의 최대·최소 유한값, 16F 범위(±65504)를 넘는 유한값은 ±65504로 자른다. 16F 변환은 round-to-nearest-even. 16비트 정수 → 8비트 출력도 이 규칙이다(선형 또는 디코딩된 float를 거친다).

### 12.3 원본 구성과 지정 문법

| 차원 | 원본 | 명령(13절 문법) |
|---|---|---|
| 2D | 파일 하나 | `asset-import /Game/T/A a.png` |
| Cube | 6면 파일, 또는 한 파일의 배치 | `asset-import /Game/T/Sky --cube=px:a_px.png,nx:a_nx.png,py:a_py.png,ny:a_ny.png,pz:a_pz.png,nz:a_nz.png` / `asset-import /Game/T/Sky cross.png --cube-layout=HorizontalCross` |
| 2DArray | 파일 여러 개(slice 순서), 또는 Texture2D 에셋 목록 | `asset-import /Game/T/Arr --array=a0.png,a1.png` / `asset-new Texture2DArray /Game/T/Arr SourceTextures=/Game/T/A,/Game/T/B` |
| CubeArray | Cube 구성 × n, 또는 TextureCube 에셋 목록 | `asset-import /Game/T/Skies "--cube-array=px:…,nz:…;px:…,nz:…"` / `asset-new TextureCubeArray /Game/T/Skies SourceTextures=/Game/T/S0,/Game/T/S1` |
| Volume | 2D 파일 + 타일 크기, 또는 slice 파일 여러 개, 또는 Texture2D 에셋 + 타일 크기 | `asset-import /Game/T/Vol atlas.png --volume-tile=16x16` / `asset-import /Game/T/Vol --volume=s0.png,s1.png` / `asset-new VolumeTexture /Game/T/Vol Source2DTexture=/Game/T/Atlas Source2DTileSizeX=16 Source2DTileSizeY=16` |

- Cube 면 순서는 D3D 규약(+X −X +Y −Y +Z −Z = `px nx py ny pz nz`)이고 면 이미지의 방향도 D3D 규약을 따른다. 배치: `HorizontalCross`(4×3 칸: (1,0)=+Y, (0,1)=−X, (1,1)=+Z, (2,1)=+X, (3,1)=−Z, (1,2)=−Y), `VerticalCross`(3×4 칸: (1,0)=+Y, (0,1)=−X, (1,1)=+Z, (2,1)=+X, (1,2)=−Y, (1,3)=−Z를 180° 회전해 담음), `Strip6`(가로 6칸, 면 순서대로). 칸 크기가 같지 않거나 비면 `InvalidImportOption`.
- `--cube-array`의 각 cube 지정은 `--cube` 값과 같은 형식이고 `;`로 나눈다(셸 명령 구분자이므로 따옴표로 감싼다). `--cube-layout`·`--volume-tile`은 위치 인자 원본 하나와 함께, `--cube`·`--array`·`--cube-array`·`--volume`은 위치 인자 원본 없이 쓴다.
- equirect(위도·경도) → Cube 변환은 HDR 디코더와 함께 후속. 서비스 API는 같은 구성을 `TextureImportSpec` 구조로 받는다.

### 12.4 설정(UE 5.6 대조)

대조 근거: UE 5.6 공식 API 문서 — C++ `UTexture` 멤버 75개와 기반 `UStreamableRenderAsset`의 스트리밍 멤버, `FTextureSource`, `FTextureLODGroup` 멤버 29개, 관련 enum, Python `unreal.Texture`·`Texture2D`·`TextureCube`·`Texture2DArray`·`TextureCubeArray`·`VolumeTexture` 편집 속성(2026-11-12 확인). 속성 이름은 UE C++ 이름이며 Rocks 필드는 `ROCKS_PROPERTY(Name="…")`로 그 이름을 노출한다(5.6절, 명령행 이름). enum 값 이름은 UE 접두사(`TC_`·`TMGS_`·`TEXTUREGROUP_`·`TF_`·`TA_` 등)를 뗀 나머지 그대로다(예: `TC_HDR_Compressed` → `HDR_Compressed`). **계약** 설정은 스키마에 있고 기본값이 아니면 `UnsupportedSetting`이며, 사용자가 정한 순서(기반 → 화면 → 압축·보정·스트리밍)의 뒤 단계에서 구현한다.

#### 12.4.1 공통 설정(UTexture·UStreamableRenderAsset)

| UE 5.6 멤버 | 상태 |
|---|---|
| `LODGroup`(TextureGroup 66종: World…Project32, 값 0–65. Project01–32는 프로젝트 설정에서 이름을 바꾼다) | 구현 |
| `SRGB`, `bUseLegacyGamma`(원본을 sRGB 대신 2.2 감마로 해석, 12.2절) | 구현 |
| `CompressionSettings`(편집 가능 15종, 대응은 12.4.3절). 숨김 값 `TC_LQ`(13)·`TC_EncodedReflectionCapture`(14, UE 주석: 더 이상 쓰지 않음) | 구현. 숨김 값은 `InvalidSetting` |
| `CompressionNoAlpha` | 구현. UE 설명대로 비압축 RGBA 출력에는 적용하지 않으므로 지금은 결과를 바꾸지 않는다(블록 압축 단계에서 의미가 생긴다) |
| `CompressionForceAlpha`, `CompressionNone`(UE 내부 설정 — 편집 속성 아님) | 구현(스키마·빌드 캐시 키 포함). 비압축 쿠킹은 알파를 자동으로 빼지 않고 모두 비압축이므로 지금은 결과를 바꾸지 않는다 |
| `CompressionCacheId`(Guid — 빌드 캐시 키에 넣어 바꾸면 재빌드) | 구현 |
| `MipGenSettings`: `FromTextureGroup`·`SimpleAverage`·`NoMipmaps`·`Unfiltered` | 구현(12.5절) |
| `Filter`(`Nearest`·`Bilinear`·`Trilinear`·`Default` = 그룹 값) | 구현 |
| `LODBias`, `MaxTextureSize`(0 = 형식 최대) | 구현(그룹 값과의 합성은 12.5절) |
| `bFlipGreenChannel`, `bPreserveBorder`, `bNormalizeNormals` | 구현 |
| `PowerOfTwoMode`(`None`·`PadToPowerOfTwo`·`PadToSquarePowerOfTwo`·`StretchToPowerOfTwo`·`StretchToSquarePowerOfTwo`·`ResizeToSpecificResolution`), `PaddingColor`, `bPadWithBorderColor`, `ResizeDuringBuildX`·`ResizeDuringBuildY`(`ResizeToSpecificResolution`일 때만 적용, 0 = 원래 크기) | 구현(12.5절) |
| `NeverStream`, `bGlobalForceMipLevelsToBeResident`(UStreamableRenderAsset), `MipLoadOptions` = `Default`·`AllMips` | 구현. 지금은 모든 mip이 상주하므로 이 값들은 모두 충족된다 |
| `Availability`(`GPU`·`CPU` — CPU는 Texture2D 전용, 픽셀은 CPU에서 읽고 GPU에는 작은 검정 placeholder만 올린다) | 구현(저장·쿠킹). 렌더러 반영은 화면 마일스톤 |
| `CookPlatformTilingSettings`(`FromTextureGroup`·`Tile`·`DoNotTile`), `bNoTiling`(UE 내부) | 구현. 타일링을 지원하는 플랫폼이 없어 모든 값이 비타일 결과다 |
| `SourceColorSettings`: `EncodingOverride` = `None`·`Linear`·`sRGB`, `ColorSpace` = `None`·`sRGB` | 구현(색 공간 변환 없음) |
| `AssetImportData` | ImportRecord로 구현(5.4절) |
| `MipGenSettings`: `Sharpen0`–`Sharpen10`·`Blur1`–`Blur5`·`Angular`(Cube 전용)·`LeaveExistingMips`(원본 mip — 12.4.4절) | 계약 |
| `bUseNewMipFilter`(공개 문서에 의미가 "현대적 mip 필터, 끄면 옛 동작"으로만 있다) | 계약(기본값 false만 받는다. 의미는 보정 단계에서 UE 동작과 대조해 확정) |
| 보정: `AdjustBrightness`·`AdjustBrightnessCurve`·`AdjustVibrance`·`AdjustSaturation`·`AdjustRGBCurve`·`AdjustHue`·`AdjustMinAlpha`·`AdjustMaxAlpha`, `bChromaKeyTexture`·`ChromaKeyThreshold`·`ChromaKeyColor`, `bDoScaleMipsForAlphaCoverage`·`AlphaCoverageThresholds`, `CompositeTexture`·`CompositeTextureMode`(`Disabled`·`NormalRoughnessToRed/Green/Blue/Alpha`)·`CompositePower`, `Downscale`(플랫폼별)·`DownscaleOptions`(`Default`·`Unfiltered`·`SimpleAverage`·`Sharpen0`–`Sharpen10`) | 계약 |
| 색 관리: `SourceColorSettings`의 그 밖 `EncodingOverride`(`ST2084`·`Gamma22`·`BT1886`·`Gamma26`·`Cineon`·`REDLog`·`REDLog3G10`·`SLog1`–`SLog3`·`AlexaV3LogC`·`CanonLog`·`ProTune`·`VLog`), `ColorSpace`(`Rec2020`·`ACESAP0`·`ACESAP1`·`P3DCI`·`P3D65`·`REDWideGamut`·`SonySGamut3`·`SonySGamut3Cine`·`AlexaWideGamut`·`CanonCinemaGamut`·`GoProProtuneNative`·`PanasonicVGamut`·`Custom`), 색도 좌표 4개, `ChromaticAdaptationMethod`(`None`·`Bradford`·`CAT02`) | 계약 |
| 압축: `CompressionYCoCg`, `LayerFormatSettings`(층별 형식 설정, UE 내부), `LossyCompressionAmount`(7종), `CompressionQuality`(ASTC 6종), `bOodlePreserveExtremes`, `OodleTextureSdkVersion` | 계약 |
| 스트리밍: `NumCinematicMipLevels`(UStreamableRenderAsset), `MipLoadOptions` = `OnlyFirstMip` | 계약 |
| 가상 텍스처: `VirtualTextureStreaming`, `VirtualTextureStreamingPriority`, `bUseVirtualTextureStreamingPriority`, `VirtualTexturePrefetchMips` | 계약 |
| 설정이 아닌 멤버: `DeferCompression`·`CompressFinal`(에디터 세션 동작), `OverrideRunningPlatformName`(에디터 미리보기), `TextureEditorCustomEncoding`, `bIsImporting`·`bCustomPropertiesImported`·`bNotOfflineProcessed`(임포트 내부 표시), `LightingGuid`·`LevelIndex`(정적 조명·스트리밍 내부), `DidSerializeStreamingMipsForPlatform`·`LoadedMainStreamObjectVersion`(직렬화 내부), `TextureReference`·`ReleaseFence`·`PrivateResource`·`PrivateResourceRenderThread`·`bAsyncResourceReleaseHasBeenStarted`·`KnownCompositeTexture`(런타임 자원), 폐기 멤버 `bDitherMipMapAlpha_DEPRECATED`·`SourceFilePath_DEPRECATED`. `AssetUserData`(Python 편집 속성)는 일반 에셋 사용자 데이터 기능으로 후속(18절) | 대응하지 않음 |

#### 12.4.2 차원별 설정

| 클래스 | UE 5.6 고유 설정 | 상태 |
|---|---|---|
| Texture2D | `AddressX`·`AddressY`(`Wrap`·`Clamp`·`Mirror`) | 구현 |
| TextureCube | 없음(UE 5.6 TextureCube에는 주소 설정이 없다 — 샘플러는 Clamp) | 구현 |
| Texture2DArray | `AddressX`·`AddressY`·`AddressZ`, `SourceTextures`(Texture2D 목록) | 구현(A0-G에서 GraphicsTypes 샘플러에 W축 주소 추가) |
| TextureCubeArray | `SourceTextures`(TextureCube 목록) | 구현 |
| VolumeTexture | `AddressMode`(세 축 공통), `Source2DTexture`·`Source2DTileSizeX`·`Source2DTileSizeY` | 구현(같은 A0-G 보강) |
| Rocks 확장 | 주소 `Border` + `BorderColor`(`TransparentBlack`·`OpaqueBlack`·`OpaqueWhite`). UE TextureAddress에는 없다(Wrap·Clamp·Mirror). GraphicsTypes에 구현되어 있다 | 구현 |

#### 12.4.3 CompressionSettings → 쿠킹 형식(비압축)

UE 5.6 공개 문서로는 설정별 블록 압축 형식(BC·ASTC)과 단일 채널 설정이 원본의 어느 채널을 쓰는지가 확인되지 않는다. 이번 비압축 쿠킹은 채널 의미와 원본 정밀도를 보존하는 아래 규칙으로 정하고, 블록 압축 단계에서 UE 대응을 확정한다. "빨강"은 원본의 빨강 채널(회색 원본은 회색 값)이다 — UE 5.6 주석이 `Alpha`를 "단일 채널, displacement·gray에도 쓸 수 있다"고 하므로 회색·빨강 데이터를 담는 단일 채널로 본다. 표의 형식은 SRGB off 기준이며 sRGB 출력은 12.2절 3번 규칙을 따른다.

| CompressionSettings(값) | 데이터 | 8비트 정수 원본 | 16비트 정수 원본 | float 원본 | swizzle | SRGB |
|---|---|---|---|---|---|---|
| `Default`(0) | 색·알파 | G8 → R8, GA8 → RG8, RGBA8 → RGBA8 | G16 → R16, GA16 → RG16, RGBA16 → RGBA16 | 같은 채널 수의 16F | R: rrr1, RG: rrrg, RGBA: 항등 | 설정값 |
| `Normalmap`(1) | 법선 xyz | RGBA8 | RGBA16 | RGBA16F | 항등 | 무시 |
| `Masks`(2) | 선형 채널 | RGBA8 | RGBA16 | RGBA16F | 항등 | 무시 |
| `Grayscale`(3) | 빨강 | R8 | R16 | R16F | rrr1 | 설정값 |
| `Displacementmap`(4) | 빨강 | R8 | R16 | R16F | 항등(r001) | 무시 |
| `VectorDisplacementmap`(5) | RGBA | RGBA8 | RGBA16 | RGBA16F | 항등 | 무시 |
| `HDR`(6) | RGBA | RGBA16F | RGBA16F | RGBA16F | 항등 | 무시 |
| `EditorIcon`(7, UI) | RGBA | RGBA8 | RGBA8 | RGBA8 | 항등 | 설정값 |
| `Alpha`(8) | 빨강 | R8 | R16 | R16F | 항등(r001) | 무시 |
| `DistanceFieldFont`(9) | 빨강 | R8 | R8 | R8 | 항등(r001) | 무시 |
| `HDR_Compressed`(10) | RGB | RGBA16F | RGBA16F | RGBA16F | 항등(데이터 알파 1) | 무시 |
| `BC7`(11) | RGBA | RGBA8 | RGBA8 | RGBA8 | 항등 | 설정값 |
| `HalfFloat`(12) | 빨강 | R16F | R16F | R16F | 항등(r001) | 무시 |
| `SingleFloat`(15) | 빨강 | R32F | R32F | R32F | 항등(r001) | 무시 |
| `HDR_F32`(16) | RGBA | RGBA32F | RGBA32F | RGBA32F | 항등 | 무시 |

- 채널 수가 늘면 회색은 r = g = b, 없는 알파는 1이다. 16비트 → 8비트, float → 8비트는 12.2절 5번 양자화를 따른다(float 원본의 8비트 출력에서 `SRGB = on`이면 sRGB로 부호화해 어두운 영역 정밀도를 지킨다).
- 블록 압축 형식이 필요한 설정은 프로파일이 압축을 요구하면 `UnsupportedSetting`이다(12.6절).

#### 12.4.4 TextureSource(UE FTextureSource 대조)

| UE 5.6 FTextureSource | Rocks TextureSource | 상태 |
|---|---|---|
| `Format`(ETextureSourceFormat: G8·BGRA8·BGRE8·RGBA16·RGBA16F·G16·RGBA32F·R16F·R32F) | G8·GA8·G16·GA16·RGBA8·RGBA16·RGBA16F·RGBA32F·R16F·R32F. GA8·GA16은 Rocks 추가(UE는 BGRA8·RGBA16으로 확장), 색 순서는 RGBA, BGRE8(RGBE)은 디코딩 때 float로 | 구현 |
| `SizeX`·`SizeY`·`NumSlices`(Cube 6, CubeArray 6 × n, Volume 깊이, 2DArray 층 수, long-lat Cube 1) | 같음(long-lat 제외) | 구현 |
| `NumMips`(원본 mip — `LeaveExistingMips`) | numMips = 1만 | 계약 |
| `NumLayers`·`LayerFormat`·`LayerColorInfo_LockProtected`(다층 원본) | numLayers = 1만 | 계약 |
| `Blocks`·`BaseBlockX`·`BaseBlockY`·`BlockDataOffsets`(UDIM 다중 블록) | 블록 1개만 | 계약(가상 텍스처와 함께) |
| `bLongLatCubemap` | 없음 | 후속(equirect → Cube, HDR 디코더와 함께) |
| `CompressionFormat`(None·PNG·JPEG·UEJPEG·UEDELTA) | 정규 픽셀 + Zstd(원본 압축 바이트를 저장하지 않음 — 12.1절 근거, JPEG는 손실이라 받지 않음) | 구현(설계 차이) |
| `Id`·`bGuidIsHash` | 콘텐츠 SHA-256(`BulkDataRef`) | 구현 |
| `BulkData`(FEditorBulkData) | `BulkDataRef` → Bulk 섹션 | 구현 |
| `Owner`, `BulkDataLock`·`LockState`·`LockedMipData`·`NumLockedMips`, `TornOffGammaSpace`·`TornOffOwnerName`·`TornOffTextureClass` | 런타임 소유·잠금·분리 상태(데이터 아님) | 대응하지 않음 |

#### 12.4.5 LOD 그룹(TargetProfile 그룹 표, UE 5.6 FTextureLODGroup 대조)

| FTextureLODGroup 멤버 | 상태 |
|---|---|
| `Group`, `MinLODSize`, `MaxLODSize`, `LODBias`, `MaxLODMipCount`, `MipGenSettings`, `MaxAniso`, `HighPriorityLoad`(로드 우선순위 High), `MipLoadOptions` = `Default`·`AllMips`, `CookPlatformTilingDisabled`(타일링 플랫폼 없음) | 구현(합성은 12.5절) |
| `MinMagFilter`·`MipFilter`(FName: min/mag `point`·`linear`·`aniso`, mip `point`·`linear`)와 `Filter`(ETextureSamplerFilter) | 구현. 프로파일은 UE 설정처럼 MinMagFilter·MipFilter로 적고 Filter는 그 조합으로 정한다: `point`(mip 무관) → `Point`, `linear`+`point` → `Bilinear`, `linear`+`linear` → `Trilinear`, `aniso`+`point` → `AnisotropicPoint`, `aniso`+`linear` → `AnisotropicLinear`. Filter를 함께 적었는데 조합과 다르면 `InvalidSetting`, Filter만 적으면 MinMagFilter·MipFilter는 그 역대응으로 정한다(`Point` → point·point). AnisotropicPoint는 A0-G에서 GraphicsTypes 필터에 추가한다. 프로파일 검증: MinLODSize > MaxLODSize면 `InvalidSetting` |
| `LODBias_Smaller`·`LODBias_Smallest`·`MaxLODSize_Smaller`·`MaxLODSize_Smallest`(장치 메모리 등급별) | 계약 |
| `NumStreamedMips`, `OptionalLODBias`·`OptionalMaxLODSize`·`DuplicateNonOptionalMips`(선택 mip 패키지) | 계약(스트리밍) |
| `LODBias_VT`·`MaxLODSize_VT`·`VirtualTextureTileCountBias`·`VirtualTextureTileSizeBias`·`VirtualTextureStreamingPriority` | 계약(가상 텍스처) |
| `Downscale`·`DownscaleOptions`, `LossyCompressionAmount` | 계약(보정·압축) |

### 12.5 빌드 처리와 mip 규칙

적용 순서(한 텍스처의 각 slice·면에 같은 순서):

1. 원본 → 선형 float 디코딩(12.2절 1번). 알파는 그대로.
2. `bFlipGreenChannel`(g → 1 − g).
3. `PowerOfTwoMode`(X·Y 축에만 적용 — Volume은 각 slice의 X·Y, Z는 그대로. Square 변형은 X = Y = 둘 중 큰 값): Pad는 각 축을 n 이상인 가장 작은 2의 거듭제곱으로 늘리고, 늘어난 영역은 `bPadWithBorderColor`면 가장 가까운 테두리 texel, 아니면 `PaddingColor`(원본과 같은 색 해석으로 디코딩한 값)로 채운다. Stretch는 각 축을 가장 가까운 2의 거듭제곱(차이가 같으면 큰 쪽)으로, ResizeToSpecificResolution은 `ResizeDuringBuildX`·`ResizeDuringBuildY`(0이면 원래 크기)로 Lanczos3 재표본한다. 차원 규칙: Cube·CubeArray 면은 정사각형이어야 하고(원본이 아니면 `InvalidSource`) Resize가 정사각형을 깨면 `InvalidSetting`이다. 2DArray·CubeArray·Volume의 slice는 모두 같은 크기·형식이어야 한다(파일 구성이면 `InvalidSource`, 에셋 원본 묶음이면 `InvalidSetting`).
4. 크기 제한과 LOD 합성(쿠킹 때, 그룹 = 프로파일 LOD 그룹, `FromTextureGroup`·`Default`도 그룹 값으로 바뀐다). N = 1×1(×1)까지의 전체 mip 사슬 길이, d = 버릴 상위 mip 수:
   - 하드 상한 d_hard = max(상위 mip의 긴 축(X·Y, Volume은 Z 포함)이 min(MaxTextureSize(0이면 무한), 그룹 MaxLODSize(0이면 무한), 차원 최대(2D·Cube·Array 16384, Volume 2048 — 프로파일 형식 표)) 이하가 되는 최소 수, N − 그룹 MaxLODMipCount(0이면 제한 없음)).
   - 편향 d_bias = 텍스처 LODBias + 그룹 LODBias를 남는 상위 mip의 긴 축(Volume은 Z 포함)이 그룹 MinLODSize 이상이 되는 최대 수로 줄인 값(원본이 MinLODSize보다 작으면 0).
   - d = min(max(d_bias, d_hard), N − 1). 상한이 MinLODSize보다 우선한다.
   - `NoMipmaps`면 mip 사슬을 저장하지 않는다. LODBias·MaxLODMipCount는 적용하지 않고 d_hard의 크기 상한만 2배씩 SimpleAverage 축소로 맞춘다.
5. mip 생성(선형 float, 아래 규칙): mip k + 1은 처리를 마친 mip k에서 만들고, 만든 직후 `bPreserveBorder`(Address가 Wrap이면 `PreserveBorderWithWrap` 경고)와 `bNormalizeNormals`를 그 mip에 적용한 결과가 다음 mip의 입력이 된다(mip 0에는 둘 다 적용하지 않는다). d개의 상위 mip은 만든 뒤 버린다.
6. 출력 부호화·양자화(12.2절 3·5번), swizzle 기록(12.4.3절).

크기 조정 필터 Lanczos3(결정적 계산 — 기준값 표로 고정):
- 표본 중심: 대상 i의 원본 좌표 = (i + 0.5) × (원본 길이 / 대상 길이) − 0.5.
- 커널: L(x) = sinc(x)·sinc(x / 3)(|x| < 3, 그 밖 0), sinc(x) = sin(πx) / (πx), sinc(0) = 1. Core 결정적 수학의 `SinPi`로 계산한다(A0-C).
- 축소(비율 s = 대상 / 원본 < 1)이면 커널을 1/s배 넓힌다(L(x·s), 반경 3/s 원본 texel). 확대이면 반경 3.
- 가중치는 탭 합으로 정규화한다. 축별 분리(X → Y, Volume Resize는 X·Y만).
- 경계는 텍스처 Address를 따른다: Wrap → 감싸기, Mirror → GPU MIRROR(주기 2n, 끝 texel 포함 반사 — n = 1이면 항상 그 texel), Clamp·Border → 끝 texel 반복.
- 정수 출력 형식으로 갈 값은 결과를 [0, 1]로 자른다(음수 lobe의 넘침 제거). float 형식은 자르지 않고 12.2절 5번만 적용한다.

mip 규칙:

- 크기: 각 축 길이 n의 다음 mip 길이는 m = max(1, floor(n / 2))이며 1×1(×1)까지 만든다.
- SimpleAverage(축별 가중치의 곱 — 분리 가능, 2D 최대 3×3, Volume 최대 3×3×3 표본):
  - n = 1: 그 축은 복사(가중치 1).
  - n 짝수: 대상 i는 원본 2i·2i+1을 ½·½.
  - n 홀수(n ≥ 3): 대상 i는 원본 2i·2i+1·2i+2를 (m − i)/n·m/n·(i + 1)/n(다상 3탭 상자 필터 — 모든 원본 texel의 기여가 m/n으로 같아 평균이 보존된다).
- 색은 선형 float에서 평균하고(1번에서 이미 선형화), 알파 가중하지 않는다(UE SimpleAverage). Normalmap(`bNormalizeNormals`)은 평균 후 [−1, 1]로 펴서 재정규화하고(길이 0이면 (0, 0, 1)) 다시 [0, 1]로 접는다.
- Unfiltered: 원본 [2i, 2j(, 2k)] 표본(UE: 2×2(×2) 묶음의 첫 texel). n = 1인 축은 0.
- Cube: 면별 독립 mip(면 경계 연속성은 Angular와 함께 후속).
- PreserveBorder(mip k ≥ 1, 각 slice·면): 바깥 한 줄(첫·끝 행과 열)의 texel을 mip 0 테두리에서 최근접으로 가져온 값으로 덮는다. 길이 m인 변의 위치 j는 mip 0의 같은 변(길이 n)의 위치 clamp(floor((j + 0.5) × n / m), 0, n − 1)를 쓰고, 모서리는 mip 0의 같은 모서리다. 한 축 길이가 1이면 그 축은 덮을 안쪽이 없으므로 mip 0 테두리의 해당 위치 값 하나다. 바뀐 값은 다음 mip의 입력이 된다(5번).
- 정수 반올림은 출력 양자화에서만 한다(12.2절 5번).
- 시험: 4×4·5×3·1×7·3×1·Volume 4×4×4·3×3×3 원본의 mip 픽셀 표, PowerOfTwoMode 6종·Lanczos3 기준값 표, LOD 합성(LODBias·MaxTextureSize·그룹 MaxLODSize·MinLODSize·MaxLODMipCount·NoMipmaps) 표.

### 12.6 GPU 압축 조사

BC·ASTC·ETC 인코더 없음, 렌더러는 비압축만. 쿠킹은 비압축으로 완성하고 프로파일의 압축 지원은 "없음". 압축 인코더는 텍스처 파이프라인 후속 단계(사용자 결정 순서).

## 13. 명령행(Programs/Commandlet)

레지스트리 이름은 단일 토큰이라 모든 명령을 하이픈으로 이은 한 단어로 등록한다(`asset-<대상>-<동사>` 또는 `asset-<동사>`). 미커밋 `import-texture`는 `asset-import`로 대체한다.

```
asset-import <path> <source> [--cube-layout=<배치>|--volume-tile=<W>x<H>] [설정…]
asset-import <path> (--cube=<cube 지정>|--array=<파일 목록>|--cube-array=<cube 지정>;…|--volume=<파일 목록>) [설정…]
asset-reimport <path> [--source=<파일> | --source.<역할>=<파일>…] [설정…]
asset-new <Type> <path> [<속성>=<값>…]
asset-set <path> <속성>=<값> [<속성>=<값>…]
asset-list [--type=<Type>] [--under=<path>]
asset-info <path|AssetId>
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
asset-cook --profile=<id> [<path>…] [--verify-cache]
asset-load-cooked --profile=<id> <AssetId|경로 힌트>
cache-gc --max-bytes=<n> --unused-days=<n>
공통 옵션: --project=<dir>(기본: 현재 디렉터리에서 위로 *.rocksproject 탐색), --wait=<초>(쓰기 잠금 대기, 기본 0)
```

문법:

- 옵션은 값이 있는 `--<이름>=<값>`과 값 없는 bool 플래그 `--<이름>`(`--force`·`--verify-cache`) 두 형태다. 공백으로 나눈 값·짧은 옵션은 없다.
- 설정(`[설정…]`)은 `--<속성>=<값>`, `asset-new`·`asset-set`의 속성은 `<속성>=<값>`이다. 속성 이름은 `ROCKS_PROPERTY(Name="…")`로 노출한 이름(텍스처는 UE C++ 이름, 대소문자 무시)이고 구조체 필드는 `<속성>.<필드>`(예: `SourceColorSettings.ColorSpace=sRGB`)다. 모르는 이름은 `UnknownProperty`.
- 값: enum은 열거자 이름(대소문자 무시, UE 접두사는 붙여도 되고 빼도 된다 — 12.4절), 정수·실수는 10진, bool은 `on`·`off`·`true`·`false`, 문자열은 그대로, 참조는 논리 경로 또는 36자 AssetId(`none` = null), 배열은 쉼표 목록(빈 배열은 빈 값).
- 목록 구분자는 `,`, `--cube-array`의 cube 구분자는 `;`, `--cube`의 면 이름 구분자는 `:`(앞 2자 `px`…`nz`만 면 이름으로 본다). 값 안의 `\,`·`\;`만 이스케이프(→ `,`·`;`)이고 그 밖의 `\`는 글자 그대로다(Windows 경로 `C:\Art\a.png`·UNC `\\server\share\a.png`는 그대로 쓴다). 논리 경로는 경로 정책상 `,`·`;`·`\`를 가질 수 없다. `;`가 든 인자는 셸 구분자이므로 따옴표로 감싼다.
- 위치 인자 원본은 하나뿐이다. `--cube-layout`·`--volume-tile`은 위치 인자 원본과 함께, `--cube`·`--array`·`--cube-array`·`--volume`은 위치 인자 원본 없이 쓴다.
- `asset-reimport`의 원본 재지정: 원본이 하나면 `--source=<파일>`, 여럿이면 역할마다 `--source.<역할>=<파일>`(역할은 5.4절: `px`…`nz`, slice `0`…, CubeArray `<i>.<면>`). 기록에 없는 역할은 `InvalidImportOption`.
- 종료 코드: 0 성공(경고만 있어도 0), 1 실행 실패(진단은 표준 오류에 `<진단 코드>: <path>: <설명>` 한 줄씩 — 임포터가 거부한 옵션 값은 `InvalidImportOption`), 2 사용법 오류(모르는 명령·옵션, 같은 옵션 두 번, 값이 필요한 옵션에 값 없음, 위치 인자 수·조합 오류).
- 쓰기 잠금(8절)을 잡는 명령: `asset-import`·`asset-reimport`·`asset-new`·`asset-set`·`asset-move`·`asset-duplicate`·`asset-delete`·`asset-fix-duplicate-id`·`asset-alias-remove`·`asset-alias-prune`·`asset-orphans-delete`·`asset-index-rebuild`(캐시 기록)·`asset-cook`(쿠킹 동안 에디터 패키지가 바뀌지 않게, 쿠킹 출력 보호). 나머지는 잠금 없이 읽는다(캐시는 읽기만).

서비스 API(`AssetService`, AssetDatabase)는 명령과 1:1 함수이며 에디터가 나중에 같은 함수를 쓴다.

## 14. 검증 시나리오

1. PNG 임포트·저장 → 새 프로세스에서 같은 AssetId·픽셀
2. 원본 PNG 삭제 후 에디터 패키지만으로 쿠킹
3. 원본 변경 재임포트: ID·참조 유지, 해시 갱신, 빌드 캐시 무효화
4. 잘못된 PNG·원본 누락·잘못된 옵션 재임포트 실패 시 기존 패키지 바이트 불변
5. 이동·이름 변경(디렉터리 포함) 후 ID 참조 유지, 별칭 해석·경고·만료
6. 복제본 새 ID·내부 참조 재매핑
7. 중복 ID·깨진 참조·TypeMismatch·UnsupportedCycle 진단, 하드 순환 SCC 로드
8. 인덱스 캐시 삭제 후 재구축, 에디터 밖 이동·삭제 후 재검색, 감시 Overflow·RootLost 복구
9. 빌드 캐시 적중·설정 변경 무효화·툴체인 지문 변경 무효화·손상 복구·두 프로세스 동시 기록·GC
10. mip 크기·대표 픽셀(12.5절 표: sRGB·알파·홀수·Unfiltered·PreserveBorder·FlipGreen·NoAlpha(비압축 결과 불변)·Normalize·Volume·Cube)
11. 쿠킹 패키지만으로 로드, EditorOnly 데이터·필드 부재
12. 잘못된 패키지(magic·CRC·섹션 순서·겹침·범위·크기)·UnsupportedVersion·NewerVersion
13. 쿠킹 재현성(빌드 캐시 빈/찬, 두 번 쿠킹 동일 바이트, Debug·Release 도구 사이 동일 바이트), 부분 쿠킹 병합·전체 쿠킹 정리, 실패 시 게시본 불변
14. 저장 실패 주입 시 기존 보존, 열린 대상 교체, 잠금 Busy, ExternalModification
15. 경로 정책: NFC/NFD 같은 이름, 대소문자 중복, 예약 이름, 길이, CaseCollision
16. 소프트 참조 비자동 로드, 요청 합류·개별 취소, OnReady 중복 AlreadyRegistered, scope Revoke·ScopeClosed, 의존 retention, lease 해제 후 회수
17. PlatformBase A0-P: DirectoryWatcher 8건·파일 내구성 8건 Windows 통과(깊은 작업 디렉터리 포함), Android 빌드, macOS 미검증. SyncDirectory 시험(디렉터리·끝 구분자·현재 디렉터리·디렉터리 link 성공, 파일·끝 구분자 붙인 파일·파일 link WrongType, 없는 경로·파일을 지나는 경로 NotFound, 잘못된 경로 InvalidArgument, wrapper 기본 Unsupported) Windows 통과
18. PNG 적합성: PngSuite 대조
19. UnsupportedSetting(계약 설정 값), NeverCookReferenced, LossySource 거부
20. 다중 출력: 시험 전용 임포터(원본 하나 → 출력 둘)로 출력 대응 유지·사라진 출력 OrphanedOutput 표시·명시 삭제
21. Cube·2DArray·Volume 임포트·쿠킹·로드(원본 구성별)
22. 매니페스트 게시: 교체 전후 reader가 일관된 집합만 보는지, 쿠킹 실패 주입 시 이전 매니페스트 유지, 부분 쿠킹 `IncompatibleCook`
23. 수명: Closing 직후 같은 ID 재요청이 GC 전에 새 로드로 성공(Discard), Closing 대기 중 재요청 revive, 하드 순환 SCC 단위의 해제, 로드 중 의존 단위 보유, 동적 하드 간선의 계수 반영, lease 해제 지연
24. 12.4절 표: 모든 계약 설정에 기본값이 아닌 값 → `UnsupportedSetting`, 숨김 CompressionSettings → `InvalidSetting`, 선형 설정·float 출력 + SRGB on → `SrgbIgnored`, CompressionSettings 15종 × 원본 형식의 쿠킹 형식·swizzle, sRGB 출력 규칙, LOD 그룹 합성·필터 조합
25. 결정적 수학 기준값·CRT 초월 함수 미사용 링크 검사, 서로 다른 호스트·빌드 구성의 쿠킹 바이트 비교
26. 원본 규칙: BMP 32비트 BI_RGB 알파 무시·V5 알파 마스크, TGA 알파 비트·확장 영역(premultiplied), PSD 색 모드·hasRealMergedData·추가 채널, PNG 회색 16비트 + tRNS
27. 매니페스트 대조: 프로파일 불일치 `IncompatibleCook`, 크기·SHA·의존 불일치 `CorruptPackage`, 손상된 게시 파일의 재게시
28. 순환 키: 하드 순환 A↔B·3-node 순환의 cookKey와 Build 순환의 빌드 캐시 키가 정의되고, 구성원 하나를 바꾸면 SCC 전체의 키가 바뀐다. 동적 간선으로 생긴 순환의 단위 병합과 해제, 간선 다중도(필드 둘 중 하나 제거)
29. 저장: 이전 파일의 손상 bulk entry 복사 거부, bulkId 재매핑 후 지연 읽기, 저장 중 편집 시 재연결 생략, 대체 경로(FAT 등)에서 자기 핸들 때문에 실패하지 않음

## 15. PNG 디코더 범위

색 형식 0/2/3/4/6, 비트 깊이 1–16, Adam7, PLTE·tRNS, chunk CRC, zlib(Core `Compression`)·Adler-32, 한 변 16384·해제 크기 = IHDR 계산값. gAMA·cHRM·sRGB·iCCP는 변환하지 않고 sRGB로 보며 sRGB가 아닌 표시가 있으면 `NonSrgbSource` 경고. APNG는 기본 이미지. 시험은 PngSuite(자유 사용 라이선스, `TextureTools/Tests/Fixtures/PngSuite`).

## 16. 임포트·빌드 의존

### 16.1 재임포트

기존 옵션 유지·명시 값만 변경, 잘못된 옵션 `InvalidImportOption`, 임포터 버전이 기록보다 높으면 `ImporterOutdated` 표시(자동 실행 없음), 사라진 출력은 `OrphanedOutput` 표시·`asset-orphans-*`로 처리.

### 16.2 Build 의존 선언

`DependencyDeclarer`(AssetRuntime 인터페이스, 타입별 등록)가 에디터 객체에서 Build 의존(예: Texture2DArray의 원본 Texture2D)을 선언한다. AssetDatabase는 저장 때 이 인터페이스로 목록을 얻어 Dependencies에 기록하고(쿠커를 부르지 않는다 — 의존 방향 유지), AssetCooker는 같은 선언을 빌드 캐시 키·선행 조건에 쓴다.

## 17. 계획 문서 개정 사항

계획 문서에 함께 반영한다.

- 하드 참조 = 다른 패키지 객체로의 `ObjectRef`(에셋 객체 Registry 안), 소프트 = `SoftObjectRef<T>`(AssetId + 객체 경로), World 객체 → 에셋은 `SoftObjectRef` + `AssetLease`(6.3절). §7.1의 `ObjectRef<MaterialAsset>` 예시 수정.
- 패키지당 대표 에셋 ID = 패키지 ID. 쿠킹 패키지 파일 이름은 `<AssetId>-<패키지 SHA-256 앞 16자>`(내용 주소, 4.2절).
- §9.1: "AssetRegistry"는 AssetIndex(에디터 패키지 인덱스, 재검색으로 복원되는 캐시)이고, 별칭 표는 인덱스 안이 아니라 `Config/AssetAliases.json`의 버전 관리 사용자 데이터다(재검색으로 복원되지 않는다, 4.3절).
- 로딩 API 추가: `AssetRequest.expectedType`·`scope`, `LoadBlocking`(도구·초기화 전용), `OnReady(LoadTicket&)` 참조 인자. `OnReady` lane·`ReadyMask`의 이번 구현 범위는 개정이 아니라 11.1절의 `계약` 상태다.
- §9.2 수명: 상주 단위 = 하드 참조 SCC, Closing 대기 중 revive, Pump 안의 Closing → Discard(11.4절). 계획의 발급·Closing 선형화 규칙과 같다.
- 임포트 worker process(§9.1): 장시간 실행 호스트(에디터)를 임포터 실패로부터 보호하는 장치이므로 에디터와 함께 구현한다. 이번 범위의 임포트 호스트는 명령행 프로세스 자체이며(실패해도 원자적 저장으로 에셋 보존), `Importer` 인터페이스는 입력(원본 바이트·옵션)·출력(객체 그래프·bulk)만 주고받아 프로세스 경계로 옮길 수 있게 한다.
- Virtual Asset(§9.4): 원본 bulk의 분리 저장소는 후속. 이번에는 에디터 패키지 안 bulk가 권위 저장소다(빌드 캐시에만 있는 원본은 허용하지 않는다는 원칙은 지킨다).
- 런타임 VFS(APK)는 Android 그래픽·패키징과 함께 후속(쿠킹 매니페스트는 쿠킹 루트 기준 상대 경로만 사용).
- 용어: UE의 DDC(Derived Data Cache)에 해당하는 것을 **빌드 캐시**(BuildCache)라 부른다(사용자 결정). 코드 산출물 캐시는 계획 문서의 `BuildArtifactCache`다. 계획 문서 전체에 반영.
- 반영한 계획 문서 위치: §3.1 모듈 표(AssetRuntime은 빌드 캐시 reader가 아니라 쿠킹 매니페스트 reader), §7.1 예시(`SoftObjectRef<MaterialAsset>`), §9.1 AssetIndex·별칭·worker process, §9.2 `AssetRequest`·수명, §9.4 Virtual Asset 후속·쿠킹 패키지 이름(내용 주소), VFS 후속, A21 위험 항목.

## 18. 범위 밖과 후속

후속(이번 범위에 API도 없음): 에디터 UI, GPU 업로드·텍스처 mip 스트리밍(계획 §9.5), HDR·EXR 디코더·equirect Cube, Cube 면 경계 연속 mip, 임포트 worker process, Virtual Asset, VFS, 예산 LRU 해제, 실제 다중 출력 임포터(FBX·glTF), UE의 다른 텍스처 계열(렌더 타깃 에셋·IES `TextureLightProfile`·`RuntimeVirtualTexture`·미디어·동적 텍스처), 일반 에셋 사용자 데이터(`AssetUserData`), 쿠킹 결과 패키징(배포 위치 배치).

계약(API·스키마는 있고 기본값이 아니거나 호출하면 명시 오류)으로 이번에 두는 것은 3·11·12절 표에 있다: 공유·원격 빌드 캐시 backend, 블록 압축, 12.4절 계약 설정(가상 텍스처·스트리밍·보정·색 관리·압축 설정 포함), `ReadyMask::Render`, `TaskLane::Worker`·`Render`.
