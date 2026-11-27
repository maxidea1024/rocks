## 20. Element 동작 정렬(UW8 상세 설계)

9.1절 UW8의 상세다. 2026-11-25 리뷰가 찾은 Element 동작의 UE 차이 일곱 가지를 UE 5.8과 같게 맞추고, 완료 판정인 UV2(무효화)·UV3(입력 라우팅) 시험을 채운다. 근거 행은 UE 5.8.4 소스(`C:\github\UnrealEngine`) 기준이다.

### 20.1 지금 상태와 UE 차이

| 항목 | 지금(Rocks) | UE 5.8 | 결과 차이 |
|---|---|---|---|
| 시계 | 툴팁·타이머만 함수 포인터로 주입(`SetToolTipClock`·`SetTimerClock`). `ActivityIndicator`(자기 `Clock`)·`ProgressBar`·`Carousel`·`ViewportWorld`는 `platform::MonotonicNanoseconds`를 직접 읽는다 | `FSlateApplication::TickTime`이 프레임마다 한 번 `CurrentTime`을 정하고 큰 간격은 1/8초로 자른다(SlateApplication.cpp 1676–1687행). 위젯은 `FPaintArgs::GetCurrentTime`·`GetDeltaTime`(PaintArgs.h 71–79행)으로 읽는다 | 같은 프레임 안에서 부품마다 시각이 다르다. 시험·snapshot이 실제 시간에 묶인다(18장 Widgets 탭 기준 불일치) |
| hit grid 생성 | 그리기가 끝난 뒤 `HitGrid::Update`가 `ActiveChildren` 순서로 트리를 따로 돈다 | 그리기 중 `SWidget::Paint`가 `AddWidget(this, 0, LayerId, 정렬 순서)`로 등록한다(SWidget.cpp 1577행). cache 패널은 자기 grid를 `AddGrid`로 끼운다(SInvalidationPanel.cpp 385행, SRetainerWidget.cpp 685행, HittestGrid.cpp 766행) | `PaintChildren`을 바꾼 Element(팝업 층·지연 그리기)는 hit 순서와 그리기 순서가 어긋난다. 그리지 않은(cull) Element도 등록된다 |
| 비활성 hit | 비활성 Element와 그 하위 트리를 grid에서 뺀다 | 비활성도 등록하고, 경로를 루트부터 내려가 첫 비활성 위젯에서 자른다(HittestGrid.cpp 235–246행). 툴팁은 `bIgnoreEnabledStatus = true`로 찾는다(SlateUser.cpp 1351행) | 비활성 버튼을 누르면 그 뒤 형제가 맞는다(UE는 비활성 버튼의 부모가 받는다). 비활성 위젯의 툴팁이 안 뜬다 |
| 속성 갱신 | `UpdateBoundAttributes`가 Hidden·Collapsed 하위 트리를 통째로 건너뛴다. 보이기는 묶을 수 없다 | 자식마다 보이기 속성을 먼저 갱신하고, Collapsed가 아니면(Hidden 포함) 나머지 속성을 갱신한다(SWidget.cpp 1846–1859행) | Hidden(공간 유지) Element의 원하는 크기가 낡는다. 보이기를 묶은 접힌 Element는 다시 나타나지 못한다(묶을 수 있게 하면) |
| navigation 변환 | 아무 Element도 키를 처리하지 않으면 `InputAdapter`가 방향키·Tab을 navigation으로 바꾼다(InputAdapter.cpp 82–106행). 초점이 없어도 바꾼다 | 초점 가능한 위젯의 기본 `OnKeyDown`이 bubble 중에 `FNavigationConfig`로 키를 방향으로 바꿔 `FReply::SetNavigation`을 돌려준다(SWidget.cpp 415–427행, NavigationConfig.cpp 59–81행). 처리 안 된 키에 대체 경로는 없다(SlateApplication.cpp 5045–5072행) | 초점 Element의 조상이 방향키를 먼저 받는다(UE는 못 받는다). 키 대응을 사용자별로 바꿀 수 없다 |
| 방향 탐색 | 초점 가능한 후보 전체에서 중심 거리 비교 | hit grid 셀을 진행 방향으로 훑으며, 진행 축으로 쓸어 넓힌 사각형과 겹치는 후보를 고른다(`FindFocusableWidget`, HittestGrid.cpp 366–500행). 경계 규칙은 navigation 응답 | 겹친 후보·멀리 떨어진 후보의 선택이 다르다 |
| slot | 패널 slot은 크기 규칙만 있다. 여백·정렬은 `SlotPresenter` wrapper Element를 끼운다(UiWidget 3곳) | slot이 여백·정렬 mixin을 가진다(BasicLayoutWidgetSlot.h, SlotBase.h) | 트리에 보이지 않는 Element가 끼어 hit·초점·접근성 경로가 길어진다 |
| 형 변환(CB2) | `static_cast<Derived&>(*element)` 166곳, 검사 없음 | `SLATE_DECLARE_WIDGET`의 형식 정보와 `StaticCastSharedRef` | 잘못된 변환이 조용히 정의되지 않은 동작이 된다 |
| 스타일 | `StyleSet`·`AddCoreStyles`(버튼·텍스트 8개)는 있으나 아무도 쓰지 않는다. 부품은 `DefaultButtonStyle()` 같은 함수 기본값을 직접 쓴다 | `FSlateStyleRegistry`에 집합을 등록하고, 앱 스타일(`FAppStyle`)을 바꿔 끼운다. 위젯은 `FAppStyle::Get().GetWidgetStyle<T>("이름")`으로 받는다 | 에디터·게임 스타일 교체가 안 된다. 4.2절 세 집합이 없다 |

