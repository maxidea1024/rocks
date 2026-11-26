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
  - `collections`: 머티리얼이 참조하는 컬렉션 layout 목록 `{ assetId, 항목(이름, scalar|vector) }`. 항목 순서가 9.7절 layout이다(base 상속은 컬렉션 에셋이 펼쳐 넘긴다 — UE PC:681–732의 base 먼저 배치).
- **IR 값.**
  - `Parameter`(번호 85): 수 파라미터 읽기. 즉시값 [0]·[1]은 ParameterId(high·low), [2]는 종류(Scalar·Vector·DoubleVector), [3]은 기본값 묶음 번호(모듈 표). 빈도 `Uniform`. 같은 ParameterId는 한 값이다.
  - `CollectionValue`(번호 86): 컬렉션 벡터 하나. 즉시값 [0]·[1]은 컬렉션 asset id, [2]는 벡터 번호. Float4, 빈도 `Instance`(World마다 실행 중에 바뀌는 공유 상수 — 머티리얼 상수 식으로 끌어올리지 않는다. UE도 uniform 식이 아니라 코드 조각이다 — HT:5058–5085).
- **상수 식 프로그램(9.4절).** `BuildMaterialConstants(module, out)`가 모듈에서 상수 블록 layout과 프로그램을 만든다(번역 결과에서 유도 — 쿠킹이 같은 함수를 쓴다).
  - **출력.** 빈도가 `Uniform`이고 상수가 아니며 프로그램 연산인 값 가운데, 소비자가 프로그램 밖(빈도가 더 높거나 프로그램 연산이 아님)이거나 진입점 결과인 값이다. 프로그램 연산이 아닌 `Uniform` 값(Pow·Lerp·Smoothstep·Step·Select·비교 — UE도 접지 않는다, HT:9819–10466)은 셰이더가 계산하고 그 피연산자가 출력이 된다.
  - **프로그램 연산**(UE 접기 목록과 같다 — HT:5358–10907): 사칙·Min·Max·Abs·Floor·Ceil·Round·Truncate·Frac·Saturate·Sign·Sqrt·Reciprocal·Length·Normalize·Dot·Cross·Sin·Cos·Tan·ASin·ACos·ATan·ATan2·Exp·Exp2·Log·Log2·Log10·Fmod·Modulo·Negate·Clamp·Splat·Swizzle·Append·Float↔Double 변환. 명령은 후위 스택 기계(상수·파라미터 읽기·연산)이고 형식 버전 1이다. Double 값은 double로, 그 밖은 float32로 실행한다(9.4절 "float32" — LWC 파라미터는 UE도 double 두 칸이다).
  - **layout.** float 계열 출력을 크기 큰 것부터(같은 크기는 값 번호 순) float4 경계를 넘지 않게 앞에서부터 채운다(7.4절 5항 — UE도 비 LWC 값이 레지스터를 넘지 않는다, HT:3744–3792). Double 출력은 같은 크기의 float 칸 둘(tile, offset — 5.3절 LWC 표현)이다. 결과는 `MaterialConstants { 멤버(출력 값 번호, 칸 번호, 성분 시작, 성분 수, 종류) 목록, float4 수, 프로그램 }`다.
  - **생성 원문.** `cbuffer MaterialConstants { float4 materialFloats[N]; };`(N > 0일 때만)를 내고, 출력 값은 `materialFloats[k].<성분>`(Double은 `MakeMaterialDouble<n>(tile 칸, offset 칸)`)로 읽는다. 함수 몸체는 출력 아래를 내지 않는다.
- **파라미터 컬렉션.** 머티리얼당 컬렉션은 2개까지다(9.7절 — 넘으면 V9 "Material references too many MaterialParameterCollections!  A material may only reference 2 different collections." — HT:5071). 생성 원문은 모듈 컬렉션 표(asset id 순)의 k마다 `cbuffer MaterialCollection<k> { float4 Vectors[N]; };`를 낸다.

| 노드 | lowering | UE 근거·비고 |
|---|---|---|
| ScalarParameter | `useCustomPrimitiveData`면 CustomPrimitiveData1(primitiveDataIndex)(UI 영역이면 V6 "Custom Primitive Data can't be used with the UI material domain."), 아니면 Parameter(Scalar, defaultValue) | ME:8425–8440 |
| VectorParameter | CPD면 CustomPrimitiveData4(index), 아니면 Parameter(Vector). RGB·R·G·B·A·RGBA 마스크 | ME:8032–8047 |
| DoubleVectorParameter | Parameter(DoubleVector) Double4. XYZ·X·Y·Z·W | ME:8200–8203 |
| ChannelMaskParameter | Input(필수 — "Missing mask input")을 Float4로 맞춘다(스칼라는 복제, 짧으면 0을 붙이고 길면 앞 넷 — UE `ForceCast`). Dot(input, Parameter(Vector, defaultValue)) | ME:8352–8374. CPD는 UE처럼 무시 |
| StaticBoolParameter | static 값(없으면 defaultValue)의 Bool 상수 | HT:8947–8965. UE의 숨은 `DynamicBranch` 표지(Dynamic Bool)는 Rocks 속성에 없다(9.3절 Dynamic Bool은 MAT2 — 그때 노드·속성을 더한다) |
| StaticSwitchParameter | A·B 둘 다 연결이 필요하다("Missing A input"·"Missing B input"). static 값(없으면 defaultValue)이 고른 가지만 번역한다(늦은 입력) | ME:8644–8697 |
| StaticComponentMaskParameter | Input(필수 — "Missing ComponentMaskParameter input")에 static mask(없으면 defaultR·G·B·A)로 ComponentMask와 같은 성분 선택 | ME:5465–5482, HT:8967–8991 |
| ParameterCollectionValue | 컬렉션이 입력 목록에 없으면 "CollectionParameter has invalid Collection!", 이름이 없으면 "CollectionParameter has invalid parameter <이름>". scalar는 CollectionValue(벡터 i/4)의 성분 i%4, vector는 그 벡터 | ME:17179–17204, HT:5058–5085 |
| ParameterCollectionTransform | UE 검사 순서와 문구 그대로(입력 없음·벡터 입력 아님·컬렉션·파라미터·scalar·벡터 수 부족). 연속 벡터 r0…로 Position = (float4(in, 1) × M).xyz(Float4 입력은 in × M), Vector = (float4(in.xyz, 0) × M3).xyz, Projection = in.xyzz × (r0.x, r1.y, r2.zw) + (0, 0, r3.zw), LocalToWorld = Double(in × M) + Double(r4.xyz) × T, WorldToLocal = (Double(in) − Double(r4.xyz) × T) × M(Float). 행렬 곱은 행 벡터 합으로 IR에 펼친다 | ME:17327–17410, HT:5087–5153 |

- 문맥 값 표에 더한다: CustomPrimitiveData1(Float1, 인자 index)·CustomPrimitiveData4(Float4)(공급 기능 primitive — UE `GetPrimitiveData(Parameters).CustomPrimitiveData`).
- 편차·한계: UE는 같은 이름의 다른 종류 파라미터를 허용하지만 Rocks는 V8이다(9.2절 결정 — 교환 번역기가 이름을 나눈다). UE 컬렉션 참조는 실행 중 파생 컬렉션으로 바꿔 끼울 수 있는 파라미터이고 Rocks는 static 변형이다(9.3절). UE는 preshader 슬롯을 첫 접근 순으로 배정하고 32 float4마다 간격을 두지만 Rocks는 크기 순 결정적 배치다(9.4절).
