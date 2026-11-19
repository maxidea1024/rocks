# Unreal 교환 형식과 추출 플러그인 설계

기준일: 2026-11-20, 개정 4(UXR5 경계 정책 검사 구현 — 11.1절). 개정 3(13절 요구를 다른 문서에 반영하고 상태 열을 더함, 태그 키 이름). 개정 2(교차 검증 반영 — 18절). UE 에디터에서 텍스처·정적 메시·스켈레톤·스킨 메시·애니메이션 클립·머티리얼을 꺼내 Rocks 에셋으로 가져오는 경로를 정한다. 경로는 세 부분이다. UE 쪽 추출 플러그인 `RocksExport`, 둘 사이의 교환 형식 **Unreal 교환 묶음**, Rocks 쪽 임포터 플러그인 `UnrealExchange`다.

- 배경: 사용자 지시(2026-11-19). 엔진 시험을 위해 UE에서 쓰던 콘텐츠를 Rocks로 가져온다. `.uasset`을 직접 해석하는 방식은 어려움이 많으므로 UE 플러그인이 Rocks가 읽을 수 있는 형태로 추출한다.
- 받는 쪽 자료구조: 텍스처는 [에셋 시스템](AssetSystem.ko.md) 12절, 머티리얼은 [머티리얼 시스템](MaterialSystem.ko.md) 4절, 메시·스켈레톤·애니메이션은 [메시·스켈레톤·애니메이션 에셋](MeshAnimationAssets.ko.md)이 정한다. 이 문서는 추출과 대응만 다룬다.
- UE 근거: UE 5.8.4(`release`, commit `f45729c0b6f0`, 로컬 클론 `C:\github\UnrealEngine`). 경로 약어: `E/` = `Engine/Source/Runtime/Engine`, `EP/` = `E/Public`, `RT/` = `Engine/Source/Runtime`, `Ed/` = `Engine/Source/Editor/UnrealEd`, `Dev/` = `Engine/Source/Developer`.
- 좌표·단위는 UE와 Rocks가 같다(왼손, +Z 위, cm, 도). 교환 경로 어디에도 좌표 변환을 두지 않는다.
- 착수 시점은 사용자가 정한다. Rocks 쪽 임포트는 받는 에셋 타입의 단계(A5 텍스처, MA 메시·애니메이션, MaterialSystem MS3·MS6)가 끝나야 가능하다(14절). UE 플러그인은 Rocks 구현 순서와 무관한 별도 코드다.

## 1. 결정 요약

| 항목 | 결정 | 근거·대안 |
|---|---|---|
| 방식 | UE 에디터 플러그인이 에디터 메모리의 객체에서 원본 데이터를 꺼낸다 | `.uasset` 해석은 UE 직렬화(버전·커스텀 버전·bulk·압축·tagged property)를 Rocks에 다시 만드는 일이다. glTF·FBX는 텍스처 설정·머티리얼 그래프·n-gon·애니메이션 이벤트를 담지 못하고, glTF는 좌표 변환(Y 위, 오른손, m)까지 들어간다(결정 UX-D1) |
| 형식 | 디렉터리 하나: `bundle.json`(목록) + 에셋별 JSON + 내용 주소 blob(`blobs/<sha256>.bin`) | 사람이 읽고 diff할 수 있는 메타데이터와, 크고 중복되는 배열의 분리. 같은 blob은 한 번만 쓴다 |
| 데이터 수준 | "UE 원본": 텍스처 `FTextureSource` 픽셀, 메시 `FMeshDescription`, 애니메이션 데이터 모델, 머티리얼 그래프 | Rocks가 자체 빌드·쿠킹을 하므로 UE 빌드 결과(렌더 버퍼·압축 텍스처·압축 애니메이션)는 원본으로 쓰지 않는다. 축소로 만든 LOD처럼 원본이 없는 것만 보조로 받는다 |
| 손실 원본 텍스처 | UE에 JPEG로 저장된 원본은 기본 거부(`LossySource`), 명시 옵션 `--allow-lossy-origin`이면 받고 ImportRecord에 표시 | 사용자 결정(2026-11-20). UE 5.8은 기본 설정에서 JPEG 바이트를 원본으로 보관한다(4.3절) |
| 메시 법선·tangent | 정적 메시는 기본으로 "UE 준비 원본"(UE 빌드 전처리를 적용한 사본)을 Rocks 원본으로 쓰고 재계산 플래그를 끈다. 옵션으로 순수 원본 + UE 플래그. 스킨 메시는 순수 원본만 | UE와 같은 셰이딩을 시험하려면 UE의 법선·tangent 계산 결과가 필요하다(5.3절). 스킨 메시는 UE에 사본용 공개 함수가 없다(6.2절) |
| 머티리얼 | 그래프 전체를 노드 단위로 추출하고 Rocks 노드로 번역한다. 번역하지 못하면 플러그인이 구운 텍스처로 "구운 표면" 대체를, 그것도 없으면 엔진 기본 머티리얼 인스턴스로 대체한다 | 그래프만으로는 번역 표가 덜 찬 동안 쓸 수 없고, 굽기만으로는 파라미터·인스턴스 구조가 사라진다. Epic GLTFExporter도 "단순하면 직접, 아니면 굽기" 순서다(8.4절) |
| 애니메이션 | 프레임별 평가 Transform(float) + 커브 키 + 이벤트 | UE 5.8 데이터 모델이 회전을 Euler 채널로 저장한다(MeshAnimationAssets 7.2절) |
| 식별 | Rocks AssetId는 새로 발급(무작위 v4). 재임포트는 ImportRecord·태그의 UE 객체 경로(옛 경로 포함)로 기존 에셋을 찾는다 | AssetSystem 4.1절 규칙을 바꾸지 않는다(결정 UX-D4) |
| 해시 | blob은 SHA-256. 플러그인이 자체 SHA-256 구현을 갖는다 | Rocks `BulkDataRef`가 SHA-256이다. UE에는 공개 SHA-256 API가 Experimental 플러그인에만 있다(11.3절) |
| 플러그인 위치 | UE 플러그인은 저장소 최상위 `Extras/UnrealExport/RocksExport/`, Rocks 임포터는 Rocks 엔진 플러그인 `Engine/Plugins/Importers/UnrealExchange/` | UE 플러그인의 `*.Build.cs`는 UE `ModuleRules`라 Rocks 규칙 glob·정책 검사 루트 밖이어야 한다(11.1절). Rocks 임포터는 선택 기능이라 엔진 핵심 계층이 아니라 플러그인에 둔다(10.1절) |

## 2. 범위

| 받는 것 | UE 클래스 |
|---|---|
| 텍스처 | `UTexture2D`·`UTextureCube`·`UTexture2DArray`·`UTextureCubeArray`·`UVolumeTexture` |
| 정적 메시 | `UStaticMesh` |
| 스켈레톤·스킨 메시 | `USkeleton`·`USkeletalMesh` |
| 애니메이션 | `UAnimSequence` |
| 머티리얼 | `UMaterial`·`UMaterialInstanceConstant`·`UMaterialFunction`·`UMaterialFunctionInstance`·`UMaterialParameterCollection` |

범위 밖(추출 시 `UnsupportedAsset` 진단): 레벨·액터·블루프린트, montage·composite·blend space·pose asset, physics asset, 렌더 타깃·미디어·IES·runtime virtual texture, Niagara, landscape, 사운드. Rocks에서 UE로 되돌리는 방향은 없다.

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

### 3.4 `bundle.json`

| 키 | 내용 |
|---|---|
| `formatVersion` | 정수. v1 = 1. Rocks 리더는 자기가 아는 버전만 받는다(하위호환 읽기 없음 — 초기 단계 원칙) |
| `exporter` | `{ "plugin": "RocksExport", "pluginVersion": "1.0.0", "engineVersion": "5.8.4", "engineCommit": "…" }` |
| `projectSettings` | 결과를 바꾸는 UE 프로젝트 설정: `substrate`(bool, 8.3절), `retainJpegFormat`(bool) |
| `environment` | 정보용: 굽기에 쓴 RHI·GPU·드라이버 이름(굽기를 했을 때만) |
| `createdUtc` | 정보용 |
| `assets` | `[{ "objectPath": "/Game/Props/SM_Rock.SM_Rock", "class": "StaticMesh", "file": "assets/Game/Props/SM_Rock.json" }]`, objectPath 순 정렬 |
| `redirects` | `[{ "from": 옛 objectPath, "to": 새 objectPath }]` — 닫힘 수집 중 만난 UE redirector를 풀어 기록(9.2절) |

### 3.5 에셋 JSON 공통

