## 9. 단계 계획

UiCore·UiFramework는 창·GPU 없이 시험할 수 있으므로 에디터 일정의 임계 경로에 있는 툴킷을 Foundation Gate 직후 RHI와 병행 착수한다. 일정 열은 전체 계획 30.1절의 M 단계 표기다.

| 단계 | 범위 | 선행 | 완료 판정 | 일정 |
|---|---|---|---|---|
| UT0 | Core 계약 CU1–CU9, Core 보강 CB1–CB3 | 현재 Core 작업 | 리플렉션 하네스에서 사용 예제·음성 대조·직렬화 왕복 통과 | M1–M2 |
| UT1 | UiCore·UiFramework 기반: Element·slot·자식 목록·속성·무효화·Geometry·레이아웃 Element·그리기 목록·clip·cache root·hit grid·`ui::Application` 라우팅·사용자·focus·navigation·drag·menu stack·주입 시계·스타일 체계·batcher·빈 renderer·가짜 `ui::Platform` | CB2 | UV1–UV4, UV6의 CPU 부분 통과 | Foundation Gate 직후 |
| UT2 | 출력: PW1–PW7, TextLayout(FreeType·HarfBuzz·ICU·msdfgen adapter, CB4·CU7 — 글꼴 부분은 [글꼴 설계](../Font/10-Stages.ko.md) FT2·FT6, 서드파티 도입 FT1은 먼저 할 수 있다), UiRender(Render lane 제출·atlas·render target·viewport 합성), SVG, 기본 핵심·게임 스타일(4.2절) | UT1, RHI 기본 | UV5, UV6 픽셀 비교, UV8 핵심·게임 스타일, 한국어 IME 조합 | M3 |
| UT3 | 표준 Element: 텍스트 편집·목록/트리 가상화·스크롤·콤보·숫자 입력·색 선택·메뉴·tooltip·알림·viewport Element·독 배치와 저장·명령 | UT2 | 에디터 규모 Element 수에서 레이아웃·그리기 예산 측정(UR3), UV9 독·명령 부분 | M3 |
| UT4 | EditorFramework: 셸·자산 편집기 host·메뉴 등록·Inspector(CU1·CU6)·Transaction 연결·에디터 스타일·EditorViewport. 전체 계획 22.2절의 M3 기능(Outliner·Inspector·Asset Browser·viewport·gizmo·log) | UT3, UT0 | M3 gate 「import→edit→save→cook→run」을 Windows·macOS에서 통과, UV8 에디터 스타일 | M3 |
| UT5 | UiWidget·UiScreens: Widget·slot 객체·UserWidget·WidgetTree·WidgetAsset·결합 표·검증·Widget 풀·화면 layer(WorldIntegration 연동)·화면 스택·입력 라우터·입력 장치 전환, Designer 기초(palette·hierarchy·canvas·Inspector 재사용) | UT4, CB1·CB3 | UV7, UV9 화면 부분, 전체 계획 21.5절 HUD·menu·설정 sample | M3 착수 · M4 완료 |
| UT6 | 완성: ViewModel 바인딩·UI 애니메이션·서식 텍스트·Widget 층 엔트리 목록·월드 공간 Widget·cache/retained panel 활용·곡선·타임라인 Element·코드 편집 Element·접근성(PW8)·전문 Designer | UT5 | 전체 계획 21.5절 필수 sample 5종·14.9절 MAT 시험·B12 | M5–M6 |

그래프 편집 화면은 [노드 그래프](../NodeGraph/14-Stages.ko.md) 단계 NG5–NG7이다. NG5는 UT3과 EditorBackend Transaction(UT4의 headless 부분, 노드 그래프 NG4) 뒤에, NG6은 공용 2D 격자([공간 색인](../SpatialIndex.ko.md) 5.3절) 뒤에 착수한다.

### 9.1 선행 구현 재작업(UW 단계, 2026-11-25)

