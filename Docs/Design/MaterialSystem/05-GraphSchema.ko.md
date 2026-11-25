# 머티리얼 시스템(MaterialSystem) 설계

## 5. 그래프 스키마

### 5.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 노드 | `UMaterialExpression` 파생 UCLASS 하나가 노드 하나. 기본 번역기는 노드의 `Compile`이 코드 조각을 낸다. IR 번역기는 `Build(MIR::FEmitter&)`를 쓴다(구현 237개) | `RE/Public/Materials/MaterialExpression.h:284`, `RE/Private/Materials/MaterialExpressionsToMIR.cpp:330–333` |
| 출력 속성 | `EMaterialProperty`: Emissive·Opacity·OpacityMask·BaseColor·Metallic·Specular·Roughness·Anisotropy·Normal·Tangent·WPO·Subsurface·AO·Refraction·CustomizedUVs0–7·PixelDepthOffset·ShadingModel·FrontMaterial(Substrate)·SurfaceThickness·Displacement·MaterialAttributes·CustomOutput. WorldDisplacement·TessellationMultiplier는 `_DEPRECATED` | `RE/Public/SceneTypes.h:149–190` |
| 함수 입력 타입 | Scalar부터 Substrate까지 13종 | `RE/Public/Materials/MaterialExpressionFunctionInput.h:18–35` |
| attribute 집합 | `FMaterialAttributeDefinitionMap`이 속성 Guid·기본값·타입·빈도를 정의하고 custom attribute를 등록받는다 | `RE/Public/Materials/MaterialAttributeDefinitionMap.h:83–160` |
| static 분기 노드 | Quality·ShadingPath·FeatureLevel·ShaderStage Switch | `RE/Public/Materials/MaterialExpressionQualitySwitch.h:14`, `…ShadingPathSwitch.h:17, 28`, `…FeatureLevelSwitch.h:17`, `…ShaderStageSwitch.h:14` |
| 임의 HLSL 노드 | `Code`·`Inputs`·`AdditionalOutputs`·`AdditionalDefines`·`IncludeFilePaths` | `RE/Public/Materials/MaterialExpressionCustom.h:74–104` |
| IR 전용 노드 | `meta=(NewMaterialTranslator)` 노드는 기본 번역기에서 "Unsupported expression" | `RE/Private/Materials/MaterialExpressions.cpp:855–861` |
| exec wire | 5.8 deprecated | `RE/Public/Materials/Material.h:1068–1070` |

### 5.2 노드·핀·간선

형식은 [노드 그래프](../NodeGraph/06-DataModel.ko.md) 6절(문서·노드·간선·정규 순서·불변식 GI1–GI10)을 따른다. 이 절은 머티리얼 쪽 규칙만 적는다.

