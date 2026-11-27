# 머티리얼 시스템(MaterialSystem) 설계

## 6. IR — MS2b 구현 세부

### 6.9 MS2b 구현 세부(문맥 값·평가 문맥·스위치·공급 노드)

MS2b는 네 묶음으로 나눈다. 묶음마다 시험을 함께 넣고, 리뷰는 MS2b 끝에 한 번 받는다. 노드별 lowering 표는 묶음마다 이 절 아래에 더한다.

| 묶음 | 내용 |
|---|---|
| MS2b1 | 기반: IR `Extern` 연산, 평가 문맥 셋(정점·이전 정점·픽셀)과 문맥별 노드 결과, 이전 프레임 진입점 번역, 문맥 값 검사(V4 단계·V6 영역), 진입점별 사용 기능·최소 기능 등급 기록, 사용 정의, `MaterialContext.rsh`(표에서 만든 문맥 칸·접근자), 값 하나를 읽는 문맥 노드·`Ddx`·`Ddy`·`StaticBool` |
| MS2b2 | 좌표·벡터·공간 변환과 LWC 표현(Double 값의 원문 — UE tile + offset): `TextureCoordinate`·`WorldPosition`·`ScreenPosition`·`Panner`·`Rotator`·`TransformVector`·`TransformPosition`·경계 노드 등, `MATERIAL_NUM_TEXCOORDS` |
| MS2b3 | 스위치(19.3.4절 해석 시점별): IR `Condition` 연산, 번역 입력의 품질 수준·형식 사실·프로필 기능, `StaticSwitch`·`QualitySwitch`·pass 조건·렌더 경로·기능 등급·실행 중 값 스위치, V7·V15 |
| MS2b4 | 보간기(`VertexInterpolator`·CustomizedUv 출력 핀)와 공급 노드(파티클·데칼·하늘·대기·후처리·distance field·물리장·SpeedTree·Hair·RVT·메시 페인트·글꼴·XR), 표에 없는 문맥 값 추가 |

**평가 문맥.** 번역은 진입점(7.3절)을 셋으로 묶은 평가 문맥마다 따로 한다.

| 문맥 | 진입점 | 근거 |
|---|---|---|
| 정점 | Vertex | UE 속성 컴파일의 `SF_Vertex` |
| 이전 정점 | VertexPrevious | UE는 WPO가 있을 때만 이전 프레임 WPO를 다시 컴파일한다(`bCompilingPreviousFrame` — `RE/Private/Materials/HLSLMaterialTranslator.cpp:1663–1667`). Rocks도 Vertex `WorldPositionOffset` 결과가 있을 때만 이 문맥을 번역한다 |
| 픽셀 | Surface·Opacity·OpacityMask·PixelDepthOffset·CustomOutput·Preview | UE `SF_Pixel` |

- 노드 결과 기억은 (문맥, 노드, 출력 핀)마다다. UE 기본 번역기도 셰이더 단계마다 함수 상태 스택을 따로 두고(`FunctionStacks[SF_NumFrequencies]` — `HLSLMaterialTranslator.h:309`), 노드 결과 키에 이전 프레임 여부가 들어간다(`FMaterialExpressionKey::bCompilingPreviousFrameKey` — `RE/Public/MaterialShared.h:2054–2079`).
- 문맥 순서는 정점 → 이전 정점 → 픽셀이다(진입점 순서). 문맥마다 그 문맥 진입점의 활성 출력 핀에서 번역 순서(6.8절)를 따로 만든다. 순환(V2)도 문맥마다 찾지만 같은 진단은 한 번만 남는다(아래).
- **같은 진단은 한 번.** (코드, 노드, 핀, 문구)가 같은 진단은 한 번만 기록한다. 한 노드가 여러 문맥에서 번역되어도 오류는 하나다. UE도 같은 식의 같은 오류 문구는 다시 넣지 않는다(`HLSLMaterialTranslator.cpp:4446–4455`).
- IR 값은 문맥 사이에 공유한다. 같은 계산은 한 값이고, 생성 원문은 진입점 함수마다 그 함수에서 닿는 값을 다시 낸다(7.7절). 현재·이전 값이 섞이지 않는 것은 `Extern`의 변형 즉시값이 다르기 때문이다(6.2절 "진입점 문맥" — hash-consing 키는 연산·즉시값으로 충분하다).

**`Extern` 연산(문맥 값).**
- `IrOp::Extern`(번호 82 — 끝에 더한다). 즉시값 [0]은 문맥 값 표(19.3.2절) 번호, [1]은 변형(0 현재, 1 이전 프레임)이다. 피연산자는 함수형 값의 인자(표의 `argumentCount`, 최대 3)다. 결과 타입은 표의 타입이다.
- 표 번호는 IR hash·번역 키에 들어가므로 표는 끝에만 더하고 순서를 바꾸지 않는다(문맥 공급 기능 비트와 같은 규칙).
- 이전 변형: 이전 정점 문맥에서 표의 `hasPrevious`가 참인 값은 변형 1이다. 아닌 값은 현재 값 그대로다(UE도 DeltaTime 등은 바꾸지 않는다 — 19.3.5절).
- 빈도(6.2절): 정점 기원 값(공급 기능 vertex·skin·particle·speedtree의 정점 단계 허용 값)은 `Vertex`, 픽셀 단계 전용 값은 `Pixel`, 그 밖(view·primitive·sky·post 등 그리기·view 단위 값)은 `Instance`다. 함수형 값은 인자 빈도와의 최대다. `Instance`는 `Uniform`보다 높으므로 상수 식 프로그램(9.4절)으로 끌어올리지 않는다 — 머티리얼 상수 블록에는 view·그리기 값이 없다.
- 상수 접기·대수 단순화를 하지 않는다(값을 모른다).

**문맥 값 검사.** 진입점 결과에서 닿는 `Extern`마다(DCE 뒤 — UE도 활성 속성에서 닿는 식만 컴파일한다):
- **단계(V4).** 정점·이전 정점 진입점이 픽셀 전용 값(표 `stages`에 정점 없음)을, 픽셀 진입점이 정점 전용 값(`PreSkinned*` 등)을 쓰면 오류다. 문구 `"'<이름>' is not available in the <vertex|pixel> stage"`. 미분 연산(`Ddx`·`Ddy`)도 같은 검사다(6.8절).
- **영역(V6).** 표 `domains`에 기본 속성의 영역이 없으면 오류다. 문구 `"'<이름>' is not available in the <영역> material domain"`. UE도 값마다 허용 영역 밖이면 오류다(외부 코드 선언 `Domains`, `CFG/BaseMaterialExpressions.ini:1038–`).
- **기능 등급.** 번역은 형식을 모르므로(결정 MD4 — 원문 하나) 진입점에서 닿는 값의 `minimumTier` 최대를 모듈에 기록한다. 형식 등급이 그보다 낮은 형식의 job은 거르기에서 V6로 빠진다(MS3 — 8.3절 거르기). UE는 플랫폼별 번역 때 오류다.
- **공급.** 번역은 pass를 모른다. 공급하지 않은 기능의 값은 UE 기본값이다(결정 MD15, 19.3.1절) — 아래 접근자가 낸다.

