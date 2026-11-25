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

결정성: 같은 입력·설정·도구 버전·프로필이면 결과 bytes가 같아야 한다(AssetSystem 9·10절). 부동소수점 계산은 쿠킹 결정성 환경(`RunWithDeterministicFloatingPoint`) 안에서 하고, 초월 함수는 Core 결정적 수학만 쓴다(MikkTSpace 패치 포함 — 결정 MA-D5). UE와의 결과 일치는 비트 단위로 보장하지 않는다. 노멀·tangent를 UE와 같게 받아야 하는 시험 콘텐츠는 교환 형식의 "UE 준비 원본" 모드를 쓴다([Unreal 교환 형식](../UnrealExchange.ko.md) 5.3절).

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