| 키 | 내용 |
|---|---|
| `objectPath` | UE 객체 경로 |
| `class` | 교환 클래스 이름(`Texture2D`·`StaticMesh`·…, UE 클래스 이름에서 `U` 접두사를 뺀 것) |
| `ueClass` | 실제 UE 클래스 경로(파생 클래스 구분용, 예 `/Script/Engine.Texture2D`) |
| `dependencies` | `[{ "objectPath": …, "kind": "hard" | "soft" }]` — 이 에셋 JSON 안에서 참조한 객체 전부 |
| `diagnostics` | 추출 중 플러그인이 낸 진단 `[{ "code", "severity", "message" }]` |
| `settings` | 에셋 설정(종류별, 4–8절). 키는 UE 프로퍼티 이름 그대로다(대소문자 변환 없음 — 11.5절) |
| `data` | 원본 데이터(종류별) |

객체 참조 값은 `{ "objectPath": "/Game/…" }` 형태다. 묶음에 함께 추출되지 않은 객체를 Hard로 참조하면 Rocks 임포트에서 9.3절 규칙으로 처리한다.

## 4. 텍스처

### 4.1 추출

- `UTexture::Source`가 유효해야 한다(동적 텍스처·쿠킹 전용 텍스처는 `Source.IsValid() == false` — E/Classes/Engine/Texture.h:1269-1272). 아니면 `NoSourceData`.
- 픽셀은 `FTextureSource::GetMipData(nullptr)`로 mip 사슬 전체를 한 번 풀고(Texture.h:347, 549-567), (block, layer, mip)별 `FSharedBuffer`를 blob으로 쓴다. 호출마다 다시 푸는 `GetMipData(Out, B, L, M)`(Texture.h:329)는 쓰지 않는다.
- mip 안의 배치는 slice 순서, slice 안은 행 우선·패딩 없음이다(RT/ImageCore/Public/ImageCore.h:234-246). slice 의미: Volume은 depth(mip마다 줄어든다), Array는 층(mip에서 그대로), Cube는 큐브 하나에 6 slice(Cube Array는 6 × N)다(Texture.cpp:3162-3169, 2209-2230).
- **큐브 면 순서.** slice 0–5는 D3D 면 순서(+X·−X·+Y·−Y·+Z·−Z, D3D Y 위 기준)이고 면 안 u·v 방향도 D3D 규약이다(RT/RHI/Public/RHIDefinitions.h:506-515 `ECubeFace`, Dev/TextureCompressor/Private/TextureCompressorModule.cpp:2194-2213). Rocks `TextureSource` 큐브도 같은 순서·방향이므로(AssetSystem 12.3절) 재배치하지 않는다. 참고로 UE는 cubemap 샘플링 때 Y·Z를 맞바꾸므로 UE 월드 축 기준으로는 slice가 +X·−X·+Z·−Z·+Y·−Y에 대응한다. 이 대응은 렌더러가 cubemap을 샘플링할 때 맞춘다(Rocks 좌표도 UE와 같다).
- 위도·경도 큐브맵(`IsLongLatCubemap()`, Texture.h:425-429)은 `longLat: true`로 표시한다. Rocks의 equirect → Cube 변환은 후속이므로(AssetSystem 12.3절) 임포트는 `UnsupportedSourceLayout`이다.
- layer가 2개 이상(다층 가상 텍스처)이거나 block이 2개 이상(UDIM)이면 추출은 하되 Rocks 임포트는 `UnsupportedSourceLayout`이다. UDIM은 가상 텍스처와 함께 다룬다(AssetSystem 12.2절 사용자 결정).

### 4.2 형식 대응

| UE `ETextureSourceFormat`(TextureDefines.h:350-369) | Rocks `TextureSource` 형식 | 변환 |
|---|---|---|
| G8 | G8 | 없음 |
| BGRA8 | RGBA8 | 임포트가 B·R 교환 |
| BGRE8 | RGBA32F | UE `FColor::FromRGBE`와 같은 식으로 해제(지수 0이면 0, 아니면 (m + 0.5) · 2^(e − 136), 알파 1 — RT/Core/Private/Math/Color.cpp:353-369) |
| RGBA16 | RGBA16 | 없음 |
| RGBA16F | RGBA16F | 없음 |
| G16 | G16 | 없음 |
| RGBA32F | RGBA32F | 없음 |
| R16F | R16F | 없음 |
| R32F | R32F | 없음 |

layer의 감마 공간(`GetGammaSpace(Layer)` — Texture.h:400)은 `gammaSpace`(`Linear`·`sRGB`·`Pow22`)로 기록하고, 임포트가 Rocks `SRGB`·`bUseLegacyGamma` 설정 해석(AssetSystem 12.2절 색 해석 1번)과 대조한다. 둘이 다르면 `GammaMismatch` 경고를 내고 UE 설정값을 따른다.

### 4.3 손실 원본

`GetSourceCompression()`(Texture.h:395)이 `TSCF_JPEG`·`TSCF_UEJPEG`이면 원본 자체가 손실 데이터다. UE 5.8은 JPEG 임포트 때 `[TextureImporter] RetainJpegFormat`(기본 True — Engine/Config/BaseEditor.ini:284-288)이면 JPEG 바이트를 원본으로 보관한다. Interchange 경로(Engine/Plugins/Interchange/Runtime/Source/Import/Private/Texture/InterchangeTextureFactory.cpp:717-721)와 옛 팩토리 경로(Ed/Private/Factories/EditorFactories.cpp:2967-3003) 모두 이 설정을 따른다.

- 플러그인: 해제한 픽셀을 그대로 추출하고 `lossyOrigin: "jpeg"`를 기록한다. 원래 JPEG 바이트(`GetBulkDataPayload()`, Texture.h:443-446)는 넣지 않는다.
- Rocks 임포트: 기본은 `LossySource` 오류로 거부한다. `--allow-lossy-origin`을 주면 받고, ImportRecord의 `options`에 그 옵션을, 결과에 `lossyOrigin`을 남기며 `LossyOriginAccepted` 경고를 낸다(사용자 결정 2026-11-20). 옵션이 `options`에 남으므로 재임포트도 같은 판단을 한다. 이 표시는 쿠킹 결과를 바꾸지 않는다.
- `TSCF_None`·`TSCF_PNG`·`TSCF_UEDELTA`는 무손실이다(TextureDefines.h:337-346. UEDELTA는 무손실 delta + Oodle — Texture.cpp:2612-2650).

### 4.4 설정

UTexture·차원별 클래스의 저장 프로퍼티를 이름 그대로 `settings`에 쓴다. Rocks 텍스처 설정은 UE 5.8 이름과 1:1 대조표가 이미 있다(AssetSystem 12.4절). 임포트는 그 표로 옮기고, 표에서 "후속"인 설정이 기본값이 아니면 `UnsupportedSetting` 경고를 낸다. `Texture2DArray`의 `SourceTextures`·`VolumeTexture`의 `Source2DTexture` 같은 에셋 참조는 의존으로 기록한다.

## 5. 정적 메시

### 5.1 추출

LOD i(0 ≤ i < `GetNumSourceModels()`)마다 다음을 모두 기록한다.
- `hasSource`: `IsMeshDescriptionValid(i)`. 참이면 `CloneMeshDescription(i, Out)`(E/Classes/Engine/StaticMesh.h:1635)으로 사본을 얻어 `source`로 쓴다(5.2절).
- 설정: `BuildSettings`(EngineTypes.h:2942-3033), `ReductionSettings`, `ScreenSize`(플랫폼별 값 전체).
- `generated`: UE 판정 `!IsMeshDescriptionValid(i) || IsReductionActive(i)`(E/Private/StaticMesh.cpp:9772-9773). 참이면 UE가 만든 렌더 데이터(`FStaticMeshLODResources`)를 다각형 묶음 = 섹션인 삼각형 메시로 바꿔 `baked`로 함께 쓴다. 원본·축소 설정은 버리지 않는다.

에셋 수준: `StaticMaterials`, `SectionInfoMap`, `Sockets`, `LightMapResolution`, `LightMapCoordinateIndex`, 경계 확장, `LODGroup`, 화면 크기 자동 계산(`GetAutoComputeLODScreenSize()`), Nanite 설정(접근자 `GetNaniteSettings()` — 멤버 직접 접근은 5.7부터 deprecated, StaticMesh.h:720, 744), `MinLOD`, `DistanceFieldReplacementMesh`(의존). 충돌: `GetBodySetup()`의 `AggGeom` 형상 전부, `CollisionTraceFlag`, `bDoubleSidedGeometry`, `LODForCollision`, `ComplexCollisionMesh`. level set 계열은 개수만 진단으로 남긴다. Nanite 고해상도 원본은 v1에서 추출하지 않고 `HiResSourceSkipped` 진단만 낸다.

