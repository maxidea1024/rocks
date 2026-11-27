## 7. Core 선행 계약

현재 리플렉션은 `ROCKS_CLASS`·`ROCKS_PROPERTY`·enum·struct·배열·Soft 참조를 지원하고(A0-B), 리플렉션 B9가 Name·Text·Optional·Map·Set·클래스 참조·정적 배열·경로의 map key 단계를 더한다([리플렉션 B9](../ReflectionExtensions.ko.md)). 함수 리플렉션·이벤트·편집 메타데이터·변경 통지·동적 속성은 아직 없다. Widget 층·에디터·Sparks는 아래 항목에 의존하므로 Foundation Gate 전에 계약을 확정한다. ID는 초안 1.0과 같으며 다른 문서가 같은 ID로 참조한다.

| ID | 항목 | UE 5.8 대응 | 계약 내용 · 사용처 |
|---|---|---|---|
| CU1 | 편집 메타데이터 어휘 | `UPROPERTY(meta=…)`(`CoreUObject/Public/UObject/ObjectMacros.h:1242-1655`: ClampMin·Delta·EditCondition·EditConditionHides·InlineEditConditionToggle·MetaClass·MultiLine·UIMin·Units·GetOptions 등)와 편집용 속성 지정자(Category·AdvancedDisplay·EditDefaultsOnly·EditInstanceOnly 등, 1086–1218행) · 타입 메타데이터(DisplayName·Category) · 문서 주석 ToolTip · 에디터 전용 저장(`WITH_METADATA`) | 범위(편집 범위와 강제 범위 구분)·단계·단위·enum 표시·비트 플래그·여러 줄·허용 클래스·편집 조건(UD8)·조건 불만족 시 숨김·인라인 토글·후보 목록 함수·고급·숨김·재생성 표지, 편집 위치(UD13)·설명(UD14)·타입 표시 메타(UD15)·분류 계층(UD16) — 7.3절. 버전 있는 manifest로 등록하고 메타 컴파일러가 검증한다. 에디터 전용 값은 Shipping 스키마에서 제외한다. 접근자 지정·결합 표는 실행 스키마이므로 CU1에 속하지 않는다(5.3·5.4절). 사용처: Inspector·Designer·프로젝트 설정 |
| CU2 | 함수 리플렉션 | `UFUNCTION` | `ROCKS_FUNCTION`(설계 [함수 리플렉션](../FunctionReflection.ko.md) — FR1–FR4). 인자·반환 스키마, 순수·const 표지, 호출 thunk. 사용처: 이벤트 대상·바인딩 변환 함수·Sparks 바인딩·Inspector 버튼·에디터 명령·치트 함수(`CheatSet`의 `Cheat`·`Authority` 지정자 — [CVar·치트 명령 설계](../CVarSystem.ko.md) 14.3절). 스크립트 재정의 표지 `ScriptOverridable`과 dispatch, 함수 `DisplayName`·`Category`·`Help`는 7.3절 (b)·(c)·(f) |
| CU3 | 리플렉션된 이벤트 | dynamic multicast delegate | ObjectRuntime `Event<Args...>` 필드(안쪽이 Core `MulticastDelegate`) + `ROCKS_PROPERTY(Params = ...)`, `FieldKind::Event`. 약한 Object 바인딩, 대상 파괴 시 자동 해제, 연결을 WidgetAsset에 저장(템플릿 바인딩 표), 에디터 이벤트 목록 — 설계 [이벤트 리플렉션](../EventReflection.ko.md) |
| CU4 | 속성 경로 | `FPropertyPath` · `FEditPropertyChain` · `FCachedPropertyPath` | (객체 stable ID, FieldId 체인, 배열 index·map key) 한 형식을 Transaction·Inspector·UI 애니메이션 트랙·바인딩에서 공용. map key 단계는 리플렉션 B9 B9f3 |
| CU5 | 변경 통지 | `PreEditChange`·`PostEditChangeChainProperty`·`EPropertyChangeType` + `INotifyFieldValueChanged`·`FFieldId`(`Runtime/FieldNotification`) | (a) 편집 통지: 변경 전·후와 종류(대화형·값 설정·배열 연산·기본값 복원 등), Transaction 병합의 기반. (b) 실행 중 필드 변경 통지: 클래스별 필드 번호, 구독 비트 검사 후 통지(5.3절). 생성 setter에 포함(UD7). Sparks 필드 `Changed` 이벤트로 노출 |
| CU6 | 동적 속성 | `IDetailCustomization` 계열의 사용자 정의 행 · 스크립트 정의 속성 | 형태가 데이터로 정해지는 속성(스크립트 정의 속성·머티리얼 parameter)을 객체가 추가 노출. 저장 형식·Transaction 경로는 정적 속성과 동일 |
| CU7 | 텍스트 위치 변환 | `FString`(UTF-16)·ICU · TSF ACP | UTF-8 byte ↔ UTF-16 ↔ code point 위치 변환은 Core, grapheme·줄 경계는 TextLayout의 ICU adapter(UD3). IME와 편집 Element가 사용 |
| CU8 | 필드 형식 확장 | `FEnumProperty`·`FStructProperty`·`FArrayProperty`·`FTextProperty` 계열 | enum(플래그 포함)·값 구조체(Vector2·Color·여백·anchor·brush·Element 스타일)·Text·에셋 참조·배열. enum·구조체도 `.rocksids.json` 안정 ID 체계에 포함. A0-B와 B9(Text는 B9l)로 대부분 충족 |
| CU9 | 스크립트 구현 interface 표 | `UINTERFACE` | [Sparks 언어 설계](../SparksLanguage/10-EngineBinding.ko.md) 10.3절에서 정의. `NativeOnly` 표지와 C++ interface 조회 offset은 7.3절 (f) |

