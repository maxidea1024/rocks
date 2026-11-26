# 머티리얼 시스템(MaterialSystem) 설계

## 6. IR — MS2c 구현 세부

### 6.10 MS2c 구현 세부(파라미터·텍스처·scene·선행 진단)

MS2c는 네 묶음으로 나눈다. 묶음마다 시험을 함께 넣고, 리뷰는 MS2c 끝에 한 번 받는다.

| 묶음 | 내용 |
|---|---|
| MS2c1 | 늦은 진단: TODO 노드(`PrerequisiteMissing`)와 모르는 노드(V12)를 static 해석·DCE 뒤 살아 있는 것에만 낸다(19.2.1절). 조건 값 가지에만 있으면 경고 |
| MS2c2 | 파라미터: scalar·vector·double vector·channel mask·static bool·static switch·static component mask 파라미터, 파라미터 컬렉션 값·변환, IR 파라미터 값(`Uniform` 빈도), static 값 입력, 상수 식 프로그램(9.4절)·상수 블록 layout, V8 |
| MS2c3 | 텍스처: IR 자원 타입, 텍스처 객체·파라미터, TextureSample과 파라미터 샘플 노드, TextureProperty, AntialiasedTextureMask, RecordTextureStreamingInfo, ParticleSubUv, V9(텍스처 예산)·V10 |
| MS2c4 | scene·깊이·버퍼: SceneTexture·SceneColor·UserSceneTexture·SceneDepth·DepthFade·SceneDepthWithoutWater·DecalBufferTexture·PathTracingBufferTexture(영역·블렌드 모드 검사 V6) |

**MS2c1 늦은 진단.**
- **자리 값.** 번역기는 TODO 노드(정의 상태 `PrerequisiteMissing`)와 카탈로그에 없는 노드를 만나면 바로 진단하지 않는다. 대신 받는 쪽이 요청한 출력 핀마다 IR 자리 값 `Pending`(번호 84)을 낸다. 즉시값 [0]은 종류(0 모르는 노드, 1 선행 없음), [1]은 선행 ID 번호(`MaterialPrerequisite`), [2]·[3]은 노드 NodeId다. NodeId를 키에 넣어 서로 다른 노드의 자리가 hash-consing으로 합쳐지지 않는다(진단 위치가 사라지지 않는다). 타입은 정의 출력 핀의 수 타입이고, `Numeric`·`Any`·모르는 노드는 Float4다.
- **진단.** 모듈을 만든 뒤(static 해석은 늦은 입력으로 이미 끝났고, DCE는 진입점에서 닿는 값만 남긴다) 결과에서 닿는 `Pending`마다 진단한다. 모르는 노드는 V12("Node type is not registered"), TODO 노드는 `PrerequisiteMissing`(108 — "Node is not translated until prerequisite <ID> exists")다.
  - 조건 값(Bool 문맥 값)을 조건으로 한 `Select`의 한 가지로만 닿으면 **경고**다(19.2.1절 "이 조합에서만 오류" — 그 가지를 고르는 (형식, pass) 조합의 거부는 쿠킹 MS3이 한다). 그 밖으로 닿으면 오류이고 번역이 실패한다.
  - 경고만 있으면 모듈이 남는다. 생성 원문은 자리 값을 그 타입의 0으로 낸다(조합 거르기가 그 가지를 쓰는 job을 만들지 않으므로 실제로 그려지지 않는다 — "검은 표면 금지" 규칙은 쿠킹 쪽 거부로 지킨다).
- UE 근거: 쓰지 않는 노드는 컴파일되지 않으므로 UE도 그 노드의 오류를 내지 않는다(classic은 활성 속성에서 닿는 식만 컴파일). Quality Switch 검증 컴파일(`MaterialExpressions.cpp:9088–9102`)의 모든 가지 진단은 에디터 경고 등급이다(14장 E4).
- `NotYetLowered`(이후 단계 노드)는 그대로 즉시 오류다(구현 중 표지 — MS2가 끝나면 남지 않는다).