**모듈 기록.** `IrModule`이 `BuildIrModule` 때 진입점마다 닿는 `Extern`의 공급 기능 비트(`EntryFeatures(entry)`)와 `minimumTier` 최대(`RequiredTier()`)를 계산해 둔다. 값에서 유도되므로 IR hash에 따로 넣지 않는다. 쿠킹(MS3)이 셰이더 특성(`MaterialShaderTraits`)에 옮긴다.

**생성 원문(7.7절에 더함).**
- 사용 정의: 진입점 e가 기능 f를 쓰면 `#define MATERIAL_<E>_USES_<F> 1`을 낸다. E는 `VERTEX`·`VERTEX_PREVIOUS`·`SURFACE`·`OPACITY`·`OPACITY_MASK`·`PIXEL_DEPTH_OFFSET`·`CUSTOM_OUTPUT`·`PREVIEW`, F는 `MaterialContextFeatureDefineName`이다. 쓰지 않는 쌍은 정의하지 않는다(전처리 `#if`에서 정의 없는 이름은 0이다 — 8 × 24 = 192줄을 늘 내지 않는다. 19.3.1절 "0|1 고정 정의"를 이렇게 정한다).
- `Extern` 철자: `MaterialContext<이름>(c)`, 함수형은 `MaterialContext<이름>(c, a0, …)`, 이전 변형은 `MaterialContextPrevious<이름>(c)`다. `c`는 진입점 함수의 문맥 인자다.

**`MaterialContext.rsh`(문맥 칸·접근자).** `Engine/Shaders/Private/Material/MaterialContext.rsh`는 문맥 값 표에서 만든 파일이다. `MaterialCommon.rsh`가 구조체 자리에서 include한다.
- **만들기.** MaterialCompiler 공개 함수 `WriteMaterialContextSource(allocator, out)`가 표로 텍스트를 만든다. 저장소의 파일은 그 출력이고, 시험이 byte로 견준다(어긋나면 실제 텍스트를 실패에 남긴다 — 골든 원문과 같은 방식, 7.7절). 표를 고치면 파일을 함께 고친다. 파일 안 주석은 생성 원문 규칙대로 영어다.
- **공급 표지.** pass 원문은 include 전에 공급하는 기능마다 `#define MATERIAL_SUPPLY_<F> 1`을 둔다(pass 셰이더 타입과 정점 입력 타입의 공급 합집합 — 19.3.1절, 16절 MRD13·MRB9). 정의하지 않은 기능은 공급하지 않은 것이다.
- **칸.** 필드형 값마다 칸 하나다. 이름은 Rocks 이름의 lowerCamel(`Time` → `time`), 이전 변형은 `previous<이름>`이다. 표 `stages`대로 정점 단계 값은 `MaterialVertexInputs`, 픽셀 단계 값은 `MaterialInputs`에 둔다(둘 다면 둘 다). 칸은 공급 여부와 무관하게 늘 있다(구조체 layout이 머티리얼마다 바뀌지 않는다. 쓰지 않는 칸은 하위 컴파일러가 지운다). Double 칸은 LWC 표현 타입이다(MS2b2).
- **접근자.** 값마다 매크로 `MaterialContext<이름>`을 둔다. `MATERIAL_SUPPLY_<F>`이면 필드형은 칸(`((c).time)`), 함수형은 pass 원문이 정의하는 `MaterialPass<이름>(c, …)` 호출이다. 예외는 pass가 늘 채우는 고정 칸 SvPosition(표의 고정 칸 표지 — 7.3절)으로, 공급 표지와 상관없이 칸을 읽는다. 아니면 표의 기본값 리터럴이다(UE 템플릿의 0, 파티클 색 1 — 결정 MD15). 그래서 생성 원문은 어떤 pass에서도 컴파일된다.
- **채울 기능.** 생성 파일 include 뒤에 `MATERIAL_CONTEXT_<F>` = `MATERIAL_SUPPLY_<F>` ∧ (어느 진입점이든 `MATERIAL_<E>_USES_<F>`)를 정의한다. pass가 칸을 채울지·보간할지 묻는 데 쓴다(pass가 부르는 진입점만으로 더 좁힐 수 있다 — 19.3.1절).
- MS2b1은 표의 기존 값만 담는다. MS2b4가 표에 값을 더하면 같은 파일이 다시 만들어진다.

**MS2b1 lowering 표.** 근거는 UE `MaterialExpressions.cpp` Compile·`HLSLMaterialTranslator.cpp` 번역 함수·`CFG/BaseMaterialExpressions.ini` 외부 코드 선언이다(UE 기본 번역기는 선언의 단계·영역만 보고 최소 기능 등급·`<PREV>`·환경 정의는 IR 경로만 본다 — `HLSLMaterialTranslator.cpp:3589–3610`). 단계·영역·등급은 표 값으로 검사한다(위).

| 노드 | lowering | UE 근거·비고 |
|---|---|---|
| DeltaTime·IsOrthographic·TwoSidedSign·PerInstanceRandom·PerInstanceFadeAmount·DistanceCullFade·PrecomputedAoMask·IsFirstPerson·ObjectRadius·ObjectOrientation·ObjectBounds·PreSkinnedPosition·PreSkinnedNormal·VertexNormal·VertexTangent·PixelNormal·CameraVector·LightVector·PixelDepth·ViewSize·SceneTexelSize·LightmapUvs·ParticleDirection·ParticleMacroUv·ParticleMotionBlurFade·ParticleRadius·ParticleRandom·ParticleRelativeTime·ParticleSize·ParticleSpeed·AtmosphericLightVector·AtmosphericLightColor·EyeAdaptation·ScalarBlueNoise | 같은 이름의 문맥 값 하나(DecalLifetimeOpacity는 `Opacity` 핀) | 외부 코드 선언(ini 1037–1146) 또는 번역 함수. PixelNormal의 Normal 입력 순환은 MS2d(19.3.3절) |
| VertexColor | Result = RGB(앞 셋), R·G·B·A = 성분 | UE 출력 0은 RGB 마스크(`HLSLMaterialTranslator.cpp:9116`, `MaterialExpressions.cpp:10022`) |
| DecalColor·ParticleColor | RGB·R·G·B·A·RGBA | ini 1041·1056 |
| FontSignedDistance | 네 핀 = FontSignedDistanceData의 x·y·z·w | ini 1046 |
| ScreenPosition | ViewportUV·PixelPosition 값 | `:5924–5960` |
| ParticleSpriteRotation | Rad·Deg = 성분 0·1 | ini 1067 `float2(rot, rot × 57.2957795131)` |
| ParticleSubUvProperties | TextureCoordinate0·1 = SubUvCoords0·1, Blend = SubUvLerp | `:6006–6027` |
| StaticBool | `value`의 Bool 상수 | `:8914`(MCT_StaticBool — Rocks IR은 Bool 상수, 빈도 Constant라 StaticOnly 입력을 통과한다) |
| Ddx·Ddy | Value의 미분(묶음 (b) — Double은 낮춘다), 빈도 Pixel | `:11805–11892`. Value 연결이 없으면 `MissingInput`이다(UE 기본 번역기는 진단 없이 값 없음, IR 경로는 필수 입력 — 값 없는 결과가 조용히 번지지 않게 IR 경로를 따른다) |
| Time | `ignorePause`면 RealTime, 아니면 Time(이전 정점 문맥은 이전 변형). `override_Period`면 period 0은 상수 0, 아니면 fmod(시간, period) | `:5276–5323` |
| PerInstanceCustomData·PerInstanceCustomData3 | (dataIndex UInt 상수, DefaultValue) 인자의 함수형 값. 기본값은 Float1·Float3로 맞춘다(`CoerceParameter`) | `:14643–14672`. UE는 기본값을 그대로 넘겨 HLSL 암묵 변환에 맡긴다(편차). PerInstanceCustomData3 기본값은 UE처럼 Float3(0) |

