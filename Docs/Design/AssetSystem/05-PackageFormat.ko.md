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
  - export 집합은 Outer 체인이 이 패키지에 닿는 모든 객체다(Outer 표에서 정한다 — 참조 순회로 정하지 않는다). 저장 불가 객체(클래스가 Transient·Deprecated인 객체와 그 부분 트리)는 `IsUnsaveableObject`로 걸러 빼고, 그 객체를 가리키는 참조는 null + 진단으로 저장한다(graph 저장 정책 Exclude — [리플렉션 B9](../ReflectionExtensions.ko.md) 9.2절). 패키지 정책 Registry의 자식 고리가 소속 객체를 붙잡으므로 이 집합은 GC 시점과 무관하다([ObjectFoundations](../ObjectFoundations.ko.md) 1.7절). export index는 1부터(0 = 패키지 자신). 저장 순서는 대표 에셋, 이후 Outer 깊이 우선이며 같은 Outer 안은 이름 순(4.2절 정렬)이다. writer가 이 순서를 정해(`CollectInners(DepthFirst)`) graph v2에 넘기고 graph v2의 localId k = export k다. graph v2 노드 표의 outer 0은 Exports outerIndex 0과 같이 패키지(기준 Outer)다(A0-O — ObjectFoundations 5.1절).
  - export 표의 유일성: export 1이 대표 에셋이고 그것만 Primary 표시·빈 이름·outerIndex 0을 가진다. 나머지 export는 outerIndex ≥ 1(자기 index보다 작다)이고 같은 Outer 안에서 이름이 유일하다. 위반은 `CorruptPackage`.
  - object import 표의 유일성: outerImportIndex가 0이 아니면 자기 index보다 작고 같은 assetId의 이름 있는 항목을 가리켜야 하며, 5.2절 순서(같은 outer 아래 이름 엄격 오름차순)가 (assetId, 객체 경로)의 중복을 막는다. 위반은 `CorruptPackage`.
  - WeakObjectRef 필드는 같은 패키지(그래프) 안 대상만 보존하고 밖 대상은 null + 진단으로 저장한다(weak import 없음 — ReflectionExtensions 5.1절).
  - 패키지 밖 객체로의 참조는 object import다. 다른 패키지에 속하면 import, 어떤 패키지에도 속하지 않으면 저장을 `UnpackagedReference`로 거부한다.
  - Objects 섹션의 참조 값(u32): 0 = null, 1–0x7FFFFFFF = export index, 0x80000000 | i = object import index i(1부터). export 수·import 수를 넘는 값과 0x80000000(i = 0)은 `CorruptPackage`. Exports의 outerIndex도 export index다(0 = 패키지).
  - 객체 타입의 원천은 Exports.typeId 하나다. graph v2 노드는 타입을 따로 쓰지 않고 export를 가리키며, 모든 export의 typeId는 Summary의 타입 버전 표에 있어야 한다(없으면 `CorruptPackage`).
- CRC32는 우발적 손상 검출용이며, 콘텐츠 동일성은 SHA-256, 변조 방지는 범위 밖이다.

### 5.2 섹션 내용

| kind | 내용 | flags | 빈 경우 | Editor | Cooked |
|---|---|---|---|---|---|
| Summary(1) | Record: 타입 스키마 버전 표(TypeId → 버전, TypeId 정렬), 클래스 기본값 목록 `ClassDefaults`(레코드 있는 export 클래스마다 저장 당시 지문 — TypeId 정렬, 행 ⊆ export 타입, A5c, RuntimeClassMetadata 'A5c 지문 연결'), 커스텀 버전(Guid → u32), Editor면 savedByEngineVersion(주·부·패치)·compatibleWithEngineVersion(주·부·0, 5.3절 — 쿠킹 패키지에는 없다), Cooked면 프로파일 id·프로파일 출력 설정 SHA-256·쿠커 버전 | Required | 항상 있음 | ○ | ○ |
| Exports(2) | `{ name(대표는 빈 문자열), typeId, outerIndex, flags(Primary·EditorOnly) }` | Required | 항상 있음 | ○ | ○(EditorOnly 제외) |
| ObjectImports(3) | `{ assetId, outerImportIndex(0 = 그 패키지의 대표 에셋을 Outer로 하는 경로의 시작), name(빈 이름 = 대표 에셋), typeId }`. 깊은 하위 객체는 outer import를 거쳐 객체 경로를 이룬다(UE import 표와 같은 방식). 순서: assetId 오름차순, 같은 에셋 안은 깊이 우선(outer는 바로 앞 항목이거나 그 outer 사슬의 항목, 또는 0)이고 같은 outer 아래 이름은 byte 엄격 오름차순(중복 금지 — 대표 에셋 항목은 빈 이름이라 그 에셋의 첫 항목). 이름은 객체 이름 규칙(4.2절 구성 요소)을 따르고 객체 경로는 4 KiB 이하(5.7절 13) | Required | 생략 | ○ | ○ |
| Dependencies(4) | `{ assetId, kind(Hard·Soft·EditorOnly·Build·ImportOwner), expectedTypeId, required }` (assetId, kind) 정렬·중복 제거. Hard·Soft·EditorOnly는 저장 결과의 외부 참조 목록(A0-O)에서, Build는 저장 전에 호출한 `DependencyDeclarer`(16.2절)가 준 목록에서, ImportOwner는 ImportRecord `ownerAssetId`(자기 자신이 아니면)에서 만든다. 로드 때 Hard·Soft·EditorOnly는 Objects 디코딩 결과와, ImportOwner는 ImportRecord와 대조하며(A2 — 5.7절 17, Build는 대조하지 않음) 불일치는 `CorruptPackage` | Required | 생략 | ○ | ○(EditorOnly·Build·ImportOwner 제외) |
| Objects(5) | ObjectSerialization graph v2 바이트 | Required | 항상 있음 | ○ | ○ |
| ImportRecord(6) | Import Metadata(5.4절) | — | 생략(임포트하지 않은 에셋) | ○ | × |
| Tags(7) | 인덱스 검색 태그 Record(타입이 정한 키→값, 키 정렬) | — | 생략 | ○ | × |
| Bulk(8) | entry 본문(bulkAlignment 정렬, PerEntryChecksum) | Required·PerEntryChecksum | 생략(entry 없음) | ○ | ○ |
| BulkIndex(9) | `{ bulkId(1부터 연속), flags(EditorOnly), offset(Bulk 섹션 시작 기준), storedBytes, rawBytes, codec(None=0·Zstd=1 — Zstd 저장 byte는 블록 컨테이너, 5.7절 6), crc32(저장 바이트), sha256(raw) }`. 검증: bulkId 순이며 offset도 증가, entry는 Bulk 섹션 안에서 겹치지 않고 entry 사이에는 bulkAlignment 정렬 패딩(0)만 있다, codec None이면 storedBytes = rawBytes, 쿠킹 패키지의 EditorOnly 표시는 0, Objects의 모든 `BulkDataRef`가 가리키는 bulkId가 있고 rawBytes·SHA-256이 같다 | Required | Bulk와 함께 생략 | ○ | ○ |

