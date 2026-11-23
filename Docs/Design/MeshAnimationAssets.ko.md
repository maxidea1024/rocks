# 메시·스켈레톤·애니메이션 에셋 설계

기준일: 2026-11-23, 개정 14(MA2b 선행을 MaterialSystem MS3에서 MS3a — `MaterialBase` 골격 — 로 좁힘, 13절). 개정 13(MA 묶음 리뷰 반영 — 3.7절 12, 5.4절 16·17, 6.4절 6, 7.5절 10, 12·13절). 개정 12(MA3a 구현 — 4.8절·12.6절). 개정 11(MA6a 구현 — 7.5절·12.5절). 개정 10(MA5a 구현 — 6.5절·12.4절). 개정 9(MA3·MA5·MA6을 a·b로 나눔). 개정 8(MA4a 구현 — 6.4절·12.3절). 개정 7(MA2a 구현 — 5.4절·12.2절). 개정 6(MA2a·MA4a 분리). 개정 5(MA1 구현 — 3.7절). 개정 4(MA2 전 결정 반영 — index 형식·통합 bone map·메시 LOD 스트리밍). 개정 3(15절 요구를 다른 문서에 반영하고 상태 열을 더함). 개정 2(교차 검증 반영 — 17절). 정적 메시·스켈레톤·스킨 메시·애니메이션 클립의 에셋 자료구조를 정한다. 범위는 원본 데이터(에디터 데이터), 저장 설정, 빌드 계약, 쿠킹 데이터, 모듈 배치다. [엔진 구현 계획](EngineImplementationPlan.ko.md) 13.1절(StaticMeshAsset)·15절(Skeleton·SkeletalMesh·AnimationClip)의 개요를 구현할 수 있는 수준으로 구체화한다. 텍스처는 [에셋 시스템](AssetSystem/12-Textures.ko.md) 12절, 머티리얼은 [머티리얼 시스템](MaterialSystem.ko.md) 4절이 이미 정했으므로 이 문서는 그 둘을 참조만 한다.

- 배경: 사용자 지시(2026-11-19). 엔진 시험용으로 UE에서 텍스처·정적 메시·머티리얼·애니메이션을 가져오는 기능을 넣는다. Rocks에 메시·애니메이션 자료구조가 아직 없으므로 함께 정한다. 가져오는 경로는 [Unreal 교환 형식](UnrealExchange.ko.md)이 정한다. 이 문서의 자료구조는 그 경로와 상관없이 쓰는 엔진 기반이며, 뒤에 올 glTF·FBX 임포터도 같은 구조로 들어온다.
- UE 근거: UE 5.8.4(`release`, commit `f45729c0b6f0`, 로컬 클론 `C:\github\UnrealEngine`)를 읽어 확인했다. 경로 약어는 다음과 같다. `E/` = `Engine/Source/Runtime/Engine`, `RT/` = `Engine/Source/Runtime`, `Dev/` = `Engine/Source/Developer`. UE 이름은 대응표와 근거에만 쓰고, 타입 이름은 Rocks 이름으로 짓는다. 빌드 설정·축소 설정처럼 업계 공용 서술 이름은 그대로 쓴다.
- 좌표·단위는 UE 5.8과 같다(왼손, +Z 위, cm, `Rotator`는 도). 원본·쿠킹 데이터 어디에도 좌표 변환 계층을 두지 않는다. 삼각형 winding도 UE 원본 순서를 그대로 저장한다. 앞면 판정은 렌더 쪽 몫이다([셰이더 시스템](ShaderSystem/README.ko.md)의 음수 행렬식 규칙).
- 착수 시점: 고정 구현 순서(… → A1–A6 → 화면 마일스톤)를 바꾸지 않는다. 이 문서의 단계(MA, 13절)를 언제 시작할지는 사용자가 정한다. 에셋 시스템 A1–A4(패키지·AssetDatabase·에셋 너겟·쿠커)가 선행 조건이다.

## 1. 결정 요약

| 항목 | 결정 | 근거·대안 |
|---|---|---|
| 원본 데이터 모델 | 속성 기반 다각형 메시 `MeshSource`(UE `FMeshDescription` 대응). 정점·코너·변·삼각형·다각형·다각형 묶음의 여섯 요소와, 요소별로 이름 있는 속성 배열을 둔다 | UE 정적·스킨 메시가 모두 이것을 원본으로 쓴다(3절). 빌드 결과(렌더 버퍼)를 원본으로 두면 n-gon·hard edge·정밀도를 잃는다(3.6절) |
| 원본 저장 | 속성 배열 하나 = Bulk entry 하나. 요소 수·속성 목록은 리플렉션 구조체에 둔다 | 텍스처 원본(`TextureSource`)과 같은 방식이다. 네이티브 직렬화 경로를 만들지 않는다(AssetSystem 5.6절). 대안은 결정 MA-D2 |
| 정적 메시 | `StaticMesh : StreamableRenderAsset`. 원본 LOD 목록, 빌드·축소 설정, 머티리얼 슬롯, 섹션 정보, 소켓, 단순 충돌, 쿠킹 렌더 데이터 | UE `UStaticMesh`. 기반 클래스는 AssetSystem 12.1절이 이미 메시용으로 나눠 두었다 |
| 스켈레톤 | `Skeleton : Asset`. 본 계층(`BoneHierarchy` — 이름·부모·기준 pose), 본별 이동 리타깃 방식, 가상 본, 소켓, 커브 정보, 슬롯 묶음 | UE `USkeleton`·`FReferenceSkeleton` |
| 스킨 메시 | `SkinnedAsset : StreamableRenderAsset`(추상) → `SkeletalMesh`. 메시는 자기 본 계층을 갖고 Skeleton을 Hard로 참조한다. 스킨 가중치·morph target의 원본은 `MeshSource` 속성이다 | UE 5.8은 LOD별 MeshDescription이 원본이고 `UMorphTarget`은 빌드 결과다(6절) |
| 애니메이션 | `AnimationAsset`(추상, Skeleton Hard + Build 참조) → `AnimationClip`. 원본은 프레임 균일 간격의 본 트랙(이동·회전·크기, float), 커브(`TangentCurve`), 이벤트, 동기 마커, 설정이다 | UE 5.8 데이터 모델은 회전을 float Euler 채널로 저장한다. 원본 quaternion이 따로 없고 프레임별 평가값이 받을 수 있는 최선이다(7.2절) |
| 머티리얼 연결 | 메시의 머티리얼 슬롯이 `ObjectRef<MaterialBase>`를 Hard로 갖는다. 슬롯 배열은 `StructRefArray` | MaterialSystem 4.2·4.4절. 참조 struct의 배열은 `StructRefArray`만 허용된다(ReflectionExtensions 1.3절 표) |
| 정점 형식 | 쿠킹 정점 stream은 GraphicsTypes `VertexFormat` 번호로 기록한다. index 형식도 GraphicsTypes `IndexFormat` 번호로 기록한다 | RenderBase GX3a(10.4절)에서 정점 원소 형식 20종이 구현되었다. `IndexFormat`은 MA2 전에 Graphics에서 GraphicsTypes로 옮기고 고정 번호표를 붙인다(RenderBase 결정 R20) |
| tangent 계산 | MikkTSpace 참조 구현을 ThirdParty로 쓰되, CRT 초월 함수를 Core 결정적 수학으로 바꾸는 패치를 둔다 | UE와 같은 tangent를 내야 노멀맵 베이크 결과와 맞는다. 쿠킹 중 CRT 초월 함수는 금지다(AssetSystem 10절). 결정 MA-D5 |
| 축소 LOD·Nanite·lightmap UV 생성·거리장·Lumen 카드 | 설정은 보존한다. 빌드는 후속 단계에서 하고, 그때까지는 `UnsupportedSetting` 경고를 낸다 | 해당 기능의 소비자(축소기·GI·VirtualGeometry)가 아직 없다 |
| 애니메이션 압축 | 쿠킹 v1은 비압축(상수 트랙 접기만)이다. ACL은 후속이다 | 계획 15절 |

## 2. UE 5.8 대응

| Rocks | UE 5.8 | 근거 |
|---|---|---|
| `MeshSource`(원본 메시) | `FMeshDescription` | RT/MeshDescription/Public/MeshDescription.h:131-149 |
| 정점 `Vertex` / 코너 `Corner` / 변 `Edge` / 삼각형 `Triangle` / 다각형 `Polygon` / 다각형 묶음 `PolygonGroup` | Vertex / VertexInstance / Edge / Triangle / Polygon / PolygonGroup | 같은 곳 |
| `StaticMeshSourceLod` | `FStaticMeshSourceModel` | E/Classes/Engine/StaticMeshSourceData.h |
| `MeshBuildSettings` | `FMeshBuildSettings` | E/Classes/Engine/EngineTypes.h:2942 |
| `MeshReductionSettings` | `FMeshReductionSettings` | E/Public/MeshReductionSettings.h:34 |
| `MeshMaterialSlot` | `FStaticMaterial`·`FSkeletalMaterial` | E/Classes/Engine/StaticMesh.h:486, E/Classes/Engine/SkinnedAssetCommon.h:370 |
| `MeshSectionInfo` | `FMeshSectionInfo` | StaticMesh.h:350-370 |
| `MeshSocket` | `UStaticMeshSocket` | E/Classes/Engine/StaticMeshSocket.h |
| `SimpleCollision`·`CollisionComplexity` | `UBodySetup`·`FKAggregateGeom`·`ECollisionTraceFlag` | E/Classes/PhysicsEngine/BodySetup.h, AggregateGeom.h:23-52, RT/PhysicsCore/Public/BodySetupEnums.h:10-22 |
| `StaticMeshRenderData`·`MeshRenderLod`·`MeshRenderSection` | `FStaticMeshRenderData`·`FStaticMeshLODResources`·`FStaticMeshSection` | E/Public/StaticMeshResources.h |
| `BoneHierarchy`·`BoneInfo` | `FReferenceSkeleton`·`FMeshBoneInfo` | E/Public/ReferenceSkeleton.h:13-23, 107-116 |
| `VirtualBone` | `FVirtualBone` | E/Classes/Animation/Skeleton.h:254-266 |
| `BoneSocket` | `USkeletalMeshSocket` | E/Classes/Engine/SkeletalMeshSocket.h:25-41 |
| `SkeletonCurveInfo` | `FCurveMetaData` | E/Public/Animation/AnimCurveMetadata.h:20-52 |
| `BoneTranslationRetarget` | `EBoneTranslationRetargetingMode` | Skeleton.h:71-85 |
| `SkinnedAsset`·`SkeletalMesh` | `USkinnedAsset`·`USkeletalMesh` | E/Classes/Engine/SkinnedAsset.h:46, SkeletalMesh.h |
| `SkeletalMeshSourceLod` | `FSkeletalMeshSourceModel`(`FSkeletalMeshLODInfo` 상속) | E/Classes/Engine/SkeletalMeshSourceModel.h:28-29 |
| `BoneInfluence` | `FBoneWeight` | RT/AnimationCore/Public/BoneWeights.h:206-210 |
| `AnimationAsset`·`AnimationClip` | `UAnimationAsset`·`UAnimSequence` | E/Classes/Animation/AnimationAsset.h:987, AnimSequence.h:202 |
| `ClipEvent`·`ClipSyncMarker` | `FAnimNotifyEvent`·`FAnimSyncMarker` | E/Public/Animation/AnimTypes.h:276-356, 479-506 |
| `ClipCurve`·`ClipTransformCurve` | `FFloatCurve`·`FTransformCurve` | E/Public/Animation/AnimCurveTypes.h:217-223, 303-319 |

이름 원칙: "정점·변·삼각형·다각형·morph target·소켓·가상 본" 같은 업계 공용어는 그대로 쓴다. UE의 vertex instance는 Rocks에서 **코너**라 부른다. 위치를 공유하는 정점과 구분하기 위해서다.

## 3. 원본 메시 `MeshSource`

### 3.1 요소와 위상

| 요소 | 저장 | 의미 |
|---|---|---|
| 정점 | 위치 `Vec3`(float) | 공간의 점. 여러 코너가 공유한다. UE `Vertex::Position`이 `FVector3f`다(MeshAttributes.h:90) |
| 코너 | 정점 번호 `uint32` | 정점 하나에 붙는 면 쪽 속성 묶음(법선·UV·색). **이웃한 삼각형·다각형이 같은 코너를 공유할 수 있다.** UE vertex instance도 공유된다(MeshDescription.h:619, 638 `ConnectedTriangleIDs`·`ConnectedPolygons`). 속성이 같은 매끈한 면은 코너 하나를 쓰고, seam·hard edge에서 코너가 갈린다 |
| 변 | 정점 번호 쌍 `uint32 × 2`(작은 번호 먼저) | hard edge 속성을 담는다 |
| 다각형 | 둘레 코너 목록(코너 번호 배열의 시작·개수), 묶음 번호 `uint32` | n-gon을 보존한다 |
| 삼각형 | 코너 번호 `uint32 × 3`, 다각형 번호 `uint32` | 다각형마다 임포트 때 정해진 삼각 분할. UE도 삼각형이 항상 있고 n-gon은 다각형으로 보존한다(MeshDescription.h:333-369, 772-775) |
| 다각형 묶음 | 없음(속성만) | 머티리얼 슬롯 하나에 대응하는 다각형 집합 |

불변식(검증 함수 `ValidateMeshSource`가 확인한다):
- 모든 번호는 범위 안이어야 한다. 삼각형의 세 코너는 그 다각형의 둘레 코너여야 한다. 세 코너가 같은 정점을 가리키는 퇴화 삼각형도 받는다(UE 원본에도 있다. 퇴화 처리는 빌드 몫 — 5.2절).
- 다각형 둘레는 `MeshMinPolygonCorners`(= 3)코너 이상이다. 둘레를 잇는 변은 변 목록에 있어야 한다. 같은 정점 쌍의 변이 둘 이상이면 안 된다. 같은 정점을 잇는 변(정점 쌍이 같음)은 없다. 퇴화 다각형의 둘레는 그 변을 건너뛴다.
- 다각형 하나의 삼각형 수는 둘레 코너 수 − 2다(구멍 있는 다각형은 받지 않는다. UE도 둘레 하나만 갖는다).
- 위치는 유한값이어야 한다. 아니면 `InvalidMeshData` 오류다. 속성 값의 비유한값은 3.3절 규칙으로 고친다.
- 요소 수는 `MeshMaxElements`(= 2^31 − 1) 이하다(번호 계산의 넘침 방지). 다각형 묶음 수는 `MeshMaxPolygonGroups`(= 65535) 이하다(3.7절 12 — 묶음 수는 위상·위치 bulk에 묶인 개수가 아니라서 따로 묶는다).
- 속성 검증은 3.5절이다.

UE의 UV 공유 요소(채널별 UV 정점 — MeshDescription.h:131-149)는 저장하지 않는다. 빌드와 UE 빌더가 쓰는 값은 코너의 UV 속성이고, UV 공유 요소는 UV 편집 도구의 위상 정보라 코너 UV에서 다시 만들 수 있다. UV 편집 도구를 만들 때 다시 검토한다.

### 3.2 속성

속성은 (요소, 이름, 값 타입, 채널 수)로 정의한다. 아래는 엔진이 이름을 정한 속성이다. 임포터·도구가 다른 이름의 속성을 더할 수 있다. 빌드는 모르는 속성을 무시하고 저장은 보존한다.

| 요소 | 이름 | 값 타입 | 채널 | 대응(UE `FStaticMeshAttributes`·`FSkeletalMeshAttributes`) |
|---|---|---|---|---|
| 정점 | `Position` | Vec3 | 1 | Vertex::Position(MeshAttributes.h:90). 필수이며 메모리 형태에서는 위상과 함께 따로 둔다(3.7절 1) |
| 코너 | `Normal` | Vec3 | 1 | Normal(StaticMeshAttributes.h:23) |
| 코너 | `Tangent` | Vec3 | 1 | Tangent(:24) |
| 코너 | `BinormalSign` | Float | 1 | BinormalSign(:25) |
| 코너 | `Color` | Vec4(선형) | 1 | Color, 기본 흰색(.h:26, StaticMeshAttributes.cpp:55) |
| 코너 | `TexCoord` | Vec2 | 1–8 | TextureCoordinate. 상한은 UE `MAX_MESH_TEXTURE_COORDS_MD`(MeshDescription.h:52)와 같은 `MeshMaxTexCoords` = 8 |
| 변 | `IsHard` | Bool | 1 | IsHard(:31) |
| 다각형 | `PartName` | Name | 1 | ObjectName(선택, :43) |
| 다각형 묶음 | `MaterialSlotName` | Name | 1 | ImportedMaterialSlotName(:48). 머티리얼 슬롯과 맞추는 키 |
| 정점 | `SkinWeights`, `SkinWeights-<profile>` | BoneInfluences | 1 | SkinWeights(SkeletalMeshAttributes.cpp:10, 45) |
| 정점 | `Morph-<이름>` | SparseVec3(위치 변화량) | 1 | 같은 이름(SkeletalMeshAttributes.cpp:40) |
| 코너 | `MorphNormal-<이름>` | SparseVec3(법선 변화량) | 1 | UE는 코너 쪽에도 같은 이름 `Morph-<이름>`으로 등록한다(SkeletalMeshAttributes.cpp:194-212). Rocks는 (요소, 이름)이 유일하더라도 한 이름이 두 요소에 걸치면 혼동되므로 코너 쪽을 `MorphNormal-`로 바꿔 둔다(임포터가 바꾼다) |

UE의 삼각형 법선·tangent(StaticMeshAttributes.h:36-38)는 transient라 저장하지 않으며 Rocks도 저장하지 않는다(빌드 중 계산 값).

**스킨 가중치.** `BoneInfluence { uint16 boneIndex; uint16 weight; }`. weight는 0–`BoneWeightTotal`(= 65535) 정규화 값이다(UE `FBoneWeight`와 같은 정밀도, BoneWeights.h:206-210). 정점당 개수는 0–`MeshMaxSourceInfluences`(= 12, UE `MAX_TOTAL_INFLUENCES`, E/Public/GPUSkinPublicDefs.h:12)다. boneIndex는 그 메시가 가진 본 계층(6.1절)의 원본 번호다(UE도 RefSkeleton raw 번호다 — Dev/SkeletalMeshUtilitiesCommon/Private/StaticToSkeletalMeshConverter.cpp:481-491). 가중치 합이 `BoneWeightTotal`이 아니면 임포트 검증에서 `NonNormalizedWeights` 경고를 내고 정규화한다(비례 배분 후 반올림 잔차를 가장 큰 영향에 더한다. 같으면 본 번호가 작은 쪽).

**morph target.** UE는 정점마다 조밀하게 저장하지만 Rocks는 0이 아닌 요소만 (번호, 변화량) 쌍으로 저장한다. "0"은 세 성분이 모두 비트 단위 `+0.0`인 경우만이다(`-0.0`은 0이 아닌 값으로 보존한다). 그래서 손실이 없다.

### 3.3 값 정리 규칙

임포트·편집 결과를 저장하기 전에 다음을 적용한다. UE 정리 함수 `ValidateAndFixData`(RT/StaticMeshDescription/Private/StaticMeshOperations.cpp:3062-3135, 빌드 전처리 MeshDescriptionHelper.cpp:51에서 부른다)에 대응한다.

- 위치 비유한값: 오류(`InvalidMeshData`). **UE와 다르다** — UE는 0으로 고친다(:3074-3077). 위치를 0으로 옮기면 형상이 조용히 망가지므로 Rocks는 받지 않는다.
- 법선·tangent의 비유한값 또는 길이 0: 0 벡터로 둔다. 빌드가 그 코너만 다시 계산한다(UE `ComputeTangentsAndNormals`도 플래그가 없어도 잘못된 값은 다시 계산한다 — RT/StaticMeshDescription/Public/StaticMeshOperations.h:67-71).
- UV 비유한값: 0으로 바꾸고 `NonFiniteAttribute` 경고(개수 포함). UE와 같다.
- 색 비유한값: (1, 1, 1, 1)로 바꾸고 `NonFiniteAttribute` 경고. UE와 같다(:3112-3115).
- UV 절댓값이 `MeshUvRangeWarning`(= 1024)을 넘으면 `UvOutOfRange` 경고(계획 13.1절 UV 범위 진단). 값은 바꾸지 않는다.
- 퇴화 삼각형은 원본에서 지우지 않는다. 빌드 설정 `removeDegenerates`가 빌드 때 뺀다(5.2절).

### 3.4 저장 형식

`MeshSourceData`(`ROCKS_STRUCT`, 리플렉션 저장)가 영속 형태이고, `MeshSource`(C++ 클래스, 리플렉션 없음)가 편집·빌드용 메모리 형태다. 둘 사이 변환은 `MeshSource::Load(const MeshSourceData&, const Package&, NamePool&, …)`(bulk는 `Package::BulkBytes`로 읽는다)·`MeshSource::Store(MemoryBulkSource&, NamePool&, MeshSourceData&, …)`가 한다. 속성 이름(`Name` 필드)의 원문을 얻고 만드는 데 pool이 필요해 인자에 더했다(3.7절 2).

```cpp
ROCKS_ENUM() enum class MeshElement : uint8 { Invalid = 0, Vertex = 1, Corner = 2, Edge = 3, Polygon = 4, Triangle = 5, PolygonGroup = 6 };
ROCKS_ENUM() enum class MeshValueType : uint8 { Invalid = 0, Float = 1, Vec2 = 2, Vec3 = 3, Vec4 = 4, UInt32 = 5, Bool = 6, Name = 7, BoneInfluences = 8, SparseVec3 = 9 };

ROCKS_STRUCT()
struct MeshAttributeRecord {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() MeshElement element = MeshElement::Invalid;
    ROCKS_PROPERTY() MeshValueType type = MeshValueType::Invalid;
    ROCKS_PROPERTY() uint8 channels = 1;
    ROCKS_PROPERTY() BulkDataRef data;   // 값 타입별 배치(3.4.1절)
};

ROCKS_STRUCT()
struct MeshSourceData {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 formatVersion = 1;
    ROCKS_PROPERTY() uint32 vertexCount = 0;
    ROCKS_PROPERTY() uint32 cornerCount = 0;
    ROCKS_PROPERTY() uint32 edgeCount = 0;
    ROCKS_PROPERTY() uint32 polygonCount = 0;
    ROCKS_PROPERTY() uint32 triangleCount = 0;
    ROCKS_PROPERTY() uint32 polygonGroupCount = 0;
    ROCKS_PROPERTY() BulkDataRef topology;   // 위상 배열들(9.1절 고정 배치)
    ROCKS_PROPERTY() Array<MeshAttributeRecord> attributes;   // (element, name) 순 정렬
};
```

#### 3.4.1 값 타입별 bulk 배치

모두 little-endian이고 정렬 패딩이 없다. N = 그 요소의 수, C = 채널 수다.

| 값 타입 | 배치 | 길이 검증 |
|---|---|---|
| Float·Vec2·Vec3·Vec4·UInt32 | 채널 0의 N개, 채널 1의 N개, … (성분은 float32·uint32) | 원소 크기 × N × C |
| Bool | byte 0 또는 1, N × C | N × C, 0·1 이외는 오류 |
| Name | 정수 `uint32` 길이 + NFC UTF-8 byte, N × C개 | 끝까지 정확히 소비, 각 길이 ≤ `NameMaxBytes` |
| BoneInfluences | 시작 위치 `uint32` × (N + 1)(단조 증가, 첫 값 0) + `BoneInfluence` × 마지막 값 | 연속 차 ≤ `MeshMaxSourceInfluences`, 크기 일치 |
| SparseVec3 | 개수 `uint32` K + 번호 `uint32` × K(엄격 증가) + Vec3 × K | 번호 < N, 크기 일치 |

#### 3.4.2 기타

- `formatVersion`은 위상·값 배치의 버전이다. 바꿀 때 하위호환 읽기를 두지 않는다(초기 단계 원칙). 원본 메시 저장 데이터가 생긴 뒤에 바꾸면 그때 migration을 정한다.
- "원본 없음"(축소로 만드는 LOD 등)은 빈 `MeshSourceData`가 아니라 LOD 쪽의 명시 플래그 `hasSource`로 나타낸다(5.1절). 요소 수 0인 원본은 정상적인 빈 메시다.

