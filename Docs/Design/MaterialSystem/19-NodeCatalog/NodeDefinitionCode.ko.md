# 머티리얼 시스템(MaterialSystem) 설계

## 19. 노드 목록 — 노드 정의 코드(단계 MS1b)

### 19.13 노드 정의 코드 배치와 UE 변환 규칙

MS1b는 19.6절 표의 "구현"·"TODO" 노드 297개의 정의(핀·속성 struct·메뉴)와 문맥 값 표(19.3.2절)를 MaterialSchema에 넣는다. 이 절은 그 코드를 어디에 어떻게 두는지와, UE 클래스에서 Rocks 정의로 옮기는 규칙을 정한다. 규칙이 애매하면 UE 5.8 동작을 따른다.

#### 19.13.1 파일 배치

| 무엇 | 위치 | 규칙 |
|---|---|---|
| 노드 속성 struct | `Public/MaterialSchema/Nodes/<범주>/<노드>Properties.h` | struct 하나에 파일 하나다(클래스당 파일 규칙). 범주 폴더 이름은 `MaterialNodeCatalog.json`의 `category`다 |
| 속성 enum | `Public/MaterialSchema/Nodes/<범주>/<범주>NodeEnums.h` | 범주 안에서 쓰는 enum을 모은다. 여러 범주가 쓰는 enum은 처음 쓰는 범주의 파일에 둔다. MS3b `MaterialTraitEnums.h`와 같은 묶음 방식이다 |
| 속성 안 보조 struct | 그 struct를 쓰는 노드 속성 파일과 같은 폴더, struct당 파일 | 예: `CustomInput.h`(임의 HLSL 입력 하나) |
| 노드 정의 표 | `Private/Nodes/<범주>Nodes.cpp` | 범주마다 `Span<const MaterialNodeDefinition> <범주>NodeDefinitions()` 하나. `BuiltinMaterialNodeGroups`가 범주 묶음을 차례로 등록한다 |
| 동적 핀 함수 | 그 범주의 `Private/Nodes/<범주>Nodes.cpp` | `nodegraph::DynamicPinsFn` |
| 문맥 값 표 | `Public/MaterialSchema/MaterialContextValue.h`·`Private/MaterialContextValueTable.cpp` | 19.13.5절 |

- 속성 struct 이름은 `<노드>Properties`다(예: `TextureSampleProperties`). namespace는 `rocks::material`이다.
- 속성이 없는 노드는 struct를 만들지 않는다(`propertiesSchema` null).
- 처음 작성은 UE 헤더·cpp·ini에서 뽑은 데이터로 기계적으로 만들고, 그 뒤로는 손으로 유지한다. 추출 스크립트는 저장소에 두지 않는다. UE 소스 경로에 의존하고, 노드마다 lowering 단계에서 손으로 고칠 것이기 때문이다. 대조는 MS-T12(목록 파일)와 리뷰로 한다.

#### 19.13.2 속성 변환

| UE | Rocks | 근거 |
|---|---|---|
| 편집 가능한 `UPROPERTY`(`EditAnywhere`·`EditDefaultsOnly`·`EditInstanceOnly`·`VisibleAnywhere`) | 속성 struct 필드 | 사용자가 정하는 값이다 |
| `transient`·`config`·편집 지정자가 없는 `UPROPERTY` | 넣지 않는다 | 편집 상태·캐시·ini 데이터다. ini 데이터(외부 코드 식별자)는 문맥 값 표로 간다 |
| 기반 `UMaterialExpression`의 편집기 칸(위치·`Desc`·접힘·미리보기·`MaterialExpressionGuid`) | 넣지 않는다 | NodeGraph 공통 기능(19.6.30절 "공통")이다 |
| deprecated 칸(이름·지정자에 `DEPRECATED`) | 넣지 않는다 | 하위호환을 두지 않는다 |
| 입력 덮기 상수(`meta=(OverridingInputProperty=X)`)의 float·vector 칸 | 핀 X의 기본값(`MaterialPinDefault`) | UE도 연결이 없을 때 쓰는 값이다. NodeGraph 핀 값이 같은 일을 한다 |
| 입력 덮기 상수 중 UV 채널 번호(`ConstCoordinate`) | 속성 `constCoordinate` | 값이 아니라 "연결이 없으면 이 UV 채널"이라는 선택이다 |
| 입력 덮기 상수 중 정수 값(`ConstIndex`·`ConstBitIndex`·`ConstCollectionIndex`) | 핀 기본값(scalar) | 값으로 쓰인다 |
| `StaticSwitch.DefaultValue` | 속성 `defaultValue : bool` | static 값이라 핀 값(float)으로 담지 않는다 |
| 파라미터 식별 `ExpressionGUID` | `parameterId : Guid` | 9.2절 키 |
| `ParameterName`·`Group`·`SortPriority` | `parameterName : Name`·`group : Name`·`sortPriority : int32` | |
| 파라미터 기본값·범위·UI 칸 | 필드이고 `nonSemanticFields`에 넣는다 | 5.5절: 기본값은 원문을 바꾸지 않는다. static 파라미터 기본값은 의미 칸이다 |

