## 4. 메시 공통 설정과 구성 요소

### 4.1 빌드 설정 `MeshBuildSettings`

정적 메시와 스킨 메시가 같은 구조를 쓴다. 스킨 메시만의 필드는 `SkinnedMeshBuildSettings`가 더한다(6.3절). 상태 열: **구현** = MA 단계에서 빌드가 쓴다. **보존** = 저장·임포트만 하고 소비자는 후속(값이 기본값이 아니면 빌드가 `UnsupportedSetting` 경고).

| 필드 | UE | 상태 |
|---|---|---|
| `recomputeNormals`, `recomputeTangents`, `useMikkTSpace`, `computeWeightedNormals` | bRecomputeNormals·bRecomputeTangents·bUseMikkTSpace·bComputeWeightedNormals(EngineTypes.h:2948-2960) | 구현 |
| `removeDegenerates` | bRemoveDegenerates(:2964) | 구현 |
| `useHighPrecisionTangentBasis`, `useFullPrecisionUVs` | :2972, :2976 | 구현(쿠킹 정점 형식 선택) |
| `buildScale` `Vec3d` | BuildScale3D(:3009) | 구현(5.2절 8단계) |
| `buildReversedIndexBuffer` | :2968 | 보존 |
| `generateLightmapUVs`, `minLightmapResolution`, `srcLightmapIndex`, `dstLightmapIndex` | :2983, 2996-3002 | 보존(라이트맵 단계, 계획 14.10절) |
| `generateDistanceFieldAsIfTwoSided`, `distanceFieldResolutionScale`, `maxLumenMeshCards` | :2990, 3016, 3033 | 보존(GI 단계) |
| `supportFaceRemap` | :2993 | 보존(물리 단계) |

UE `DistanceFieldReplacementMesh`(:3024)는 객체 참조라 빌드 설정 struct에 두지 않는다. 그러면 빌드 설정이 참조 struct가 되어 LOD 배열 전체가 참조 struct 배열이 된다. 대신 `StaticMesh`의 EditorOnly 필드 `distanceFieldReplacementMesh : ObjectRef<StaticMesh>`(Build 의존, 5.1절)로 둔다. UE의 `bUseBackwardsCompatibleF16TruncUVs`(:2980)는 옛 콘텐츠 호환용이라 넣지 않는다(하위호환 없음 원칙).

### 4.2 축소 설정 `MeshReductionSettings`

필드는 UE `FMeshReductionSettings`(MeshReductionSettings.h:40-110)와 같은 이름 집합을 갖는다: `percentTriangles`, `maxTriangles`, `percentVertices`, `maxVertices`, `maxDeviation`, `pixelError`, `weldingThreshold`, `hardAngleThreshold`, `baseLod`, 중요도 넷(`silhouetteImportance`·`textureImportance`·`shadingImportance`·`vertexColorImportance`), `recalculateNormals`, `generateUniqueLightmapUVs`, `keepSymmetry`, `visibilityAided`, `cullOccluded`, `terminationCriterion`, `visibilityAggressiveness`. 전부 **보존**이다. 축소기는 후속 단계다.

생성 LOD 판정: UE는 `!IsMeshDescriptionValid(i) || IsReductionActive(i)`다(E/Private/StaticMesh.cpp:9772-9773). UE의 "축소 활성"은 축소 모듈이 있어야 하고, LOD 그룹이 덮어쓴 설정과 캐시된 정점·삼각형 수로 판정한다(StaticMesh.cpp:3532-3550). Rocks는 UE 기본 축소기의 판정식을 그대로 쓴다(5.5.1절 5·6 — LOD 그룹 덮어쓰기는 메시 LOD 그룹 표가 생길 때). 축소기가 없는 동안 생성 LOD는 임포터가 준 결과 메시(`bakedSource`, 5.1절)가 있으면 그것을, 없고 원본이 있으면 원본을 축소 없이 빌드하고 `UnsupportedSetting` 경고를 낸다. 둘 다 없으면 그 LOD를 뺀다(5.5.2절 5).