### 20.2 시계(UW8a)

- **`ui::Clock`(UiCore).** 시각 공급 delegate(`Delegate<double()>`, 초)다. 기본은 `platform::MonotonicNanoseconds`를 초로 바꾼 것이고, 시험·snapshot은 고정 시계를 준다.
- **프레임 시각.** `Application::BeginFrame()`이 프레임마다 한 번 시계를 읽어 `currentTime`·`deltaTime`을 정한다(UE `TickTime`). 간격이 1/8초(`MaxFrameDeltaSeconds`)를 넘으면 자른다. `PrepareLayout`이 아직 부르지 않았으면 부른다(호스트가 따로 부르지 않아도 프레임당 한 번).
- **읽는 곳.**
  - `PaintList`가 `CurrentTime()`·`DeltaTime()`을 든다(UE `FPaintArgs`). 애니메이션 부품(`ActivityIndicator`·`ProgressBar` marquee·`Carousel`)은 `OnPaint`에서 이것으로 위상을 계산한다. 자기 시계 멤버를 지운다.
  - 툴팁 지연·타이머는 `Application`의 프레임 시각을 쓴다. `SetToolTipClock`·`SetTimerClock`은 `SetClock` 하나로 합친다. 입력 처리 중에 타이머 마감을 계산할 때는 프레임 시각이 아니라 시계를 다시 읽는다(UE도 입력 처리 시각은 `FPlatformTime::Seconds()`). 그래야 호스트의 대기 시간 계산(`SecondsUntilNextTimer`)이 그 프레임 안의 입력을 반영한다.
  - `ViewportWorld`의 World tick 간격은 호스트가 주는 `deltaTime`이다.
- **결정성.** UiDemo `--snapshot`·`--self-test`는 고정 시계(0초에서 시작, 프레임마다 1/60초)를 준다. 그러면 Widgets 탭도 실행마다 byte가 같다.
- **버린 대안.** Element에 `Application*`를 주어 시계를 직접 묻는 방식은 UiCore가 UiFramework를 알게 되어 버렸다. 전역 시계는 창이 여럿인 호스트(창마다 Application)와 시험을 섞어 버렸다.

### 20.3 그리기 중 hit grid(UW8b)

