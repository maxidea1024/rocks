## 5. Widget 층

### 5.1 Widget과 Element의 관계

| Rocks | UE 5.8 대응 | 계약 |
|---|---|---|
| `Widget` : `rocks::Object` | `UVisual` · `UWidget`(`UMG/Public/Components/Widget.h`) | 기반은 Element를 **약하게** 참조한다(`Widget.h:1195-1201`). 파생 Widget은 자기 형식의 Element를 강한 포인터로 보관하고 해제 함수에서 끊는다(UE `TextBlock.h:306`, `TextBlock.cpp:46-51`) |
| Element 생성 | `TakeWidget` · `RebuildWidget` · `SynchronizeProperties` · `OnWidgetRebuilt` | Element가 없을 때만 생성하고, 새로 생성한 경우 저장된 속성 전체를 Element에 반영한 뒤 재구성 통지(`Widget.cpp:962-1099`). 반영 함수의 부모 호출 누락은 검사한다 |
| Element 해제 | `ReleaseSlateResources`(`Visual.cpp:20-26`) | Object 파괴 시작 시에는 자기 Element만, root 해제 시에는 하위까지 끊는다 |
| root 유지 Element | `SObjectWidget`(`UMG/Public/Slate/SObjectWidget.h:30`) | UserWidget의 Element 루트를 감싸며 UserWidget에 대한 GC root(Rocks `RootRef`)를 가진다. Element 트리가 살아 있는 동안 UserWidget이 살아 있고, 이 Element 파괴 시 해체 통지 → 하위 Element 해제를 GC 밖에서 실행한다(`SObjectWidget.cpp:42-106`) |

이 관계로 UserWidget의 구성(construct)·해체(destruct) 통지는 Object 수명이 아니라 Element 트리의 존재를 따른다(UE와 같다). 일반 Widget은 WidgetTree → slot → 내용 Widget의 속성 참조로 유지된다.

### 5.2 slot 객체

배치 속성(여백·크기 규칙·정렬·anchor·offset·z 순서)은 부모·자식 관계의 속성이므로 slot 객체에 둔다(UD2 확정). `PanelWidget`은 `WidgetSlot` 배열을 소유하고 패널마다 slot 형식을 선언한다(`PanelWidget.h:22, 167-190`). slot 객체는 Element 패널의 slot을 비소유 포인터로 보관하고, setter가 Element slot에 값을 바로 반영한다(`HorizontalBoxSlot.cpp:30-58`). 같은 자식을 다른 부모로 옮기면 slot 객체가 교체되어 부모별 속성만 Inspector에 표시된다. Designer 이동·drag 미리보기 hook과 템플릿 동기화도 slot이 제공한다(`PanelSlot.h:51-61`).

### 5.3 속성 정의

UE 5.1부터 UMG Widget 속성은 접근자 지정(`Setter`·`Getter` 속성 지정자)과 직접 접근 사용 중단 표시를 두어 리플렉션 경로(Inspector·애니메이션·바인딩)도 C++ 접근자를 거친다(`TextBlock.h:28-31`). setter는 필드 갱신 → Element가 있으면 반영 → 값이 바뀐 경우 변경 통지 순서이고, getter는 Element가 있으면 Element의 현재 값을 읽는다(`Widget.cpp:406-441`). Rocks는 같은 의미를 리플렉션 지정자와 생성 접근자로 구현한다(UD7).

~~~cpp
// Rocks 제안. 지정자 이름은 확정 전이다.
ROCKS_CLASS()
class TextLabelWidget : public Widget {
    ROCKS_GENERATED_BODY()

private:
    // 생성 setter: 동등 비교 → 필드 대입 → Element 반영(ApplyText) → 변경 통지(CU5)
    ROCKS_PROPERTY(Category = "Content", Multiline, Apply = ApplyText, Notify)
    Text text;

    ROCKS_PROPERTY(Category = "Appearance", Apply = ApplyStyle)
    TextStyle style;

    SharedPtr<ui::TextLabel> label_;   // 해제 함수에서 끊는다(5.1절)

    void ApplyText();   // label_이 있으면 SetText
    void ApplyStyle();

protected:
    // Element 생성과 전체 속성 반영(5.1절)
    Result<SharedRef<ui::Element>> BuildElement() override;
    void ApplyAllProperties() override;
    void ReleaseElements(bool releaseChildren) override;
};
~~~