### 5.3 버전 축

| 축 | 위치 | 에디터 패키지 | 쿠킹 패키지 |
|---|---|---|---|
| 컨테이너 | `formatVersion` | 현재보다 낮으면 읽으며 마이그레이션, 높으면 `NewerVersion`(해석할 수 없는 컨테이너라 열지 않음) | 정확히 일치, 아니면 `UnsupportedVersion`(다시 쿠킹) |
| 섹션 | `schemaVersion` | 낮으면 마이그레이션, 높으면 그 섹션이 Required면 `NewerVersion`, 아니면 건너뛰고 읽기 전용 | 정확히 일치 |
| 타입 데이터 | Summary 타입·커스텀 버전·클래스 기본값 지문(A5), 마이그레이션 콜백 | 낮으면 마이그레이션, 높거나 모르는 필드가 있으면 읽기 전용으로 열고 저장은 `NewerVersion` 거부. 이 모드에서는 Dependencies 대조를 부분집합 검사로 바꾼다(모르는 필드 안의 참조는 디코딩 결과에 없으므로 "디코딩 결과 ⊆ Dependencies"만 본다). 클래스 기본값 지문이 다르면 "기본값이 바뀐 타입·영향받은 객체"를 진단하고 계속 연다(RuntimeClassMetadata '클래스 기본값 결정') | 정확히 일치(클래스 기본값 지문이 다르면 `UnsupportedVersion`, 다시 쿠킹 — 쿠킹 패키지도 기준 차이 형식이다) |
| 엔진 | Summary savedByEngineVersion·compatibleWithEngineVersion(에디터만, [ReflectionFoundations](../ReflectionFoundations.ko.md) 8.2절) | compatibleWith의 주·부가 현재 엔진보다 새면 `NewerVersion`(`EngineVersion::CanLoadPackageCompatibleWith`가 거짓 — 더 새 엔진이 저장한 패키지). 같거나 오래되면 엔진 버전으로는 거부하지 않고 위 세 축이 판정한다. 패치는 보지 않는다. savedBy는 진단용이다 | 기록하지 않는다(엔진 버전으로 다시 쿠킹을 요구하지 않는다 — 위 세 축과 쿠커 버전이 판정한다) |

버전을 올릴 때 옛 fixture 3개 이상을 읽고 다시 저장하는 시험을 둔다(계획 §7).

타입 축 판정은 등록 저장소 조회 결과로 한다. 미등록(NotFound)과 클래스가 아닌 타입(TypeMismatch)은 "현재 버전 없음"(쿠킹 `UnsupportedVersion`, 에디터 읽기 전용 — `AssetPackage_NonClassTypeHasNoCurrentVersion`)이고, 그 밖의 조회 실패(종료된 저장소의 ShuttingDown 등)는 파일 판정이 아니므로 그 오류를 그대로 돌려준다(진단 코드 None — `AssetPackage_ClosedTypeRegistryIsNotVersionVerdict`). 처음 구현은 OutOfMemory만 전달하고 나머지를 미등록으로 보았는데, 조회는 할당하지 않아 그 분기는 닿지 않았고 ShuttingDown이 버전 판정으로 바뀌었다.

엔진 버전 축(A1): writer는 엔진 버전을 인자로 받는다(호출자는 `build::CurrentEngineVersion()`을 넘기고, 시험은 고정 값을 넘겨 golden이 버전 올림에 흔들리지 않게 한다). savedByEngineVersion은 그 값, compatibleWithEngineVersion은 `CompatibleBaseline()`(주·부·0 — 패치는 데이터 축과 공개 API를 바꾸지 않는다)이다. 쿠킹 패키지 Summary에는 둘 다 기록하지 않는다(UE도 쿠킹·procedural 저장에서 SavedBy·CompatibleWith를 비운다 — 쿠킹 byte·내용 주소 이름의 결정성, 도구 빌드 ID를 뺀 이유와 같다).

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