**MS2c2 파라미터·상수 식.** 근거는 UE 기본 번역기(`MaterialExpressions.cpp`(ME)·`HLSLMaterialTranslator.cpp`(HT)·`MaterialUniformExpressions.cpp`(MUE)·`ParameterCollection.cpp`(PC))다.
- **번역 입력.** `MaterialTranslationInput`에 둘을 더한다(객체 주소 없이 값만 — 6.3절 1항).
  - `staticParameters`: static 값 목록 `{ parameterId, kind(Bool·ComponentMask), value, mask }`. 인스턴스 사슬을 평탄화한 값이다(9.5·9.6절 — 쿠킹 MS3이 만든다). 없는 키는 노드 기본값이다(UE `StaticParameters` 정확 일치, 없으면 기본 — HT:8947–8991).
  - `collections`: 머티리얼이 참조할 수 있는 컬렉션 layout 목록 `{ asset, 항목(parameterId, 벡터 번호, 성분 — vector면 전체), float4 수 }`. 배치는 컬렉션 에셋이 9.7절대로 계산해 넘기고(base 상속도 펼친다 — UE PC:681–732의 base 먼저 배치) 번역기는 다시 계산하지 않는다. 노드는 UE처럼 `parameterId`로 찾는다(UE `GetParameterIndex(ParameterId)` — 이름은 진단 문구에만 쓴다). 그래서 ParameterCollectionTransform 속성에도 UE처럼 `parameterId`를 둔다.
- **IR 값.**
  - `Parameter`(번호 85): 수 파라미터 읽기. 즉시값 [0]·[1]은 ParameterId(high·low), [2]는 종류(Scalar·Vector·DoubleVector), [3]은 기본값 묶음 번호(모듈 표). 빈도 `Uniform`. 같은 ParameterId는 한 값이다.
  - `CollectionValue`(번호 86): 컬렉션 벡터 하나. 즉시값 [0]·[1]은 컬렉션 asset id, [2]는 벡터 번호, [3]은 그 컬렉션의 float4 수(모듈 컬렉션 표가 가져간다). Float4, 빈도 `Instance`(World마다 실행 중에 바뀌는 공유 상수 — 머티리얼 상수 식으로 끌어올리지 않는다. UE도 uniform 식이 아니라 코드 조각이다 — HT:5058–5085).
