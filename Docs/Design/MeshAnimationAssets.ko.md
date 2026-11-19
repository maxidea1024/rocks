# 메시·스켈레톤·애니메이션 에셋 설계

기준일: 2026-11-20, 개정 4(MA2 전 결정 반영 — index 형식·통합 bone map·메시 LOD 스트리밍). 개정 3(15절 요구를 다른 문서에 반영하고 상태 열을 더함). 개정 2(교차 검증 반영 — 17절). 정적 메시·스켈레톤·스킨 메시·애니메이션 클립의 에셋 자료구조를 정한다. 범위는 원본 데이터(에디터 데이터), 저장 설정, 빌드 계약, 쿠킹 데이터, 모듈 배치다. [엔진 구현 계획](EngineImplementationPlan.ko.md) 13.1절(StaticMeshAsset)·15절(Skeleton·SkeletalMesh·AnimationClip)의 개요를 구현할 수 있는 수준으로 구체화한다. 텍스처는 [에셋 시스템](AssetSystem.ko.md) 12절, 머티리얼은 [머티리얼 시스템](MaterialSystem.ko.md) 4절이 이미 정했으므로 이 문서는 그 둘을 참조만 한다.

- 배경: 사용자 지시(2026-11-19). 엔진 시험용으로 UE에서 텍스처·정적 메시·머티리얼·애니메이션을 가져오는 기능을 넣는다. Rocks에 메시·애니메이션 자료구조가 아직 없으므로 함께 정한다. 가져오는 경로는 [Unreal 교환 형식](UnrealExchange.ko.md)이 정한다. 이 문서의 자료구조는 그 경로와 상관없이 쓰는 엔진 기반이며, 뒤에 올 glTF·FBX 임포터도 같은 구조로 들어온다.
- UE 근거: UE 5.8.4(`release`, commit `f45729c0b6f0`, 로컬 클론 `C:\github\UnrealEngine`)를 읽어 확인했다. 경로 약어는 다음과 같다. `E/` = `Engine/Source/Runtime/Engine`, `RT/` = `Engine/Source/Runtime`, `Dev/` = `Engine/Source/Developer`. UE 이름은 대응표와 근거에만 쓰고, 타입 이름은 Rocks 이름으로 짓는다. 빌드 설정·축소 설정처럼 업계 공용 서술 이름은 그대로 쓴다.
- 좌표·단위는 UE 5.8과 같다(왼손, +Z 위, cm, `Rotator`는 도). 원본·쿠킹 데이터 어디에도 좌표 변환 계층을 두지 않는다. 삼각형 winding도 UE 원본 순서를 그대로 저장한다. 앞면 판정은 렌더 쪽 몫이다([셰이더 시스템](ShaderSystem.ko.md)의 음수 행렬식 규칙).
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
- 요소 수는 `MeshMaxElements`(= 2^31 − 1) 이하다(번호 계산의 넘침 방지).
- 속성 검증은 3.5절이다.

UE의 UV 공유 요소(채널별 UV 정점 — MeshDescription.h:131-149)는 저장하지 않는다. 빌드와 UE 빌더가 쓰는 값은 코너의 UV 속성이고, UV 공유 요소는 UV 편집 도구의 위상 정보라 코너 UV에서 다시 만들 수 있다. UV 편집 도구를 만들 때 다시 검토한다.

### 3.2 속성

속성은 (요소, 이름, 값 타입, 채널 수)로 정의한다. 아래는 엔진이 이름을 정한 속성이다. 임포터·도구가 다른 이름의 속성을 더할 수 있다. 빌드는 모르는 속성을 무시하고 저장은 보존한다.

| 요소 | 이름 | 값 타입 | 채널 | 대응(UE `FStaticMeshAttributes`·`FSkeletalMeshAttributes`) |
|---|---|---|---|---|
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

`MeshSourceData`(`ROCKS_STRUCT`, 리플렉션 저장)가 영속 형태이고, `MeshSource`(C++ 클래스, 리플렉션 없음)가 편집·빌드용 메모리 형태다. 둘 사이 변환은 `MeshSource::Load(const MeshSourceData&, const Package&)`(bulk는 `Package::BulkBytes`로 읽는다)·`MeshSource::Store(...)`가 한다.