### 3.5 속성 검증

`ValidateMeshSource`는 위상 불변식(3.1절)과 함께 다음을 확인한다. 하나라도 어기면 `InvalidMeshData`(로드 중이면 `CorruptPackage`)다. 통과한 원본은 빌드가 범위 밖 읽기를 하지 않음을 보장한다.

- 3.4.1절 길이·단조·범위 규칙.
- `BoneInfluence::boneIndex` < 메시 본 수(스킨 메시. 정적 메시에 스킨 속성이 있으면 경고 후 빌드가 무시).
- 속성 이름은 (element, Name 비교 규칙)으로 유일하다. 대소문자만 다른 두 이름은 같은 이름이다.
- 엔진 이름 속성(3.2절 표)은 정해진 값 타입·채널 범위여야 한다.

### 3.6 빌드 결과를 원본으로 두지 않는 이유

| 잃는 것 | UE 근거 |
|---|---|
| 다각형·n-gon·변·hard edge·부분 이름 | Dev/MeshBuilder/Private/StaticMeshBuilder.cpp:1649-1677(삼각형만 순회) |
| 다각형 묶음 이름(슬롯 키) | StaticMeshBuilder.cpp:1613-1623 |
| 선형 float 정점 색(빌드는 8비트 sRGB) | StaticMeshBuilder.cpp:1704-1708 |
| 법선·tangent·UV 정밀도(기본 8비트 tangent·half UV) | E/Public/Rendering/StaticMeshVertexBuffer.h:107-134 |
| 재계산·생성 결과와 원본의 구분(BuildScale3D·lightmap UV가 섞임) | Dev/MeshBuilder/Private/MeshDescriptionHelper.cpp:34-130, StaticMeshBuilder.cpp:1596 |

### 3.7 구현에서 정한 점(MA1)

MA1 구현(모듈 `Engine/Source/Runtime/MeshSource`) 중 위 절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다.

1. **정점 위치는 엔진 이름 속성 `Position`(정점, Vec3, 1)으로 저장한다.** 9.1절 위상 blob에 위치 배열이 없어 위치를 담을 곳이 정해지지 않았다. UE도 위치를 정점 속성(`MeshAttribute::Vertex::Position`)으로 둔다. 속성 레코드 하나 = bulk 하나(MA-D2)를 그대로 따르고 위상 blob은 번호만 담게 한다. 이 레코드는 정점 수가 0이어도 반드시 있어야 하며(없으면 `CorruptPackage`), 메모리 형태 `MeshSource`는 위치를 위상과 함께 따로 갖는다(`Positions()`). 그래서 속성 목록에 정점 `Position`을 더하는 것은 이름 중복이다. 버린 대안: 위상 blob 0번 배열로 넣기(위상 blob이 번호 아닌 값을 담게 되고 UE 모델과 달라진다).
2. **Load·Store 인자.** `Load(const MeshSourceData&, const Package&, NamePool&, MeshDiagnostic*)`, `Store(MemoryBulkSource&, NamePool&, MeshSourceData&, MeshDiagnostic*)`다. 레코드 이름이 실행 중 pool 토큰(`Name`)이라 원문을 읽고(`AcquireNameFieldText`) 만들 때(`TryIntern`) pool이 필요하다. bulk 쓰기 경로는 AssetRuntime의 기존 `PackageBulkSource`(writer가 저장 전 ref로 raw byte를 읽고 크기·SHA-256을 대조한다)이며, 메모리에 raw byte를 모아 임시 bulkId를 주는 구현이 없어 가장 작은 공용 구현 `MemoryBulkSource`(AssetRuntime, 헤더 `AssetRuntime/MemoryBulkSource.h`)를 더했다. 텍스처·애니메이션 원본도 같은 것을 쓴다. 버린 대안: MeshSource 모듈 안 전용 bulk 저장소(같은 기능을 모듈마다 두게 된다).
3. **bulk는 늘 있다.** 위상과 모든 속성은 길이가 0이어도 bulk entry를 갖는다(0 byte entry는 패키지 포맷이 받는다). 빈 ref(bulkId 0)·읽을 수 없는 ref는 `CorruptPackage`다. 빈 ref를 "0 byte"로 읽는 특례를 두지 않아 저장 형태가 하나다.
4. **로드 판정 분담.** 로드는 배치를 해석하는 데 필요한 것을 본다: `formatVersion`(다르면 `UnsupportedVersion`, ErrorCode `Unsupported` — 하위호환 읽기가 없으므로), 요소 수 ≤ `MeshMaxElements`, 레코드 element·값 타입·채널(값 타입 범위 밖, 0채널, 가변 길이 값 타입의 채널 ≠ 1), 레코드 순서(9.2절 순서의 **엄격** 증가 — 같은 이름 byte 둘·역순은 손상), `Position` 레코드, 3.4.1절 길이·단조·상한(Name 각 길이 ≤ `MaxNameFieldBytes`, 영향 개수 ≤ `MeshMaxSourceInfluences`, 희소 번호 < N·엄격 증가), 희소 항목이 비트 단위 `+0.0` 셋이면 손상(저장 형태는 0이 아닌 것만 담는다). 그다음 `ValidateMeshSource`를 불러 내용 불변식(번호 범위·변·다각형·삼각형·위치 유한·속성 이름·엔진 속성·값)을 보며, 로드 중 실패는 모두 `CorruptPackage`(원래 판정은 `MeshDiagnostic::issue`에 남는다)다. 할당·pool 오류는 진단 코드 없이 그 오류 그대로다.
5. **엔진 이름 속성 판정.** 이름은 Name 비교 규칙(`NameTextEquals` — ASCII 대소문자 무시)으로, 접두(`Morph-`·`MorphNormal-`·`SkinWeights-`)는 ASCII 대소문자 무시로 맞춘다. 엔진 이름과 맞는 속성은 3.2절 표의 요소·값 타입·채널이어야 한다(다른 요소에 붙은 `Normal`도 오류 — 이름만 같고 뜻이 다른 속성을 막는다). 접두 뒤가 빈 이름(`Morph-`)도 오류다. 그 밖의 사용자 속성은 Float·Vec2·Vec3·Vec4·UInt32·Bool·Name이면 채널 1–255, BoneInfluences·SparseVec3이면 채널 1이다(3.4.1절 배치가 채널 하나만 정한다).
6. **스킨 가중치.** 한 정점 안 같은 boneIndex 둘은 `InvalidMeshData`다(UE `FBoneWeights`도 같은 본을 합쳐 담는다). boneIndex 범위 검사는 본 수를 아는 호출자가 `MeshValidateOptions::boneCount`로 준 때만 한다(원본 메시는 본 계층을 모른다 — 6.3절 빌드가 넘긴다). 정규화(`SanitizeMeshSource`)는 영향이 있는데 합이 0이면 비례 배분할 수 없어 `InvalidMeshData`다(UE는 `ensure`로 막는 경우다 — BoneWeights.h `NormalizeWeights`). 영향 순서는 바꾸지 않는다.
7. **값 정리 `SanitizeMeshSource`.** 먼저 `ValidateMeshSource`를 부르고(실패하면 아무것도 바꾸지 않는다), 정규화 합 0을 먼저 모두 확인하고 경고 칸을 미리 잡은 뒤 값을 고친다 — 실패하면 원본은 그대로다. 법선·tangent를 0 벡터로 고치는 것은 경고를 내지 않는다(빌드가 다시 계산하므로 3.3절이 경고를 정하지 않았다). UV·색 비유한값과 UV 범위는 속성마다 경고 하나에 고친(해당) 값 수를 담는다.
8. **기본값.** 요소를 더하면 그 요소의 속성 값은 0(Name은 빈 문자열 = None, 영향은 없음, 희소는 0)이고, 엔진 `Color`만 UE처럼 (1, 1, 1, 1)이다.
9. **가변 길이 값의 메모리 형태.** Name·BoneInfluences는 요소마다 (offset, 개수) 범위와 덧붙이기 저장소를 갖는다. 같은 길이 이하로 다시 쓰면 제자리, 더 길면 저장소 끝에 덧붙인다(앞 자리는 Store가 압축할 때까지 남는다). 요소를 하나씩 더하고 값을 순서대로 쓰는 임포트가 O(전체)로 끝나게 하려는 것이다. 버린 대안: 저장 배치 그대로 메모리에 두기(중간 값을 바꾸면 뒤를 모두 옮겨 O(N²)).
10. **진단.** 실패·경고는 `MeshDiagnostic{code(AssetDiagnosticCode), error, issue(MeshIssue), element, attribute, index, count}`로 돌려준다. `code`가 AssetSystem 4.6절 진단 이름이고 `issue`는 어긴 규칙 하나를 가리키는 세부 분류다(시험이 정확히 그 규칙이 걸렸는지 대조한다). `ToAssetDiagnostic`이 4.6절 `AssetDiagnostic`으로 옮긴다. MA1이 등록한 코드는 `InvalidMeshData`·`NonFiniteAttribute`·`UvOutOfRange`·`NonNormalizedWeights`·`TooManyInfluences`다(나머지 11절 코드는 MA2 이후 단계가 등록한다).
11. **위상 조회 `MeshTopologyIndex`.** `ValidateMeshSource`를 통과한 메시에서 만든다(통과하지 않은 메시는 계약 위반 — 범위 단언). 다각형 → 삼각형, 묶음 → 다각형, 정점 → 코너·변, 코너 → 삼각형·다각형, 변 → 다각형(퇴화 변은 건너뜀)과 `FindEdge`를 준다. 만들기 실패는 할당 오류뿐이다.
12. **로드 할당은 bulk 크기에 묶는다(리뷰 반영, 개정 13).** 요소 수 다섯(정점·코너·변·다각형·삼각형)은 위상 blob·`Position` bulk 길이 판정이 할당보다 먼저라 변조된 수가 큰 할당을 만들지 못한다. 그렇지 않던 두 곳을 고쳤다. (1) Name 값 bulk는 값마다 길이 word가 있으므로 N × C × 4 byte보다 짧으면 범위 표를 잡기 전에 `AttributeLength`다(정점 Name 속성은 이름 순서상 `Position`보다 먼저 읽힐 수 있다 — 예: "Alpha"). (2) 다각형 묶음 수는 어느 bulk에도 묶이지 않아(속성 없는 묶음은 bulk가 없다) 크게 써도 로드·검증을 지나고 빌드가 묶음·섹션·위상 표를 그 수만큼 만들었다. 그래서 이름 있는 상한 `MeshMaxPolygonGroups`(65535)를 두고 로드(`ElementCountTooLarge`)와 `AddPolygonGroup`(`CapacityExceeded`)이 지킨다. 묶음은 머티리얼 섹션 하나라 65535는 섹션·슬롯 번호를 16비트로 담는 값이다(UE 고전 정적 메시에는 섹션 수 상한이 없고 Nanite는 64 — NaniteBuilder.h `MaxSectionArraySize`). 버린 대안: `≤ max(다각형 수, 작은 상수)`(빈 묶음을 허용하는 UE 의미와 겹쳐 규칙이 복잡해진다).

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

생성 LOD 판정: UE는 `!IsMeshDescriptionValid(i) || IsReductionActive(i)`다(E/Private/StaticMesh.cpp:9772-9773). UE의 "축소 활성"은 축소 모듈이 있어야 하고, LOD 그룹이 덮어쓴 설정과 캐시된 정점·삼각형 수로 판정한다(StaticMesh.cpp:3532-3550). Rocks는 축소기가 없는 동안 "`hasSource`가 거짓이거나 축소 설정이 기본값이 아님"으로 근사한다. 생성 LOD는 축소기가 생길 때까지 빌드에서 빠지고 `UnsupportedSetting` 경고를 낸다. 단, 임포터가 다른 엔진에서 만든 결과 메시를 함께 준 경우(`bakedSource`, 5.1절)는 그것을 쓴다.

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
- `MeshUvDensity`는 UE `FMeshUVChannelInfo`(bInitialized·bOverrideDensities·LocalUVDensities[4])에 대응한다. 빌드가 계산하고, [텍스처 스트리밍](TextureStreaming.ko.md)이 AssetSystem에 남긴 메시 UV 밀도 요구를 채운다.

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

- 형상의 의미(중심·회전·전체 길이)는 UE 필드를 그대로 따른다(SphereElem.h:25-29, BoxElem.h:28-44, SphylElem.h:28-40, TaperedCapsuleElem.h:21-45, ConvexElem.h:37-51). [물리 어댑터](PhysicsJoltAdapter.ko.md)가 Jolt 형상으로 옮긴다. 이 문서는 데이터만 정한다.
- 테이퍼드 캡슐의 `Width`·`bOneSidedCollision`은 넣지 않고, 값이 기본값이 아니면 임포트 경고를 낸다.
- level set·ML level set·skinned 형상(AggregateGeom.h:28-52)은 받지 않는다(`UnsupportedCollisionShape` 경고, 개수 포함).
- 볼록 형상의 쿠킹(Jolt 형상 직렬화)은 물리 단계다.

### 4.7 LOD 화면 크기와 플랫폼별 값

LOD 화면 크기(UE `FPerPlatformFloat ScreenSize`, StaticMeshSourceData.h:177-182)와 최소 LOD는 플랫폼별 값이다. 텍스처의 플랫폼별 설정과 같은 장치(기본값 + TargetProfile 이름별 덮어쓰기 표, AssetSystem 12.4절)를 쓴다. `autoComputeLodScreenSize`(UE `GetAutoComputeLODScreenSize()` — 멤버 직접 접근은 5.7부터 deprecated, StaticMesh.h:720)와 `lodGroup Name`도 둔다. 메시 LOD 그룹 표는 TargetProfile에 텍스처 LOD 그룹과 나란히 둔다(요구 MR9).

### 4.8 구현에서 정한 점(MA3a)

MA3a 구현(`Mesh` 모듈의 `Mesh/SimpleCollision.h`) 중 4.6절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다. UE 근거는 E/Classes/PhysicsEngine/ShapeElem.h·SphereElem.h·BoxElem.h·SphylElem.h·TaperedCapsuleElem.h·ConvexElem.h·AggregateGeom.h, E/Private/Collision/KAggregateGeom.cpp(생성자 기본값), E/Classes/Engine/EngineTypes.h(`ECollisionEnabled`)다.

1. **타입.** 형상 struct 여섯(`CollisionSphere`·`CollisionBox`·`CollisionCapsule`·`CollisionTaperedCapsule`·`CollisionCylinder`·`CollisionConvex`)은 공통 필드(`name`·`contributeToMass` 기본 참·`collisionEnabled` 기본 `QueryAndPhysics`·`restOffset` 기본 0 — UE `FKShapeElem`)를 각자 갖는다. 공통 기반 struct로 상속하지 않는 까닭은 UE도 형상마다 독립 USTRUCT 필드를 저장하고, 필드 순서가 저장 의미에 들어가지 않아 기반을 둘 이점이 없기 때문이다(버린 대안: `CollisionShapeBase` 상속 — 필드 경로가 형상마다 같아지는 것 말고는 얻는 것이 없다). 크기 기본값은 UE 생성자와 같다(구 반경 1, 상자 1·1·1, 캡슐 반경 1·길이 1, 테이퍼드 캡슐 1·1·1). 실린더 기본값은 캡슐과 같게 반경 1·길이 1로 둔다. `CollisionEnabled`는 UE 값 여섯(`NoCollision`·`QueryOnly`·`PhysicsOnly`·`QueryAndPhysics`·`ProbeOnly`·`QueryAndProbe`)을 같은 번호로 둔다. `CollisionComplexity`는 4.6절 넷이고, `StaticMesh` 필드 연결은 MA2b다.
2. **크기 규칙(UE 의미, 물리 사상 가능성 기준).** 모든 실수는 유한이어야 한다. 구 반경 > 0, 상자 `size` 세 성분 > 0, 캡슐 반경 > 0·길이 ≥ 0(0이면 구와 같다 — UE가 받는 값), 테이퍼드 캡슐 두 반경 > 0·길이 ≥ 0, 실린더 반경 > 0·길이 > 0(길이 0은 원판이라 축을 정할 수 없다 — [Cylinder 형상 설계](CylinderShape.ko.md) 3.3절), `restOffset`은 유한(부호 제한 없음 — UE도 음수를 받는다). 회전(`Rotator<double>`, 도)은 유한이면 범위를 두지 않는다(UE 편집 범위 ±360은 UI 제한이다). UE는 이 값을 검사하지 않고 물리 생성 때 잘라 쓰지만, Rocks는 데이터 단계에서 거부해 물리 어댑터가 받는 값을 좁힌다.
3. **볼록 형상.** 점은 `Vec3d`(little-endian double 셋, 24 byte), index는 `uint32`(4 byte) bulk이고 bulk는 늘 있다(0 byte index도 entry — 3.7절 3과 같다). 점 수는 `CollisionConvexMinVertices`(4 — 부피가 있는 hull의 최소) 이상 `CollisionConvexMaxVertices`(65535) 이하, index 수는 3의 배수, 각 index < 점 수, 삼각형 수 ≤ 2 × 점 수 − 4(닫힌 볼록 다면체 삼각 분할의 상한 — Euler 공식)다. index가 비어도 된다(UE `IndexData`는 hull에서 다시 만드는 값이다). `transform`은 성분 유한, 회전 |길이² − 1| ≤ `CollisionRotationNormalizedTolerance`(0.01, UE `THRESH_QUAT_NORMALIZED`), 크기 세 성분 ≠ 0이다. UE의 `ElemBox`(점에서 다시 계산하는 캐시)는 저장하지 않는다. 메모리 형태는 `ConvexGeometry{vertices, indices}`이고 `StoreConvexGeometry`·`LoadConvexGeometry`가 bulk와 오간다. 점 수 상한은 데이터 상한이고 Jolt hull 점 수 한도(256)는 쿠킹(물리 단계)이 다룬다.
4. **형상 수 상한.** 형상 수 합 ≤ `CollisionMaxElements`(65535)다(compound 자식 번호를 16비트로 담을 수 있게).
5. **검증 함수.** `ValidateSimpleCollision`은 값 필드만 본다(형상 수, 형상 종류 순서 구·상자·캡슐·테이퍼드·실린더·볼록, 배열 번호 순). bulk는 `ValidateSimpleCollisionBulk`(볼록 형상마다 `LoadConvexGeometry`)가 본다. 위반은 새 진단 `InvalidCollisionData`(오류 — 11절 표에 더했다)이고 결과는 `CollisionDiagnostic{code, error, issue(CollisionIssue), shape(CollisionShapeKind), index, related, count}`다. 로드 중 bulk 배치 위반은 `CorruptPackage`다.
6. **받지 않는 형상 진단.** `UnsupportedCollisionShape`(경고)는 임포터가 낸다. 형식은 `MakeUnsupportedCollisionWarning(feature, count)` — issue `UnsupportedFeature`, `related` = `UnsupportedCollisionFeature`(level set·skinned level set·ML level set·skinned triangle mesh, 테이퍼드 캡슐 `Width` ≠ 0·`bOneSidedCollision` — 4.6절의 "기본값이 아니면 경고"를 같은 코드로 낸다), `count` = 버린 형상(또는 그 값을 가진 테이퍼드 캡슐) 수다.
7. **결정성.** 검증은 첫 위반이 입력만으로 정해진다. 부동소수점 계산은 사칙연산과 비교뿐이다.

## 5. 정적 메시 `StaticMesh`

### 5.1 클래스

```text
StaticMesh : StreamableRenderAsset
  EditorOnly:
    sourceLods         : StructRefArray<StaticMeshSourceLod {
                           hasSource bool, source MeshSourceData,
                           bakedSource MeshSourceData,          // 다른 엔진이 축소로 만든 결과(교환 경로), 없으면 비움
                           build MeshBuildSettings, reduction MeshReductionSettings, screenSize(플랫폼별) }>
    autoComputeLodScreenSize, lodGroup
    distanceFieldReplacementMesh : ObjectRef<StaticMesh>         // Build 의존(보존)
  저장(쿠킹에도 남음):
    materialSlots      : StructRefArray<MeshMaterialSlot>
    sectionInfo        : Array<MeshSectionInfoEntry { lod, section, MeshSectionInfo }>
    sockets            : Array<MeshSocket>
    simpleCollision, complexity, lodForCollision, complexCollisionMesh
    lightMapResolution int32, lightMapCoordinateIndex int32           // 보존(라이트맵 단계)
    positiveBoundsExtension, negativeBoundsExtension : Vec3d
    minLod(플랫폼별) int32
    virtualGeometry    : VirtualGeometrySettings                    // 보존(UE FMeshNaniteSettings 필드 이름 집합, 계획 13.2절)
  쿠킹 데이터:
    renderData         : StaticMeshRenderData
```

- `sourceLods`의 원소는 참조가 없지만, EditorOnly 원소가 참조를 갖게 될 확장(LOD별 대체 메시 등)을 미리 막지 않도록 `StructRefArray`로 둔다. 원소 struct에 `ObjectRefArray`를 둘 수 없는 제한(ReflectionExtensions RF 결정 15)을 지킨다.
- UE의 고해상도 원본(Nanite hi-res, `GetHiResSourceModel` — StaticMesh.h:1973-1974)은 VirtualGeometry 단계에서 `hiResSource`로 더한다.

### 5.2 빌드 순서(MeshTools `StaticMeshBuilder`)

LOD마다 원본 사본에 다음 순서로 적용한다. UE `FStaticMeshBuilder`의 순서(StaticMeshBuilder.cpp:933-951, MeshDescriptionHelper.cpp:34-130, BuildVertexBuffer :1560~)를 따른다.

1. 원본 로드·검증(3.1·3.5절). 실패하면 빌드 실패.
2. 값 정리(3.3절).
3. 삼각형 법선·tangent 계산. 삼각형을 지우지 않는다(UE `ComputeTriangleTangentsAndNormals`도 지우지 않는다 — StaticMeshOperations.cpp:137-235).
4. 겹치는 코너 찾기(UE와 같은 위치 임계값).
5. 코너 법선·tangent 계산. 플래그는 겹친 코너 법선 섞기 + (가중 법선) + (법선 재계산) + (MikkTSpace) + (퇴화 무시) + (tangent 재계산)이다. 재계산 플래그가 꺼져 있어도 0 벡터인 코너는 계산한다.
6. (보존 설정) lightmap UV 생성 — 후속.
7. (보존 설정) 축소 — 후속.
8. `buildScale` 적용: 위치에 곱하고, tangent는 크기를 곱한 뒤 정규화, 법선은 역전치로 변환한 뒤 정규화한다(UE `ScaleStaticMeshVertex`와 같은 식 — StaticMeshBuilder.cpp:1596, 1693). 성분이 음수여도 winding을 뒤집지 않는다(UE와 같다). 음수 성분이 홀수 개면 `NegativeBuildScale` 경고(계획 13.1절 winding 진단).
9. 정점 버퍼 구성: 삼각형 순서로 코너를 렌더 정점으로 옮긴다. `removeDegenerates`면 두 꼭짓점 위치가 `MeshPointsSameThreshold`(UE `THRESH_POINTS_ARE_SAME`) 안에서 같은 삼각형을 뺀다(UE 판정과 같다 — StaticMeshBuilder.cpp:1644-1674. 면적 0 판정이 아니다). 렌더 정점 용접은 위치·법선·tangent·UV·색을 UE와 같은 임계값으로 비교한다(:1724). 임계값 상수 값은 UE 원본으로 확정했다(5.4절 4).
10. 섹션 구성: 다각형 묶음 순서로 삼각형을 모으고, 슬롯 대응(4.3절)으로 머티리얼 번호를 정한다.
11. 정점 cache·fetch 최적화(meshoptimizer 어댑터, 계획 13.1절. 어댑터가 생기기 전에는 원본 순서). 결과 순서는 결정적이어야 한다.
12. bounds 계산(상자 + 구, 경계 확장 적용).
13. 쿠킹 형식으로 패킹(5.3절).

