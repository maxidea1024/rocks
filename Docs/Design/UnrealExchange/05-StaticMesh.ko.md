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

### 5.4 구현에서 정한 점(UX3a)

UX3a(UE 쪽 정적 메시 추출, `FRocksStaticMeshExtractor`·`FRocksMeshDescriptionWriter`) 중 5절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다.

1. **`data` 스키마.** `data`는 `{ "lods": [...], "collision": … }`다. LOD 항목은 `hasSource`·`source`·`prepared`·`preparedBuild`(`{ "SrcLightmapIndex", "DstLightmapIndex" }` — 5.3절 사본 설정이 고친 값)·`generated`·`baked` 순이다. 원본이 없는 LOD의 `source`·`prepared`·`preparedBuild`, 생성 LOD가 아닌 LOD의 `baked`는 `null`이다. 5.2절 예시의 LOD별 `build`·`reduction`·`screenSize`는 `data`에 다시 쓰지 않는다 — `settings`의 `SourceModels[i]`(UE `FStaticMeshSourceModel` 덤프: `BuildSettings`·`ReductionSettings`·`ScreenSize` 기본값과 플랫폼별 값)에 이미 있어 두 곳에 같은 값을 두지 않는다.
2. **MeshSource 기록.** `{ "counts": { "vertices", "corners", "edges", "polygons", "triangles", "polygonGroups" }, "topology": [blob 7개], "attributes": [{ "element", "name", "type", "channels", "data" }] }`다. 위상 blob은 MeshAnimationAssets 9.1절 순서이고 모두 `u32`(components 1·2·1·1·1·3·1, count = 코너 수·변 수·다각형 수 + 1·둘레 길이 합·다각형 수·삼각형 수·삼각형 수)다. 변은 작은 정점 번호를 먼저 쓴다. 요소 번호는 메시 설명 사본을 `Compact`한 순서다. 속성 `element`는 `Vertex`·`Corner`·`Edge`·`Polygon`·`Triangle`·`PolygonGroup`, `type`은 `Float`·`Vec2`·`Vec3`·`Vec4`·`UInt32`·`Bool`·`Name`이다. 값 blob은 MeshAnimationAssets 3.4.1절 배치(채널 우선)이고 배치 이름은 Float·Vec* `f32`(components = 성분 수 × 채널), UInt32 `u32`, Bool `u8`(components = 채널), Name `names`(components = 채널), count = 그 요소 수다. 정점 위치는 속성 `Vertex`·`Position`이다(MA1 3.7절 1). 속성은 (요소 번호, 이름 UTF-8 byte) 순이다(MeshAnimationAssets 9.2절).
3. **속성 대응.** MeshAnimationAssets 3.2절 표대로 UE 이름 상수로 맞춘다: 정점 `Position`(UE 이름은 끝에 공백이 있는 `"Position "` — MeshAttributes.cpp), 코너 `Normal`·`Tangent`·`BinormalSign`·`Color`·`TexCoord`(UE `TextureCoordinate`, 채널 ≤ 8), 변 `IsHard`, 다각형 `PartName`(UE `ObjectName`), 다각형 묶음 `MaterialSlotName`(UE `ImportedMaterialSlotName`). 그 밖의 UE 속성은 같은 요소·이름으로 보존한다(float·FVector2f·FVector3f·FVector4f·bool·FName, int32는 `UInt32`에 비트 그대로). transient 속성(삼각형 법선 등)과 번호 참조 속성(위상)은 쓰지 않는다. 다른 값 타입, Rocks 엔진 이름과 겹치는 보존 속성, 채널이 상한(255, TexCoord 8)을 넘는 속성은 빼고 `SkippedMeshAttribute` 진단(info·warning)을 단다. 채널 0인 속성은 값이 없어 쓰지 않는다. FName `None`은 빈 문자열이다. 이름(속성 이름·Name 값)의 NFC 정규화는 플러그인이 하지 않고 UE 값을 그대로 쓴다. 정규화와 진단은 Rocks 임포트(UX3b)가 한다: Name 값·속성 이름을 NFC로 바꾸고 바뀐 것이 있으면 정보 진단을 낸다(MeshSource는 NFC만 받는다 — MeshAnimationAssets 3.2절). 리더는 Name blob의 내용을 보지 않으므로 비NFC 이름으로 묶음을 거부하지 않는다(개정 15).
4. **`prepared`.** LOD마다 메시 설명 사본(`CloneMeshDescription`)과 빌드 설정 사본에 `FMeshDescriptionHelper::SetupRenderMeshDescription(메시, 사본, IsNaniteEnabled(), bNeedTangents = true)`를 적용하고 다시 압축한다(5.3절). 원본 에셋은 바꾸지 않는다(자기 시험이 확인한다 — 7번). lightmap UV 생성이 켜진 LOD는 `prepared`에 UV 채널이 늘 수 있고 고친 번호가 `preparedBuild`다.
5. **`generated`와 `baked`.** `generated` = `!IsMeshDescriptionValid(i) || IsReductionActive(i)`(5.1절). 참이면 그 LOD의 `FStaticMeshLODResources` CPU 사본(에디터는 쿠킹 데이터를 요구하지 않으므로 `bNeedsCPUAccess`가 켜져 있다 — StaticMesh.cpp)을 삼각형 메시로 바꾼다: 렌더 정점 하나 = 정점 하나 = 코너 하나, 다각형 = 삼각형, 변은 정점 쌍마다 하나(처음 나온 순서), 다각형 묶음 = 섹션(이름은 `StaticMaterials[섹션 머티리얼 번호].ImportedMaterialSlotName`). 코너 속성은 `Normal`(TangentZ)·`Tangent`(TangentX)·`BinormalSign`(TangentZ.w)·`TexCoord`·`Color`(8비트 sRGB를 선형으로 — info `BakedColorQuantized`)다. 같은 정점을 두 번 쓰는 삼각형은 빼고 info `BakedDegenerateTriangles`(개수)를 단다(Rocks 불변식이 같은 정점을 잇는 변을 받지 않는다). 렌더 데이터가 없으면 `baked`는 `null`이고 warning `BakedUnavailable`이다. `baked` 위치에는 `BuildScale3D`가 이미 들어 있다(5.1절).
6. **`settings`와 `collision`.** `settings`는 UStaticMesh UPROPERTY 덤프 전체다(LOD 설정, 슬롯·섹션 정보·소켓, Nanite 설정, lightmap, 경계 확장, LOD 그룹·최소 LOD, `ComplexCollisionMesh`·`DistanceFieldReplacementMesh` 참조 — 참조는 의존에 합친다). UE의 `BodySetup`은 transient UPROPERTY(따로 직렬화)라 덤프에 들지 않으므로 `data.collision`에 BodySetup 객체의 덤프(`AggGeom` 형상 전부, `CollisionTraceFlag`, `bDoubleSidedGeometry`, 물리 머티리얼 등)를 쓴다. 없으면 `null`이다. level set 계열 형상은 info `LevelSetCollision`(개수)이고 Nanite 고해상도 원본은 info `HiResSourceSkipped`다(5.1절). 메시 설명을 기록으로 바꾸지 못하면 그 기록은 `null`이고 error `MeshSourceUnavailable`이다.
7. **자기 시험.** `RocksExport.StaticMeshExtraction`(commandlet `-SelfTest`에도 포함)이 엔진 정육면체·구를 내보내고 LOD 수, LOD 0 원본의 수(압축한 메시 설명과 같음)·위상 배열 7개·위치 blob bytes(메시 설명 값과 같음)·엔진 속성 8종·`prepared`·`preparedBuild`를 확인한다. 추출 전후의 에셋 UPROPERTY 덤프, LOD별 원본 메시 설명 기록의 digest, 빌드 설정 lightmap 번호가 같은지로 "`prepared` 추출이 원본 설정을 바꾸지 않음"(12.2절)을 본다.
8. **Rocks 쪽 왕복 확인.** 선택 시험 `UnrealExchange_ExternalBundle`(11.7절 18번)이 Ready 정적 메시의 `source`·`prepared`·`baked` 기록마다 blob을 `LoadExchangeBlob`으로 읽어 MA1 저장 형태(`MeshSourceData` + `MemoryBulkSource` → 패키지 → `MeshSource::Load` — 12.3절, 개정 15 전에는 Add*·AddAttribute)로 적재하고 Load의 검증을 통과하는지, MA2a `BuildStaticMeshLod`(기본 빌드 설정)가 성공하는지 본다. 이 적재 코드는 시험 안에만 있다. 임포터의 적재·모드 처리는 UX3b다. 수동 검증(2026-11-22): 엔진 BasicShapes 6개와 시험 프로젝트 메시 2개(OBJ로 임포트한 오각기둥 — n-gon 다각형 7개·삼각형 16개, 섹션 2개, 다각형 `PartName`, 볼록 충돌 / 구 사본에 축소 LOD 둘 — `generated`·`baked`)를 닫힘과 함께 내보낸 묶음(에셋 26개)을 두 번 내보내 `createdUtc` 말고 bytes가 같고, 리더가 18개 기록(원본 8·prepared 8·baked 2) 모두 적재·검증·빌드에 성공했다(경고는 시험이 슬롯 이름을 주지 않아 나는 `MaterialSlotFallback`뿐이다).