**문맥 값 표 정정(MS2b1).** UE 기본 번역기와 다른 표 값을 고쳤다: Time·RealTime·CameraPosition·ViewProperty·ActorPosition에 이전 변형(`PrevFrameGameTime`·`PrevFrameRealTime`·`<PREV>` — `:5257, 5280, 6406–6441`), ActorPosition 영역 Surface·DeferredDecal·Volume(`MaterialExpressions.cpp:12651`), AtmosphericLightColor와 SkyAtmosphere 조명·대기 값 다섯을 픽셀 단계(픽셀 파라미터 오버로드뿐 — `SH/Private/MaterialTemplate.ush:2300–2462`), DecalMipmapLevel 영역 DeferredDecal(`:8043`), ScalarBlueNoise 영역 Surface(`:12060`), ObjectOrientation 영역 전체(기본 번역기는 영역을 보지 않는다 — `:11716`). 자원 타입 값(MeshPaintTextureDescriptor)은 원문 타입이 생기는 MS2c까지 `MaterialContext.rsh`에서 뺀다.

**시험(MS2b1).** 문맥별 번역(같은 노드가 정점·픽셀에서 각각 값을 내고 진단은 한 번), 이전 진입점 변형(Time의 previous 변형과 DeltaTime의 현재 값), V4(픽셀 전용 값이 WPO에, 정점 전용 값이 표면에)·V6(영역 밖 값) 음성 대조와 DCE된 노드의 무진단, 사용 정의가 실제 쓰는 기능과 같음(MS-T13), 최소 기능 등급 기록, `MaterialContext.rsh` 일치, 골든 원문(MS-T3).

**MS2b2 묶음.** 공간 변환이 커서 둘로 나눈다. MS2b2a는 LWC 표현·좌표·위치·view 값·경계, MS2b2b는 `TransformVector`·`TransformPosition`(UE 공간 쌍 전부와 경유 경로)이다.

**LWC 표현(MS2b2a — 생성 원문).** UE 5.8 기본 번역기는 tile + offset이다(`WSVECTOR_IS_TILEOFFSET 1` — `HLSLMaterialTranslator.cpp:2539`). Rocks도 같다(5.3절).
- 타입은 `MaterialDouble1`–`4 { tile, offset }`(값 = tile × `MaterialDoubleTileSize` + offset, tile 크기 2097152 — UE `UE_LWC_RENDER_TILE_SIZE`)이고 `Engine/Shaders/Private/Material/MaterialDouble.rsh`에 둔다.
- 연산은 UE `SH/Private/LWCOperations.ush`와 같은 식이다: 더하기·빼기(tile끼리·offset끼리), 곱하기(`tileL × (tileR × T + offsetR) + tileR × offsetL`, `offsetL × offsetR` — `:113–118`), 나누기(곱하기 × 역수), 부정, 최소·최대(비교 선택), 절댓값, 내림·올림·반올림·버림(tile을 정수로 맞춘 뒤 offset — `:103–106`), 비교(`(tileL − tileR) × T` 대 `offsetR − offsetL` — `:23–58`), 선택. IR 연산의 피연산자는 산술 공통 타입으로 모두 Double이므로 Double × Double 판만 둔다. 이름은 `MaterialDoubleAdd` 등이다.
- 변환: Float → Double은 `MaterialDoublePromote`(tile 0), Double → Float는 `MaterialDoubleDemote`(tile × T + offset — UE `WSDemote`), 성분 선택·이어 붙이기·복제는 tile과 offset에 같은 swizzle을 쓴 `MakeMaterialDouble<N>(tile, offset)`이다.
- 상수: 호스트가 double 값 v를 tile = floor(v / T + 0.5)(UE `LWCNormalizeTile`과 같은 정수 tile), offset = float(v − tile × T)로 나눠 `MakeMaterialDouble<N>(float<N>(tile…), float<N>(offset…))`로 쓴다(T가 2의 거듭제곱이라 v − tile × T는 double에서 정확하다). UE는 double 상수를 `%0.8f` 숫자로 내고 uniform 식이 나눈다(`ShaderValue.cpp:373–378`) — 값은 같고 표기만 다르다.
- **Double 뺄셈의 Float 자름.** 같은 Double 타입 둘의 뺄셈 결과는 Float로 바꾼다(상대 공간으로 본다). UE 기본 번역기 `Sub`의 `LWCTruncateMode` 기본 2(`HLSLMaterialTranslator.cpp:9550–9575`, `r.MaterialEditor.LWCTruncateMode`)와 같다. 타입이 다른 둘(Double − Float)은 Double이다.
- `Distance`는 UE처럼 뺄셈 뒤 길이다(`CompileHelperLength` — `MaterialExpressions.cpp:18522`, 6.8절 lowering 표). 그래서 Double 둘의 거리도 이 자름을 거쳐 정밀도를 잃지 않는다.
- MS2a의 생성 원문 `NotYetLowered`(Double)는 없앤다. `WorldPositionTile` 연산만 남는다(UE IR 전용 연산 — 노드가 만들지 않는다).

**MS2b2a lowering 표.**