결정성: 같은 입력·설정·도구 버전·프로필이면 결과 bytes가 같아야 한다(AssetSystem 9·10절). 부동소수점 계산은 쿠킹 결정성 환경(`RunWithDeterministicFloatingPoint`) 안에서 하고, 초월 함수는 Core 결정적 수학만 쓴다(MikkTSpace 패치 포함 — 결정 MA-D5). UE와의 결과 일치는 비트 단위로 보장하지 않는다. 노멀·tangent를 UE와 같게 받아야 하는 시험 콘텐츠는 교환 형식의 "UE 준비 원본" 모드를 쓴다([Unreal 교환 형식](UnrealExchange.ko.md) 5.3절).

### 5.3 쿠킹 렌더 데이터

```text
StaticMeshRenderData {
  lods   : Array<MeshRenderLod>
  bounds : { box Box<double>, sphereRadius double }
}
MeshRenderLod {
  screenSize      : float                         // 쿠킹 플랫폼 값
  vertexCount     : uint32
  positions       : MeshStream { format = Float3 }
  tangentBasis    : MeshStream { format = Byte4Snorm × 2 | Short4Snorm × 2 }   // tangent·법선(w = 쌍법선 부호)
  texCoords       : Array<MeshStream>              // UV 두 채널을 한 속성에 묶음(Half4 | Float4), 홀수 채널의 마지막은 Half2 | Float2
  colors          : MeshStream { format = 프로필별(Bgra8Unorm | UByte4Unorm) }   // 정점 색이 모두 흰색이면 없음
  indices         : { indexFormat uint32,        // GraphicsTypes IndexFormat 번호(UInt16 = 1, UInt32 = 2), 로드 때 검증
                      data BulkDataRef }
  sections        : Array<MeshRenderSection { materialIndex, firstIndex, triangleCount, minVertex, maxVertex, castShadow }>
}
MeshStream { uint32 vertexFormat;  // GraphicsTypes VertexFormat 번호. 로드 때 범위 검증
             BulkDataRef data }
```

- 정점 형식은 GraphicsTypes `VertexFormat`의 고정 번호표다(VertexFormat.h:49-70). 번호를 `uint32`로 저장하고 로드 때 검증한다(enum 순서 값을 직렬화하지 않는 RenderBase 10.4절 규칙). `PackedNormal` 패킹은 Core `VertexPacking.h`를 쓴다.
- 정점 색은 8비트 sRGB로 부호화한다(UE와 같다 — StaticMeshBuilder.cpp:1704-1708). 형식은 쿠킹 프로필에 따라 D3D·Vulkan은 `Bgra8Unorm`, OpenGL ES는 `UByte4Unorm`이다(VertexFormat.h:43 주석, RenderBase 10.4절).
- UV 두 채널을 한 정점 속성에 묶는 이유: UV 8채널 + 영향 8개 + tangent·색·위치를 각자 속성으로 두면 정점 속성 상한 16(RenderBase 10절)을 채워 인스턴스 stream을 둘 자리가 없다. UE도 UV를 쌍으로 묶는다.
- index 폭은 정점 수가 `MeshIndex16MaxVertices`(= 65535) 이하면 16비트다.
- LOD 하나가 mip 하나처럼 스트리밍 단위다(StreamableRenderAsset 기반). LOD별 bulk 묶음과 상주 LOD 수 규칙은 텍스처 스트리밍의 메시 확장으로 렌더 세션이 정한다(요구 MR3).
- 렌더러에 넘기는 정점 입력 타입(stream 묶음의 이름·셰이더 입력 대응)은 RenderBase 10절 몫이다(요구 MR1).

### 5.4 구현에서 정한 점(MA2a)

MA2a 구현(모듈 `Engine/Source/Runtime/Mesh`, `Engine/Source/Developer/MeshTools`, ThirdParty `Engine/Source/ThirdParty/MikkTSpace`, Core `DeterministicMath::Acos`) 중 위 절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다.

1. **API.** `MeshTools/StaticMeshBuilder.h`의 순수 함수 `BuildStaticMeshLod(const StaticMeshBuildInput&, StaticMeshBuildResult&, MeshDiagnostic*)`다. 입력은 원본 메시 포인터, `MeshBuildSettings`, 슬롯마다의 importedSlotName 목록(`slotNames` — 슬롯 머티리얼 참조는 MA2b), 섹션 정보 덮어쓰기(`MeshBuildSectionInfo{materialIndex, castShadow}` — `MeshNoIndex`면 슬롯 대응 결과), bounds 확장 두 벡터(StaticMesh 필드를 MA2b가 넘긴다), 임시 할당기다. 결과는 정점 속성별 배열(위치·법선·tangent·쌍법선 부호·UV·선형 색), `uint32` index, 섹션, bounds(상자 최소·최대 + 구 반지름, 구 중심 = 상자 중심), 경고 배열이다. UV는 채널 우선(`texCoords[c × 정점 수 + v]` — 5.3절 쌍 묶기 패킹이 채널 단위라서), 색은 원본에 `Color`가 있을 때만 채운다(전부 흰색이면 stream을 빼는 판정은 5.3절 패킹 몫). 실패하면 결과는 그대로다(지역 결과에 만든 뒤 옮긴다). 버린 대안: 정점 구조체 배열(UV 채널 수가 메시마다 달라 고정 구조체로 담으면 빈 칸이 생긴다).
2. **모듈 의존(지금).** `Mesh`는 `MeshBuildSettings`(리플렉션 값 struct, 4.1절 필드 전부)와 진단용 필드 번호 `MeshBuildSettingField`만 가지며 ObjectRuntime·CoreTypeBindings(`Vec3d` 미러)에 의존한다. `MeshTools`는 MeshSource·Mesh(public)와 DeterministicMath·MikkTSpace(private)에 의존한다. 8절 표의 나머지 의존(AssetRuntime·Animation·Material·AssetCooker 등)은 그 기능이 들어오는 MA2b·MA5에서 더한다.
3. **원본 사본.** 5.2절 "원본 사본"은 위상 전체와 빌드가 쓰는 코너 속성(Normal·Tangent·BinormalSign·Color·TexCoord)만 복사한 `MeshSource`다. 앞의 셋은 원본에 없으면 0으로 더한다. 스킨·morph·사용자 속성은 정적 메시 빌드가 쓰지 않아 복사하지 않는다 — 그래서 값 정리(`SanitizeMeshSource`)가 스킨 가중치 정규화 경고나 합 0 오류를 내지 않는다. 3.5절의 "정적 메시에 스킨 속성이 있으면 경고"는 11절에 코드가 없어 MA2a는 내지 않는다(MA5에서 스킨 메시 빌드와 함께 정한다). 원본 검증은 사본이 아니라 원본 전체에 `ValidateMeshSource`로 한다(로드를 거치지 않은 메모리 메시도 받으므로). 값 정리 경고의 속성 번호는 원본 번호로 바꿔 돌려준다.
4. **임계값(UE 원본으로 확정).** `MeshPointsSameThreshold` = 0.00002(UE `THRESH_POINTS_ARE_SAME`), `MeshNormalsSameThreshold` = 0.00002(`THRESH_NORMALS_ARE_SAME`), `MeshUvsSameThreshold` = 1/1024(`THRESH_UVS_ARE_SAME`) — UnrealMathUtility.h·MeshBuild.h. 비교는 UE와 같이 성분마다 `|a − b| ≤ 임계값`이다. 위치 임계값은 `removeDegenerates`면 위 값, 아니면 0이고(UE `VertexComparisonThreshold`), 퇴화 판정·겹치는 코너·렌더 정점 용접이 같은 값을 쓴다. 그래서 `removeDegenerates`를 꺼도 두 꼭짓점 위치가 **정확히** 같은 삼각형은 빠진다(UE와 같다 — 5.2절 9단계의 "removeDegenerates면"을 이렇게 읽는다). 내부 계산은 UE의 `SMALL_NUMBER`(1e-8)·`KINDA_SMALL_NUMBER`(1e-4)·`DELTA`(1e-5)·`MIN_flt`를 이름 있는 상수로 둔다. 9.3절 너겟 키용으로 `MeshBuildThresholdVersion`·`MeshMikkTSpacePatchVersion`(둘 다 1)을 둔다.
5. **삼각형·코너 법선과 tangent.** UE `GetTriangleTangentsAndNormalsWithUV`·`WithNoUVs`(UV 채널이 없으면 Duff·Frisvad 기저)와 `ComputeTangentsAndNormals`의 정점 단위 묶음을 그대로 옮긴다: 정점마다 그 정점을 쓰는 삼각형을, 정점에서 나가는 변이 soft인 쌍끼리 이어 묶고, 묶음의 (가중) 삼각형 법선 합을 0 벡터 코너에 쓰고, MikkTSpace를 쓰지 않으면 UV0 값별 tangent·쌍법선 합을 Gram–Schmidt로 직교화한 뒤 행렬식 부호를 쓴다. 변 목록에 없는 쌍(다각형 안 대각선)은 soft다 — UE는 다각형 내부 변을 soft 변으로 만든다. 묶음 안 합산 순서는 삼각형 번호 오름차순이고(UE는 해시 표 순서라 bit 일치는 보장하지 않는다 — 5.2절), UV0 키는 −0을 +0으로 본 정확 비교다(UE `FNTBGroupKeyFuncs`). 가중치는 면적 × 코너 각이고 코너 각은 `deterministic::Acos`(입력을 [−1, 1]로 자른다 — UE `FMath::Acos`)다. UE의 `BlendOverlappingNormals` 플래그는 그 함수가 읽지 않아 효과가 없으므로 따로 구현하지 않는다(겹치는 코너는 용접에만 쓰인다).
6. **비유한 중간값.** 아주 큰 좌표의 넘침이 낸 비유한 법선·tangent는 0 벡터로 둔다(NaN을 남기면 호스트마다 기본 NaN bit가 달라 결정성이 깨진다). 크기를 곱한 위치가 비유한이면 빌드 실패다(7). bounds는 double로 계산해 넘치지 않는다.
7. **`buildScale` 검사.** UE처럼 float로 바꿔 쓴다. 성분이 float 범위 밖·0(−0, float로 바꿔 0이 되는 작은 값 포함)·NaN이거나 역수가 비유한(법선을 나눌 수 없다)이면, 또는 곱한 위치가 비유한이면 `InvalidSetting`(AssetSystem 4.6절에 있던 이름, issue `InvalidBuildScale`, index = 축) 실패다. UE는 검사하지 않아 NaN 법선이 나온다. 버린 대안: `UnsupportedSetting`(경고 뜻이다), `InvalidMeshData`(메시가 아니라 설정 문제다).
8. **MikkTSpace.** 저자 저장소(mmikk/MikkTSpace 3e895b49d05e, zlib)를 고정 해시로 들인다. UE 사본의 `m_bIgnoreDegenerates`(퇴화 삼각형 후처리 생략 — 성능용 확장)는 원본에 없어 넣지 않는다: 빠지는 삼각형의 코너도 이웃에서 tangent를 받을 뿐이고 렌더 정점에는 쓰이지 않는다. 패치(`Rocks.patch`, manifest `modifications`)는 `cos`(각 임계값)를 `CosPi(t/180)`로(같은 함수, 기본 180°에서 둘 다 −1), `acos`를 `deterministic::Acos`로, `malloc`·`free`를 호출자 allocator 훅으로 바꾸고, 원본 버그 하나를 고치고(해시 표 할당이 실패할 때 쓰는 `GenerateSharedVerticesIndexListSlow`가 찾은 번호를 저장하지 않아 −1을 쓰고 범위 밖을 읽는다 — UE 사본도 같다, 할당 실패 주입 시험이 찾았다), `-Wunused-but-set-variable`에 걸리는 세지 않는 계수기 둘을 지운다. 훅은 MeshTools가 C 연결로 정의한다(MikkTSpace를 링크하는 곳은 MeshTools뿐). 부호는 UE와 같이 MikkTSpace 부호의 반대이고, 코너를 여러 삼각형이 쓰면 마지막 삼각형의 값이 남는다(UE와 같다). 원본은 일부 할당 실패를 느린 경로로 넘기는데 그때도 결과가 같다(시험으로 확인). 면 번호를 `int`에 2비트 밀어 쓰므로 삼각형 수 상한은 `MeshMikkTSpaceMaxTriangles`(2^29 − 1)이고 넘으면 `CapacityExceeded`다. 패치 전 원본 대조용 글루 `MikkTSpaceReference.c`는 같은 원본을 원래 식(CRT `cos`·`acos`·`malloc`)으로 다른 이름에 다시 빌드한다(시험만 부른다).
9. **결정적 `Acos`(Core).** `DeterministicMath`에 `Acos(double)`를 더했다(CoreDataFoundations 2.2절). |x| ≤ ½은 π/2 − asin(x)의 asin Taylor 30항(계수는 컴파일 시점 IEEE 연산), 그 밖은 2·asin(√((1 − |x|)/2))이고 √는 고정 횟수 Newton 근사 뒤 z − s²(오차 없는 곱)로 보정한다(CRT·명령 sqrt를 쓰지 않는다 — 모듈 소스 정책). 정확도 상한 1 ulp(400비트 기준값 표), 기존 함수의 결과 bit는 그대로라 `Version`은 1을 유지하고 Acos 표본 digest를 따로 고정한다.
10. **렌더 정점 용접.** UE `BuildVertexBuffer`와 같이 겹치는 앞 wedge의 렌더 정점 가운데 wedge 번호 순 첫 일치를 쓴다. 비교 항목은 위치·법선·tangent·쌍법선(= cross(N, T) × 부호를 크기 적용 뒤 정규화)·색·UV 전 채널이다. 색은 UE처럼 8비트 sRGB로 비교하되 Rocks 정확 반올림 부호화(`LinearColor::ToSRGB`)를 쓰고, 출력은 먼저 나온 코너의 선형 float다. UE는 흰색 아닌 색이 처음 나오기 전에는 색을 비교하지 않아 색 있는 코너가 흰 정점에 합쳐질 수 있는데 이 동작은 따르지 않는다(Color 속성이 있으면 늘 비교한다). 쌍법선 부호 출력은 크기를 적용한 (tangent, 쌍법선, 법선)의 행렬식 부호다(UE 패킹의 `GetBasisDeterminantSign`).
11. **섹션과 슬롯.** 다각형 묶음마다 섹션 하나(빈 묶음도 삼각형 0 섹션 — UE와 같다). 슬롯 대응은 Name 비교 규칙의 첫 일치이고, None(빈 이름)은 빈 importedSlotName과 맞는다(UE FName None == None). `MaterialSlotFallback`(index = 묶음)은 대체 번호를 실제로 쓸 때만 낸다 — 섹션 정보가 머티리얼 번호를 덮어쓴 섹션은 내지 않는다.
12. **경고 순서.** 값 정리(NonFiniteAttribute·UvOutOfRange), `UnsupportedSetting`(보존 필드마다, 필드 순서, index = `MeshBuildSettingField`), `NegativeBuildScale`(count = 음수 성분 수), `MaterialSlotFallback`(묶음 순서). 보존 필드의 "기본값이 아님"은 실수 필드를 bit로 비교한다(−0.0도 다르다 — 저장 byte가 다르다).
13. **결정성.** 빌드 전체를 `RunWithDeterministicFloatingPoint` 안에서 하고(호출 전 FTZ가 켜져 있어도 같은 결과 — 시험), 초월 함수는 Core 결정적 수학만, √는 정확 반올림 명령(`math::Sqrt`)만 쓴다. 스레드를 쓰지 않으므로 12절의 "스레드 수 바꿔서" 시험은 대상이 없어 두지 않는다(스레드를 쓰게 되면 그때 더한다). 11단계는 원본 순서다.
14. **진단 등록.** `AssetDiagnosticCode`에 `MaterialSlotFallback`·`NegativeBuildScale`(메시)과 `UnsupportedSetting`·`InvalidSetting`(임포트·설정 — 4.6절에 이름만 있던 것)을, `MeshIssue`에 빌드 세부 규칙 넷(`MaterialSlotFallback`·`NegativeBuildScale`·`UnsupportedSetting`·`InvalidBuildScale`)을 더했다. 원본 검증 실패는 `ValidateMeshSource`의 진단 그대로, 할당 실패·용량 초과는 진단 코드 없이 그 오류다.
15. **시험 도우미.** MA1 퍼저의 무작위 메시 생성기(`RandomFuzzMesh`·`RandomFuzzAttributes`)를 `MeshSourceTestSupport.h`로 옮겨 빌드 퍼징이 같은 생성기를 쓴다(동작 불변 이동).
16. **겹치는 코너 정렬 키(리뷰 반영, 개정 13).** 처음 구현은 z만으로 정렬해 평면 메시(z가 모두 같다)에서 훑기가 O(n²)였다. UE `FindOverlappingCorners`는 `FIndexAndZ` 키 `0.30X + 0.33Y + 0.37Z`(float, 이 순서의 곱·합 — StaticMeshOperations.cpp)로 정렬하고 키 차가 임계값을 넘으면 끊는다. Rocks도 같은 식·연산 순서의 float 키를 쓰고, 같은 키는 wedge 번호 순으로 끊어 결정적이다(UE는 불안정 정렬이라 같은 키의 순서가 정해지지 않는다). 성분마다 |차| ≤ t인 두 점의 키 차는 계수 합(1.00) × t 이내라 끊는 조건이 일치 쌍을 놓치지 않는다(float 반올림 경계는 UE와 같다). 훑기의 비교 횟수를 내부 표에 세어 평면 대형 격자 시험이 결정적으로 단언한다.
17. **MikkTSpace 느린 경로 방어와 오브젝트 검사(리뷰 반영, 개정 13).** 느린 경로의 `index2rec` 시작값을 −1 대신 자기 index로 바꿨다(NaN 입력이면 `veq`가 자기 자신과도 거짓이라 아무것도 맞지 않는다 — 그때도 범위 밖 번호를 쓰지 않게). `Rocks.patch`·manifest를 갱신했다. 패치된 mikktspace 오브젝트의 외부 심볼에 CRT 초월 함수·`malloc`이 없는지 보는 오브젝트 검사(DeterministicMath의 `DeterministicMathObjectCheck`와 같은 방식)는 이번에 더하지 않았다: 그 검사는 ToolTests 네이티브 단계의 구성별 compile DB에서만 돌고, mikktspace 번역 단위는 `assert`(구성마다 다른 CRT 심볼 — `_wassert`·`__assert_fail`)와 `sqrtf`를 부르며 같은 폴더의 참조 글루(`MikkTSpaceReference.c`)는 일부러 CRT를 쓰므로, 구성·플랫폼별 허용 목록과 파일 제외 규칙을 따로 세워야 한다. 지금은 패치(정책이 역적용으로 원본 일치를 본다)와 훅 정의가 MeshTools 링크에만 있다는 점(훅이 없으면 링크가 실패한다)으로 지킨다. 물리 쿠킹이 같은 ThirdParty 검사를 필요로 할 때 일반화해 더한다.

## 6. 스켈레톤과 스킨 메시

### 6.1 본 계층 `BoneHierarchy`

```cpp
inline constexpr int32 NoParentBone = -1;

ROCKS_STRUCT()
struct BoneInfo {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;                         // 본의 안정 식별자(UE와 같이 이름으로 식별)
    ROCKS_PROPERTY() int32 parentIndex = NoParentBone;  // 루트만 NoParentBone
    ROCKS_PROPERTY(EditorOnly) String exportName;       // 대소문자를 보존한 원래 이름(UE ExportName)
};

ROCKS_STRUCT()
struct BoneHierarchy {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Array<BoneInfo> bones;
    ROCKS_PROPERTY() Array<Transform> referencePose;    // 부모 기준 local, double(UE FTransform)
};
```

- 불변식: 루트는 하나이고 번호 0이다. 나머지 본의 부모 번호는 자기 번호보다 작다. UE도 기본(`bOnlyOneRootAllowed = true`)에서 같은 검사를 한다(E/Public/ReferenceSkeleton.h:101, 214-220). 이름은 Core `Name` 규칙(ASCII 대소문자 무시)으로 유일하다. 기준 pose의 회전은 단위 quaternion이며 저장 전에 정규화한다(UE도 Add 때 정규화한다).
- 본 수 상한은 `uint16` boneIndex(3.2절) 때문에 `MeshMaxBones`(= 65535)다. UE `FBoneIndexType`도 uint16이다(RT/AnimationCore/Public/BoneIndices.h:7).
- 계획 15절의 "stable bone ID"는 본 이름이다. 메시·클립·스켈레톤 사이 대응은 이름으로 하고 번호는 각 에셋 안의 지역 번호다(UE와 같다).
- 계층은 원본 본만 담는다. 가상 본을 덧붙인 전체 계층은 로드 때 계산하며 저장하지 않는다(UE raw·final 구분 — ReferenceSkeleton.h:107-116, E/Private/Animation/ReferenceSkeleton.cpp:942-948 직렬화, 960-965 로드 때 final 구성).
- 계획 15절의 inverse-bind pose는 저장하지 않고 빌드가 기준 pose에서 계산해 쿠킹 데이터(6.3절)에 넣는다.

### 6.2 스켈레톤 `Skeleton`

```text
Skeleton : Asset
  bones               : BoneHierarchy
  retargetModes       : Array<BoneTranslationRetarget>    // bones와 같은 길이. { Animation, Skeleton, AnimationScaled, AnimationRelative, OrientAndScale }
  virtualBones        : Array<VirtualBone { sourceBone Name, targetBone Name, name Name }>   // 이름 접두사 "VB "(UE Skeleton.cpp:109)
  sockets             : Array<BoneSocket { name, boneName, relativeLocation, relativeRotation, relativeScale, forceAlwaysAnimated }>
  curves              : Array<SkeletonCurveInfo { name, isMaterial, isMorphTarget, linkedBones Array<Name>, maxLod uint8 }>
  slotGroups          : Array<AnimSlotGroup { name, slots Array<Name> }>
  retargetSources     : Array<NamedReferencePose { name, pose Array<Transform> }>
  compatibleSkeletons : Array<SoftObjectRef<Skeleton>>
  useRetargetModesFromCompatibleSkeleton : bool
```

- `retargetModes`는 **구현**(애니메이션 평가 단계에서 쓴다)이고, 슬롯 묶음·리타깃 원본·호환 스켈레톤은 **보존**(montage·리타깃 단계)이다.
- blend profile(UE `BlendProfiles`, Skeleton.h:526)은 blend 단계에서 더한다.
- 본 병합: 스킨 메시를 임포트할 때 메시 본이 스켈레톤에 없으면 스켈레톤에 더한다. 부모 사슬이 맞지 않으면(중간 본이 빠짐) `SkeletonMismatch`로 실패한다(UE `MergeBonesToBoneTree`·`DoesParentChainMatch`, Skeleton.h:786-797, 912-923). 병합은 스켈레톤 패키지를 바꾸므로 그 스켈레톤에 Build 의존을 건 클립이 다시 쿠킹된다(7.1절).

### 6.4 구현에서 정한 점(MA4a)

MA4a 구현(모듈 `Engine/Source/Runtime/Animation`) 중 6.1·6.2절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다.