선행 관계: CU1·CU4는 enum·구조체·배열을 대상으로 하므로 CU8 이후, CU3은 CU2 이후, CU5는 UD7 이후, CU6은 CU4·CU5 이후다. CU7은 독립이다. Sparks는 CU2·CU3·CU5·CU8을 언어 L1의 선행 조건으로 사용한다.

### 7.1 Core 기반 보강

리플렉션 밖에서 Element 층과 Widget 층이 필요로 하는 Core 기능이다.

| ID | 항목 | 사용 층 | 현재 | 필요 계약 |
|---|---|---|---|---|
| CB1 | Object 약한 바인딩 delegate | Widget | Core `Delegate`는 static·lambda·raw·`SharedPtr` 약한 바인딩만 지원(FoundationStatus) | Element 이벤트가 Widget(Object)을 약하게 가리키는 바인딩(UE `CreateUObject` 계열). 대상 파괴 시 호출하지 않는다 |
| CB2 | 공유 포인터 형 변환 | Element | `SharedPtr`에 static·dynamic cast helper 없음 | Element 형식 질의와 정적 변환(UE `StaticCastSharedRef`). RTTI 없이 Element 클래스 정보로 판정 |
| CB3 | 외부 GC root 보관 | Widget | `RootRef` 있음 | 비객체(Element)가 Object를 GC root로 붙드는 수명 규약과 GC 중 해제 금지 검사(5.1절 root 유지 Element) |
| CB4 | 유니코드 경계 | Element(텍스트) | Core 유니코드는 정규화·대소문자 접기만 | grapheme·단어·줄 경계와 BiDi를 TextLayout의 ICU adapter로 제공(UD3). Core 유니코드 표와 기능이 겹치는 부분(정규화 등)은 Core를 그대로 쓴다 |

### 7.2 플랫폼 보강(PlatformWindow)

| ID | 항목 | UE 5.8 대응 |
|---|---|---|
| PW1 | 다중 창(생성·소유 관계·종류·최상위·테두리 유무·활성화) | `GenericApplication::MakeWindow` · `FGenericWindowDefinition` |
| PW2 | 모니터별 DPI와 변경 통지 | `GetDPIScaleFactorAtPoint` · `HandleDPIScaleChanged` |
| PW3 | IME(TSF·IMM, `NSTextInputClient`) | `FWindowsTextInputMethodSystem` · `FTextStoreACP` · `FCocoaTextView` |
| PW4 | clipboard · cursor 모양 · 커서 아래 창 | `ClipboardCopy`/`Paste` · `ICursor` · `GetWindowUnderCursor` |
| PW5 | 창별 입력 이벤트·modifier 상태·고정밀 마우스·capture | `FGenericApplicationMessageHandler` · `SetHighPrecisionMouseMode` · `SetCapture` |
| PW6 | OS drag/drop | `OnDragEnter`·`OnDragDrop` 계열 |
| PW7 | 자체 제목 표시줄 영역 판정 | `GetWindowZoneForPoint` |
| PW8 | 접근성 provider(UIA·NSAccessibility) | `FWindowsUIAManager` · `MacAccessibilityElement` 계열 |

### 7.3 CU1·CU2·CU9 보강: 편집 위치·도움말·타입 표시·분류 계층·스크립트 재정의

UE 5.8 `Templates`의 C++ 템플릿(헤더 119개, `.cpp` 119개)과 대조해 CU1·CU2·CU9 계약에서 빠진 것을 더한다(초안 2.1). 템플릿 사용 수는 `UPROPERTY` 461회(그중 `EditDefaultsOnly`·`EditInstanceOnly`·`VisibleDefaultsOnly` 13회, `VisibleAnywhere` 78회), `/** */` 문서 주석 1374개, `USTRUCT(meta=(DisplayName, Category))` 14회, `|` 계층 Category 97회, `BlueprintImplementableEvent` 38회, 엔진 `BlueprintNativeEvent`의 `_Implementation` 재정의 11회, `UINTERFACE(NotBlueprintable)` 4회다. 교차 검증(12절 초안 2.1 기록)으로 타입 단위 편집 메타와 남은 공백 목록(g)까지 넓혔다.

