## 8. 머티리얼

### 8.1 그래프 추출

`UMaterial::GetExpressions()`(EP/Materials/Material.h:1478-1480)와 `GetEditorComments()`의 노드마다 아래를 쓴다. 쿠킹된 에셋에는 그래프가 없으므로(`UMaterialEditorOnlyData`는 `UCLASS(Optional)` — Material.h:324-325) 미쿠킹 프로젝트에서만 추출할 수 있다. `UMaterialEditorOnlyData`와 expression의 Outer는 그 머티리얼이다(E/Private/Materials/MaterialInterface.cpp:182-185, 1073). 그래서 덤프 범위는 "Outer가 그 머티리얼인 객체"로 정한다.

```json
{ "guid": "…", "class": "MaterialExpressionTextureSampleParameter2D",
  "position": [x, y],
  "inputs":  [ { "name": "UVs", "from": { "node": "<guid>", "output": 0 }, "mask": [r, g, b, a] | null } ],
  "outputs": [ { "name": "RGB", "mask": [1, 1, 1, 0] } ],
  "properties": { … 노드 고유 UPROPERTY 덤프(11.5절) … } }
```

- 입력 열거는 `CountInputs()`·`GetInput(i)`·`GetInputName(i)`(MaterialExpression.h:328-345)다. 5.5에서 deprecated된 `GetInputsView`는 쓰지 않는다. 연결은 대상 노드 GUID(`MaterialExpressionGuid`, :177)와 출력 번호다. reroute는 따라가지 않고 노드 그대로 쓴다(Rocks 쪽 번역이 접는다).
- Custom 노드는 `Code`·`OutputType`·`Inputs`·`AdditionalOutputs`·`AdditionalDefines`·`IncludeFilePaths`를 그대로 쓴다(MaterialExpressionCustom.h:79-101). include 파일 내용은 추출하지 않는다(`CustomIncludeSkipped` 진단).
- 머티리얼 출력: `bUseMaterialAttributes`(Material.h:1066)에 따라 개별 출력 핀(BaseColor·Metallic·…·`FrontMaterial`) 또는 `MaterialAttributes` 핀 하나. 핀마다 연결 또는 미연결 상수(`FMaterialInput<T>`의 `UseConstant`·`Constant` — EP/MaterialExpressionIO.h:158-171)를 쓴다.
- 기본 속성: `MaterialDomain`, `BlendMode`, `ShadingModel`, `OpacityMaskClipValue`, `TwoSided`, `bTangentSpaceNormal`, usage 플래그(`bUsedWith*`), 그 밖의 표면 설정 UPROPERTY.

### 8.2 인스턴스·함수·컬렉션

- 인스턴스: `Parent`, 종류별 override 배열 9종(Scalar·Vector·DoubleVector·Texture·TextureCollection·ParameterCollection·RuntimeVirtualTexture·SparseVolumeTexture·Font — MaterialInstance.h:773-805)의 `ParameterInfo{Name, Association, Index}`·값, static 파라미터(`GetStaticParameters()` — switch·component mask·레이어, MaterialInstance.h:654-663), `BasePropertyOverrides`(MaterialInstanceBasePropertyOverrides.h:12-138). 해석된 최종 값(`GetAllParametersOfType`, MaterialInterface.h:950)도 `resolved`로 함께 쓴다(파라미터 키 순 정렬). Rocks 번역이 부모 사슬을 다시 풀지 않고 결과를 대조할 수 있게 하기 위해서다.
- 함수: 그래프(8.1절과 같은 형식), 입출력 노드, 용도(`Default`·`MaterialLayer`·`MaterialLayerBlend`), `bExposeToLibrary`. 함수 인스턴스: 부모와 override.
- 파라미터 컬렉션: scalar·vector 파라미터 이름·GUID·기본값.

### 8.3 Substrate