- **실행 스키마:** 접근자 지정(`Apply`·`Notify`)은 편집 메타데이터가 아니라 실행 스키마이며 Shipping에 포함된다(UE의 `Setter`·`Getter`도 실행 중 속성 정보에 남는다). 범주·표시 이름 같은 편집 정보만 CU1에 속한다.
- **변경 통지 비용:** UE는 Widget에 구독 여부 비트 배열만 두고 delegate 저장소는 소유 UserWidget의 확장 객체에 지연 생성하여, 구독자가 없으면 통지 비용이 비트 검사 하나다(`Widget.h:1216`, `Widget.cpp:1902-2055`). Rocks도 같은 구조로 CU5를 구현한다.
- **UE와 다른 점:** UE의 폴링 바인딩(속성 옆 getter delegate, `Widget.h:108-170`)은 도입하지 않는다. 바인딩은 5.5절의 변경 통지 기반만 제공한다.

### 5.4 UserWidget·WidgetTree·WidgetAsset

| Rocks | UE 5.8 대응 | 계약 |
|---|---|---|
| `UserWidget` | `UUserWidget`(`UMG/Public/Blueprint/UserWidget.h:280`) | WidgetTree 소유, 이름 있는 slot 제공, 실행 확장 객체 목록. 초기화 → Element 생성 → 사전 구성(Designer에서도 실행 가능)·구성 → 해체. 소유 LocalPlayer 문맥 전파 |
| `WidgetTree` | `UWidgetTree`(`WidgetTree.h:106-158`) | 루트 Widget과 이름 있는 slot 내용, Widget 생성 함수 |
| `NamedSlot` | `UNamedSlot` · `INamedSlotInterface` | 파생 에셋·인스턴스가 내용을 채우는 자리 |
| `WidgetAsset` | `UWidgetBlueprint` + `UWidgetBlueprintGeneratedClass` | **UE와 다른 점:** Blueprint·생성 클래스 대신 에셋이 템플릿 WidgetTree, 컨트롤러 클래스, 결합 표, 애니메이션, 바인딩, 이름 있는 slot 선언, 부모 에셋 참조, 에셋 확장 자료(ViewModel 정의 등)를 저장한다. 인스턴스 생성은 템플릿 기반 subobject 인스턴싱이다(UE `DuplicateAndInitializeFromWidgetTree`, `UserWidget.cpp:228-337`). 파생 에셋은 부모 트리를 쓰고 이름 있는 slot 내용만 병합한다(UE `WidgetBlueprintGeneratedClass.cpp:210-226`) |
| 컨트롤러 | C++ `UserWidget` 파생(UE의 C++ 부모 클래스와 같다) 또는 Sparks `WidgetController` | Sparks의 `Widget.이름` 경로는 컴파일 시 결합 표의 stable ID로 해석하며 이름은 작성·표시용이다. 구성·해체 통지는 WidgetController의 수명 hook으로 전달한다([Sparks 언어 설계](../SparksLanguage/README.ko.md)) |
| 결합 표 | `meta=(BindWidget)` · `BindWidgetOptional` · `BindWidgetAnim` | **UE와 다른 점:** UE는 인스턴싱 후 같은 이름의 모든 속성에 대입하고 명시 navigation 대상도 이름으로 해석한다(`WidgetBlueprintGeneratedClass.cpp:238-283`, `Widget.cpp:788`). Rocks 에셋은 (컨트롤러 FieldId → Widget stable ID) 표를 저장한다. 표는 Designer가 이름 일치나 명시 지정으로 만들고, 실행 스키마로 Shipping에 포함된다. 명시 navigation 대상·애니메이션 대상·이름 있는 slot도 같은 ID 체계를 쓰며, 파생 에셋 사슬 전체에서 ID 유일성을 보장한다 |
| Widget navigation 규칙 | `UWidget::SetNavigationRule*` · `FNavigationMethod`(`Widget.cpp:788-893`) | 방향별 규칙(경계 탈출·명시·순환·정지·사용자 정의)과 명시 대상 ID를 Widget 속성으로 저장하여 Element navigation(3.9절)에 반영 |
| 확장 | `UWidgetBlueprintGeneratedClassExtension` · `UUserWidgetExtension` | 에셋 확장 자료(예: ViewModel 정의)와 인스턴스 확장 객체(예: 바인딩 실행 view, 변경 통지 저장소) |
| 검증 | `FWidgetBlueprintCompilerContext`(`UMGEditor/Private/WidgetBlueprintCompiler.cpp:502-600, 1257-1412`) | `rocks validate --ui`와 cook에서 필수 결합 누락·형식 불일치·애니메이션 대상 누락·이름 있는 slot 충돌·ID 중복을 진단 코드로 거부한다. 진단 코드 구간은 [컴파일러 진단](../CompilerDiagnostics.ko.md) 체계에 등록한다 |
| 화면 추가 | `AddToViewport` · `AddToPlayerScreen` · `UGameViewportSubsystem`(`GameViewportSubsystem.cpp:150-193, 301-333`) | 전체 화면 추가(viewport overlay)와 LocalPlayer별 추가를 구분한다. UiWidget은 화면 layer 등록 interface만 두고, game viewport·LocalPlayer layer·World 제거 시 자동 제거(slot 옵션, 기본 켬)는 UiWorld이 구현한다. 소유는 Element 쪽(viewport → canvas → root 유지 Element → UserWidget)이다 |