**공통 규칙.** CU1과 같이 버전 있는 manifest에 등록하고 메타 컴파일러가 검증한다.
- **저장 위치:** 이 절의 편집 값은 **편집 메타데이터 블록**에 둔다. `FieldSchema`·`TypeSchema`는 블록 포인터 하나를 가지며, `ROCKS_WITH_EDITOR_DATA=0` 빌드([빌드 타깃](../BuildTargets/README.ko.md) 3.2·3.4절)에서는 생성 코드가 블록을 만들지 않아 null이다. UE `WITH_METADATA`(= `WITH_EDITORONLY_DATA`, `Runtime/Core/Public/Misc/CoreMiscDefines.h:31`)와 같은 타깃 축이며, CU1 표의 "Shipping 스키마에서 제외"도 이 뜻으로 읽는다.
- **실행 스키마에 남는 것:** 기존 `FieldSchema`의 `displayName`은 게임 빌드의 콘솔 설정 도움말이 쓰므로(`Core/Source/Runtime/Config/Private/ConsoleSettings.cpp:665-667`) 실행 스키마로 남긴다. `category`는 실행 중 소비처가 없으므로 편집 메타데이터 블록으로 옮긴다.
- **값 문자열:** 이 절에서 새로 받는 표시 문자열(`Help`, 타입·함수 `DisplayName`·`Category`)은 NUL·제어 문자 없는 유효 UTF-8이고 이름 붙은 길이 한도를 따른다(각 항). 기존 필드 `DisplayName`·`Category`는 지금 중간 NUL을 허용하고 한도가 없다([ReflectionMetadata](../../Implementation/ReflectionMetadata.ko.md) 182행). CU1 구현 때 같은 규칙으로 좁힌다(하위 호환은 두지 않는다).
- **언어:** 엔진 저장소(Core·Engine·엔진 플러그인)의 `Help`·`DisplayName`·`Category`는 영어다. 실행 중 사용자에게 보이는 문구이기 때문이며 [AppInfo·CommandLine](../AppInfoCommandLine/README.ko.md) D18(도움말은 영어)과 같다. 정책 검사로 막는다. 게임 프로젝트와 시험 fixture(모듈 `Tests/`·시험 전용 fixture 모듈)는 제한하지 않는다. 지금 이스케이프 시험용 fixture(`ReflectionTestFixtures/ReflectionFixture.h` 60·79·82행)의 NUL·제어 문자 값은 새 문자열 규칙에 맞게 바꾸고(이스케이프 시험은 따옴표·역슬래시·비ASCII로), 문자 규칙 위반은 음성 시험으로 옮긴다.
- **지역화:** 원문 문자열을 Localization 수집 대상으로 두고, 키는 (TypeId, FieldId·FunctionId·enum 값, 항목 종류)로 만든다. UE는 `UObjectDisplayNames`·`UObjectToolTips` 네임스페이스에 전체 경로 키를 쓴다(`Runtime/CoreUObject/Private/UObject/Class.cpp:303-316, 346-359`). Rocks는 경로 대신 안정 ID를 쓴다. 수집·번역 도구는 Localization 단계 일이다.

**(a) Inspector 노출과 편집 위치(UD13).**

| Rocks `ROCKS_PROPERTY` | UE 5.8 지정자 | UE 플래그(`EpicGames.UHT/Specifiers/UhtPropertyMemberSpecifiers.cs:21-91`) |
|---|---|---|
| (없음) | (편집 지정자 없음) | `CPF_Edit` 없음 — Inspector에 나오지 않는다 |
| `Edit` | `EditAnywhere` | `CPF_Edit` |
| `Edit, EditScope = "Archetype"` | `EditDefaultsOnly` | `CPF_Edit \| CPF_DisableEditOnInstance` |
| `Edit, EditScope = "Instance"` | `EditInstanceOnly` | `CPF_Edit \| CPF_DisableEditOnTemplate` |
| `DisplayOnly` | `VisibleAnywhere` | `CPF_Edit \| CPF_EditConst` |
| `DisplayOnly, EditScope = "Archetype"` | `VisibleDefaultsOnly` | `CPF_Edit \| CPF_EditConst \| CPF_DisableEditOnInstance` |
| `DisplayOnly, EditScope = "Instance"` | `VisibleInstanceOnly` | `CPF_Edit \| CPF_EditConst \| CPF_DisableEditOnTemplate` |

- **노출은 명시한다.** UE와 같이 `Edit`·`DisplayOnly`가 없는 리플렉션 필드는 Inspector에 나오지 않는다. [Sparks](../SparksLanguage/10-EngineBinding.ko.md) 10.1절 예시의 `ROCKS_PROPERTY(Edit, …)`가 이 지정자다. `Edit`과 `DisplayOnly`를 함께 쓰거나, 둘 다 없이 `EditScope`를 쓰면 메타 컴파일러 오류다(UE도 편집·보이기 지정자를 둘 이상 쓰면 오류 — 같은 파일 24–27행).
- **`EditScope`는 값 하나다**(`"Archetype"`·`"Instance"`). 배타적인 두 지정자를 두지 않아 함께 쓰는 경우가 생기지 않고, `EditorOnly`·`WorldOnly`·`DevelopmentOnly` 같은 "존재 범위"의 `…Only` 이름과 헷갈리지 않는다.
- **`DisplayOnly`와 `ReadOnly`는 다르다.** `DisplayOnly`는 Inspector 표시 전용(UE `CPF_EditConst`)이고 코드·스크립트·바인딩·애니메이션 트랙의 쓰기를 막지 않는다. `ReadOnly`는 경로·리플렉션 쓰기를 거부하는 실행 의미다([리플렉션 기반](../ReflectionFoundations.ko.md) 954행, staged 쓰기만 허용). Inspector 규칙: `Edit`이고 `ReadOnly`인 필드는 생성 setter(5.3절 `Apply`, UD7)가 있으면 setter를 거쳐 편집하고(예: 장부를 거쳐야 하는 `relativeTransform`), 없으면 표시 전용으로 보인다.
- **`EditScope`를 붙일 수 있는 곳:** 원형과 인스턴스가 나뉘는 타입, 곧 `WorldOnly` 클래스(Actor·Component — prefab 노드)와 UI Widget 클래스(UiWidget이 기반 클래스를 등록)의 필드, 그리고 struct 멤버다. 그 밖의 클래스(자산 전용 등)의 필드에 붙이면 메타 컴파일러 오류다. struct 멤버의 `EditScope`는 그 struct가 자산 객체 안에서 쓰일 때는 문맥이 없으므로 무시한다(두 위치 모두 편집할 수 있다).
- **중첩 전파:** 실효 위치는 객체에서 그 필드까지 경로에 있는 `EditScope`의 교집합이다(UE도 중첩 노드마다 자기 플래그로 다시 판정한다 — `Editor/PropertyEditor/Private/PropertyEditorHelpers.cpp:407-423`). 정적으로 알 수 있는 모순 조합(바깥 `"Archetype"` 필드의 struct 형식 안에 `"Instance"` 멤버, 그 반대)은 메타 컴파일러 오류다. Atomic struct 멤버에는 `EditScope`를 붙일 수 없다(통째 단위라 멤버만 따로 막을 수 없다). 통째 단위 override(Array·StaticArray·Optional·InstancedStruct — [Prefab](../Prefab.ko.md) 3.5절)의 데이터 검사는 바깥 필드의 실효 위치로만 판정하고, 원소 struct 멤버의 `EditScope`는 Inspector 표시에만 쓴다.