UE 5.8은 엔진 기본이 Substrate 꺼짐이지만(`r.Substrate` 기본 0 — RT/RenderCore/Private/RenderUtils.cpp:2068-2072), 5.7 이후 새로 만든 프로젝트에는 켜진 설정이 들어간다(Engine/Source/Editor/GameProjectGeneration/Private/GameProjectUtils.cpp:199-212). Substrate가 켜진 프로젝트에서는 `UMaterial::PostLoad`가 옛 그래프를 메모리에서 Substrate 그래프로 바꾼다(E/Private/Materials/Material.cpp:4629-4643). 이 변환은 옛 핀용 `ShadingModels` 노드, `bUseMaterialAttributes` 머티리얼용 `ConvertMaterialAttributes` 노드(:3829), 도메인별 변환 노드(Decal·LightFunction·PostProcess·UI·VolumetricFogCloud, :3905-4128)를 만든다. 그래서 같은 에셋이라도 프로젝트 설정에 따라 추출 결과가 다르다.

- 플러그인은 에디터 메모리의 그래프를 그대로 추출하고, `bundle.json`의 `projectSettings.substrate`와 머티리얼별 변환 버전(`SubstrateConversionVersion`, Material.h:424)을 기록한다.
- Rocks 번역의 1차 범위(UX4)는 Substrate가 꺼진 프로젝트의 옛 출력 핀 그래프다. Substrate 그래프(변환 노드 `ShadingModels`·`ConvertMaterialAttributes`, `SlabBSDF`, 연산자 노드 — MaterialExpressionSubstrate.h)를 Rocks 표면 모델(MaterialSystem 10.3절 결정 MD1)로 옮기는 일은 UX5로 나눈다. UX5 전에 Substrate 그래프를 만나면 8.5절 대체 규칙을 따른다.

### 8.4 굽기 대체(선택)

추출 옵션 `-BakeMaterials`를 주면 플러그인이 머티리얼마다 `IMaterialBakingModule::BakeMaterials`(Dev/MaterialBaking/Public/IMaterialBakingModule.h:28-37)로 BaseColor·Normal·Metallic·Roughness·Specular·Emissive·Opacity·OpacityMask·AmbientOcclusion을 텍스처로 굽는다. 크기는 호출자가 `PropertySizes`로 주며(MaterialBakingStructures.h:24, 49) 플러그인 기본값은 `-BakeSize`의 기본 `RocksDefaultBakeSize`(= 1024)다. emissive는 `SetEmissiveHDR(true)`(IMaterialBakingModule.h:43)로 HDR(RGBA16F)로 굽고, 나머지는 RGBA8이다. 메시 데이터 없이 정사각 quad 기준으로 굽는다(MaterialBakingStructures.h:120-121). 결과는 그 머티리얼 JSON의 `baked` 아래에 텍스처 원본 형식으로 넣는다. commandlet에서 굽기를 하려면 `-AllowCommandletRendering`이 필요하다(RT/Core/Public/Misc/App.h:400-406).

- Epic GLTFExporter가 같은 선례다. 속성마다 상수·단순 텍스처 샘플이면 직접 쓰고, 아니면 MaterialBaking으로 굽는다(Engine/Plugins/Enterprise/GLTFExporter/Source/GLTFExporter/Private/Tasks/GLTFDelayedMaterialTasks.cpp:1264-1513, 1636-1799).
- 구운 결과는 UV 공간 결과라 월드 위치·카메라·시간에 의존하는 그래프는 맞지 않는다. 대체 수단이며 번역의 목표 품질이 아니다.
- GPU 렌더 결과라 같은 UE 상태라도 GPU·드라이버에 따라 bytes가 다를 수 있다. 결정성 보장에서 제외하고 `environment`에 기록한다(3.2절).

### 8.5 Rocks 쪽 번역

