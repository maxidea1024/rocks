## 20. Element 동작 정렬(UW8 상세 설계)

9.1절 UW8의 상세다. 2026-11-25 리뷰가 찾은 Element 동작 차이를 UE 5.8과 같게 맞추고, 완료 판정인 UV2(무효화)·UV3(입력 라우팅) 시험과 UV4 일부를 채운다. 근거 행은 UE 5.8.4 소스(`C:\github\UnrealEngine`) 기준이다. 설계 리뷰 20건을 반영했다(12장 초안 2.13).

### 20.1 지금 상태와 UE 차이

| 항목 | 지금(Rocks) | UE 5.8 | 결과 차이 |
|---|---|---|---|
| 시계 | 툴팁·타이머만 함수 포인터로 주입(`SetToolTipClock`·`SetTimerClock`). `ActivityIndicator`(자기 `Clock`)·`ProgressBar`·`Carousel`·`ViewportWorld`는 `platform::MonotonicNanoseconds`를 직접 읽는다. Carousel·ProgressBar는 그리기 밖(`Show`·`SetMarquee`·속성 갱신·Arrange)에서도 읽는다 | `FSlateApplication::TickTime`이 프레임마다 `CurrentTime`을 실제 시계로 정하고, 간격이 1/8초를 넘으면 `LastTickTime`만 당긴다(SlateApplication.cpp 1675–1686행). 위젯은 `FPaintArgs::GetCurrentTime`·`GetDeltaTime`(PaintArgs.h 71–79행)과 `GetCurrentTime()`(active timer 등록 — SWidget.cpp 1896–1898행)으로 읽는다 | 같은 프레임 안에서 부품마다 시각이 다르다. 시험·snapshot이 실제 시간에 묶인다(18장 Widgets 탭 기준 불일치) |
| hit grid 생성 | 그리기가 끝난 뒤 `HitGrid::Update`가 `ActiveChildren` 순서로 트리를 따로 돈다. clip과 겹치지 않는 Element는 뺀다 | 그리기 중 `SWidget::Paint`가 `AddWidget(this, 0, LayerId, 정렬 순서)`로 등록한다(SWidget.cpp 1577행). cache 패널·retainer는 자기 grid를 `AddGrid`로 끼운다(SInvalidationPanel.cpp 385행, SRetainerWidget.cpp 685행, HittestGrid.cpp 766행) | `PaintChildren`을 바꾼 Element(`RetainerBox`·`Carousel`)는 hit 대상과 그리기 대상이 어긋난다. 그리기 정렬(DrawBatch의 layer·순번)과 hit 순서도 다를 수 있다 |
| 비활성 hit | 비활성 Element와 그 하위 트리를 grid에서 뺀다. `Accepts`·`CandidateFromTop`·대체 `Element::HitTest`가 `IsInputPathActive`로 조상 비활성도 거른다(Element.cpp 159행) | 비활성도 등록하고, 경로를 루트부터 내려가 첫 비활성 위젯에서 자른다(HittestGrid.cpp 235–246행). 툴팁은 `bIgnoreEnabledStatus = true`로 찾는다(SlateUser.cpp 1351행) | 비활성 버튼을 누르면 그 뒤 형제가 맞는다(UE는 비활성 버튼의 부모가 받는다). 비활성 위젯의 툴팁이 안 뜬다 |
| 속성 갱신 | `UpdateBoundAttributes`가 Hidden·Collapsed 하위 트리를 통째로 건너뛴다. 보이기는 묶을 수 없다 | 자식마다 보이기 속성을 먼저 갱신하고, Collapsed가 아니면(Hidden 포함) 나머지 속성을 갱신한다(SWidget.cpp 1846–1859행) | Hidden(공간 유지) Element의 원하는 크기가 낡는다. 보이기를 묶을 수 없다 |
| navigation 변환 | 아무 Element도 키를 처리하지 않으면 `InputAdapter`가 방향키·Tab을 navigation으로 바꾼다(InputAdapter.cpp 84–106행). `ScreenInputRouter`도 D-pad를 바로 `Navigate`하고 East를 뒤로, South를 Enter로 합성한다(UiScreens ScreenInputRouter.cpp 120–190행). 초점이 없으면 첫 후보에 초점을 준다(Navigation.cpp 66행) | 초점 가능한 위젯의 기본 `OnKeyDown`이 bubble 중에 `FNavigationConfig`로 키를 방향으로 바꿔 `FReply::SetNavigation`을 돌려준다(SWidget.cpp 415–427행, NavigationConfig.cpp 13–38·59–81행). 그 뒤 초점 경로의 `OnNavigation`이 경계 규칙을 정한다. 처리 안 된 키에 대체 경로는 없다(SlateApplication.cpp 5045–5072행) | 초점 Element의 조상이 방향키를 먼저 받는다(UE는 `OnNavigation`으로만 본다). 키 대응을 사용자별로 바꿀 수 없다 |
| 방향 탐색 | 초점 가능한 후보 전체에서 중심 거리 비교 | hit grid 셀을 진행 방향으로 훑어, 후보가 있는 첫 줄에서 고른다(`FindFocusableWidget`, HittestGrid.cpp 366–570행, 쓸어 넓힌 사각형 598–640행) | 겹친 후보·멀리 떨어진 후보의 선택이 다르다 |
| 단일 자식 slot | 패널 slot은 이미 여백·정렬(`StackSlot`, `SlotPlacement` — Grid·UniformGrid·Wrap)을 가진다. 그러나 Widget 층 단일 자식 slot(`AlignedWidgetSlot` — Border·Button·SizeBox·ScaleBox·SafeZone·BackgroundBlur·Switcher·WindowTitleBarArea·Overlay·ScrollBox)은 `SlotPresenter` wrapper Element를 끼운다 | 단일 자식 위젯이 slot 여백·정렬을 직접 든다(`FSimpleSlot`·`TSingleWidgetChildrenWithBasicLayoutSlot`, BasicLayoutWidgetSlot.h) | 트리에 보이지 않는 Element가 끼어 hit·초점·접근성 경로가 길어진다 |
| 형 변환(CB2) | Element 계열 하향 `static_cast` 약 186곳, 검사 없음 | `SLATE_DECLARE_WIDGET`의 형식 정보와 `StaticCastSharedRef` | 잘못된 변환이 조용히 정의되지 않은 동작이 된다 |
| 스타일 | `StyleSet`·`AddCoreStyles`(버튼·텍스트 8개)는 있으나 아무도 쓰지 않는다. 부품은 `DefaultButtonStyle()` 같은 함수 기본값을 직접 쓴다 | 전역 `FSlateStyleRegistry`에 집합을 등록하고, 위젯이 생성 때 `FAppStyle::Get().GetWidgetStyle<T>("이름")`으로 받는다 | 에디터·게임 스타일 교체가 안 된다. 4.2절 세 집합이 없다 |