**편집 문맥.** UE는 Details 뷰 설정과 객체의 `IsTemplate()`을 함께 쓴다. 레벨 편집기 Actor Details는 기본값 전용을 숨기고(`Editor/LevelEditor/Private/SActorDetails.cpp:455` `DefaultsOnlyVisibility = Hide`), Blueprint 편집기는 선택 객체가 모두 템플릿(클래스 기본값·SCS 컴포넌트 템플릿)이면 인스턴스 전용을 숨긴다(`Editor/Kismet/Private/SKismetInspector.cpp:845-922`). UMG Designer는 트리의 자식 Widget을 인스턴스로, 편집 중인 루트 UserWidget을 원형으로 다룬다(`Editor/UMGEditor/Private/Details/SWidgetDetailsView.cpp:386-401`). 그 밖의 자산 편집기는 기본값 `Show`(`Editor/PropertyEditor/Public/DetailsViewArgs.h:158`)로 둘 다 보여 주고, `Automatic` 모드는 클래스 기본값 객체일 때만 기본값 전용을 보인다(같은 파일 20–27행). Rocks는 **값이 저장되는 곳으로 문맥을 정한다.**

| 문맥 | 대상(저장 위치) | `"Archetype"` | `"Instance"` |
|---|---|---|---|
| 원형 | `PrefabAsset`에 저장되는 모든 것: prefab·variant 편집 World의 노드, variant `overrides`, 바깥 prefab이 중첩 노드에 둔 override([Prefab](../Prefab.ko.md) 3.6·3.8절). WidgetAsset Designer의 루트 UserWidget | 편집 | 숨김 |
| 배치 인스턴스 | 레벨에 저장되는 것: `PrefabPlacement.overrides`, 레벨에 직접 배치한 객체의 Record. WidgetAsset Designer 트리의 자식 Widget·중첩 UserWidget(그 WidgetAsset 안의 배치 값) | 숨김 | 편집 |
| 클래스 기본값 보기 | 클래스 레코드·subobject 기본값 레코드 | 표시 전용. 이 값은 C++ 생성자·Sparks 필드 초기값에서만 나오고 Inspector 편집 경로가 없다([Prefab](../Prefab.ko.md) 3.1·3.2절, RCM 76행) | 숨김 |

독립 자산 객체는 위 문맥에 속하지 않는다. 그 클래스 필드에는 `EditScope`를 붙일 수 없고, 그 안에 쓰인 struct 멤버의 `EditScope`는 무시한다.

**강제 위치.** UE는 Inspector 말고도 편집기 쓰기 경로에서 막는다. 편집기 스크립팅 `SetEditorProperty`는 `CanSetPropertyValue`로 `CannotEditInstance`·`CannotEditTemplate`를 거부하고(`Runtime/CoreUObject/Private/UObject/PropertyAccessUtil.cpp:723-751`, `Runtime/Engine/Private/KismetSystemLibrary.cpp:3507`), SpawnActorFromClass 노드의 기반은 `DisableEditOnInstance` 필드를 spawn 핀으로 내지 않으며(`Editor/BlueprintGraph/Private/K2Node_ConstructObjectFromClass.cpp:134-139`), UHT는 `ExposeOnSpawn`과 함께 쓰면 경고한다(`EpicGames.UHT/Types/UhtProperty.cs:2411-2416`). Rocks는 세 곳에서 막는다.
1. **편집기 쓰기 경로:** Inspector `FieldHandle`과 편집기 Transaction은 문맥을 받고, 문맥 밖 필드 쓰기를 `PrefabOverridePathInvalid`(InvalidArgument)로 거부한다. [Prefab](../Prefab.ko.md) 3.11절 이 진단의 원인에 "편집 위치 위반"을 더한다. 편집기 스크립트(Sparks `@editorButton` 등)도 이 경로를 쓴다.
2. **저장 검증(방어선):** 외부 편집·병합·스키마 변경으로 들어온 위반을 잡는다. 원형 문맥 저장물에 `"Instance"` 필드가 클래스 레코드와 다른 값으로 있거나, 배치 인스턴스 문맥 저장물에 `"Archetype"` 필드가 원형과 다른 값으로 있으면 진단 `PrefabEditScopeViolation`이다. 처리는 Prefab 3.7절 충돌과 같다(편집기: 진단만 하고 값은 적용, cooked: InvalidArgument). 이미 override가 있는 필드가 나중에 `EditScope`를 얻은 경우도 같은 규칙이라 값을 잃지 않고 편집기에서 지우거나 원형으로 옮기게 한다.
3. **spawn 인자:** `"Archetype"` 필드는 spawn 인자(Sparks 생성 인자, Prefab 3.1절 적용 순서의 마지막 층)로 쓸 수 없다. Sparks 컴파일러와 C++ spawn 인자 검증이 오류로 낸다.