### 5.5 ViewModel 바인딩

UE MVVM 플러그인의 구조를 따른다(`Engine/Plugins/Runtime/ModelViewViewModel`).

- **ViewModel:** 변경 통지(CU5)를 구현한 Object. 값 설정 helper는 비교 → 대입 → 통지다(`MVVMViewModelBase.h:16-110`). Widget도 변경 통지 원천이 될 수 있다.
- **보관:** UE는 ViewModel 인스턴스를 생성 클래스에 추가한 변수로 둔다(`MVVMViewBlueprintCompiler.cpp:493`). Rocks 에셋은 컨트롤러 클래스에 필드를 추가할 수 없으므로, ViewModel 원천은 UserWidget의 바인딩 실행 확장 객체에 에셋 정의 ID로 보관하고 조회 API를 둔다.
- **생성 방식:** 수동·인스턴스 생성·전역 모음·속성 경로·해석기·문맥(`MVVMBlueprintViewModelContext.h:19-33`).
- **방향:** 한 번(→ 대상)·단방향(→ 대상)·양방향·단방향(→ 원천)(`Types/MVVMBindingMode.h:10-17`). 양방향 바인딩에는 변환 함수를 허용하지 않는다(UE 컴파일 오류, `MVVMViewBlueprintCompiler.cpp:797-799`).
- **실행 시점:** 즉시·프레임 끝 지연(그리기 전)·매 프레임·자동(여러 필드에서 촉발되면 지연, 아니면 즉시)(`Types/MVVMExecutionMode.h`).
- **저장:** 원천·대상 필드 경로(CU4)와 변환 함수 참조를 에셋에 저장하고 cook에서 해석·검증한다. 통지가 없는 원천은 경고한다(UE "will only trigger ONCE").
- **UE와 다른 점:** 변환 함수는 Blueprint 그래프 대신 C++ 또는 Sparks의 리플렉션된 함수(CU2)다. 컨트롤러 멤버는 순수·const, 라이브러리 함수는 정적·순수여야 하며(UE `MVVMBlueprintViewConversionFunction.cpp:31-61`), 다중 인자는 인자별 원천 경로를 가진다. 프로젝트 허용 목록으로 사용 가능한 함수를 제한한다. CU2는 순수·const 표지를 제공해야 한다.

### 5.6 UI 애니메이션

UE `UWidgetAnimation`은 MovieScene 시퀀스이며 이름(Widget 이름·slot Widget 이름)으로 대상에 결합하고 공유 entity system linker에서 일괄 평가한다. 5.6부터 재생 상태를 UObject 플레이어(`UUMGSequencePlayer`) 대신 비객체 `FWidgetAnimationState`·handle로 둔다(`UMG/Public/Animation/WidgetAnimation.h`, `WidgetAnimationState.h:47`, `UMGSequenceTickManager.cpp:56-60`). Rocks는 전체 계획 21.3절에 따라 공유 curve/평가기를 사용하고, 재생 상태는 비객체 handle로 둔다. 대상은 stable ID와 속성 경로(CU4)로 결합한다. 트랙은 render transform·여백·색·opacity·Widget material parameter·이벤트를 포함한다. 시계(gameplay·실시간·컷씬)는 전체 계획 21.3절을 따른다.

### 5.7 Widget 풀과 엔트리 목록

