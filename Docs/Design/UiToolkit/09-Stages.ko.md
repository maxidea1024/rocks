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