- **상수 식 프로그램(9.4절).** `BuildMaterialConstants(module, out)`가 모듈에서 상수 블록 layout과 프로그램을 만든다(번역 결과에서 유도 — 쿠킹이 같은 함수를 쓴다).
  - **출력.** 빈도가 `Uniform`이고 프로그램 연산인 값 가운데, 소비자가 프로그램 밖(빈도가 더 높거나 프로그램 연산이 아님)이거나 진입점 결과인 값이다. 빈도 `Constant`인 식(파라미터에 기대지 않는 값 — 예: `sin(0.3)`)은 출력이 아니고 셰이더 원문에 남는다(UE도 상수 uniform 식은 코드 조각으로 넣는다 — HT `AddUniformExpression`의 `IsConstant`). 그런 값이 프로그램 값의 피연산자면 프로그램이 함께 계산한다. 프로그램 연산이 아닌 `Uniform` 값(Pow·Lerp·Smoothstep·Step·Select·비교 — UE도 접지 않는다, HT:9819–10466)은 셰이더가 계산하고 그 피연산자가 출력이 된다.
  - **프로그램 연산**(UE 접기 목록과 같다 — HT:5358–10907): 사칙·Min·Max·Abs·Floor·Ceil·Round·Truncate·Frac·Saturate·Sign·Sqrt·Reciprocal·Length·Normalize·Dot·Cross·Sin·Cos·Tan·ASin·ACos·ATan·ATan2·Exp·Exp2·Log·Log2·Log10·Fmod·Modulo·Negate·Clamp·Splat·Swizzle·Append·Float↔Double 변환. 명령은 출력까지 닿는 값(상수 포함)을 모듈 번호 순으로 늘어놓은 명령열이고, 명령의 피연산자는 앞 명령 번호다(IR 값 형식 그대로 — Parameter는 모듈 파라미터 표 번호를 읽는다). 형식 버전은 `MaterialConstantProgramVersion` 1이다. 스택 기계 대신 이 형식을 고른 까닭: IR이 이미 SSA·위상 순서라 변환이 없고, 공유 부분식(같은 값을 두 소비자가 쓰는 경우)을 다시 계산하지 않으며, 실행기가 명령 번호로 결과를 바로 찾는다. 버린 대안 — UE preshader식 후위 스택(공유 부분식을 다시 넣거나 Assign 명령이 필요하다). Double 값은 double로, 그 밖은 float32로 실행한다(9.4절 "float32" — LWC 파라미터는 UE도 double 두 칸이다).
  - **layout.** float 계열 출력을 크기 큰 것부터(같은 크기는 값 번호 순) float4 경계를 넘지 않게 앞에서부터 채운다(7.4절 5항 — UE도 비 LWC 값이 레지스터를 넘지 않는다, HT:3744–3792). Double 출력은 같은 크기의 float 칸 둘(tile, offset — 5.3절 LWC 표현)이다. 결과는 `MaterialConstants { 멤버(출력 값 번호, 칸 번호, 성분 시작, 성분 수, 종류) 목록, float4 수, 프로그램 }`다.
  - **생성 원문.** `#define MATERIAL_NUM_CONSTANT_VECTORS N`·`MATERIAL_NUM_COLLECTIONS K`를 내고, `cbuffer MaterialConstants { float4 materialFloats[N]; };`(N > 0일 때만)를 낸다. 출력 값은 `materialFloats[k].<성분>`(float4 전체면 성분 없음, Double은 `MakeMaterialDouble<n>(tile 칸, offset 칸)`)로 읽는다. 함수 몸체는 출력 아래를 내지 않고, LWC 지원 검사(6.9절)도 원문에 나오는 값만 본다(출력 아래 Double 연산은 프로그램이 double로 계산한다). register 배정은 셰이더 시스템 바인딩 layout이 한다(이름으로 찾는다).
- **파라미터 컬렉션.** 머티리얼당 컬렉션은 2개까지다(9.7절 — 넘으면 V9 "Material references too many MaterialParameterCollections!  A material may only reference 2 different collections." — HT:5071). 생성 원문은 모듈 컬렉션 표(asset id 순)의 k마다 `cbuffer MaterialCollection<k> { float4 materialCollection<k>Vectors[N]; };`를 내고 값은 `materialCollection<k>Vectors[i]`로 읽는다(HLSL cbuffer 멤버는 전역 이름이라 UE의 `MaterialCollection<k>.Vectors`처럼 겹치는 멤버 이름을 쓸 수 없다). 상한 검사는 번역 중 처음 참조 순으로 하고(UE `AccessCollectionParameter`와 같다) 세 번째 컬렉션을 참조한 노드에 V9를 낸다.
- **진단 코드.** 컬렉션 없음·파라미터 없음·scalar 파라미터·벡터 수 부족은 InvalidProperty(103, UE 문구 그대로), 입력 없음은 MissingInput(101), 벡터 아닌 입력은 V1, custom primitive data의 UI 영역은 V6, 범위 밖 번호(36 이상 — UE는 check)는 InvalidProperty다.