Rocks 임포트: `hasSource`·`source`·`reduction`은 그대로 옮긴다. `baked`가 있으면 MeshAnimationAssets의 `bakedSource`로 옮긴다. `baked`는 이미 `BuildScale3D`가 적용된 결과이므로, Rocks 빌드가 `bakedSource`를 쓸 때는 그 LOD의 `buildScale`을 (1, 1, 1)로 본다(MeshAnimationAssets 4.2절 생성 LOD 규칙). `GeneratedLodBaked` 정보 진단.

### 5.2 `MeshSource` 기록

```json
"lods": [{
  "hasSource": true,
  "source":   { "counts": {…}, "topology": [ 7개 배열 blob 참조 ], "attributes": [ { "element": "Corner", "name": "Normal", "type": "Vec3", "channels": 1, "data": {blob} }, … ] },
  "prepared": { 같은 구조 } | null,
  "generated": false, "baked": null,
  "build": {…}, "reduction": {…}, "screenSize": { "default": 1.0, "perPlatform": {…} }
}]
```

- 위상과 속성 이름은 MeshAnimationAssets 3.1·3.2·9.1절과 같다. 교환 형식은 위상 배열 7개를 blob 7개로 따로 쓰고, Rocks 임포트가 9.1절 순서로 이어 붙여 `topology` bulk 하나로 만든다. UE 요소 ID는 압축(`Compact`) 순서로 0부터 다시 매겨 쓴다. UE vertex instance는 그대로 Rocks 코너가 된다(공유 포함).
- UE 속성 → Rocks 속성 대응은 MeshAnimationAssets 3.2절 표다. 대응이 없는 UE 속성은 이름·타입을 그대로 보존 속성으로 쓴다(Rocks 빌드는 무시한다).

### 5.3 UE 준비 원본(`prepared`)

플러그인은 사본 하나에 UE 빌더가 하는 전처리를 그대로 적용해 `prepared`로 쓴다. 전처리는 `FMeshDescriptionHelper::SetupRenderMeshDescription`이다(Dev/MeshBuilder/Public/MeshDescriptionHelper.h:23-28, MESHBUILDER_API — 에디터 플러그인이 링크할 수 있다). 멤버 함수이고 생성자가 빌드 설정 포인터를 받으며, 실행 중 그 설정의 `SrcLightmapIndex`·`DstLightmapIndex`를 고친다(MeshDescriptionHelper.cpp:106-121). 그래서 UE 빌더처럼 설정 사본을 넘긴다(StaticMeshBuilder.cpp:933-951).

```cpp
FMeshBuildSettings Settings = SourceModel.BuildSettings;           // 사본 — 원본 에셋 설정을 바꾸지 않는다
FMeshDescriptionHelper Helper(&Settings);
const bool bForNanite = StaticMesh->IsNaniteEnabled();             // UE 빌더와 같은 선택(StaticMeshBuilder.cpp:306)
Helper.SetupRenderMeshDescription(StaticMesh, MeshCopy, bForNanite, /*bNeedTangents*/ true);
```

전처리 순서(MeshDescriptionHelper.cpp:34-130): Compact → 값 정리 → 삼각형 법선·tangent → 겹친 코너 찾기 → 코너 법선·tangent(MikkTSpace 포함) → lightmap UV 생성. 삼각형은 지우지 않는다(퇴화 제거는 정점 버퍼 생성 때다 — StaticMeshBuilder.cpp:1644-1674). `BuildScale3D`도 이 단계에 없고 정점 버퍼 생성 때 적용된다(:1596). 그래서 `prepared`는 순수 원본과 위상이 같고 크기 변환이 들어가지 않는다. 고친 lightmap 번호는 `prepared` 옆에 `preparedBuild`로 기록한다.

Rocks 임포트 모드(`--mesh-normals=`, ImportRecord `options`에 남는다):
- `unreal`(기본): `prepared`를 Rocks 원본으로 쓴다. 이미 계산된 값이므로 `recomputeNormals`·`recomputeTangents`·`generateLightmapUVs`를 끄고 lightmap 번호는 `preparedBuild` 값을 쓴다. UE 원래 플래그는 ImportRecord `options`에 남긴다.
- `rocks`: `source`(순수 원본)와 UE 빌드 설정을 그대로 쓴다. Rocks 빌더가 계산한다.

## 6. 스켈레톤·스킨 메시

### 6.1 스켈레톤

- `GetReferenceSkeleton()`의 원본 계층(raw — 가상 본 제외): 본 이름·`ExportName`·부모 번호·기준 pose(`FTransform`, double — E/Public/ReferenceSkeleton.h:13-23, 110).
- 본별 이동 리타깃 방식(`GetBoneTranslationRetargetingMode`), 가상 본, 소켓, 커브 정보(`ForEachCurveMetaData` — 스켈레톤 AssetUserData `UAnimCurveMetaData`에 있다), 슬롯 묶음, 리타깃 원본 pose, 호환 스켈레톤(E/Classes/Animation/Skeleton.h:317-556).

### 6.2 스킨 메시

- LOD별 원본: `HasMeshDescription(i)`·`CloneMeshDescription(i, Out)`(E/Classes/Engine/SkeletalMesh.h:547-556). 스킨 가중치(`SkinWeights`, 이름 있는 profile), morph 속성을 MeshAnimationAssets 3.2절 형식으로 쓴다. 코너 쪽 morph 법선(UE 이름 `Morph-<이름>`, SkeletalMeshAttributes.cpp:194-212)은 `MorphNormal-<이름>`으로 바꿔 쓴다. morph 원본은 이 속성이다(`UMorphTarget`은 빌드 결과 — Dev/MeshBuilder/Private/SkeletalMeshBuilder.cpp:493-497).
- 메시 자체 계층(`GetRefSkeleton()` raw), 스켈레톤 참조, `FSkeletalMeshLODInfo`·`FSkeletalMeshBuildSettings`·`FSkeletalMaterial`·메시 소켓·`SkinWeightProfiles`.
- **UE 준비 원본 없음.** 스킨 메시 빌더의 공개 API는 `Build(FSkeletalMeshRenderData&, …)` 하나이고(SkeletalMeshBuilder.h:40), 법선·tangent는 MeshDescription이 아니라 `FSkeletalMeshLODModel`에 계산된다(SkeletalMeshBuilder.cpp:295, 586). 그래서 정적 메시식 사본을 만들 수 없다. 스킨 메시는 `rocks` 모드(순수 원본 + UE 빌드 설정)만 지원한다.
- **자동 생성 LOD**(`GetMeshDescription`이 nullptr — SkeletalMesh.h:533-538): `GetImportedModel()`(E/Classes/Engine/SkinnedAsset.h:299)의 `LODModels[i].Sections[s].SoftVertices`에서 정점·영향(`FSoftSkinVertex::InfluenceBones`·`InfluenceWeights`, uint16 × 12 — SkeletalMeshTypes.h:57-73)을 읽어 `baked`로 쓴다. 본 번호는 섹션 지역 번호라 `Section.BoneMap`으로 메시 본 번호로 바꾼다(SkeletalMeshLODModel.h:61, 75). morph는 `UMorphTarget::GetMorphLODModels()[i]`의 `FMorphTargetDelta`(MorphTarget.h:19-25, 239)에서 읽는다. 정점 색은 8비트라 손실이 있다(`BakedColorQuantized` 정보 진단).

## 7. 애니메이션 클립

- 데이터 모델(`GetDataModelInterface()`, E/Classes/Animation/AnimSequenceBase.h:243-291): `GetFrameRate()`, `GetNumberOfFrames()`, `GetNumberOfKeys()`(= 프레임 수 + 1).
- 본 트랙: `GetBoneTrackNames()` 순으로 `GetBoneTrackTransforms(Name, Out)`(IAnimationDataModel.h:189-193)이 준 키 수만큼의 `FTransform`(double)을 float로 바꿔 이동·회전·크기 blob 세 개로 쓴다. 성분의 모든 키가 비트 단위로 같으면 키 1개로 쓴다.
- 커브: `GetFloatCurves()`의 `FRichCurve` 키(`InterpMode`·`TangentMode`·`TangentWeightMode`·시간·값·접선·가중치, E/Classes/Curves/RichCurve.h:80-118)와 외삽·기본값, 플래그. Rocks `TangentCurve`가 같은 필드를 갖는다([커브 데이터](CurveData.ko.md)). transform 커브는 9개 커브로 쓴다.
- 이벤트: `Notifies`의 `GetTriggerTime()`·`GetDuration()`, `TrackIndex`, `NotifyName`, notify 클래스 경로(`Notify` 또는 `NotifyStateClass`의 클래스), `NotifyTriggerChance`, 서버·follower 트리거 플래그. notify 객체의 프로퍼티도 `properties`로 덤프하지만 Rocks v1은 받지 않는다(버리고 경고 — MeshAnimationAssets 7.3절). `AnimNotifyTracks` 이름, `AuthoredSyncMarkers`.
- 설정: MeshAnimationAssets 7.1절 표의 UE 프로퍼티(additive·root motion·interpolation·RateScale·bLoop·PlatformTargetFrameRate·RetargetSource). `RefPoseSeq`는 Hard 의존으로 기록한다.
- animated bone attribute는 개수만 진단으로 남긴다.