1. **모듈과 파일.** `Animation`은 AssetRuntime(`Asset`·진단)과 CoreTypeBindings(`Transform`·`Rotator<double>`·`Vec3d` 미러 — 모두 있어 새 미러는 없다)에 의존한다. 8절의 Curves 의존은 클립(MA6)이 들어올 때 더한다. 리플렉션 헤더는 `Animation/BoneHierarchy.h`(`BoneInfo`·`BoneHierarchy`)와 `Animation/Skeleton.h`(`BoneTranslationRetarget`·`VirtualBone`·`BoneSocket`·`SkeletonCurveInfo`·`AnimSlotGroup`·`NamedReferencePose`·`Skeleton`)이고, 병합은 `Animation/SkeletonMerge.h`, 진단은 `Animation/AnimationDiagnostic.h`다. 모든 리플렉션 class·struct에 `ROCKS_GENERATED_BODY()`를 둔다.
2. **계층 불변식(`ValidateBoneHierarchy`).** 차례대로 본 수 ≤ `MeshMaxBones`(65535, 넘으면 `CapacityExceeded`), pose 길이 = 본 수, 본 번호 순으로 (본 0의 부모 = `NoParentBone`, 본 i ≥ 1의 부모 ∈ [0, i) — 두 번째 루트·자기·뒤 본·−2 이하는 모두 `ParentOrder`, 이름은 구조가 맞는 Normal 토큰 — None 거부, pose 성분 유한, 회전 |길이² − 1| ≤ `BoneRotationNormalizedTolerance`(0.01, UE `THRESH_QUAT_NORMALIZED`)), 마지막에 이름 중복(같은 이름의 두 번째 본 번호가 가장 작은 쌍, related = 앞 본)이다. 이름 비교는 Name 토큰 비교(같은 pool, ASCII 대소문자 무시)라 pool 없이 한다. 빈 계층은 정상이다. `exportName`은 검사하지 않는다(원래 이름의 대소문자 보존용 EditorOnly 표시값이라 이름과 다를 수 있다 — UE도 검사하지 않는다).
3. **pose 정규화(`NormalizeBoneReferencePose`).** 6.1절 "저장 전에 정규화"를 함수로 둔다: 길이² = 1이 아닌 유한 회전을 1/√길이²로 나누고, 길이²가 `BoneRotationMinimumLengthSquared`(1e-8, UE `FQuat::Normalize`의 `UE_SMALL_NUMBER`) 미만이면 단위 회전으로 바꾼다. 비유한 회전은 그대로 둔다(검증이 거부한다). 저장 경로(임포트·병합 저장)가 부르는 것은 MA4b다.
4. **스켈레톤 불변식(`ValidateSkeleton`).** 계층 다음에 `retargetModes` 길이 = 본 수·값이 열거 안, 가상 본(이름은 "VB " 접두사로 시작하고 접두사보다 길다 — ASCII 대소문자 무시, UE `CheckVirtualBonePrefix`의 `StartsWith` 기본과 같다, 이름이 본·앞 가상 본과 겹치지 않음, (원본, 대상) 쌍이 앞 가상 본과 같지 않음 — UE `AddNewVirtualBone`, 원본·대상은 본 또는 **앞** 가상 본 — 사슬은 되지만 순환은 막는다), 소켓(이름 유일, 본은 원본 본 또는 가상 본, 상대 변환 유한), 커브(이름 유일, `linkedBones`는 원본 본만 — UE `FCurveMetaData::LinkedBones`가 raw 본 참조다), 슬롯 묶음(이름 유일, 슬롯 이름은 모든 묶음을 통틀어 유일 — UE에서 슬롯은 묶음 하나에만 속한다), 리타깃 원본(이름 유일, pose 유한 — 보존 필드라 길이는 보지 않는다) 순이다. `compatibleSkeletons`는 soft 참조라 보지 않는다. 가상 본 접두사는 원문이 필요해 `ValidateSkeleton`만 `NamePool`을 받고, 원문을 읽지 못하면(다른 pool·stale 토큰) 그 pool 오류다.
5. **진단.** 6.1·6.2절 불변식 위반은 새 코드 `InvalidSkeletonData`(오류 — 11절 표에 더했다)이고, 병합의 부모 사슬 실패는 `SkeletonMismatch`다. 둘을 `AssetDiagnosticCode`에 등록했다. 결과는 `SkeletonDiagnostic{code, error, issue(SkeletonIssue), role(Skeleton·Mesh), index, related}`이고 `ToAssetDiagnostic`이 4.6절 형식(detail = index)으로 옮긴다. 할당 실패는 진단 코드 없이 그 오류다. 버린 대안: 계층 위반도 `SkeletonMismatch`(병합 실패와 입력 손상을 가를 수 없다), `InvalidMeshData`(메시가 아니다).
6. **병합(`MergeBoneHierarchy`).** UE `MergeBonesToBoneTree`의 기본 경로(부모 사슬 검사 켬 — `a.AllowIncompatibleSkeletalMeshMerge` 끔)를 순수 함수로 옮긴다. 두 입력을 검증하고(진단의 role로 어느 쪽인지), 메시 본이 없으면 UE처럼 거부한다(`SkeletonMismatch`·`EmptyMeshHierarchy` — UE `IsCompatibleMesh`는 맞는 본이 하나도 없으면 거짓이고 `CreateReferenceSkeletonFromMesh`도 본이 없으면 만들지 않는다, 리뷰 반영 개정 13). 스켈레톤이 비었으면 결과는 메시 그대로(UE `CreateReferenceSkeletonFromMesh`)다. 아니면 메시 본마다 UE `IsCompatibleMesh`대로 같은 이름 본 또는 스켈레톤에 있는 가장 가까운 메시 조상에서 `DoesParentChainMatch`(부모 이름을 차례로 비교하고 한쪽이 루트에 닿으면 다른 쪽도 루트, 시작 본이 스켈레톤 루트면 두 루트 이름만 비교)를 한다. 실패는 `ParentChainMismatch`(index = 메시 본, related = 대조한 스켈레톤 본), 조상이 하나도 없으면 `MissingAncestor`다. 통과하면 스켈레톤 본을 번호 그대로 두고 없는 메시 본을 메시 순서로 뒤에 더한다(부모 = 메시 부모의 결과 번호 — 부모가 앞이라 늘 있다, pose = 메시 pose). 이미 있는 본은 이름이 대소문자만 다르면(같은 토큰 id, 다른 표시) UE `Rename`처럼 메시 표시와 `exportName`으로 바꾼다(`renamedBones`). UE의 둘째 단계(부모가 바뀐 본 옮기기)는 사슬 검사를 켠 경로에서 일어나지 않으므로 두지 않는다. 결과 본 수가 `MeshMaxBones`를 넘으면 `InvalidSkeletonData`·`TooManyBones`·`CapacityExceeded`다. 결과는 계층 + 메시 본 → 결과 본 대응표(`meshToMerged`)이고, 실패하면 out은 그대로다. `retargetModes` 등 스켈레톤의 본 길이 배열을 늘리는 것과 저장은 MA4b다(새 본의 리타깃 방식은 UE 기본 `Animation`).
7. **결정성.** 검증은 첫 위반 규칙과 위치가 입력만으로 정해지고(이름 중복은 정렬 뒤 가장 작은 두 번째 번호), 병합은 메시 순서를 따르므로 같은 입력이면 같은 결과다. 부동소수점은 정규화의 √ 하나뿐이다(IEEE 정확 반올림). 스레드를 쓰지 않는다.
8. **시험 환경.** 패키지 왕복은 AssetRuntime 시험의 `AssetEnv`에 `RegisterAnimationTypes`를 더 불러 실제 `Skeleton` 클래스로 한다(새 fixture 모듈을 두지 않는다 — 같은 의존 모듈 타입은 lease로 공유된다).

### 6.3 스킨 메시 `SkinnedAsset`·`SkeletalMesh`

```text
SkinnedAsset : StreamableRenderAsset (추상)
SkeletalMesh : SkinnedAsset
  skeleton            : ObjectRef<Skeleton>        // Hard
  bones               : BoneHierarchy              // 메시 자체 계층(스켈레톤의 부분집합, 순서가 달라도 됨)
  EditorOnly:
    sourceLods        : StructRefArray<SkeletalMeshSourceLod {
                          hasSource bool, source MeshSourceData, bakedSource MeshSourceData,
                          build SkinnedMeshBuildSettings, reduction MeshReductionSettings,
                          screenSize(플랫폼별), lodHysteresis float, lodMaterialMap Array<int32>,
                          bonesToRemove Array<Name>, bonesToPrioritize Array<Name>, weightOfPrioritization float }>
  materialSlots       : StructRefArray<MeshMaterialSlot>
  sockets             : Array<BoneSocket>          // 메시 전용 소켓(스켈레톤 소켓과 합쳐 쓴다)
  skinWeightProfiles  : Array<SkinWeightProfileInfo { name, defaultProfile(플랫폼별 bool), defaultProfileFromLod(플랫폼별 int32) }>
  positiveBoundsExtension, negativeBoundsExtension, minLod(플랫폼별)
  쿠킹 데이터:
    renderData        : SkeletalMeshRenderData
```

- `SkinnedMeshBuildSettings` = `MeshBuildSettings` + `useHighPrecisionSkinWeights`, `thresholdPosition`, `thresholdTangentNormal`, `thresholdUV`, `morphThresholdPosition`, `boneInfluenceLimit`(UE `FSkeletalMeshBuildSettings`, EngineTypes.h:3096-3164). 이 여섯은 **구현**이다. `bOptimizeForInstancing`은 넣지 않는다(UE 전용 렌더 최적화 플래그).
- `skinWeightProfiles`의 두 기본값 필드는 UE에서 플랫폼별 값이다(E/Public/Rendering/SkinWeightProfile.h:46, 50).
- 메시 본 ↔ 스켈레톤 본 대응 표(UE `FSkeletonToMeshLinkup`, Skeleton.h:47-65)는 로드 때 이름으로 만든다. 저장하지 않는다. 대응이 없으면 `NoParentBone`과 같은 표지(-1)다.
- physics asset(UE `PhysicsAsset`)은 물리 단계에서 별도 에셋 타입으로 더한다.
- 축소 LOD·bone 제거·bake pose는 **보존**이다.

**쿠킹 렌더 데이터.** 정적 메시의 `MeshRenderLod`에 다음을 더한 `SkinnedRenderLod`다.
- `boneInfluences`: 정점당 영향 `MeshCookedInfluencesLow`(= 4) 또는 `MeshCookedInfluencesHigh`(= 8)개(빌드 설정 `boneInfluenceLimit`에 따라 올림). 번호는 아래 LOD 통합 bone map의 위치이고, 통합 본이 `MeshBoneIndex8Limit`(= 256)개 이하면 `UByte4`, 넘으면 `UShort4`다. 가중치 `UByte4Unorm`/`UShort4Unorm`(`useHighPrecisionSkinWeights`).
- LOD 통합 bone map(`Array<uint16>`, 메시 본 번호의 정렬 목록) — LOD의 모든 섹션이 같이 쓴다. 섹션을 본 수로 나누지 않는다(RenderBase 결정 R18, UE 5.8과 같다 — 섹션당 최대 본 설정이 5.8에서 deprecated). 본 행렬은 렌더러가 typed buffer로 넘긴다.
- LOD별 필요 본 목록(`requiredBones`, 부모 포함 정렬)과 메시 본별 inverse-bind 행렬(기준 pose의 component 공간 역).
- morph target: 이름별 희소 (렌더 정점 번호, 위치 변화량 `Half4`, 법선 변화량 `Half4`) 목록. `morphThresholdPosition` 미만의 변화량은 뺀다.

영향 개수 줄이기: 원본 영향(최대 12)을 가중치 내림차순(같으면 본 번호 오름차순)으로 정렬해 쿠킹 상한(최대 8)까지 남기고 다시 정규화한다. 버린 영향이 있으면 정점 수를 세어 `InfluencesTrimmed` 정보 진단을 낸다. 8비트 가중치는 비례 배분 후 반올림하고 잔차(255와의 차)를 가장 큰 영향에 더한다. 모두 결정적이다.

### 6.5 구현에서 정한 점(MA5a)

MA5a 구현(`Mesh`의 `SkinnedMeshBuildSettings`, `MeshTools`의 스킨 빌드) 중 6.3절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다. UE 근거는 Dev/MeshUtilities/Private/MeshUtilities.cpp(`GenerateSkeletalRenderMesh`), SkeletalMeshTools.cpp(`BuildSkeletalMeshChunks`·`AreSkelMeshVerticesEqual`), Dev/SkeletalMeshUtilitiesCommon/Private/LODUtilities.cpp(morph 변화량·대체 가중치), RT/AnimationCore/Public/BoneWeights.h, E/Private/SkeletalMesh.cpp(`CalculateInvRefMatrices`·`CalculateRequiredBones`)다.

1. **API.** `MeshTools/SkinnedMeshBuilder.h`의 순수 함수 `BuildSkinnedMeshLod(const SkinnedMeshBuildInput&, SkinnedMeshBuildResult&, MeshDiagnostic*)`다. 입력은 원본 메시, 메시 본 계층(`BoneHierarchy` — 6.1절), `SkinnedMeshBuildSettings`, 슬롯 이름·섹션 정보·bounds 확장(5.4절 1과 같다), 임시 할당기다. 결과는 정적 메시와 같은 `StaticMeshBuildResult`(`mesh` — 렌더 정점 속성·index·섹션·bounds·경고)에 정점당 영향 칸 수, 가중치 합(255 또는 65535), 영향 배열(정점 × 칸, 본 번호는 bone map 위치), 가중치 프로필별 영향, LOD 통합 bone map, 필요 본 목록, 메시 본별 inverse-bind 행렬(`Mat4d`), morph target별 희소 변화량을 더한다. 프로필·morph target은 원본 속성 번호로 가리킨다(이름 원문은 원본에서 얻는다). 실패하면 결과는 그대로다. `MeshTools`는 `Animation`에 public으로 의존한다(본 계층이 공개 입력이다).
2. **설정 struct.** `SkinnedMeshBuildSettings`는 `MeshBuildSettings`의 리플렉션 파생 struct다(struct 상속 — ReflectionExtensions 12절). 여섯 필드의 기본값은 UE와 같다(`thresholdPosition` 0.00002, `thresholdTangentNormal` 0.00002, `thresholdUV` 1/1024, `morphThresholdPosition` 0.015, `boneInfluenceLimit` 0, `useHighPrecisionSkinWeights` 거짓 — EngineTypes.h:3167-3183). 진단용 필드 번호는 `MeshBuildSettingField` 뒤에 이어 붙인다(`SkinnedMeshBuildSettingFieldCount` = 23). 버린 대안: 공통 설정을 멤버로 담기(필드 경로가 한 단 깊어지고 공통 필드 판정 함수를 다시 써야 한다), UE처럼 따로 된 struct(공통 필드를 두 번 정의한다).
3. **스킨 빌드의 `buildScale`은 보존이다.** UE `FSkeletalMeshBuildSettings`에는 BuildScale이 없다. 정점만 키우면 본 기준 pose와 맞지 않아 inverse-bind가 틀어지므로 크기는 임포터가 메시와 본에 함께 적용할 일이다. 스킨 빌드는 크기 1로 빌드하고 `buildScale`이 기본값이 아니면 다른 보존 필드처럼 `UnsupportedSetting`을 낸다(`NegativeBuildScale`은 내지 않는다).
4. **설정 검사.** 네 임계값은 유한한 0 이상, `boneInfluenceLimit`은 0 이상이어야 한다. 어기면 `InvalidSetting`(issue `InvalidSettingValue`, index = 필드 번호)·InvalidArgument다. `boneInfluenceLimit` 0은 기본 상한이다: UE는 0이면 프로젝트 기본값을 쓰고 그것도 0이면 `MAX_TOTAL_INFLUENCES`인데 unlimited influence 모드가 없으면 `EXTRA_BONE_INFLUENCES`(8)로 자른다(GPUSkinVertexFactory.cpp `GetBoneInfluenceLimitForAsset`, MeshUtilities.cpp `GenerateSkeletalRenderMesh`). Rocks는 프로젝트 설정·unlimited 모드가 없으므로 0과 8을 넘는 값은 모두 `MeshCookedInfluencesHigh`(8)다.
5. **원본 사본과 검증.** 5.4절 3의 사본에 정점 `SkinWeights`·`SkinWeights-<profile>`도 담아 값 정리가 가중치를 정규화하게 한다(`NonNormalizedWeights` 경고, 합 0은 `InvalidMeshData`·`ZeroWeightSum` 실패 — 진단의 속성 번호는 원본 번호로 바꾼다). 원본 검증은 `boneCount` = 본 수로 해서 범위 밖 boneIndex를 거부한다. 본 계층은 `ValidateBoneHierarchy`로 보고, 어기면 `InvalidSkeletonData`(issue `InvalidBoneHierarchy`, index = 그 진단의 본 번호 — 세부 규칙은 `ValidateBoneHierarchy`를 다시 불러 얻는다), 본이 없으면 `InvalidSkeletonData`·`EmptyBoneHierarchy`다(영향 없는 정점이 루트 본을 쓰므로 루트가 있어야 한다).
6. **임계값.** 렌더 정점 용접은 위치 `thresholdPosition`, 법선·tangent·쌍법선 `thresholdTangentNormal`, UV `thresholdUV`로 비교한다(UE `AreSkelMeshVerticesEqual`). UE 스킨 빌드는 용접 위치 임계값을 `bRemoveDegenerates`와 무관하게 쓰므로 Rocks도 그렇게 한다. 퇴화 삼각형 판정과 삼각형 기저의 위치 임계값은 5.4절 4와 같은 규칙에 스킨 임계값을 넣는다(`removeDegenerates`면 `thresholdPosition`, 아니면 0). 그에 더해 용접 뒤 세 렌더 정점 번호가 서로 다르지 않은 삼각형도 뺀다(UE `BuildSkeletalMeshChunks` — 그 삼각형이 만든 렌더 정점은 남는다, UE와 같다). 법선·tangent 계산은 5.2절 3–5단계 그대로다. UE의 스킨 경로는 옛 import data 기반의 다른 tangent 구현이라 비트 일치는 원래 보장하지 않는다(5.2절).
7. **영향 줄이기(원본 정점마다 한 번).** 가중치 0인 영향을 빼고(UE `FBoneWeights`의 기본 문턱), 가중치 내림차순·같으면 본 번호 오름차순으로 정렬해 상한까지 남기고, 버린 것이 있으면 MA1 정규화(`NormalizeBoneWeights` — 내림 비례 배분 + 잔차를 가장 큰 영향에)로 다시 합을 65535로 맞춘 뒤 다시 정렬한다. 영향이 없는 정점(속성이 없거나 빈 목록)은 루트 본 0에 65535다(UE "Missing influence … weighting to root"). 그런 정점이 있으면 가중치 속성마다 새 경고 `MissingInfluences`(index = 첫 정점, count = 정점 수 — 속성이 없으면 attribute = `MeshNoAttribute`), 버린 정점이 있으면 `InfluencesTrimmed`(정보, 같은 형식)다. 둘 다 삼각형이 쓰지 않는 정점까지 원본 정점 전체를 센다.
8. **용접 비교의 스킨 항목.** 같은 원본 정점의 두 코너는 영향·morph 위치 변화량이 늘 같으므로 `MorphNormal-` 값만 비교한다. 다른 원본 정점이면 줄인 16비트 영향(기본·프로필 모두)과 모든 morph target의 위치 변화량(비트 단위), `MorphNormal-` 값을 비교한다. UE는 영향만 비교해 위치가 겹치는 서로 다른 정점의 morph 변화량이 다르면 하나를 잃는다 — Rocks는 손실을 막는다. 아래 10의 재계산 법선 변화량은 용접 뒤에 정해지므로 비교하지 않는다(렌더 정점을 만든 코너의 값을 쓴다).
9. **가중치 프로필도 빌드한다.** `SkinWeights-<profile>`마다 같은 규칙으로 줄여 렌더 정점별 영향을 만든다. UE는 대체 가중치를 렌더 정점의 원본 점에서 가져오고 용접에는 넣지 않는다(LODUtilities.cpp `GenerateImportedSkinWeightProfileData`). bone map에 프로필 본을 넣는 것은 UE와 같다(`AlternateBoneIDs`).
10. **쿠킹 칸과 가중치 부호화.** 정점당 칸 수는 렌더 정점의 (기본·프로필) 영향 수 최댓값이 4 이하면 `MeshCookedInfluencesLow`(4), 아니면 `MeshCookedInfluencesHigh`(8)다(UE 섹션 `MaxBoneInfluences`의 extra influence 판정과 같다 — 상한은 자르는 값이고 칸 수는 실제 최댓값의 올림). 빈 칸은 (0, 0)이다. `useHighPrecisionSkinWeights`면 16비트(합 65535) 그대로, 아니면 8비트로 `(w × 255 + 32767) / 65535`(반올림) 후 잔차(255 − 합, 음수일 수 있다)를 첫 영향(가장 큰 것, 같으면 본 번호가 작은 것)에 더하고 다시 정렬한다. UE는 8비트를 `w >> 8`로 잘라 합이 255보다 작아질 수 있다(SkinWeightVertexBuffer.cpp `SetBoneWeight`) — Rocks는 6.3절대로 합을 맞춘다. 반올림으로 0이 된 영향도 칸과 본을 유지한다.
11. **bone map과 필요 본.** bone map은 렌더 정점이 쓰는 (기본·프로필) 영향 본의 정렬된 중복 없는 목록이다(빠진 삼각형만 쓰던 정점은 넣지 않는다). 필요 본 목록은 UE `CalculateRequiredBones`(제거 본이 없을 때)와 같이 메시 본 전체다 — 본 제거(`bonesToRemove`)는 보존 설정이라 MA5b 이후에 뺀다. 버린 대안: bone map + 조상(스킨에 안 쓰는 소켓·IK 본이 평가되지 않아 UE와 달라진다).
12. **inverse-bind.** 본 번호 순으로 component[b] = local[b].ToMatrix() × component[부모](`Mat4d`, UE `CalculateInvRefMatrices`와 같은 곱 순서)를 만들고 `Mat4d::TryInverse`로 뒤집는다. UE는 float(`FMatrix44f`)로 저장하는데 그 변환은 쿠킹 패킹(MA5b) 몫이다. 행렬식이 0이라 역이 없으면(크기 0 축) `InvalidSkeletonData`·`SingularBindPose`(index = 본) 실패다 — UE는 경고만 남기고 쓸 수 없는 행렬을 둔다.
13. **morph target.** `Morph-<이름>` 속성마다 하나(원본 속성 순서)이고 같은 이름(Name 비교 규칙)의 `MorphNormal-<이름>`을 짝으로 쓴다. 짝이 없는 `MorphNormal-`도 그 뒤에 위치 변화량 0인 target으로 둔다. 렌더 정점 번호 오름차순으로 Δp = 그 정점을 만든 코너의 원본 정점 변화량, Δn = 같은 코너의 `MorphNormal-` 값이다. `MorphNormal-`이 없으면 UE처럼 법선을 다시 계산한다: Δp가 0이 아닌 정점을 옮긴 위치로, 그 정점을 쓰는 삼각형의 세 정점에 붙은 코너 법선만 0으로 두고 5.2절 3·5단계(법선만, 같은 가중 설정)를 다시 돌려 Δn = (새 법선의 렌더 정규화) − (렌더 법선)이다. UE는 한 고리 더 넓게 0으로 두고(바뀌지 않은 면만 쓰는 코너까지) 원본 점 하나의 법선을 모든 렌더 정점에 쓰는데, Rocks는 실제로 바뀌는 코너만 다시 계산하고 렌더 정점마다 자기 코너 값을 쓴다. 담는 조건은 UE `ComputeMorphDeltas`와 같다: |Δp|² > `morphThresholdPosition`² 이거나, 법선을 추적하는 경우(`MorphNormal-`이 있거나 `recomputeNormals`) |Δn|² > `MorphNormalDeltaThresholdSquared`(0.01). 변화량이 하나도 없는 target도 빈 목록으로 남는다. morph 속성 값은 3.3절 정리 대상이 아니므로 비유한 항목은 0으로 보고 속성마다 `NonFiniteAttribute`(issue `NonFiniteMorph`, count = 항목 수) 경고를 낸다.
14. **경고 순서.** 값 정리(NonFiniteAttribute·UvOutOfRange·NonNormalizedWeights) → `UnsupportedSetting`(필드 순) → 가중치 속성 순(기본 먼저, 프로필은 원본 순서)으로 `MissingInfluences`·`InfluencesTrimmed` → morph target 순으로 `NonFiniteAttribute`(morph) → `MaterialSlotFallback`(묶음 순)이다.
15. **정적 빌드와 공유.** 5.2절 단계 함수(원본 사본·값 정리 경고·설정 경고·렌더 정점·섹션·bounds)를 MeshTools 내부 공용 파일로 옮기고 렌더 정점 구성에 용접 임계값·삼각형 번호 붕괴 제외·추가 비교 함수·렌더 정점을 만든 코너 목록을 매개변수로 더했다. 정적 빌드 결과 byte는 그대로다(기존 시험).
16. **진단 등록.** `AssetDiagnosticCode`에 `InfluencesTrimmed`(11절에 있던 이름)와 `MissingInfluences`(새 경고 — 11절 표에 더했다)를, `MeshIssue`에 `InfluencesTrimmed`·`MissingInfluences`·`NonFiniteMorph`·`InvalidSettingValue`·`InvalidBoneHierarchy`·`EmptyBoneHierarchy`·`SingularBindPose`를 더했다. MeshSource에 접두 판정 `MatchesMeshAttributePrefix`(3.7절 5와 같은 ASCII 대소문자 무시, 접두 뒤가 비면 거짓)를 공개해 빌드가 이름 판정을 다시 구현하지 않게 했다.
17. **결정성.** 빌드 전체가 `RunWithDeterministicFloatingPoint` 안이고 스레드를 쓰지 않는다. 정수 규칙(정렬·정규화·부호화)은 입력만으로 정해지고, morph 법선 재계산은 기본 빌드와 같은 결정적 수학을 쓴다.