| 노드 | lowering | UE 근거·비고 |
|---|---|---|
| ScalarParameter | `useCustomPrimitiveData`면 CustomPrimitiveData1(primitiveDataIndex)(UI 영역이면 V6 "Custom Primitive Data can't be used with the UI material domain."), 아니면 Parameter(Scalar, defaultValue) | ME:8425–8440 |
| VectorParameter | CPD면 CustomPrimitiveData4(index), 아니면 Parameter(Vector). RGB·R·G·B·A·RGBA 마스크 | ME:8032–8047 |
| DoubleVectorParameter | Parameter(DoubleVector) Double4. XYZ(Double3)·X·Y·Z·W(Double1) — 출력 핀 타입도 Double로 고친다 | ME:8200–8203 |
| ChannelMaskParameter | Input(필수 — "Missing mask input")을 Float4로 맞춘다(스칼라는 복제, 짧으면 0을 붙이고 길면 앞 넷 — UE `ForceCast`). Dot(input, Parameter(Vector, defaultValue)) | ME:8352–8374. CPD는 UE처럼 무시 |
| StaticBoolParameter | static 값(없으면 defaultValue)의 Bool 상수 | HT:8947–8965. UE의 숨은 `DynamicBranch` 표지(Dynamic Bool)는 Rocks 속성에 없다(9.3절 Dynamic Bool은 MAT2 — 그때 노드·속성을 더한다) |
| StaticSwitchParameter | A·B 둘 다 연결이 필요하다("Missing A input"·"Missing B input"). static 값(없으면 defaultValue)이 고른 가지만 번역한다(늦은 입력) | ME:8644–8697 |
| StaticComponentMaskParameter | Input(필수 — "Missing ComponentMaskParameter input")에 static mask(없으면 defaultR·G·B·A)로 ComponentMask와 같은 성분 선택 | ME:5465–5482, HT:8967–8991 |
| ParameterCollectionValue | 컬렉션이 입력 목록에 없으면 "CollectionParameter has invalid Collection!", 이름이 없으면 "CollectionParameter has invalid parameter <이름>". scalar는 CollectionValue(벡터 i/4)의 성분 i%4, vector는 그 벡터 | ME:17179–17204, HT:5058–5085 |
| ParameterCollectionTransform | UE 검사 순서와 문구 그대로(입력 없음·벡터 입력 아님·컬렉션·파라미터·scalar·벡터 수 부족). 연속 벡터 r0…로 Position = (float4(in, 1) × M).xyz(Float4 입력은 in × M), Vector = (float4(in.xyz, 0) × M3).xyz, Projection = in.xyzz × (r0.x, r1.y, r2.zw) + (0, 0, r3.zw), LocalToWorld = Double(in × M) + Double(r4.xyz) × T, WorldToLocal = (Double(in) − Double(r4.xyz) × T) × M(Float). 행렬 곱은 행 벡터 합으로 IR에 펼친다 | ME:17327–17410, HT:5087–5153 |

- 문맥 값 표에 더한다: CustomPrimitiveData1(Float1, 인자 Int index)·CustomPrimitiveData4(Float4 — 연속 float 넷, 36을 넘는 성분은 0)(공급 기능 primitive — UE `GetPrimitiveData(Parameters).CustomPrimitiveData`). 표 영역은 전부이고 UI 영역 검사는 노드가 UE 문구로 한다.
- 편차·한계: UE는 같은 이름의 다른 종류 파라미터를 허용하지만 Rocks는 V8이다(9.2절 결정 — 교환 번역기가 이름을 나눈다). UE 컬렉션 참조는 실행 중 파생 컬렉션으로 바꿔 끼울 수 있는 파라미터이고 Rocks는 static 변형이다(9.3절). UE는 preshader 슬롯을 첫 접근 순으로 배정하고 32 float4마다 간격을 두지만 Rocks는 크기 순 결정적 배치다(9.4절).

