## 9. 식별·경로·의존

### 9.1 경로 대응

UE 객체 경로 `/<Mount>/<경로>/<이름>.<이름>` → Rocks 논리 경로 `<root>/<Mount>/<경로>/<이름>`. `<root>`는 임포트 옵션 `--root`(기본 `/Game/Unreal`)다. 마운트(`Game`·`Engine`·플러그인 마운트 `Paper2D` 등)를 첫 구성 요소로 그대로 두므로 마운트 사이 이름이 겹치지 않는다.

구성 요소 이름 정리(AssetSystem 4.2절 규칙을 만족시키기 위해):
1. 받지 않는 문자(공백·`.`·기호 등)를 `_`로 바꾼다.
2. 첫 문자가 L*·N*가 아니면(예: `_Shared`) 앞에 `U`를 붙인다.
3. Windows 예약 이름이면 앞에 `U`를 붙인다.
4. 1–3에서 하나라도 바뀌었거나 구성 요소가 `UnrealPathComponentMaxBytes`(= 64 byte)를 넘으면, 정리한 이름을 잘라 `-` + 원래 UE 경로(그 구성 요소까지)의 SHA-256 앞 `UnrealPathHashChars`(= 8)자를 붙여 64 byte에 맞춘다.
5. 전체 경로가 256 byte를 넘으면 `UnrealPathTooLong` 오류(사용자가 `--root`를 줄이거나 UE에서 이름을 줄인다).

이 규칙은 원래 경로만으로 결과가 정해지므로 함께 가져오는 에셋 집합(`--only` 포함)과 무관하게 결정적이다. 정리 뒤에도 경로가 겹치면(비ASCII 대소문자 차이 등 드문 경우) `UnrealPathCollision` 오류다. 바뀐 경우 `UnrealPathRenamed` 정보 진단을 내고 원래 경로를 태그에 남긴다(9.2절).

재임포트에서는 이 대응보다 기존 에셋 위치가 우선한다. 사용자가 Rocks에서 옮긴 에셋도 9.2절 태그로 찾아 그 자리에 다시 임포트한다.

### 9.2 ID와 재임포트

- 새 에셋은 AssetSystem 4.1절대로 무작위 AssetId를 받는다.
- ImportRecord(AssetSystem 5.4절 스키마):
  - `sources`: role `asset`(에셋 JSON — root는 묶음 디렉터리를 등록한 원본 루트, relativePath, sha256, bytes), role `blob:<sha256>`(그 JSON이 참조한 blob마다 하나).
  - `options`: `objectPath`, `--mesh-normals`, UE 원래 빌드 플래그(5.3절).
- 태그(Tags 섹션, AssetIndex 검색 키): `UnrealExchange.ObjectPath` = 현재 objectPath, `UnrealExchange.FormerPaths` = 묶음 `redirects`로 알게 된 옛 경로 목록. 임포터가 정하는 태그 키를 AssetIndex가 검색할 수 있어야 한다(요구 UXR2).
- 위 기록·태그를 쓰는 길은 A2f 임포트 프레임워크에 아직 없다(임포터 태그 UXR8, 가변 원본 역할 UXR9 — 13절). 종류별 임포트(UX2b 이후)가 그 보강 뒤에 쓴다.
- 재임포트: 묶음 에셋의 objectPath, 또는 그 objectPath로 이어지는 `redirects`의 옛 경로를 `UnrealExchange.ObjectPath`·`UnrealExchange.FormerPaths` 태그로 찾는다. 찾으면 그 에셋에 다시 임포트한다(ID 유지). UE에서 이름을 바꿔도 UE redirector가 남아 있으면 같은 Rocks 에셋으로 이어진다. redirector를 지운 뒤의 이름 변경은 새 에셋이 된다.

### 9.3 의존과 순서

- 플러그인: 선택한 에셋에서 시작해 AssetRegistry의 패키지 Hard 의존(`IAssetRegistry::GetDependencies`, `EDependencyQuery::Hard` — RT/AssetRegistry/Public/AssetRegistry/IAssetRegistry.h:517-538)을 따라 범위 안 클래스(2절)의 닫힘을 추출한다(`-Closure`, 기본 켜짐). Soft 의존은 기록만 한다. redirector 패키지는 대상으로 풀고 `redirects`에 기록한다.
- Rocks 임포트 순서: 묶음 전체의 Hard·Build 참조 그래프를 만들고 강한 연결 요소(SCC)로 묶어 위상 정렬한다. 같은 위상 깊이 안은 objectPath 순이다(결정적).
  - Hard 참조만으로 된 SCC(예: 정적 메시 둘이 서로를 complex collision mesh로 가리킴)는 AssetSystem 6.2절이 허용하는 순환이라 한 저장 묶음으로 함께 저장한다.
  - Build 참조가 낀 순환(머티리얼 함수 상호 호출, 인스턴스 부모 사슬 순환)은 `UnsupportedCycle`이다(MaterialSystem 4.4절).
  - 참조 깊이가 `UnrealImportMaxDepth`(= 256)를 넘으면 `UnsupportedCycle`과 같이 실패한다(조작된 묶음 방어).
- 저장 묶음(SCC 하나 또는 에셋 하나) 단위로 원자적으로 저장한다. 하나가 실패하면 그것을 Hard로 참조하는 묶음은 `MissingDependency`로 실패한다. 묶음 전체를 한 transaction으로 묶지 않는다(부분 결과로도 시험 콘텐츠를 쓸 수 있게).
- 묶음에 없는 객체를 Hard로 참조할 때: 같은 objectPath 태그의 Rocks 에셋이 이미 있으면 그것에 잇는다. 없으면 머티리얼·텍스처 참조는 비우고(엔진 기본 머티리얼·텍스처로 대체) `MissingUnrealDependency` 경고, 그 밖의 참조(스켈레톤 등)는 `MissingDependency` 오류다.