- **번역 표**: UE 노드 클래스 → Rocks 노드(MaterialSystem 5.7절 1차 노드 묶음). 표는 `UnrealExchange`의 데이터(노드 클래스 이름, 핀 이름 대응, 속성 대응, 버전)다.
- **같은 이름 파라미터 병합**: UE는 한 머티리얼 안에서 이름이 같은 파라미터 노드 여러 개를 파라미터 하나로 다룬다. 함수 안 파라미터도 global association이면 이름으로 합친다. Rocks는 같은 scope의 같은 이름을 검증 오류(MaterialSystem 5.4절 V8)로 보고 함수 호출 위치마다 독립 파라미터로 만든다. 번역은 같은 이름·같은 종류의 파라미터를 Rocks 파라미터 노드 하나로 합치고 나머지 사용처를 그 노드에 잇는다. 기본값이 다르면 GUID 순으로 첫 노드의 값을 쓰고 `ParameterDefaultConflict` 경고를 낸다(UE가 어느 값을 쓰는지는 UX4에서 원본으로 확인한다). 함수 안 global association 파라미터는 "공유" 표시를 붙인 전역 scope 파라미터로 옮긴다. 결과 그래프에는 같은 scope의 같은 이름이 남지 않아 V8을 통과한다(MaterialSystem 9.2절 "외부 그래프 번역", UXR4 반영).
- **결과 결정**:
  1. 출력에 닿는 모든 노드가 번역되면 `Material` 그래프로 저장한다.
  2. 번역 못 한 노드가 출력에 닿고 `baked`가 있으면, 엔진 내장 "구운 표면" 부모 머티리얼의 `MaterialInstance`로 만든다(텍스처 파라미터에 구운 텍스처). 구운 텍스처는 같은 임포트의 다중 출력 에셋으로 만든다(ImportRecord `outputs`·`ownerAssetId` — AssetSystem 6.2절 ImportOwner). 원래 그래프는 따로 저장하지 않는다. 원본(묶음 JSON)이 ImportRecord의 원본으로 남는다. `MaterialBakedFallback` 경고.
  3. `baked`도 없으면 엔진 기본 머티리얼을 부모로 둔 `MaterialInstance`로 저장하고 `UntranslatedMaterialNode` 경고(노드 클래스 목록)를 낸다. 자리 노드가 든 그래프로 저장하지 않는 이유: 출력에 닿은 자리 노드는 쿠킹 오류이고(MaterialSystem 5.4절 V12), 쿠킹은 하나라도 실패하면 게시하지 않으므로(AssetSystem 10절) 머티리얼 하나가 프로젝트 쿠킹 전체를 막는다.
- 인스턴스는 부모가 1번으로 번역되었으면 override를 Rocks 파라미터 키로 옮긴다. 부모가 2·3번이면 인스턴스도 같은 결과를 따른다(2번이면 그 인스턴스용 구운 텍스처가 필요하므로 플러그인이 인스턴스도 굽는다).
- 번역 결과 확인: `resolved` 파라미터 값과 Rocks 인스턴스 해석 결과를 대조한다(다르면 `ParameterResolveMismatch` 경고).

### 8.6 구현에서 정한 점(UX4a)

UX4a(UE 쪽 머티리얼 계열 추출, `FRocksMaterialExtractor`) 중 8.1·8.2절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다. 번역(8.5절)·Substrate 번역(8.3절)·굽기(8.4절)는 하지 않는다.