## 7. 애니메이션 클립

### 7.1 클래스

```text
AnimationAsset : Asset (추상)
  skeleton : ObjectRef<Skeleton>     // Hard + Build(쿠킹 트랙 → 본 번호가 스켈레톤에 의존 — 너겟 키에 스켈레톤 build key가 들어간다)

AnimationClip : AnimationAsset
  EditorOnly:
    source           : ClipSourceData
    eventTracks      : Array<Name>                                   // 이벤트 트랙 이름(UE AnimNotifyTracks)
  저장:
    events           : Array<ClipEvent { time float, duration float, track int32, name Name, type Name,
                                         triggerChance float, triggerOnDedicatedServer bool, triggerOnFollower bool }>
    syncMarkers      : Array<ClipSyncMarker { name Name, time float, track int32 }>
    interpolation    : ClipInterpolation { Linear, Step }
    rateScale        : float
    loop             : bool
    rootMotion       : RootMotionSettings { enable bool, rootLock { RefPose, FirstFrame, Zero }, forceRootLock bool, useNormalizedScale bool }
    additive         : AdditiveSettings { type { None, LocalSpace, MeshSpaceRotation },
                                          basePose { None, RefPose, Scaled, Frame, LocalFrame },
                                          basePoseClip ObjectRef<AnimationClip>(Hard, 비어도 됨), basePoseFrame int32 }
    targetFrameRate  : FrameRate(플랫폼별)                            // 쿠킹 샘플링 비율
    retargetSource   : Name
  쿠킹 데이터:
    playback         : ClipPlaybackData
```

UE 대응: AnimSequence.h:285-336(additive·root motion·interpolation), AnimSequenceBase.h:61-68(RateScale·bLoop), AnimSequence.h:731-735(PlatformTargetFrameRate). notify·sync marker 필드는 AnimTypes.h:276-356, 479-506이다.

`basePoseClip`이 Hard인 이유: 쿠킹 v1은 additive 차이를 미리 계산하지 않고 런타임에 기준 클립을 함께 평가한다. 쿠킹에서 미리 계산하게 되면 Build + EditorOnly로 바꾼다.

### 7.2 원본 데이터 `ClipSourceData`

```text
ClipSourceData {
  frameRate   : FrameRate                // 유리수(Core FrameRate). UE FFrameRate Numerator/Denominator
  frameCount  : uint32                   // 키 수 = frameCount + 1(UE NumberOfKeys = NumberOfFrames + 1)
  boneTracks  : Array<BoneTrack { bone Name,
                                  translation BulkDataRef(Vec3 float, 1 또는 키 수),
                                  rotation    BulkDataRef(Quat float, 1 또는 키 수),
                                  scale       BulkDataRef(Vec3 float, 1 또는 키 수) }>   // bone 이름 순 정렬
  curves      : Array<ClipCurve { name Name, flags ClipCurveFlags, curve TangentCurve }>
  transformCurves : Array<ClipTransformCurve { name Name, flags, translation TangentCurve[3], rotation TangentCurve[3], scale TangentCurve[3] }>
}
```

- **키는 프레임 균일 간격이다.** UE 5.8 데이터 모델도 tick resolution과 표시 비율을 같게 두고 프레임 0..키 수−1에서 본 키를 평가한다(Engine/Plugins/Animation/AnimationData/Source/AnimationData/Private/AnimSequencerController.cpp:320-321, AnimSequencerDataModel.cpp:1280-1293).
- **상한.** `frameCount` ≤ `ClipMaxFrames`(= 2^20), 원본·목표 frame rate는 0보다 크고 `ClipMaxFrameRate`(= 1000 fps) 이하, 본 트랙 수 ≤ `MeshMaxBones`. 넘으면 `InvalidClipData` 오류다. 쿠킹 할당(샘플 수 × 트랙 수)의 넘침을 막는다.
- **회전 원본에 관한 한계.** UE 5.8은 본 키를 FK 채널(이동·회전·크기 각 3개 float 채널)에 저장하고 회전은 Euler 각이다. 읽을 때 quaternion으로 바꾼다(AnimSequencerDataModel.cpp:1260-1293). 그래서 Rocks가 받는 원본은 "프레임별로 평가한 Transform"이다. float로 저장하는 이유는 UE의 원본 키 형식(`FRawAnimSequenceTrack`, AnimTypes.h:852-866)과 런타임 평가가 float이기 때문이다. quaternion의 연속 키 사이 부호 반전은 원본 그대로 둔다. 보간 때 최단 경로를 고르는 것은 평가 쪽 규칙이다.
- **상수 트랙.** 성분 하나의 모든 키가 비트 단위로 같으면 키 1개로 줄인다(손실 없음).
- 트랙은 본 이름으로 스켈레톤과 맞춘다. 가상 본 이름도 받는다(UE는 전체 계층 기준으로 검증한다 — AnimSequencerController.cpp:1496-1502). 스켈레톤에 없는 본의 트랙은 임포트 때 `UnknownBoneTrack` 경고를 내고 보존한다. 빌드는 무시한다.
- 커브는 Curves 모듈 `TangentCurve`(UE `FRichCurve` 대응, [커브 데이터](CurveData.ko.md))를 그대로 쓴다. 키 시간은 float 초다(UE도 시퀀서 채널에서 `FRichCurve`로 줄 때 float 초로 바꾼다 — AnimSequencerHelpers.cpp:13-28). `ClipCurveFlags`는 UE `AACF_*`(AnimCurveTypes.h:41-57) 가운데 `Editable`·`Metadata`·`Disabled`를 둔다.
- animated bone attribute(UE `FAnimationAttributeIdentifier`)는 후속이다. 임포트 때 개수만 경고한다.

### 7.3 이벤트

- 이벤트 `type`은 이벤트 종류의 이름이다(UE에서는 `UAnimNotify`·`UAnimNotifyState` 클래스 경로). 게임플레이 단계에서 이 이름을 처리기에 연결한다. 등록되지 않은 종류는 보존하고 무시한다.
- 종류별 속성(UE notify 객체의 UPROPERTY)은 v1에서 받지 않는다. 임포터가 버리고 `EventPropertiesDropped` 경고(개수)를 낸다. 게임플레이 단계에서 `InstancedStruct` 필드로 더한다.
- `duration > 0`이면 구간 이벤트(UE NotifyState)다.
- 시간은 클립 시작 기준 초다. UE의 link 방식(`LinkMethod`·`SegmentBeginTime` — AnimLinkableElement.h:142-165)은 montage용이라 클립에서는 계산한 절대 시간만 저장한다.

### 7.4 쿠킹 데이터 v1

```text
ClipPlaybackData {
  sampleRate    : FrameRate        // targetFrameRate 플랫폼 값. 원본 비율과 다르면 원본을 보간해 다시 샘플링한다
  sampleCount   : uint32
  trackToBone   : Array<uint16>    // 쿠킹 트랙 → 스켈레톤 본 번호(스켈레톤에 없는 트랙은 빠진다)
  tracks        : BulkDataRef      // 트랙별 성분 3개, 각 성분은 상수(1키) 또는 sampleCount 키, float
  curves        : 원본 그대로(TangentCurve)
}
```

- 재샘플링의 회전 보간(slerp)은 Core 결정적 수학으로 한다(AssetSystem 10절 쿠킹 결정성).
- 압축(ACL)은 후속이고, 압축이 생기면 이 구조의 `tracks`가 코덱 표시와 압축 바이트로 바뀐다. 오차 목표는 계획 15절(끝 효과기 1 cm, 회전 0.5°)이다.

### 7.5 구현에서 정한 점(MA6a)

MA6a 구현(`Animation` 모듈의 클립 클래스·원본·쿠킹 데이터, CoreTypeBindings `FrameRate` 미러) 중 7.1–7.4절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다. UE 근거는 E/Private/Animation/AnimDataModel.cpp(`Evaluate`), AnimationRuntime.cpp(`GetKeyIndicesFromTime`), AnimSequence.cpp(`CalculateNumberOfSampledKeys`·`SortSyncMarkers`), RT/Core/Public/Math/TransformNonVectorized.h(`Blend`)·Quat.h(`FastLerp`), E/Public/Animation/AnimTypes.h(`FAnimNotifyEvent::operator<`)다.

1. **파일과 의존.** 리플렉션 헤더 `Animation/AnimationClip.h`(열거·값 struct·`ClipSourceData`·`ClipPlaybackData`·`AnimationAsset`·`AnimationClip`)와 메모리 형태·순수 함수 헤더 `Animation/ClipSource.h`(`ClipSource`·`ClipPlayback`, 검증, Load·Store, 샘플링, 쿠킹 변환)를 둔다. `Animation`은 `Curves`(`TangentCurve`)에 public으로 의존한다(8절). 진단은 `AnimationDiagnostic.h`의 `ClipIssue`·`ClipDiagnostic{code, error, issue, index, related}`이다.
2. **`AnimationAsset`·Build 의존.** `AnimationAsset`은 `ROCKS_CLASS(Abstract)`이고 `skeleton`은 `ObjectRef<Skeleton>`(Hard)이다. 7.1절의 Build 의존은 필드 플래그가 아니라 `DependencyDeclarer`(AssetSystem 16.2절 — A2 이후)가 선언하므로 쿠킹 처리기와 함께 MA6b에서 더한다.
3. **`FrameRate` 미러(MR8).** CoreTypeBindings에 `ROCKS_STRUCT(Binds = "rocks::FrameRate", Atomic, Version = 1)`을 두고 private 멤버(`numerator_`·`denominator_`, 도구 이름 `numerator`·`denominator`)를 InterpCurve 미러와 같은 방식(`GeneratedType` friend)으로 리플렉션한다. 검증 hook이 두 값이 양수인지 본다(0·음수 값은 게시하지 않는다 — `TryCreate`와 같은 불변식).
4. **플랫폼별 목표 비율.** `targetFrameRate`는 `PlatformFrameRate{defaultRate, overrides OrderedMap<Name, FrameRate>}`(UE `FPerPlatformFrameRate`)이고 `Resolve(platform)`이 덮어쓰기 표에 없으면 기본값을 준다. 기본값은 UE `UAnimationSettings::DefaultFrameRate`와 같은 30/1이다. 공용 플랫폼별 값 타입이 생기면 그것으로 옮긴다.
5. **커브 표시.** `ClipCurveFlags`는 bool 필드 셋(`editable` 기본 참 — UE `AACF_DefaultCurve`, `metadata`, `disabled`)으로 둔다(UE bit 값을 저장 형식으로 끌고 오지 않는다). `ClipTransformCurve`의 TangentCurve 9개는 `Array<TangentCurve> channels`(이동 x·y·z, 회전 x·y·z, 크기 x·y·z 순, 길이 정확히 `ClipTransformCurveChannels` = 9)다 — 할당기를 받는 struct의 정적 배열 필드는 리플렉션이 받지 않는다.
6. **원본 bulk 배치.** 성분마다 bulk 하나이고 little-endian float, 패딩 없음이다(이동·크기 Vec3 12 byte, 회전 Quat x·y·z·w 16 byte). 원소 수는 1 또는 키 수(`frameCount + 1`)이고 0개·나머지 byte·다른 개수는 손상(로드 `CorruptPackage`)이다. 메모리 형태 `ClipSource`는 트랙마다 성분 배열을 갖고, `Store`가 상수 성분(모든 키 bit 같음)을 1키로 접고 트랙을 본 이름 원문 순으로 정렬해 쓴다. 순서는 원문을 ASCII 소문자로 접은 byte 비교다(9.2절의 NFC UTF-8 byte 순에 Name 비교 규칙을 겹쳐, 대소문자만 다른 두 이름이 같은 자리가 되어 중복이 한 규칙으로 드러난다). `Load`는 엄격 증가가 아니면(정렬 위반·중복) 손상으로 본다. 쿠킹 데이터와 같이 원본 커브는 `ClipSourceData`에 리플렉션 값으로 그대로 있고 `Store`·`Load`는 본 트랙만 다룬다.
7. **원본 검증(`ValidateClipSource`).** 차례로 비율 ≤ `ClipMaxFrameRate`(1000 fps — `FrameRate`는 이미 양수), `frameCount` ≤ `ClipMaxFrames`, 트랙 수 ≤ `MeshMaxBones`, 트랙마다(이름 None 아님, 성분 원소 수 1 또는 키 수, 값 유한, 회전 |길이² − 1| ≤ `BoneRotationNormalizedTolerance`), 이름 중복(Name 비교 규칙 — 두 번째 트랙 번호가 가장 작은 쌍)이다. 순서는 `Store`가 정하므로 보지 않는다(pool이 필요 없다). 위반은 `InvalidClipData`, 할당 실패는 그 오류다. 커브 키(시간 순·유한)는 `TangentCurve` API와 리플렉션 검증 hook이 이미 지키므로 다시 보지 않는다.
8. **샘플링 규칙(UE `UAnimDataModel::Evaluate`).** 시간 t(초, double)가 0 이하이거나 키가 하나면 키 0, `FrameRate::AsFrameTime(t)`가 마지막 키 이상이면 마지막 키, 아니면 키 = 프레임, α = subframe이다. Step이면 α = 0이고, α < `ClipKeySnapThreshold`(1e-4, UE `KINDA_SMALL_NUMBER`)면 앞 키, α > 1 − 그 값이면 다음 키, 그 밖은 UE `FTransform::Blend`처럼 이동·크기는 선형, 회전은 `FastLerp`(내적 부호로 최단 경로를 고른 nlerp) 뒤 정규화(제곱합 < 1e-8이면 단위)다. 성분이 1키면 그 값이다. **7.4절의 slerp 대신 UE와 같은 nlerp를 쓴다** — UE 평가·재샘플링이 nlerp이고 초월 함수가 없어 결정성도 쉽다.
9. **쿠킹 변환(`BuildClipPlayback`).** 목표 비율이 원본과 같으면(`FrameRate` ==) 키를 그대로 옮긴다. 다르면 표본 수 N = frameCount × 목표/원본이 정수여야 한다(정수 연산으로 확인 — 아니면 `InvalidClipData`·`IncompatibleTargetFrameRate`. UE는 `ensure`만 하고 내림해 끝 프레임을 잃는다), N ≤ `ClipMaxFrames`이다. 키 k는 목표 비율의 k 프레임 시간(초)에서 8의 규칙(클립 보간 방식)으로 원본을 평가한 값이다(UE도 시간으로 평가한다). 그다음 성분마다 상수 접기를 한다. 트랙은 원본 순서대로 스켈레톤 원본 본 이름(`FindBoneIndex`)과 맞추고 없는 트랙은 뺀다. 원본 본도 가상 본도 아닌 이름은 `UnknownBoneTrack` 경고(index = 원본 트랙), 가상 본 트랙은 경고 없이 뺀다(가상 본은 평가가 계산한다). 커브는 그대로 복사하고, 변환 커브는 쿠킹 v1에 들어가지 않는다(UE는 본 트랙에 bake한다 — bake는 후속) — 사용 중(disabled 아님)인 수를 결과 `skippedTransformCurves`로 돌려준다. 전체를 `RunWithDeterministicFloatingPoint` 안에서 한다.
10. **쿠킹 bulk 배치.** `tracks` bulk 하나에 트랙마다 원소 수 `uint32` 셋(이동·회전·크기, 각 1 또는 `sampleCount`)과 그 값들을 잇는다(6과 같은 원소 배치). `trackToBone` 길이 = 트랙 수, `sampleCount` = 표본 수(키 수)다. 로드는 배치·길이·값 유한을 보고 어기면 `CorruptPackage`다. 쿠킹 데이터 샘플링은 8과 같은 규칙에 `sampleRate`·`sampleCount`를 쓴다. 로드 머리 판정의 진단은 규칙마다 다르다: `sampleCount` 0은 `NoSamples`, 프레임 상한 초과는 `TooManyFrames`, 비율 상한 초과는 `FrameRateTooHigh`, `trackToBone` 길이 불일치는 `TrackBoneLength`다(리뷰 반영 개정 13 — 처음엔 모두 `TrackBoneLength`였다). `trackToBone`의 본 번호와 회전 정규화는 스켈레톤을 알아야 하므로 이 로드는 보지 않는다 — 쿠킹 로드(MA6b)가 스켈레톤과 대조하기 전에는 믿을 수 없다(헤더 계약에 적었다).
11. **클립 검증(`ValidateAnimationClip`).** 에디터 데이터가 있는 클립을 본다(원본 머리·커브 이름(None·중복)·변환 커브 채널 수 + 설정 + 이벤트). 길이 = frameCount / 원본 비율(UE `GetPlayLength`). 이벤트: 시간 유한·[0, 길이], duration 유한·0 이상·끝 ≤ 길이, `triggerChance` ∈ [0, 1], track ≥ 0이고 `eventTracks`가 있으면 그 범위, 정렬(시간 오름차순, 같으면 track 오름차순 — UE `operator<`의 근사 비교 대신 정확 비교). 동기 마커: 이름 None 아님, 시간 [0, 길이], track 규칙 같음, 시간 오름차순(UE `SortSyncMarkers`. UE는 범위 밖을 잘라 넣지만 Rocks는 거부한다). `rateScale` 유한, `basePoseFrame` ≥ 0, 목표 비율(기본·덮어쓰기)이 `ClipMaxFrameRate` 이하. 정렬 도우미 `SortClipEvents`·`SortClipSyncMarkers`(안정 정렬)를 둔다. 쿠킹 클립의 검증은 쿠킹 로드(MA6b)가 한다.
12. **진단 등록.** `AssetDiagnosticCode`에 `InvalidClipData`·`UnknownBoneTrack`·`EventPropertiesDropped`(임포터가 내는 경고 — MA6b·UX 임포터가 쓴다)를 더했다.
13. **결정성.** 정렬은 원문 byte·정확 비교로 입력만으로 정해지고, 부동소수점은 √(정확 반올림)와 사칙연산뿐이다. 스레드를 쓰지 않는다.

## 8. 모듈 배치

| 모듈 | 계층 | 책임 | 의존 |
|---|---|---|---|
| `MeshSource` | Engine Runtime | `MeshSourceData`·`MeshSource`, 속성 이름 상수, `ValidateMeshSource`, 위상 조회(다각형의 삼각형·코너 등) | AssetRuntime(`BulkDataRef`·`Package`)·Core·MetaSchema·Serialization |
| `Animation` | Engine Runtime | `BoneHierarchy`·`Skeleton`·`AnimationAsset`·`AnimationClip`·쿠킹 데이터, 클립 샘플링(원본·쿠킹) | AssetRuntime·Curves·Core |
| `Mesh` | Engine Runtime | `StaticMesh`·`SkinnedAsset`·`SkeletalMesh`, 슬롯·섹션·소켓·단순 충돌·빌드 설정 값 타입, 쿠킹 렌더 데이터 구조 | AssetRuntime·MeshSource·Animation·Material·GraphicsTypes |
| `MeshTools` | Engine Developer | 빌드(5.2·6.3절), 법선·tangent·MikkTSpace, 용접, 쿠킹 처리기, 메시 임포트 서비스 | Mesh·MeshSource·AssetCooker·AssetDatabase·AssetNugget·ThirdParty MikkTSpace(패치) |
| `AnimationTools` | Engine Developer | 클립 빌드·재샘플링·쿠킹 처리기, 스켈레톤 병합, 임포트 서비스 | Animation·AssetCooker·AssetDatabase·AssetNugget |

- 텍스처 모듈 구성(`Texture` Runtime + `TextureTools` Developer, AssetSystem 3절)과 같은 모양이다. 렌더 층은 이 에셋 모듈을 모른다. 쿠킹 데이터 → 렌더 자원 연결은 렌더 쪽 연결 모듈이 한다(머티리얼의 `Material` → `MaterialRuntime` 분리와 같다).
- `Mesh`가 `Animation`에 의존하는 이유는 `SkeletalMesh`가 `Skeleton`·`BoneHierarchy`를 쓰기 때문이다. 반대 방향은 없다. 스켈레톤의 미리보기 메시는 에디터 UI 범위라 넣지 않는다.
- `MeshSource`가 AssetRuntime에 의존하는 것은 Runtime 모듈 사이 의존이 하나 느는 것뿐이고 방향 문제는 없다(결정 MA-D3). AssetSystem 3절 모듈 그래프에 이 다섯 모듈을 더한다(요구 MR10).

## 9. 저장 세부

### 9.1 원본 메시 위상 blob 배치

| 순서 | 배열 | 원소 | 길이 |
|---|---|---|---|
| 1 | 코너 → 정점 | uint32 | cornerCount |
| 2 | 변 정점 쌍 | uint32 × 2 | edgeCount |
| 3 | 다각형 둘레 시작 | uint32 | polygonCount + 1(첫 값 0, 단조 증가, 마지막은 전체 길이) |
| 4 | 다각형 둘레 코너 | uint32 | 3번 마지막 값 |
| 5 | 다각형 → 묶음 | uint32 | polygonCount |
| 6 | 삼각형 코너 | uint32 × 3 | triangleCount |
| 7 | 삼각형 → 다각형 | uint32 | triangleCount |

모두 little-endian이고 정렬 패딩이 없다. 정점·코너 번호의 순서는 임포트 결과 그대로이며 저장이 다시 매기지 않는다(재임포트 diff가 안정되도록).

### 9.2 결정적 정렬

속성 목록은 (element, name의 NFC UTF-8 byte 순)으로, 본 트랙은 본 이름 순으로, 커브는 이름 순으로 저장한다. 슬롯·섹션·소켓·본 계층은 의미 있는 순서라 사용자가 정한 순서를 유지한다.

### 9.3 너겟 키

AssetSystem 9절 공통 항목(원본 bytes 해시·처리기 버전·정규 설정·의존 키·프로필·스키마 버전)에 더해 다음을 넣는다.

| 처리기 | 추가 키 입력 |
|---|---|
| 정적 메시 빌드 | LOD별 원본 해시(위상·속성 bulk SHA-256), 빌드 설정 정규형, 쿠킹 플랫폼의 screenSize·minLod, 색 형식 프로필, MikkTSpace 패치 버전, meshoptimizer 어댑터 버전, 임계값 상수 집합 버전 |
| 스킨 메시 빌드 | 위 + 스켈레톤 build key(메시 본 ↔ 스켈레톤 대응), 영향 상한·정밀도 |
| 클립 쿠킹 | 원본 해시, 쿠킹 플랫폼 targetFrameRate, 스켈레톤 build key(본 번호), 보간 규칙 버전 |

## 10. 임포트 서비스

임포터(교환 형식·glTF·FBX)는 파일 형식을 읽어 아래 명세를 만들고, 공용 서비스가 에셋을 만든다. 텍스처의 `TextureImportSpec`(AssetSystem 12.3절)과 같은 방식이다.