9. **퇴화 다각형의 자기 변(개정 15).** UE는 둘레에서 같은 정점이 이웃한 다각형에도 변을 만든다(MeshDescription.cpp `CreatePolygon_Internal` — Interchange OBJ·FBX 임포트가 이런 면을 만든다). Rocks 변 목록은 같은 정점을 잇는 변을 받지 않으므로(`EdgeSameVertex`) LOD 기록 전체가 거부됐다. writer는 자기 변을 빼고 남은 변을 UE 순서대로 다시 번호를 매기며, 변 속성(`IsHard` 등)도 같은 번호 표로 옮긴다. 둘레의 퇴화 변은 Rocks 불변식이 변 없이 허용한다(MeshAnimationAssets 3.1절). 뺀 변이 있으면 info `DegenerateEdgesDropped`(개수)다. 그런 다각형의 둘레는 UE `GetPolygonVertexInstances`가 정점 변으로 다시 잇다가 assert하므로(둘레 변 재정렬), 삼각형에 같은 정점이 두 번 나오는 다각형만 코너(vertex instance) 변으로 둘레를 복원한다: 방향 있는 코너 변 중 반대 방향이 없는 것을 첫 삼각형의 둘레 변부터 잇는다. 고리 하나가 되지 않으면 그 기록은 `null`이고 error `MeshSourceUnavailable`이다. 정상 다각형은 UE 호출 그대로라 결과 bytes가 바뀌지 않는다.
10. **원본 복제 실패(개정 15).** `hasSource`가 참인데 `CloneMeshDescription`이 실패하면 그 기록(`source`·`prepared`)은 `null`이고 error `MeshSourceUnavailable`이다(6번 규칙을 복제 실패에도 적용). `baked`(렌더 데이터)로 대신하지 않는다: 원본이 있다는 LOD를 몰래 렌더 결과로 바꾸면 품질 차이를 알 수 없고, 플러그인 error 진단이 있는 에셋은 리더가 받지 않으므로(10.4절 20번) 대체해도 쓰이지 않는다.