- **등록.** `PaintList`가 `HitGridBuilder*`를 든다(UE `FPaintArgs::GetHittestGrid`). `Element::PaintSubtree`는 clip을 넣은 뒤, `OnPaint` 전에 자신을 (Element, geometry, 부모 clip 항목, layer, 그리기 순번)으로 등록한다.
  - 등록 조건은 UE와 같다: 그리는 Element(Hidden·Collapsed가 아님). Visibility가 `HitTestInvisible`이면 자신과 자식을 후보에서 빼고 clip 항목으로만 남긴다. `SelfHitTestInvisible`이면 자신만 뺀다. 비활성은 빼지 않는다(20.4절).
  - 셀 색인·넓은 항목 목록은 지금 `HitGrid` 구조를 그대로 쓴다(셀 128 px — UE `HittestGrid.cpp` 109행).
- **순서.** 셀 안에서 그리기 순번의 역순(마지막에 그린 것이 위)이다. 같은 순번은 없다. UE의 정렬 키(batch 우선순위·layer·정렬 순서)는 Rocks에서 그리기 순번 하나로 같아진다. Rocks의 layer는 그리기 순서를 바꾸지 않기 때문이다(3.4절).
- **cache root.** `CacheRoot`는 기록할 때 자기 하위 트리의 hit 항목을 자기 grid 조각에 담는다. 다시 쓸 때(빠른 경로) 조각을 바깥 builder에 통째로 붙인다(UE `AddGrid`). 조각의 geometry는 기록 때 창 좌표라, 부모 변환이 바뀌면 cache root가 무효화되어 다시 기록된다(지금 규칙 그대로).
- **완성.** 그리기가 끝나면 `Application`이 builder를 `HitGrid`로 바꿔 넣는다. 따로 도는 `HitGrid::Update`는 지운다. 영역 지도(19.2절)·`QueryWindowRegion`·툴팁은 이 grid를 읽는다.
- **한계.** 그리지 않은 프레임(최소화)에는 grid가 지난 프레임 것으로 남는다(UE와 같다).

### 20.4 비활성 Element 경로 자름(UW8c)

- `HitGrid::Find(point, scope, ignoreEnabled)`가 맨 위 후보를 찾은 뒤, 루트에서 그 후보까지의 경로를 내려가며 첫 비활성 Element 앞에서 자른다. 결과는 남은 경로의 가장 깊은 Element다(UE `GetBubblePath`). 경로가 비면(루트가 비활성) 없음이다.
- 포인터 라우팅·커서 모양은 `ignoreEnabled = false`다. 툴팁 대상 찾기는 `true`다(UE SlateUser.cpp 1351행 — 비활성 위젯도 툴팁을 띄운다).
- 영역 지도(UW7f)는 구조 판정이라 `true`다. 비활성 최대화 버튼도 그 자리는 버튼 영역이다.
- 키 라우팅은 지금처럼 초점 경로에서 비활성 Element를 건너뛴다(UE `RouteAlongFocusPath`의 `IsEnabled` 검사, SlateApplication.cpp 5049행).

### 20.5 속성 갱신과 보이기 묶기(UW8d)

- 보이기를 `Bindable<Visibility>`로 묶을 수 있게 한다(`Element::BindVisibility`). 사유는 Layout이다(UE `Visibility` 속성 — 부모 Layout).
- `UpdateBoundAttributes` 순서는 UE와 같다. 자식마다 보이기 묶음을 먼저 평가하고, Collapsed가 아니면(Hidden 포함) 나머지 묶음을 평가한 뒤 그 자식으로 내려간다. 루트도 같은 규칙이다.
- Collapsed 하위 트리의 묶음은 평가하지 않는다(그 Element의 보이기 묶음만 평가한다). 보이기가 Collapsed에서 바뀌면 Layout 무효화로 다음 원하는 크기 계산에 들어간다.

### 20.6 navigation 변환(UW8e)