실행 중 게임 코드·스크립트의 일반 쓰기는 막지 않는다(UE와 같다).

**원형 적용과 variant 만들기.** "원형에 적용"(Prefab 3.7절 Apply to source)과 "variant 만들기"(3.13절)는 `"Instance"` 필드 override를 옮기지 않으며, 옮기지 않은 항목은 인스턴스 집합에 그대로 남는다. UE는 인스턴스 변경을 Blueprint에 적용할 때(`ApplyInstanceChangesToBlueprint`, `Editor/UnrealEd/Private/Kismet2/Kismet2.cpp:2177`, 플래그 2210행), Actor에서 Blueprint를 만들 때(`CreateBlueprintFromActor`, 같은 파일 1553행), 컴포넌트 속성을 옮길 때(`Editor/UnrealEd/Private/Editor.cpp:1910`) `SkipInstanceOnlyProperties`로 뺀다.

Sparks `@edit` 필드의 대응 표기(`@edit(Scope: Archetype)` 등)는 [Sparks 언어 설계](../SparksLanguage/README.ko.md)가 정한다.

**(b) 도움말(UD14).** 설명은 `Help = "…"`로 PROPERTY·CLASS·STRUCT·ENUM·ENUMERATOR·FUNCTION(CU2, 인자 포함)에 둔다. 명령줄 바인더의 `ROCKS_PROPERTY(Help=…)`([AppInfo·CommandLine](../AppInfoCommandLine/README.ko.md) 13.2절), 계획된 `ROCKS_FUNCTION(…, Help=…)`([CVar](../CVarSystem.ko.md) 14.3절), CVar `.Help()`와 같은 키로 하나로 통일한다. Inspector tooltip, Designer palette, 자산·클래스 선택 목록, Sparks 언어 서버 hover(native API 설명)가 쓴다.
- 길이 한도는 `MaxHelpBytes`(1024 — CVar `MaxVarHelpBytes`와 같은 값)다.
- `Help`가 없으면 tooltip은 표시 이름을 쓴다(UE도 같다 — `Runtime/CoreUObject/Private/UObject/Class.cpp:323-363`). 선택 목록처럼 짧은 설명이 필요한 곳은 `Help`의 첫 줄(첫 줄바꿈 앞)을 쓴다. UE `ShortToolTip` 같은 별도 키는 두지 않는다.
- 설정 struct 필드([설정 시스템](../ConfigSystem.ko.md) 7절)와 명령줄 바인더의 `Help`, 치트 함수(`ROCKS_FUNCTION(Cheat, …)` — [함수 리플렉션](../FunctionReflection.ko.md) FD12)의 `Help`는 게임 빌드의 콘솔·명령줄 도움말이 쓰므로 실행 스키마에도 남는다. 그 밖의 `Help`는 편집 메타데이터 블록에만 있다.
- **C++ 문서 주석은 가져가지 않는다.** UE UHT는 선언 앞 주석(`/** */`와 `//` 모두)을 `Comment`·`ToolTip` 메타데이터로 자동 추출하고 명시 `ToolTip`이 있으면 건너뛴다(`EpicGames.UHT/Parsers/UhtParsingScope.cs:172-220`). 함수 주석의 `@param`은 편집기가 핀 tooltip으로 쓴다(`Editor/BlueprintGraph/Private/K2Node_CallFunction.cpp:1681` `GeneratePinTooltipFromFunction`). Rocks C++는 명시한 `Help`만 쓴다. 근거: (1) 저장소 규칙상 손으로 쓴 소스 주석은 한국어이고 표시 문구는 영어다(D18과 같은 이유). (2) 공개 API 주석은 프로그래머용(목적·사용 예·인자·오류·수명)이라 편집자용 tooltip과 독자·분량이 다르다. (3) 주석 수정이 생성 메타데이터를 바꿔 재빌드와 스키마 차이를 만든다. C++ 문서 주석은 프로그래머용 hover(C++ 언어 서버)가 그대로 쓴다.
- **Sparks는 다르다.** Sparks는 `///` 문서 주석이 편집기 tooltip이다([Sparks](../SparksLanguage/10-EngineBinding.ko.md) 10.1절 표). Sparks 문서 주석은 언어가 정한 단일 형식이고 스크립트 작성자가 편집자에게 보이는 설명으로 쓰는 것이 언어 설계의 전제이므로 이 차이는 의도한 것이다. Sparks 컴파일러는 그 주석을 같은 `Help` 메타데이터로 기록한다.