```cpp
ROCKS_ENUM() enum class MeshElement : uint8 { Invalid = 0, Vertex = 1, Corner = 2, Edge = 3, Polygon = 4, Triangle = 5, PolygonGroup = 6 };
ROCKS_ENUM() enum class MeshValueType : uint8 { Invalid = 0, Float = 1, Vec2 = 2, Vec3 = 3, Vec4 = 4, UInt32 = 5, Bool = 6, Name = 7, BoneInfluences = 8, SparseVec3 = 9 };

ROCKS_STRUCT()
struct MeshAttributeRecord {
    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() MeshElement element = MeshElement::Invalid;
    ROCKS_PROPERTY() MeshValueType type = MeshValueType::Invalid;
    ROCKS_PROPERTY() uint8 channels = 1;
    ROCKS_PROPERTY() BulkDataRef data;   // 값 타입별 배치(3.4.1절)
};

ROCKS_STRUCT()
struct MeshSourceData {
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
9. 정점 버퍼 구성: 삼각형 순서로 코너를 렌더 정점으로 옮긴다. `removeDegenerates`면 두 꼭짓점 위치가 `MeshPointsSameThreshold`(UE `THRESH_POINTS_ARE_SAME`) 안에서 같은 삼각형을 뺀다(UE 판정과 같다 — StaticMeshBuilder.cpp:1644-1674. 면적 0 판정이 아니다). 렌더 정점 용접은 위치·법선·tangent·UV·색을 UE와 같은 임계값으로 비교한다(:1724). 임계값 상수 값은 MA2에서 UE 원본으로 확정한다.
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

## 6. 스켈레톤과 스킨 메시

### 6.1 본 계층 `BoneHierarchy`

```cpp
inline constexpr int32 NoParentBone = -1;

ROCKS_STRUCT()
struct BoneInfo {
    ROCKS_PROPERTY() Name name;                         // 본의 안정 식별자(UE와 같이 이름으로 식별)
    ROCKS_PROPERTY() int32 parentIndex = NoParentBone;  // 루트만 NoParentBone
    ROCKS_PROPERTY(EditorOnly) String exportName;       // 대소문자를 보존한 원래 이름(UE ExportName)
};