### 20.2 host 서비스 묶음

시계·스타일·텍스트 입력·팝업 허용 영역은 모두 "창 호스트가 주고 Element가 루트로 올라가 찾는" 값이다. 지금은 Element semantics에 하나씩 들어 있다. 이것을 `UiHostServices`(UiCore, 공유 레코드) 하나로 묶는다.
- `Application::ApplyHostServices`가 루트(툴팁 임시 호스트 포함)에 단다. `Element::FindUiHostServices()`는 한 번 올라가서 찾는다.
- 트리에 아직 붙지 않은 Element는 못 찾는다. 이때 시각은 0, 스타일은 내장 기본값이다. 붙으면 `TryAddChild`의 Layout 무효화로 다시 측정한다.
- 묶음은 Application마다 하나다. 프레임 시각이 Application마다 다르기 때문이다(월드 위젯은 World 시간). 여러 Application이 나눠 쓰는 자원(스타일 등록부)은 묶음 안의 공유 포인터로, 호스트가 한 번 만들어 모든 Application에 준다.

### 20.3 시계(UW8a)

- **시계 원천.** `ui::Clock`(UiCore)은 초 단위 시각 공급 delegate다. 기본은 단조 시계다. 시험·snapshot은 `ManualClock`을 준다. 이 시계는 읽어도 전진하지 않고, 호스트가 `Advance(초)`로 올린다. 읽는 횟수가 결과를 바꾸지 않게 하려는 것이다.
- **프레임 시각(UE `TickTime`).**
  - `Application::BeginFrame()`이 프레임마다 한 번 시계를 읽는다. `currentTime`은 읽은 값 그대로다.
  - 간격이 1/8초(`MaxFrameDeltaSeconds`)를 넘으면 이전 시각(`lastTime`)만 당겨 `deltaTime`을 자른다. 누적 시각을 따로 두지 않으므로 타이머 마감과 시간 기준이 갈라지지 않는다.
  - 호스트는 입력 처리 전에 `BeginFrame`을 부른다. 측정·배치(`PrepareLayout`)와는 따로다. 부르지 않은 채 `PrepareLayout`이 오면 그것이 먼저 부른다(시험·단순 호스트). 그때 그 프레임의 입력은 지난 프레임 시각을 본다.
  - 월드 위젯(UiWorld)은 `BeginFrame(FrameTime{world current, world delta})`로 World 시간을 준다(UE `WidgetComponent`의 widget tick). 지금 World에는 시각을 읽는 공개 접근자가 없다(`lastRealDeltaSeconds_`가 private). 그래서 그 접근자가 생길 때까지(World 모듈 요청, 엔진 런타임 세션) 월드 위젯은 자기 Application 시계로 `PrepareLayout`이 프레임을 시작한다.