UI 코드 일부가 9절 순서(UT0·CB2 선행)보다 먼저, 글꼴 시스템이 없던 때 만들어졌다. UT1·UT3 상당 부분, UT2 일부(UiRender), UT5·UT6 일부가 섞여 있다. 2026-11-25 리뷰(영역별 리뷰어 4명 + policy·format 검사)의 결론은 "동작은 하지만 병합 불가, 기반 품질은 좋아 다시 쓰지 않고 경계를 옮긴다"이다. 이 절은 그 편차와 해소 단계다. UW가 끝나면 UT1 완료 판정(UV1–UV4, UV6 CPU)으로 넘어간다.

**편차(리뷰 결과 요약).**
- 렌더 경계(2·3.4·3.7절 위반): `graphics::Device&`가 UiCore 공개 헤더(`WidgetSurfaceScene::Draw`)와 UiWidget(`ViewportWorld`)에 나와 있다. UiRender(`WidgetSurfaceHost`)가 Element 트리를 순회하고 World를 tick한다. `DrawBatchBuffers`가 Game lane `DrawBatch` 참조를 보관한다. 표면마다 따로 제출하고 `RenderLane`·RenderGraph·`UiRenderer` 계약이 없다. UiRender 객체가 장치를 멤버로 보관한다.
- 셰이더: UI 셰이더를 HLSL·GLSL 원문으로 코드에 넣었다. engine-runtime은 런타임 셰이더 컴파일러를 제거했으므로(셰이더 시스템 SH2e2) 이 경로는 더는 동작하지 않는다.
- 텍스트(3.12절): worktree 판 `TextLayout`(GlyphAtlas·GlyphAtlasCache)이 글꼴 시스템의 같은 이름 모듈과 겹친다. 데모가 GDI로 래스터한다. `TextLabel`은 shaping 결과와 renderer handle을 받고 배율을 무시한다.
- 창·입력(3.8·3.11·7.2절): OS 메시지 스레드 = 게임 스레드 가정(`WM_NCHITTEST`에서 UI 트리 판정), IMM push형 IME, DPI 없음, `InputEvent`의 공유 문자열, 창 단위 `WaitForEvents`, backend 안의 전체 화면 전환.
- Element 동작: 실제 시계 직접 사용(3.10절 주입 시계 아님), 비활성 Element hit가 UE와 다름, Element slot 대신 wrapper, hit grid를 그리기와 따로 만듦, Hidden 속성 갱신 생략, navigation 변환 위치, 스타일 체계(4절) 없음.
- Widget 층: 속성 접근이 손으로 쓴 이름 표(UT0 CU 지정자 없음), 텍스트 편집 Element가 UiWidget Private, UiScreens 입력 설정 미적용, Element 소멸자의 사용자 코드, UD12와 다른 이름(`Retainer`·`CanvasPanel`·`ScaleBox`·`WrapBox`·`InvalidationBox`).
- 규칙: 빌드 파일 한국어 주석, 데모 표시 문자열 한국어, format 위반, 이름 없는 수치, 시험 부족(UV 기준값 없음).

**우선순위(사용자 결정 2026-11-25).** UI는 에디터부터 쓴다. 게임 UI는 그 뒤다. 목표는 UE 5.8 에디터의 룩과 기능 수준이고, 에디터에서 어떻게 보이는지가 가장 중요하다. 그래서 에디터 룩(UW3)과 모든 위젯을 탭별로 시험하는 갤러리(UW4)를 렌더 경계·입력 계약 정리보다 앞에 둔다. 데모 화면(궤도 시각화)은 위젯 시험에 쓸모가 없어 갤러리로 바꾼다.

**병합 때 정한 것(UW0).**
- 터치는 Input의 `TouchBegan`·`TouchMoved`·`TouchEnded`(손가락 번호, `cancelled`)를 쓴다([터치 입력](../TouchInput.ko.md) 3.1절). UI 쪽 `TouchBegin`·`TouchMove`·`TouchEnd`·`TouchCancel`과 `pointerId` 필드는 지웠다. `InputAdapter`가 손가락 번호를 pointer 번호로 쓰고 `cancelled`를 Cancel로 바꾼다. UI capture는 (source, user, pointer)로 찾으므로 마우스 0번과 손가락 0번은 겹치지 않는다.
- 마우스 capture 상실은 취소(`PointerCaptureLost`)다(UE `OnMouseCaptureLost`). engine-runtime의 "눌린 버튼마다 가짜 MouseUp" 방식은 버린다. 가짜 뗌은 Button을 클릭으로 처리한다. `InputState`는 취소를 받으면 눌린 버튼을 뗀 것으로 기록한다(클릭 없음).
- `PlatformWindow`의 창 기본 구현(`Window.cpp`)은 `Private/Common`으로 옮겼다. Android는 clipboard·조합 창·drag 이미지를 아직 구현하지 않아 Unsupported를 낸다(`AndroidUiServices.cpp`).
- 병합 때 `MaterialBase`를 클래스 단위로 export했으나(빌드 타깃 BS8d3 — 다른 모듈의 파생) UW1a에서 파생 클래스(`RetainerEffectMaterial`)를 지우며 되돌렸다.

