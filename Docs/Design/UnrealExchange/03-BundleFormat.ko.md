## 3. 교환 묶음 형식

### 3.1 디렉터리

```text
<이름>.rxbundle/
  bundle.json                         목록·내보낸 환경
  assets/Game/Props/SM_Rock.json      에셋 하나 = JSON 하나. 경로는 UE 패키지 경로를 그대로 쓴다
  blobs/3f/3fa1…e9.bin                SHA-256(64자 소문자 hex) 이름의 원시 바이트, 앞 2자 하위 디렉터리
```

### 3.2 JSON 규칙

- RFC 8259 JSON, UTF-8, BOM 없음. Rocks Core JSON 읽기로 읽는다(JSON5 아님 — 사람이 쓰는 설정이 아니라 기계가 만드는 파일이다).
- 숫자: 왕복 보장 표기(11.4절 — float는 9자리, double은 17자리 유효숫자)로 쓴다. 비유한값은 JSON 숫자가 될 수 없으므로 문자열 `"nan"`·`"inf"`·`"-inf"`로 쓴다. NaN의 부호·payload는 잃는다(UE 원본 정리 규칙이 어차피 NaN을 바꾼다 — MeshAnimationAssets 3.3절). UE 기본 JSON writer는 쓰지 않는다. `WriteFloat`는 `%g`(6자리)라 왕복이 안 되고 비유한값을 `nan`·`inf`로 내보내 JSON이 깨진다(RT/Json/Public/Policies/JsonPrintPolicy.h:59-75).
- 객체 키 순서는 이 문서의 표 순서, 배열 순서는 의미 순서다. 맵(UE `TMap`·`TSet` 덤프)은 키 문자열의 UTF-8 byte 순으로 정렬한다.
- 결정성: 같은 UE 상태를 두 번 추출하면 bytes가 같아야 한다. 예외는 `bundle.json`의 `createdUtc`·`environment`(3.4절)와 머티리얼 굽기 결과 blob(GPU·드라이버 결과라 bytes가 다를 수 있다 — 8.4절)이다. 둘 다 Rocks 쪽 결정성 키에 쓰지 않는다.
- 모르는 키: Rocks 리더는 경고(`UnknownField`)를 내고 무시한다. 같은 `formatVersion` 안에서 플러그인이 정보를 더할 수 있게 하기 위해서다. 필수 키가 없으면 오류다.

### 3.3 blob 참조

```json
{ "blob": "3fa1…e9", "bytes": 1048576, "layout": "f32", "components": 3, "count": 87381 }
```

- `layout`:
  - 고정 크기 원소: `u8`·`u16`·`u32`·`i32`·`f16`·`f32`·`f64`. `bytes` = 원소 크기 × components × count여야 한다.
  - 가변 길이 배치: `names`·`boneInfluences`·`sparseVec3`. 배치는 MeshAnimationAssets 3.4.1절 표와 같다. `count`는 요소 수다. 리더가 배치를 끝까지 파싱해 `bytes`와 정확히 맞는지 확인한다.
- 모두 little-endian, 패딩 없음. blob은 압축하지 않는다(v1, 결정 UX-D6).
- 리더는 blob을 열 때 크기와 SHA-256을 확인한다. 다르면 `CorruptBundle` 오류다.
- **예약 키**(개정 15): `settings`·`data` 안에서 `blob`은 blob 참조, `objectPath`·`subobject`는 객체 참조(11.5절)의 키다. 리더는 `blob` 키가 있는 객체를 모두 blob 참조로 읽는다. 그래서 플러그인 덤프는 이 셋과 같은 이름(대소문자 구분)의 UE 프로퍼티를 앞에 `@`를 붙여 쓴다(`@blob` — UE 이름에는 `@`를 쓸 수 없다, 11.7절 23번).

### 3.4 `bundle.json`

| 키 | 내용 |
|---|---|
| `formatVersion` | 정수. v1 = 1. Rocks 리더는 자기가 아는 버전만 받는다(하위호환 읽기 없음 — 초기 단계 원칙) |
| `exporter` | `{ "plugin": "RocksExport", "pluginVersion": "1.0.0", "engineVersion": "5.8.4", "engineCommit": "…" }` |
| `projectSettings` | 결과를 바꾸는 UE 프로젝트 설정: `substrate`(bool, 8.3절), `retainJpegFormat`(bool) |
| `environment` | 정보용: 굽기에 쓴 RHI·GPU·드라이버 이름(굽기를 했을 때만) |
| `createdUtc` | 정보용 |
| `assets` | `[{ "objectPath": "/Game/Props/SM_Rock.SM_Rock", "class": "StaticMesh", "file": "assets/Game/Props/SM_Rock.json" }]`, objectPath의 UTF-8 byte 순 엄격 증가(10.4절 6번), `file`은 `assets` + 패키지 경로 + `.json` |
| `redirects` | `[{ "from": 옛 objectPath, "to": 새 objectPath }]` — 닫힘 수집 중 만난 UE redirector를 끝까지 풀어 기록(9.2절). `from` 순 엄격 증가, `from`은 에셋 경로와 겹치지 않고 `to`는 다른 `from`이 아니다(10.4절 10번) |

### 3.5 에셋 JSON 공통

| 키 | 내용 |
|---|---|
| `objectPath` | UE 객체 경로 |
| `class` | 교환 클래스 이름(`Texture2D`·`StaticMesh`·…, UE 클래스 이름에서 `U` 접두사를 뺀 것) |
| `ueClass` | 실제 UE 클래스 경로(파생 클래스 구분용, 예 `/Script/Engine.Texture2D`) |
| `dependencies` | `[{ "objectPath": …, "class": 교환 클래스 이름, "kind": "hard" | "soft" }]` — 이 에셋 JSON 안에서 참조한 객체 전부와 AssetRegistry 패키지 의존의 에셋(11.7절 4번). objectPath 순 엄격 증가(한 객체 한 번, 둘 다면 hard). `class`는 묶음에 없는 대상의 9.3절 분류에 쓴다(10.4절 9번) |
| `diagnostics` | 추출 중 플러그인이 낸 진단 `[{ "code", "severity", "message" }]` |
| `settings` | 에셋 설정(종류별, 4–8절). 키는 UE 프로퍼티 이름 그대로다(대소문자 변환 없음 — 11.5절) |
| `data` | 원본 데이터(종류별) |

객체 참조 값은 `{ "objectPath": "/Game/…" }` 형태다. 묶음에 함께 추출되지 않은 객체를 Hard로 참조하면 Rocks 임포트에서 9.3절 규칙으로 처리한다.