### 5.5 Rocks 임포트(UX3b)

설계일 2026-11-27(사용자 지시 "순서대로 진행해" — UX2b 다음). 공통 틀은 10.6절(두 층·원본·옵션·태그·설정 번역)이고, 에셋 내용은 MeshAnimationAssets의 정적 메시 임포트 서비스 `ImportStaticMesh`(5.5.3절 — MA2b3)가 쓴다. 교환 쪽 일은 교환 데이터를 그 서비스의 명세 `StaticMeshImportSpec`으로 옮기는 것이다. UX3b1은 순수 변환(1–9번)과 그 시험, UX3b2는 A2f 임포터·종류별 임포터·제품 연결과 그 시험(10–12번)이다. 모두 권장안으로 정했다.

1. **기록 적재 `LoadExchangeMeshSource`.** MeshSource 기록(5.4절 2번) 하나를 Rocks `MeshSource`로 만든다. 방법은 MA1 저장 형태를 거치는 것이다(시험에만 있던 적재 — 5.4절 8번 — 를 플러그인으로 옮긴다):
   - `MeshSourceData`의 요소 수는 `counts`, 위상 bulk는 위상 blob 일곱을 MeshAnimationAssets 9.1절 순서로 이어 붙인 것, 속성 레코드는 기록 순서(9.2절 정렬과 같다)이고 값 bulk는 blob bytes 그대로다(배치가 같다 — 3.4.1절).
   - bulk는 메모리 bulk 원본(blob byte를 ref로 내주는 `PackageBulkSource`)으로 주고 `MeshSource::Load(data, bulk, names)`를 부른다. 그래서 배치 판정과 `ValidateMeshSource`(본 수 모름)가 패키지 로드와 같다.
   - 이름 정규화(5.4절 3번): 속성 이름과 Name 값 blob의 문자열을 NFC로 바꾼다(Core Unicode). 바뀐 것이 있으면 경고 `UnrealNameNormalized`(새 코드, detail = 에셋의 모든 기록에서 바뀐 이름 수 — 끝에 하나)다. 5.4절 3번은 정보 진단이라 했으나 A2f 문맥은 경고만 남기므로(`AssetImportContext::Warn`) 경고로 정했다. MeshSource는 NFC만 받는다. 바뀐 이름이 9.2절 순서(요소, 이름 byte)를 바꾸면 레코드를 다시 정렬한다(NFD `e◌́`는 `f`보다 앞이고 NFC `é`는 뒤다). 정규화 뒤 같은 이름 둘은 Load가 순서 위반으로 거부한다.
   - blob 참조는 `{blob, layout, components, count}`가 같은 `asset.blobs` 항목이고, 배치·components가 값 타입과 맞아야 한다(Float·Vec* `f32` × 성분 수 × 채널, UInt32 `u32`, Bool `u8`, Name `names`, BoneInfluences·SparseVec3 같은 이름 배치 — components = 채널), count = 그 요소 수다. 위상 blob은 `u32`이고 components는 9.1절 순서대로 1·2·1·1·1·3·1이다.
   - 거부: 기록 형식(키·종류·counts 여섯·위상 blob 수 7·요소·값 타입 이름·채널 1–255·blob 참조·Name 값 배치·잘못된 UTF-8 이름) `CorruptBundle` / `CorruptData`, `MeshSource::Load` 실패는 그 진단 코드(`CorruptPackage`는 교환 쪽이라 `CorruptBundle`로 바꾼다)와 오류, 세부는 `MeshDiagnostic`. blob 수가 `asset.blobs`와 다르면 호출 계약 위반 `InvalidSource` / `InvalidArgument`다. 공개 함수(`ExchangeMeshSource.h`)라 스킨 메시(UX7b)도 쓴다. 5.4절 8번의 시험 안 적재 코드는 이 함수로 바꿨다(외부·golden 묶음 시험).