## 8. 머티리얼

### 8.1 그래프 추출

`UMaterial::GetExpressions()`(EP/Materials/Material.h:1478-1480)와 `GetEditorComments()`의 노드마다 아래를 쓴다. 쿠킹된 에셋에는 그래프가 없으므로(`UMaterialEditorOnlyData`는 `UCLASS(Optional)` — Material.h:324-325) 미쿠킹 프로젝트에서만 추출할 수 있다. `UMaterialEditorOnlyData`와 expression의 Outer는 그 머티리얼이다(E/Private/Materials/MaterialInterface.cpp:182-185, 1073). 그래서 덤프 범위는 "Outer가 그 머티리얼인 객체"로 정한다.

```json
{ "guid": "…", "class": "MaterialExpressionTextureSampleParameter2D",
  "position": [x, y],
  "inputs":  [ { "name": "UVs", "from": { "node": "<guid>", "output": 0 }, "mask": [r, g, b, a] | null } ],
  "outputs": [ { "name": "RGB", "mask": [1, 1, 1, 0] } ],
  "properties": { … 노드 고유 UPROPERTY 덤프(11.5절) … } }
```

- 입력 열거는 `CountInputs()`·`GetInput(i)`·`GetInputName(i)`(MaterialExpression.h:328-345)다. 5.5에서 deprecated된 `GetInputsView`는 쓰지 않는다. 연결은 대상 노드 GUID(`MaterialExpressionGuid`, :177)와 출력 번호다. reroute는 따라가지 않고 노드 그대로 쓴다(Rocks 쪽 번역이 접는다).
- Custom 노드는 `Code`·`OutputType`·`Inputs`·`AdditionalOutputs`·`AdditionalDefines`·`IncludeFilePaths`를 그대로 쓴다(MaterialExpressionCustom.h:79-101). include 파일 내용은 추출하지 않는다(`CustomIncludeSkipped` 진단).
- 머티리얼 출력: `bUseMaterialAttributes`(Material.h:1066)에 따라 개별 출력 핀(BaseColor·Metallic·…·`FrontMaterial`) 또는 `MaterialAttributes` 핀 하나. 핀마다 연결 또는 미연결 상수(`FMaterialInput<T>`의 `UseConstant`·`Constant` — EP/MaterialExpressionIO.h:158-171)를 쓴다.
- 기본 속성: `MaterialDomain`, `BlendMode`, `ShadingModel`, `OpacityMaskClipValue`, `TwoSided`, `bTangentSpaceNormal`, usage 플래그(`bUsedWith*`), 그 밖의 표면 설정 UPROPERTY.

### 8.2 인스턴스·함수·컬렉션

- 인스턴스: `Parent`, 종류별 override 배열 9종(Scalar·Vector·DoubleVector·Texture·TextureCollection·ParameterCollection·RuntimeVirtualTexture·SparseVolumeTexture·Font — MaterialInstance.h:773-805)의 `ParameterInfo{Name, Association, Index}`·값, static 파라미터(`GetStaticParameters()` — switch·component mask·레이어, MaterialInstance.h:654-663), `BasePropertyOverrides`(MaterialInstanceBasePropertyOverrides.h:12-138). 해석된 최종 값(`GetAllParametersOfType`, MaterialInterface.h:950)도 `resolved`로 함께 쓴다(파라미터 키 순 정렬). Rocks 번역이 부모 사슬을 다시 풀지 않고 결과를 대조할 수 있게 하기 위해서다.
- 함수: 그래프(8.1절과 같은 형식), 입출력 노드, 용도(`Default`·`MaterialLayer`·`MaterialLayerBlend`), `bExposeToLibrary`. 함수 인스턴스: 부모와 override.
- 파라미터 컬렉션: scalar·vector 파라미터 이름·GUID·기본값.

### 8.3 Substrate

UE 5.8은 엔진 기본이 Substrate 꺼짐이지만(`r.Substrate` 기본 0 — RT/RenderCore/Private/RenderUtils.cpp:2068-2072), 5.7 이후 새로 만든 프로젝트에는 켜진 설정이 들어간다(Engine/Source/Editor/GameProjectGeneration/Private/GameProjectUtils.cpp:199-212). Substrate가 켜진 프로젝트에서는 `UMaterial::PostLoad`가 옛 그래프를 메모리에서 Substrate 그래프로 바꾼다(E/Private/Materials/Material.cpp:4629-4643). 이 변환은 옛 핀용 `ShadingModels` 노드, `bUseMaterialAttributes` 머티리얼용 `ConvertMaterialAttributes` 노드(:3829), 도메인별 변환 노드(Decal·LightFunction·PostProcess·UI·VolumetricFogCloud, :3905-4128)를 만든다. 그래서 같은 에셋이라도 프로젝트 설정에 따라 추출 결과가 다르다.

- 플러그인은 에디터 메모리의 그래프를 그대로 추출하고, `bundle.json`의 `projectSettings.substrate`와 머티리얼별 변환 버전(`SubstrateConversionVersion`, Material.h:424)을 기록한다.
- Rocks 번역의 1차 범위(UX4)는 Substrate가 꺼진 프로젝트의 옛 출력 핀 그래프다. Substrate 그래프(변환 노드 `ShadingModels`·`ConvertMaterialAttributes`, `SlabBSDF`, 연산자 노드 — MaterialExpressionSubstrate.h)를 Rocks 표면 모델(MaterialSystem 10.3절 결정 MD1)로 옮기는 일은 UX5로 나눈다. UX5 전에 Substrate 그래프를 만나면 8.5절 대체 규칙을 따른다.

### 8.4 굽기 대체(선택)

추출 옵션 `-BakeMaterials`를 주면 플러그인이 머티리얼마다 `IMaterialBakingModule::BakeMaterials`(Dev/MaterialBaking/Public/IMaterialBakingModule.h:28-37)로 BaseColor·Normal·Metallic·Roughness·Specular·Emissive·Opacity·OpacityMask·AmbientOcclusion을 텍스처로 굽는다. 크기는 호출자가 `PropertySizes`로 주며(MaterialBakingStructures.h:24, 49) 플러그인 기본값은 `-BakeSize`의 기본 `RocksDefaultBakeSize`(= 1024)다. emissive는 `SetEmissiveHDR(true)`(IMaterialBakingModule.h:43)로 HDR(RGBA16F)로 굽고, 나머지는 RGBA8이다. 메시 데이터 없이 정사각 quad 기준으로 굽는다(MaterialBakingStructures.h:120-121). 결과는 그 머티리얼 JSON의 `baked` 아래에 텍스처 원본 형식으로 넣는다. commandlet에서 굽기를 하려면 `-AllowCommandletRendering`이 필요하다(RT/Core/Public/Misc/App.h:400-406).

- Epic GLTFExporter가 같은 선례다. 속성마다 상수·단순 텍스처 샘플이면 직접 쓰고, 아니면 MaterialBaking으로 굽는다(Engine/Plugins/Enterprise/GLTFExporter/Source/GLTFExporter/Private/Tasks/GLTFDelayedMaterialTasks.cpp:1264-1513, 1636-1799).
- 구운 결과는 UV 공간 결과라 월드 위치·카메라·시간에 의존하는 그래프는 맞지 않는다. 대체 수단이며 번역의 목표 품질이 아니다.
- GPU 렌더 결과라 같은 UE 상태라도 GPU·드라이버에 따라 bytes가 다를 수 있다. 결정성 보장에서 제외하고 `environment`에 기록한다(3.2절).

### 8.5 Rocks 쪽 번역