| 단계 | 범위 | 완료 판정 |
|---|---|---|
| UW0 | engine-runtime 병합·충돌 해결, 문서 분할, 다른 세션이 고치다 멈춘 선언 복구 | 병합 커밋(UiRender 셰이더 경로만 미해결) |
| UW1a | UI 셰이더를 엔진 셰이더 타입으로: `/Engine/Private/Ui/*.rsf`(엔진 셰이더 A19 `UiElements` 일부를 SLU보다 앞당김), 타입 desc는 UiRender, 파이프라인은 `ShaderSet`·`PipelineCache`. 효과 셰이더 원문(`RetainerEffect`·`RetainerEffectMaterial`, 안정 ID는 retire) 제거 — 효과는 3.6절대로 Material 참조(Material 셰이더가 생길 때까지 효과 없음, 색조는 남김). 셰이더 공급은 engine-runtime의 엔진 셰이더 호스트와 Developer `DevelopmentShaderProvider` 모듈이다([RenderBase](../RenderBase/09-ShaderRuntime.ko.md) 9.12절 RR11 — 같은 이름으로 따로 만든 추출본은 병합 때 버렸다). UiDemo는 Sandbox self-test처럼 호스트를 붙이고 엔진 모듈 묶음을 시작한 뒤 장치마다 `EngineRenderResources`로 집합을 열고, Release 밖에서만 공급자 모듈을 링크하며 워커를 선행 타깃으로 빌드한다 | EngineTests 빌드·UI 시험 통과, UiDemo 실행 |
| UW2 | 텍스트를 글꼴 시스템으로: worktree 판 GlyphAtlas·GlyphAtlasCache, UiRender `GlyphAtlasTexture`, 데모 DemoFont(GDI) 삭제. UiRender `GlyphCacheTextures`가 `text::GlyphCache`의 텍스처(Alpha R8·Color BGRA8 sRGB·거리장 RGBA8)를 GPU에 두고 바뀐 텍스처만 올린다(`TakeDirtyRegion`이 바뀐 것을 알려 준다). Graphics `UpdateTexture`가 mip 단위라 지금은 바뀐 텍스처 전체를 올린다 — 영역 갱신은 Graphics 업로드 ring(GX8)에 영역 API가 생기면 바꾼다. 데모는 `TextLayoutBackend`(FreeType·HarfBuzz·ICU)와 시험 글꼴(Roboto·나눔고딕·Noto Naskh Arabic·컬러 이모지) 조합 글꼴로 `ShapeText`→`GlyphCache`를 거쳐 기존 `TextLabel::SetShapedText`에 넘긴다(grapheme 클러스터·줄 경계·BiDi 수준). `TextLabel`이 `FontInfo`·크기를 받아 최종 배율로 스스로 shaping하는 것(3.12절)은 그리기 데이터에서 renderer handle을 빼는 UW4와 함께 한다. `TextLayoutModel`(BiDi·hit test·caret)은 TextLayout 모듈에 남긴다 | 데모 self-test 통과, 한글·영문·아랍어 레이블 표시, GDI 코드 0 |
| UW3 | 에디터 룩(4절): `StyleSet`·`StyleRegistry`(`ui::Application` 소유)·`ThemeColors`·`ColorRef`·Element 스타일 구조체(버튼·체크·텍스트·편집·스크롤·진행·슬라이더·콤보·표 머리·독 탭·메뉴·tooltip)를 만들고, 값은 UE 5.8 에디터 스타일(StarshipCoreStyle·StarshipStyle, 테마 색 StyleColors.cpp, Roboto 조합 글꼴과 크기)에서 출처 행과 함께 옮긴다. 코드에 박힌 색·크기를 모두 스타일 조회로 바꾼다. 게임 Widget 기본 스타일(UMGCoreStyle)은 그 뒤 | 기본 값 표 = UE 출처 대조 시험(UV8 일부), 스타일 조회 없는 색 0 |
| UW4 | UI 갤러리: UiDemo(궤도 화면)를 지우고 UE 5.8 `SStarshipGallery`(AppFramework Testing) 구성의 Rocks 갤러리 프로그램으로 바꾼다. Element 층(에디터가 쓰는 층)으로 짓고 탭마다 범주를 둔다: 색(테마 slot), 텍스트(글꼴 크기·굵기), 위젯(버튼·체크·라디오·콤보·편집·숫자 입력·슬라이더·진행·스크롤·tooltip·메뉴), 목록(목록·트리·표 머리), 배치(패널·독·탭·분할), 에디터 부품(컬러 피커, Inspector·디테일 패널 — 분류 접기·속성 행·벡터·색·열거·객체 참조 편집, 사용자 요청 2026-11-25). 에디터 자체가 아니라 에디터에 쓰일 부품과 기본 컨트롤의 진열장이고, 스타일과 동작은 UE 에디터를 예상할 수 있는 수준으로 맞춘다. self-test가 탭마다 화면을 캡처한다 갤러리를 만들며 UE 5.8 에디터가 쓰는 부품(Slate·ToolWidgets·EditorWidgets·PropertyEditor·AppFramework)과 Rocks Element 층을 대조해, 에디터를 지을 때 빠질 부품·틀(메뉴·툴바 빌더, 명령, 탭 배치 저장, 디테일 사용자 정의, 속성 handle 등)의 빈칸 목록을 만든다(사용자 목적 2026-11-25) | 모든 구현 위젯이 갤러리에 있음, 탭별 캡처, UE 갤러리와 나란히 비교, 에디터 부품 빈칸 목록 |
| UW5 | 규칙: 빌드 파일 영어, 데모 문자열(지역화 표나 시험 fixture), format·빈 줄, 이름 있는 상수 | policy·format 통과 |
| UW6 | 렌더 경계: UiCore에 그래픽 타입 없는 `UiRenderer` 계약(3.7절), UI lane이 그리기 묶음을 복사해 `RenderLane::Post`, UiRender는 processor 소유·`RenderFrameContext::Device()`, RenderGraph pass로 프레임당 한 번 제출, viewport는 `ViewportElement`·`ViewportSource`(3.11절) | lane 분리 시험, 제출 수 = 1/프레임 |
| UW7 | 창·입력 계약: 영역 판정은 게시한 사각형으로 backend가 판정, 스레드 계약 명시, DPI(PW2), pull형 `TextInputContext`(PW3), `WaitForEvents`를 펌프 수준으로, 전체 화면 명시 API, 이벤트 문자열은 프레임 버퍼 offset | Win64 동작 유지, Android·mac 구현 가능성 문서 대조 |
| UW8 | Element 동작을 UE와 맞춤: 주입 시계, 비활성 hit·Hidden 속성·navigation 위치, 그리기 중 hit grid 등록, Element slot·CB2, StyleSet(4.2절 UE 기본값) | UV2·UV3 시험 |
| UW9 | Widget 층: UD12 이름, 텍스트 편집 Element를 UiFramework로, UiScreens 입력 설정 적용, 소멸자 사용자 코드 제거. 손으로 쓴 속성 표는 UT0 CU 지정자 뒤 교체(그때까지 임시) | UV7 음성 시험 |