- **읽는 곳.**
  - `UiHostServices`가 그 프레임의 `FrameTime`을 든다. Element는 그리기 밖(속성 갱신·Arrange·`Show`·`SetMarquee`)에서도 `FindUiHostServices()->Time()`으로 같은 값을 읽는다.
  - `PaintList`도 같은 값을 든다(UE `FPaintArgs`). cache root·표면 기록(`SurfaceRecorder`)의 목록도 같은 값을 받는다. `ViewportSource::Advance`는 프레임 번호와 함께 이 값을 받는다. 애니메이션 부품(`ActivityIndicator`·`ProgressBar` marquee·`Carousel`)은 이 값으로 위상을 계산하고 자기 시계 멤버를 지운다.
  - 툴팁 지연·타이머 등록·마감은 프레임 시각을 쓴다(UE `RegisterActiveTimer`가 `GetCurrentTime()` — SWidget.cpp 1898행). `SetToolTipClock`·`SetTimerClock`은 `SetClock` 하나로 합친다.
  - 호스트 대기 시간(`SecondsUntilNextTimer`)은 시계를 다시 읽어 마감까지 남은 시간을 계산한다(대기 계산만 — 상태는 바꾸지 않는다).
- **결정성.** UiDemo `--snapshot`·`--self-test`는 `ManualClock`을 0초에서 시작해 프레임마다 1/60초 올린다. 그러면 Widgets 탭도 실행마다 byte가 같다.
- **버린 대안.**
  - Element에 `Application*`를 주는 방식은 UiCore가 UiFramework를 알게 되어 버렸다.
  - 전역 시계는 창이 여럿인 호스트와 시험을 섞는다.
  - 읽을 때마다 전진하는 고정 시계는 읽는 횟수에 결과가 묶여 버렸다.

### 20.4 그리기 중 hit grid(UW8b)

- **등록.** `PaintList`가 `HitGridBuilder*`를 든다(UE `FPaintArgs::GetHittestGrid`). `Element::PaintSubtree`는 자신을 (Element, geometry, 부모 clip 항목, layer, 순번)으로 등록한다.
  - 등록 위치는 local cache 빠른 경로의 `return`보다 앞이다(Element.cpp 389–407행). 그래야 CachePanel 자신도 등록된다.
  - 조건은 지금 `Record`와 같다: Hidden·Collapsed가 아니고 clip과 겹친다.
  - `HitTestInvisible`은 자신과 자식을 후보에서 빼되 clip 항목으로는 남긴다. `SelfHitTestInvisible`은 자신만 뺀다. 비활성은 빼지 않는다(20.5절).
  - 셀 색인·넓은 항목 목록은 지금 구조 그대로다(셀 128 px — UE HittestGrid.cpp 109행).