2. **모드(5.3절, 옵션 `meshNormals`).** LOD마다:
   - `unreal`(기본): `hasSource`면 원본은 `prepared`이고 그 LOD의 빌드 설정에서 `recomputeNormals`·`recomputeTangents`·`generateLightmapUVs`를 끄고 `srcLightmapIndex`·`dstLightmapIndex`는 `preparedBuild` 값이다. UE 원래 플래그는 `SourceModels[i].BuildSettings` 그대로라 ImportRecord 원본(에셋 JSON)에 남는다.
   - `rocks`: 원본은 `source`, 빌드 설정은 UE 값 그대로다.
   - `generated`면 `baked`를 `bakedSource`로 함께 둔다(5.1절 — buildScale (1, 1, 1) 규칙은 Rocks 빌드가 한다).
   - `hasSource`인데 고른 기록이 없거나(플러그인 `MeshSourceUnavailable` — 문서 읽기가 이미 거부한다) `unreal` 모드에서 `preparedBuild`가 없으면 `CorruptBundle`(detail = LOD)이다. `hasSource`·`generated`는 bool이어야 하고, 생성 LOD가 아닌데 `baked`가 null이 아니어도 형식 위반이다. 원본이 없는 LOD는 빌드 플래그를 바꾸지 않는다. 원본도 `baked`도 없는 LOD는 서비스가 `InvalidSetting`으로 거부한다.