**UW3 하위 단계.** UW3a 둥근 상자 그리기, UW3b `StyleSet`·스타일 구조체·기본 테마와 에디터 스타일 값(14장), UW3c 위젯의 박힌 색·크기를 스타일 조회로 바꿈.

**UW3a 둥근 상자(UE `FSlateRoundedBoxBrush`, 14장 5절).**
- `Brush`에 `BrushMode::RoundedBox`와 외곽선 설정(모서리 반경 4개 — 왼쪽 위·오른쪽 위·오른쪽 아래·왼쪽 아래 순, 색, 두께, 반경 방식 Fixed·HalfHeight)을 더한다. 반경을 주지 않으면 HalfHeight(높이의 절반 — 알약·원)다(UE와 같다).
- `PaintBox`에 둥근 상자 값(로컬 크기, 외곽선 두께·색, 반경)을 싣는다. `DrawBatch`는 이 값이 다르면 그리기 명령을 나눈다(UE도 ShaderParams가 배치 키라 크기·반경·두께가 다른 상자는 따로 그린다). 채움색은 정점 색, UV는 상자 안 로컬 0..1이다.
- 셰이더는 엔진 셰이더 `/Engine/Private/Ui/UiRoundedBox.rsf`(그리기 상수: 화면 변환, (0, 두께, 크기 x, 크기 y), 반경 4, 외곽선 색)다. 식은 UE `SlateShaderCommon.ush` 84–119행과 같다: 외곽선은 상자 안쪽, 테두리 AA 반폭 1.0·채움 0.5(로컬 단위), straight 알파 출력과 SrcAlpha 블렌드. 텍스처를 곱한 둥근 상자(UE는 브러시 텍스처를 곱한다)는 쓰는 곳이 생기면 더한다.