- **순서 키.** (layer, 순번)이다. `DrawBatch`가 그리기를 같은 키로 정렬하므로(DrawBatch.cpp 104–111행) 위에 보이는 것이 위에서 맞는다. 셀 안에서는 키의 역순으로 검사한다.
- **grid 조각(UE `AddGrid`).**
  - `CacheRoot`(창 cache·CachePanel)는 기록할 때 하위 트리의 hit 항목을 조각에 담고, 다시 쓸 때 바깥 builder에 통째로 붙인다.
  - `RetainerBox`도 보관 텍스처를 만들 때(`RecordContent`) 조각을 담고, 보관본을 쓰는 프레임마다 붙인다. 지금은 보관 중 `PaintChildren`이 비어 자식이 hit에서 사라진다(RetainerBox.cpp 103–104행).
  - `Carousel`은 슬라이드 중 `PaintChildren`이 그리는 두 자식만 등록한다(UE와 같이 보이는 것만 맞는다).
  - 조각의 geometry는 기록 때 창 좌표다. 부모 변환이 바뀌면 plan revision이 올라 다시 기록되므로(지금 규칙) 낡지 않는다.
- **완성.** 그리기가 끝나면 `Application`이 builder를 `HitGrid`로 바꿔 넣는다. 따로 도는 `HitGrid::Update`는 지운다. 영역 지도(19.2절)·`QueryWindowRegion`·툴팁은 이 grid를 읽는다.
- **스냅샷 없는 판정.** grid가 없을 때 쓰는 `Element::HitTest`(Element.cpp 782행)는 같은 규칙(그리기 자식 순서, 비활성 자름)으로 맞춘다. 둘이 다르면 첫 프레임 입력이 다르게 간다.
- **한계.** 그리지 않은 프레임(최소화)에는 grid가 지난 프레임 것으로 남는다(UE와 같다).

### 20.5 비활성 Element 경로 자름(UW8c — UW8b 뒤)

- `IsInputPathActive(scope, requireEnabled)`로 활성 검사를 인자로 뗀다. hit 판정은 보이기·scope만 보고, 활성은 아래 자름으로만 다룬다.
- `HitGrid::Find(point, scope, ignoreEnabled)`가 맨 위 후보를 찾은 뒤, 루트에서 그 후보까지의 경로를 내려가며 첫 비활성 Element 앞에서 자른다. 결과는 남은 경로의 가장 깊은 Element다(UE `GetBubblePath`). 경로가 비면(루트가 비활성) 없음이다. 자른 결과를 버리고 뒤 후보로 가지 않는다.
- `ignoreEnabled`의 쓰임은 다음과 같다.
  - 포인터 라우팅·커서 모양은 false다.
  - 툴팁 대상 찾기는 true다(UE SlateUser.cpp 1351행 — 비활성 위젯도 툴팁을 띄운다).
  - 영역 지도(UW7f)는 구조 판정이라 true다. 비활성 최대화 버튼도 그 자리는 버튼 영역이다.
- 키 라우팅은 지금처럼 초점 경로에서 비활성 Element를 건너뛴다(UE `RouteAlongFocusPath`의 `IsEnabled` 검사, SlateApplication.cpp 5049행).

### 20.6 속성 갱신과 보이기 묶기(UW8d)

- 보이기를 `Bindable<Visibility>`로 묶을 수 있게 한다(`Element::BindVisibility`). 사유는 Layout이다(UE `Visibility` 속성 — 부모 Layout).
- `UpdateBoundAttributes` 순서는 UE와 같다. 자식마다 보이기 묶음을 먼저 평가하고, Collapsed가 아니면(Hidden 포함) 나머지 묶음을 평가한 뒤 그 자식으로 내려간다. 루트도 같다.
- Collapsed 하위 트리의 묶음은 평가하지 않는다(그 Element의 보이기 묶음만 평가한다). 보이기가 Collapsed에서 바뀌면 Layout 무효화로 다음 원하는 크기 계산에 들어간다.

### 20.7 키 응답과 navigation 이벤트(UW8e1)