**(c) 타입·함수 표시 메타(UD15).** `DisplayName`·`Category`·`Help`를 CLASS·STRUCT·ENUM과 FUNCTION(CU2 — Inspector 버튼·에디터 명령 목록)에도 둔다. 타입 선택 UI(클래스 선택, palette, 노드 목록)가 쓴다. 예를 들어 StateTree 노드 선택 목록은 struct 메타데이터 `Category`를 `|`로 나눠 분류한다(`Plugins/Runtime/StateTree/Source/StateTreeEditorModule/Private/Customizations/Widgets/SStateTreeNodeTypePicker.cpp:176`).
- `DisplayName`이 없으면 필드 `DisplayName`이 없을 때와 같은 기본 표시 규칙을 타입 도구 이름(enum은 C++ 이름)에 적용한다. UE는 메타데이터가 없으면 이름을 표시 문자열로 바꾼다(`Runtime/CoreUObject/Private/UObject/Class.cpp:303-313`, `FName::NameToDisplayString`). Rocks 기본 표시 규칙은 CU1 구현에서 하나로 정해 필드·타입·함수가 같이 쓴다.
- CLASS·STRUCT의 `Name`(타입 도구 이름, [리플렉션 B9](../ReflectionExtensions.ko.md) 5.2절)과 ENUM의 `ToolPrefix`는 저장·조회용 식별자이고 `DisplayName`은 표시용이다. 둘은 독립이다.
- `Category`·`DisplayName`·`Help`는 파생 타입으로 상속하지 않는다(UE와 같다). Sparks 타입의 대응 표기(attribute 인자)는 Sparks 문서가 정한다.

**(d) 분류 계층(UD16).** 필드·타입·함수 `Category`의 `|`는 계층 구분자다(`"Input|Touch Controls"`). UE PropertyEditor의 구분자와 같다(`Editor/PropertyEditor/Private/PropertyNode.h:97` `CategoryDelimiterChar = '|'`). 메타 컴파일러 검사:
- 빈 마디(`"A||B"`, 앞뒤 `|`)와 마디 앞뒤 공백은 오류다(UE StateTree 선택 목록은 공백을 정리하지만 Rocks는 원문을 정규형으로 강제한다).
- 한도: 마디 수 `MaxCategoryDepth`(8), 마디 `MaxCategorySegmentBytes`(64), 전체 `MaxCategoryBytes`(256).
- 대소문자만 다른 범주 경로는 오류다. 범위는 한 클래스의 필드 전체(다른 모듈에서 import한 기반 클래스 필드 포함)다. UE는 FName이라 합치지만 Rocks는 더 엄격하게 거부한다.
- 타입 `Category`의 모듈 사이 충돌은 메타 컴파일러가 볼 수 없으므로, 편집기가 대소문자를 무시하고 합쳐 먼저 등록된 철자로 표시하고 진단을 남긴다.
- 표시 순서는 기반 클래스부터 선언 순서로 처음 나온 순서다. Rocks 고유 규칙이다. UE는 필드 반복이 파생 클래스부터 돌고 Blueprint에 보이는 범주를 앞쪽에 끼워 넣는다(`Editor/PropertyEditor/Private/ObjectPropertyNode.cpp:524-571`, 특히 562–570행). Rocks는 Blueprint 구분이 없고, 기반 클래스 범주가 먼저 오는 편이 파생마다 순서가 흔들리지 않는다. 앞으로 올릴 범주는 (e)의 `PrioritizeCategories`로 정한다.

**(e) 타입 단위 편집 메타(UD15).** UE `UCLASS` 지정자 중 Inspector·선택 목록 동작을 정하는 것이다. UHT의 부모 목록 처리는 지정자마다 다르다(`EpicGames.UHT/Types/UhtClass.cs:919-941, 1349-1380`). Rocks도 같은 규칙을 쓴다.

| UE 5.8 | Rocks CLASS 지정자 | 의미 |
|---|---|---|
| `ClassGroup` | 타입 `Category`로 대체 | 선택 목록 분류 |
| `HideCategories`·`ShowCategories` | 같은 이름(값은 `\|`가 아닌 `;`로 나열한 범주 경로 목록 — `\|`는 경로 안 구분자다) | 상속 범주 숨김·다시 보임, 부모 목록과 병합 |
| `AutoExpandCategories`·`AutoCollapseCategories` | 같은 이름, 같은 값 형식 | Inspector 초기 펼침·접힘. 부모 목록과 병합하되, 자식의 펼침 목록에 있는 범주는 상속한 접힘 목록에서 빼고 그 반대도 같다(`UhtClass.cs:1361-1380`) |
| `PrioritizeCategories` | 같은 이름, 같은 값 형식 | 범주 표시 순서를 앞으로 올린다. 병합하지 않고, 자식 목록이 비었을 때만 부모 목록을 물려받는다(`UhtClass.cs:924-927`) |
| `HideDropdown` | `HiddenInPicker` | 클래스 선택 목록에서 뺀다(`Abstract`는 이미 빠진다) |
| `NotPlaceable` | `NotPlaceable` | 레벨 배치 목록에서 뺀다. `WorldOnly` 클래스에만 쓰며 파생에 상속한다(UE와 같다). 레벨 스크립트 `LevelScript`가 쓴다([레벨 스크립트](../LevelScript.ko.md) LD3 — Outliner에서 "레벨 스크립트" 항목으로만 보이는 것은 새 지정자가 아니라 편집기 규칙이다). 실행 중 spawn은 막지 않는다 |
| `AdvancedClassDisplay` | 두지 않는다 | 기반 클래스 필드 전체를 고급 표시로 보내는 거친 수단이다. 필드 단위 고급 표지(CU1)로 충분하다 |