| 서비스 | 입력 | 결과 |
|---|---|---|
| `MeshTools::ImportStaticMesh(StaticMeshImportSpec)` | LOD별 `MeshSource`(원본·결과 메시), 설정, 슬롯 목록(이름 + 머티리얼 참조), 소켓, 단순 충돌 | `StaticMesh` 패키지 |
| `AnimationTools::ImportSkeleton(SkeletonImportSpec)` | `BoneHierarchy`, 소켓, 가상 본, 커브 정보, 리타깃 방식 | `Skeleton` 패키지(기존 스켈레톤이면 병합) |
| `MeshTools::ImportSkeletalMesh(SkeletalMeshImportSpec)` | 위 정적 메시 입력 + 메시 본 계층 + 스켈레톤 참조 | `SkeletalMesh` 패키지, 필요하면 스켈레톤 병합 |
| `AnimationTools::ImportClip(ClipImportSpec)` | `ClipSourceData`, 이벤트·마커·설정, 스켈레톤 참조 | `AnimationClip` 패키지 |

서비스는 3.1·3.3·3.5·6.1·7.2절 검증을 하고 진단(AssetSystem 4.6절 형식)을 돌려준다. 저장은 원자적이다. 실패하면 기존 에셋을 보존한다(AssetSystem 17절).

## 11. 진단 코드

AssetSystem 4.6절 진단 표에 등록한다(요구 MR10).

| 코드 | 수준 | 상황 |
|---|---|---|
| `InvalidMeshData` | 오류 | 위상·속성 불변식 위반, 위치 비유한값 |
| `NonFiniteAttribute` | 경고 | UV·색 비유한값을 고침 |
| `UvOutOfRange` | 경고 | UV 절댓값이 `MeshUvRangeWarning` 초과 |
| `NonNormalizedWeights` | 경고 | 스킨 가중치 합이 `BoneWeightTotal`이 아님 |
| `TooManyInfluences` | 오류 | 정점 영향이 `MeshMaxSourceInfluences` 초과(원본 상한) |
| `InfluencesTrimmed` | 정보 | 쿠킹 상한으로 영향을 버림 |
| `MissingInfluences` | 경고 | 스킨 메시 정점에 영향이 없어 루트 본에 붙임(6.5절 7 — MA5a에서 더함) |
| `MaterialSlotFallback` | 경고 | 다각형 묶음 이름과 맞는 슬롯 없음 |
| `NegativeBuildScale` | 경고 | buildScale 음수 성분 홀수 개(winding 반전 없음) |
| `UnsupportedSetting` | 경고 | 보존 상태 설정이 기본값이 아님 |
| `UnsupportedCollisionShape` | 경고 | level set 등 받지 않는 충돌 형상 |
| `InvalidCollisionData` | 오류 | 단순 충돌 값·볼록 형상 bulk 규칙 위반(4.8절 — MA3a에서 더함) |
| `InvalidSkeletonData` | 오류 | 본 계층·스켈레톤 불변식 위반(6.4절 — MA4a에서 더함) |
| `SkeletonMismatch` | 오류 | 본 병합 때 부모 사슬 불일치 |
| `UnknownBoneTrack` | 경고 | 스켈레톤에 없는 본의 트랙 |
| `InvalidClipData` | 오류 | 클립 상한·frame rate 위반 |
| `EventPropertiesDropped` | 경고 | 이벤트 종류별 속성을 버림 |

## 12. 시험

에셋 시스템 시험 원칙(변조·실패 주입·취소·동시성·퍼징까지 넓게 시험한다)을 따른다.

- **원본 메시**: 위상·속성 불변식별 위반 fixture(번호 범위, 중복 변, 둘레 3 미만, 삼각형 수 불일치, 비유한 위치, offset 비단조, 희소 번호 중복·역순·범위 밖, Name 길이 넘침, 대소문자만 다른 속성 이름, boneIndex 범위 밖). 코너 공유·퇴화 삼각형이 있는 정상 메시. 저장·로드 왕복의 비트 일치. bulk 길이 변조·잘림·SHA 불일치(`CorruptPackage`). 무작위 위상·속성 퍼징(검증이 오류를 내거나 통과하고, 통과한 것은 빌드가 범위 검사 단언에 걸리지 않고 끝남). morph 희소 저장의 `-0.0` 보존.
- **빌드**: 결정성(같은 입력 두 번 — 스레드는 쓰지 않으므로 "스레드 수 바꿔서"는 5.4절 13대로 대상이 없다). 용접(속성 하나만 다른 코너는 합치지 않음). 퇴화 삼각형 제거 on/off와 UE 판정식(일직선 위 세 점은 남김). 슬롯 대응·대체 경고. `buildScale` 음수 성분(winding 유지, 경고). MikkTSpace 참조 구현과의 대조(공개 시험 메시, 패치 전후 차이 상한).
- **스켈레톤·스킨**: 계층 불변식, 병합 성공·부모 사슬 실패, 영향 줄이기 결정성(가중치 동률), 정규화 잔차. "병합 뒤 클립 재쿠킹(너겟 무효화)"은 MA4b·MA6b 시험으로 넘긴다(에셋 DB·쿠커가 필요하다). "palette 분할 경계"는 섹션을 본 수로 나누지 않으므로(RenderBase R18 — 6.3절) 대상이 없다.
- **클립**: 상수 트랙 접기의 비트 보존, 재샘플링(원본 비율 = 목표 비율이면 키 그대로), 상한 초과, 스켈레톤에 없는 트랙, 커브 왕복, 이벤트 정렬.

### 12.1 요구 대조표(MA1 — 원본 메시)

시험은 EngineTests의 `MeshSource_*`(`Engine/Source/Runtime/MeshSource/Tests`, 52개)와 AssetRuntime의 `AssetPackage_MemoryBulkSource*`(3개)다. 패키지 왕복은 fixture 에셋 `TestMeshAsset`(AssetTestFixtures `MeshAssets.h` — `MeshSourceData` 필드·배열·EditorOnly 필드)으로 실제 패키지를 쓰고 다른 Registry에 로드한 뒤 `MeshSource::Load`를 부른다. 불변식 위반 시험은 진단 코드와 세부 규칙(`MeshIssue`)·위치를 함께 대조한다.

| 요구 | 시험 |
|---|---|
| 3.1 번호 범위(코너·변·다각형·삼각형) | `MeshSource_ValidRejectsIndexRanges`, `MeshSource_LoadRejectsTopologyBytes`(로드 중 CorruptPackage) |
| 3.1 코너 공유·퇴화 삼각형·같은 코너 반복 정상 | `MeshSource_ValidAcceptsSharedAndDegenerate`, `MeshSource_TopologyQueries`, `MeshSource_TopologyRepeatedCorners` |
| 3.1 변: 같은 정점·중복(순서만 다른 것 포함)·작은 번호 먼저 | `MeshSource_ValidRejectsBadEdges`, `MeshSource_AddElementsKeepsShape`, `MeshSource_LoadRejectsTopologyBytes`(EdgeOrder) |
| 3.1 둘레 ≥ 3·둘레 변 존재·퇴화 변 건너뜀 | `MeshSource_ValidRejectsBadPolygons`, `MeshSource_ValidAcceptsSharedAndDegenerate` |
| 3.1 다각형별 삼각형 수·둘레 소속 | `MeshSource_ValidRejectsBadTriangles` |
| 3.1 위치 유한값(MA-D9) | `MeshSource_ValidRejectsNonFinitePosition` |
| 3.1 요소 수 ≤ `MeshMaxElements`·묶음 수 ≤ `MeshMaxPolygonGroups`(경계 포함, 여섯 종류 모두 큰 할당 없이 — 3.7절 12) | `MeshSource_LoadRejectsFormatFields`, `MeshSource_LoadBoundsElementAllocations` |
| 3.2 엔진 이름 속성 형태·대소문자·접두 | `MeshSource_ValidRejectsEngineAttributeShapes`, `MeshSource_LoadRejectsCaseOnlyDuplicateNames` |
| 3.2 기본값(Color 흰색)·속성 추가 규칙 | `MeshSource_AttributeDefaults`, `MeshSource_AddAttributeRejectsBadArguments`, `MeshSource_AddAttributeNamesAreUniquePerElement` |
| 3.2 스킨 가중치 상한·같은 본·boneIndex 범위 | `MeshSource_ValidRejectsAttributeValues`, `MeshSource_LoadRejectsInfluenceBytes` |
| 3.2 정규화 잔차·동률 | `MeshSource_NormalizeWeightsResidual`, `MeshSource_SanitizeNormalizesWeights` |
| 3.2 morph 희소 저장, `-0.0` 보존 | `MeshSource_SparseKeepsNegativeZero`, `MeshSource_StoreLoadRoundTrip`, `MeshSource_LoadRejectsSparseBytes`(+0.0 항목) |
| 3.3 값 정리와 경고 수, 실패 시 무변경 | `MeshSource_SanitizeFixesValues`, `MeshSource_SanitizeFailureKeepsMesh`, `MeshSource_SanitizeWarningAllocation` |
| 3.4 Load·Store 왕복(쿠킹·에디터·Zstd)·비트 일치 | `MeshSource_StoreLoadRoundTrip`, `MeshSource_StoreIsDeterministicAndResaveIdentical`, `MeshSource_SeveralSourcesShareBulk`, `MeshSource_EmptyMeshRoundTrip` |
| 3.4 포맷 필드 하나씩 변조 | `MeshSource_LoadRejectsFormatFields`, `MeshSource_LoadRecordNameTokens` |
| 3.4 bulk 길이 변조·잘림·SHA 불일치 | `MeshSource_LoadRejectsRefMismatch`, `MeshSource_PackageBulkTamperIsCorrupt`, `MeshSource_LoadRejectsFixedValueBytes`, `MeshSource_LoadRejectsNameValueBytes` |
| 3.4.1 값 타입별 배치·길이·단조·범위 | `MeshSource_LoadRejectsFixedValueBytes`, `MeshSource_LoadRejectsNameValueBytes`, `MeshSource_LoadRejectsInfluenceBytes`, `MeshSource_LoadRejectsSparseBytes` |
| 3.5 이름 유일(대소문자만 다른 이름)·Name 값 내용 규칙 | `MeshSource_LoadRejectsCaseOnlyDuplicateNames`, `MeshSource_ValidRejectsAttributeValues` |
| 3.7 2 `MemoryBulkSource` | `AssetPackage_MemoryBulkSourceEntries`, `AssetPackage_MemoryBulkSourceFailures`, `AssetPackage_MemoryBulkSourceWrites` |
| 3.7 9 가변 길이 값 덮어쓰기·별칭 | `MeshSource_NameValuesOverwriteAndAlias`, `MeshSource_InfluencesOverwriteAndAlias` |
| 3.7 11 위상 조회 | `MeshSource_TopologyQueries`, `MeshSource_TopologyRepeatedCorners`, `MeshSource_TopologyEmpty` |
| 9.1 위상 blob 배치 | `MeshSource_LoadRejectsTopologyBytes`, `MeshSource_EmptyMeshRoundTrip` |
| 9.2 결정적 정렬 | `MeshSource_StoreIsDeterministicAndResaveIdentical`, `MeshSource_StoreLoadRoundTrip`, `MeshSource_LoadRejectsFormatFields`(AttributeOrder) |
| 11 진단 코드 등록·이름·4.6절 변환 | `MeshSource_DiagnosticNames`, `AssetPackage_DiagnosticNames`, `MeshSource_ValidReportsAttributeIndex` |
| 12 퍼징(고정 seed) | `MeshSource_FuzzRandomMeshes`(600개 — 통과분은 위상 조회 전체·일부 패키지 왕복), `MeshSource_FuzzStoredBytes`(160개 bulk 변조) |
| 할당 실패 주입(강한 보장) | `MeshSource_EditFailuresKeepState`, `MeshSource_EditFailuresFromEmpty`, `MeshSource_FirstAttributeFailureAndRegistration`, `MeshSource_ValidScratchFailures`, `MeshSource_StoreFailures`, `MeshSource_LoadFailures`, `MeshSource_TopologyBuildFailures`, `MeshSource_SanitizeWarningAllocation` |

Coverage(2026-11-20, clang source-based — Rocks.Build `Coverage` 구성, Swift 6.3.3 툴체인의 llvm-profdata·llvm-cov, 측정 방법은 `Build/MeasureCoverage.ps1`과 같고 출력만 이 작업 전용 디렉터리에 두었다): 시험 `MeshSource`(52개)·`AssetPackage_MemoryBulkSource`(3개)·`AssetPackage_DiagnosticNames`(1개) 모두 통과. 대상은 MeshSource Private·Public 14개 파일과 AssetRuntime `MemoryBulkSource.h/.cpp`다.

| 파일 | 줄 | 분기 |
|---|---|---|
| MeshSource.cpp | 472/488 | 265/307 |
| MeshSourceStorage.cpp | 681/684 | 335/342 |
| MeshSourceValidation.cpp | 263/263 | 150/151 |
| MeshSourceSanitize.cpp | 178/178 | 104/108 |
| MeshSourceTypes.cpp | 231/231 | 173/174 |
| MeshTopologyIndex.cpp | 250/250 | 86/90 |
| MemoryBulkSource.cpp | 26/28 | 11/13 |
| 그 밖(MeshSourceModule.cpp·MeshSourceInternal.h·공개 헤더 5개·MemoryBulkSource.h) | 87/87 | 2/2 |
| 합계 | 2188/2209(99.05%) | 1126/1187(94.86%), 함수 157/157 |

닿지 않은 분기 61개의 사유:
- 47개는 불변식 검사 `ROCKS_ASSERT`의 실패 쪽이다(용량을 미리 잡은 뒤의 실패할 수 없는 추가, 공개 접근 함수의 계약 위반 — 매크로 전개 안이라 `-show-expansions`로만 보인다).
- 10개는 메모리 상한이라 시험으로 만들 수 없다: 요소 종류마다 `MeshMaxElements`(2^31 − 1)개에 닿은 뒤의 `Add*` 거부 7개(`AddPolygon`의 둘레 전체 길이 포함), 가변 길이 값 저장소의 uint32 offset 넘침(Name·영향 쓰기, Name 로드) 3개 — 모두 수 GiB–수십 GiB를 실제로 할당해야 한다. `MemoryBulkSource::Add`의 bulkId 소진(2^32 − 2 entry)도 같은 이유다(1개).
- 1개는 모듈 내부 열거 `EngineName`의 열거 밖 값(switch의 암묵 default)으로, 모듈 안 호출자가 만들지 않는다.
- 2개는 `BoneInfluence`의 defaulted `operator==` 거짓 쪽이다. `MeshSource_NormalizeWeightsResidual`이 본이 다른 경우와 가중치만 다른 경우를 런타임 값으로 모두 부르는데도 기록이 0으로 남는다(헤더 constexpr 기본 비교의 계측 기록 방식으로 보이며 원인은 확인하지 못했다).

입력 변조·실패 주입으로 닿을 수 있는 오류 분기는 모두 시험으로 닿게 했다. 측정 중 닿을 수 없음을 확인한 분기는 구조를 고쳤다: 저장 후 정렬 비교의 빈 이름 경로(레코드 이름은 비지 않는다), 64비트에서 늘 거짓인 크기 검사, 저장 경로의 `Invalid` 값 타입 case, 미리 잡은 변 표 삽입의 실패(단언으로), 엔진 규칙의 최소 채널(늘 1 이상), 속성 이름 내용 검사(만드는 두 길이 모두 지킨다 — 3.7절 4).

개정 13(리뷰 반영) 뒤 다시 잰 값: `MeshSource.cpp` 줄 474/488·분기 266/307(`AddPolygonGroup`의 상한 거부가 `MeshMaxPolygonGroups`로 닿게 되어 위 메모리 상한 10개 가운데 하나가 빠졌다), `MeshSourceStorage.cpp` 줄 688/691·분기 339/346(Name 값 bulk 최소 길이 판정 포함, 남은 미달은 위와 같다). MeshTools `MeshOverlappingCorners.cpp`는 줄 76/76·분기 38/38, Animation `SkeletonMerge.cpp`는 줄 133/133·분기 64/64, `ClipPlayback.cpp`는 줄 320/320·분기 154/155(상수 접기 단언)다.

### 12.2 요구 대조표(MA2a — 정적 메시 빌드 핵심)

시험은 EngineTests의 `StaticMeshBuild_*`(`Engine/Source/Developer/MeshTools/Tests`, 26개)와 Core의 `DeterministicMath_AccuracyAgainstReferenceTables`(Acos 표)·`DeterministicMath_AcosSpecialValuesAndDigest`다. 진단 이름표는 `AssetPackage_DiagnosticNames`·`MeshSource_DiagnosticNames`가 새 코드까지 본다.

| 요구 | 시험 |
|---|---|
| 5.2 1단계 원본 검증(실패는 검증 진단 그대로) | `StaticMeshBuild_RejectsInvalidInputs`, `StaticMeshBuild_FuzzSourceMeshes` |
| 5.2 2단계 값 정리(사본에만, 경고는 원본 속성 번호) | `StaticMeshBuild_SanitizeWarningsUseSourceAttributes`, `StaticMeshBuild_RecomputesOnlyZeroCorners`(NaN 법선) |
| 5.2 3단계 삼각형 기저(UV 경로·UV 없는 Duff·Frisvad 경로) | `StaticMeshBuild_QuadBasics`, `StaticMeshBuild_TangentsWithoutUvs`, `StaticMeshBuild_AveragedTangentsMatchConvention` |
| 5.2 4단계 겹치는 코너(위치 임계값 경계 안·밖, 임계값 0, UE 가중합 정렬 키 — 평면 격자 비교 횟수, 5.4절 16) | `StaticMeshBuild_WeldPositionThreshold`, `StaticMeshBuild_OverlapKeySpreadsFlatMeshes` |
| 5.2 5단계 0 벡터 코너만 재계산·가중 법선·hard edge·대각선 soft·MikkTSpace | `StaticMeshBuild_RecomputesOnlyZeroCorners`, `StaticMeshBuild_WeightedNormals`, `StaticMeshBuild_HardEdgeSplitsNormals`, `StaticMeshBuild_NgonDiagonalsAreSoft`, `StaticMeshBuild_MikkTSpaceMatchesReference` |
| 5.2 6·7단계 보존 설정 경고 | `StaticMeshBuild_PreservedSettingsWarn` |
| 5.2 8단계 buildScale(음수 성분 winding 유지·경고 홀짝, 비균일 역전치, 잘못된 값) | `StaticMeshBuild_NegativeBuildScale`, `StaticMeshBuild_NonUniformScaleNormals`, `StaticMeshBuild_RejectsInvalidInputs` |
| 5.2 9단계 퇴화 on/off·UE 판정식(일직선 세 점 남김)·용접(속성 하나만 다르면 안 합침) | `StaticMeshBuild_DegenerateTriangles`, `StaticMeshBuild_WeldPositionThreshold`, `StaticMeshBuild_WeldRequiresEveryAttribute` |
| 5.2 10단계 섹션(묶음 순서·빈 묶음)·슬롯 대응·대체 경고·섹션 정보 | `StaticMeshBuild_SectionsFollowGroups`, `StaticMeshBuild_SlotMatchingAndSectionInfo` |
| 5.2 11단계 원본 순서 | `StaticMeshBuild_QuadBasics`, `StaticMeshBuild_NgonDiagonalsAreSoft`, `StaticMeshBuild_SectionsFollowGroups` |
| 5.2 12단계 bounds·확장 | `StaticMeshBuild_BoundsAndExtension`, `StaticMeshBuild_QuadBasics` |
| 5.2 결정성(두 번, 호출 전 FTZ, 비정규 중간값) — 스레드 없음(5.4절 13) | `StaticMeshBuild_DeterministicBytes`, `StaticMeshBuild_FuzzSourceMeshes` |
| n-gon·코너 공유·빈 메시·UV 채널·색 | `StaticMeshBuild_NgonDiagonalsAreSoft`, `StaticMeshBuild_EmptyMeshes`, `StaticMeshBuild_UvChannelsAndColors` |
| 11 `MaterialSlotFallback`·`NegativeBuildScale`·`UnsupportedSetting`·`InvalidSetting` | `StaticMeshBuild_SlotMatchingAndSectionInfo`, `StaticMeshBuild_NegativeBuildScale`, `StaticMeshBuild_PreservedSettingsWarn`, `StaticMeshBuild_RejectsInvalidInputs` |
| 4.1 구현 필드 `recomputeNormals`·`recomputeTangents` | `StaticMeshBuild_RecomputesOnlyZeroCorners`, `StaticMeshBuild_DeterministicBytes` |
| 4.1 `useMikkTSpace`·`computeWeightedNormals`·`removeDegenerates` | `StaticMeshBuild_AveragedTangentsMatchConvention`, `StaticMeshBuild_MikkTSpaceMatchesReference`, `StaticMeshBuild_WeightedNormals`, `StaticMeshBuild_DegenerateTriangles` |
| 4.1 `useHighPrecisionTangentBasis`·`useFullPrecisionUVs`(패킹은 MA2b — 여기서는 저장·경고 없음) | `StaticMeshBuild_SettingFieldTable`, `StaticMeshBuild_PreservedSettingsWarn` |
| 4.1 `buildScale` | `StaticMeshBuild_NegativeBuildScale`, `StaticMeshBuild_NonUniformScaleNormals`, `StaticMeshBuild_BoundsAndExtension` |
| 4.1 필드 표·UE 기본값·리플렉션 필드 이름 | `StaticMeshBuild_SettingFieldTable` |
| MikkTSpace 패치 전 원본 대조(정육면체·구·거울 UV 띠·울퉁불퉁한 격자, 상한 성분 1e-6, 부호 일치) | `StaticMeshBuild_MikkTSpaceMatchesReference` |
| 할당 실패 주입(결과 그대로, MikkTSpace 느린 경로는 같은 결과) | `StaticMeshBuild_AllocationFailures` |
| 고정 seed 퍼징(MA1 생성기 400개) | `StaticMeshBuild_FuzzSourceMeshes` |
| 아주 큰 값(좌표·UV·원본 법선 넘침)·아주 작은 삼각형 — 결과 유한, byte 결정적 | `StaticMeshBuild_ExtremeValuesStayFinite` |
| Core `Acos`(기준값 1 ulp·특수값·digest) | `DeterministicMath_AccuracyAgainstReferenceTables`, `DeterministicMath_AcosSpecialValuesAndDigest` |

Coverage(2026-11-21, clang source-based — `Build/MeasureCoverage.ps1`과 같은 방법, Swift 6.3.3 툴체인의 llvm-profdata·llvm-cov): 시험 `StaticMeshBuild`(26개)·`DeterministicMath`(7개) 모두 통과. 메인 트리가 다른 세션의 리플렉션 작업 중이라 빌드되지 않아 검증은 `origin/feature/engine-runtime`(90972993)에 이 작업을 얹은 임시 worktree에서 했다.

| 파일 | 줄 | 분기 |
|---|---|---|
| StaticMeshBuilder.cpp | 517/521 | 261/264 |
| MeshTangentBasis.cpp | 442/442 | 217/220 |
| MeshOverlappingCorners.cpp | 70/70 | 38/38 |
| MeshMikkTSpace.cpp | 79/79 | 10/11 |
| MeshBuildInternal.h | 78/78 | 37/40 |
| MeshBuildSettings.cpp·MeshModule.cpp·StaticMeshBuilder.h | 121/121 | 114/114 |
| 합계(Mesh·MeshTools) | 1307/1311(99.7%) | 677/687(98.5%), 함수 전부 |

닿지 않은 분기 10개의 사유:
- 3개는 메모리 상한이다: 삼각형 수 상한(`MeshMikkTSpaceMaxTriangles`·index 수 uint32 — 수억 개 삼각형) 둘, 원본 사본의 `AddPolygonGroup` 실패(묶음은 속성이 없으면 할당하지 않아 `MeshMaxElements`에서만 실패) 하나.
- 3개는 UE 식을 그대로 옮긴 판정의 닿지 않는 쪽이다(`BasisWithUv`): `CreateOrthonormalBasis`는 tangent가 있으면 쌍법선을 외적으로 채우고 법선은 단위 벡터로 끝나므로 "tangent는 있는데 쌍법선이 0"과 "직교화 뒤 법선이 0"이 생기지 않는다.
- 3개는 성분별 단락의 뒤쪽 성분이다(`IsFiniteVector`·`ContainsNaN`의 y·z 성분만 비유한) — 시험의 넘침 입력은 x 성분부터 넘친다.
- 1개는 `ROCKS_ASSERT`의 실패 쪽(MikkTSpace 실패가 할당 실패뿐이라는 단언)이다.