- 이름은 UE 이름에서 `b` 접두를 떼고 camelCase로 쓴다(`bShowTextureInputPin` → `showTextureInputPin`). bitfield `uint32 b…:1`은 `bool`이다.
- 타입: `float`·`int32`·`uint32`·`uint8`(수)·`double`은 그대로, `FName` → `Name`, `FString` → `String`, `FLinearColor` → `LinearColor`, `FVector2D`·`FVector`·`FVector4`(double) → `Vector2d`·`Vector3d`·`Vector4d`, `FVector4f` → `Vector4f`, `FGuid` → `Guid`, `TArray<T>` → `Array<T>`, `TEnumAsByte<E>`·`E` → Rocks enum.
- **에셋 참조**(`TObjectPtr<UTexture>` 등, 대상 클래스가 있든 없든)는 19.4.3절대로 `MaterialAssetReference`다. 대상 클래스가 없는 참조(TODO 클래스)는 필드 위에 `// TODO(MPQ-<이름>)`를 단다. 같은 그래프 안 노드를 가리키는 UE 객체 포인터(`NamedRerouteDeclaration`·함수 호출 노드)는 노드 id(`Guid`)다.
- **enum**: 이름에서 `E` 접두를 떼고, 값에서 UE 접두(`TMVM_`·`SSM_` 등)를 뗀다. 값 번호와 순서는 UE와 같다. UE의 숨은 값(`UMETA(Hidden)`)과 `_MAX` 값은 넣지 않는다. 이름이 상표면 19.5절 규칙으로 바꾼다.
- 기본값은 UE 헤더 초기값과 생성자 대입 결과와 같다.
- UE가 편집 지정자를 주었지만 Rocks 기능과 맞지 않는 칸은 그 struct 주석에 이유를 적고 넣지 않는다.
- **문자열 기본값**: UE 생성자가 `FString`·`FName`에 비어 있지 않은 기본값을 주면(예: Custom `Code` "1", `Description` "Custom") 정의의 `textDefaults`(`MaterialPropertyTextDefault { field, text }`)에 둔다. C++ 초기값으로는 줄 수 없다: String은 allocator, Name은 NamePool이 필요하다. 노드를 새로 만드는 쪽(에디터 노드 추가 — 14장)이 속성 struct 기본 생성 값에 이 값을 넣고 Record를 쓴다. 등록 검사는 field가 속성 schema의 최상위 String·Name 필드인지 본다.

#### 19.13.3 핀