**(f) 스크립트 재정의 함수와 interface 구현 표지(CU2·CU9 보강, UD17).** UE는 C++ 함수를 Blueprint가 재정의하게 하는 두 지정자를 둔다(`EpicGames.UHT/Specifiers/UhtFunctionSpecifiers.cs:60, 88`).
- `BlueprintImplementableEvent`: C++ 본문 없이 스크립트만 구현한다.
- `BlueprintNativeEvent`: C++ 기본 구현(`_Implementation`)이 있고 스크립트가 재정의한다.
- 생성 thunk는 `ProcessEvent`로 스크립트 재정의를 찾고, 없으면 `_Implementation`을 부른다(`EpicGames.UHT/Exporters/CodeGen/UhtHeaderCodeGeneratorCppFile.cs:2429` 이후, `_Implementation` 호출은 2480행).

Sparks는 엔진 기반 타입의 재정의 가능 함수를 `override fn`으로 재정의한다고만 정했다(Sparks 3.2절·SD27). C++ 쪽 표시와 dispatch 계약은 비어 있었다. 다음과 같이 정한다.

- **표시:** `ScriptExtensible` 클래스의 `virtual` 멤버 함수에 `ROCKS_FUNCTION(ScriptOverridable)`을 붙인다.
  - 함수는 `final`이 아니어야 하고, 인자·반환이 Sparks 바인딩 가능한 형식이어야 한다(메타 컴파일러 검사).
  - 본문이 있으면 UE NativeEvent, 순수 가상(`= 0`)이면 UE ImplementableEvent에 해당한다.
  - 순수 가상은 `Abstract` 클래스에만 허용하고, 반환 형식은 `void`나 `Result<T>`여야 한다. 그 클래스를 상속하는 구체 Sparks 클래스가 재정의하지 않으면 Sparks 컴파일 오류다.
  - UE `_Implementation` 같은 이름 규칙은 두지 않는다. C++ 기본 구현은 그 가상 함수 자체다.
  - `ScriptExtensible` 클래스는 `final`일 수 없다(메타 컴파일러 오류).
- **dispatch — 스크립트 파생 대리 클래스:** 메타 컴파일러는 `ScriptOverridable` 함수가 있는(상속받은 것 포함) `ScriptExtensible` 클래스 X마다 생성 C++ 클래스 `ScriptProxy<X>`를 만든다.
  - 대리 클래스는 X를 상속하고 `using X::X`로 생성자를 그대로 받는다. 각 `ScriptOverridable` 함수를 재정의해, 인스턴스의 Sparks 타입에 `override fn`이 있으면 VM을 부르고 없으면 `X::`의 구현을 부른다.
  - **대리 클래스는 등록하지 않는 구현 세부다.** TypeId·필드·생성 메타데이터가 없고, 필드를 더하지 않으며, `sizeof`·정렬이 X와 같다(생성 `static_assert`). 그래서 native 파생 클래스의 저장 금지 규칙(`completeFieldLayout=false` — `Core/Source/Runtime/ObjectRuntime/Public/Object/NativeClass.h:22-24`, [Prefab](../Prefab.ko.md) 3.11절 `PrefabUnsaveableNode`)과 무관하다.
  - 인스턴스의 TypeId는 Sparks 타입이다(Sparks 12절 두 번째 발급자). 저장·로드는 Sparks 타입 규칙을 따른다.
  - 레지스트리가 Sparks 타입의 인스턴스를 만들 때는 X의 생성 함수 대신 대리의 생성 함수를 쓴다. 할당 크기는 X와 같다. 생성자 인자(Allocator 등)와 생성 문맥(`ConstructionContext`)은 X와 같다.
  - 클래스 기본값 레코드는 지금처럼 X의 생성자로 만든다. 대리는 vtable만 다르므로 레코드가 같다.
  - X를 상속한 C++ 클래스 Y가 `ScriptExtensible`이면 Y도 자기 대리 `ScriptProxy<Y>`를 가진다. Y를 기반으로 한 Sparks 타입은 그것을 쓴다.
  - C++ 호출자는 보통 가상 호출을 쓰고, C++ 파생 클래스의 재정의도 그대로 동작한다.
- **호출 문맥:** VM 진입은 Game lane에서만 허용한다(Sparks 10.1절 규칙). 다른 lane에서 스크립트 재정의가 있는 함수를 부르면 계약 위반 진단이다.
  - 본문이 있는 함수는 기반 구현으로 대체한다.
  - 순수 가상 함수는 `void`면 아무것도 하지 않고, `Result<T>`면 계약 위반 오류를 돌려준다.
  - 스크립트 실행 오류도 같은 규칙으로 대체하고 진단한다(Sparks native 경계 규칙).