| 노드 | lowering | UE 근거·비고 |
|---|---|---|
| TextureCoordinate | `TexCoord(coordinateIndex)`(UInt 인자). `|uTiling − vTiling| > 1e−8`이면 × Float2(u, v), 아니고 `|1 − uTiling| > 1e−8`이면 × uTiling, 아니면 그대로. `unMirrorU`·`unMirrorV`면 해당 성분을 `x × UnMirrored × 0.5 + 0.5`로(픽셀 전용 값 `UnMirrored`). 색인 ≥ 8이면 `InvalidProperty`("Only 8 texture coordinate sets can be used by this feature level, currently using N") | `MaterialExpressions.cpp:5324–5346`, `HLSLMaterialTranslator.cpp:6686–6727`, `SH/Private/MaterialTemplate.ush:2623–2650`(`UE_SMALL_NUMBER` 1e−8) |
| WorldPosition | Default → WorldPosition(Double3), ExcludeAllShaderOffsets → 픽셀 문맥은 WorldPositionNoShaderOffsets·정점 문맥은 WorldPosition, CameraRelative → TranslatedWorldPosition(Float3), CameraRelativeNoOffsets → 픽셀 TranslatedWorldPositionNoShaderOffsets·정점 TranslatedWorldPosition. XYZ·XY·Z 출력. 이전 정점 문맥은 이전 변형 | `:6220–6302`(`_NoMaterialOffsets`는 픽셀에서만, `Prev`는 정점 이전 프레임에서만) |
| LocalPosition | Instance → LocalPosition(픽셀 ExcludeOffsets는 LocalPositionNoShaderOffsets), Primitive → PrimitivePosition(NoShaderOffsets), InstancePreSkinning → PreSkinnedPosition(정점 전용) | `:6131–6218` |
| CameraPosition | CameraPosition(Double3, 이전 변형) | `MaterialExpressions.cpp:5581`(ViewProperty WorldSpaceCameraPosition) |
| ObjectPosition | Absolute → ObjectPosition(Double3), CameraRelative → ObjectTranslatedPosition(Float3). 이전 변형 없음 | `:6304–6315` |
| ActorPosition | Absolute → ActorPosition, CameraRelative → ActorTranslatedPosition(둘 다 이전 변형, 영역 Surface·DeferredDecal·Volume) | `:6406–6441`, `MaterialExpressions.cpp:12651` |
| ParticlePosition | Absolute → ParticlePositionAbsolute(Double3), CameraRelative → ParticlePositionTranslated(Float3). 둘 다 이전 변형 | `:6035–6061` |
| ViewProperty | 속성마다 문맥 값 하나(Property)와 역수(InvProperty — 전용 값이 없으면 1 / 값). BufferSize는 ViewSize, ViewportOffset은 두 출력 모두 Float2(0, 0) | `MaterialExpressions.cpp:10394–10428`, `HLSLMaterialTranslator.cpp:5245–5267`, 표 `MaterialExternalCodeRegistry.cpp:143–177`. 문맥 값 표의 범용 `ViewProperty`(Float4)는 속성마다 타입이 달라 없애고 속성별 값으로 바꾼다 |
| Panner | Time(연결 없으면 Time 문맥 값), Speed의 x·y(스칼라면 둘 다 그 값 — UE ComponentMask 복제), Argᵢ = Time × Speedᵢ(`fractionalPart`면 frac), 결과 = Append(Arg₁, Arg₂) + Coordinate(연결 없으면 TexCoord(constCoordinate)). Time을 스칼라로 맞추지 않는다 | `MaterialExpressions.cpp:5626–5656`. PeriodicHint는 uniform 식에만 의미가 있어 없다(MS2c 상수 식에서 본다) |
| Rotator | t = Time × speed, RowX = (cos t, −1 × sin t), RowY = (sin t, cos t), O = (centerX, centerY). 좌표가 Float3면 Append(Append(dot(RowX, uv.xy − O), dot(RowY, uv.xy − O)) + O, uv.z), 아니면 Append(dot(RowX, uv − O), dot(RowY, uv − O)) + O | `:5676–5713` |
| TruncateWorldPosition | Input이 Double이면 Float로, 아니면 그대로. 연결 없으면 `MissingInput` | `HLSLMaterialTranslator.cpp:4857–4881` |
| ObjectLocalBounds·Bounds·PreSkinnedLocalBounds | Half Extents·Extents·Min·Max = 표의 *LocalBoundsHalf·Full·Min·Max(Bounds는 `type`으로 Instance·Object·PreSkinned). 영역은 표(Surface·Volume — V6) | `:6327–6397`. UE의 decal 영역 전용 문구 대신 V6 공통 문구다 |
| ReflectionVector | CustomWorldNormal이 연결되면 N(Float3로 맞춤, `normalizeCustomWorldNormal`이면 normalize) → −CameraVector + N × dot(N, CameraVector) × 2(픽셀 전용 CameraVector가 V4를 낸다), 아니면 ReflectionVector 값 | `:5879–5908`, `MaterialTemplate.ush:2674–2682` |
| XrPassthroughCameraUv | L = lerp(XrPassthroughCameraUv0, XrPassthroughCameraUv1, uv.x), 결과 = lerp(L.xy, L.zw, uv.y). 연결 없으면 `MissingInput` | `:14061–14072` |

- `MATERIAL_NUM_TEXCOORDS`는 진입점에서 닿는 TexCoord의 최대 상수 색인 + 1이다(UE 정점·픽셀 UV 할당의 합 — 정점 입력에서 읽을 채널 수, 7.3절).
- 문맥 값 표에 더하는 값(끝에 더한다): TranslatedWorldPositionNoShaderOffsets·LocalPositionNoShaderOffsets·PrimitivePositionNoShaderOffsets(픽셀 전용), PrimitivePosition, ObjectTranslatedPosition, ActorTranslatedPosition, UnMirrored(픽셀 전용), XrPassthroughCameraUv0·1(xr), view 값 스물(FieldOfView·TanHalfFieldOfView·CotanHalfFieldOfView·WorldViewOrigin·TemporalSample Count·Index·Offset·RVT 출력 Level·Derivative·MaxLevel·PreExposure·OneOverPreExposure·ResolutionFraction·ResolutionFractionInverse·PostVolumeUserFlags·FirstPerson FieldOfView·TanHalfFieldOfView·CotanHalfFieldOfView·Scale·NearPlane — UE `<PREV>`가 있는 것은 이전 변형).
- 기본값 정정: VertexNormal (0, 0, 1)·VertexTangent (1, 0, 0)(UE 픽셀 파라미터의 TangentToWorld 단위 행렬 — `MaterialTemplate.ush:665–670`), ParticleMotionBlurFade 1(`:4386` — 스프라이트가 아니면 1).