3. **LOD 설정.** `settings.SourceModels`의 수는 `data.lods` 수와 같아야 한다(아니면 `CorruptBundle`). `SourceModels[i]`:
   - `BuildSettings` → `MeshBuildSettings`, `ReductionSettings` → `MeshReductionSettings`: 10.6절 8번 설정 번역 + `ImportJson`(값 struct 대상 — `AssignmentTarget::Value`)이다. 행 표는 4번이다.
   - `ScreenSize` `{Default, PerPlatform}` → `screenSize`, PerPlatform이 비지 않으면 `UnsupportedSetting`(텍스처 `Downscale`과 같다 — UE 플랫폼 이름 대응은 후속).
   - `BuildSettings.DistanceFieldReplacementMesh`는 LOD 0의 값만 에셋 칸 `distanceFieldReplacementMesh`로 쓴다(Rocks는 에셋 하나에 하나 — UE도 LOD 0 값만 쓴다, StaticMesh.cpp `GetDistanceFieldReplacementMesh`). 다른 LOD의 값이 null이 아니면 `UnsupportedSetting`.
   - 그 밖의 키(`StaticMeshDescriptionBulkData`·`CacheMeshDescription*`·`SourceImportFilename`·`bImportWithBaseMesh`)는 설정이 아니다(버림).
4. **번역 행 표(키 이름이 다른 것만 — 나머지는 b 접두만 떼고 같다, 10.6절 8번 `Same`).** 10.6절 8번 행에 `rocksKey`(비면 b 접두를 뗀 UE 키)를 더한다.
   - BuildSettings: `BuildScale3D` → `buildScale`, `bUseBackwardsCompatibleF16TruncUVs`(Rocks 필드 없음 — Unsupported, 기본 false: EngineTypes.h `FMeshBuildSettings` 생성자. 옛 에셋은 PostLoad가 참으로 바꾸므로 엔진 정육면체가 경고를 낸다 — UE 빌더는 이 값을 쓴다, StaticMeshBuilder.cpp), `DistanceFieldReplacementMesh`(3번 — 표에서 버림).
   - ReductionSettings: `MaxNumOfTriangles` → `maxTriangles`, `MaxNumOfVerts` → `maxVertices`, `BaseLODModel` → `baseLod`, enum(`SilhouetteImportance` 등·`TerminationCriterion`)은 접두가 없는 UE 이름 그대로다(`Normal`·`Triangles`).
   - NaniteSettings → `VirtualGeometrySettings`: 이름은 대소문자만 다르다(`LerpUVs` → `lerpUvs` 등). `DisplacementMaps`(배열 — Rocks 필드 없음, 기본 `[]`)와 `NaniteAssemblyData`(기본 `{"Parts": [], "Nodes": []}`)는 Unsupported, `NormalPrecision` 등 정수는 그대로다.
   - 번역 결과는 문서 하나다. 값 struct(빌드·축소·Nanite·UV 밀도·섹션 정보)는 그 부분 객체를 값 대상 `ImportJson`으로 읽고, 나머지(화면 크기·슬롯 이름·참조·소켓·에셋 스칼라)는 코드가 읽는다. `ImportJson` 진단 거부는 `InvalidSource` / `CorruptData`(10.6절 8번 — detail = LOD 또는 행), 번역 자체의 실패(표 종류와 맞지 않는 값)는 `InvalidSource`(detail 0), 코드가 읽는 값의 종류 위반은 `CorruptBundle`(detail = 행)이다.
   - `UVChannelData` → `MeshUvDensity`: `bInitialized`·`bOverrideDensities`·`LocalUVDensities`(4개 배열)는 같다.
   - SectionInfo 값 → `MeshSectionInfo`: 같다(`MaterialIndex`·`bEnableCollision` 등).