- **키와 순서**: UE 입력 멤버 선언 순서(부모 클래스부터)와 이름이다(19.5절). 출력은 UE 생성자의 `Outputs` 목록 순서와 이름이다. 이름 없는 출력 하나는 키 `Result`다(UE는 빈 이름).
- **입력 타입**: UE `GetInputValueType`의 값이다. 함수가 없으면 UE 기본 `MCT_Float`이다. 입력 struct 종류가 정하는 타입은 그것을 쓴다: `FMaterialAttributesInput` → `MaterialAttributes`, `FSubstrateMaterialInput` → `Surface`, `FShadingModelMaterialInput` → `ShadingModel`.
- **출력 타입**: UE `GetOutputValueType`의 값이다. 없으면 출력 마스크로 정한다. 마스크가 켠 성분이 1·2·3·4개면 `Float1`–`Float4`, 마스크가 없으면 `Numeric`(입력 폭을 따른다 — 5.3절)이다.
- **UE 값 타입 대응**: `MCT_Float` → `Numeric`, `MCT_Float1..4` → `Float1..4`, `MCT_LWCScalar..LWCVector4` → `Double1..4`, `MCT_UInt1..4` → `UInt1..4`, `MCT_Bool` → `Bool`, `MCT_StaticBool` → `StaticBool`, `MCT_Texture` → `TextureAny`, `MCT_Texture2D`·`Texture2DArray`·`TextureCube`·`TextureCubeArray`·`VolumeTexture` → 같은 이름(`VolumeTexture`는 `Texture3D`), `MCT_TextureExternal` → `TextureExternal`, `MCT_TextureVirtual` → `TextureVirtual`, `MCT_SparseVolumeTexture` → `SparseVolumeTexture`, `MCT_TextureCollection` → `TextureCollection`, `MCT_TextureMeshPaint` → `TextureMeshPaint`, `MCT_TextureMaterialCache` → `TextureMaterialCache`, `MCT_MaterialAttributes` → `MaterialAttributes`, `MCT_Substrate` → `Surface`, `MCT_ShadingModel` → `ShadingModel`, `MCT_Unknown` → `Numeric`.
- **조건부 핀**: UE가 속성에 따라 입력·출력을 숨기면(`GetInputValueType`·`GetInputs`의 분기, 예: `TextureSample`의 `MipValueMode`) 같은 조건의 동적 핀 함수를 둔다. 정적 핀 목록에는 늘 있는 핀만 둔다.
- **동적 핀 함수**(`Private/Nodes/MaterialDynamicPins.cpp`)는 UE가 속성에 따라 핀을 바꾸는 노드에 둔다. 동적 핀은 입력 기본값이 없다(연결이 없을 때의 값은 UE처럼 Const 속성이 준다 — 예: TransformPosition의 `constPeriodicWorldTileSize`).

  | 노드 | 핀 | 키 |
  |---|---|---|
  | CustomHlsl | 입력 `inputs`(타입 Any — UE도 텍스처 객체를 HLSL 인자로 받는다), 출력 Result(`outputType`)·`additionalOutputs` | 사용자 이름, 비면 `Input<n>`·`Output<n>` |
  | Switch | `inputs` 분기 입력(정적 SwitchValue·Default 뒤) | 사용자 이름, 비면 `Input<n>` |
  | Convert | `convertInputs`·`convertOutputs`(성분 폭 타입) | `Input<n>`·`Output<n>` |
  | Operator | `dynamicInputs` | A–Z, 27번째부터 `Input<n>`(표시 이름 없음 — UE는 "Input <n>") |
  | 텍스처·SVT 샘플 계열 | `mipValueMode`가 MipLevel·MipBias면 MipValue, Derivative면 CoordinatesDX·CoordinatesDY | UE 멤버 이름 |
  | RVT 샘플 | MipLevel·MipBias면 MipValue, DerivativeUV·DerivativeWorld면 DDX·DDY | UE 멤버 이름 |
  | TextureObject·TextureObjectParameter | 출력 Result(타입은 `texture`의 기대 클래스, 비면 Texture2D) | Result |
  | TextureObjectFromCollection | 출력 Result(`textureType`) | Result |
  | TransformPosition | 주기 world면 PeriodicWorldTileSize, 1인칭 공간이면 FirstPersonInterpolationAlpha | UE 멤버 이름 |
  | DynamicParameter | 출력 Param1–Param4(표시 이름 `paramNames`)·RGB·RGBA | Param1–Param4 |
  | FunctionCall | 함수 입출력 사본 `inputs`·`outputs`(UE `FunctionInputs`·`FunctionOutputs` — 함수가 바뀌면 에디터가 다시 채운다) | `P_<입출력 노드 id 32자 hex>`(NodeGraph GD2) |
  | FunctionInput | Preview 입력과 Result 출력(타입 `inputType`) | Preview·Result |
  | Aggregate | `attributeNames`마다 입력·출력 한 쌍(MaterialAttributes 종류면 내장 속성 타입, 사용자 종류는 Any — TODO MPQ-MaterialAggregate) | 속성 이름 |
  | GetAttributes·SetAttributes | `attributeGetTypes`·`attributeSetTypes`마다 출력·입력(내장 속성 표의 이름·타입, custom 속성은 표시 없음·Any) | `A_<속성 id 32자 hex>` |
  | LayerStack | 레이어 함수 입력 사본 `layerInputs`(UE `LayerInputs`) | 입력 이름 |
  | MaterialCacheOutput | 표본(`isSample`)이면 Primitive·UV 입력과 `attributes` 출력, 아니면 `attributes` 입력 | 속성 표시 이름 |