- **번역 표**: UE 노드 클래스 → Rocks 노드(MaterialSystem 5.7절 1차 노드 묶음). 표는 `UnrealExchange`의 데이터(노드 클래스 이름, 핀 이름 대응, 속성 대응, 버전)다.
- **같은 이름 파라미터 병합**: UE는 한 머티리얼 안에서 이름이 같은 파라미터 노드 여러 개를 파라미터 하나로 다룬다. 함수 안 파라미터도 global association이면 이름으로 합친다. Rocks는 같은 scope의 같은 이름을 검증 오류(MaterialSystem 5.4절 V8)로 보고 함수 호출 위치마다 독립 파라미터로 만든다. 번역은 같은 이름·같은 종류의 파라미터를 Rocks 파라미터 노드 하나로 합치고 나머지 사용처를 그 노드에 잇는다. 기본값이 다르면 GUID 순으로 첫 노드의 값을 쓰고 `ParameterDefaultConflict` 경고를 낸다(UE가 어느 값을 쓰는지는 UX4에서 원본으로 확인한다). 함수 안 global association 파라미터는 "공유" 표시를 붙인 전역 scope 파라미터로 옮긴다. 결과 그래프에는 같은 scope의 같은 이름이 남지 않아 V8을 통과한다(MaterialSystem 9.2절 "외부 그래프 번역", UXR4 반영).
- **결과 결정**:
  1. 출력에 닿는 모든 노드가 번역되면 `Material` 그래프로 저장한다.
  2. 번역 못 한 노드가 출력에 닿고 `baked`가 있으면, 엔진 내장 "구운 표면" 부모 머티리얼의 `MaterialInstance`로 만든다(텍스처 파라미터에 구운 텍스처). 구운 텍스처는 같은 임포트의 다중 출력 에셋으로 만든다(ImportRecord `outputs`·`ownerAssetId` — AssetSystem 6.2절 ImportOwner). 원래 그래프는 따로 저장하지 않는다. 원본(묶음 JSON)이 ImportRecord의 원본으로 남는다. `MaterialBakedFallback` 경고.
  3. `baked`도 없으면 엔진 기본 머티리얼을 부모로 둔 `MaterialInstance`로 저장하고 `UntranslatedMaterialNode` 경고(노드 클래스 목록)를 낸다. 자리 노드가 든 그래프로 저장하지 않는 이유: 출력에 닿은 자리 노드는 쿠킹 오류이고(MaterialSystem 5.4절 V12), 쿠킹은 하나라도 실패하면 게시하지 않으므로(AssetSystem 10절) 머티리얼 하나가 프로젝트 쿠킹 전체를 막는다.
- 인스턴스는 부모가 1번으로 번역되었으면 override를 Rocks 파라미터 키로 옮긴다. 부모가 2·3번이면 인스턴스도 같은 결과를 따른다(2번이면 그 인스턴스용 구운 텍스처가 필요하므로 플러그인이 인스턴스도 굽는다).
- 번역 결과 확인: `resolved` 파라미터 값과 Rocks 인스턴스 해석 결과를 대조한다(다르면 `ParameterResolveMismatch` 경고).

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
  - `options`: `objectPath`, `--mesh-normals`, `--allow-lossy-origin`, UE 원래 빌드 플래그(5.3절).
- 태그(Tags 섹션, AssetIndex 검색 키): `UnrealExchange.ObjectPath` = 현재 objectPath, `UnrealExchange.FormerPaths` = 묶음 `redirects`로 알게 된 옛 경로 목록. 임포터가 정하는 태그 키를 AssetIndex가 검색할 수 있어야 한다(요구 UXR2).
- 재임포트: 묶음 에셋의 objectPath, 또는 그 objectPath로 이어지는 `redirects`의 옛 경로를 `UnrealExchange.ObjectPath`·`UnrealExchange.FormerPaths` 태그로 찾는다. 찾으면 그 에셋에 다시 임포트한다(ID 유지). UE에서 이름을 바꿔도 UE redirector가 남아 있으면 같은 Rocks 에셋으로 이어진다. redirector를 지운 뒤의 이름 변경은 새 에셋이 된다.

### 9.3 의존과 순서

- 플러그인: 선택한 에셋에서 시작해 AssetRegistry의 패키지 Hard 의존(`IAssetRegistry::GetDependencies`, `EDependencyQuery::Hard` — RT/AssetRegistry/Public/AssetRegistry/IAssetRegistry.h:517-538)을 따라 범위 안 클래스(2절)의 닫힘을 추출한다(`-Closure`, 기본 켜짐). Soft 의존은 기록만 한다. redirector 패키지는 대상으로 풀고 `redirects`에 기록한다.
- Rocks 임포트 순서: 묶음 전체의 Hard·Build 참조 그래프를 만들고 강한 연결 요소(SCC)로 묶어 위상 정렬한다. 같은 위상 깊이 안은 objectPath 순이다(결정적).
  - Hard 참조만으로 된 SCC(예: 정적 메시 둘이 서로를 complex collision mesh로 가리킴)는 AssetSystem 6.2절이 허용하는 순환이라 한 저장 묶음으로 함께 저장한다.
  - Build 참조가 낀 순환(머티리얼 함수 상호 호출, 인스턴스 부모 사슬 순환)은 `UnsupportedCycle`이다(MaterialSystem 4.4절).
  - 참조 깊이가 `UnrealImportMaxDepth`(= 256)를 넘으면 `UnsupportedCycle`과 같이 실패한다(조작된 묶음 방어).
- 저장 묶음(SCC 하나 또는 에셋 하나) 단위로 원자적으로 저장한다. 하나가 실패하면 그것을 Hard로 참조하는 묶음은 `MissingDependency`로 실패한다. 묶음 전체를 한 transaction으로 묶지 않는다(부분 결과로도 시험 콘텐츠를 쓸 수 있게).
- 묶음에 없는 객체를 Hard로 참조할 때: 같은 objectPath 태그의 Rocks 에셋이 이미 있으면 그것에 잇는다. 없으면 머티리얼·텍스처 참조는 비우고(엔진 기본 머티리얼·텍스처로 대체) `MissingUnrealDependency` 경고, 그 밖의 참조(스켈레톤 등)는 `MissingDependency` 오류다.

## 10. Rocks 쪽 임포터 `UnrealExchange`

### 10.1 플러그인과 모듈

| 항목 | 내용 |
|---|---|
| 위치 | `Engine/Plugins/Importers/UnrealExchange/` — Rocks 엔진 플러그인(빌드 규칙 glob `Engine/Plugins/**`, PluginCatalog). 엔진 핵심 계층(`Engine/Source/Runtime`)에 두지 않는다 |
| 모듈 | `UnrealExchange`(Developer 종류) |
| 책임 | 묶음 읽기·검증(3절), 종류별 대응(4–8절), 머티리얼 번역 표, 경로·ID·순서(9절), 임포트 서비스 호출 |
| 의존 | AssetDatabase·TextureTools·MeshTools·AnimationTools·MaterialCompiler(그래프 검증)·Material·Core(JSON·SHA-256) |

UE 쪽 지식(클래스 이름·프로퍼티 이름·enum 값)은 이 모듈에만 둔다. 에셋 모듈과 도구 모듈은 UE를 모른다. 플러그인을 켜지 않은 프로젝트·Shipping 빌드에는 들어가지 않는다.

### 10.2 명령

```text
asset-import-unreal <묶음 디렉터리> [--root=/Game/Unreal] [--only=<objectPath>...]
                    [--allow-lossy-origin] [--mesh-normals=unreal|rocks] [--dry-run]
```

- `--only`는 지정한 에셋과 그 Hard 닫힘을 가져온다(경로 대응은 9.1절이라 결과가 전체 임포트와 같다).
- `--dry-run`은 검증·대응·경로 계산만 하고 결과 표(만들 에셋, 진단)를 출력한다.
- 결과 보고: 에셋별 성공·실패·경고 수, 머티리얼 번역 결과(1·2·3번 비율), 번역 못 한 노드 클래스 빈도 표. 번역 표를 채울 우선순위를 정하는 데 쓴다.

### 10.3 진단 코드

AssetSystem 4.6절 진단 표에 등록한다(요구 UXR3).

| 코드 | 수준 | 상황 |
|---|---|---|
| `CorruptBundle` | 오류 | blob 크기·SHA-256 불일치, JSON 구문 오류, 필수 키 누락, 배열 길이 불일치, 배치 파싱 실패 |
| `UnsupportedBundleVersion` | 오류 | 모르는 `formatVersion` |
| `UnknownField` | 경고 | 모르는 JSON 키 |
| `UnsupportedAsset` | 경고 | 범위 밖 클래스(2절) |
| `LossySource` / `LossyOriginAccepted` | 오류 / 경고 | 4.3절 |
| `UnsupportedSourceLayout` | 오류 | UDIM·다층·위도경도 큐브 |
| `GammaMismatch` | 경고 | 4.2절 |
| `GeneratedLodBaked` / `BakedColorQuantized` | 정보 | 5.1·6.2절 |
| `UnrealPathRenamed` | 정보 | 9.1절 |
| `UnrealPathTooLong` / `UnrealPathCollision` | 오류 | 9.1절 |
| `MissingUnrealDependency` | 경고 | 9.3절 |
| `UntranslatedMaterialNode` / `MaterialBakedFallback` | 경고 | 8.5절 |
| `ParameterDefaultConflict` / `ParameterResolveMismatch` | 경고 | 8.5절 |