원본 역할(`role`): 2D `source`, Cube 면 `px`·`nx`·`py`·`ny`·`pz`·`nz`, 2DArray·Volume slice `0`·`1`…, CubeArray `<i>.<면>`. 옵션 경계: 디코딩·원본 변환 옵션 = ImportRecord.options(바뀌면 재임포트), 빌드·쿠킹 설정 = 에셋 저장 속성(바뀌면 재빌드, 너겟 키 변경).

### 5.5 저장

1. 잠금(8절). 대상이 있으면 로드 때 기록한 지문(헤더 + 섹션 표 SHA-256)과 비교, 다르면 `ExternalModification`.
2. 같은 디렉터리에 `<name>.rasset.<guid 36자>.tmp`를 `CreateNew`.
3. Objects를 먼저 메모리에 직렬화한다(그 결과의 외부 참조 목록이 있어야 ObjectImports·Dependencies를 만든다). 그다음 섹션을 물리 순서대로 스트리밍 기록하며 CRC를 증분 계산한다. bulk는 블록 단위로 압축·CRC·SHA-256을 계산하며, 원본은 패키지 bulk 저장소(6.1절 — 메모리 raw 데이터이거나 아직 읽지 않은 entry면 이전 패키지 파일을 경로로 다시 열어 읽는다)다. 이전 파일에서 옮기는 entry는 저장 crc32와 raw SHA-256을 검증하며 다르면 저장을 `CorruptPackage`로 실패시킨다(손상이 새 CRC로 세탁되지 않게). bulkId는 저장마다 객체·필드 순서로 1부터 다시 매기고 writer가 (옛 id → 새 id) 재매핑 표를 만든다. BulkIndex·섹션 표를 쓰고 offset 0으로 돌아가 헤더를 쓴다. 메모리 피크(객체와 이미 상주한 raw 데이터 제외)는 Objects 바이트 + 다른 비-Bulk 섹션 하나 + bulk 블록 하나다(비-Bulk 섹션은 각각 64 MiB 상한).
4. `File::Flush` → Close.
5. 고정 크기 버퍼로 다시 읽어 헤더·표·섹션·entry CRC를 검증한다(writer 버그·짧은 쓰기 검출. 페이지 캐시를 읽으므로 매체 손상 검출이 목적은 아니다).
6. `FileSystem::Replace(temp, target)`(구현, `Platform/File.h` 계약). Windows: `SetFileInformationByHandle(FileRenameInfoEx, REPLACE_IF_EXISTS | POSIX_SEMANTICS)`(다른 쪽이 `FILE_SHARE_DELETE`로 연 대상도 교체 — 이전 파일의 열린 핸들도 계속 유효하다). 이 정보 클래스가 없는 볼륨(FAT·일부 네트워크 FS)은 `MoveFileExW(REPLACE_EXISTING)`로 대신하며, 그 볼륨에서 열린 대상은 `Busy`·`AccessDenied`로 끝나고 대상은 바뀌지 않는다. `ReplaceFileW`는 쓰지 않는다(이전 대상의 속성·스트림을 합치고, `ERROR_UNABLE_TO_MOVE_REPLACEMENT`에서 대상을 지운 채 남긴다). Windows의 이름 변경 매체 기록은 NTFS 메타데이터 로그에 맡긴다(반환 직후 전원 차단 시 이전 상태일 수 있다). POSIX: 부모 디렉터리를 먼저 열고 `rename` 후 그 디렉터리를 `fsync`(macOS `F_FULLFSYNC`). 오류: 원본이 일반 파일이 아니거나 대상이 디렉터리·link면 `WrongType`, 대상이 읽기 전용이거나 권한이 없으면 `AccessDenied`, 공유 위반이면 `Busy`.
   교체 직전에 bulk 저장소가 캐시한 이전 파일 핸들이 있으면 닫는다(POSIX 의미 rename이 없는 볼륨의 `MoveFileExW`는 자기 핸들이 열려 있으면 실패하므로 모든 경로에서 같은 순서로 한다). 교체가 실패하면 이후 저장은 1단계의 지문 비교로 대상이 로드 때의 것인지 다시 본다(다르면 `ExternalModification` — 교체 직후 따로 확인하지 않는다, 7.1절 15 A2c2b).
7. 실패하면 임시 파일을 지우고 보고(기존 보존). `NotDurable`(POSIX 디렉터리 동기화만 실패)은 교체가 이미 보이는 상태이므로 임시 파일 정리 없이 저장 성공으로 처리하고 `NotDurable` 경고를 남긴다. 성공(경고 포함) 후: 저장 세대와 무관하게 패키지 bulk 저장소의 "메모리 id → entry" 표를 재매핑 표(옛 id → 새 id)로 새 파일 entry에 다시 잇는다(아직 읽지 않은 entry도 새 파일에 있다 — 객체의 `BulkDataRef` 필드는 바꾸지 않는다, 7.1절 15). 직렬화를 시작한 세대 = 현재 세대일 때만 dirty를 지운다(저장 중 편집이 있었으면 dirty 유지 — 편집으로 새로 생긴 raw 데이터는 메모리에 있으므로 다음 저장이 쓴다). 마지막으로 인덱스를 갱신. 파일에 쓰는 새 id는 저장 직렬화의 값 치환 hook(B4가 구현한 `SubstituteStruct` — graph v2가 쓴다)이 직렬화 바이트에 쓰고, 같은 옛 id를 가리키는 여러 참조는 같은 새 id가 된다.
8. 잠금 아래에서 발견한 `<name>.rasset.<guid 36자>.tmp` 형태의 파일만 고아로 지운다. 재검색·감시는 `.tmp`를 무시. 교체는 이름만 바꾸므로 새 파일은 이전 대상의 ACL·속성·대체 스트림을 승계하지 않는다(두 플랫폼, 두 경로 모두).