- UE가 계속 저장하지만 편집 지정자가 없는 칸 중 핀을 정하는 사본(FunctionCall 입출력, LayerStack 입력, MaterialCache 속성, Convert 성분 대응 `convertMappings`, NamedRerouteUsage `declaration`, 함수 입출력 노드 `id`)은 속성에 넣는다(19.13.2절 예외).
- **UE와 다른 점**: UE Custom 생성자는 이름 없는 입력 하나를 더하지만 Rocks 새 노드는 입력이 없다(배열 기본값은 정적 data로 주지 않는다). Operator 27번째 이후 입력의 표시 이름이 없다. 속성에 따라 바뀌는 정적 핀 표시 이름은 대표 이름 하나다.
- **필수 입력**(`Required` — 연결이 없으면 번역 오류)은 UE `Compile`이 연결 없음을 오류로 내는 입력이다(`if (!X.GetTracedInput().Expression) return Compiler->Errorf(...)` 꼴, 가장 가까운 조상의 `Compile`). UE `meta=(RequiredInput)`·`IsInputConnectionRequired`는 편집기 표시(굵은 핀)일 뿐이라 근거로 쓰지 않는다. 예: Substrate BSDF 입력은 메타가 없어 표시는 필수지만 `Compile`이 기본값을 넣으므로 Rocks에서는 필수가 아니다. 입력 배열 스위치(Quality·RenderPath·CapabilityTier)의 Default는 UE `IsInputConnectionRequired`와 `Compile`이 함께 요구하므로 필수다. 각 노드의 lowering 단계(MS2)가 `Compile`을 옮기며 이 표를 다시 확인한다.
- static 전용 입력은 `StaticOnly`(핀 타입 StaticBool·StaticInt·StaticEnum), UE `meta=(AdvancedDisplay)` 입력은 `Advanced`다. 기본값이 없는 입력은 값 편집을 숨긴다(`HideValue` — UE도 입력 덮기 상수가 없는 입력에는 칸이 없다).
- **표시 이름**은 UE `GetInputName`이다. 재정의가 없으면 UE 기본 규칙대로 입력 멤버 이름 그대로이고(띄어쓰기 없음), C 배열 입력은 `<이름>_<색인>`이다(UE `UMaterialExpression::GetInputName`). UE가 속성에 따라 바꾸는 표시 이름(world 위치 원점에 따른 "Absolute World Position" 등, Substrate shading model별 이름)은 정적 핀에서는 대표 이름 하나로 둔다. 동적 핀은 UE와 같게 바꾼다.
- **키 충돌**: 입력과 출력이 같은 이름이면(UE `SubstrateTransmittanceToMFP`의 `Thickness`, `GetMaterialAttributes`의 `MaterialAttributes`) 출력 키에 `Out`을 붙인다. NodeGraph 핀 키는 방향과 상관없이 노드 안에서 유일하다. 표시 이름은 UE 그대로다. UnrealExchange는 이 규칙으로 출력 이름을 잇는다.
- **입력 배열 노드**: UE `FExpressionInput Inputs[N]`(QualitySwitch·ShadingPathSwitch·FeatureLevelSwitch)는 UE `GetInputName`의 이름을 키로 한 정적 핀이다. QualitySwitch는 Default·Low·High·Medium·Epic, RenderPathSwitch는 Default·Deferred·Forward(표시 Forward+)·Mobile, CapabilityTierSwitch는 Default·ES3_1(표시 Mobile)·SM5·SM6이다. UE의 제거된 기능 등급(ES2_REMOVED·SM4_REMOVED) 칸은 두지 않는다.
- **동적 핀 함수의 속성 읽기**: `nodegraph::DynamicPinsFn`은 문서 view와 NamePool만 받는다(객체 Registry가 없다). 그래서 `NodePropertyReader`가 속성 Record를 공용 `RecordReader`로 직접 읽는다. 필드는 리플렉션 schema의 필드 이름으로 찾고 id는 schema가 준다. 기록되지 않은 필드는 기본값이다(누락 = 기준). 읽은 문자열은 속성 바이트를 빌린다. NodeGraph 6.2절이 동적 핀 표시 이름을 노드 속성 바이트에 두도록 허용한다. 버린 대안: `DynamicPinsInput`에 객체 Registry를 넣어 `LoadStructRecord`로 struct 전체를 해석하는 것. NodeGraph 서술 계약을 바꾸고, 핀 계산마다 struct 전체 할당과 2단계 해석을 하게 된다.
- **통과 핀 타입 `Any`**: UE `MCT_Unknown` 입출력(static switch·reroute·조건 스위치 값)은 `MaterialPinType::Any`다(5.3절). 연결 때 막지 않고 번역이 연결된 값의 타입으로 정한다.