| Rocks | UE 5.8 대응 | 계약 |
|---|---|---|
| `WidgetPool` | `FUserWidgetPool`(`UMG/Public/Blueprint/UserWidgetPool.h:14-158`) | 활성·비활성 목록. 비활성 항목은 Element를 cache하여 구성·해체 통지를 생략할 수 있고, 전체 해제 시 모두 해체한다 |
| 엔트리 목록 Widget | `UListViewBase` · `IUserObjectListEntry` | 항목 객체마다 엔트리 Widget을 풀에서 받아 Element 가상화 목록(3.5절)에 표시 |
| 동적 엔트리 상자 | `UDynamicEntryBoxBase` | 가상화 없이 엔트리 Widget을 동적으로 추가·제거. 풀 사용 |

### 5.8 화면 스택과 입력 모드(UiScreens)

| Rocks | UE 5.8 대응(CommonUI·CommonInput) | 계약 |
|---|---|---|
| `Screen` | `UCommonActivatableWidget`(`CommonActivatableWidget.h:43-290`) | 활성화·비활성화, 뒤로 가기 처리, 입력 차단 root(modal) 여부, 자동 활성화, focus 복원, 원하는 입력 설정, 원하는 focus 대상, 화면별 입력 mapping context와 우선순위. 입력 차단 root는 3.10절의 modal 창과 다르다 |
| `ScreenStack` · `ScreenQueue` | `UCommonActivatableWidgetStack` · `Queue` | 스택은 맨 위 전환, 큐는 다음 순서. Widget 풀로 재사용하며 맨 위 비활성 시 아래로 전환하고 위 화면을 반환한다(`CommonActivatableWidgetContainer.cpp:33, 193-396`) |
| 화면 입력 라우터 | `UCommonUIActionRouterBase`(`ULocalPlayerSubsystem`, `CommonUIActionRouterBase.h:66-299`) | LocalPlayer마다 하나. 활성 화면 트리를 유지하고, game viewport 입력보다 먼저 화면 동작 바인딩을 처리한다(UE 진입점 `CommonGameViewportClient.cpp:130-205`). action domain 표, 입력 모드(메뉴·게임·모두), 마우스 capture·cursor·이동/시점 입력 무시를 가장 깊은 활성 화면의 설정으로 적용한다(`CommonUIActionRouterBase.cpp:1888-1911`) |
| 입력 장치 추적 | `UCommonInputSubsystem` · `FCommonInputPreprocessor` | 마우스·키보드 / 게임패드 / 터치를 입력 전처리기에서 판별하고 변경을 통지 |
| 버튼 아이콘 자료 | `UCommonInputBaseControllerData` · `UCommonInputPlatformSettings` | 플랫폼·게임패드 종류별 key → brush 표 |
| 동작 표시줄 | `UCommonBoundActionBar` | 현재 바인딩된 동작의 버튼 목록 |
| 기본 입력 모드 | `APlayerController::SetInputMode`(`FInputModeUIOnly`·`GameAndUI`·`GameOnly`, `PlayerController.h:126-212`) | UiScreens를 쓰지 않는 게임의 기본 입력 모드 API. UiWorld이 LocalPlayer 단위로 제공 |

**UE와 다른 점:** CommonUI는 버튼·텍스트 스타일과 컨트롤러 자료를 클래스 기본 객체(CDO)로 공유한다(`CommonButtonBase.cpp:71-77`, `CommonInputBaseTypes.h:290`). Rocks는 Blueprint 클래스가 없으므로 데이터 에셋으로 공유하고, 클래스 상속은 부모 에셋 + 재정의로 대체한다. 입력 동작은 Rocks Input의 action 체계(전체 계획 Enhanced Input 계열 행)를 사용한다.

### 5.9 월드 공간 Widget

UE `UWidgetComponent`는 화면 공간(매 프레임 투영한 game layer)과 월드 공간(가상 창을 render target에 그려 mesh에 표시) 두 방식을 두고, 월드 공간 입력은 viewport의 사용자 정의 hit 경로 또는 상호작용 component의 가상 사용자로 주입한다(`UMG/Public/Components/WidgetComponent.h:25-95`, `WidgetComponent.cpp:153-191, 1206, 1478`, `WidgetInteractionComponent.cpp:77-535`). UE의 viewport hit 경로는 첫 번째 플레이어만 사용한다(`WidgetComponent.cpp:171`). Rocks는 UiWorld에 같은 구조로 두되 LocalPlayer마다 hit 경로를 둔다. 전체 계획 21.4절의 depth·velocity·업스케일 정책을 적용한다.