**MS2b2b 공간 변환.** UE 기본 번역기 `TransformBase`(`HLSLMaterialTranslator.cpp:11093–11507`)를 그대로 따른다.
- **공간 번호.** UE `EMaterialCommonBasis`의 Tangent·Local·World·View·Camera·MeshParticle·Instance·TranslatedWorld·PeriodicWorld·FirstPerson이다. 노드 enum은 UE 값 그대로이고, MS1b가 뺀 UE 숨김 값(벡터 ParticleWorld 5, 위치 Particle 7 — UE `UMETA(Hidden)`이지만 저장된 에셋에 나온다)을 되살린다(`RE/Public/Materials/MaterialExpressionTransform.h`, `MaterialExpressionTransformPosition.h`).
- **검증**(UE 순서, 문구는 UE 그대로): 위치 변환에 Tangent가 끼면 "Tangent basis not available for position transformations", FirstPerson이 원본이고 대상이 아니면 "Transforming from First Person Space is not supported"(InvalidProperty). 입력이 스칼라이고 원본이 Tangent면 (0, 0, x)로 만들고, 성분이 3보다 적으면 "input must be a 3-component vector (current: …) or a scalar (if source is Tangent)"(V1). 원본 = 대상이면 입력 그대로(UE도 캐스트 없이 돌려준다).
- **입력·결과 타입.** 원본이 World인 위치 변환은 입력을 Double3로, 그 밖은 Float3로 맞춘다(Float4는 앞 셋 — `ValidCast`). 결과는 대상이 World인 위치 변환만 Double3, 그 밖은 Float3다.
- **직접 쌍.** 아래 쌍만 직접 계산하고 나머지는 UE와 같은 경유 공간(기본 World, 원본이 World·Camera·View·PeriodicWorld면 TranslatedWorld, Local·Particle·Instance → PeriodicWorld·FirstPerson은 TranslatedWorld)으로 두 번 나눈다.
  - 행렬 곱 쌍은 문맥 함수 값 `Transform<원본><Position|Vector>To<대상>`(인자 하나)이다. 이 함수는 UE `MaterialTemplate.ush` 보조 함수처럼 pass·정점 입력 타입이 공급한다(primitive·instance·particle 행렬과 DF 행렬 표현은 Renderer 몫 — 16절 MRD13). 이전 정점 문맥에서 UE가 `<PREV>`를 쓰는 쌍은 이전 변형이다.
  - TranslatedWorld ↔ World 위치는 IR이다: World = Double(A) − PreViewTranslation(자르지 않는 Double 뺄셈 — UE `WSSubtract` 직접 코드), TranslatedWorld = Float(World + PreViewTranslation)(UE `WSAddDemote`). 벡터는 그대로다.
  - TranslatedWorld ↔ PeriodicWorld 위치는 A ∓ `PeriodicWorldOrigin(tile)`(tile이 2의 거듭제곱 상수면 `PeriodicWorldOriginPow2` — UE `GetPeriodicWorldOrigin[_Pow2]`, `MaterialTemplate.ush:1031–1044`)이다. tile 크기는 `PeriodicWorldTileSize` 입력(연결 없으면 속성 32)을 Float3로 맞춘 값이다.
  - TranslatedWorld → FirstPerson은 `TransformToFirstPerson(A, saturate(alpha))`(alpha는 `FirstPersonInterpolationAlpha` 입력, 연결 없으면 1, Float1로 맞춤 — 이전 정점 문맥은 `TransformToPreviousFirstPerson`)이다.
- **영역.** TranslatedWorld·World → Local은 Surface·Volume만이다("This transformation is only supported in the 'Surface' material domain." — V6, 표 값 영역으로도 걸린다).

| 원본 → 대상 | 위치 | 벡터 |
|---|---|---|
| Tangent → World | — | TransformTangentVectorToWorld |
| Local → World | TransformLocalPositionToWorld(→ Double3, 이전) | TransformLocalVectorToWorld(이전) |
| Local → TranslatedWorld | TransformLocalPositionToTranslatedWorld(이전) | (World 경유) |
| TranslatedWorld → World | IR 뺄셈 | 그대로 |
| TranslatedWorld → Camera·View | TransformTranslatedWorldPositionToCamera·View(이전) | …VectorToCamera·View(이전) |
| TranslatedWorld·World → Tangent | — | TransformWorldVectorToTangent |
| TranslatedWorld → Local·Particle·Instance | TransformTranslatedWorldPositionTo…(이전) | TransformTranslatedWorldVectorTo…(Local만 이전) |
| TranslatedWorld → PeriodicWorld·FirstPerson | IR(위) | — |
| World → Local·Particle·Instance | TransformWorldPositionTo…(Double3 입력, Local만 이전) | TransformWorldVectorTo…(Local만 이전) |
| World → TranslatedWorld | IR 더하기·낮춤 | 그대로 |
| Camera·View → TranslatedWorld | TransformCamera·ViewPositionToTranslatedWorld(이전) | …VectorToTranslatedWorld(이전) |
| Particle → World·TranslatedWorld | TransformParticlePositionToWorld(Double3)·…ToTranslatedWorld(이전) | TransformParticleVectorToWorld·…ToTranslatedWorld |
| Instance → World·TranslatedWorld | TransformInstancePositionToWorld(Double3, 이전)·…ToTranslatedWorld(이전) | TransformInstanceVectorToWorld·…ToTranslatedWorld(이전) |
| PeriodicWorld → TranslatedWorld | IR(위) | — |

- 두 노드의 입력이 없으면 "Missing Transform input vector"·"Missing Transform Position input vector"(MissingInput)다. PeriodicWorldTileSize·FirstPersonInterpolationAlpha 입력은 그 공간을 쓸 때만 번역한다(UE `MaterialExpressions.cpp:11275–11282`).

**MS2b3 스위치.** UE 기본 번역기를 따른다(번역 때 고르는 스위치는 고른 입력만 컴파일하고 결과 타입은 그 입력 타입, 실행 중 값 스위치는 두 입력을 모두 컴파일해 `GenericSwitch` 삼항 — `HLSLMaterialTranslator.cpp:11603–11650`).
- **번역 대상(`MaterialTranslationTarget`).** 번역 입력에 더한다. 품질 수준(Low·High·Medium·Epic·None — UE `EMaterialQualityLevel` 순서, None은 UE `Num`: Default 입력), 형식 사실(IsMobile·IsPC·IsConsole — UE DDSPI 스위치가 노출하는 셋, `RHI/Private/DataDrivenShaderPlatformInfo.cpp:213–215`), 형식 샘플러 상한(UE 기본 16), bindless 설정(Disabled·RayTracing·Minimal·All), 프로필 기능(distance field·virtual texture·ray tracing·virtualized geometry), 컴파일러 종류(프록시 굽기·조명 굽기), Low 품질 모바일 흉내(UE `r.Material.SimulateMobileForLowQuality`). 번역은 실제로 읽은 항목을 결과(`MaterialTranslation.consultedTarget`)에 기록한다 — 쿠킹(MS3)은 그 항목만 번역 키에 넣어 형식 등급을 묶는다(19.3.4절 "쓰지 않으면 키에 없음").
- **늦은 입력.** 번역 때 고르는 스위치의 가지 입력은 번역 순서 계획에 넣지 않는다(고른 가지만 IR로 만든다 — 6.3절 2항). lowering 함수가 고른 입력을 요청하면 번역기가 그 입력에서 거슬러 닿는 노드로 작은 번역 순서를 다시 만들어 반복으로 번역한다(재귀는 스위치가 겹친 깊이만큼뿐이다). 계획 밖 요청의 깊이 상한(`MaxLoweringDepth`)은 이 겹침 깊이에 쓴다. 순환은 늦은 계획에서도 V2다.
- **조건 값.** UE 기본 번역기의 pass·렌더러 상태 스위치는 셰이더 함수(`GetShadowReplaceState()` 등 — `Common.ush:2152–2309`)를 조건으로 한 삼항이다. Rocks도 같게 문맥 값 표의 Bool 값(공급 기능 view·meshpaint·pathtracing, pass가 상수로 정의하면 하위 컴파일러가 접는다)을 조건으로 한 IR `Select`로 낸다. 6.2절·6.8절이 예고한 별도 `Condition` 연산은 두지 않는다(하위 컴파일러가 상수 조건 삼항을 `#if`와 같은 코드로 줄인다 — 자원 layout은 MS2c에서 실제로 닿는 텍스처만 센다).