- **키 응답.** `Element::OnKeyEvent`는 `KeyReply`를 돌려준다: 처리 안 함 · 처리함 · 방향 navigation(방향, 원인 Keyboard·Controller). 인자는 `KeyEvent`(사용자, 입력 이벤트, 조합 키, 이 키의 navigation 방향·동작)다.
- **기본 처리(UE `SWidget::OnKeyDown`).** 기본 구현은 Element가 초점을 받을 수 있고 KeyDown이며 `KeyEvent::navigation`이 있으면 navigation 응답이다. 키를 쓰고 싶은 override는 처리함을, 아니면 `Super::OnKeyEvent`를 돌려준다(UE 관례). override 13곳을 옮긴다.
- **navigation 이벤트(UE `OnNavigation`).**
  - navigation 응답이 나오면 키 라우팅을 멈춘다(조상은 키를 받지 않는다).
  - 그 뒤 초점 경로를 초점 Element부터 루트로 올라가며 `OnNavigation(NavigationEvent)`를 부른다. 결과 `NavigationReply`가 Escape가 아니면 거기서 멈추고 그 규칙(Explicit·Custom·CustomBoundary·Stop·Wrap)과 경계 Element로 탐색한다(UE `FSlateApplication::AttemptNavigation`).
  - 기본 `OnNavigation`은 Element의 navigation 규칙(`SetNavigationRule`)을 돌려준다.
  - 조상이 키로 하던 방향 처리를 `OnNavigation`으로 옮긴다:
    - `MenuPanel`: Up·Down의 구분선 건너뛰기, Left의 하위 메뉴 닫기, Tab
    - `ListView`·`TreeView`: 선택 이동
    - `ScrollView`: 끝에서 경계 탈출
    - `Slider`: 값 변경
  - UE 근거: SListView.h 61–73행, SScrollBox.cpp 1285행, SSlider.cpp 193행.
- **`NavigationConfig`(UiFramework, 사용자별 — UE `FNavigationConfig`).**
  - 키 대응표 기본값은 UE와 같다: 방향키·게임패드 D-pad → 상하좌우(NavigationConfig.cpp 19–29행). 왼쪽 스틱은 아날로그 경로다(축 16–17행, 임계값 0.5 — 13–14행).
  - Tab → Next, Shift+Tab → Previous다. Ctrl·Alt·Command가 눌려 있으면 바꾸지 않는다(59–81행). Next·Previous는 지금처럼 tree 순서(`MoveFocus`)로 찾는다.
  - 동작 키: 확인은 Enter·Space·패드 확인, 뒤로는 Escape·패드 뒤로다(31–38행). 버튼류는 키 대신 동작으로 누름을 판정한다(UE `GetNavigationActionFromKey`).
  - `Application`이 키마다 방향·동작을 계산해 `KeyEvent`에 넣는다(Element는 Application을 모른다).
- `InputAdapter`의 대체 변환을 지운다. 초점 경로에 초점 가능한 Element가 없으면 navigation도 없다(UE와 같다). Ctrl+F1 툴팁(UW4t, UE에 없음)은 처리 안 된 키 단계에 남긴다.
- **회귀 시험.** 메뉴(구분선·하위 메뉴·Tab)·목록 선택·스크롤·슬라이더의 키보드 동작을 이 단계 전 시험과 같은 결과로 확인한다.

### 20.8 게임패드·초기 초점(UW8e2 — UW8a·UW8e1 뒤)

- 버튼 누름(`GamepadState`)을 가상 키(`GamepadDPadUp`·`GamepadFaceBottom` 등)의 KeyDown·KeyUp으로 바꿔 같은 키 경로에 넣는다. `OnGamepadButtonDown`은 지운다.
- 왼쪽 스틱은 임계값을 넘는 순간과 반복 간격(프레임 시각)으로 방향 navigation을 만든다(UE `GetNavigationDirectionFromAnalog`).
- `ScreenInputRouter`의 직접 `Navigate`·Enter 합성을 지운다. 확인·뒤로는 `NavigationConfig` 동작으로 처리한다(South = 확인, East = 뒤로 — UE `Virtual_Gamepad_Accept`·`Back`).
- **초기 초점.** UE Slate는 초점이 없으면 아무것도 하지 않고, 게임 UI는 화면 활성화 때 원하는 초점 대상에 초점을 준다(UE `UUserWidget::DesiredFocusWidget`, CommonUI `GetDesiredFocusTarget`). Rocks도 `Application`은 초점 없는 navigation을 하지 않는다. 대신 UiScreens 화면이 활성화될 때와 초점이 사라진 채 navigation·확인 입력이 올 때 화면의 원하는 초점 대상에 초점을 준다. 지금 Navigation.cpp 66행의 "첫 후보" 동작은 이 정책으로 옮긴다.