**MS2c3 텍스처.** 두 묶음이다. MS2c3a는 기반(자원 타입·텍스처 사실 입력·텍스처 값·샘플·검사·생성 원문)과 텍스처 객체·샘플 노드, MS2c3b는 나머지 노드(TextureProperty·AntialiasedTextureMask·RecordTextureStreamingInfo·ParticleSubUv·TextureSampleParameterSubUv)다. 근거는 UE 기본 번역기 `UMaterialExpressionTextureSample::Compile`(ME:2770–2950)·`MaterialExpressionUtils`(MEU)·HT `TextureSample`·`Common.ush`(CU)다.
- **IR 자원 타입.** `IrScalar::Texture`(5)를 더하고 `IrType`에 텍스처 종류 칸 `texture`(`IrTextureKind` — Texture2D·TextureCube·Texture2DArray·TextureCubeArray·Texture3D·External)를 둔다. 텍스처 타입은 성분 1·행 1이고 산술·비교·선택·Coerce 어디에도 들어가지 못한다(V1). 값으로 흐르는 곳은 번역 때 고르는 스위치·재경유뿐이다(UE도 static 스위치로 텍스처를 고른다). 실행 중 Select의 텍스처는 V1이다. `IrHashFormatVersion`을 3으로 올린다.
- **번역 입력 `textures`.** 머티리얼이 참조하는 텍스처 에셋의 사실 목록 `{ asset, 종류(IrTextureKind), compressionSettings, srgb, lodGroup, virtualTextureStreaming, addressX·Y·Z }`이다(값만 — 쿠킹·에디터가 텍스처 에셋에서 채운다, MAS3 조회 API). V10 검사와 "참조 해석"에만 쓰고 IR·키에 넣지 않는다(5장 V10 규칙). 목록에 없는 asset을 참조하면 "Could not resolve referenced texture '<asset>'."(InvalidProperty)다.
- **IR 값.**
  - `Texture`(87): 텍스처 하나. 즉시값 [0]·[1]은 asset id(참조) 또는 ParameterId(파라미터), [2]는 `출처(0 참조, 1 파라미터) | 샘플 종류 << 8`, [3]은 모듈 텍스처 표 번호(빌더 표 → 모듈이 (출처, id) 순으로 다시 매긴다). 빈도 `Uniform`. 샘플 종류를 값에 두는 까닭: TextureObject 입력으로 받은 샘플은 그 객체 노드의 샘플 종류를 쓴다(UE `GetTextureForExpression`).
  - 텍스처 표 항목 `IrTexture { 출처, id, 종류, 기본 asset(파라미터), 자기 샘플러 사용 }`. 같은 (출처, id)는 한 slot이다.
  - `TextureSample`(88): 피연산자 [텍스처, uv, mip 값 또는 ddx, ddy]. 즉시값 [0] 샘플 종류, [1] mip 방식(None·Level·Bias·Grad), [2] 샘플러 출처(자기·공유 Wrap·공유 Clamp), [3] `gather 채널 | decal << 4`. view mip bias는 lowering이 mip 값에 이미 반영한다(방식도 Bias로 바뀐다). 결과 Float4(샘플 종류 처리 뒤 값). 빈도는 피연산자 최대이고 Implicit·Bias(gather 포함)는 픽셀이다(하드웨어 미분 — 정점 문맥은 아래처럼 Level로 바뀐다).
  - `AddressDemote`(89): 아래 "Double uv".