`DeterministicMath.cpp`의 `Acos`·`AsinTail`·`SqrtApproximation`은 줄·분기 모두 닿았다(그 파일의 나머지 미달은 기존 함수의 특수값 분기로 이 작업 범위 밖이다). 측정 중 닿을 수 없음을 확인한 분기는 구조를 고쳤다: MikkTSpace 해제 훅의 null 검사(원본은 할당에 성공한 포인터만 넘긴다), 할당 오류의 "처음 것만" 조건, 지역 진단 null 검사, `GroupByUv`의 빈 묶음, `HasAllTangents`의 NaN·x·z 판정(입력이 유한하고 x·z가 0이면 y도 0), `SqrtApproximation`의 양수 지수 쪽(z < 1). 할당 실패 주입이 찾은 MikkTSpace 원본 버그는 5.4절 8에 적었다.

### 12.3 요구 대조표(MA4a — 본 계층·스켈레톤·병합 판정)

시험은 EngineTests의 `Skeleton_*`(`Engine/Source/Runtime/Animation/Tests`, 15개)다. 진단 이름표는 `AssetPackage_DiagnosticNames`가 새 코드까지 본다.

| 요구 | 시험 |
|---|---|
| 6.1 계층 불변식(정상: 빈·루트 하나·가지, 이름 찾기 대소문자 무시) | `Skeleton_HierarchyAcceptsValid` |
| 6.1 규칙마다 하나씩 어기기(pose 길이·루트 부모·두 번째 루트·자기·뒤·−2 부모·None·잘못된 토큰·대소문자만 다른 중복·NaN 이동·크기·회전·정규화 안 된 회전, 허용 경계 안쪽) | `Skeleton_HierarchyRejectsEachRule` |
| 6.1 본 수 상한 `MeshMaxBones`(경계 포함) | `Skeleton_HierarchyBoneLimit`, `Skeleton_MergeRejectsInvalidInputs`(병합 결과) |
| 6.1 기준 pose 정규화 | `Skeleton_NormalizeReferencePose` |
| 6.2 스켈레톤 필드 하나씩 변조 거부(리타깃 방식·가상 본·소켓·커브·슬롯 묶음·리타깃 원본, 다른 pool 이름) | `Skeleton_ValidateRejectsEachField` |
| 6.2 저장 → 로드 → 재저장 byte 일치, 두 번 쓴 byte 일치(결정성) | `Skeleton_PackageRoundTripResaveIdentical` |
| 6.2 병합 거부(본 없는 메시 — `EmptyMeshHierarchy`)·병합 성공(빈 스켈레톤, 없는 본 더하기·대응표·pose, 대소문자 이름 바꾸기, 결정성) | `Skeleton_MergeIntoEmptySkeleton`, `Skeleton_MergeAddsMissingBones`, `Skeleton_MergeCaseOnlyRename` |
| 6.2 부모 사슬 실패(`SkeletonMismatch` — 중간 본 빠짐·조상 사슬 다름·루트 다름) | `Skeleton_MergeRejectsParentChain` |
| 병합 입력 검증(role)과 결과 상한 | `Skeleton_MergeRejectsInvalidInputs` |
| 할당 실패 주입(결과 그대로·같은 결과) | `Skeleton_MergeAllocationFailures`, `Skeleton_ValidateAllocationFailures` |
| 고정 seed 퍼징(무작위 계층 500개 — 검증·스켈레톤에서 만든 메시와 병합·무관한 계층 병합) | `Skeleton_FuzzHierarchiesAndMerges` |
| 11 `InvalidSkeletonData`·`SkeletonMismatch` 등록·이름·4.6절 변환 | `Skeleton_DiagnosticNames`, `AssetPackage_DiagnosticNames` |

Coverage(2026-11-21, clang source-based — `Build/MeasureCoverage.ps1`): 시험 `Skeleton_`(15개) 모두 통과. Animation Private·Public 8개 파일 줄 627/627(100%), 분기 333/334(99.70%), 함수 47/47. 닿지 않은 분기 1개는 이름 표 이진 탐색(`FindNamed`)의 "다른 pool 이름" 비교 쪽이다 — 병합·검증의 이름 조회는 같은 pool이라는 전제(6.4절 6) 안에서만 부르므로 domain이 다른 키가 오지 않는다(정렬 쪽 domain 비교는 다른 pool 이름을 섞은 계층 시험이 닿는다). 측정 중 닿을 수 없음을 확인한 구조는 고쳤다: 중복 이름 찾기의 "묶음 첫 원소까지 거슬러 오르기"(묶음 안이 번호 순이라 두 번째 원소의 바로 앞이 첫 원소다), 검증 단계 사이에 이름 표를 다시 써 용량이 남아 할당이 일어나지 않던 경로(단계마다 새 표).

### 12.4 요구 대조표(MA5a — 스킨 빌드 핵심)

시험은 EngineTests의 `SkinnedMeshBuild_*`(`Engine/Source/Developer/MeshTools/Tests`, 16개)다. 정적 빌드 시험(`StaticMeshBuild_*` 26개)은 단계 함수를 옮긴 뒤에도 그대로 통과한다(결과 byte 불변). 진단 이름표는 `MeshSource_DiagnosticNames`·`AssetPackage_DiagnosticNames`가 새 코드까지 본다.

| 요구 | 시험 |
|---|---|
| 6.3 `SkinnedMeshBuildSettings` 여섯 필드·UE 기본값·리플렉션 필드(상속 펼침)·보존 판정(buildScale 포함) | `SkinnedMeshBuild_SettingFieldTable`, `SkinnedMeshBuild_PreservedSettingsWarn`, `StaticMeshBuild_SettingFieldTable` |
| 6.5절 3 스킨 빌드의 buildScale 미적용·`UnsupportedSetting`·`NegativeBuildScale` 없음 | `SkinnedMeshBuild_PreservedSettingsWarn` |
| 6.5절 4·5 설정·본 계층·원본 입력 오류(임계값 NaN·무한·음수, 상한 음수, 본 없음·계층 위반·boneIndex 범위·합 0, 실패 시 결과 그대로) | `SkinnedMeshBuild_RejectsInvalidInputs` |
| 6.5절 5 값 정리 경고의 원본 속성 번호(기본·프로필) | `SkinnedMeshBuild_RejectsInvalidInputs` |
| 6.5절 6 스킨 임계값(위치·UV·법선 경계 안·밖, 위치 0)·번호가 겹친 삼각형 제외(세 쌍)·removeDegenerates와 정적 빌드의 차이 | `SkinnedMeshBuild_ThresholdsAndCollapsedTriangles` |
| 6.5절 7 영향 줄이기(가중치 0 제외, 동률은 본 번호, 잔차는 가장 큰 영향, 상한 1·4·8·100, 12개 원본, 루트 대체) | `SkinnedMeshBuild_ReduceInfluences`, `SkinnedMeshBuild_InfluencesAndBoneMap` |
| 6.5절 7 `MissingInfluences`·`InfluencesTrimmed`(정점 수·첫 정점·속성 번호·순서) | `SkinnedMeshBuild_InfluencesAndBoneMap`, `SkinnedMeshBuild_WeightProfiles` |
| 6.5절 8 용접의 스킨 항목(기본·프로필 영향 값과 개수, morph 위치 변화량, MorphNormal, 비유한 morph를 0으로) | `SkinnedMeshBuild_WeldComparesSkinData` |
| 6.5절 9 가중치 프로필(원본 순서, 프로필 본이 bone map에, 빠진 정점은 루트) | `SkinnedMeshBuild_WeightProfiles` |
| 6.5절 10 칸 수 4/8, 8비트 잔차(양·음)·0이 된 영향 유지, 16비트 그대로, 빈 칸 (0, 0) | `SkinnedMeshBuild_QuantizeWeights8`, `SkinnedMeshBuild_InfluencesAndBoneMap` |
| 6.5절 11 bone map(오름차순·map 위치 번호)·필요 본(메시 본 전체) | `SkinnedMeshBuild_InfluencesAndBoneMap`, `SkinnedMeshBuild_BindPose` |
| 6.5절 12 inverse-bind(독립 계산과 bit 일치, 역 × component ≈ 단위)·`SingularBindPose` | `SkinnedMeshBuild_BindPose`, `SkinnedMeshBuild_RejectsInvalidInputs` |
| 6.5절 13 morph 법선 재계산(옮긴 메시를 처음부터 빌드한 법선과 bit 일치, 가중 법선, 위치 문턱으로 법선만 담기) | `SkinnedMeshBuild_MorphTargets` |
| 6.5절 13 MorphNormal 짝(대소문자 무시)·짝 없는 MorphNormal·문턱 경계(같으면 뺌)·법선 추적 여부·빈 target·비유한 항목 경고 | `SkinnedMeshBuild_MorphAttributesAndThreshold` |
| 6.5절 15 정적 빌드와 렌더 부분 일치(정육면체·구·공유 메시, 설정 8가지) | `SkinnedMeshBuild_RenderPartMatchesStaticBuild` |
| 6.5절 17 결정성(두 번, 호출 전 FTZ, 임시 할당기 따로) | `SkinnedMeshBuild_DeterministicBytes`, `SkinnedMeshBuild_FuzzSourceMeshes` |
| 할당 실패 주입(결과 그대로·같은 결과, 경고를 처음 할당하는 단계 7가지) | `SkinnedMeshBuild_AllocationFailures` |
| 고정 seed 퍼징(MA1 생성기 400개 — 원본 진단 일치, 영향·bone map·morph 순서 불변식, 두 번 빌드 byte 일치) | `SkinnedMeshBuild_FuzzSourceMeshes` |
| 11 `InfluencesTrimmed`·`MissingInfluences` 등록·이름, 접두 판정 `MatchesMeshAttributePrefix` | `MeshSource_DiagnosticNames`, `AssetPackage_DiagnosticNames` |

Coverage(2026-11-21, clang source-based — `Build/MeasureCoverage.ps1`, 필터 `SkinnedMeshBuild`·`StaticMeshBuild`): 시험 42개 모두 통과. 대상은 Mesh·MeshTools Private·Public이다.

| 파일 | 줄 | 분기 |
|---|---|---|
| SkinnedMeshBuilder.cpp | 664/666 | 320/328 |
| MeshLodBuild.cpp | 432/434 | 249/250 |
| StaticMeshBuilder.cpp | 157/159 | 62/64 |
| MeshTangentBasis.cpp | 442/442 | 217/220 |
| MeshBuildInternal.h | 81/81 | 34/36 |
| MeshMikkTSpace.cpp·MeshOverlappingCorners.cpp | 149/149 | 48/49 |
| MeshBuildSettings.cpp·MeshModule.cpp·공개 헤더 둘 | 176/176 | 168/168 |
| 합계(Mesh·MeshTools) | 2101/2107(99.72%) | 1098/1115(98.48%), 함수 125/125 |

닿지 않은 분기 17개의 사유:
- 6개는 `ROCKS_ASSERT`의 실패 쪽이다(`ReduceBoneInfluences`의 인자 계약 넷, `QuantizeSkinWeights8`의 잔차 범위 둘 — 합이 65535이고 영향이 8개 이하면 첫 영향은 32 이상이고 잔차는 ±4 안이다).
- 5개는 메모리 상한이다: 두 빌드의 삼각형 수 상한(index 수 uint32·`MeshMikkTSpaceMaxTriangles`) 넷, 원본 사본의 `AddPolygonGroup` 실패 하나(12.2절과 같다).
- 6개는 12.2절에 적은 MA2a 그대로다(`BasisWithUv`의 UE 식 셋, `IsFiniteVector`의 y·z 성분 단락 둘, MikkTSpace 단언 하나).

측정 중 닿을 수 없음을 확인한 구조는 고쳤다: morph 재계산 판정의 "위치 속성 있음" 조건(MorphNormal이 없는 target은 늘 위치 속성이 있다), 원본 사본의 가중치 접두 속성 요소 검사(원본 검증이 보장한다).

### 12.5 요구 대조표(MA6a — 애니메이션 클립 핵심)

시험은 EngineTests의 `Clip_*`(`Engine/Source/Runtime/Animation/Tests`, 17개)다. 진단 이름표는 `AssetPackage_DiagnosticNames`가 새 코드까지 본다. `FrameRate` 미러는 클립 패키지 왕복(원본·쿠킹 비율, 덮어쓰기 표)으로 확인한다.

| 요구 | 시험 |
|---|---|
| 7.1 `AnimationAsset`·`AnimationClip` 저장 → 로드 → 재저장 byte 일치(에디터·쿠킹), 두 번 쓴 byte 일치, 쿠킹 패키지의 EditorOnly 제외 | `Clip_PackageRoundTripResaveIdentical` |
| 7.2 원본 상한(비율·프레임·트랙 수)·트랙 규칙(이름·원소 수·유한·회전 정규화 경계·대소문자만 다른 중복) | `Clip_ValidateSourceRules` |
| 7.2·7.5절 6 상수 트랙 접기의 bit 보존·이름 순 정렬(접은 byte, 접두·숫자)·본 트랙 bulk 왕복 | `Clip_PackageRoundTripResaveIdentical`, `Clip_StoreOrdersAndFolds` |
| 7.5절 6 원본 bulk 변조(원소 크기·개수, 순서·중복, 비유한·정규화 안 된 키, 빈 ref 셋, 프레임·트랙 수 상한, 다른 pool 이름) | `Clip_LoadRejectsSourceBytes`, `Clip_StoreRejectsInvalidSource` |
| 7.2 커브 왕복(TangentCurve 키 bit), 변환 커브 채널 9개 | `Clip_PackageRoundTripResaveIdentical`, `Clip_ValidateClipRules` |
| 7.3 이벤트·동기 마커 규칙(시간·길이·NaN·확률·track·정렬)과 정렬 도우미(안정 정렬) | `Clip_ValidateClipRules` |
| 7.5절 4 플랫폼별 목표 비율 | `Clip_PlatformFrameRate`, `Clip_ValidateClipRules` |
| 7.5절 8 샘플링 규칙(0 이하·NaN·끝 이후, 선형·nlerp 최단 경로, α 문턱, Step, 상수·1키, 0 길이 회전) | `Clip_SampleSourceRules`, `Clip_SamplePlaybackRules` |
| 7.5절 9 쿠킹 변환: 원본 비율 = 목표 비율이면 키 그대로(bit), 상수 접기, 트랙 대응·가상 본·`UnknownBoneTrack`, 커브 복사·변환 커브 수, 쿠킹 샘플링 = 원본 샘플링 | `Clip_BuildSameRateKeepsKeys` |
| 7.5절 9 재샘플링(30 → 60 짝수 표본 = 원본 키, 홀수 = 중간, Step, NTSC, 정수가 아닌 표본 수·상한 초과 `IncompatibleTargetFrameRate`, 0 프레임) | `Clip_BuildResamples` |
| 7.5절 10 쿠킹 bulk 변조(원소 수, 짧은 bulk, 성분별 비유한, trackToBone 길이, sampleCount 0(`NoSamples`)·상한(`TooManyFrames`), 비율 상한(`FrameRateTooHigh`), 빈 ref, 트랙 수 상한)와 저장 인자 오류 | `Clip_LoadRejectsPlaybackBytes` |
| 7.5절 13 결정성(두 번, 호출 전 FTZ, 비정규 값, 저장 SHA-256) | `Clip_DeterministicBuild`, `Clip_FuzzSources` |
| 할당 실패 주입(쿠킹 변환·원본·쿠킹 저장·검증·트랙 찾기·원본·쿠킹 로드 — 결과 그대로·같은 결과) | `Clip_AllocationFailures`, `Clip_LoadAllocationFailures` |
| 고정 seed 퍼징(무작위 원본 300개 — 검증, 쿠킹 불변식, 두 번 변환 일치) | `Clip_FuzzSources` |
| 11 `InvalidClipData`·`UnknownBoneTrack`·`EventPropertiesDropped` 등록·이름·4.6절 변환 | `Clip_DiagnosticNames`, `AssetPackage_DiagnosticNames` |

Coverage(2026-11-21, clang source-based — `Build/MeasureCoverage.ps1`, 필터 `Clip_`·`Skeleton_`): 시험 32개 모두 통과. Animation Private·Public 전체는 줄 1618/1618(100%), 분기 819/826(99.15%), 함수 126/126이다. 클립 파일만 보면 다음과 같다.

| 파일 | 줄 | 분기 |
|---|---|---|
| ClipSource.cpp | 418/418 | 170/174 |
| ClipPlayback.cpp | 315/315 | 154/155 |
| AnimationClip.cpp | 215/215 | 160/161 |
| AnimationInternal.h·AnimationClip.h·ClipSource.h | 49/49 | 6/6 |

닿지 않은 분기 7개 가운데 6개는 `ROCKS_ASSERT`의 실패 쪽이다(용량을 미리 잡은 뒤의 추가 넷 — 트랙 이름 표·정렬 표·저장 트랙·로드 트랙, 커브 이름 표 하나, 상수 접기의 줄이는 크기 조정 하나). 나머지 하나는 12.3절에 적은 MA4a의 `FindNamed` 다른 pool 비교다. 측정 중 닿을 수 없음을 확인한 구조는 고쳤다: 커브 키 재검증(TangentCurve API·검증 hook이 이미 지킨다 — 7.5절 7), 샘플링의 `TryAsFrameTime` 실패·음수 프레임(양수 시간만 그 경로로 간다), 유한 판정의 성분별 단락(v − v 합 한 번), 쿠킹 로드의 원소별 유한 단락(개수 세기), 상수 접기의 빈 배열.

### 12.6 요구 대조표(MA3a — 단순 충돌 값·검증)

시험은 EngineTests의 `Collision_*`(`Engine/Source/Runtime/Mesh/Tests`, 8개)다. 패키지 왕복은 fixture 에셋 `TestCollisionAsset`(AssetTestFixtures `MeshAssets.h` — `SimpleCollision` 필드·배열·EditorOnly 사본, `CollisionComplexity`)으로 한다. 진단 이름표는 `AssetPackage_DiagnosticNames`가 새 코드까지 본다.

| 요구 | 시험 |
|---|---|
| 4.6·4.8절 1 형상 struct·공통 필드의 UE 기본값, 열거 번호 | `Collision_Defaults` |
| 4.8절 2 형상 규칙마다 하나씩(위치·회전·restOffset 비유한, 크기 0·음수·비유한과 성분 번호, 길이 음수·0 허용, 실린더 길이 0) | `Collision_ValidateRules` |
| 4.8절 3 볼록 transform(비유한·정규화·크기 0, 허용 경계)과 점·index 규칙(점 수 하한·상한 경계, 비유한 점, index 수·범위, 삼각형 수 2V − 4, index 없음 허용) | `Collision_ValidateRules`, `Collision_ConvexGeometryRules` |
| 4.8절 4 형상 수 상한(경계 포함) | `Collision_ValidateRules` |
| 저장 → 로드 → 재저장 byte 일치(에디터·쿠킹), 두 번 쓴 byte 일치, 볼록 bulk(점 double bit·index) 왕복, EditorOnly 제외 | `Collision_PackageRoundTripResaveIdentical` |
| 4.8절 3·5 볼록 bulk 변조(길이·빈 ref·점 수·index 범위 — CorruptPackage), bulk 검증의 형상 번호, 저장 거부 | `Collision_LoadRejectsConvexBytes` |
| 할당 실패 주입(저장·로드·bulk 검증 — 결과 그대로·같은 결과) | `Collision_AllocationFailures` |
| 고정 seed 퍼징(형상 값·볼록 점 400개 — 같은 진단, 저장 byte 결정성) | `Collision_FuzzValues` |
| 11 `InvalidCollisionData`·`UnsupportedCollisionShape` 등록·이름, 받지 않는 형상 경고 형식(4.8절 6), 4.6절 변환 | `Collision_DiagnosticNames`, `AssetPackage_DiagnosticNames` |

Coverage(2026-11-21, clang source-based — `Build/MeasureCoverage.ps1`, 필터 `Collision_`): 시험 8개 모두 통과. `SimpleCollision.cpp` 줄 375/375(100%), 분기 182/182(100%), 함수 27/27, `SimpleCollision.h` 줄 9/9. 측정 중 형상 종류별 템플릿 인스턴스마다 위치 판정 분기가 따로 세어져(구 등 일부 종류에서 회전 비유한을 만들 수 없는 조합) 위치·크기·길이 판정을 템플릿 밖 함수 하나(`CheckSized`)로 모았고, 점·index bulk 추가를 같은 자리에서 실패를 처리하는 반복으로 바꿨다(두 번째 entry 추가 실패는 bulk 원본의 용량이 미리 잡혀 있어 따로 닿지 않는다).

## 13. 단계

ID는 이 문서가 부여한다(MA). 착수는 사용자가 정하고, 고정 구현 순서를 바꾸지 않는다. 단, 사용자 지시(2026-11-21 "진행 가능한거부터")에 따라 에셋 DB·쿠커·머티리얼 클래스에 기대지 않는 부분을 a 단계(MA2a·MA3a·MA4a·MA5a·MA6a)로 떼어 먼저 한다. 떼어 낸 부분은 순수 함수·값 타입·A1 수준 에셋 클래스뿐이라, 뒤 단계(MA2b·MA4b)가 그 위에 쿠킹 처리기·임포트 서비스를 얹을 때 바꿀 계약이 없다.