### 20.9 방향 탐색 UE 규칙(UW8f — UW8b·UW8c·UW8e1 뒤)

`HitGrid::FindNextFocusable(start, direction, reply, user, scope)`가 UE `FindFocusableWidget`(HittestGrid.cpp 366–570행)과 호출부(598–640행)를 옮긴다.
- **사각형.**
  - 시작 사각형은 초점 Element의 그리기 경계(회전·배율 포함)다.
  - 진행 축으로 경계 규칙 Element의 사각형 끝까지 쓸어 넓히고, 수직 축은 양쪽을 0.5씩 줄인다(598–640행).
- **훑기.** 셀을 진행 방향으로 한 줄씩 훑는다.
  - 후보가 있는 첫 줄에서 고른다. 진행 축 비교는 허용오차 0.1로 한다(`A ± 0.1 < B`).
  - 같은 줄 안에서는 진행 축 거리가 가까운 것, 같으면 중심 거리 제곱이 작은 것이다.
- **후보.**
  - hit grid에 등록되고 초점 받을 수 있고 활성이며, 그 사용자가 볼 수 있는 Element다(UE `IsCompatibleUserIndex`·`SupportsKeyboardFocus`·`IsEnabled`).
  - `HitTestInvisible` 아래 Element는 등록 후보가 아니므로 탐색 후보도 아니다(UE와 같다).
  - 경계 Element의 자손이어야 한다(`NotADescendant` 검사 454–460행).
  - 고른 후보의 조상이 비활성이면 그 후보를 비활성 목적지에 넣고 다시 찾는다(`IsParentsEnabled`, 528–533행).
- **경계 규칙.** 찾은 것이 경계를 넘으면 규칙을 적용한다(495–523행). Explicit는 지정 대상, Custom·CustomBoundary는 delegate, Stop은 없음이다. Wrap은 반대쪽 변에서 한 번 더 훑는다(516–523·543–560행).
- **scope.** Rocks 팝업은 같은 창 grid 안에 있다. 그래서 지금의 `scope` 인자(팝업·modal 범위)를 유지한다. Escape 규칙이어도 scope 밖으로는 나가지 않는다. UE는 팝업이 별도 창이라 이 문제가 없다.
- **확장점.** UE 1179행은 하위 트리 메타데이터 확장점으로, `GEnableNavigationExperimental`(기본 false, 35행)일 때만 쓴다. Rocks는 탐색 방식을 교체 가능한 계약으로 두되 기본값은 위 규칙 하나다.

### 20.10 단일 자식 slot·CB2(UW8g)

- **단일 자식 slot(UE `FSimpleSlot`).**
  - UiFramework 단일 자식 Element(`Frame`·`Button`·`SizeBox`·`ScaleLayer`·`SafeArea`·`BackgroundBlur`·`Switcher` slot·`WindowTitleBarArea`·`Overlay` slot·`ScrollView` 내용)가 `SlotPlacement`(여백·정렬)를 직접 든다.
  - 배치는 지금 `SlotPlacement::ArrangeChild`를 쓴다(UE `ArrangeSingleChild`).
  - Widget 층 `AlignedWidgetSlot`은 wrapper 대신 그 값을 Element에 넘긴다. `SlotPresenter` Element는 지운다. `SlotPlacement`는 mixin으로 남긴다.
  - `ScrollBoxWidget`은 이미 `StackSlot` 여백으로 대체할 수 있다.