- **그래프** = NodeGraph `GraphDocument`(도메인 `Material`). 출력 노드(`Material.Output`)는 하나다. schema의 `CreateDefaultNodes`가 만들고 문서당 하나 capability로 지킨다. 함수 그래프는 입력·출력 노드를 여럿 가진다.
- **노드** = `{ NodeId, NodeTypeId, schemaVersion, properties(PropertyRecord) }`. 속성은 리플렉션 Record 바이트다(노드 그래프 GD23). 핀은 인스턴스로 저장하지 않는다. 저장하는 것은 입력 기본값 override와 고아 핀뿐이다(노드 그래프 GD3). 핀은 노드 타입 descriptor가 정한다. 개수가 바뀌는 핀(임의 HLSL 노드의 입력, 함수 호출 노드의 입출력)은 노드 속성에서 핀 목록을 만드는 descriptor 함수가 정한다.
- **핀** = `{ PinKey, 방향, 타입 제약, 기본값(입력 핀), 단계 제약, 필수 여부 }`. `PinKey`는 노드 타입 descriptor가 선언한 안정 키다(계획 14.6절 PinId). 표시 이름·배열 순번과 독립이라 핀 이름을 바꿔도 키는 그대로다. 핀을 없애거나 합치면 노드 migration이 옛 키를 새 키로 옮긴다.
- **간선** = `(출력 NodeId, 출력 PinKey) → (입력 NodeId, 입력 PinKey)`. 입력 핀은 간선 하나, 출력 핀은 여럿을 가진다.
- **재경유(reroute)**는 의미 비교 때 접히는 공통 노드 `Reroute`다(노드 그래프 6.1절). IR을 만들 때와 semantic hash를 계산할 때 재경유를 빼고 간선을 이어 붙인다. 그래서 재경유를 더하거나 옮겨도 compile이 일어나지 않는다. **주석**은 노드가 아니라 배치 데이터(`GraphDocumentLayout`의 주석 상자)다.
- 노드 타입 descriptor(계획 14.6절 `MaterialNodeDescriptor`)는 NodeGraph `NodeTypeDescriptor`에 머티리얼 확장(단계·capability)을 더한 것이다. MaterialSchema에 데이터로 두고, lowering 함수(노드 → IR)는 MaterialCompiler에 둔다. plugin은 둘을 같은 module lease로 등록한다(계획 14.6절 `RegisterMaterialNode`).

### 5.3 타입 체계

| 타입 | 값 | 비고 |
|---|---|---|
| `Float1`–`Float4` | 32-bit float 벡터 | half는 없다(셰이더 시스템 7.2절 5항이 막는다) |
| `Int1`–`Int4`, `UInt1`–`UInt4` | 32-bit 정수 벡터 | 상수 블록에서는 float와 다른 16 byte slot에 둔다(7.2절 5항) |
| `Bool` | 실행 중 bool | 상수 블록에는 uint 비트로 담는다(UE도 32비트 단위 비트 패킹, `RE/Private/Materials/HLSLMaterialTranslator.cpp:3792–3822`) |
| `Float3x3`, `Float4x4` | 행렬 | 상수 블록에는 `Float4x4`만 둔다(7.2절 5항). `Float3x3`는 IR 안에서만 쓴다 |
| `Double1`–`Double4` | 큰 좌표 값(LWC, 셰이더 안에서는 타일 + 오프셋 `Float` 벡터 둘) | 절대 world 위치(`Double3`, cm)·`ConstantDouble`(`Double1`)·`DoubleVectorParameter`(`Double4`). UE `MCT_LWCScalar`–`MCT_LWCVector4`와 같다(`HLSLMaterialTranslator.cpp:511, 2539`). uniform 값은 double의 high·low 두 slot이다(`MaterialUniformExpressions.cpp:1344–1365`). Float로의 변환은 명시 노드(`TruncateWorldPosition`)나 translated world 변환으로만 한다. 셰이더 안 기본 위치는 카메라 상대 world(translated world)이고, 절대 위치가 필요한 노드만 이 타입을 쓴다. 표현은 Renderer 스펙의 view 상수(16절 MRD6)를 따른다. UE는 LWC 타입을 쓰고 기본으로 켠다(`RE/Private/Materials/HLSLMaterialTranslator.cpp:150–178` `r.MaterialEditor.LWCEnabled`) |
| `Texture2D`·`Texture2DArray`·`TextureCube`·`TextureCubeArray`·`Texture3D` | 텍스처 자원 | `TextureAny`는 이 다섯을 받는 입력 전용 타입(UE `MCT_Texture`) |
| `TextureExternal`·`TextureVirtual`·`SparseVolumeTexture`·`TextureCollection`·`TextureMeshPaint`·`TextureMaterialCache` | 자원 종류가 아직 없는 텍스처 | 선언만 한다(19.4.1절 `MPQ-ExternalTexture`·`MPQ-VirtualTexture`·`MPQ-SparseVolumeTexture`·`MPQ-TextureCollection`). UE `MaterialValueType.h:38–40, 67–69`. 같은 타입끼리만 잇는다 |
| `StaticBool`·`StaticInt`·`StaticEnum` | 번역 때 정해지는 값 | static switch·component mask·preset 선택 |
| `Surface` | BSDF 트리(10절) | 출력 노드의 표면 입력 |
| `MaterialAttributes` | 속성 집합(표면 입력 + 등록된 custom attribute) | 레이어·혼합 함수의 입출력 |