| 단계 | 내용 | 선행 |
|---|---|---|
| MA1 | `MeshSource` 모듈: 저장 형식·검증·위상 조회·값 정리, 시험. **구현(2026-11-20, d5405bfa)**: 모듈 `Engine/Source/Runtime/MeshSource`, AssetRuntime `MemoryBulkSource`·메시 진단 코드 5개, 시험 52개 + AssetRuntime 3개(12.1절 대조표·coverage — 줄 99.05%, 분기 94.86%, 닿지 않은 분기는 단언·메모리 상한), 정한 점은 3.7절 | A1(패키지·Bulk) |
| MA2a | 선행 조건에 기대지 않는 빌드 핵심: `Mesh` 모듈에 빌드 설정 값 타입(`MeshBuildSettings`), `MeshTools` 모듈에 `MeshSource` → 메모리 빌드 결과(렌더 정점·index·섹션·bounds) 순수 함수(5.2절 1–5·8–10·12단계, 6·7·11단계는 후속이라 경고 또는 원본 순서), 용접·퇴화 판정 임계값 상수를 UE 원본으로 확정, MikkTSpace ThirdParty(결정적 수학 패치)와 참조 구현 대조 시험, 필요한 결정적 초월 함수는 Core에 더함. 쿠킹 형식 패킹(13단계)·`StaticMesh` 클래스·슬롯 머티리얼 참조·쿠킹 처리기·임포트 서비스는 MA2b. 슬롯 대응(10단계)은 슬롯 이름 목록만 입력으로 받는다. **구현(2026-11-21, 미커밋)**: 모듈 `Mesh`·`MeshTools`, ThirdParty MikkTSpace(3e895b49d05e + Rocks.patch), Core `deterministic::Acos`, 진단 코드 4개, 시험 26개 + Core 2개(12.2절 대조표·coverage — Mesh·MeshTools 줄 99.7%, 분기 98.5%), 정한 점은 5.4절 | MA1 |
| MA2b | `StaticMesh` 클래스·슬롯·섹션 정보·소켓, 쿠킹 렌더 데이터 패킹(5.3절), 쿠킹 처리기·임포트 서비스, 로드 경로에서 `ValidateSimpleCollision`·`ValidateSimpleCollisionBulk`를 부른다, CoreTypeBindings `Box<double>` 미러(MR8), `IndexFormat` GraphicsTypes 이동 확인(MR7) | MA2a, A2–A4, `MaterialBase` 에셋 클래스(MaterialSystem MS3a — 골격만, 2026-11-23 앞당김) |
| MA3a | 단순 충돌 값 타입(4.6절 `SimpleCollision`·형상 struct·`CollisionComplexity`)과 검증(유한값·양의 크기·볼록 형상 bulk 길이·index 범위), `UnsupportedCollisionShape` 등록 — `Mesh` 모듈, `StaticMesh` 필드 연결은 MA2b. **구현(2026-11-21, 미커밋)**: `Mesh/SimpleCollision.h`(형상 struct 여섯·`CollisionEnabled`·`CollisionComplexity`·`SimpleCollision`, 검증, 볼록 bulk 저장·로드), 진단 코드 `InvalidCollisionData`·`UnsupportedCollisionShape`, 시험 8개(12.6절 대조표·coverage), 시험 fixture `TestCollisionAsset`, 정한 점은 4.8절 | MA2a |
| MA4a | `Animation` 모듈: `BoneHierarchy`(불변식 검증)·`Skeleton` 에셋 클래스(저장·로드 왕복), 병합 판정 함수(부모 사슬 대조, `SkeletonMismatch`)까지 — 에셋 DB 없이 되는 부분. **구현(2026-11-21, 미커밋)**: 모듈 `Engine/Source/Runtime/Animation`, 진단 코드 `InvalidSkeletonData`·`SkeletonMismatch`, 시험 15개(12.3절 대조표·coverage), 정한 점은 6.4절 | A1 |
| MA4b | 스켈레톤 임포트 서비스·기존 스켈레톤 병합 저장·너겟 무효화, 로드 경로에서 `ValidateSkeleton`을 부른다 | MA4a, A2–A4 |
| MA5a | 스킨 빌드 핵심(순수 함수): `SkinnedMeshBuildSettings`, 원본 스킨 가중치를 렌더 정점까지 옮기기(용접 비교에 영향 포함), 영향 줄이기·8/16비트 정규화(`InfluencesTrimmed`), LOD 통합 bone map·필요 본 목록·inverse-bind 행렬(MA4a `BoneHierarchy`), morph target 희소 변화량 빌드(`morphThresholdPosition`) — 메모리 결과까지. **구현(2026-11-21, 미커밋)**: `Mesh`의 `SkinnedMeshBuildSettings`(리플렉션 파생 struct), `MeshTools`의 `BuildSkinnedMeshLod`·`ReduceBoneInfluences`·`QuantizeSkinWeights8`(5.2절 단계 함수는 공용 파일로 옮김), 진단 코드 `InfluencesTrimmed`·`MissingInfluences`, 시험 16개(12.4절 대조표·coverage), 정한 점은 6.5절 | MA2a, MA4a |
| MA5b | `SkinnedAsset`·`SkeletalMesh` 클래스, 쿠킹 렌더 데이터 패킹(`SkinnedRenderLod`), 쿠킹 처리기·임포트 서비스, 스켈레톤 병합 저장 연결 | MA5a, MA2b, MA4b |
| MA6a | 애니메이션 클립 핵심: `AnimationAsset`·`AnimationClip` 클래스(저장·로드 왕복), `ClipSourceData`·이벤트·동기 마커·설정 검증(`InvalidClipData`·`UnknownBoneTrack`), 원본 샘플링, 쿠킹 데이터 v1로 바꾸는 순수 함수(상수 트랙 접기·재샘플링)와 쿠킹 데이터 샘플링, CoreTypeBindings `FrameRate` 미러(MR8). **구현(2026-11-21, 미커밋)**: `Animation`의 `AnimationClip.h`(리플렉션)·`ClipSource.h`(메모리 형태·검증·Load·Store·샘플링·`BuildClipPlayback`), CoreTypeBindings `FrameRate` 미러, 진단 코드 `InvalidClipData`·`UnknownBoneTrack`·`EventPropertiesDropped`, 시험 15개(12.5절 대조표·coverage), 정한 점은 7.5절 | MA4a, Curve 단계(완료) |
| MA6b | 클립 쿠킹 처리기·임포트 서비스·스켈레톤 병합 뒤 재쿠킹(너겟 무효화), 로드 경로에서 `ValidateAnimationClip`(에디터)과 쿠킹 데이터의 스켈레톤 대조(`trackToBone` 본 번호 < 스켈레톤 본 수·중복 없음, 회전 키 |길이² − 1| ≤ `BoneRotationNormalizedTolerance`)를 부른다 | MA6a, MA4b |
| MA7 | 명령행·문서·검증 보고 | MA1–MA6b |

렌더 쪽 소비: Renderer RS0(정적 메시)가 MA2 결과를, RS12(skinned)가 MA5 결과를 쓴다(Renderer 17절 RQ19).

## 14. 결정 사항

| ID | 결정 | 버린 대안 |
|---|---|---|
| MA-D1 | 원본은 UE와 같은 다각형 + 삼각 분할 모델, 공유 가능한 코너 속성 방식 | 삼각형만 저장: n-gon·hard edge 정보를 잃어 재삼각화·모델링 도구가 불가능하다 |
| MA-D2 | 속성 하나 = Bulk entry 하나 | 메시 전체를 네이티브 blob 하나로(UE `FMeshDescriptionBulkData`): 리플렉션 바깥 직렬화 경로가 생기고, 빌드가 필요 없는 속성까지 풀어야 하며, 속성 단위 길이 검증·SHA 대조를 할 수 없다 |
| MA-D3 | `MeshSource`를 Runtime 모듈에 두고 AssetRuntime에 의존 | Developer에 두기: 에셋 클래스(Runtime)의 EditorOnly 필드 타입이 Runtime에서 보여야 한다. UE도 MeshDescription이 Runtime 모듈이다 |
| MA-D4 | 애니메이션 원본 키는 float, 프레임 균일, 프레임별 평가값 | double 저장: UE 원본 형식·런타임이 float다. Euler 채널 원본 저장: UE 내부 형식(FK Control Rig 채널)에 묶인다 |
| MA-D5 | MikkTSpace 참조 구현을 ThirdParty로 두고 초월 함수(`acos` 등)를 Core 결정적 수학으로 바꾸는 패치를 둔다. 패치 버전을 너겟 키에 넣는다 | 패치 없이 그대로: 호스트 libm에 따라 tangent 비트가 달라져 쿠킹 결정성이 깨진다. 자체 구현: 목적이 참조 구현과의 일치라 옮겨 적는 것과 차이가 없다 |
| MA-D6 | 소켓·충돌 형상은 값 구조체 배열 | UE처럼 UObject: 하위 객체 정체성이 필요한 기능(개별 참조·편집 transaction)이 아직 없다. 필요해지면 하위 객체로 옮긴다 |
| MA-D7 | 축소·lightmap UV·Nanite 등은 설정만 보존 | 지금 무시하고 빼기: 교환 형식으로 가져온 UE 설정이 사라져 나중에 다시 가져와야 한다 |
| MA-D8 | 퇴화 삼각형 판정·용접 임계값은 UE와 같은 식 | 면적 0 판정: 일직선 위의 서로 다른 세 점을 지워 UE와 위상이 달라진다 |
| MA-D9 | 위치 비유한값은 오류(UE는 0으로 고침) | UE처럼 고치기: 형상이 조용히 망가진다. 정상 콘텐츠에서는 나오지 않는다 |
| MA-D10 | index 형식은 GraphicsTypes `IndexFormat` 번호(MA2 전 이동, RenderBase R20 — 개정 4에서 바꿈) | Graphics `IndexFormat` 그대로 사용: 에셋 모듈이 RHI 모듈에 의존한다. Mesh 자체 열거(개정 2–3의 `MeshIndexWidth`): 같은 뜻의 열거가 둘이 된다 |

## 15. 다른 문서에 반영할 요구

| ID | 대상 | 요구 | 상태 |
|---|---|---|---|
| MR1 | [렌더 기반](RenderBase.ko.md) 10절 | 정적 메시 정점 입력 타입: stream 집합(위치 Float3, tangent 기저 Byte4Snorm×2 또는 Short4Snorm×2, UV 쌍 Half4/Float4 × 1–4, 색 프로필별 선택). 고정밀 형식은 퍼뮤테이션 축이 아니라 stream 형식 차이로 처리할지 정한다 | 반영(RenderBase 개정 29, 10.4절 끝) |
| MR2 | 렌더 기반 10절, [렌더러](Renderer.ko.md) RS12 | skinned 정점 입력: 영향 4/8개, 번호 8/16비트, 가중치 8/16비트, 섹션 본 palette 크기 한도 | 반영·결정(RenderBase 개정 30 결정 R18·R19: 섹션 분할 없음·LOD 통합 bone map·256 기준 번호 폭·typed buffer 본 행렬, 고정밀은 stream 형식 차이) |
| MR3 | [텍스처 스트리밍](TextureStreaming.ko.md) | 메시 LOD 스트리밍(StreamableRenderAsset 공유): LOD별 bulk 묶음, 상주 LOD 규칙 | 반영·결정(TextureStreaming 개정 6 결정 T9·4.9절·TS5) |
| MR4 | [머티리얼 시스템](MaterialSystem.ko.md) 8.4절 | 메시 정점 입력 타입 이름(정적·skinned)을 사용 대상 집합에 등록. morph 사용 여부 표시 | 반영(MaterialSystem 개정 6, 8.4절) |
| MR5 | [물리 어댑터](PhysicsJoltAdapter.ko.md) | `SimpleCollision` 형상 → Jolt 형상 대응, `CollisionComplexity`, 볼록 형상 쿠킹 | 반영(PhysicsJoltAdapter 개정 1, 4절·J20·J21) |
| MR6 | [엔진 구현 계획](EngineImplementationPlan.ko.md) 13.1·15절 | 이 문서를 가리키는 연결(개정 1에서 반영) | 반영(개정 1) |
| MR7 | 렌더 기반(GraphicsTypes) | `IndexFormat`을 Graphics에서 GraphicsTypes로 옮긴다(직렬화하는 쪽이 장치 없는 모듈을 쓰게) | 반영·결정(RenderBase 개정 30 결정 R20 — 고정 번호표 UInt16 = 1·UInt32 = 2, 이동은 MA2 착수 전) |
| MR8 | Core 리플렉션 미러 | `FrameRate`·`Box<double>`·`Rotator<double>`·`Transform`·`LinearColor`의 Binds 미러가 있는지 확인하고 없으면 더한다(필드 타입으로 쓴다) | 확인함: `Rotator<double>`·`LinearColor`·`Transform` 미러는 있다(CoreTypeBindings/CoreValueTypes.h). `FrameRate`는 MA6a에서 더했다(7.5절 3). `Box<double>`는 MA2b에서 더한다 |
| MR9 | [에셋 시스템](AssetSystem/12-Textures.ko.md) 12.4.5절·TargetProfile | 메시 LOD 그룹 표(screenSize·minLod 기본값)를 텍스처 LOD 그룹과 나란히 둔다 | 반영(AssetSystem 개정 10, 12.4.5절 — UE `FStaticMeshLODGroup` 멤버 대응) |
| MR10 | 에셋 시스템 3·4.6절 | 모듈 그래프에 8절 다섯 모듈 추가, 11절 진단 코드 등록 | 반영(AssetSystem 개정 10, 3·4.6절) |

## 16. 개정 이력

- 개정 1(2026-11-20): 초안. 사용자 지시(2026-11-19 "언리얼에서 텍스쳐·스테이틱메시·매터리얼·애니메이션을 임포트하는 기능 … 관련 자료구조 잡는것도 같이")에 따라 작성했다. UE 5.8.4 조사 세 갈래(텍스처·정적 메시 원본 / 스켈레톤·스킨 메시·애니메이션 / 머티리얼 그래프·플러그인 기반)를 근거로 했다.
- 개정 2(2026-11-20): 교차 검증(설계 반례 리뷰, UE 사실 확인) 반영. 17절.
- 개정 3(2026-11-20): 15절 요구 MR1–MR10을 대상 문서에 반영했다(RenderBase 29, Renderer 10, TextureStreaming 5, MaterialSystem 6, PhysicsJoltAdapter 1, AssetSystem 10). MR8은 확인 결과 `FrameRate`·`Box<double>` 미러만 없어 MA2·MA6 단계에 넣었다. 15절에 상태 열을 더했다.
- 개정 4(2026-11-20): MA2 전 결정을 권장안으로 확정해 반영했다(사용자 지시 "MA2 전 결정 사항들도 권장안대로"). index 형식은 GraphicsTypes `IndexFormat` 번호(자체 `MeshIndexWidth` 삭제, MA-D10 개정), skinned 쿠킹 데이터는 섹션별 palette 대신 LOD 통합 bone map과 256 기준 번호 폭(RenderBase R18), 메시 LOD 스트리밍은 TextureStreaming 4.9절(T9). 15절 MR2·MR3·MR7 상태를 고쳤다.
- 개정 5(2026-11-20): MA1 `MeshSource` 모듈 구현에서 정한 점을 3.7절에 적었다(위치 속성 `Position`, Load·Store 인자와 AssetRuntime `MemoryBulkSource`, 로드·검증 판정 분담, 엔진 이름 속성 판정, 스킨 가중치 중복·합 0, 값 정리 순서, 가변 길이 값의 메모리 형태, 진단 형식, 위상 조회). 3.2절 표에 `Position`을 더하고 13절 MA1 상태를 적었다.

- 개정 6(2026-11-21): 사용자 지시 "진행 가능한거부터"에 따라 MA2를 MA2a(빌드 핵심 — 순수 함수·MikkTSpace)와 MA2b(클래스·쿠킹·임포트)로, MA4를 MA4a(본 계층·스켈레톤 클래스·병합 판정)와 MA4b(임포트·병합 저장)로 나눴다. MA3·MA5·MA6의 선행을 b 단계로 고쳤다.

- 개정 7(2026-11-21): MA2a 구현에서 정한 점을 5.4절에 적었다(API·입출력 형태, 지금 모듈 의존, 원본 사본의 범위, UE 원본 임계값 확정과 removeDegenerates를 끈 때의 정확 일치 퇴화 제거, 정점 단위 법선 묶음과 대각선 soft, 비유한 중간값, buildScale 검사(`InvalidSetting`), MikkTSpace 저자 원본·패치·원본 버그 수정·참조 글루, Core `Acos`, 용접 색 비교, 섹션·슬롯, 경고 순서, 결정성, 진단 등록, 시험 도우미 이동). 5.2절 9단계 임계값 문장, 12.2절 대조표, 13절 MA2a 상태를 고쳤다.

- 개정 8(2026-11-21): MA4a 구현에서 정한 점을 6.4절에 적었다(모듈 의존, 계층 불변식의 순서와 경계, pose 정규화 함수, 스켈레톤 필드 규칙, 새 진단 코드 `InvalidSkeletonData`와 `SkeletonDiagnostic`, UE 기본 경로를 옮긴 병합 판정과 대소문자 이름 바꾸기, 결정성, 시험 환경). 11절 표에 `InvalidSkeletonData`를 더하고 12.3절 대조표, 13절 MA4a 상태를 고쳤다.

- 개정 9(2026-11-21): MA3·MA5·MA6도 같은 방식으로 a·b로 나눴다(MA3a 충돌 값·검증, MA5a 스킨 빌드 핵심, MA6a 클립 핵심 — 에셋 DB·쿠커 없이 되는 부분). UX1a는 엔진 플러그인 빌드 지원(BuildTargets BS4 엔진 쪽)이 없어 보류했다.

- 개정 10(2026-11-21): MA5a 구현에서 정한 점을 6.5절에 적었다(API·결과 형태, 설정 struct 상속과 필드 번호, 스킨 빌드의 buildScale 보존, 설정 검사와 영향 상한 기본값, 가중치를 담은 원본 사본, 스킨 임계값과 번호가 겹친 삼각형 제외, 영향 줄이기와 루트 본 대체(`MissingInfluences` — 새 경고), 용접의 스킨 항목(영향·프로필·morph), 가중치 프로필 빌드, 쿠킹 칸 수와 8비트 잔차, bone map과 필요 본, inverse-bind와 `SingularBindPose`, morph target 짝·법선 재계산·담는 조건, 경고 순서, 정적 빌드와의 단계 공유, 진단 등록, 결정성). 11절 표에 `MissingInfluences`를 더하고 12.4절 대조표, 13절 MA5a 상태를 고쳤다.

- 개정 11(2026-11-21): MA6a 구현에서 정한 점을 7.5절에 적었다(파일·의존, `AnimationAsset`과 Build 의존 시점, CoreTypeBindings `FrameRate` 미러와 검증 hook, 플랫폼별 목표 비율 `PlatformFrameRate`, 커브 표시 bool과 변환 커브 채널 배열, 원본 bulk 배치·정렬·상수 접기, 원본 검증, UE `Evaluate`와 같은 샘플링 규칙(7.4절의 slerp를 UE nlerp로 바꿈), 쿠킹 변환의 정수 표본 수·트랙 대응·변환 커브 제외, 쿠킹 bulk 배치, 클립 검증·정렬, 진단 등록, 결정성). 13절 MA6a 상태, 15절 MR8 상태, 12.5절 대조표를 고쳤다.

- 개정 12(2026-11-21): MA3a 구현에서 정한 점을 4.8절에 적었다(형상 struct와 공통 필드·UE 기본값·열거 번호, 크기 규칙(반경·크기 양수, 캡슐 길이 0 허용, 실린더 길이 양수, 유한값), 볼록 형상 bulk 배치와 점 수·index·삼각형 수 상한·transform 규칙, 형상 수 상한, 검증 함수와 새 진단 `InvalidCollisionData`, 임포터의 `UnsupportedCollisionShape` 형식, 결정성). 11절 표에 `InvalidCollisionData`를 더하고 12.6절 대조표, 13절 MA3a 상태를 고쳤다.

- 개정 13(2026-11-21): MA 묶음 리뷰를 반영했다. 원본 로드의 Name 값 bulk 최소 길이 판정과 다각형 묶음 수 상한 `MeshMaxPolygonGroups`(3.1절, 3.7절 12), 겹치는 코너의 UE 가중합 정렬 키(5.4절 16), MikkTSpace 느린 경로의 자기 index 방어와 오브젝트 검사를 미룬 사유(5.4절 17), 본 없는 메시 병합 거부(6.4절 6), 쿠킹 데이터 로드의 규칙별 진단과 신뢰 범위(7.5절 10), 12절 머리 목록(스레드·palette·재쿠킹 항목 정리), 13절 b 단계 행의 로드 경로 검증 항목.
- 개정 14(2026-11-23): MA2b의 머티리얼 선행을 MaterialSystem MS3(에셋 모듈 전체)에서 MS3a(`MaterialBase`·`Material` 골격)로 좁혔다. 슬롯 참조 타입만 있으면 되기 때문이다(사용자 결정 2026-11-23 "Material 에셋 클래스만 먼저 만들어").

## 17. 교차 검증 기록(개정 2)

리뷰어 두 명이 병렬로 봤다. 설계 반례 리뷰(초안과 기존 스펙·코드 대조)와 UE 5.8.4 사실 확인(주장 약 170개 대조, 문제 27건)이다. 이 문서에 해당하는 지적과 조치는 다음과 같다.

| 지적 | 심각도 | 조치 |
|---|---|---|
| 참조 struct(`MeshMaterialSlot`)를 `Array`에 담음, 빌드 설정의 객체 참조(`DistanceFieldReplacementMesh`)가 LOD 배열을 참조 struct 배열로 만듦 | 차단 | `StructRefArray`로 바꿈, 대체 메시는 StaticMesh EditorOnly 필드 + Build 의존(4.1·4.3·5.1절) |
| 코너를 "다각형 꼭짓점 하나"로 정의했으나 UE vertex instance는 공유됨, "세 코너는 서로 다른 정점" 불변식이 퇴화 삼각형 보존과 모순 | 차단 | 코너 공유 명시, 정점 중복 금지 삭제(3.1절) |
| UE `ComputeTriangleTangentsAndNormals`가 퇴화 삼각형을 지운다는 근거가 틀림(실제는 정점 버퍼 생성 때 위치 일치 판정) | 중요 | 빌드 순서·판정식을 UE와 같게 고침(5.2절 3·9단계, MA-D8) |
| MikkTSpace 참조 구현의 CRT `acos`가 쿠킹 결정성을 깸 | 중요 | 결정적 수학 패치·너겟 키(MA-D5, 9.3절), 재샘플링 slerp도 같은 규칙(7.4절) |
| 클립 쿠킹이 스켈레톤 본 번호에 의존하는데 Hard 참조뿐이라 병합 뒤 오래된 너겟이 적중함, 메시·클립 너겟 키 표 없음 | 중요 | Skeleton 참조를 Hard + Build로(7.1절), 너겟 키 표(9.3절) |
| 색 형식을 BGRA로 고정(GL 프로필은 UByte4Unorm), `IndexFormat`이 Graphics에 있음 | 중요 | 프로필별 색 형식, 자체 `MeshIndexWidth`, 요구 MR7(5.3절, MA-D10) |
| 속성 검증 없음(offset·boneIndex·희소 번호·Name 길이·대소문자 중복) | 중요 | 3.4.1·3.5절, 시험 추가 |
| 클립 `frameCount`·frame rate 상한 없음(OOM·넘침) | 중요 | `ClipMaxFrames`·`ClipMaxFrameRate`(7.2절) |
| 가변 길이 값(Name·BoneInfluences·SparseVec3)의 bulk 배치가 정해지지 않음, 스킨 가중치 "배열 둘"과 `data` 하나 불일치 | 중요 | 3.4.1절 값 타입별 배치 |
| UE 위치·색 비유한값 처리 설명이 틀림(UE는 위치 0, 색 흰색) | 사실 | 3.3절 고침, 위치는 UE와 다르게 오류로 둔 이유 명시(MA-D9) |
| UE morph 법선 속성 이름이 `Morph-`로 같음 | 사실 | 3.2절 표 고침, `MorphNormal-`로 바꾸는 이유 명시 |
| 중요도 필드 넷, 설정 여섯, skin weight profile 기본값이 플랫폼별, `bAutoComputeLODScreenSize` 접근자, `GetHiResSourceModel` 행, ReferenceSkeleton 경로·행, `bOnlyOneRootAllowed` 조건, UAnimationAsset 위치, StaticMeshBuilder 행, Color 기본값 행 | 사실 | 해당 줄 고침 |
| 축소 판정이 "UE와 같다"는 과장(UE는 축소 모듈 판정) | 사실 | 근사임을 명시(4.2절) |
| 빈 `MeshSourceData` = 원본 없음이 정상 빈 메시와 구분되지 않음 | 사소 | `hasSource` 플래그(3.4.2·5.1절) |
| buildScale 음수 winding·tangent 규칙, UV 범위 진단, stable bone ID·inverse-bind 누락(계획 13.1·15절) | 사소 | 5.2절 8단계, 3.3절, 6.1·6.3절 |
| 쿠킹 영향 상한 8로 9–12번째 영향을 버리는 결정과 8비트 잔차 규칙 누락 | 사소 | 6.3절 |
| UV 8채널 + 영향 8 등이 정점 속성 상한 16을 채움 | 사소 | UV 쌍 묶기(5.3절) |
| 이름 없는 수치, enum 기본값 없음 | 사소 | 이름 있는 상수, `Invalid = 0` |
| `BulkReader` 타입 없음, MA-D2 근거 "속성 하나만 다시 쓰기"가 사실이 아님 | 사소 | `Package` 기반 API, 근거 고침 |
| additive 기준 클립 참조 종류 근거 없음 | 사소 | 쿠킹이 미리 계산하지 않아 Hard임을 명시(7.1절) |
| 다른 문서 요구 누락(미러·LOD 그룹·모듈 그래프·진단 등록) | 사소 | MR7–MR10 |
| 타입 이름이 UE에서 접두만 뺀 것 | 판단 | 빌드 설정·축소 설정·섹션 정보·스킨 asset은 업계 공용 서술 이름이라 유지(머리말) |