- **키 응답.** `Element::OnKeyEvent`는 `KeyReply`를 돌려준다: 처리 안 함 · 처리함 · 방향 navigation(방향, 원인 Keyboard·Controller). 인자는 `KeyEvent`(사용자, 입력 이벤트, 조합 키, 이 키의 navigation 방향과 동작)다.
- **기본 처리(UE `SWidget::OnKeyDown`).** `Element::OnKeyEvent` 기본 구현은 Element가 초점을 받을 수 있고 KeyDown이며 `KeyEvent::navigation`이 있으면 navigation 응답을 돌려준다. 키를 쓰고 싶은 override는 처리함을 돌려주고, 아니면 `Super::OnKeyEvent`를 부른다(UE와 같은 관례). 지금 override 13곳을 옮긴다.
- **`NavigationConfig`(UiFramework, 사용자별 — UE `FNavigationConfig`).**
  - 키 대응표 기본값은 UE와 같다: 방향키·게임패드 D-pad → 상하좌우(NavigationConfig.cpp 19–29행). 왼쪽 스틱은 키가 아니라 아날로그 경로다(축 16–17행, 임계값 0.5 — 13–14행).
  - Tab → Next, Shift+Tab → Previous. Ctrl·Alt·Command가 눌려 있으면 바꾸지 않는다(59–81행).
  - 동작 키도 둔다: 확인 Enter·Space·패드 확인, 뒤로 Escape·패드 뒤로(31–38행). 버튼류는 키 대신 이 동작으로 누름을 판정한다(UE `GetNavigationActionFromKey`).
  - `keyNavigation`·`tabNavigation`·`analogNavigation` 켜기와 아날로그 반복 간격도 UE 기본값으로 둔다.
  - `Application`이 키마다 방향과 동작을 미리 계산해 `KeyEvent`에 넣는다(Element는 Application을 모른다).
- **적용.** bubble 중 navigation 응답이 나오면 라우팅을 멈추고(조상은 받지 않는다) 그 사용자의 navigation을 실행한다. `InputAdapter`의 대체 변환은 지운다. 초점이 없거나 초점 경로에 초점 가능한 Element가 없으면 navigation도 없다(UE와 같다).
- **게임패드.** 버튼 누름(`GamepadState`)을 키 이벤트(가상 키 `GamepadDPadUp` 등)로 바꿔 같은 경로에 넣는다. `OnGamepadButtonDown`은 지운다. 아날로그 스틱 navigation은 임계값을 넘는 순간과 반복 간격(주입 시계)으로 방향 이벤트를 만든다(UE `FAnalogCursor`가 아니라 `FNavigationConfig::GetNavigationDirectionFromAnalog` 경로).
- **F1 툴팁**(UW4t, UE에 없음)은 지금처럼 처리 안 된 키에서 연다. 앱 명령(`AddWindowCommands`)과 같은 단계다.

### 20.7 방향 탐색 UE 규칙(UW8f)

- `HitGrid::FindNextFocusable(start, direction, reply, user)`가 UE `FindFocusableWidget`(HittestGrid.cpp 366–500행)을 옮긴다.
  - 시작 사각형은 초점 Element의 그리기 경계(`GetRenderBoundingRect` — 회전·배율 포함)다.
  - 진행 축으로 쓸어 넓힌 사각형(swept rect)을 만든다. 경계 규칙(Escape·Stop·Wrap·Explicit·Custom·CustomBoundary)이 막는 경계는 navigation 응답의 경계 Element 사각형이다.
  - 셀을 진행 방향으로 한 줄씩 훑는다. 줄마다 후보 중 진행 축 거리가 가장 가까운 것, 같으면 중심 거리 제곱이 작은 것을 고른다. 다음 줄은 이미 찾은 것보다 멀면 멈춘다.
  - 후보는 초점 받을 수 있고, 활성이며, 사용자가 볼 수 있는 Element다(UE `IsCompatibleUserIndex`·`SupportsKeyboardFocus`·`IsEnabled`). 비활성 목적지는 UE처럼 건너뛰되 진단 목록에 남긴다.