| 노드 | 해석 | UE 근거·비고 |
|---|---|---|
| StaticSwitch | Value가 연결되면 그 값이 상수 Bool이어야 하고(아니면 V7 "Failed to cast Value input to static bool type"), 없으면 `defaultValue`. 고른 가지(A = True·B = False)만 번역한다. 고른 가지에 연결이 없으면 `MissingInput`(UE는 진단 없이 값 없음 — 편차) | `MaterialExpressions.cpp:8889–8914`, `HLSLMaterialTranslator.cpp:9018–9025`. UE의 Dynamic Bool(MCT_Bool) 경로는 DynamicBoolParameter가 오는 MS2c에서 DynamicBranch로 |
| QualitySwitch | 대상 품질의 입력이 연결되면 그것, 아니면 Default. None이면 Default. Default가 없으면 "Quality switch missing default input" | `:9067–9087` |
| RenderPathSwitch | 조건 값 RenderPathDeferred·RenderPathForwardPlus로 Select(Deferred, Select(ForwardPlus, Default)) — 연결 없는 경로 입력은 Default. Mobile 입력은 Rocks 렌더 경로에 없어 쓰지 않는다(Default가 받는다). Default가 없으면 "Shading path switch missing default input". WPO·OpacityMask·PDO 진입점에서 닿으면 V15 | 6.3절 3항(편차 — UE는 번역 때 하나를 고른다, `:9843–9898`) |
| CapabilityTierSwitch | 조건 값 CapabilityTierSm6·CapabilityTierSm5로 Select(Sm6, Select(Sm5, Mobile)) — 연결 없는 입력은 Default. 대상의 Low 품질 모바일 흉내가 켜져 있고 품질이 Low면 Mobile 입력만 쓴다. Default가 없으면 "Feature Level switch missing default input" | 19.3.4절(편차 — UE는 번역 때 고른다, `:9220–9243`). 원문이 형식마다 하나라 조건 값이다(결정 MD4) |
| StageSwitch | 픽셀 문맥은 PixelShader, 정점 문맥은 VertexShader 입력. 둘 다 필수("Missing input PixelShader"·"Missing input VertexShader") | `:17651–17660` |
| PreviousFrameSwitch | 이전 정점 문맥은 PreviousFrame, 아니면 CurrentFrame | `:9026–9033` |
| FormatFactSwitch | `ddspiPropertyNames`의 None 아닌 항목 조건(True면 사실이 참, False면 거짓)의 논리곱이 참이면 InputTrue, 아니면 InputFalse. 모르는 이름·빈 목록은 "… whose condition is invalid …", 모두 None이면 "… whose condition is empty …"(InvalidProperty) | `:9330–9393` |
| SamplerBudgetSwitch | `requiredSamplers` ≤ 대상 샘플러 상한이면 InputTrue, 아니면 InputFalse | `:9784–9797` |
| BindlessFeatureSwitch | 대상 bindless가 All·Minimal이면 Bindless, 그 밖(RayTracing 포함 — 머티리얼 단계가 ray tracing이 아니다)은 Default | `:9510–9535` |
| DistanceFieldFeatureSwitch·VirtualTextureFeatureSwitch | 대상 기능이 켜져 있으면 Yes, 아니면 No. 둘 다 필수 | `:12603–12625`, `:3844–3861` |
| ProxyBakePassSwitch | 대상 컴파일러가 프록시 굽기면 MaterialProxy, 아니면 Realtime. 둘 다 필수 | `:17684–17692` |
| RuntimeVirtualTexturePassSwitch | RVT 출력 노드 입력을 번역하는 중이면 VirtualTextureOutput, 아니면 Default(RVT 출력 노드가 오는 MS2d 전까지는 늘 Default). 둘 다 필수 | `:3799–3807`(출력 범위 규칙 — pass 정의가 아니다) |
| ShadowPassSwitch·ReflectionCaptureSwitch·MeshPaintTextureSwitch | Select(조건 값 ShadowPass·ReflectionCapturePass·MeshPaintTextureValid, 대체 입력, Default). 두 입력 모두 필수 | `HLSLMaterialTranslator.cpp:11670–11688, 9142–9145` |
| VirtualizedGeometryPassSwitch | 대상이 virtualized geometry를 지원하면 Select(VirtualizedGeometryPass, Nanite, Default), 아니면 Default(Nanite 입력은 번역하지 않는다) | `MaterialExpressions.cpp:18648` |
| LightBakePassSwitch | 대상 컴파일러가 조명 굽기면 Lightmass, 아니면 Select(LightBakePass, Lightmass, Realtime)(값 없는 쪽이 있으면 남은 쪽) | `:17580–17601` |
| GlobalIlluminationPassSwitch | Select(GlobalIlluminationPass, DynamicIndirect, Default). StaticIndirect·DynamicIndirect가 없으면 Default. StaticIndirect는 UE처럼 번역하지만 쓰지 않는다(오류는 남는다) | `HLSLMaterialTranslator.cpp:11665–11668` |
| RayTracingQualitySwitch | 대상이 ray tracing을 지원하면 Select(RayTracingPass, RayTraced, Normal), 아니면 Normal(RayTraced는 번역하지 않는다) | `MaterialExpressions.cpp:17757–17775` |
| PathTracingQualitySwitch | Select(PathTracingPass, PathTraced, Normal). 두 입력 모두 필수("Missing PathTracingQualitySwitch input 'Normal'"·"'PathTraced'") | `:17808–17838` |
| PathTracingRayTypeSwitch | Main에서 시작해 Shadow·IndirectDiffuse·IndirectSpecular·IndirectVolume 순서로 Select(그 광선 종류 조건 값, 입력, 앞 결과)를 겹친다(뒤가 이긴다). 연결 없는 입력은 Main | `HLSLMaterialTranslator.cpp:11700–11709`(광선마다 실행 중 값) |

- 실행 중 값 스위치의 결과는 두 값의 산술 공통 타입이다(UE `GetArithmeticResultType`). 두 입력이 같은 값이면 그 값이다.
- 번역 때 고르는 스위치(Quality·Low 품질 모바일 흉내의 CapabilityTier)는 Default의 연결 여부만 보고 고른 입력 하나만 번역한다(UE `GetEffectiveInput` — 고르지 않은 Default의 오류는 나지 않는다, `MaterialExpressions.cpp:9082–9107`). 실행 중 값 스위치의 "필수" 입력은 고르기 전에 UE 문구로 검사한다(ShadowPass·ReflectionCapture·MeshPaintTexture는 "Missing input Default"·"Missing input <대체>" — `:17614–17628, 21612–21629, 3975–3991`).
- **진단 위치의 한계.** 문맥 값(`Extern`)은 hash-consing으로 한 값이 되고 원인 위치는 NodeId가 가장 작은 노드다(6.8절). 그래서 같은 문맥 값을 읽는 두 노드 가운데 하나만 단계·영역 밖이면 V4·V6·V15가 다른 노드에 붙을 수 있다(결정적이다). 소비 노드를 따라가 위치를 고치는 것은 후속이다(UE는 노드마다 따로 컴파일해 그 노드에 붙인다).
- **보간기 입력의 순환.** VertexInterpolator 입력이 같은 보간기로 돌아오면 정점 문맥에서 그 보간기를 다시 만나 V2 대신 V4("only available in pixel shaders")로 끝난다(무한 반복은 없다). UE는 보간기 미리 컴파일 단계에서 정점 단계 사용 오류로 같은 결과를 낸다.
- 문맥 값 표에 더하는 Bool 값: RenderPathDeferred·RenderPathForwardPlus·CapabilityTierSm5·CapabilityTierSm6·GlobalIlluminationPass·RayTracingPass·PathTracingPass·PathTracingShadowRay·PathTracingIndirectDiffuseRay·PathTracingIndirectSpecularRay·PathTracingIndirectVolumeRay(공급하지 않으면 거짓 — UE 기본 `Common.ush`).
- V15: 진입점 Vertex·VertexPrevious·OpacityMask·PixelDepthOffset의 결과에서 RenderPathDeferred·RenderPathForwardPlus 값이 닿으면 오류다(원인 위치는 그 조건 값을 만든 RenderPathSwitch 노드).

