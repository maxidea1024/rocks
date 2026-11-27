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