**암묵 변환.**
- scalar → 벡터 복제는 허용한다.
- 벡터 → 짧은 벡터(앞 성분 사용)는 허용하고 그래프 진단에 정보 수준으로 남긴다. 텍스처 RGBA → 색 RGB 같은 연결이 흔하기 때문이다.
- 짧은 벡터 → 긴 벡터는 오류다. 명시 Append 노드를 쓴다.
- 정수 → float는 허용하고, float → 정수는 오류다(명시 변환 노드).
- `StaticBool` → `Bool`은 허용하고, 반대는 오류다.

### 5.4 검증 규칙

검증은 MaterialCompiler가 한다. 진단은 `(AssetId, NodeId, PinKey, 코드, 메시지)`다(계획 MT02 "원인 node/pin 위치 일치"). 화면은 이 진단을 그대로 표시한다.

| ID | 규칙 | 시점 |
|---|---|---|
| V1 | 핀 타입 제약과 암묵 변환(5.3절). 연결 때는 타입 군만 보고(수 군·같은 자원 타입·TextureAny), 폭·변환은 번역이 본다(UE `CanConnectMaterialValueTypes`와 같다) | 연결(군)·번역(폭) |
| V2 | 데이터 흐름 순환 금지(재경유 포함) | 연결·번역 |
| V3 | 함수 호출 순환·인스턴스 부모 사슬 순환 금지 | 번역(Build 의존 SCC) |
| V4 | 단계 규칙. 픽셀 전용 노드(화면 좌표·미분·scene texture)를 정점 출력에 쓰면 오류다. 정점에서 암묵 mip 텍스처 샘플도 오류다(명시 mip 필요) | 번역(IR 분석, 6.4절) |
| V5 | 영역·블렌드 모드가 허용하지 않는 출력 핀 연결(예: Opaque의 Opacity) | 번역. 연결은 경고로 남기고 IR에서 버린다 |
| V6 | 렌더 경로·pass·형식이 제공하지 않는 입력·기능 사용(예: Forward+에서 GBuffer 읽기, 불투명에서 scene color 읽기, GLES Deferred에서 GBufferD가 필요한 shading model) | 번역(capability 검사, 계획 14.7절, Renderer 14.2·14.6절) |
| V7 | static 입력에 비static 값 연결 | 번역 |
| V8 | 파라미터 이름 충돌(같은 scope에서 같은 이름·다른 종류) | 번역 |
| V9 | 텍스처·샘플러 수, 보간 벡터 수, 상수 블록 크기, 컬렉션 수의 형식·pass 예산 초과(7.4절 4항·13절, (pass, 퍼뮤테이션, 형식)별 텍스처 예산은 Renderer 7.4절) | 번역(프로필 형식마다 진단)·job(형식별 상한, 넘는 형식만 쿠킹 오류) |
| V10 | 텍스처 샘플 종류와 텍스처 설정 불일치(예: sRGB 텍스처를 `Normal`로 샘플) | 번역. 텍스처 메타데이터는 진단용으로만 읽고 키에 넣지 않는다 |
| V11 | NaN·Inf 상수 금지 | 연결·번역 |
| V12 | 등록되지 않은 노드 타입이 출력에 닿음 | 번역(에디터는 열 수 있고, 쿠킹은 실패) |
| V13 | 임의 HLSL 노드 규칙(7.5절) | 번역·job |
| V14 | 레이어 스택: 레이어 함수는 `MaterialAttributes` 출력 하나, 혼합 함수는 두 입력과 알파, 레이어 수 상한(MAT3 착수 때 정함) | 번역 |
| V15 | Render Path Switch가 `WorldPositionOffset`·`OpacityMask`·`PixelDepthOffset`에 닿으면 오류다. 공통 depth pass(Default 입력)와 경로 전용 base pass(경로 입력)의 깊이가 달라져 base pass 깊이 `Equal`이 깨지기 때문이다(Renderer 14.3절 "깊이 일치") | 번역(IR 분석) |