메시·애니메이션 데이터 검증 진단은 MeshAnimationAssets 11절, 텍스처는 AssetSystem 4.6절 코드를 그대로 쓴다.

## 11. UE 플러그인 `RocksExport`

### 11.1 위치와 빌드

```text
Extras/UnrealExport/RocksExport/
  RocksExport.uplugin
  Source/RocksExport/RocksExport.Build.cs       (UE ModuleRules)
  Source/RocksExport/Public|Private/…
  README.md                                     설치(UE 프로젝트 Plugins/에 복사 또는 링크)·사용법
```

- `Extras/`는 Rocks 빌드 규칙 glob(Build/Rules/Rocks.Engine.Rules.csproj:13-16), 정책 검사 루트(Programs/Rocks.Tooling.Checks/PolicyChecks.cs:61-64), C# 서식 루트(FormatCheck.cs:26) 어디에도 들지 않는다. Rocks 빌드와 검사는 이 코드를 보지 않는다.
- **경계 정책 검사 `foreign-engine-boundary`**(요구 UXR5, 구현 2026-11-20 — `PolicyChecks.ForeignEngineBoundary`). 두 엔진의 빌드 파일이 서로의 자리에 들어가는 실수를 막는다. Rocks 모듈을 `Extras/`에 두면 Rocks 빌드가 조용히 빼먹고, UE 플러그인을 Rocks 규칙 경로에 두면 Rocks 규칙 프로젝트가 UE `ModuleRules`를 컴파일하다 원인이 흐린 오류로 멈추기 때문이다.
  - `Extras/` 아래: Rocks 서술자(`.rocksproject`·`.rocksplugin`)가 있으면 실패. 규칙 파일(`*.Build.cs`·`*.Target.cs`)이 Rocks 빌드 네임스페이스(`Rocks.Build`)를 참조하면 그 줄에서 실패.
  - Rocks 규칙 경로(엔진 규칙 프로젝트가 모으는 `Core/Source`·`Engine/Source`·`Engine/Plugins`·`Programs`(C# 도구 `Programs/Rocks.*` 제외)와 템플릿 규칙 프로젝트가 모으는 `Templates`) 아래: UE 서술자(`.uplugin`·`.uproject`)가 있으면 실패. 규칙 파일이 UE 빌드 네임스페이스(`UnrealBuildTool`)를 참조하면 그 줄에서 실패.
  - 네임스페이스 판정은 주석·문자열을 지운 텍스트로 한다(설명 주석에서 상대 엔진 이름을 언급하는 것은 허용). 서술자 확장자는 대소문자를 무시한다. `Extras/`가 없으면 그쪽 검사는 할 일이 없다.
- 코드 규약은 UE 코딩 표준(F·U 접두사, UE 컨테이너)을 따른다. 손으로 쓴 주석은 한국어, 로그·진단 메시지는 영어다(Rocks 규약과 같은 언어 경계).
- 지원 UE 버전은 5.8 하나다. `.uplugin`의 `EngineVersion`을 5.8로 두고, 코드는 `ENGINE_MAJOR_VERSION`·`ENGINE_MINOR_VERSION`이 다르면 `#error`로 멈춘다(메시지 영어).
- 모듈: `Type: "Editor"`, `LoadingPhase: "Default"`. `Editor` 타입은 `GIsEditor`일 때 로드되고 commandlet도 `GIsEditor`를 켜므로 commandlet에서 로드된다(RT/Projects/Private/ModuleDescriptor.cpp:724-742, RT/Launch/Private/LaunchEngineLoop.cpp:2223). 의존 모듈: UnrealEd, AssetRegistry, ToolMenus, ContentBrowser, MeshDescription, StaticMeshDescription, SkeletalMeshDescription, MeshBuilder, MaterialEditor, MaterialBaking, Json, JsonUtilities.

### 11.2 진입점

| 진입점 | 동작 |
|---|---|
| 콘텐츠 브라우저 우클릭 "Export to Rocks…" | `UToolMenus`로 `ContentBrowser.AssetContextMenu.<클래스>` 메뉴를 확장한다(Engine/Source/Editor/ContentBrowser/Private/AssetContextMenu.cpp:407-435). 상위 클래스 메뉴(`Texture`·`StaticMesh`·`SkeletalMesh`·`Skeleton`·`AnimSequence`·`MaterialInterface`·`MaterialFunctionInterface`·`MaterialParameterCollection`)에 한 번씩 등록한다. 선택은 `UContentBrowserAssetContextMenuContext::GetSelectedAssetsOfType`으로 얻는다. 출력 디렉터리와 옵션 대화상자를 띄운다 |
| commandlet `-run=RocksExport` | `URocksExportCommandlet::Main`(E/Classes/Commandlets/Commandlet.h:103). 인자 `-Assets=<경로 또는 /Game/폴더/...>` `-Out=<묶음 디렉터리>` `-Closure`(기본 켬)/`-NoClosure` `-BakeMaterials` `-BakeSize=<n>`. 굽기에는 `-AllowCommandletRendering`도 필요하다. 시작 때 `IAssetRegistry::SearchAllAssets(true)`로 스캔을 끝낸다(IAssetRegistry.h:809) |

UE `UExporter` 계열(콘텐츠 브라우저 Export 대화상자, E/Classes/Exporters/Exporter.h)은 쓰지 않는다(결정 UX-D7).

### 11.3 SHA-256

UE Core의 `FGenericPlatformMisc::GetSHA256Signature`는 구현이 없다(`checkf(false, …)` — RT/Core/Private/GenericPlatform/GenericPlatformMisc.cpp:2019-2023). 공개 SHA-256 API는 Experimental 플러그인 PlatformCrypto(OpenSSL)뿐이고, 그 밖에는 모듈 내부의 OpenSSL 직접 호출과 자체 구현만 있다. Experimental 플러그인이나 ThirdParty OpenSSL 링크에 기대지 않도록 플러그인 안에 FIPS 180-4 SHA-256을 직접 둔다(200줄 안팎). NIST 시험 벡터로 자기 시험을 한다.

### 11.4 숫자 서식

float는 9자리, double은 17자리 유효숫자(`%.9g`·`%.17g`)로 쓴다. 다시 읽었을 때 같은 비트가 된다(최단 표기는 아니다). 같은 상태는 같은 bytes가 된다. Rocks 리더는 Core의 정확한 숫자 파서로 읽는다(A0-C).

### 11.5 UPROPERTY 덤프

노드·notify·설정 덤프는 자체 writer(`FRocksPropertyDumper`)로 한다. `FJsonObjectConverter`를 쓰지 않는 이유는 둘이다. 기본으로 키 첫 글자를 바꾸고(`StandardizeCase` — RT/JsonUtilities/Private/JsonObjectConverter.cpp:449-452), 숫자를 UE writer 서식으로 내보낸다. 규칙:
- 키는 UE 프로퍼티 이름 그대로.
- transient·deprecated 프로퍼티는 건너뛴다.
- `FExpressionInput`은 8.1절 `inputs`로 옮기고 `properties`에서 뺀다.
- 객체 참조는 `{ "objectPath": … }`. Outer가 덤프 대상인 하위 객체만 값으로 펼친다.
- `TMap`·`TSet`은 키 문자열 순 정렬.
- 숫자는 11.4절.

### 11.6 클래스 구성

| 클래스 | 책임 |
|---|---|
| `FRocksExportModule` | 메뉴 등록(`UToolMenus::RegisterStartupCallback`) |
| `URocksExportCommandlet` | 명령행 진입점 |
| `FRocksBundleWriter` | 디렉터리·`bundle.json`·에셋 JSON 쓰기, blob 저장(내용 주소, 이미 있으면 건너뜀), 숫자 서식 |
| `FRocksSha256` | 11.3절 |
| `FRocksTextureExtractor` / `FRocksStaticMeshExtractor` / `FRocksSkeletonExtractor` / `FRocksSkeletalMeshExtractor` / `FRocksAnimSequenceExtractor` / `FRocksMaterialExtractor` | 종류별 추출(4–8절) |
| `FRocksMeshDescriptionWriter` | `FMeshDescription` → 교환 형식 `MeshSource`(정적·스킨 공용) |
| `FRocksPropertyDumper` | 11.5절 |
| `FRocksExportClosure` | AssetRegistry 의존 닫힘·redirector 풀기 |

## 12. 시험

### 12.1 Rocks 쪽(UE 없이)

- 플러그인 안에 손으로 만든 작은 묶음 fixture를 둔다(`Engine/Plugins/Importers/UnrealExchange/Tests/Bundles/`): 텍스처 형식 9종, 큐브·배열·볼륨, 손실 원본(옵션 유무), 정적 메시(n-gon·hard edge·공유 코너·UV 8채널·슬롯 이름 불일치·Hard 순환), 생성 LOD(`baked` + buildScale), 스켈레톤 병합, morph·스킨 profile, 애니메이션(상수 트랙·가상 본·없는 본·additive 기준 클립), 머티리얼(번역 가능·불가·굽기 있음·같은 이름 파라미터), 인스턴스 사슬, redirector, 경로 정리 규칙(`_Shared`·공백·64 byte 초과·예약 이름).
- 변조·실패 주입: blob 잘림·SHA 불일치·크기 불일치, 가변 배치 파싱 실패, JSON 필수 키 누락·타입 오류, 배열 길이와 counts 불일치, 범위 밖 번호, Build 순환, 깊이 초과, 없는 의존, 경로 충돌, `formatVersion` 불일치. JSON·blob 퍼징(리더는 오류로 끝나거나 통과하고, 통과한 것은 임포트가 정상 진단으로 끝난다).
- 결정성: 같은 묶음을 두 번 임포트(다른 스레드 수) → 같은 패키지 bytes. AssetId 발급기를 시험용 결정적 발급기로 바꿔 비교한다. ObjectImports 정렬이 AssetId 순이라 ID만 빼고 비교하면 bytes가 다르기 때문이다. 시험용 발급기 주입 지점은 요구 UXR6이다.
- `--only` 결과와 전체 임포트 결과의 경로·내용 일치.
- 재임포트: 묶음 재추출(같은 objectPath, 또는 redirector로 이어진 새 경로) → 같은 AssetId 유지, 참조 보존.

### 12.2 UE 쪽

- 플러그인 자기 시험(UE Automation): SHA-256 NIST 벡터, 숫자 서식 왕복, 엔진 콘텐츠(`/Engine/BasicShapes/Cube` 등 정적 메시, 엔진 기본 머티리얼, 텍스처) 추출 결과의 형식 검증, 같은 상태 두 번 추출의 bytes 일치(굽기 제외), `prepared` 추출이 원본 에셋 설정을 바꾸지 않음.
- 대조 시험(수동, UX 단계마다): 같은 콘텐츠를 UE와 Rocks에서 그렸을 때의 차이는 화면 마일스톤 이후 렌더 세션과 정한다.

## 13. 다른 문서에 반영할 요구

| ID | 대상 | 요구 | 상태 |
|---|---|---|---|
| UXR1 | [에셋 시스템](AssetSystem.ko.md) 12.2절 | 무손실 원본 원칙의 예외: Unreal 교환 경로에서 `--allow-lossy-origin`으로 받은 손실 원본과 `LossyOriginAccepted`, ImportRecord `lossyOrigin` 표시 | 반영(AssetSystem 개정 10, 12.2절) |
| UXR2 | 에셋 시스템 7절(AssetIndex)·5.2절 Tags | 임포터가 정하는 태그 키(`UnrealExchange.ObjectPath`·`UnrealExchange.FormerPaths`)를 기록하고 검색할 수 있게 한다 | 반영(AssetSystem 개정 10, 7절 — 임포터 키는 `<임포터 이름>.` 접두) |
| UXR3 | 에셋 시스템 4.6절 | 10.3절 진단 코드 등록 | 반영(AssetSystem 개정 10, 4.6절) |
| UXR4 | [머티리얼 시스템](MaterialSystem.ko.md) 5.4·9.2절 | 같은 이름 파라미터 병합·함수 global 파라미터 끌어올리기(8.5절)를 번역 결과가 V8을 통과하는 형태로 받는지 확인. 엔진 내장 "구운 표면" 부모 머티리얼 정의(엔진 콘텐츠 `/Engine` 마운트는 후속이므로 그 전까지 두는 곳 포함) | 반영(MaterialSystem 개정 6, 9.2·11.3절) |
| UXR5 | 정책 검사(Rocks.Tooling.Checks) | `Extras/` 아래에 Rocks 모듈 규칙·서술자가 없음을 확인 | 구현(2026-11-20, 11.1절 `foreign-engine-boundary`) |
| UXR6 | 에셋 시스템 시험 기반 | 결정적 AssetId 발급기 주입 지점(시험 전용) | 반영(AssetSystem 개정 10, 14.1절) |
| UXR7 | [빌드 대상](BuildTargets.ko.md) | 첫 엔진 플러그인 `Engine/Plugins/Importers/UnrealExchange`의 서술자·종류(Developer)·Shipping 제외 | 반영(BuildTargets 개정 3, 3.6절) |

## 14. 단계

ID는 이 문서가 부여한다(UX). 고정 구현 순서를 바꾸지 않으며 착수는 사용자가 정한다.

| 단계 | 쪽 | 내용 | 선행 |
|---|---|---|---|
| UX1 | 둘 다 | 묶음 형식 v1, 플러그인 골격(메뉴·commandlet·writer·SHA-256·숫자 서식·덤프·닫힘·redirector), Rocks 리더·검증·경로 대응·위상 정렬·fixture | A2(AssetDatabase 임포트 프레임워크) |
| UX2 | 둘 다 | 텍스처 추출·임포트(4절) | UX1, A5 |
| UX3 | 둘 다 | 정적 메시 추출(`source`·`prepared`·생성 LOD·충돌)·임포트 | UX1, MeshAnimationAssets MA2–MA3 |
| UX4 | 둘 다 | 머티리얼·인스턴스·함수·컬렉션 추출, 옛 출력 핀 그래프 번역 표 1차, 파라미터 병합, 대체 규칙 3번 | UX2, MaterialSystem MS3·MS6 |
| UX5 | 둘 다 | Substrate 그래프 번역(8.3절) | UX4 |
| UX6 | 둘 다 | 굽기 대체(8.4절)와 "구운 표면" 내장 머티리얼, 대체 규칙 2번 | UX4 |
| UX7 | 둘 다 | 스켈레톤·스킨 메시(6절, 생성 LOD 복원 포함) | UX3, MA4–MA5 |
| UX8 | 둘 다 | 애니메이션 클립(7절) | UX7, MA6 |

## 15. 결정 사항

| ID | 결정 | 버린 대안 |
|---|---|---|
| UX-D1 | UE 에디터 플러그인 추출 | `.uasset` 직접 해석: UE 직렬화 전체를 다시 구현해야 하고 UE 버전마다 깨진다. glTF: UE 공식 exporter가 있지만 머티리얼은 굽기 위주, 텍스처 설정·n-gon·이벤트가 없고 좌표·단위 변환이 들어간다. FBX: 바이너리 SDK 의존, 머티리얼 그래프 없음 |
| UX-D2 | 자체 교환 형식(JSON + 내용 주소 blob) | Rocks 패키지(`.rasset`)를 플러그인이 직접 쓰기: Rocks 패키지 형식·리플렉션 스키마를 UE 쪽에 복제해야 하고, Rocks 형식이 바뀔 때마다 플러그인이 깨진다. 교환 형식은 UE 데이터 모델 쪽에 붙어 있어 덜 바뀐다 |
| UX-D3 | UE 원본 수준 추출 | UE 빌드 결과 추출: 정밀도·위상·설정을 잃고(MeshAnimationAssets 3.6절), Rocks 자체 빌드·쿠킹을 시험할 수 없다 |
| UX-D4 | AssetId 무작위 + 태그(objectPath·옛 경로)로 재임포트 연결 | objectPath에서 이름 기반 ID 유도: AssetSystem의 ID 규칙(무작위 v4)에 예외가 생기고, Rocks에서 에셋을 복제·옮길 때의 ID 규칙과 충돌한다. 이름 변경 대응은 ID가 아니라 redirector 기록으로 한다 |
| UX-D5 | 정적 메시 기본 모드는 UE 준비 원본 | 순수 원본만: Rocks 법선·tangent 계산이 UE와 비트 단위로 같지 않아(겹친 코너 섞기·임계값) 같은 콘텐츠의 셰이딩 대조가 어렵다 |
| UX-D6 | blob 비압축 | zstd 압축: 시험 콘텐츠 규모에서는 이득보다 플러그인에 압축 라이브러리를 넣는 부담이 크다. 필요하면 `formatVersion`을 올려 더한다 |
| UX-D7 | 자체 메뉴 + commandlet | UE `UExporter`: 한 번의 export가 파일 하나 중심이라 의존 닫힘 묶음과 맞지 않는다 |
| UX-D8 | 손실 원본 기본 거부, 옵션 허용(사용자 결정 2026-11-20) | 항상 거부: UE 콘텐츠 상당수를 쓸 수 없다. 항상 받음: 무손실 원본 원칙이 묵시적으로 무너진다 |
| UX-D9 | 번역 실패 머티리얼은 기본 머티리얼 인스턴스로 대체 | 자리 노드 그래프로 저장: 쿠킹 오류가 프로젝트 전체 게시를 막는다 |
| UX-D10 | 경로 정리는 원래 경로 해시 접미 | 겹칠 때 `_2`·`_3` 번호: 함께 가져오는 집합에 따라 결과가 달라진다 |
| UX-D11 | Rocks 임포터는 엔진 플러그인 | 엔진 Developer 모듈: 선택 기능을 엔진 핵심 계층에 두게 된다 |
| UX-D12 | SHA-256 자체 구현 | UE ThirdParty OpenSSL 링크: 가능하지만 공개 API가 아니라 UE 버전마다 모듈 구성이 바뀔 수 있다. 200줄 구현이 더 안정적이다 |

## 16. 미확인 항목

| 항목 | 확인 시점 |
|---|---|
| UE가 같은 이름 파라미터 노드의 기본값이 다를 때 쓰는 값 | UX4 |
| 정적 메시 정점 버퍼 생성의 퇴화·용접 임계값 상수 값(Rocks 상수로 옮김) | MA2·UX3 |

## 17. 개정 이력

- 개정 1(2026-11-20): 초안. 사용자 지시(2026-11-19 "언리얼에 플러그인을 만들어서 rocks가 인식할수 있는 형태로 추출")에 따라 작성했다. 손실 원본 처리는 사용자 결정(2026-11-20 "기본 거부, 옵션으로 허용")을 반영했다.
- 개정 2(2026-11-20): 교차 검증(설계 반례 리뷰, UE 사실 확인) 반영. 18절.
- 개정 3(2026-11-20): 13절 요구 UXR1–UXR4·UXR6·UXR7을 대상 문서에 반영했다(AssetSystem 10, MaterialSystem 6, BuildTargets 3). AssetSystem 7절의 임포터 태그 접두 규칙에 맞춰 태그 키를 `UnrealExchange.ObjectPath`·`UnrealExchange.FormerPaths`로 바꿨다. UXR5(정책 검사)는 코드라 UX1에서 한다. 13절에 상태 열을 더했다.
- 개정 4(2026-11-20): UXR5를 구현했다. 11.1절에 정책 검사 `foreign-engine-boundary`의 규칙(양방향: `Extras/`의 Rocks 서술자·Rocks 규칙 참조, Rocks 규칙 경로의 UE 서술자·UE 규칙 참조)을 적었다.

## 18. 교차 검증 기록(개정 2)

리뷰어 두 명이 병렬로 봤다. 설계 반례 리뷰(초안과 기존 스펙·코드 대조)와 UE 5.8.4 사실 확인(주장 약 170개 대조, 문제 27건, 미확인 5건 결론)이다. 이 문서에 해당하는 지적과 조치는 다음과 같다.

| 지적 | 심각도 | 조치 |
|---|---|---|
| 경로 대응이 AssetSystem 4.2절(첫 문자 L*·N*, 64·256 byte)을 어김(`_Engine`, `_` 치환), 플러그인 마운트 대응 없음 | 차단 | 마운트를 첫 구성 요소로, 정리 규칙 1–5, 길이·충돌 오류(9.1절) |
| UE 준비 원본이 퇴화 삼각형을 지운다는 서술이 틀림 | 중요 | 지우지 않음을 명시, 위상이 원본과 같음(5.3절) |
| 생성 LOD를 렌더 데이터로 바꾸며 원본·축소 설정을 버림, 렌더 데이터에 이미 적용된 BuildScale3D를 다시 곱함 | 중요 | `source`·`reduction` 항상 기록, `baked`는 보조, 사용 때 buildScale 1(5.1절) |
| 번역 실패 결과 3번이 쿠킹 전체를 막음, MaterialSystem 11.3절 인용이 틀림 | 중요 | 기본 머티리얼 인스턴스로 대체(8.5절, UX-D9) |
| "ImportRecord 옆 보조 파일"이 AssetSystem에 없는 저장소, 구운 텍스처의 다중 출력 규칙 누락 | 중요 | 보조 파일 삭제, ImportRecord `outputs`·`ownerAssetId`(8.5절) |
| ImportRecord 원본 표기가 5.4절 스키마와 다름, AssetIndex가 objectPath로 찾을 수 없음 | 중요 | `sources` role 표기, `options`, 태그(9.2절, UXR2) |
| 같은 단계 안 Hard 참조·SCC·순환을 objectPath 순으로 처리 못 함 | 중요 | 전체 그래프 SCC 위상 정렬, 깊이 상한(9.3절) |
| `_2` 규칙이 처리 집합에 따라 달라짐 | 중요 | 해시 접미(9.1절, UX-D10) |
| UX-D4 근거가 재연결 방식과 맞지 않음, redirector 미처리 | 중요 | redirector 풀기·`redirects`·`UnrealExchange.FormerPaths` 태그, 근거 고침 |
| blob 원소 종류로 가변 길이 배치를 표현할 수 없음 | 중요 | `layout`에 `names`·`boneInfluences`·`sparseVec3`(3.3절) |
| UE 파라미터 이름 병합과 Rocks V8 충돌 | 중요 | 병합·끌어올리기 규칙(8.5절), UXR4 |
| `UStructToJsonObject`의 키 대소문자 변환·숫자 서식 | 중요 | 자체 덤프 writer(11.5절) |
| 굽기 결과가 GPU마다 달라 결정성 보장의 반례 | 중요 | 보장에서 제외, `environment` 기록(3.2·8.4절) |
| `SetupRenderMeshDescription`이 설정의 lightmap 번호를 고침, Nanite 메시 인자 | 중요 | 설정 사본, `bForNanite` 선택, `preparedBuild`(5.3절) |
| 3.2절 "최단 왕복"과 11.4절 모순, NaN 부호 손실 | 사소 | 3.2절 고침 |
| 절 번호 오인용(6.2→5.3, 13.3→9.3), MAT 단계 이름 | 사소 | 고침(MaterialSystem MS3·MS6) |
| notify 속성 보존 서술이 메시 문서와 불일치 | 사소 | 받지 않고 경고로 통일(7절) |
| 위상 blob 7개와 bulk 하나의 대응 | 사소 | 임포트가 이어 붙임(5.2절) |
| 결정성 시험의 AssetId 비교 규칙이 없음 | 사소 | 결정적 발급기 주입(12.1절, UXR6) |
| `--allow-lossy-origin`이 재임포트에 남지 않음 | 사소 | `options`에 기록(4.3절) |
| "다른 문서에 반영할 요구" 절 없음 | 사소 | 13절 |
| 굽기·Substrate를 v1에 모두 넣는 범위 | 사소 | UX5(Substrate)·UX6(굽기)로 분리 |
| Rocks 임포터 위치(핵심 계층) | 사소 | 엔진 플러그인(10.1절, UX-D11) |
| OpenSSL 링크 대안 | 사소 | 검토 후 자체 구현 유지(UX-D12) |
| UE 사실: IsLongLatCubemap·GetGammaSpace 행, BGRE8 해제식, UEDELTA 무손실 근거, RetainJpegFormat의 Interchange 경로, `SetupRenderMeshDescription` 멤버 함수, override 배열 9종, Substrate 변환 노드 범위, 굽기 HDR·크기 기본값, commandlet 렌더 플래그, JsonUtilities 의존, Editor 모듈 로드 근거, SHA-256 공개 API 범위 | 사실 | 해당 줄 고침 |
| 미확인 결론: 큐브 면 순서(D3D 순서), FJsonValueNumber 서식(`%.17g`·비유한값 깨짐), 스킨 빌더 사본 함수 없음, 생성 스킨 LOD 복원 가능, UMaterialEditorOnlyData Outer = 머티리얼 | 사실 | 4.1·3.2·6.2·8.1절에 반영, 미확인 표에서 지움 |