- **형식 정보(CB2).**
  - `ElementType`(이름, 부모 `ElementType*`)을 둔다. 클래스 선언에 `ROCKS_UI_ELEMENT(Class, Super)`를 쓴다(UE `SLATE_DECLARE_WIDGET` — `using Super`, `using ThisClass`, 정적 `StaticType()`, 가상 `GetType()`). 3.2절의 `ROCKS_UI_` 접두 규칙에 맞는다.
  - `IsA<T>()`는 부모 사슬을 따라간다. `ElementCast<T>(ptr)`는 아니면 빈 포인터, `StaticElementCast<T>(ptr)`는 Release 밖에서 assert한다.
  - 두 함수 모두 `static_assert(IsSameV<typename T::ThisClass, T>)`로 매크로를 빠뜨린 파생 클래스를 막는다(부모 `StaticType`이 상속되어 잘못된 변환이 통과하는 일).
  - policy는 변환 대상 형식이 Element 계열인지로 판정한다(형식 이름이 아니라). UI 모듈의 Element 하향 `static_cast`를 0으로 만든다.
  - 약 186곳 변환 교체는 동작 불변 기계 변경이라 따로 커밋한다.
- **버린 대안.** CRTP 기반 클래스(`ElementBase<Self, Super>`)는 매크로 없이 되지만, 모든 Element의 상속 줄을 바꾸고 다중 상속(`TextInputContext`)과 섞기 어렵다.

### 20.11 스타일 등록부(UW8h1·UW8h2)

- **`StyleRegistry`(UiCore, 공유 — UE `FSlateStyleRegistry`).** 이름 있는 `StyleSet`을 등록한다. 집합은 부모 집합 이름을 가질 수 있고, 찾지 못한 이름은 부모로 올라간다(UE `FSlateStyleSet::ParentStyleName`). 호스트가 한 번 만들어 `UiHostServices`로 모든 Application에 준다. 전역은 아니다.
- **앱 스타일(UE `FAppStyle`).**
  - `UiHostServices`가 활성 집합 이름과 집합 revision을 든다. 기본은 `CoreStyle`이다.
  - 에디터 호스트는 `EditorStyle`(부모 `CoreStyle`)로 바꾼다. 게임 Widget 기본은 `GameWidgetStyle`(UMGCoreStyle)이다.
- **조회 시점.** 측정·배치·그리기 모두 같은 값을 써야 한다. 크기를 정하는 여백·글꼴이 측정과 그리기에서 달라지면 안 되기 때문이다.
  - 부품의 `Args::style`이 nullptr이면 부품은 처음 측정될 때 `FindUiHostServices()`의 활성 집합에서 UE 키로 스타일을 찾아 그 주소를 든다. UE가 생성 때 `FAppStyle`에서 받는 것과 같은 효과다.
  - 집합 revision이 바뀌면(집합 교체·값 변경) `Application`이 테마 revision처럼 전체 Layout·그리기 cache를 무효화한다. 부품은 다음 측정 때 다시 찾는다.
- **형식별 표.** 지금 있는 스타일 구조체 전부를 표로 둔다: 버튼, 체크, 텍스트, 편집 텍스트, 스크롤 막대, 진행 막대, 슬라이더, 콤보, 표 머리, 독 탭, 창, 메뉴, 툴팁. 색·brush·실수·여백 표도 둔다.
- **두 단계.**
  - UW8h1은 등록부·앱 스타일·조회·무효화와 `CoreStyle`의 지금 값(함수 기본값 옮김)이다.
  - UW8h2는 세 집합 값을 4.2절 출처(StarshipCoreStyle.cpp, StarshipStyle.cpp, UMGCoreStyle.cpp)에서 옮기고 출처 행 대조 시험을 단다(UV8 일부). Rocks 구조체가 없는 항목은 15장 빈칸 목록에 남긴다.

### 20.12 UV2·UV3·UV4 시험(UW8i)

8장의 장면 구성 중 이 단계까지 구현된 것을 모두 시험으로 만든다. 기대값마다 UE 근거 행을 단다.
- **UV2 무효화.**
  - 같은 값 설정은 무효화 0회다.
  - 사유 정규화: RenderTransform·Visibility 변경은 부모 Layout이다.
  - 크기 불변 Layout은 부모로 전파하지 않는다.
  - cache root 빠른 경로를 다시 쓰고, volatile Element만 다시 그린다.
  - 기본 경로(창 전체 cache 꺼짐)의 다시 그린 Element 수를 센다.
  - 보이기 묶음 평가 순서(20.6절)와 Hidden 하위 트리 갱신을 본다.