5. **에셋 설정 표(최상위 `settings`).**
   - 명세로 옮김: `StaticMaterials`(6번), `SectionInfoMap`(7번), `Sockets`(8번), `LightMapResolution`, `LightMapCoordinateIndex`, `PositiveBoundsExtension`·`NegativeBoundsExtension`(`{X,Y,Z}`), `bAutoComputeLODScreenSize`, `LODGroup`(`None` → 빈 값), `MinLOD`·`NumStreamedLODs`(`{Default, PerPlatform}` — PerPlatform이 비지 않으면 `UnsupportedSetting`), `NaniteSettings`(4번), `LODForCollision`, `ComplexCollisionMesh`(참조 — 옵션 `references`, 없으면 null).
   - 버림(설정 아님): `SourceModels`(3번), `HiResSourceModel`(플러그인이 `HiResSourceSkipped`를 냈다), `OriginalSectionInfoMap`, `ImportVersion`, `MaterialRemapIndexPerImportVersion`, `LightmapUVVersion`, `ElementToIgnoreForTexFactor`, `LightmapUVDensity`, `AssetImportData`, `ThumbnailInfo`, `EditorCameraPosition`, `bCustomizedCollision`, `ExtendedBounds`.
   - Rocks 필드 없음(기본값이 아니면 `UnsupportedSetting`): `LODStreaming`(`Default`), `MinQualityLevelLOD`(`{Default: 0, PerQuality: []}`), `StaticMeshPaintSupport`(`Default`), `MeshPaintTextureCoordinateIndex`(0), `MeshPaintTextureResolution`(0), `DistanceFieldSelfShadowBias`(0), `bGenerateMeshDistanceField`(false), `bHasNavigationData`(true), `bSupportUniformlyDistributedSampling`(false), `bSupportPhysicalMaterialMasks`(false), `bUseLegacyTangentScaling`(false), `RayTracingProxySettings`(UE 기본), `bSupportRayTracing`(true), `bDoFastBuild`(false), `bAllowCPUAccess`(false), `bSupportGpuUniformlyDistributedSampling`(false), `AssetUserData`(`[]`), `NumCinematicMipLevels`(0), `NoRefStreamingLODBias`(`{Default: -1, PerQuality: []}`), `NeverStream`(false), `bGlobalForceMipLevelsToBeResident`(false). 한계: StreamableRenderAsset 칸(`NeverStream` 등)은 정적 메시 서비스 명세에 없어 v1은 경고로만 둔다.
   - 표에 없는 키는 `UnsupportedSetting`(detail = `ExchangeUnknownSettingDetail`)이다(텍스처와 같다). 경고 detail은 이 표(`ExchangeStaticMeshSettingRows`)의 행 번호다.
6. **슬롯(`StaticMaterials[i]`).** `MaterialSlotName` → `slotName`, `ImportedMaterialSlotName` → `importedSlotName`(`None`은 빈 값), `MaterialInterface`·`OverlayMaterialInterface` → `material`·`overlayMaterial`(옵션 `references`의 id, 없으면 null — 9.3절 대체: 엔진 기본 머티리얼), `UVChannelData` → `uvDensity`(4번).
7. **섹션 정보(`SectionInfoMap.Map`).** 항목 `{key, value}`의 key는 UE `FMeshSectionInfoMap` 키(`LODIndex << 16 | SectionIndex` — StaticMesh.cpp `GetMeshMaterialKey`)다. (lod, section)으로 풀어 엄격 증가로 정렬한다(UE 맵 덤프는 키 JSON byte 순이라 숫자 순과 다를 수 있다). 값은 `MeshSectionInfo`(4번).
8. **소켓(`Sockets[i]`).** 하위 객체 덤프(11.7절 13번 `{subobject, class, properties}`)의 `properties`: `SocketName` → `name`, `RelativeLocation`(`{X,Y,Z}`), `RelativeRotation`(`{Pitch,Yaw,Roll}` — Rocks `Rotator`와 같은 도·순서), `RelativeScale`, `Tag`, `bSocketCreatedAtImport` → `createdAtImport`(덤프에 없으면 UE 기본 거짓 — 명세 기본값 참은 파일 형식 임포터의 값이다). `PreviewStaticMesh`(편집기 미리보기)는 버리고 모르는 프로퍼티는 `UnsupportedSetting`(detail = Sockets 행)이다. 같은 하위 객체를 다시 가리키는 `{subobject}`만 있는 항목은 `CorruptBundle`이다(소켓은 공유하지 않는다).
9. **충돌(`data.collision` — BodySetup 덤프).** null이면 충돌 없음이다. 아니면:
   - `AggGeom`: `SphereElems`(`Center`·`Radius`) → `spheres`, `BoxElems`(`Center`·`Rotation`·`X`·`Y`·`Z` → `size`) → `boxes`, `SphylElems`(`Center`·`Rotation`·`Radius`·`Length`) → `capsules`, `TaperedCapsuleElems`(`Radius0`·`Radius1`·`Length`) → `taperedCapsules`, `ConvexElems`(`VertexData` 점 배열·`IndexData` 삼각형 index·`Transform` `{Rotation(쿼터니언), Translation, Scale3D}`) → `convexes` + `ConvexGeometry`. 형상마다 공통 `Name`·`bContributeToMass`·`CollisionEnabled`(UE 값 이름 그대로 — `QueryAndPhysics` 등)·`RestOffset`. UE 원통은 없다(Rocks 확장).
   - level set 계열(`LevelSetElems`·`SkinnedLevelSetElems`·`MLLevelSetElems`·`SkinnedTriangleMeshElems`)이 비지 않으면 경고 `UnsupportedCollisionShape`(MeshAnimationAssets 4.8절 6 — detail = `UnsupportedCollisionFeature`, 종류마다 하나)이고 버린다. 테이퍼드 캡슐의 `Width` ≠ 0·`bOneSidedCollision` 참도 같은 경고(값 종류마다 하나)이고 값만 버린다. A2f 경고는 detail 하나라 4.8절 6의 개수(`count`)는 남지 않는다.
   - 형상 칸은 덤프에 없으면 Rocks 기본값(UE 생성자와 같다)이고, 그 밖의 칸(`bIsGenerated`·`ElemBox` 등 캐시)은 버린다. 이름 `None`은 빈 Name이다.
   - `CollisionTraceFlag`(`CTF_UseDefault`·`CTF_UseSimpleAndComplex`·`CTF_UseSimpleAsComplex`·`CTF_UseComplexAsSimple`) → `complexity`, `bDoubleSidedGeometry` → `doubleSidedGeometry`.
   - 그 밖의 BodySetup 칸(`PhysMaterial`·`DefaultInstance`·`WalkableSlopeOverride` 등 물리 설정)은 v1에서 버린다(한계 — 물리 에셋·body 설정은 물리 모듈 단계).
   - 형식 위반은 `CorruptBundle` / `CorruptData`(detail = 그 종류 배열의 형상 번호, BodySetup 자체는 0), 형상 값 검사는 서비스가 한다(`InvalidCollisionData`).