- **interface 구현 표지:** C++ interface(`ROCKS_INTERFACE`, CU9)는 기본으로 Sparks 클래스가 구현할 수 있다. `ROCKS_INTERFACE(NativeOnly)`(UE `UINTERFACE(NotBlueprintable)`)이면 Sparks 구현이 컴파일 오류다.
- **C++ interface 상속과 조회:**
  - UE는 C++ 다중 상속으로 interface를 구현하고, 클래스의 interface 표에 포인터 offset을 둔다(`Runtime/CoreUObject/Public/UObject/Class.h:3788` `FImplementedInterface`). `Cast<IFoo>(UObject*)`가 이것으로 조회한다(`Runtime/CoreUObject/Public/Templates/Casts.h:146`).
  - 지금 메타 컴파일러는 리플렉션 클래스에 public 비가상 기반을 정확히 하나만 허용한다(`Programs/Rocks.MetaCompiler/ClangAstParser.cs:786-789`, NativeClass도 단일 상속). 이 제약을 다음과 같이 넓힌다(CU9 구현): 리플렉션 데이터 기반은 지금처럼 하나이고, 그 밖에 `ROCKS_INTERFACE` 타입을 public 비가상 기반으로 여러 개 둘 수 있다.
  - `ROCKS_INTERFACE` 타입은 데이터 멤버가 없고(가상 함수와 비가상 inline 함수만), 리플렉션 데이터 기반이 될 수 없다.
  - 생성 코드는 등록 때 각 interface 기반의 `this` 조정 offset(`static_cast<IFoo*>`의 주소 차이)을 계산해 CU9 `InterfaceImpl`에 넣는다.
  - 새 API `ObjectCast<IFoo>(object)`(CU9와 같이 신설)는 RTTI 없이 이 표로 interface 포인터를 돌려준다.
  - Sparks 클래스가 구현한 interface는 C++ 하위 객체가 없으므로 `ObjectCast`가 null을 돌려준다. 이런 객체에는 slot 표(FunctionId) 기반 호출(`ROCKS_FUNCTION` 메서드만)을 쓴다.
  - interface의 리플렉션하지 않는 C++ 메서드(raw 포인터 인자 등)는 slot 표에 들어가지 않고 C++ 호출(`ObjectCast`)로만 쓴다.

**(g) 대조에서 나온 남은 공백.** 이 절에서 결정하지 않고 주인 문서·단계로 넘기는 항목이다. 놓치지 않도록 여기 기록한다.

| UE 요소(템플릿 사용) | 판단 | 주인 |
|---|---|---|
| 클래스 단위 config 속성 `UCLASS(config=Game)` + `UPROPERTY(Config)`(10개 클래스, 모두 `bForceTouchControls`) | **채택하지 않는다.** 설정 값은 설정 struct(`Settings(Branch, Section)`)로 읽는다. 클래스 레코드가 설정 파일에 따라 달라지면 원형 차이 저장의 기준이 흔들린다(RCM 74행이 CDO config 로드를 두지 않은 것과 같은 이유) | [설정 시스템](../ConfigSystem.ko.md) 7절 |
| 엔진 태그 사전(GameplayTags, `Config/Tags/*.ini`) | Sparks `Tag`·`TagSet`과 저장 형식은 있다(RX 1417행). 사전의 정의 원천(설정 파일·C++ 선언), 계층 매칭, 등록 API의 스펙이 없다. 별도 설계가 필요하다 | 전체 계획 후속(태그 사전 설계 신설) |
| 동작이 있는 다형 struct(StateTree 노드 struct 17개의 virtual, `FTableRowBase` virtual) | 리플렉션 struct의 virtual은 RMC2301로 금지이고 다형성은 InstancedStruct(데이터)다. 동작 dispatch의 대체 경로는 StateTree 설계 때 정한다(후보: 노드를 Object로, 또는 InstancedStruct + 함수 표) | StateTree 설계(전체 계획 P2) |
| Category로 바인딩 역할을 정하는 용도(StateTree `Context`·`Input`·`Output`·`Parameter` — `Plugins/Runtime/StateTree/Source/StateTreeModule/Private/StateTreePropertyBindings.cpp:35-49`) | Rocks `Category`는 표시 전용이다. 역할은 별도 지정자로 정한다 | StateTree 설계 |
| `FDataTableRowHandle`(테이블 + 행 이름) | Rocks `Row<T>`는 행 이름만 저장한다(RX 1417행). 같은 행 타입의 테이블이 여럿일 때 테이블을 지정하는 형식을 정해야 한다 | DataTable 설계(전체 계획 2101행 부근) |
| C++ 지역화 리터럴 `LOCTEXT`와 소스 수집 | `Text`·`Catalog`·Sparks `Loc()`는 있다. C++ 리터럴 매크로와 수집 도구는 없다 | Localization 단계 |
| `AllowPrivateAccess`(69회) | 두지 않는다. Rocks의 스크립트·편집 노출은 지정자(`Edit`·`ScriptWritable` 등)로 정하고, C++ 접근 수준과 무관하다(생성 코드가 private 멤버에 닿는다) | — |
| `MinimalAPI`·`*_API` export 매크로 | Editor 타깃은 UE와 같이 모듈식으로 링크한다(BD17). 모듈 API 매크로는 `ROCKS_<MODULE>_API`이고, 리플렉션 타입 정보는 생성 코드가 항상 export하므로 `MinimalAPI` 지정자는 두지 않는다 | [빌드 타깃](../BuildTargets/03-Design/03.08-LinkType.ko.md) 3.8절 BD27·BD28 |

**선행·단계.** (a)–(e)는 CU1과 같이 UT0에서 구현한다. (a)의 편집기 쓰기 경로 거부·저장 검증·원형 적용 제외는 Prefab 구현 단계(PF)와 Inspector(UT4)에서 넣는다. (b)·(c)의 함수 항목과 (f)는 CU2·CU9와 같이 넣는다.