**MS2b4 묶음.** 둘로 나눈다. MS2b4a는 정점 → 픽셀 값(VertexInterpolator·CustomizedUv 출력 핀), MS2b4b는 공급 노드(파티클·데칼·하늘·대기·후처리·distance field·물리장·SpeedTree·Hair·RVT·메시 페인트·글꼴 밖 나머지)와 표에 없는 문맥 값이다.

**MS2b4a 보간기.**
- **문맥 사이 번역.** 노드 결과 기억을 평가 문맥마다 따로 둔다(문맥을 바꿀 때 지우지 않는다). 픽셀 문맥의 VertexInterpolator lowering은 입력을 정점 문맥에서 번역해 달라고 요청하고(번역기가 문맥을 잠시 정점으로 바꿔 그 입력의 늦은 계획을 번역한다), 그 값을 피연산자로 한 IR `Interpolate`(번호 83)를 낸다. UE도 보간기 입력을 정점 단계로 컴파일한다(`HLSLMaterialTranslator.cpp:771–809, 1433–1451`). UE는 그래프의 보간기를 모두 미리 컴파일하고 오류를 픽셀에서 쓸 때만 보고한다 — Rocks는 픽셀에서 닿는 보간기만 번역하므로 결과가 같다.
- **`Interpolate` 값.** 결과 타입은 입력과 같은 폭의 Float다(Double은 낮춘다 — UE `ValidCast`, Bool·정수는 V1 "Bad type … for VertexInterpolator" — `:14446–14448`). 빈도는 Pixel이다. 입력 연결이 없으면 "VertexInterpolator> Input missing"(MissingInput). 정점 문맥에서 VertexInterpolator 출력을 쓰면 "Custom interpolator outputs only available in pixel shaders."(V4 — `:9161`).
- **검사·사용 기능.** 픽셀 진입점에서 거슬러 갈 때 `Interpolate`의 피연산자 쪽은 정점 단계다: V4 검사는 그 아래를 정점 단계로 보고(정점 전용 PreSkinnedPosition 등이 허용된다), 그 아래 문맥 값의 공급 기능은 Vertex 진입점 사용 기능에 넣는다.
- **보간 배정(6.4절).** `BuildIrModule`이 픽셀 진입점에서 닿는 `Interpolate`를 모듈 번호 순(6.6절 — 노드 저장 순서와 무관)으로 모아 성분을 앞에서부터 이어 붙인다(UE도 성분을 이어 채운다 — `GetVertexInterpolatorsOffsets`, `:2615–2681`). 즉시값 [0]에 첫 성분 번호(성분 단위 오프셋)를 둔다. 같은 입력의 두 보간기는 hash-consing으로 한 슬롯이다.
- **생성 원문.** 정점 함수는 루트(WPO·CustomizedUv 결과·보간 값의 정점 쪽)를 모아 값을 한 번에 낸다(루트마다 내면 함께 쓰는 부분식이 두 번 선언된다). `MaterialVertexResult`에 `float4 interpolants[MaterialMaxInterpolantVectors]`(상한은 V9 보간 예산 — 16 벡터로 시작, 셰이더 시스템 7.2절 7항 `TEXCOORDn` n ≤ 15)를 둔다. 정점 함수는 `Interpolate` 피연산자를 계산해 `r.interpolants[k].<성분> = …;`로 쓰고, 픽셀 함수는 `c.interpolants[k].<성분>`을 읽는다(pass가 `MaterialInputs.interpolants`로 넘긴다). 픽셀 함수는 `Interpolate` 아래를 내지 않는다. `MATERIAL_NUM_INTERPOLANT_VECTORS`는 ⌈성분 수 / 4⌉다. 넘으면 V9("Maximum number of custom vertex interpolators exceeded").

**CustomizedUv 출력 핀.** UE는 픽셀 속성을 모두 번역한 뒤 픽셀이 쓰는 UV 채널 수(가장 큰 TexCoord 색인 + 1)만큼 CustomizedUV를 정점 단계로 컴파일한다. i < `NumCustomizedUVs`이고 연결이 있으면 그 값, 아니면 정점의 TexCoord(i) 그대로다(`HLSLMaterialTranslator.cpp:1684–1697`, `Material.cpp:7254–7268`). Rocks:
- 픽셀 문맥 뒤에 정점 문맥에서 i < (픽셀 진입점의 TexCoord 최대 색인 + 1)인 CustomizedUv<i>를 번역해 Vertex 결과 `CustomizedUv<i>`(Float2)로 둔다. 연결은 i < `traits.customizedUvCount`일 때만 쓴다(넘는 연결은 V5 경고 — UE는 조용히 버린다, 편차).
- 생성 원문: `MaterialVertexResult.customizedUvs[MaterialMaxTexCoords]`(8)에 정점 함수가 쓴다. pass는 그것을 픽셀 TexCoord로 보간한다. `MATERIAL_NUM_CUSTOMIZED_UVS` = 결과 수. 정점 진입점의 TexCoord는 원래 정점 UV다(UE와 같다).

**MS2b4b 공급 노드.** 남은 MS2b 노드 29개다. 렌더러 자원을 읽는 값은 모두 pass가 공급하는 문맥 함수 값이고(19.3.2절 — 자원 수는 Renderer가 기능별로 선언, MRD13), lowering은 UE 기본 번역기의 인자 기본값·검사·식을 맞춘다. 근거 줄은 `MaterialExpressions.cpp`(ME)·`HLSLMaterialTranslator.cpp`(HT)·`BaseMaterialExpressions.ini`(ini)·`MaterialTemplate.ush`(MT)다.
- **피연산자 상한.** 인자가 많은 함수형 값(DistanceFieldApproxAo·SpeedTree)을 위해 `MaxIrOperands`를 4로 올린다(6.8절 값 정의 — IR hash 형식은 피연산자 수를 이미 쓰므로 형식 표지만 올린다). 상수 인자 여럿은 Float 벡터 하나로 묶어 넘긴다(pass 함수가 푼다 — UE 함수 서명과 인자 순서는 같고 묶음만 다르다).
- **위치 기본값.** WorldPosition 입력이 없으면 `worldPositionOriginType`이 Absolute면 WorldPosition(Double3), CameraRelative면 TranslatedWorldPosition이다. 연결된 입력도 같은 타입(Double3·Float3)으로 맞춘다(UE `GetWorldPositionOrDefault`의 `CoerceParameter` — HT:3316–3329).
- **인자 기본값.** DynamicParameter·PerInstanceCustomData(3Vector)는 공급하지 않으면 노드의 기본값 인자를 돌려준다(UE `MaterialTemplate.ush:302–354, 1501–1593` — 문맥 값 표의 `defaultArgument`).