- **TextureSample 순서(UE ME:2770–2950).** (1) `AutomaticViewMipBiasValue` 연결이면 그 static Bool(아니면 속성), (2) TextureObject 연결이면 그 값, 아니면 texture 속성(null이면 "Missing input texture"), (3) 값이 텍스처 타입이 아니면 "Tex input requires a texture value", (4) V10 — 샘플 종류 검사(아래), (5) Cube·Volume·2DArray·CubeArray는 Coordinates 연결 필수("UVW input required for cubemap sample" 등 UE 문구 넷), (6) Coordinates(없으면 TexCoord(constCoordinate)), (7) mip 값.
  - **mip(UE ME `CompileMipValue0·1`, HT:7026–7110).** Derivative 밖은 `MipValue` 입력, 없으면 Constant(constMipValue — 기본 −1)다. Derivative는 `CoordinatesDX`·`CoordinatesDY`가 필수다("Missing DDX(UVs) parameter"·"Missing DDY(UVs) parameter", 수가 아니면 "Invalid DDX(UVs) parameter"·"Invalid DDY(UVs) parameter", mip 값이 수가 아니면 "Invalid mip map parameter"). MipBias를 정점 문맥에서 쓰면 "MipBias is only supported in the pixel shader"다. 정점 문맥(픽셀 아님)은 UE처럼 None을 MipLevel로 바꾸고(mip 값은 위 규칙대로 constMipValue 상수) view mip bias를 끈다. 편차: UE는 Derivative도 MipLevel로 바꾸고 ddx 값을 mip으로 쓴다. 명시 미분 샘플(SampleGrad)은 모든 단계에서 되므로 Rocks는 Gradient를 그대로 둔다.
  - **uv 폭.** CubeArray Float4, Cube·2DArray·Volume Float3, 그 밖 Float2로 맞춘다(UE `ForceCast`).
  - **Double uv(UE HT:7290–7330 `WSApplyAddressMode`).** IR `AddressDemote`(89 — 피연산자 Double 값, 즉시값 [0]은 성분마다 2 bit 주소 모드)로 Float로 낮춘다. Wrap은 `frac(tile × tile 크기) + offset`(텍스처 장치가 마지막 wrap을 한다 — 미분이 tile 경계 밖에서 맞다), Clamp·Mirror는 그냥 낮춘다(UE `LWCApplyAddressModeMirror`·`Clamp`와 같다). 주소 모드는 샘플러 출처가 공유 Wrap이면 Wrap, 공유 Clamp면 Clamp, 자기면 텍스처 사실의 addressX·Y·Z다(UE `GetTextureAddressForSamplerSource`). UE의 명시 LWC 미분(`bExplicitLWCDerivatives`)은 UE도 꺼 두었으므로 하지 않는다.
  - **gather.** mip 방식 None만("Gather does not support mip map overrides (implicitly accesses a specific mip)."), 2D·Cube·2DArray·CubeArray만("Gather only supports 2D and Cube texture formats." — UE 문구 그대로, 검사는 배열도 받는다). gather는 view mip bias를 끈다.
  - **view mip bias.** 켜짐 조건: 속성·입력이 참, 픽셀 문맥, gather 아님, 영역이 Surface·DeferredDecal, 형식이 모바일이 아님(UE FL < SM5 제외 — 번역 대상 isMobile을 읽는다), 텍스처가 2D. 그러면 None → Bias(문맥 값 `MaterialTextureMipBias`), Level·Bias → 값 + `MaterialTextureMipBias`, Grad → ddx·ddy × `MaterialTextureDerivativeMultiply`(UE View 값 — 문맥 값 표에 더한다, 기능 View). 편차: UE는 공유 Wrap·Clamp 출처면 bias가 구워진 View 샘플러(`MaterialTextureBilinearWrapedSampler`)를 쓰고 수동 bias를 하지 않는다. Rocks 공유 샘플러는 pass 독립 예약 이름이라 bias를 굽지 않으므로 출처와 관계없이 수동 bias다(결과 mip은 같다).
  - **decal.** DeferredDecal 영역 픽셀 문맥의 None 샘플은 decal 표지를 단다(생성 원문 `MaterialSample*_Decal` — UE `Texture2DSample_Decal`: GLES는 Level 0, 그 밖은 보통 샘플).