### 4.3 머티리얼 슬롯 `MeshMaterialSlot`

```cpp
ROCKS_STRUCT()
struct MeshMaterialSlot {                                            // 참조 struct
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name slotName;                                  // 사용자에게 보이는 이름
    ROCKS_PROPERTY(EditorOnly) Name importedSlotName;                // 다각형 묶음 MaterialSlotName과 맞추는 키
    ROCKS_PROPERTY() ObjectRef<MaterialBase> material;               // Hard. 비면 엔진 기본 머티리얼
    ROCKS_PROPERTY() ObjectRef<MaterialBase> overlayMaterial;        // Hard. 비어도 됨
    ROCKS_PROPERTY() MeshUvDensity uvDensity;                        // 텍스처 스트리밍 sampling scale 입력
};
// StaticMesh·SkeletalMesh 필드: ROCKS_PROPERTY() StructRefArray<MeshMaterialSlot> materialSlots;
```

- 다각형 묶음 → 슬롯 대응은 `importedSlotName` 일치로 정한다. 일치하는 슬롯이 없으면 묶음 번호와 같은 번호의 슬롯을 쓰고 `MaterialSlotFallback` 경고를 낸다. UE는 묶음 ID 값으로 대체한다(StaticMeshBuilder.cpp:1613-1623).
- `MeshUvDensity`는 UE `FMeshUVChannelInfo`(bInitialized·bOverrideDensities·LocalUVDensities[4])에 대응한다. 계산은 [텍스처 스트리밍](../TextureStreaming.ko.md) 단계에서 한다(소비자와 함께 — 5.5.1절 4). 그때까지 빌드는 값을 바꾸지 않는다.
- 섹션의 슬롯 번호가 슬롯 수 밖이면(슬롯 대체·섹션 정보 덮어쓰기) 엔진 기본 머티리얼로 그린다(UE와 같다 — 5.5.1절 9).

### 4.4 섹션 정보 `MeshSectionInfo`

(LOD, 섹션) 쌍마다 `materialIndex`, `enableCollision`, `castShadow`, `visibleInRayTracing`, `affectDistanceFieldLighting`, `forceOpaque`를 둔다(UE `FMeshSectionInfo`, StaticMesh.h:350-370). 기본은 섹션 번호 = 슬롯 번호다. `castShadow`·`materialIndex`는 **구현**, 나머지는 **보존**이다.

### 4.5 소켓 `MeshSocket`

`name`, `relativeLocation Vec3d`, `relativeRotation Rotator<double>`, `relativeScale Vec3d`, `tag String`. UE `UStaticMeshSocket`(StaticMeshSocket.h:26-38)은 UObject지만 Rocks는 값 구조체 배열로 둔다. 소켓마다 하위 객체 정체성이 필요하지 않기 때문이다. 미리보기 메시는 에디터 UI 범위라 넣지 않는다.

### 4.6 단순 충돌 `SimpleCollision`

```text
SimpleCollision {                                     // 값 struct(참조 없음)
  spheres        : Array<CollisionSphere { center Vec3d, radius }>
  boxes          : Array<CollisionBox { center, rotation Rotator<double>, size Vec3d }>        // size는 축별 전체 길이(UE X·Y·Z)
  capsules       : Array<CollisionCapsule { center, rotation, radius, length }>                 // length는 원기둥 부분 길이(UE Sphyl)
  taperedCapsules: Array<CollisionTaperedCapsule { center, rotation, radius0, radius1, length }>
  cylinders      : Array<CollisionCylinder { center, rotation, radius, length }>               // Rocks 추가 형상(CylinderShape CY2)
  convexes       : Array<CollisionConvex { vertices BulkDataRef(Vec3d), indices BulkDataRef(uint32), transform Transform }>
  공통 필드: name Name, contributeToMass bool, collisionEnabled CollisionEnabled, restOffset float
}
StaticMesh: simpleCollision, complexity CollisionComplexity { Default, SimpleAndComplex, SimpleAsComplex, ComplexAsSimple },
            lodForCollision int32, complexCollisionMesh ObjectRef<StaticMesh>(Hard, 비어도 됨),
            doubleSidedGeometry bool, physicalMaterial(물리 단계에서 타입이 생기면 추가)
```