10. **A2f 임포터 `unreal_static_mesh`(UX2b2와 같은 틀).** id `880ace52-8428-4e4d-82d0-924e1c041e86`, 출력 `StaticMesh`, 역할·접두·태그 접두·옵션은 텍스처와 같다. 순서: 문서 읽기·클래스·objectPath·blob 수집과 검사(10.6절 6번 — 텍스처와 공용 도우미로) → LOD 기록 적재(1·2번) → 명세 만들기(3–9번) → `Output` → `ImportStaticMesh(context, output, spec)`(`replaceSettingsOnReimport` 참 — 원본 쪽이 설정의 정본, MeshAnimationAssets 5.5.3절) → 태그. 설정 경고는 `context.Warn`이다.
11. **종류별 임포터 일반화.** 텍스처용 `ExchangeTextureKind`를 교환 클래스 → A2f 임포터 이름 표로 일반화한 `ExchangeServiceKind`로 바꾼다(원본·옵션·서비스 호출은 클래스와 무관하다). 등록 hook `RegisterExchangeServiceKinds`가 표에 이름이 있는 클래스(텍스처 다섯·`StaticMesh`)에 등록한다. 옵션 대입에 `meshNormals=<unreal|rocks>`(요청 옵션 — 10.5절 9번)를 더한다. `UnrealExchangeImportOptions`에 `meshNormals`(리플렉션 enum `ExchangeMeshNormals` — 지금 실행 헤더의 enum을 리플렉션 enum으로 옮긴다)를 더한다.
12. **시험.** UX3b1: 기록 적재(golden 묶음 정적 메시의 모든 기록 — 5.4절 8번 시험이 이 함수를 쓰게 바꾼다, 합성 기록의 형식 거부, NFC 경고), 모드 둘의 LOD 원본·빌드 플래그, 설정 번역(행마다·이름 바꾼 행·Unsupported·모르는 키), 슬롯·섹션 키 풀기와 정렬·소켓·충돌 형상 다섯과 level set 경고·`CollisionTraceFlag`. UX3b2: 명령으로 golden 묶음의 정적 메시 임포트(원본 LOD 수·슬롯·충돌·태그), 재임포트(id 유지·설정이 UE 값으로), 일반 `asset-reimport`, `--mesh-normals=rocks`, 묶음 밖 머티리얼 참조 없음 경고, A2f 임포터 거부, C# 확인.
