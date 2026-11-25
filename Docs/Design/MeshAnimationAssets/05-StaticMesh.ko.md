## 5. 정적 메시 `StaticMesh`

### 5.1 클래스

```text
StaticMesh : StreamableRenderAsset
  EditorOnly:
    sourceLods         : Array<StaticMeshSourceLod {
                           hasSource bool, source MeshSourceData,
                           hasBakedSource bool, bakedSource MeshSourceData,  // 다른 엔진이 축소로 만든 결과(교환 경로)
                           build MeshBuildSettings, reduction MeshReductionSettings, screenSize PerPlatformFloat }>
    sectionInfo        : Array<MeshSectionInfoEntry { lod, section, MeshSectionInfo }>   // UE SectionInfoMap도 EditorOnly
    autoComputeLodScreenSize bool(참), lodGroup Name
    virtualGeometry    : VirtualGeometrySettings                    // 보존(UE NaniteSettings, 5.5.1절 5)
    complexCollisionMesh : ObjectRef<StaticMesh>                    // 보존(물리 쿠킹 단계 — Build 의존 선언은 그때)
    distanceFieldReplacementMesh : ObjectRef<StaticMesh>            // 보존(GI 단계 — Build 의존 선언은 그때)
  저장(쿠킹에도 남음):
    materialSlots      : StructRefArray<MeshMaterialSlot>
    sockets            : Array<MeshSocket>
    simpleCollision, complexity, lodForCollision int32, doubleSidedGeometry bool
    lightMapResolution int32(4), lightMapCoordinateIndex int32(0)   // 보존(라이트맵 단계)
    positiveBoundsExtension, negativeBoundsExtension : Vec3d       // 확장 bounds는 소비자가 계산(5.5.1절 9)
    minLod PerPlatformInt32(0), numStreamedLods PerPlatformInt32(−1) // 쿠킹은 대상 프로파일 값(5.5.2절 9)
  쿠킹 데이터:
    renderData         : StaticMeshRenderData
```

- `sourceLods` 원소는 참조가 없어 `Array`다(참조 없는 struct의 `StructRefArray`는 메타 컴파일러가 거부한다 — RMC2304). LOD별 대체 메시 같은 참조가 생기면 `StructRefArray`로 바꾼다.
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
  lods   : Array<StaticMeshRenderLod>
  bounds : { box Box<double>, sphereRadius double }   // 쿠킹 LOD 0 결과(경계 확장 없음 — UE와 같다), 구 중심 = 상자 중심
}
StaticMeshRenderLod {
  screenSize      : float                          // 쿠킹 프로파일 값(5.5.2절 8)
  vertexCount     : uint32
  data            : BulkDataRef                    // 이 LOD의 stream·index를 이은 덩어리 하나(스트리밍 단위, 서버 쿠킹은 비움)
  positions       : MeshRenderStream { format = Float3 }
  tangentBasis    : MeshRenderStream { format = Byte4Snorm | Short4Snorm }  // 정점마다 원소 둘: tangent(w = 1), 법선(w = 쌍법선 부호)
  texCoords       : Array<MeshRenderStream>        // UV 두 채널을 한 속성에 묶음(Half4 | Float4), 홀수 채널의 마지막은 Half2 | Float2
  colors          : MeshRenderStream { format = 프로파일별(Bgra8Unorm | UByte4Unorm) }   // 색 없음·모두 흰색이면 format 0
  indexFormat     : uint32                         // GraphicsTypes IndexFormat 번호(UInt16 = 1, UInt32 = 2)
  indices         : MeshRenderStream { format = 0 } // index 구간(형식은 indexFormat)
  sections        : Array<MeshRenderSection { materialIndex, firstIndex, triangleCount, minVertex, maxVertex,
                                              enableCollision, castShadow, visibleInRayTracing,
                                              affectDistanceFieldLighting, forceOpaque }>
}
MeshRenderStream { uint32 vertexFormat;  // GraphicsTypes VertexFormat 번호, 0 = 없음. 로드 때 범위 검증
                   uint64 offset, byteSize }      // data 안 구간(offset은 16 byte 배수)