### 5.6 리플렉션·빌드 보강(A0-B 상세)

상세 설계와 단계(B1–B8)는 [리플렉션·빌드 보강 설계](../ReflectionFoundations.ko.md)다. 요약:

- 메타데이터 소유를 타깃에서 모듈로(`NativeModuleInput.Metadata`·`ModuleDirectory`, 모듈별 명세 `<ModuleDir>/<Module>.rocksids.json`, 원본 철자를 따르는 생성 include를 모듈의 public·private include로 전파). 모듈 의존 순서로 모듈마다 한 번 실행하고, descriptor(`<Module>.rocksmeta.json`)를 `--import`해 다른 모듈의 타입을 쓴다. 명세 사이 ID 중복 검사, ID 기준으로 합친 이력·병합 검사(`ids move`), 다중 명세 원자 게시. `TargetRules.Metadata`·`IdentityManifest` 제거, fixture는 `ReflectionTestFixtures` 모듈.
- 값 종류: `ROCKS_ENUM`(열거자 `Name`·`DisplayName`·`Hidden`, 플래그 enum, 미선언 값 거부·로드 정책), `ROCKS_STRUCT`(중첩 Record, 참조 struct는 배열 원소 불가, struct 버전·마이그레이션, Core 수학 타입은 `Binds`), `Array<T>`(단일 값 원소는 `ValueArray` wire, struct 원소는 `RecordList`)·`ObjectRefArray<T>`, `AssetId`, `SoftObjectRef<T>`(AssetId + 객체 경로), `Sha256Digest`.
- 지정자: `Version=N`·`WorldOnly`(상속)·`EditorOnly`·`Required`(저장·쿠킹에서 거부)·`Name="…"`(도구 이름, 텍스처 설정은 UE C++ 이름, 12.4절)·`MaxElements`. `ROCKS_NATIVE`(비영속 멤버, 강한 참조를 재귀적으로 금지, `AssetLease`처럼 WorldOnly 전용 형질을 선언한 타입은 WorldOnly 클래스에만 — 에셋 객체 Registry는 WorldOnly 타입 생성을 거부한다, 11.4절). 영속 데이터는 모두 리플렉션 필드다 — `BulkDataRef`·`TextureSource`·`TexturePlatformData`도 `ROCKS_STRUCT`이므로 네이티브 직렬화 hook을 두지 않는다. `completeFieldLayout`은 "모든 영속 필드와 강한 간선을 기술"이다.
- 필드 codec: 필드별 생성 encode·두 단계 decode thunk와 문맥(참조 wire 매개변수, Soft 참조 수집기, 유효 플래그, struct 관찰·치환 hook, 진단, decode 정책) — graph v2(A0-O)와 패키지(A1)가 같은 thunk를 쓴다.
- 부동소수점: 빌드 전체가 이미 `-ffp-contract=off`이고 fast-math를 쓰지 않으므로 모듈별 모델은 두지 않는다(clang-cl의 `/fp:precise`는 contract=on을 뜻하고 `/WX`에서 옵션 충돌 경고가 오류가 되므로 쓰지 않는다). 결정적 수학 모듈의 fmadd·`long double`·외부 심볼(`llvm-nm`) 검사는 C3에서 모든 구성에 구현되었다([CoreDataFoundations](../CoreDataFoundations.ko.md) 2절). `-fno-math-errno`는 clang-cl이 그 형태를 받지 않아 쓰지 않는다.
- 빌드가 주입하는 상수: 툴체인 지문(너겟 키, 구성·호스트 경로 제외)과 도구 빌드 ID(진단·캐시 무효화용, 입력 내용 해시 기반 — 패키지 Summary에는 넣지 않고 쿠킹 보고서·AssetIndex 캐시 header에 기록)를 Rocks.Build가 실행 파일마다 생성 번역 단위로 넣는다([ReflectionFoundations](../ReflectionFoundations.ko.md) 8.1절·결정 17).
- 엔진 World 타입(Level·Actor·Component)은 `ROCKS_CLASS(WorldOnly)`로 옮긴다(B8).
### 5.7 구현에서 정한 점(A1a — 패키지 포맷 v1)

A1a 구현(`Engine/Source/Runtime/AssetRuntime` — `PackageFormat.h`·`PackageWriter.h`·`PackageReader.h`·`PackageLoad.h`)이 위 절에서 비어 있던 것을 다음과 같이 정했다.