오류가 있는 그래프도 draft로 저장·재열기할 수 있다(계획 14.8절). 쿠킹은 오류가 하나라도 있으면 게시하지 않는다.

### 5.5 안정 id와 semantic hash

- `NodeId`: 노드를 만들 때 정하는 Guid다(NodeGraph `GraphIdSource`, 노드 그래프 8.5절). 복사·붙여넣기는 새 Guid를 받는다. 저장 순서와 무관하다.
- `ParameterId`: 파라미터 노드를 만들 때 정하는 Guid다. 이름을 바꿔도 유지된다. 에셋 복제는 새 Guid를 받는다.
- **semantic hash**: [노드 그래프](../NodeGraph/08-ChangesUndo.ko.md) 8.4절 의미 바이트를 SHA-256한 값이다. 머티리얼 schema의 `NormalizeSemanticNode`가 아래 규칙을 정한다.
  - 재경유를 먼저 접는다.
  - 노드를 `NodeId`로 정렬하고, 노드마다 (`NodeTypeId`, schemaVersion, 속성 Record 바이트, 입력 기본값 override, 활성 상태, 고아 핀)을 쓴다. 속성 Record 바이트는 리플렉션 binary codec 출력이다(기본값 포함, float는 bit 패턴).
  - 간선은 노드 그래프 6.3절 정규 순서(입력 NodeId, 입력 PinKey, 출력 NodeId, 출력 PinKey)로 쓴다.
  - 출력에 닿지 않는 노드도 넣는다. 닿지 않는 노드를 고쳐도 키가 바뀌지만 번역 결과는 같으므로, 셰이더 집합 id(8.6절)가 같아 셰이더는 다시 쌓이지 않는다.
  - 파라미터 노드의 기본값(scalar·vector·텍스처·dynamic bool)은 `NormalizeSemanticNode`가 걸러 넣지 않는다(4.5절). static 파라미터 기본값은 넣는다.
  - 배치 데이터(`GraphDocumentLayout`: 위치·크기·접힘·주석 상자·색)와 선택은 넣지 않는다. 그래서 노드를 옮겨도 compile이 일어나지 않는다(계획 14.6·14.8절, MT04).
  - hash는 migration을 거친 그래프로 계산한다. 쿠커는 저장 바이트를 읽은 뒤 같은 migration을 메모리에서 적용하고 계산하므로, 에디터에서 연 그래프와 쿠커의 번역 너겟 키가 같다(노드 그래프 8.4절).
- semantic hash는 에디터 상태 판정(현재 그래프와 마지막 성공 결과의 일치, 계획 14.8절)과 번역 너겟 키(12.2절)에 쓴다. 편집 충돌 검사는 문서 revision 하나로 하고, compile 필요 여부는 semantic hash로 판정한다(노드 그래프 GD8).

### 5.6 출력 노드(영역별 typed output)