ROCKS_STRUCT()
struct BoneHierarchy {
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
| `MaterialSlotFallback` | 경고 | 다각형 묶음 이름과 맞는 슬롯 없음 |
| `NegativeBuildScale` | 경고 | buildScale 음수 성분 홀수 개(winding 반전 없음) |
| `UnsupportedSetting` | 경고 | 보존 상태 설정이 기본값이 아님 |
| `UnsupportedCollisionShape` | 경고 | level set 등 받지 않는 충돌 형상 |
| `SkeletonMismatch` | 오류 | 본 병합 때 부모 사슬 불일치 |
| `UnknownBoneTrack` | 경고 | 스켈레톤에 없는 본의 트랙 |
| `InvalidClipData` | 오류 | 클립 상한·frame rate 위반 |
| `EventPropertiesDropped` | 경고 | 이벤트 종류별 속성을 버림 |

## 12. 시험

에셋 시스템 시험 원칙(변조·실패 주입·취소·동시성·퍼징까지 넓게 시험한다)을 따른다.

- **원본 메시**: 위상·속성 불변식별 위반 fixture(번호 범위, 중복 변, 둘레 3 미만, 삼각형 수 불일치, 비유한 위치, offset 비단조, 희소 번호 중복·역순·범위 밖, Name 길이 넘침, 대소문자만 다른 속성 이름, boneIndex 범위 밖). 코너 공유·퇴화 삼각형이 있는 정상 메시. 저장·로드 왕복의 비트 일치. bulk 길이 변조·잘림·SHA 불일치(`CorruptPackage`). 무작위 위상·속성 퍼징(검증이 오류를 내거나 통과하고, 통과한 것은 빌드가 범위 검사 단언에 걸리지 않고 끝남). morph 희소 저장의 `-0.0` 보존.
- **빌드**: 결정성(같은 입력 두 번, 스레드 수 바꿔서). 용접(속성 하나만 다른 코너는 합치지 않음). 퇴화 삼각형 제거 on/off와 UE 판정식(일직선 위 세 점은 남김). 슬롯 대응·대체 경고. `buildScale` 음수 성분(winding 유지, 경고). MikkTSpace 참조 구현과의 대조(공개 시험 메시, 패치 전후 차이 상한).
- **스켈레톤·스킨**: 계층 불변식, 병합 성공·부모 사슬 실패, 병합 뒤 클립 재쿠킹(너겟 무효화), 영향 줄이기 결정성(가중치 동률), 정규화 잔차, palette 분할 경계.
- **클립**: 상수 트랙 접기의 비트 보존, 재샘플링(원본 비율 = 목표 비율이면 키 그대로), 상한 초과, 스켈레톤에 없는 트랙, 커브 왕복, 이벤트 정렬.

## 13. 단계

ID는 이 문서가 부여한다(MA). 착수는 사용자가 정하고, 고정 구현 순서를 바꾸지 않는다.

| 단계 | 내용 | 선행 |
|---|---|---|
| MA1 | `MeshSource` 모듈: 저장 형식·검증·위상 조회·값 정리, 시험 | A1(패키지·Bulk) |
| MA2 | `Mesh` 정적 메시 클래스·설정 타입, `MeshTools` 빌드(5.2절 중 구현 상태 항목)·쿠킹 렌더 데이터·임포트 서비스, MikkTSpace ThirdParty(패치), CoreTypeBindings `Box<double>` 미러(MR8), `IndexFormat` GraphicsTypes 이동 확인(MR7) | MA1, A2–A4, `Material` 에셋 클래스(MaterialSystem MS3) |
| MA3 | 단순 충돌 데이터·검증 | MA2 |
| MA4 | `Animation` 본 계층·스켈레톤·병합 | A1–A4 |
| MA5 | 스킨 메시: 원본 스킨 가중치·morph, 빌드·쿠킹 | MA2, MA4 |
| MA6 | 애니메이션 클립: 원본·이벤트·설정, 쿠킹 v1, CoreTypeBindings `FrameRate` 미러(MR8) | MA4, Curve 단계(완료) |
| MA7 | 명령행·문서·검증 보고 | MA1–MA6 |

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
| MR8 | Core 리플렉션 미러 | `FrameRate`·`Box<double>`·`Rotator<double>`·`Transform`·`LinearColor`의 Binds 미러가 있는지 확인하고 없으면 더한다(필드 타입으로 쓴다) | 확인함: `Rotator<double>`·`LinearColor`·`Transform` 미러는 있다(CoreTypeBindings/CoreValueTypes.h). `FrameRate`·`Box<double>`는 없어 MA2(Box)·MA6(FrameRate)에서 CoreTypeBindings에 더한다 |
| MR9 | [에셋 시스템](AssetSystem.ko.md) 12.4.5절·TargetProfile | 메시 LOD 그룹 표(screenSize·minLod 기본값)를 텍스처 LOD 그룹과 나란히 둔다 | 반영(AssetSystem 개정 10, 12.4.5절 — UE `FStaticMeshLODGroup` 멤버 대응) |
| MR10 | 에셋 시스템 3·4.6절 | 모듈 그래프에 8절 다섯 모듈 추가, 11절 진단 코드 등록 | 반영(AssetSystem 개정 10, 3·4.6절) |

## 16. 개정 이력

- 개정 1(2026-11-20): 초안. 사용자 지시(2026-11-19 "언리얼에서 텍스쳐·스테이틱메시·매터리얼·애니메이션을 임포트하는 기능 … 관련 자료구조 잡는것도 같이")에 따라 작성했다. UE 5.8.4 조사 세 갈래(텍스처·정적 메시 원본 / 스켈레톤·스킨 메시·애니메이션 / 머티리얼 그래프·플러그인 기반)를 근거로 했다.
- 개정 2(2026-11-20): 교차 검증(설계 반례 리뷰, UE 사실 확인) 반영. 17절.
- 개정 3(2026-11-20): 15절 요구 MR1–MR10을 대상 문서에 반영했다(RenderBase 29, Renderer 10, TextureStreaming 5, MaterialSystem 6, PhysicsJoltAdapter 1, AssetSystem 10). MR8은 확인 결과 `FrameRate`·`Box<double>` 미러만 없어 MA2·MA6 단계에 넣었다. 15절에 상태 열을 더했다.
- 개정 4(2026-11-20): MA2 전 결정을 권장안으로 확정해 반영했다(사용자 지시 "MA2 전 결정 사항들도 권장안대로"). index 형식은 GraphicsTypes `IndexFormat` 번호(자체 `MeshIndexWidth` 삭제, MA-D10 개정), skinned 쿠킹 데이터는 섹션별 palette 대신 LOD 통합 bone map과 256 기준 번호 폭(RenderBase R18), 메시 LOD 스트리밍은 TextureStreaming 4.9절(T9). 15절 MR2·MR3·MR7 상태를 고쳤다.

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