1. **표 섹션의 바이트 배치**: Exports·ObjectImports·Dependencies·BulkIndex는 Record가 아니라 고정 배치다(`u32 count` + 항목, 정수 little-endian, Guid high·low, 문자열은 `u64 길이 + UTF-8`). Exports 항목 = `{name, typeId, outerIndex u32, flags u32}`, ObjectImports = `{assetId, outerImportIndex u32, name, typeId}`, Dependencies = `{assetId, kind u32, expectedTypeId, flags u32(bit 0 required)}`, BulkIndex = `{bulkId u32, flags u32, offset u64, storedBytes u64, rawBytes u64, codec u32, crc32 u32, sha256 32B}`. Summary만 Record다(스키마·필드 Guid는 `Private/PackageInternal.h`, 버전 표는 RecordList `{id, version}`, 클래스 기본값 목록은 RecordList `{id, fingerprint Bytes 32}`(A5c — 정렬·중복 없음·행 ⊆ export 타입, 어기면 Summary `CorruptPackage`), 엔진 버전은 중첩 Record `{주, 부, 패치}`, 프로파일 id는 String·설정 SHA-256은 Bytes 32). 섹션 schemaVersion은 모두 1이다.
2. **패딩·빈 섹션**: 섹션 offset은 정확히 앞 섹션 끝을 정렬한 값이어야 하고(사이 byte는 0), 섹션 표 offset도 마지막 섹션 끝을 8 정렬한 값이다. bytes 0인 섹션은 거부한다 — Bulk만 예외(entry가 모두 0 byte일 때). 존재하는 표 섹션의 count 0도 거부한다.
3. **Exports 순서 검증**: reader가 깊이 우선 순서(outer는 바로 앞 export 또는 그 조상)와 같은 Outer 안 이름 byte 오름차순(같은 byte 이름 중복 포함)을 검사한다. 대소문자·정규화만 다른 중복은 그래프 1단계(형제 유일성)가 잡는다.
4. **객체 단위 EditorOnly**: Exports flags의 EditorOnly(2)는 v1에서 예약이다. 객체를 EditorOnly로 표시하는 클래스 지정자가 없으므로 writer는 0을 쓰고 reader는 켜진 비트를 `CorruptPackage`로 거부한다(지정자가 생기면 schemaVersion을 올린다). 지정자와 비트를 켜는 시점은 EditorOnly 컴포넌트(시각화·편집 도우미)를 들이는 [컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) CR5다. 그때 MetaCompiler 클래스 지정자, writer의 비트 기록, 쿠킹 제외(EditorOnly 객체와 그 객체만 가리키는 참조), schemaVersion 올림을 한 번에 한다.
5. **Dependencies 합성 규칙**(writer와 로드 대조가 같은 함수 `MergeDependencyUses`): 에셋마다 hard 계열(외부 참조 대상 객체)은 EditorOnly가 아닌 쓰임이 하나라도 있으면 Hard, EditorOnly 쓰임만 있으면 EditorOnly, soft 계열도 같다(Soft·EditorOnly). 두 계열이 모두 EditorOnly면 EditorOnly 항목 하나다. required는 그 종류를 정한 쓰임들의 OR다. expectedTypeId는 Hard만 저장 때 대상 패키지 대표 에셋의 타입이고 나머지는 0이다(Soft 대상의 타입은 `RequestAsset`의 expectedType이 판정한다). 로드 대조는 (assetId, kind, required)의 정확한 일치(에디터 읽기 전용은 (assetId, kind) 부분집합)이며 모든 import의 에셋은 Hard 또는 EditorOnly 의존이어야 한다.
6. **bulk**: writer는 저장 그래프의 struct 치환 hook(`SubstituteStruct`)으로 `BulkDataRef`를 첫 출현 순 1부터 다시 매기고 (옛 id → 새 id) 표를 돌려준다. raw byte는 `PackageBulkSource`에서 얻어 크기·SHA-256을 대조한다(다르면 `CorruptPackage`). entry의 EditorOnly 표시는 그 entry의 모든 참조 위치가 EditorOnly일 때다. Zstd entry의 저장 byte는 Compression 모듈의 블록 컨테이너(RCBK, `PackageBulkBlockSizeLog2` = 18 → 256 KiB 블록)다 — writer가 블록 하나씩 압축해 sink에 흘려 쓰고 비워 둔 인덱스 자리를 끝에 `WriteAt`으로 채우므로 압축 메모리 피크는 블록 하나다(5.5절 3, `AssetPackage_BulkBlockCompressionPeak`). 블록마다 압축이 줄이지 못하면 그 블록은 raw로 담긴다(컨테이너 규칙). entry 저장 CRC는 인덱스 CRC와 payload CRC를 이어 붙여(`Crc32Combine` — Core에 더함) 만든다. 0 byte entry는 언제나 None이다. 처음 구현은 entry 전체를 한 번에 압축해 그 크기만큼 메모리를 잡았고, 압축이 줄이지 못하면 entry를 None으로 저장했다(끝까지 압축해 봐야 정해져 흘려 쓸 수 없다). 쿠킹 패키지 v1은 로더가 패키지 전체를 읽으므로(SHA-256 확인) 압축을 푼 raw를 `Package`가 소유하고 `Package::BulkBytes(ref)`로 준다(mip 부분 읽기는 TextureStreaming 단계).
7. **Package**: 하드 의존 패키지 목록(`ObjectRefArray<Package>`)은 두지 않는다 — AssetManager의 상주 단위 표가 의존과 간선 다중도를 가진다. `Package`의 id(Transient·ReadOnly)·종류·bulk는 로드가 Commit 전에 설정하고, 대표 `Asset`의 `assetId`(Transient·ReadOnly)도 같이 설정한다. 로드가 아닌 생성은 `NewPackage(registry, access, id, kind)`다.
8. **진단 모델**: API는 `Result`의 `ErrorCode`, 분류는 `AssetDiagnostic{code, error, asset, related, section, detail}` out 인자다(4.6절 이름표 `AssetDiagnosticCode`). 손상은 `InvalidArgument` + `CorruptPackage`, 버전 축은 `Unsupported` + `UnsupportedVersion`·`NewerVersion`, 할당 실패는 진단 코드 None + 원래 오류다. 그래프 단계 오류는 할당·스레드·종료 오류를 그대로, 미등록 타입(NotFound)·버전(Unsupported)은 `UnsupportedVersion`, 나머지는 `CorruptPackage`로 분류하고 원래 오류를 `error`에 남긴다.
9. **종류 불일치·버전 판정 보강**: 쿠킹을 기대하는 reader가 에디터 패키지를 받으면 `UnsupportedVersion`(다시 쿠킹), 에디터를 기대하는 reader가 쿠킹 패키지를 받으면 `CorruptPackage`다. 쿠킹 패키지의 미등록 export 타입은 `UnsupportedVersion`, 에디터는 reader가 읽기 전용으로 표시한다(로드는 18). 커스텀 버전 표는 쿠킹이면 현재 표와 전체가 같아야 하고 에디터는 높거나 모르는 id면 읽기 전용이다. 쿠킹 로드에서 Required 실효 값 누락은 `CorruptPackage`다(writer가 Required null을 거부하므로).
10. **로드 단계 나눔**: `PackageLoadState`(소유 스레드에서 생성 → `Prepare`는 어느 스레드: 헤더·표·섹션 검증, bulk 해제·SHA-256, graph 1단계, 저장 버전·BulkDataRef·Dependencies 대조) → `InstantiatePackages`(소유 스레드, SCC 묶음 한 트랜잭션: Package·노드 생성 → import 해석 → 적용 → Required → id·bulk 설정) → 호출자 Commit. 묶음 밖 import는 `PackageImportResolver`가 찾는다.
11. **bulk 해제 메모리 상한(설계 메모)**: `DecodePackageBulk`는 BulkIndex의 rawBytes 합만큼 출력 버퍼를 먼저 잡은 뒤 entry마다 풀고 SHA-256을 대조한다. codec None은 stored = raw이고 stored가 파일 안에 있어야 하므로 raw가 파일 크기를 넘지 못한다. Zstd entry는 손상된 rawBytes가 작은 stored로 큰 할당을 요구할 수 있으므로 합의 상한 `PackageLoadOptions::maxBulkRawBytes`(넘으면 `CapacityExceeded` — 자원 한도라 손상이 아니다)가 그 할당을 묶는다. 기본값 16 GiB는 시험·도구용 상한이고, 런타임 로더(A1c)는 매니페스트 항목의 bulkRawBytes(10.1절 5 — 패키지 BulkIndex rawBytes 합)와 11.5절 1번의 임시 버퍼 예산으로 로드 작업마다 이 값을 정한다(예산을 넘는 패키지는 대기 — 같은 절). Zstd entry는 블록 컨테이너 헤더의 rawSize를 BulkIndex rawBytes와 해제 전에 대조한다(다르면 `CorruptPackage`).
12. **할당 실패 분류**: reader·writer·Prepare·InstantiatePackages의 모든 할당 실패는 `OutOfMemory`이고 진단 코드는 None이다(손상으로 분류하지 않는다 — 다시 시도하면 열릴 수 있다). 처음 구현은 Summary 타입 표와 export 타입 집합의 대조에서 할당 실패를 `CorruptPackage`로 돌려줬고 할당 실패 주입 시험(`AssetPackage_AllocationFailures`)이 잡아 고쳤다.
13. **import 표 순서·작업량 상한**(빈틈 리뷰 반영): 처음 구현은 import를 (assetId, 전체 객체 경로 byte) 순으로 정렬해 writer가 초안마다 전체 경로 문자열을 만들고(Outer 사슬마다 반복), reader·로드가 import마다 경로를 처음부터 다시 만들었다(깊이² — 깊이 2040 사슬 아래 잎 6만 개면 수억 단계, 경로 문자열 메모리 수백 MiB). 다음으로 바꿨다.
    - 순서는 5.2절 깊이 우선(같은 outer 아래 이름 byte 순)이다. 경로 byte 순은 '-'(0x2D)가 '.'보다 작아 부분 트리가 끊길 수 있어 한 번 훑기로 검증할 수 없다. 깊이 우선이면 reader가 export 표와 같은 경로 스택으로 항목 수에 비례해 검증하고, 경로 길이는 outer 길이 + 1 + 이름 길이로 이어 센다(문자열을 만들지 않는다).
    - writer는 외부 참조 대상의 Outer 사슬을 이미 초안이 있는 객체에서 멈추고(객체 → 초안 해시 표), 패키지마다 대표 에셋을 한 번 찾으며, (asset, outer 초안, 이름) 정렬 한 번과 깊이 우선 순회로 순서를 정한다. 옛 bulkId → 새 bulkId도 해시 표다.
    - 로드(`InstantiatePackages`)는 에셋마다 패키지를 한 번 찾고, outer import 0이면 `ConstructionContext::FindObject(package, 이름)`, 아니면 앞서 푼 outer import 아래 `ConstructionContext::FindChild(outer, 이름)`(ObjectRuntime에 더한 한 단계 조회 — ObjectFoundations 5.4절)로 푼다. `PackageLoadState::ImportPath`는 없앴다.
    - Prepare 대조(타입 표 하한 탐색, 읽기 전용 부분집합·import ↔ 의존 병합 순회)도 항목 수에 비례한다.
    - import 이름은 객체 이름 규칙(`ValidateNameComponent`)을 reader가 확인한다(`CorruptPackage`). 처음 구현은 로드의 `FindObject`가 `InvalidArgument`로 거부해 진단 코드가 None(할당 실패 분류)으로 나갔다.
    - 다른 패키지의 Package 객체 자체를 가리키는 참조는 저장이 `InvalidArgument`로 거부한다(import 대상은 대표 에셋과 그 아래 객체).
    - 시험 `AssetPackage_ImportScale`이 깊이 2040 사슬 + 잎으로 import 65536개(상한)를 쓰기·읽기·Prepare·Instantiate까지 통과시키고 65537개는 쓰기 `CapacityExceeded`임을 본다.