- Wrap 규칙은 반대쪽 경계에서 같은 탐색을 다시 한다(UE 540–560행).
- 탐색 방식은 교체 가능한 계약이다(3.9절 — UE 1179행 확장점). 기본 구현이 위 규칙이다.

### 20.8 Element slot·CB2(UW8g)

- **slot 기능.** UiCore에 `SlotPadding`·`SlotAlignment`(UE mixin 대응)를 두고, 패널 slot 구조체가 조합한다: `StackSlot`(크기·여백·정렬), `OverlaySlot`(여백·정렬·z), `GridSlot`(행·열·span·여백·정렬), `UniformGridSlot`, `WrapSlot`(여백·정렬·채움 비율). `AnchorCanvas` slot은 이미 같다.
- 패널 배치 함수가 slot의 여백·정렬을 적용한다(UE `ArrangeSingleChild`·`AlignmentArrangeResult`). `SlotPresenter`·`SlotPlacement`를 지우고, UiWidget의 wrapper 3곳(`PanelWidget`·`CarouselNavBarWidget`·`ScrollBoxWidget`)이 slot 값을 직접 쓴다.
- **형식 정보(CB2).** `ElementType`(이름, 부모 `ElementType*`)을 둔다. 클래스 선언에 `ROCKS_UI_ELEMENT(Class, Super)`를 쓴다(UE `SLATE_DECLARE_WIDGET` — `using Super`, 정적 `StaticType()`, 가상 `GetType()`를 만든다). 3.2절의 "UI 계층에 매크로가 필요해지면 `ROCKS_UI_` 접두" 규칙에 맞는다.
  - `IsA<T>()`는 부모 사슬을 따라간다. `ElementCast<T>(ptr)`는 아니면 빈 포인터, `StaticElementCast<T>(ptr)`는 Release 밖에서 assert한다(UE `StaticCastSharedRef` + 형식 검사).
  - UI 모듈 안 Element 하향 `static_cast`를 이것으로 바꾼다. policy가 `static_cast<...Element...&>(*` 형태를 막는다.
- **버린 대안.** CRTP 기반 클래스(`ElementBase<Self, Super>`)는 매크로 없이 되지만, 모든 Element의 상속 줄을 바꾸고 다중 상속(`TextInputContext`)과 섞기 어렵다.

### 20.9 스타일 등록부(UW8h)

- **`StyleRegistry`(UiCore, `Application` 소유 — UE `FSlateStyleRegistry`).** 이름 있는 `StyleSet`을 등록한다. 집합은 부모 집합 이름을 가질 수 있고, 찾지 못한 이름은 부모로 올라간다(UE `FSlateStyleSet::ParentStyleName`).
- **앱 스타일(UE `FAppStyle`).** `Application::SetAppStyle(이름)`이 활성 집합을 고른다. 기본은 `CoreStyle`이다. 에디터 호스트는 `EditorStyle`(부모 `CoreStyle`)로 바꾼다. 게임 Widget 기본은 `GameWidgetStyle`(UMGCoreStyle)이다.
- **형식별 표.** 지금 있는 스타일 구조체 전부를 표로 둔다: 버튼, 체크, 텍스트, 편집 텍스트, 스크롤 막대, 진행 막대, 슬라이더, 콤보, 표 머리, 독 탭, 창, 메뉴, 툴팁. 색·brush·실수·여백 표도 둔다.
- **부품.** 부품의 `Args::style`이 nullptr이면 그리기 때 `PaintList`가 든 활성 집합에서 UE 키(`"Button"`, `"Checkbox"` 등)로 찾는다. `DefaultButtonStyle()` 같은 함수 기본값은 `AddCoreStyles`가 집합에 넣는 값의 출처로만 남긴다.
- **값.** 세 집합의 항목을 4.2절 출처(StarshipCoreStyle.cpp, StarshipStyle.cpp, UMGCoreStyle.cpp)에서 옮기고 출처 행을 시험에 적는다(UV8 일부). 이 단계는 Rocks 구조체가 있는 항목까지다. 구조체가 없는 항목(예: 디테일 패널 전용)은 15장 빈칸 목록에 남긴다.