- 형상의 의미(중심·회전·전체 길이)는 UE 필드를 그대로 따른다(SphereElem.h:25-29, BoxElem.h:28-44, SphylElem.h:28-40, TaperedCapsuleElem.h:21-45, ConvexElem.h:37-51). [물리 어댑터](../PhysicsJoltAdapter.ko.md)가 Jolt 형상으로 옮긴다. 이 문서는 데이터만 정한다.
- 테이퍼드 캡슐의 `Width`·`bOneSidedCollision`은 넣지 않고, 값이 기본값이 아니면 임포트 경고를 낸다.
- level set·ML level set·skinned 형상(AggregateGeom.h:28-52)은 받지 않는다(`UnsupportedCollisionShape` 경고, 개수 포함).
- 볼록 형상의 쿠킹(Jolt 형상 직렬화)은 물리 단계다.

### 4.7 LOD 화면 크기와 플랫폼별 값

LOD 화면 크기(UE `FPerPlatformFloat ScreenSize` — Rocks `PerPlatformFloat`, 5.5.1절 2, StaticMeshSourceData.h:177-182)와 최소 LOD는 플랫폼별 값이다. 텍스처의 플랫폼별 설정과 같은 장치(기본값 + TargetProfile 이름별 덮어쓰기 표, AssetSystem 12.4절)를 쓴다. `autoComputeLodScreenSize`(UE `GetAutoComputeLODScreenSize()` — 멤버 직접 접근은 5.7부터 deprecated, StaticMesh.h:720)와 `lodGroup Name`도 둔다. 메시 LOD 그룹 표는 TargetProfile에 텍스처 LOD 그룹과 나란히 둔다(요구 MR9).

### 4.8 구현에서 정한 점(MA3a)

MA3a 구현(`Mesh` 모듈의 `Mesh/SimpleCollision.h`) 중 4.6절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다. UE 근거는 E/Classes/PhysicsEngine/ShapeElem.h·SphereElem.h·BoxElem.h·SphylElem.h·TaperedCapsuleElem.h·ConvexElem.h·AggregateGeom.h, E/Private/Collision/KAggregateGeom.cpp(생성자 기본값), E/Classes/Engine/EngineTypes.h(`ECollisionEnabled`)다. `CollisionEnabled` enum은 2026-11-25에 `Collision` 모듈로 옮겼다(같은 타입 ID — 컴포넌트 body와 같은 타입을 쓰려고, World가 에셋 모듈에 의존하지 않으므로 둘 아래 모듈에 둔다 — [충돌 데이터](../CollisionData.ko.md) CDD1). `Mesh`는 `Collision`에 의존한다.