14. **값 대조 규칙 보강**(빈틈 리뷰 반영):
    - Hard 의존은 expectedType이 0이 아니어야 하고(Hard가 아니면 0 — reader), 그 에셋의 대표 에셋 import(빈 이름)가 있으면 그 타입과 같아야 한다(Prepare — 둘 다 `CorruptPackage`). 로드는 대상 패키지 대표 에셋의 실제 타입이 expectedType의 파생인지 보고 아니면 `TypeMismatch`(section Dependencies), 대표 에셋이 없으면 `MissingDependency`다.
    - 빈 `BulkDataRef`(bulkId 0)는 rawBytes·SHA-256도 0이어야 한다. writer는 저장을 `InvalidArgument`로, 로드는 Objects `CorruptPackage`로 거부한다.
    - 모든 BulkIndex entry는 어떤 `BulkDataRef`가 가리켜야 한다(아니면 BulkIndex `CorruptPackage`). 에디터 읽기 전용 열기는 모르는 필드 안의 ref를 보지 못하므로 이 검사를 하지 않는다.
    - 같은 `PackageLoadState`를 `InstantiatePackages`에 다시 넘기면(앞 호출의 성공·실패와 무관) `InvalidArgument`다. 처음 구현은 그래프 Stage의 순서 오류가 `CorruptPackage`로 분류됐다.