### 20.10 UV2·UV3 시험(UW8i)

8장의 장면 구성 중 이 단계까지 구현된 것을 모두 시험으로 만든다. 기대값마다 UE 근거 행을 단다.
- **UV2 무효화.**
  - 같은 값 설정은 무효화 0회다.
  - 사유 정규화: RenderTransform·Visibility 변경은 부모 Layout이다.
  - 크기 불변 Layout은 부모로 전파하지 않는다.
  - cache root 빠른 경로를 다시 쓰고, volatile Element만 다시 그린다.
  - 기본 경로(창 전체 cache 꺼짐)의 다시 그린 Element 수를 센다.
  - 보이기 묶음 평가 순서(20.5절)와 Hidden 하위 트리 갱신을 본다.
- **UV3 입력 라우팅.**
  - 전처리기 구간과 capture를 본다.
  - preview tunnel·bubble 순서를 본다.
  - 보이기 5종과 겹친 layer를 본다.
  - 비활성 경로 자름(20.4절)과 툴팁의 비활성 무시를 본다.
  - 팝업 바깥 클릭과 IME 조합 중 게임 전처리기를 본다.
  - modal 창은 그 기능이 생길 때(지금은 없음) 더한다.
- **UV4 일부.** bubble 중 navigation 변환(20.6절)과 기하 navigation(20.7절)을 본다. 규칙 6종과 사용자 2명도 본다.
- **UE 실행 기준값(UD6).** UE 5.8로 빌드한 덤프 프로그램 결과는 UE 빌드 환경이 필요해 이 단계에서 만들지 않는다. 소스 근거 기준값만 둔다. 덤프는 UT1 판정 때 한다(10장 위험 목록에 남긴다).

### 20.11 단계

| 단계 | 범위 | 완료 판정 |
|---|---|---|
| UW8a | `ui::Clock`, `Application::SetClock`·`BeginFrame`(1/8초 자름), `PaintList` 시각, 애니메이션 부품·툴팁·타이머·`ViewportWorld` 이관, UiDemo 고정 시계 | 시계 전진 시험(툴팁·타이머·throbber 위상), snapshot 7탭 모두 실행마다 byte 동일 |
| UW8b | 그리기 중 hit grid 등록(`HitGridBuilder`), cache root grid 조각, `HitGrid::Update` 제거 | 그리기 순서 = hit 순서(`PaintChildren` 재정의·Overlay layer), cache 재사용 뒤 hit 동일, 영역 지도 동치 시험 유지 |
| UW8c | 비활성 경로 자름, 툴팁·영역 지도의 비활성 무시 | 비활성 버튼 클릭 → 부모, 비활성 위젯 툴팁 표시 |
| UW8d | `BindVisibility`, 보이기 먼저 평가, Hidden 갱신 | 접힌 Element가 묶음으로 다시 보임, Hidden Element 원하는 크기 갱신 |
| UW8e | `KeyReply`·`KeyEvent`, 기본 navigation 변환, `NavigationConfig`, 게임패드 가상 키, `InputAdapter` 대체 변환 제거 | 조상이 방향키를 받지 않음, 초점 없음 → navigation 없음, 키 대응 교체 |
| UW8f | UE `FindFocusableWidget` 이식, Wrap, 탐색 계약 | UE 규칙 장면(겹친 후보·먼 후보·회전) 기준값 |
| UW8g | slot 여백·정렬, `SlotPresenter` 제거, `ElementType`·`ROCKS_UI_ELEMENT`·`ElementCast`, policy | wrapper 0, Element 하향 `static_cast` 0(policy) |
| UW8h | `StyleRegistry`·앱 스타일·형식별 표·세 집합, 부품이 활성 집합에서 조회 | UE 출처 대조 시험, 집합 교체 시험, snapshot 회귀 없음 |
| UW8i | UV2·UV3·UV4 일부 시험 묶음(20.10절) | 기준값 표마다 UE 근거 행 |