- **V10 샘플 종류 검사(UE MEU `GetSamplerTypeForTexture`·`VerifySamplerType`).** 기대 종류: lodGroup 8BitData·16BitData면 Data, Normalmap → Normal, Grayscale → sRGB면 Grayscale 아니면 LinearGrayscale, Alpha → Alpha, Masks → Masks, DistanceFieldFont → DistanceFieldFont, 그 밖은 sRGB면 Color 아니면 LinearColor(가상 텍스처면 Virtual 변형 — 프로필이 가상 텍스처를 끄면 Virtual 샘플 종류를 비가상으로 본다). 다르면 "Sampler type is <샘플>, should be <기대> for <asset>", Normal·Masks인데 sRGB면 "To use '<샘플>' as sampler type, SRGB must be disabled for <asset>"(SamplerMismatch). 파라미터는 기본 asset으로 검사한다(인스턴스 값은 쿠킹이 같은 함수로 검사 — 9.6절). 표시 이름은 UE enum 표시 이름(Color·Grayscale·Alpha·Normal·Masks·Distance Field Font·Linear Color·Linear Grayscale·Data·External·Virtual Color…)이다.
- **V9 샘플러 예산.** 자기 샘플러를 쓰는 텍스처 slot 수가 번역 대상 `maxSamplers`를 넘으면 넘긴 샘플 노드에 V9 "Material uses too many samplers (<n>, limit <m>)"다(7.4절 4항 — 렌더러 예약 수 차감은 Renderer RS0 뒤 MD4대로, 지금은 형식 상한만). UE 번역기는 세지 않고 하위 컴파일러가 실패한다.
- **생성 원문.** 모듈 텍스처 표의 k마다 `<HLSL 종류> MaterialTexture<k>;`와, 자기 샘플러를 쓰는 slot만 `SamplerState MaterialTexture<k>Sampler;`(7.4절 9항). 공유 출처는 `SharedWrapSampler`·`SharedClampSampler`. 텍스처 값은 지역 변수로 내지 않고 피연산자 자리에 `MaterialTexture<k>`를 바로 쓴다. 샘플은 `MaterialProcess<종류>Lookup(MaterialSample<2D|Cube|2DArray|CubeArray|3D>[Level|Bias|Grad|_Decal|GatherRed…](텍스처, 샘플러, uv[, mip…]))`이고 함수는 `MaterialTexture.rsh`(새 셰이더 파일 — UE `Common.ush` 샘플 함수·`MaterialTemplate.ush` Process*Lookup 대응)에 둔다. `#define MATERIAL_NUM_TEXTURES N`.
  - 샘플 종류 처리(UE): Color·Masks·Data는 그대로, LinearColor 그대로, Alpha·DistanceFieldFont `.rrrr`, Grayscale·LinearGrayscale `.rrrr`(Grayscale은 `MATERIAL_MANUAL_GRAYSCALE_SRGB`면 r·r — UE GLES·Vulkan 수동 sRGB, 형식 정의가 켠다), Normal은 `MaterialUnpackNormalMap`(xy = `.rg`·2 − 1, z = sqrt(saturate(1 − dot(xy, xy))), w = 1). 노멀맵 채널은 `.rg`로 고정한다(UE 기본 경로 — DXT5n·LA 노멀은 Rocks 쿠킹 형식에 두지 않는다, MAS3에 "노멀맵은 XY를 RG 두 채널에" 요구로 보낸다).
- **노드(MS2c3a).**

| 노드 | lowering | UE 근거·비고 |
|---|---|---|
| TextureObject | texture 속성(null이면 "Requires valid texture")의 Texture(참조, samplerType). 출력 타입은 속성 기대 클래스 | ME TextureObject. V10은 샘플이 한다(UE도 객체 노드는 검사하지 않는다) |
| TextureObjectParameter | parameterId의 Texture(파라미터, 기본 asset = texture 속성 — null이면 "Requires valid texture") | ME TextureObjectParameter |
| TextureSample | 위 순서 | ME:2770–2950 |
| TextureSampleParameter2D·Cube·2DArray·CubeArray·Volume | 파라미터 Texture로 TextureSample과 같은 샘플. 기본 asset 종류가 노드 종류와 다르면 "Found <종류>, requires <노드 종류>"(null이면 "Found NULL, requires …"), Cube·2DArray·CubeArray·Volume은 Coordinates 필수(스키마 Required — UE "Cube sample needs UV input" 등 문구). gatherMode·AutomaticViewMipBias 입력은 UE처럼 쓰지 않는다(속성 automaticViewMipBias만) | ME TextureSampleParameter* |

- **편차·한계.** 외부 텍스처(External 샘플 종류)는 TODO(MPQ-ExternalTexture — 그 샘플은 PrerequisiteMissing 늦은 진단). 가상 텍스처 샘플 종류는 TODO(MPQ-RuntimeVirtualTexture). 스트리밍 기록(MaterialStoreTexSample)은 UE도 보통 no-op이라 내지 않는다. 텍스처 슬롯 순서는 UE 첫 접근 순이 아니라 (출처, id) 순 결정적 배치다.