1. **타입.** 형상 struct 여섯(`CollisionSphere`·`CollisionBox`·`CollisionCapsule`·`CollisionTaperedCapsule`·`CollisionCylinder`·`CollisionConvex`)은 공통 필드(`name`·`contributeToMass` 기본 참·`collisionEnabled` 기본 `QueryAndPhysics`·`restOffset` 기본 0 — UE `FKShapeElem`)를 각자 갖는다. 공통 기반 struct로 상속하지 않는 까닭은 UE도 형상마다 독립 USTRUCT 필드를 저장하고, 필드 순서가 저장 의미에 들어가지 않아 기반을 둘 이점이 없기 때문이다(버린 대안: `CollisionShapeBase` 상속 — 필드 경로가 형상마다 같아지는 것 말고는 얻는 것이 없다). 크기 기본값은 UE 생성자와 같다(구 반경 1, 상자 1·1·1, 캡슐 반경 1·길이 1, 테이퍼드 캡슐 1·1·1). 실린더 기본값은 캡슐과 같게 반경 1·길이 1로 둔다. `CollisionEnabled`는 UE 값 여섯(`NoCollision`·`QueryOnly`·`PhysicsOnly`·`QueryAndPhysics`·`ProbeOnly`·`QueryAndProbe`)을 같은 번호로 둔다. `CollisionComplexity`는 4.6절 넷이고, `StaticMesh` 필드 연결은 MA2b다.
2. **크기 규칙(UE 의미, 물리 사상 가능성 기준).** 모든 실수는 유한이어야 한다. 구 반경 > 0, 상자 `size` 세 성분 > 0, 캡슐 반경 > 0·길이 ≥ 0(0이면 구와 같다 — UE가 받는 값), 테이퍼드 캡슐 두 반경 > 0·길이 ≥ 0, 실린더 반경 > 0·길이 > 0(길이 0은 원판이라 축을 정할 수 없다 — [Cylinder 형상 설계](../CylinderShape.ko.md) 3.3절), `restOffset`은 유한(부호 제한 없음 — UE도 음수를 받는다). 회전(`Rotator<double>`, 도)은 유한이면 범위를 두지 않는다(UE 편집 범위 ±360은 UI 제한이다). UE는 이 값을 검사하지 않고 물리 생성 때 잘라 쓰지만, Rocks는 데이터 단계에서 거부해 물리 어댑터가 받는 값을 좁힌다.
3. **볼록 형상.** 점은 `Vec3d`(little-endian double 셋, 24 byte), index는 `uint32`(4 byte) bulk이고 bulk는 늘 있다(0 byte index도 entry — 3.7절 3과 같다). 점 수는 `CollisionConvexMinVertices`(4 — 부피가 있는 hull의 최소) 이상 `CollisionConvexMaxVertices`(65535) 이하, index 수는 3의 배수, 각 index < 점 수, 삼각형 수 ≤ 2 × 점 수 − 4(닫힌 볼록 다면체 삼각 분할의 상한 — Euler 공식)다. index가 비어도 된다(UE `IndexData`는 hull에서 다시 만드는 값이다). `transform`은 성분 유한, 회전 |길이² − 1| ≤ `CollisionRotationNormalizedTolerance`(0.01, UE `THRESH_QUAT_NORMALIZED`), 크기 세 성분 ≠ 0이다. UE의 `ElemBox`(점에서 다시 계산하는 캐시)는 저장하지 않는다. 메모리 형태는 `ConvexGeometry{vertices, indices}`이고 `StoreConvexGeometry`·`LoadConvexGeometry`가 bulk와 오간다. 점 수 상한은 데이터 상한이고 Jolt hull 점 수 한도(256)는 쿠킹(물리 단계)이 다룬다.
4. **형상 수 상한.** 형상 수 합 ≤ `CollisionMaxElements`(65535)다(compound 자식 번호를 16비트로 담을 수 있게).
5. **검증 함수.** `ValidateSimpleCollision`은 값 필드만 본다(형상 수, 형상 종류 순서 구·상자·캡슐·테이퍼드·실린더·볼록, 배열 번호 순). bulk는 `ValidateSimpleCollisionBulk`(볼록 형상마다 `LoadConvexGeometry`)가 본다. 위반은 새 진단 `InvalidCollisionData`(오류 — 11절 표에 더했다)이고 결과는 `CollisionDiagnostic{code, error, issue(CollisionIssue), shape(CollisionShapeKind), index, related, count}`다. 로드 중 bulk 배치 위반은 `CorruptPackage`다.
6. **받지 않는 형상 진단.** `UnsupportedCollisionShape`(경고)는 임포터가 낸다. 형식은 `MakeUnsupportedCollisionWarning(feature, count)` — issue `UnsupportedFeature`, `related` = `UnsupportedCollisionFeature`(level set·skinned level set·ML level set·skinned triangle mesh, 테이퍼드 캡슐 `Width` ≠ 0·`bOneSidedCollision` — 4.6절의 "기본값이 아니면 경고"를 같은 코드로 낸다), `count` = 버린 형상(또는 그 값을 가진 테이퍼드 캡슐) 수다.
7. **결정성.** 검증은 첫 위반이 입력만으로 정해진다. 부동소수점 계산은 사칙연산과 비교뿐이다.