15. **쓰이지 않는 import·정준 byte**(재리뷰 반영):
    - 모든 import는 Objects가 가리키거나 뒤 import의 outer여야 한다(아니면 ObjectImports `CorruptPackage`, detail = 위치). writer는 쓰이는 대상과 그 Outer 사슬만 쓰므로 정상 파일은 늘 지킨다. 에디터 읽기 전용 열기는 모르는 필드 안의 참조를 보지 못하므로 BulkIndex 쓰임 검사(14)와 같이 이 검사를 하지 않는다(`AssetPackage_UnusedImportIsCorrupt`).
    - 에디터 Summary의 compatibleWith는 savedBy의 기준선(주·부·0)과 정확히 같아야 한다(처음 구현은 "기준선 이하"를 받아 같은 내용의 다른 byte를 받았다). 기준선보다 높음(패치·부)과 낮음(부 −1·주 −1 — savedBy 1.2.3에 1.1.0·0.9.0)은 모두 Summary `CorruptPackage`다(`AssetPackage_SummaryFields`의 `CompatPatch`·`CompatNewer`·`CompatOlderMinor`·`CompatOlderMajor`).
    - 퍼징(`AssetPackage_FixedSeedMutationFuzz`)이 받아들인 입력은 읽기 전용이 아니면 파일이 기록한 설정으로 다시 써 입력과 byte가 같아야 한다 — 같은 객체로 풀리는 다른 byte(규칙 구멍)를 체계적으로 잡는다.
16. **resolver 결과와 Prepare의 allocator**:
    - `PackageImportResolver`가 준 handle은 그 에셋의 공개된 `Package`여야 한다. null·회수된 handle·Package가 아닌 객체·다른 AssetId는 진단 None + `InvalidArgument`다(`AssetPackage_ResolverResultIsValidated` — 처음 구현은 조용히 잘못 이었다).
    - `Prepare`(아무 스레드)는 상태 allocator만 쓰고 Registry allocator(소유 스레드 전용 — ObjectRegistry `ObjectAllocator`)는 `InstantiatePackages`(소유 스레드)만 쓴다. 압축을 푼 bulk 저장소는 상태 allocator로 잡고 Package가 복사 없이 이어받는다(bulk가 GB일 수 있어 소유 스레드에서 다시 복사하는 비용을 피한다). 그래서 상태 allocator는 그 상태로 만든 Package(Registry)보다 오래 살아야 한다 — 런타임 로더(A1c)는 엔진 수명의 로더 allocator를 쓴다. 시험은 할당 스레드를 기록하는 allocator로 Registry를 만들어 worker Prepare 동안 Registry allocator가 불리지 않음을 단언한다(`AssetPackage_PrepareUsesOnlyStateAllocator`).