1. **노드 식별자는 객체 이름이다(8.1절을 고침).** 8.1절은 연결을 대상 노드 GUID(`MaterialExpressionGuid`)로 정했지만 엔진 콘텐츠에서도 GUID가 겹치는 노드가 흔하다(내보낸 엔진 그래프 29개 중 11개, `M_Grid`는 노드 91개 중 13개가 다른 노드와 GUID가 같다 — 같은 클래스 노드끼리 겹치므로 복사한 노드가 GUID를 물려받은 것으로 보인다). UObject 이름은 같은 Outer(그 머티리얼·함수) 안에서 UE가 유일하게 보장하므로 노드마다 `name`(예: `MaterialExpressionAdd_3`)을 쓰고 연결 `from.node`는 그 이름이다. `guid`는 노드 속성으로 남긴다. 8.5절 "GUID 순으로 첫 노드"는 GUID가 같으면 이름 순으로 정한다(UX4b).
2. **`data` 스키마.** Material은 `{ "nodes", "outputs" }`, MaterialFunction은 `{ "nodes" }`, MaterialInstanceConstant는 `{ "resolved" }`, MaterialFunctionInstance·MaterialParameterCollection은 `{}`다(settings 덤프가 전부다 — 함수 인스턴스의 부모·override, 컬렉션의 scalar·vector 파라미터 이름·`Id`·기본값). 노드는 `{ "guid", "name", "class", "position": [x, y], "inputs", "outputs", "properties" }`이고 expression 다음 comment 순(각자 UE 배열 순서)이다. 입력은 `CountInputs`·`GetInput`·`GetInputName` 순서의 `{ "name", "from": { "node", "output" } | null, "mask": [r, g, b, a] | null }`(mask는 연결이 있고 `Mask`가 켜졌을 때만), 출력은 `GetOutputs()`의 `{ "name"(None이면 ""), "mask" }`다. `properties`는 노드 UPROPERTY 덤프에서 소유 역참조(`Material`·`Function`), 위치·GUID·`Outputs`(노드 키로 따로 쓴다), 입력 구조체(`FExpressionInput` 계열 — 덤퍼 공통 규칙, 11.7절)를 뺀 것이다. 함수 호출의 `MaterialFunction`, 텍스처·컬렉션 참조는 `{ "objectPath" }`라 의존에 합쳐지고, 노드끼리 참조(이름 있는 reroute의 `Declaration` 등)는 `{ "subobject": 노드 이름 }`이다. Custom 노드의 include 파일은 info `CustomIncludeSkipped`(파일 수)다.
3. **출력 핀.** `UMaterialEditorOnlyData`의 핀 프로퍼티를 선언 순으로 쓴다. 핀 구조체는 `FMaterialInput` 계열이고 `MaterialAttributes`만 `FExpressionInput` 계열이다(EP/Materials/Material.h:157-265, 333-413). `bUseMaterialAttributes`면 `MaterialAttributes` 하나, 아니면 나머지 전부다(29개 — 고정 배열 `CustomizedUVs[8]`은 칸마다 `CustomizedUVs[0]`…`[7]`). 핀은 `{ "name", "from", "mask", "constant" }`이고 `constant`는 `UseConstant`가 켜졌을 때 핀 구조체가 직접 선언한 상수 칸(`Constant` 하나면 그 값, `FVector2MaterialInput`은 `[ConstantX, ConstantY]`), 아니면 `null`이다.
4. **`settings`.** 객체 UPROPERTY 덤프에서 그래프 묶음(`FMaterialExpressionCollection` — 노드·comment 배열)과 핀(`FMaterialInput` 계열)을 뺀다. 노드는 settings 덤프 전에 "펼친 객체"로 표시해서 settings에서 노드를 만나도 `{ "subobject" }`만 쓴다(노드 본문은 `data.nodes`에만 있다). `UMaterialEditorOnlyData` 하위 객체(파라미터 그룹, `SubstrateConversionVersion` 등)는 settings에 남는다. 인스턴스 settings에는 `Parent`, override 배열 9종, `StaticParametersRuntime`, `BasePropertyOverrides`, 에디터 데이터의 static 파라미터가 들어간다.
5. **`resolved`.** `GetAllParametersOfType`을 `EMaterialParameterType` 전 종류(Scalar·Vector·DoubleVector·Texture·TextureCollection·Font·RuntimeVirtualTexture·SparseVolumeTexture·StaticSwitch·ParameterCollection·StaticComponentMask — EP/Materials/MaterialParameters.h:187-205)로 부른 결과다. 항목은 `{ "parameter": FMaterialParameterInfo 덤프(Name·Association·Index), "type": 종류 이름, "value" }`이고 값은 scalar 수, vector·double vector 4개, switch bool, component mask bool 4개, 텍스처 계열·컬렉션 `{ "objectPath" }` 또는 `null`, font `{ "font", "page" }`다. 종류 이름은 플러그인 표로 쓴다(`StaticComponentMask`가 `NumRuntime`과 같은 값이라 UEnum 값 조회는 `NumRuntime`을 낸다). 순서는 `parameter`의 compact JSON bytes 순, 같으면 종류 번호 순이다(8.2절 "파라미터 키 순"의 결정적 형태).
6. **Substrate.** 그래프는 에디터 메모리 그대로 쓴다. Substrate 노드도 다른 노드와 같은 형식으로 덤프하고 플러그인은 따로 진단하지 않는다(8.3절 — 판정은 Rocks 번역). 판단 근거인 `projectSettings.substrate`(bundle.json)와 `SubstrateConversionVersion`(settings의 에디터 데이터)이 묶음에 있다.
7. **자기 시험.** `RocksExport.MaterialExtraction`(commandlet `-SelfTest`에도 포함)이 엔진 `M_Grid`(함수 호출·이름 있는 reroute·파라미터, GUID 겹침 포함), 인스턴스 사슬 `Widget3DPassThrough_Translucent_OneSided`, 컬렉션 참조 머티리얼 `M_RenderNormalizedDepth_01`을 내보내 노드 수·순서·이름·클래스, 모든 입력 연결(대상 이름·출력 번호)을 UE 그래프와, `BaseColor` 핀 연결을 UE 핀과 대조하고, 출력 핀이 개별 핀인지, settings에 노드·핀이 펼쳐지지 않았는지, 함수·컬렉션·부모 참조가 dependencies에 있는지, `resolved` 수가 `GetAllParametersOfType` 전 종류의 합이고 scalar 값이 `GetScalarParameterValue`와 같은지, 추출 전후 덤프가 같은지 본다.
8. **Rocks 쪽 확인.** 받는 MaterialSystem 클래스가 아직 없으므로 선택 시험 `UnrealExchange_ExternalBundle`은 머티리얼 계열 Ready 기록의 구조 불변식만 본다: 노드 모양, 노드 이름 유일, 모든 연결(노드 입력·머티리얼 출력 핀)의 대상 노드가 있고 출력 번호가 그 노드 `outputs` 범위 안, mask가 `null` 또는 0·1 넷, 출력 핀 이름 유일, 함수 호출의 함수와 인스턴스 부모가 dependencies에 있음, settings·data의 모든 `{ "objectPath" }`가 dependencies에 있음, `resolved` 항목 모양. 노드 이름이나 출력 번호를 바꾼 묶음 사본은 이 검사에서 실패한다.
9. **함수 인스턴스 시험 콘텐츠 없음.** 엔진 콘텐츠에 MaterialFunctionInstance가 없고, Python에서는 만들 수 없다(`MaterialFunctionInstanceFactory`의 `InitialParent`가 스크립트에 노출되지 않고 `UMaterialFunctionInstance::SetParent`는 C++ 전용이다). 함수 인스턴스는 settings 덤프만 쓰는 경로라 추출 코드는 MPC와 같다. 실제 콘텐츠로 확인하는 일은 UX4b(번역)에서 시험 콘텐츠를 C++로 만들 때 한다.
10. **수동 검증(2026-11-22).** 엔진 머티리얼 5개 선택(`M_Grid`, `M_RenderNormalizedDepth_01`·`M_RenderMotionVectors` — MPC `MPC_R2T` 참조, `Widget3DPassThrough_Translucent_OneSided`·`TranslucentLaserPointerMaterialInst` — 인스턴스 사슬 2단)을 닫힘과 함께 내보낸 묶음(에셋 46개 — Material 5·MaterialFunction 24·MaterialInstanceConstant 4·MPC 1·텍스처 12)을 두 번 내보내 `createdUtc` 말고 bytes가 같고, 리더가 머티리얼 계열 34개 모두 위 8번을 통과했다(노드 1,046개 연결 전부 해석). 정적 메시·스킨 메시 묶음을 다시 내보내 그 안의 엔진 머티리얼(`WorldGridMaterial`, Interchange 기본 머티리얼·함수, 튜토리얼 캐릭터 머티리얼)도 통과했다. 교환 범위 밖 의존(`TextureRenderTarget2D` 등)은 기존대로 의존에만 남는다.

11. **`resolved` 참조는 Soft(개정 15).** `resolved`의 텍스처·컬렉션·font 참조는 부모 사슬을 풀어 계산한 값이라 덤프 참조와 달리 의존을 새로 만들지 않아야 한다(부모·override가 이미 Hard로 담는다). 그래서 의존에 Soft로 합친다(같은 대상이 Hard로도 있으면 Hard 하나).
