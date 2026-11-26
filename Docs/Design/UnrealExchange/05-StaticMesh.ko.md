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