#### 19.13.4 메뉴·표지

- 표시 이름은 UE `GetCaption`의 첫 caption이다. 없으면 UE 기본(클래스 이름에서 접두를 떼고 단어를 띄운 것)이다. 19.5절로 이름을 바꾼 노드는 바꾼 이름을 쓴다.
- 범주는 `MaterialNodeCatalog.json`의 `category`다. 검색어는 UE `GetKeywords`와 UE 메뉴 범주(ini `MenuCategories`)다.
- 도움말은 UE 클래스 문서 주석이다. 없으면 표시 이름으로 짧게 쓴다(영어).
- UE `meta=(Private)`·팔레트 숨김 노드는 `HiddenFromPalette`, `Experimental`은 `Experimental`, 파라미터 노드는 `Parameter` 표지다.
- 그래프 종류: 함수 입출력 노드처럼 UE `IsAllowedIn`이 막는 노드만 좁힌다.
- schemaVersion은 1이고 migrate는 없다(처음 판).

#### 19.13.5 문맥 값 표

- `MaterialContextValue`는 19.3.2절 항목을 데이터로 가진다: 이름, 반환 타입(`MaterialPinType`), 형태(필드·함수와 인자 수), 공급 기능(`MaterialContextFeature`), 허용 영역 mask(`MaterialDomain`), 단계 mask, 최소 기능 등급, 미분(`Zero`·`Valid`·`NotAware`), 공급하지 않았을 때의 UE 기본값(scalar 4개), previous 변형 여부.
- 원본은 UE `BaseMaterialExpressions.ini`의 외부 코드 선언 101개와 템플릿 필드다. 이름은 Rocks 이름 규칙(19.5절)으로 바꾸고 UE 이름은 주석에 둔다.
- HLSL 정의 문자열은 표에 두지 않는다. 그것은 생성 원문의 일이라 MS2b가 `MaterialCommon.rsh` 쪽에 둔다.
- 공급 기능은 UE 환경 정의(`NEEDS_PARTICLE_*`·`USES_*`)와 원천 데이터로 정한다(19.3.1절).

#### 19.13.6 시험(MS-T12 (a))

- 등록된 정의마다 (id, 상태, 선행, 범주)가 목록 파일의 행과 같고, 목록의 구현·TODO 행이 모두 등록되어 있다. 목록 파일은 시험 데이터로 빌드에 넣는다.
- 정의마다: 핀 키가 겹치지 않음, 기본값 성분 수가 핀 타입 폭 이하, 속성 schema가 리플렉션에 등록됨, `nonSemanticFields`가 schema 필드임(등록 검사가 이미 한다).
- 동적 핀 함수마다 속성 값 둘 이상(기본값과 바꾼 값)으로 핀 목록을 확인한다.
- 문맥 값 표: 이름이 겹치지 않음, 101개 UE 선언이 모두 대응됨, 공급 기능이 유효함.
- policy: `TODO(` 표기가 `TODO(MPQ-<이름>)` 형식이고 이름이 19.4.1절 표에 있다.