17. **ImportOwner·Tags**: ImportRecord의 내용 스키마(ownerAssetId 포함)는 A2(AssetDatabase)가 정하므로 ImportOwner 의존과 ImportRecord의 로드 대조는 A2로 미룬다(지금은 ImportOwner·Build 의존을 쓰고 읽되 대조하지 않는다 — `AssetPackage_ImportOwnerCrossCheckDeferredToA2`). 여러 ImportOwner 항목과 required의 뜻도 A2가 ImportRecord 스키마와 함께 정한다. Tags는 Record envelope·TLV 구조와 키(필드 ID) 엄격 오름차순을 reader와 writer 옵션 검증이 같이 본다(값 스키마는 A2 — `AssetPackage_TagsKeysAreSorted`). A2는 값 스키마를 문자열 키 → 문자열 목록으로 정하고 이 검증을 바꾸며, ImportOwner 대조는 AssetDatabase가 한다(7.1절 2·7).
18. **에디터 읽기 전용 열기의 기대 결과**: 더 새 데이터(모르는 필드·더 새 타입·커스텀 버전·Required가 아닌 모르는 섹션)를 담은 에디터 파일은 `Prepare`가 읽기 전용으로 받고 `InstantiatePackages`·Commit까지 간다. 모르는 필드는 건너뛰므로 그 안의 Hard·Soft·bulk 참조는 기본값(null·빈 값)으로 남고, Dependencies는 부분집합만, 쓰이지 않는 import·bulk entry는 검사하지 않으며, Package는 bulk entry를 그대로 가진다(`AssetPackage_EditorReadOnlyLoadsEndToEnd`). 현재 클래스가 없는 export 타입(미등록, 또는 등록됐지만 클래스가 아닌 타입 id)은 reader가 읽기 전용으로 표시하고(헤더·인덱스 도구) `PackageContents::unavailableExportType`에 남긴다. 객체를 만들 클래스가 없으므로 `Prepare`는 그래프 1단계 전에 이 reader 판정으로 Summary `UnsupportedVersion`(`Unsupported`)을 낸다. 그래프 1단계의 TypeMismatch로 분류하지 않는 것은 그 오류가 노드 Record 손상에서도 나기 때문이다(처음 구현은 미등록은 그래프 NotFound → `UnsupportedVersion`, 클래스가 아닌 타입은 그래프 TypeMismatch → `CorruptPackage`로 갈렸다 — `AssetPackage_NonClassTypeHasNoCurrentVersion`). 미등록 클래스의 자리표시 객체(UE의 누락 클래스 처리)는 그래프 복원에 노드 생략·참조 null화를 더해야 하는 설계 변경이라 이 단계에서 하지 않는다(5.7절 9의 "에디터는 읽기 전용"은 reader 판정의 뜻이다).
19. **로드의 타입·객체 조회 실패**(판정 리뷰 반영): 로드가 찾은 객체·등록 저장소에 다시 묻는 조회의 실패는 단언하지 않고 진단 None + 원래 오류로 돌려준다.
    - import 대상(대표 에셋·하위 객체)의 `AcquireType`: 단언하지 않고 진단 None으로 그 오류를 돌려준다. 파생 판정은 그 lease(`TypeLease::IsA`)로 해 등록 저장소를 다시 묻지 않는다. 닿는 경로는 `InstantiatePackages` 도중 닫히는 TypeRegistry다: `FindObject`·`FindChild`·`Resolve`는 ObjectRegistry의 종료만 보지만 `AcquireType`은 객체의 타입 lease를 복제하고(`AcquireTypeChecked` → `TypeLease::Clone`), 그 복제는 TypeRegistry가 닫혔으면 `ShuttingDown`이다. `TypeRegistry::Shutdown`은 lease가 남아 `Busy`를 돌려줄 때도 닫힘 표시를 먼저 세운다. 시험 `AssetPackage_TypeRegistryClosesDuringInstantiate`는 상태 allocator가 Prepare 뒤 n번째 할당(첫 할당은 import 해석 전의 대상 표 예약)에서 Shutdown을 부르게 해(모든 n) 결과가 성공이거나 진단 None + `ShuttingDown`임을 보고, resolver 장면(Hard 의존의 대표 에셋 대조)과 묶음 장면(EditorOnly 참조만 — 대조 없이 import 대상 조회) 양쪽에서 ObjectImports 지점에 닿음을 센다. 처음 이 항목은 "지금 엔진에서 닿는 로드 경로가 없다"고 적었는데 이 경로를 놓쳤다. 판정 리뷰가 처음 든 경로(정책 없는 Registry의 `RequestDestroy`만 받은 대상)는 그런 Registry에 패키지를 로드할 수 없어 닿지 않는다 — 패키지는 `packageNativeType = Package`인 Registry에만 로드한다(`InstantiatePackages` 인자 `InvalidArgument` — 정책 없음·다른 타입 모두, `AssetPackage_LoadRequiresPackagePolicyRegistry`; 처음 구현은 그런 Registry로 연 트랜잭션도 받아 그래프 생성 단계가 Objects `CorruptPackage`로 거부했다).
    - Prepare의 대표 에셋 Asset 파생 판정: 그래프 1단계 전에 export 1 타입의 lease를 잡아 그 lease로 판정한다. lease를 잡는 조회가 실패하면(Prepare가 worker에서 도는 동안 `TypeRegistry::Shutdown`이 `closed_`를 먼저 세운 경우의 `ShuttingDown` 등) 그 오류를 그대로 돌려준다(5.3절). 처음 구현은 `types_.IsA`를 그래프 뒤에 불러 그 사이 Shutdown이면 단언이 걸렸다. 시험 `AssetPackage_TypeRegistryClosesDuringPrepare`는 Prepare의 상태 allocator가 n번째 할당에서 TypeRegistry를 닫게 해(모든 n) 결과가 성공이거나 `ShuttingDown`(진단 None)임을 보고, 대표 에셋 lease 지점에도 닿는다.
    - writer 쪽 같은 계열 단언(Admit hook의 `GetOutermost`·`AcquireType`, `ImportBuilder::Add`의 Outer 사슬 `GetOuter`·`AcquireType`)은 안전하다. 그래프 저장이 Exclude 정책에서 외부 참조마다 `WriteUnsaveable`로 대상과 조상을 `AcquireType`으로 먼저 확인하므로 hook·import 빌더가 보는 대상은 방금 생존·타입이 확인된 객체다(같은 소유 스레드, 그 사이 Registry 변경 없음). 대상은 언제나 대표 에셋이거나 그 아래이므로 이 확인이 대표 에셋까지 덮는다: 대표 자식(빈 이름)이 있는 Outer는 이름 있는 자식을 가질 수 없고(정체성 규칙 8 — [ObjectFoundations](../ObjectFoundations.ko.md) 1.5절), 대표 에셋이 없는 패키지의 객체를 가리키면 import 빌더가 대표 에셋 조회에서 `NotFound`로 먼저 끝난다(`AssetPackage_WriterEdgeRejections`). 그래서 export 순회의 "첫 export가 아니면 부모가 export"와 import 빌더의 "사슬이 패키지에 닿기 전에 대표 에셋을 지남"도 단언이다.