| 영역 | 출력 핀 | 단계 | 비고 |
|---|---|---|---|
| Surface | `Surface`(BSDF), `Opacity`, `OpacityMask`, `AmbientOcclusion`, `PixelDepthOffset`, `WorldPositionOffset`, `CustomizedUv0`–`7`, `Refraction`, `SurfaceThickness`, `Displacement`, `Attributes`(머티리얼 속성 사용 표시일 때), 추가 출력 노드 | 픽셀·정점 | 블렌드 모드에 따라 `Opacity`·`OpacityMask`·`Refraction`이 활성화된다(V5). UE `EMaterialProperty` 대응은 19.8절 |
| DeferredDecal | `Surface`(쓰는 속성 부분집합), `Opacity` | 픽셀 | 쓰는 속성(색·노멀·거칠기·발광)은 Renderer 스펙의 decal 계약이 정한다(16절 MRD12) |
| LightFunction | `LightColor`(Float3) | 픽셀 | |
| PostProcess | `Color`(Float4), scene texture 입력 노드 | 픽셀 | 삽입 위치는 계획 26절의 HDR·tone-map 위치 |
| UserInterface | `Color`(Float3), `Opacity`, `OpacityMask` | 픽셀 | UI 정점 입력 타입(UiToolkit) |
| Volume | `Surface`(`SurfaceVolumetricFogCloud`: `Albedo`·`Extinction`·`Emissive`·`AmbientOcclusion`), 고급 volume 출력 노드 | 픽셀 | 번역은 MS2d. 그리는 pass는 Renderer volumetric fog 단계(공급 `volume`, 19.3절) |

UE의 RuntimeVirtualTexture 영역은 deprecated이며 로드 때 Surface + RVT 출력 노드로 바뀐다(`RE/Public/MaterialDomain.h:26–27`, `RE/Private/Materials/Material.cpp:3259–3333`). Rocks는 이것을 영역으로 두지 않는다. Surface의 추가 출력 노드 `RuntimeVirtualTextureOutput`이다(19.6절, 공급 `rvt`).

번역은 모든 영역을 받는다. 그 영역을 그리는 pass가 아직 없으면 그 조합은 거르기에서 빠지고 쿠킹 보고서에 남는다(계획 14.7절 `Unsupported`, V6·19.3절). M3 기준은 Surface(Opaque·Masked)와 `DefaultLit`·`Unlit` preset이다. 영역 이름은 Renderer 14.2절을 따른다.

### 5.7 노드 묶음

**전체 목록은 [19장](19-NodeCatalog/README.ko.md)이다(개정 10).** UE 5.8 노드 클래스 360개를 전부 대응시키고 노드마다 상태·단계·공급 조건을 정했다(결정 MD11). 아래 표는 개정 9까지의 "1차 범위"(계획 14.5절 "기본 표현식", MAT0–MAT2)다. 이 범위는 19장 단계 MS2a–MS2c·MS2e에 들어 있고, 이름은 19장 표가 정한다.

| 묶음 | 노드 |
|---|---|
| 상수·파라미터 | 상수 1–4성분, Scalar·Vector·Texture 파라미터, Static Bool·Static Component Mask 파라미터, Dynamic Bool 파라미터, 컬렉션 값 |
| 텍스처 | Texture Sample(종류·샘플 종류·mip 방식: 암묵·bias·level·gradient), Texture Size, Texture Coordinate(채널·tiling) |
| 수학 | 사칙·Min·Max·Clamp·Saturate·Lerp·Abs·Floor·Frac·Sqrt·Power·Exp2·Log2·Sin·Cos·Dot·Cross·Normalize·Length·Distance·Append·Component Mask·Swizzle·If(비교 선택) |
| 기하·공간 | World Position·Object Position·Camera Position·Vertex Normal·Vertex Tangent·Vertex Color·Pixel Normal·Transform(공간 사이)·Screen Position·Pixel Depth |
| 시간·뷰 | Time·Delta Time·View Size |
| 분기 | Static Switch·Quality Switch·Render Path Switch·Capability Tier Switch·Stage Switch |
| 표면 | Surface Slab·shading preset 노드·Surface Mix 등(10절, 단계별) |
| 재사용 | Function Call·Function Input·Function Output·Make/Break Attributes·Layer Stack(MAT3) |
| 고급 | Vertex Interpolator(정점 → 픽셀 명시 보간), 임의 HLSL(선택, 7.5절) |