**UW3b 스타일 기반(설계, 구현 완료).**
- `StyleBrush`(UiCore): 그리기 방식·채움·외곽선 색을 `ColorRef`(테마 slot)로 들고, 그릴 때 테마로 `Brush`를 만든다(UE `FSlateBrush`의 `FSlateColor` tint). 둥근 상자 반경·외곽선 두께·이미지 resource를 함께 둔다.
- 스타일 구조체: `ButtonStyle`(상태 넷의 brush, 보통·눌림 여백, 상태별 전경색 — UE `FButtonStyle`), `TextStyle`(글꼴 typeface 이름·크기, 색 — UE `FTextBlockStyle`의 글꼴·색 부분), 그 밖은 UW3c에서 위젯을 바꿀 때 하나씩 더한다.
- `StyleSet`: 이름으로 형식별 스타일을 찾는 표다(UE `FSlateStyleSet::GetWidgetStyle`). `AddCoreStyles(set)`가 14장 UE 값(출처 행을 주석으로)을 넣는다. 위젯 기본값은 같은 값의 상수 함수(`DefaultButtonStyle()` 등)로 두어, 스타일 집합 없이 만든 위젯도 UE 룩이다(UE도 인자를 생략하면 핵심 스타일의 기본 항목을 쓴다).
- 테마: `PaintList`가 그릴 때 쓸 `ThemeColors`를 들고(지정하지 않으면 UE 다크 기본 테마), 위젯은 `OnPaint`에서 그 테마로 색을 푼다. 테마의 소유자는 `ui::Application`이다(전역 등록부 없음 — 4.1절).
- 구현에서 정한 것: 글꼴 typeface는 문자열 대신 `Typeface` 열거(Regular·Bold·Italic·BoldItalic·Light)로 둔다. 스타일 값이 이름 수명을 갖지 않게 하려는 것이고, 글꼴 계층이 열거를 합성 글꼴의 typeface 이름으로 바꾼다(UE `TypefaceFontName`과 같은 값). 버린 대안은 `StringView` 이름이다(사용자가 만든 스타일에서 매달린 view가 된다). `AddCoreStyles`의 키는 UE 키 그대로 "Button"·"PrimaryButton"·"SimpleButton"·"NormalText"·"SmallText"·"Text.Large"·"PrimaryButtonText"다(StarshipCoreStyle.cpp 1333–1339행, 1503–1511행). 없는 이름을 `Get*`로 찾으면 형식 기본값을 돌려준다(UE는 경고 뒤 기본값). 테마를 바꾸면 `CacheRoot::SetTheme`이 기록을 모두 버린다. 색이 바뀌므로 정점만 갱신할 수 없기 때문이다. 시험 `UiCore_*Style*`·`UiCore_PaintListThemeDefaultsToUeDark`가 14장 값을 대조한다(UV8).