```

- 정점 형식은 GraphicsTypes `VertexFormat`의 고정 번호표다(VertexFormat.h:49-70). 번호를 `uint32`로 저장하고 로드 때 검증한다(enum 순서 값을 직렬화하지 않는 RenderBase 10.4절 규칙). `PackedNormal` 패킹은 Core `VertexPacking.h`를 쓴다.
- 정점 색은 8비트 sRGB로 부호화한다(UE와 같다 — StaticMeshBuilder.cpp:1704-1708). 형식은 쿠킹 프로필에 따라 D3D·Vulkan은 `Bgra8Unorm`, OpenGL ES는 `UByte4Unorm`이다(VertexFormat.h:43 주석, RenderBase 10.4절).
- UV 두 채널을 한 정점 속성에 묶는 이유: UV 8채널 + 영향 8개 + tangent·색·위치를 각자 속성으로 두면 정점 속성 상한 16(RenderBase 10절)을 채워 인스턴스 stream을 둘 자리가 없다. UE도 UV를 쌍으로 묶는다.
- index 폭은 정점 수가 `MeshIndex16MaxVertices`(= 65535) 이하면 16비트다(0xFFFF는 OpenGL ES 고정 primitive restart index라 쓰지 않는다 — 5.5.2절 1).
- LOD 하나가 mip 하나처럼 스트리밍 단위다(StreamableRenderAsset 기반). [텍스처 스트리밍](../TextureStreaming.ko.md) 4.9절대로 LOD의 정점 stream·index를 bulk 하나(`data`)로 잇고 stream 시작은 16 byte 정렬(사이는 0), 패키지 bulk 정렬은 4 KiB 하한이다(텍스처 덩어리와 같은 RenderBase 6.5절 계약). 상주 LOD 수는 런타임 TS5가 `numStreamedLods`로 정한다.
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

### 5.5 MA2b 구현 명세(개정 15)

MA2b를 셋으로 나눈다. MA2b1 = 타입과 로드 검증, MA2b2 = 쿠킹 패킹과 쿠킹 처리기, MA2b3 = 임포트 서비스다. 단계마다 이 절을 먼저 고치고 구현한다. 모두 권장안으로 정했고, 애매한 것은 UE 5.8을 따랐다. 설계 교차 검증(리뷰어 1명, 지적 20건)을 반영했다. UE 근거 약어: `E/` = `Engine/Source/Runtime/Engine`, `Dev/` = `Engine/Source/Developer`.

#### 5.5.1 MA2b1 — 타입과 로드 검증

1. **파일 배치(한 파일 한 타입).** `Mesh` 모듈 공개 헤더에 범주 폴더 둘을 둔다. `Mesh/Common/`(정적·스킨 메시가 함께 쓰는 것): `MeshUvDensity.h`, `MeshMaterialSlot.h`, `MeshSectionInfo.h`, `MeshSectionInfoEntry.h`, `MeshSocket.h`, `MeshReductionSettings.h`(+ 그 struct만 쓰는 열거 둘), `VirtualGeometrySettings.h`(+ 그 struct만 쓰는 열거 셋), `MeshRenderStream.h`, `MeshRenderSection.h`. `Mesh/StaticMesh/`: `StaticMesh.h`, `StaticMeshSourceLod.h`, `StaticMeshRenderLod.h`, `StaticMeshRenderData.h`, `StaticMeshValidation.h`. 기존 `MeshBuildSettings.h`·`SimpleCollision.h`는 그대로 두고, 옮기려면 동작 불변 이동 커밋으로 따로 한다. `Mesh` 의존에 MeshSource·Material·GraphicsTypes를 더한다(8절 표).
2. **플랫폼별 값.** [리플렉션 확장](../ReflectionExtensions.ko.md) 7.2절이 A5 몫으로 정해 둔 `PerPlatformFloat { float defaultValue; ROCKS_PROPERTY(EditorOnly) OrderedMap<Name, float> perPlatform; }`와 같은 모양의 `PerPlatformInt32`를 AssetRuntime에 둔다(`AssetRuntime/PerPlatformFloat.h`·`PerPlatformInt32.h`, `Resolve(Name profile)`). UE `FPerPlatformFloat`·`FPerPlatformInt`(CoreUObject PerPlatformProperties.h — 맵은 editor 전용)와 같다. 맵이 EditorOnly라 쿠킹 패키지에는 `defaultValue`만 남는다. 키는 TargetProfile id(`String`)를 Name으로 바꾼 값이다(Name 비교 규칙 — ASCII 대소문자 무시). UE는 플랫폼 그룹 이름도 키로 받지만 Rocks에는 플랫폼 그룹이 없다. Texture의 같은 모양 `TextureDownscale`(TextureSource.h — 맵이 EditorOnly가 아니었다)은 이 타입으로 바꾼다(중복 공용 타입 금지, 하위호환 없음). Animation `PlatformFrameRate`(7.5절 4)는 값 타입이 달라 그대로 둔다.
3. **`StaticMesh` 필드와 기본값.** 5.1절 목록이다. 기본값은 UE 생성자다: `lightMapResolution` 4(`SetLightMapResolution(4)` — E/Private/StaticMesh.cpp 4722~), `minLod` 0, `numStreamedLods` −1(`NumStreamedLODs.Default = -1`), `autoComputeLodScreenSize` 참, `lightMapCoordinateIndex`·`lodForCollision` 0, `complexity` Default, `doubleSidedGeometry` 거짓(UE는 BodySetup 필드다 — 4.6절처럼 메시 필드로 둔다). EditorOnly 구분은 UE WITH_EDITORONLY_DATA와 같다: `sourceLods`·`sectionInfo`·`lodGroup`·`autoComputeLodScreenSize`·`virtualGeometry`(UE NaniteSettings — StaticMesh.h:650–882 안)·`complexCollisionMesh`(StaticMesh.h:1608–1614 — 쿠킹 패키지가 다른 메시를 Hard로 끌어오지 않는다. 물리 쿠킹 단계에서 Build 의존으로 둔다)·`distanceFieldReplacementMesh`. `sourceLods`는 참조 없는 struct 배열이라 `Array<StaticMeshSourceLod>`다(참조 없는 struct의 `StructRefArray`는 RMC2304 — 참조가 생기면 그때 바꾼다). 쿠킹 섹션은 섹션 정보 여섯 값을 모두 담는다(UE `ResolveSectionInfo`가 `FStaticMeshSection`에 옮기는 값 — StaticMesh.cpp 3085~). 클래스는 `final`이고 `ROCKS_MESH_API`로 export한다(World의 메시 컴포넌트가 참조한다).
4. **슬롯·섹션 정보·소켓.** `MeshMaterialSlot`은 4.3절 그대로다. `MeshUvDensity`는 `initialized`·`overrideDensities` bool과 `localUvDensities float[4]`(UE `LocalUVDensities[MAX_TEXCOORDS = 4]` — E/Public/MeshUVChannelInfo.h)다. 계산(UE `UpdateUVChannelData`)은 소비자인 텍스처 스트리밍 단계에서 한다. 그때까지 빌드는 값을 바꾸지 않는다(버린 대안: MA2b에서 계산 — 소비자가 없어 맞는지 확인할 길이 없다. 4.3절도 고쳤다). `MeshSectionInfo` 기본값은 UE 생성자다(materialIndex 0, enableCollision·castShadow·visibleInRayTracing·affectDistanceFieldLighting 참, forceOpaque 거짓 — StaticMesh.h:372~). `sectionInfo` 배열은 (lod, section) 엄격 증가다. `MeshSocket`은 4.5절 필드에 UE `bSocketCreatedAtImport`(EditorOnly)를 `createdAtImport`로 더한다. `relativeScale` 기본값은 (1, 1, 1)이다(UE 생성자). 미리보기 메시는 넣지 않는다.
5. **축소·가상 기하 설정.** `MeshReductionSettings`는 4.2절 필드이고 기본값은 UE 생성자다(percentTriangles·percentVertices 1, maxTriangles·maxVertices uint32 최댓값, maxDeviation 0, pixelError 8, weldingThreshold 0, hardAngleThreshold 80, baseLod 0, 중요도 셋 Normal·visibilityAggressiveness Lowest·vertexColorImportance Off, bool 다섯 거짓, terminationCriterion Triangles — E/Private/MeshReductionSettings.cpp 7~). 열거는 `MeshFeatureImportance`(Off·Lowest·Low·Normal·High·Highest)와 `MeshReductionTermination`(Triangles·Vertices·Any)이다. 축소 활성 판정 `IsMeshReductionActive(settings, vertexCount, triangleCount)`는 UE 기본 축소기 식과 같다(Dev/MeshSimplifier/Private/QuadricMeshReduction.cpp 513~ — 기준값 `1 − 1e-4`, 종료 기준별). UE `UStaticMesh::IsReductionActive`의 LOD 그룹 덮어쓰기는 메시 LOD 그룹 표가 생길 때 더한다. `VirtualGeometrySettings`는 UE `FMeshNaniteSettings`(E/Classes/Engine/EngineTypes.h 3280~)의 값 필드 스물세 개를 같은 순서·기본값으로 둔다: enabled·explicitTangents·lerpUvs·separable·voxelNdf·voxelOpacity(bool — lerpUvs·voxelNdf만 참), shapePreservation, positionPrecision(int32 최솟값)·normalPrecision·tangentPrecision·boneWeightPrecision(−1), targetMinimumResidencyInKb(0), keepPercentTriangles(1)·trimRelativeError(0), generateFallback·fallbackTarget(첫 열거값), fallbackPercentTriangles·fallbackRelativeError(1), maxEdgeLengthFactor(0), numRays(64)·voxelLevel(0)·rayBackUp(0), displacementUvChannel(0). 열거 셋은 `VirtualGeometryShapePreservation`·`VirtualGeometryFallbackMode`(UE `ENaniteGenerateFallback`)·`VirtualGeometryFallbackTarget`이다. `DisplacementMaps`(텍스처 참조 배열)와 `NaniteAssemblyData`는 VirtualGeometry 단계에서 더한다. 그 전에 임포터가 받은 값이 비어 있지 않으면 `UnsupportedSetting` 경고다.
6. **원본 LOD.** `StaticMeshSourceLod`에 `hasBakedSource`를 더한다(빈 `MeshSourceData`도 정상 메시라 "없음"을 플래그로 나타낸다 — 3.4.2절과 같은 규칙). 생성 LOD 판정 `IsStaticMeshLodGenerated(lod)` = `!hasSource || IsMeshReductionActive(reduction, 원본 vertexCount, 원본 triangleCount)`다(UE `!IsMeshDescriptionValid(i) || IsReductionActive(i)` — StaticMesh.cpp 9772). UE의 정점 수는 겹친 코너를 속성까지 비교해 용접한 수(`GetUniqueVertexCount` — StaticMeshSourceData.cpp 364)라 Rocks의 위치 정점 수와 다르다. 이 차이는 종료 기준이 Vertices·Any이고 `maxVertices`가 두 수 사이일 때만 판정을 바꾸며, 축소기가 없는 동안 결과는 "원본을 그대로 빌드 + 경고"(5.5.2절 5)라 같다. 축소기 단계에서 UE 수를 계산한다.
7. **`Box<double>` 미러(MR8).** CoreTypeBindings에 `ROCKS_STRUCT(Binds = "rocks::Box<double>", Atomic, Version = 1)` 미러(`min`·`max` Vec3d)를 더하고 ObjectRuntime `BoundCoreValues.h` 목록에 넣는다. 쿠킹 bounds가 쓴다.
8. **`IndexFormat` 이동(MR7).** Graphics `DeviceTypes.h`의 `IndexFormat`(UInt16 = 0, UInt32 = 1)을 GraphicsTypes `IndexFormat.h`로 옮기고 고정 번호(UInt16 = 1, UInt32 = 2 — RenderBase 결정 R20)를 붙인다. API는 `VertexFormat`과 같은 모양이다(`IsValidIndexFormat`·`IndexFormatSize`·`IndexFormatNumber`·`IndexFormatFromNumber`). Graphics와 backend는 새 헤더를 쓴다. 렌더 세션에 알린다(이동은 원래 렌더 쪽 몫이었다 — 15절 MR7).
9. **로드 검증.** 쿠킹 데이터와 단순 충돌을 소비자가 쓰기 전에 검사하는 순수 함수를 `StaticMeshValidation.h`에 둔다. 위반은 `CorruptPackage`(쿠커가 만든 데이터라 위반은 손상이다)와 새 `MeshIssue` 세부 규칙이다(진단 `index` = LOD, `count` = 섹션·UV stream 번호).
   - `ValidateStaticMeshRenderData(renderData, failure)`: LOD 수 ≤ `StaticMeshMaxLods`(8, UE `MAX_STATIC_MESH_LODS` — 0도 받는다: 에디터 패키지), bounds(유한, box min ≤ max, 반지름 ≥ 0). LOD마다: `data`가 비면(서버 쿠킹 — 5.5.2절 3) 모든 stream 형식이 0·구간 0, UV stream 목록이 비어야 하고 index 형식은 0 또는 번호표 안이다. `data`가 있으면 형식 번호(번호표로 먼저 읽는다 — `uint32` 번호를 `uint8` 바탕 enum으로 바로 바꾸면 상위 비트가 잘려 손상이 통과한다. 위치 Float3, tangent 기저 Byte4Snorm·Short4Snorm, UV Half2·Half4·Float2·Float4, 색 0·Bgra8Unorm·UByte4Unorm, index 형식 번호표, 16비트 index면 정점 수 ≤ `MeshIndex16MaxVertices` — 5.5.2절 1), UV stream 불변식(수 ≤ `MeshMaxTexCoords` / 2, 모든 stream 정밀도 같음, 2성분 형식은 마지막 stream만), 구간(offset 16 byte 배수, offset + byteSize ≤ `data.rawBytes`, 넘침 없음, 배치 순서대로 겹침 없음), 크기(vertexCount × 원소 byte × 원소 수 — tangent 기저는 원소 둘, index는 3의 배수 개)를 본다. 섹션: firstIndex + 3 × triangleCount ≤ index 수(서버 LOD는 index가 없어 보지 않는다), triangleCount > 0이면 minVertex ≤ maxVertex < vertexCount, 빈 섹션은 minVertex = maxVertex = 0(MA2a 빌드 결과와 같다). 정점 0개 LOD(삼각형이 모두 퇴화)도 정상이다. `materialIndex`는 검사하지 않는다 — 슬롯 범위 밖이면 소비자가 엔진 기본 머티리얼로 그린다(UE와 같다, 슬롯 대체가 범위를 보지 않는다 — MA2a 5.4절 11).
   - `ValidateStaticMeshIndices(lod, bytes, failure)`: index 값 < vertexCount(bulk byte가 필요해 나눈다 — 업로드하는 쪽이 bulk를 읽은 뒤 부른다).
   - `ValidateStaticMesh(mesh, package, failure)`: 단순 충돌(MA3a `ValidateSimpleCollision`·`ValidateSimpleCollisionBulk`)과 렌더 데이터. `lodForCollision`은 검사하지 않는다(UE처럼 쓰는 쪽이 [0, LOD 수 − 1]로 자른다 — StaticMesh.cpp 6200, 9295. 쿠킹은 자른 값을 쓴다 — 5.5.2절 9).
   - 클래스 PostLoad는 쓰지 않는다. 텍스처와 같이(AssetSystem 12.15절 2) 소비자(렌더 연결·물리)가 쓰기 전에 부른다. 지금은 PostLoad에서 해야 할 파생값 계산이 없다.
   - 확장 bounds: UE `ExtendedBounds`처럼 `renderData` bounds에 `positiveBoundsExtension`·`negativeBoundsExtension`을 더한 값을 `StaticMeshExtendedBounds(mesh)`가 계산한다(StaticMesh.cpp 7287–7310 `CalculateExtendedBounds` — 상자 최소 − 음 확장, 최대 + 양 확장, 구 반지름은 새 상자 반 대각선 길이).

#### 5.5.2 MA2b2 — 쿠킹 패킹과 쿠킹 처리기

1. **패킹 함수.** `MeshTools/StaticMeshPacking.h`의 `PackStaticMeshLod(built, settings, sectionInfo, colorFormat, bytes, lod)`가 `StaticMeshBuildResult`(5.4절 1)와 섹션마다의 유효 `MeshSectionInfo`(6번)를 5.3절 배치로 바꾼다. 순서는 위치 → tangent 기저 → UV 쌍 → 색 → index이고 각 시작은 16 byte 정렬(사이 0)이다. tangent 기저는 정점마다 (tangent xyz, w = 1), (법선 xyz, w = 쌍법선 부호)를 성분마다 Core `PackSnorm8`·`PackSnorm16`으로 넣는다(UE `FStaticMeshVertexBuffer` — TangentX는 FPackedNormal 대입이라 W = `MAX_int8`·`MAX_int16`, TangentZ W가 부호다, StaticMeshVertexBuffer.h:39–43·PackedNormal.h:122·366, RenderBase 10.4절 배치와 같다). Core `PackTangent`는 부호를 tangent w에 넣어 이 배치와 다르므로 쓰지 않는다. UV는 채널 쌍을 Half4(가장 가까운 짝수 반올림 — UE `FFloat16::Set`)나 Float4로, 홀수 채널의 마지막은 Half2·Float2로 넣는다. 채널이 0이면 UV stream이 없다. 색은 `LinearColor::ToSRGB` 8비트를 형식 순서(Bgra8Unorm은 B·G·R·A, UByte4Unorm은 R·G·B·A)로 넣고, 원본에 색이 없거나 모든 정점이 (255, 255, 255, 255)면 넣지 않는다. index 폭은 정점 수 ≤ `MeshIndex16MaxVertices`(65535)면 16비트다. UE는 최대 index ≤ 65535(정점 65536개)까지 16비트지만(RawIndexBuffer.cpp 187), OpenGL ES 3.0 이상은 primitive restart가 고정 index(16비트면 0xFFFF)로 늘 켜져 있어 0xFFFF를 index로 쓸 수 없다.
2. **색 형식 프로파일.** 대상 프로파일 `graphicsBackends`에 `OpenGL`이 있으면 `UByte4Unorm`, 아니면 `Bgra8Unorm`이다(VertexFormat.h 주석 — GL에 BGRA 정점 배열이 없다). 여러 backend를 가진 프로파일에서도 쿠킹 결과는 하나라 모두가 받는 형식을 고른다.
3. **장치 없는 프로파일.** `graphicsBackends`가 비면(전용 서버 등) 빌드는 하되 LOD마다 `data`·stream을 비우고 bounds·화면 크기·정점 수·섹션은 남긴다. UE가 서버 쿠킹에서 정점·index 버퍼만 빼고 섹션·bounds를 남기는 것과 같다(StaticMesh.cpp 889–891·2691). 단순 충돌 등 저장 필드는 남는다.
4. **처리기.** `StaticMeshCookProcessor`(TypeIdOf<StaticMesh>()에 등록)가 텍스처 처리기(AssetSystem 12.14절)와 같은 흐름을 따른다: 객체 값 읽기 → LOD별 입력 고르기 → 너겟 조회 → 미적중·verify면 원본 로드·빌드·패킹(적중이면 원본 bulk를 읽지 않는다) → `renderData`·플랫폼 값 채우기(정점 0개 LOD도 0 byte bulk entry를 둔다 — `data`가 비어 있는 것은 서버 쿠킹뿐이다) → 쓰기(bulk 정렬 4 KiB 하한 — `WriteCookedPackage`) → 에디터 객체 되돌리기(성공·실패 모두) → 미적중이면 Put. 원본 로드는 `registry.Resolve<Package>(access, context.package)`의 Package와 `registry.Names()` NamePool로 `MeshSource::Load`를 부른다. 쿠킹이 만든 bulk를 앞에 두고 나머지는 에디터 bulk로 보내는 연쇄 원본은 텍스처 처리기의 사본을 만들지 않고 AssetCooker 공용 타입 `CookChainedBulkSource`(`AssetCooker/CookBulkSource.h`)로 옮겨 함께 쓴다(동작 불변 이동, 에셋 세션에 알린다).
5. **LOD 입력.** 원본 LOD 수가 `StaticMeshMaxLods`를 넘으면 `InvalidSetting` 실패다. LOD i마다: 생성 LOD(5.5.1절 6)이고 `hasBakedSource`면 `bakedSource`를 쓰되 빌드 설정은 그 LOD 값에서 `buildScale` = (1, 1, 1), `recomputeNormals`·`recomputeTangents` = 거짓으로 바꾼다(교환 경로의 baked는 UE 렌더 데이터에서 만들어 크기·법선·tangent가 이미 들어 있다 — UnrealExchange 5.4절 5). 생성 LOD이고 baked가 없고 `hasSource`면 원본을 축소 없이 빌드하고 `UnsupportedSetting` 경고를 낸다(LOD 0의 축소 설정만 바꾼 메시가 렌더 데이터 없이 끝나지 않게). 원본도 baked도 없으면 그 LOD를 빼고 `UnsupportedSetting` 경고다. 남은 LOD가 없으면 `InvalidSource` 실패다. 뺀 LOD 뒤의 LOD는 앞으로 당긴다(쿠킹 LOD 번호는 연속이다 — 9번이 LOD 번호 필드를 옮긴다).
6. **빌드 입력.** 슬롯 이름은 `materialSlots[*].importedSlotName`이다. 섹션 정보는 (원본 LOD, 섹션) 항목 → 없으면 (0, 섹션) 항목(UE `FMeshSectionInfoMap::Get`의 LOD 0 대체 — StaticMesh.cpp 6519~) → 없으면 슬롯 대응(materialIndex는 MA2a 5.4절 11, 나머지 다섯은 기본값)이다. UE의 마지막 대체는 `FMeshSectionInfo(섹션 번호)`라 materialIndex = 섹션 번호인데, Rocks는 이름 대응 결과를 쓴다(MA2a에서 정한 차이 — 이름이 맞으면 같다). bounds 확장은 넘기지 않는다(0 — 확장은 쿠킹 데이터 밖, 5.5.1절 9).
7. **bounds.** `renderData` bounds는 쿠킹 LOD 0 빌드 결과(확장 없음)다(UE `RenderData->Bounds` — 확장은 `ExtendedBounds`에만, StaticMesh.cpp 7287–7310).
8. **화면 크기.** `autoComputeLodScreenSize`면 UE `ResolveSectionInfo`(StaticMesh.cpp 3105~)의 식이다: 쿠킹 LOD 0은 2.0(`LOD0ScreenSize`), 그 밖은 축소기가 없어 MaxDeviation이 0이므로 0.75^i이고, 앞 LOD보다 크면 앞 값의 절반이다. 아니면 그 LOD의 원본 LOD `screenSize`를 대상 프로파일로 푼 값이다. i는 쿠킹 LOD 번호다. UE는 축소 LOD(교환 경로 baked)를 MaxDeviation 투영식으로 계산하므로 교환 임포터(UX3b)는 UE가 푼 화면 크기를 값으로 가져오고 `autoComputeLodScreenSize`를 끈다. 화면 크기는 너겟 밖에서 계산한다(프로파일이 달라도 너겟을 함께 쓴다).
9. **플랫폼 값과 LOD 번호 필드.** 쿠킹 패키지의 `minLod`·`numStreamedLods`의 `defaultValue`는 대상 프로파일로 푼 값이다(맵은 EditorOnly라 빠진다 — UE `FPerPlatformProperty` 쿠킹과 같다). `minLod`·`lodForCollision`은 원본 LOD 번호를 쿠킹 LOD 번호로 옮긴다(뺀 LOD면 그 다음 쿠킹 LOD, 없으면 마지막) 뒤 [0, 쿠킹 LOD 수 − 1]로 자른다(UE Clamp). `numStreamedLods`(개수, −1 = 그룹 기본)는 그대로다. 되돌리기가 원래 값을 복원한다.
10. **너겟.** bucket은 새 `NuggetBucketMesh`(`"mesh"`)이고 SCC Record(구성원 하나)다. 지역 키는 머리·TypeId·`StaticMeshBuildVersion`(패킹·LOD 입력 규칙 버전 — 텍스처 `TextureBuildVersion`과 같은 역할)·`MeshMikkTSpacePatchVersion`·`MeshBuildThresholdVersion`·색 형식·장치 유무(3번)와 쿠킹 LOD마다 (원본 종류, 위상 SHA-256·크기, 요소 수, 속성 레코드(이름·요소·타입·채널·SHA-256·크기), 유효 빌드 설정(리플렉션 기록), 슬롯 이름 목록, 섹션마다 유효 섹션 정보 여섯 값)이다. 화면 크기·minLod·bounds 확장은 키에 넣지 않는다(너겟 밖 값이다 — 9.3절을 고쳤다). meshoptimizer 어댑터 버전은 어댑터가 생길 때 넣는다. 값은 part 0 = 머리(형식 버전, LOD 수, LOD마다 정점 수·stream 형식과 구간·index 형식·섹션, bounds, 빌드 경고 목록), part i = 쿠킹 LOD i의 `data` byte다(서버 쿠킹이면 part 0만). 경고를 머리에 담는 까닭: 메시 경고(값 정리·슬롯 대체)는 원본에서 나와 적중이면 다시 계산할 수 없다(텍스처는 설정만으로 경고를 정한다). 적중을 받아들이는 조건: 머리를 읽을 수 있고, LOD 수가 이번 쿠킹 LOD 수와 같고, 값 수 = LOD 수 + 1(서버면 1), part i byte 수 = 머리의 LOD i 덩어리 크기, 머리로 만든 `renderData`가 `ValidateStaticMeshRenderData`와 LOD마다 `ValidateStaticMeshIndices`를 통과한다. 아니면 미적중으로 다루고 Record를 치운다(텍스처 12.14절 6번과 같다).
11. **경고·실패.** 빌드 경고는 LOD 순서로 처리기 경고가 된다(`CookDiagnostic` code = `MeshDiagnostic` code). 빌드 실패는 그 진단 코드와 오류다. 원본 로드 실패는 `InvalidSource`다.

12. **구현에서 정한 점(MA2b2).** 모두 권장안이다.
    - **원본 읽기.** 쿠킹 문맥은 패키지가 아니라 bulk 원본(`CookContext::bulkSource`)을 주므로 `MeshSource::Load(data, PackageBulkSource&, names, failure)` 오버로드를 더했다. 판정은 Package 형태와 같다. bulk 원본은 크기·SHA-256을 보지 않으므로 byte 수나 SHA-256이 ref와 다르면 `CorruptPackage`(BulkUnreadable)·`TypeMismatch`다(원본 bulk는 EditorOnly라 쿠킹 패키지 쓰기가 다시 보지 않고, 너겟 키가 ref의 SHA-256으로 만들어지므로 손상된 원본이 정상 키로 Put되지 않게 — 리뷰 반영). 두 형태는 내부 읽기 대상(`MeshSourceBulkReader`)만 다르다.
    - **경고 위치·순서.** 처리기 경고의 `CookDiagnostic::setting`이 쿠킹 LOD 번호다(뺀 LOD의 경고는 다음 쿠킹 LOD, 마지막 LOD를 뺐으면 마지막 쿠킹 LOD). 경고는 LOD 순서로, LOD마다 입력 선택 경고(설정에서 나와 적중에서도 다시 계산) 다음 빌드 경고다. 빌드 경고는 너겟 머리에 (code, LOD)로 담아 적중에서도 같은 순서로 낸다. 머리 읽기는 알려진 진단 코드만 받는다.
    - **섹션 정보의 materialIndex.** 표 항목의 materialIndex가 음수면 슬롯 대응을 쓴다(빌드 입력 `MeshNoIndex`). 0 이상이면 그 값이다(범위 밖이면 기본 머티리얼 — 5.5.1절 9).
    - **머리 배치(`MeshTools/StaticMeshNuggetHeader.h`).** 버전 u32, LOD 수 u32, bounds 7 f64, 경고 수 u32와 (code u32, LOD u32), LOD마다 정점 수 u32, data byte 수 u64, stream 구간(형식 u32·offset u64·byteSize u64) 위치·tangent 기저, UV 수 u32와 각 구간, 색 구간, index 형식 u32, index 구간, 섹션 수 u32와 섹션마다 정수 다섯(u32)·bool 다섯(u8 0·1). 경고 수 상한 65535, 섹션 수 상한 `MeshMaxPolygonGroups`, 남는 byte가 있으면 손상이다.
    - **연쇄 bulk 원본.** 텍스처 처리기의 사본을 `AssetCooker/CookBulkSource.h`의 `CookChainedBulkSource`로 옮겼다(동작 불변 — 텍스처 시험 그대로 통과).
    - **정체성.** 처리기 `StaticMeshCookProcessorId`와 너겟 키 머리 `StaticMeshNuggetProcessorId`를 나눴다(텍스처와 같은 이유 — renderData 채우기만 바뀌면 처리기 버전만 올린다).

#### 5.5.3 MA2b3 — 임포트 서비스

1. **API.** `MeshTools/StaticMeshImport.h`의 `ImportStaticMesh(context, output, spec, failure)`가 임포터(교환 형식 UX3b, 뒤의 glTF·FBX)의 `AssetImporter::Import` 안에서 `Output()`으로 받은 `StaticMesh` 객체를 채운다. 임포터는 파일 형식만 읽고 메모리 `MeshSource`와 값으로 `StaticMeshImportSpec`을 만든다(텍스처 `TextureImportSpec`과 같은 역할 — 10절).
2. **명세.** `StaticMeshImportSpec`은 LOD 목록(원본·baked `MeshSource` 포인터 — null이면 없음, 빌드·축소 설정, 화면 크기 기본값과 프로파일별 값), 슬롯 목록(slotName·importedSlotName·머티리얼과 overlay의 `AssetId` — null이면 비움, `MeshUvDensity`), 섹션 정보 항목, 소켓, 단순 충돌(값 + 볼록 형상마다 `ConvexGeometry`), 그 밖 저장 필드 값(5.1절 — 참조 필드는 `AssetId`)이다.
3. **검증 순서(저장 전에 모두 — 리뷰 반영).** LOD 수 1–`StaticMeshMaxLods` → LOD마다 원본이나 baked 하나 이상 → 원본마다 `ValidateMeshSource` → 섹션 정보 (lod, section) 엄격 증가·0 ≤ lod < LOD 수·section ≥ 0 → 슬롯 수 ≤ `StaticMeshMaxMaterialSlots`(65535 — `MeshMaxPolygonGroups`와 같은 16비트 상한) → 이름(슬롯·소켓·LOD 그룹·프로파일 키 — Name 규칙, 프로파일 키는 비지 않고 같은 표 안에서 Name 비교로 겹치지 않음) → 단순 충돌(볼록 형상 수 = `convexes` 수, `ValidateSimpleCollision`, 볼록 형상마다 `ValidateConvexGeometry`) → 참조(`OpenAsset` 후 타입: 머티리얼 둘은 MaterialBase 파생, `complexCollisionMesh`·`distanceFieldReplacementMesh`는 StaticMesh, 자기 자신은 `OpenAsset`의 InvalidArgument 그대로). 검증이 끝나야 bulk를 넘기고 필드를 쓴다. 섹션 정보의 materialIndex·`lodForCollision`·`minLod`는 범위를 보지 않는다(UE처럼 범위 밖 슬롯은 기본 머티리얼, LOD 번호는 쿠킹이 자른다). 진단(`context.Fail`): 메시 데이터 `InvalidMeshData` 등 `ValidateMeshSource`의 코드(detail = LOD, `meshFailure`에 세부), LOD·섹션·슬롯·이름 `InvalidSetting`(detail = 위치), 충돌·볼록 수 `InvalidCollisionData`(detail = 형상 번호), 참조 타입 `TypeMismatch`(detail = 참조 위치 — 슬롯 i의 material 2i·overlay 2i+1, 그 뒤 complexCollisionMesh·distanceFieldReplacementMesh), 참조 없음·열기 오류는 `OpenAsset`이 남긴 진단.
4. **저장.** 원본은 `MeshSource::Store`로 `MemoryBulkSource`에 쓰고, 나온 ref마다 byte를 `context.AddBulk`로 출력 패키지에 넘겨 필드의 ref를 바꾼다(MemoryBulkSource의 임시 번호를 패키지 번호로). 볼록 형상도 `StoreConvexGeometry`로 같은 길을 쓴다. `renderData`는 비운다(쿠킹 데이터). 새 임포트는 임포트가 정하는 필드를 모두 쓴다.
5. **경고.** 받지 않는 설정(5.5.1절 5의 VirtualGeometry 참조 값 등)은 임포터가 `UnsupportedSetting`·`UnsupportedCollisionShape`로 낸다. 서비스는 값 정리를 하지 않는다(원본은 받은 그대로 저장하고 정리는 빌드가 한다 — 3.3절).
6. **재임포트(리뷰 반영 — UE와 같이 사용자 편집 보존).** `context.IsReimport()`면: 원본·baked 원본·섹션 수에 걸린 데이터는 새 값으로 바꾼다(원본 LOD 목록, 단순 충돌, 섹션 정보는 명세에 항목이 있을 때만 바꾼다). 슬롯은 명세 순서로 다시 만들되 옛 슬롯 중 importedSlotName(Name 비교)이 같은 것이 있으면 그 slotName·머티리얼·overlay·UV 밀도를 유지한다(명세가 머티리얼을 주면 명세가 이긴다 — UE `ImportedMaterialSlotName` 대응). 소켓은 명세 소켓 뒤에 옛 소켓 중 `createdAtImport`가 거짓(편집기에서 만든 것)을 그대로 붙인다(UE `bSocketCreatedAtImport`). LOD별 빌드·축소·화면 크기 설정과 에셋 설정(lightmap·bounds 확장·LOD 그룹·minLod·numStreamedLods·자동 화면 크기·가상 기하·충돌 설정·참조 둘)은 옛 값을 유지한다(새 LOD는 명세 값). 명세 `replaceSettingsOnReimport`가 참이면 설정도 명세 값으로 바꾼다(교환 경로처럼 원본 쪽이 설정의 정본일 때). StreamableRenderAsset 기반 필드(neverStream 등)는 임포트가 정하지 않는다(사용자 설정).