- **UV3 입력 라우팅.**
  - 전처리기 구간과 capture를 본다.
  - preview tunnel·bubble 순서를 본다.
  - 보이기 5종과 겹친 layer를 본다.
  - 비활성 경로 자름(20.5절)과 툴팁의 비활성 무시를 본다.
  - 팝업 바깥 클릭과 IME 조합 중 게임 전처리기를 본다.
  - modal 창은 기능이 생길 때(지금은 없음) 더한다.
- **UV4 focus·navigation.**
  - tab 순서와 bubble 중 navigation 변환(20.7절)을 본다.
  - `OnNavigation` 경계와 기하 navigation(20.9절)을 본다.
  - 규칙 6종, 사용자 2명, 비활성·접힘 Element 포함 장면을 본다.
- **UE 실행 기준값(UD6).** UE 5.8로 빌드한 덤프 프로그램 결과는 UE 빌드 환경이 필요해 이 단계에서 만들지 않는다. 소스 근거 기준값만 둔다. 덤프는 UT1 판정 때 한다(10장 위험 목록에 남긴다).

### 20.13 단계

순서 의존: a → e1·e2(시계), b → c → f, e1 → e2·f. 나머지(d·g·h1·h2)는 독립이고 표 순서로 한다.

| 단계 | 범위 | 완료 판정 |
|---|---|---|
| UW8a | `UiHostServices` 묶음(20.2절), `ui::Clock`·`ManualClock`, `BeginFrame`(delta 자름), 프레임 시각을 그리기 안팎에서 읽기, 애니메이션 부품·툴팁·타이머·`ViewportWorld` 이관, 월드 위젯 World 시간, UiDemo 수동 시계 | 시계 전진 시험(툴팁·타이머·throbber 위상, 긴 간격 자름 뒤 타이머 마감), snapshot 7탭 모두 실행마다 byte 동일 |
| UW8b | 그리기 중 hit grid 등록(`HitGridBuilder`, (layer, 순번) 키), cache root·`RetainerBox` grid 조각, `HitGrid::Update` 제거, 대체 `HitTest` 맞춤 | 그리기 순서 = hit 순서, CachePanel·RetainerBox 보관 중 hit 동일, UiWorld 월드 위젯 hit, 영역 지도 동치 시험 유지 |
| UW8c | `IsInputPathActive` 활성 인자 분리, 비활성 경로 자름, 툴팁·영역 지도 비활성 무시 | 비활성 버튼 클릭 → 부모, 비활성 위젯 툴팁 표시 |
| UW8d | `BindVisibility`, 보이기 먼저 평가, Hidden 갱신 | 접힌 Element가 묶음으로 다시 보임, Hidden Element 원하는 크기 갱신 |
| UW8e1 | `KeyReply`·`KeyEvent`, 기본 navigation 변환, `OnNavigation`·`NavigationReply`, `NavigationConfig`(키·동작), 메뉴·목록·스크롤·슬라이더 이관, `InputAdapter` 대체 변환 제거 | 조상이 방향키를 받지 않음, 초점 없음 → navigation 없음, 키 대응 교체, 메뉴·목록 키보드 회귀 없음 |
| UW8e2 | 게임패드 가상 키·아날로그 반복, `ScreenInputRouter` 정리, 화면 원하는 초점 대상 | 패드 D-pad·스틱·확인·뒤로 시험, 화면 활성화 초기 초점 |
| UW8f | UE `FindFocusableWidget` 이식, 경계 규칙, scope | UE 규칙 장면(겹친 후보·먼 후보·회전·비활성 조상·팝업 scope) 기준값 |
| UW8g | 단일 자식 slot, `SlotPresenter` 제거, `ElementType`·`ROCKS_UI_ELEMENT`·`ElementCast`, policy(변환 교체는 별도 커밋) | wrapper 0, Element 하향 `static_cast` 0(policy) |
| UW8h1 | `StyleRegistry`·앱 스타일·조회 시점·집합 revision 무효화, `CoreStyle` 지금 값 | 집합 교체 시험(측정·그리기 같은 값), snapshot 회귀 없음 |
| UW8h2 | 세 집합 값(UE 출처) | UE 출처 대조 시험 |
| UW8i | UV2·UV3·UV4 시험 묶음(20.12절) | 8장 장면 구성 중 구현된 항목 전부가 시험으로 있음 |