| 노드 | lowering | UE 근거·비고 |
|---|---|---|
| DecalDerivative | DDX·DDY = DecalDerivativeX·Y(Float2, 영역 DeferredDecal·픽셀) | HT:8063–8074, MT:1364–1409 |
| DecalMipmapLevel | TextureSize(연결 없으면 핀 기본 (256, 256))가 정확히 Float2가 아니면 V1("Unmatching conversion … -> float2"), 그 밖은 DecalMipmapLevel(TextureSize) | ME:19723–19747, HT:8039–8061 |
| DynamicParameter | DynamicParameter(defaultValue Float4 상수, parameterIndex UInt — 0–3으로 자름) → Param1–4 = 성분, RGB = 앞 셋, RGBA | HT:11533–11549 |
| SphericalParticleOpacity | SphericalParticleOpacity(Density, 연결 없으면 1). scene depth를 읽으므로 영역 PostProcess이거나 반투명 블렌드가 아니면 V6("Only transparent or postprocess materials can read from scene depth.") | HT:6069–6086, 1796–1799 |
| MainDirectionalLight | Illuminance·Direction = MainDirectionalLightIlluminance·Direction(공급하지 않으면 Direction은 (0, 0, 1)) | HT:12414–12422, BasePassCommonFunctions.ush:9–27 |
| SkyAtmosphereLightDirection·…IlluminanceOnGround | (lightIndex를 0–1로 자른 UInt) 인자 값 | HT:12347–12357 |
| SkyAtmosphereLightIlluminance | (위치 기본값, lightIndex) | HT:12340–12345 |
| SkyAtmosphereLightDiskLuminance | (lightIndex, 원판 지름 각: 연결되면 cos(0.5·π/180 × 입력), 없으면 −1) | ME:20587–20593, HT:12363 |
| SkyAtmosphereAerialPerspective | (위치 기본값), 공급하지 않으면 (0, 0, 0, 1) | HT:12382–12387, MT:2422–2462 |
| SkyAtmosphereDistantLightScatteredLuminance | 인자 없는 값 | HT:12389 |
| SkyAtmosphereViewLuminance | (WorldDirection, 연결 없으면 (−1, −1, −1) × CameraVector) | HT:12366–12380 |
| SkyLightEnvironmentSample | (Direction 기본 (0, 0, 1), Roughness 기본 0), 영역 Surface(표) | ME:21911–21920, HT:12395–12412 |
| EyeAdaptationInverse | LightValue(기본 (1, 1, 1)) × EyeAdaptationInverse(Alpha, 기본 1). LightValue가 float이 아니면 V1("EyeAdaptationInverse expects a float numeric type for LightValue"), Alpha가 Float1이 아니면 V1("… expects a float type for Alpha"). 결과는 LightValue 타입 | HT:14731–14764 |
| DepthOfFieldFunction | (Depth, 연결 없으면 PixelDepth 값, functionValue UInt) | HT:11922–11932 |
| PostProcessVolumeFlagTest | (BitIndex, 연결 없으면 핀 기본 0) | HT:11934–11944 |
| DistanceToNearestSurface·DistanceFieldGradient | (위치 기본값을 Float3로 — UE도 float3 인자, LWC는 낮춘 overload) | HT:12169–12230 |
| DistanceFieldApproxAo | N = clamp(numSteps, 1, 4), S = max(stepScaleDefault, 1). N = 1이면 step 0·bias Base·max Base, 아니면 step = (Radius − Base) / (S^(N−1) − 1)·bias = Base − step·max = Radius(IR로 계산 — Radius는 N ≠ 1일 때만 번역). 인자 (위치 Float3, Normal(기본 VertexNormal) Float3, Float4(step, S, bias, max), N UInt) | HT:12232–12264(검사는 자른 뒤의 N — IR 경로의 결함 MIR:4211–4289은 따르지 않는다) |
| HairAttributes | 출력마다 Hair 문맥 값(U·V = HairUv.x·y, Length·Radius = HairDimensions.x·y, Tangent = HairTangent(useTangentSpace Bool), Clump ID = HairClumpId.x, 나머지는 같은 이름 값) | ME:21055–21114, HT:12080–12148 |
| RuntimeVirtualTextureCustomData | RVT 출력 노드 입력 범위 밖이면 Float4 상수 0(UE와 같다 — RVT 출력 노드 MS2d 전까지 늘 0) | HT:8828–8837 |
| MeshPaintTextureCoordinateIndex | 인자 없는 값 | ini:1051 |
| Sample·EvaluatePhysics Vector·Scalar·IntegerField | 위치(CameraRelative면 TranslatedWorld → World 뺄셈)를 Float3로 낮춘 값과 fieldTarget UInt 인자의 값. 결과 Float3·Float1·Float1. UE는 ES3_1에서 상수 0이다 — Rocks는 공급하지 않으면 기본값 0이라 같다 | HT:12267–12323 |
| SpeedTree | `MATERIAL_NUM_TEXCOORDS`를 8로 올린다(UE `AllocateSlot(AllocatedUserVertexTexCoords, 2, 6)` — UV 2–7). 정점 단계 전용 값 SpeedTreeVertexOffset(Float3(Geometry, Wind, LOD — 연결 없으면 속성 enum 값), Float3(billboardThreshold, 이전 프레임(이전 정점 문맥 ∧ accurateWindVelocities면 1), ExtraBend 연결 여부), ExtraBend(연결 없으면 0 벡터)). UE의 skeletal mesh 사용 검사는 사용 플래그가 오는 쿠킹(MS3 — 19.10절)에서 한다 | ME:20130–20140, HT:14699–14720 |

- 문맥 값 표에 더하는 값: DecalDerivativeX·Y, DynamicParameter, MainDirectionalLightIlluminance·Direction, PostProcessVolumeFlagTest, DistanceToNearestSurface·DistanceFieldGradient·DistanceFieldApproxAo(공급 기능 distancefield, 공급하지 않으면 UE 대체값 — gradient (0, 0, 0.001)·AO 1·거리 0(UE는 view의 최대 cone 거리)), Hair 값 14개(픽셀 단계, Depth만 정점도), RuntimeVirtualTextureCustomData, 물리장 값 여섯, SpeedTreeVertexOffset(정점 단계). 기존 값의 인자 수를 위 표에 맞춘다(DepthOfFieldFunction 2 등).
