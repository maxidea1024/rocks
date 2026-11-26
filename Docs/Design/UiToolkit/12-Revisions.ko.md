## 12. 개정 기록과 기존 스펙의 개정 위치

**초안 2.8(2026.11.26):** 18장(렌더 경계 — UW6 상세 설계, 리뷰 21건 반영)을 더했다. 3.4절 draw buffer 순환 문장을 복사 방식으로 고쳤다. 9.1절 UW6 행의 완료 판정을 RenderGraph 사실(raster pass마다 장치 기록 하나)에 맞게 고쳤다.

**초안 2.7(2026.11.26):** 17장(SVG 벡터 이미지와 아이콘)을 더했다. UD10을 사용자 결정으로 자체 제작 SVG 아이콘으로 개정했다. 9.1절에 UW4s(SVG 경로·아이콘)와 UW4f(초점 표시·텍스트 입력과 IME, 사용자 지적)·UW4g(드래그·드롭 틀, 사용자 요청) 단계를, 16.2절에 Icons·Drag & Drop 탭을 더했다.

**초안 2.6(2026.11.25):** 문서를 장별 폴더로 옮겼다(500줄 규칙, 내용 변경 없음 — 별도 커밋). 단계 순서보다 먼저 만든 UI 코드를 리뷰하고 9.1절(UW0–UW7 재작업 단계·병합 때 정한 터치 스키마·capture 취소 의미)을 더했다. 상태 줄을 "선행 구현 재작업 중"으로 고쳤다.

**초안 2.5(2026.11.24):** [글꼴 설계](../Font/README.ko.md) 개정 1에 맞춰 2절 TextLayout 행의 라이브러리 의존을 ClientOnly `TextLayoutBackend`로 옮기고(서버 타깃 제외 — 빌드 타깃 BD25), 3.12절에 글꼴 상세의 소유 문서를 적고, UT2 행과 UD3에 글꼴 설계 단계·버전 결정(FD14, 사용자 결정 2026-11-24)을 연결했다.

**초안 2.4(2026.11.22):** [노드 그래프](../NodeGraph/README.ko.md) 개정 2의 GR1을 반영했다. 2절 모듈 표의 EditorFramework 책임에서 그래프 Element를 빼고 `GraphView` 모듈(Editor 계층) 행을 더했다. 6절 "그래프 편집" 행을 `GraphView` 모듈과 `NodeGraph` 데이터 모델 기준으로 고쳤다. 9절 UT6에서 그래프 Element를 빼고, 그래프 편집 화면은 노드 그래프 NG5–NG7로 이었다.

**초안 2.3(2026.11.20):** UD15의 확정 범위를 레벨 스크립트가 실제로 쓰는 CLASS 지정자 `NotPlaceable` 하나로 좁혔다. 나머지 표시 메타·편집 메타는 권장안으로 되돌렸다(사용자 결정).

**초안 2.2(2026.11.20):** [레벨 스크립트](../LevelScript.ko.md) 확정(LD3)에 맞춰 7.3절 (e) `NotPlaceable` 행에 사용처(`LevelScript`)와 Outliner 숨김이 편집기 규칙이라는 점을 적고, `NotPlaceable`을 포함한 UD15를 권장안대로 확정했다.

**초안 2.1(2026.11.19):** UE 5.8 `Templates` 대조로 7.3절(CU1·CU2·CU9 보강: Inspector 노출과 편집 위치·도움말·타입·함수 표시 메타·분류 계층·타입 단위 편집 메타·스크립트 재정의 함수·남은 공백 목록)과 결정 UD13–UD17을 더했다. 초안 작성 뒤 서브에이전트 3개(빌드 문서 UE 대조, 7.3절 UE·Rocks 대조, Templates 독립 재대조)로 교차 검증하여 설명 키 통일(`Help`), `DisplayOnly`·`ReadOnly` 분리, 노출 명시(`Edit`), 저장 위치 기준 문맥·Designer 문맥, 편집기 쓰기 경로 거부, 중첩 전파 규칙, 편집 메타데이터 블록, 스크립트 재정의 함수 계약을 반영하고 UE 인용 위치를 바로잡았다. 반영본을 서브에이전트 1개로 다시 검증해 대리 클래스의 등록·저장 관계, interface 다중 상속 허용, `category` 실행 소비처, Category 표시 순서(Rocks 고유), `PrioritizeCategories`·펼침/접힘 병합 규칙, 시험 fixture 예외를 반영했다.

이 개정에 맞춰 함께 고친 문서: [Prefab](../Prefab.ko.md) 개정 4(3.7·3.11·3.13절 — 편집 위치 규칙), [빌드 타깃](../BuildTargets/README.ko.md)(새 문서, `ROCKS_WITH_EDITOR_DATA`). 구현 전에 맞출 문서: [Sparks](../SparksLanguage/README.ko.md) — `@edit(Scope:)` 표기, `ScriptOverridable` 함수와 `override fn`의 대응, 타입·필드 `Help`와 `///` 문서 주석의 기록, 10.1절 예시의 `Edit`이 이 절의 지정자임을 표기. [리플렉션 기반](../ReflectionFoundations.ko.md) 지정자 표 — `Edit`·`DisplayOnly`·`EditScope`·`Help`, CLASS 편집 메타 지정자, `ScriptOverridable`·`NativeOnly`. 빌드 쪽 공백(타깃 종류·의존 공개 범위·플러그인 활성 조건)은 [빌드 타깃](../BuildTargets/README.ko.md)으로 분리했다.

**초안 2.0(2026.11.16):** 초안 1.0의 단일 층 설계를 폐기하고 UE 5.8 두 층 구조로 전면 재작성하였다. 외부 엔진 실행 결과와 비교하던 시험 GO1–GO6, 테마 상속·자식 Widget 배치 속성·`Affects` 효과 플래그 체계(UE 무효화 사유로 대체), AccessKit 후보를 삭제하였다. CU1–CU9의 ID와 7절 번호는 유지하였다. UD7·UD8은 같은 사안으로 유지하였다. UR4·UR5·UD5·UD6은 번호를 새 사안에 다시 사용하였다(이전 의미: UR4 세부 동작 차이, UR5 비교 기준 버전 변동, UD5 외부 코드 차용 방식, UD6 비교 기준 버전). 초안 작성 후 서브에이전트 3개(Element 층 UE 대조, Widget·에디터 층 UE 대조, Rocks 문서 정합성)로 교차 검증하여 무효화 기본 경로, hit grid 셀 크기, 정규화 표, 기본 스타일 출처, 글꼴 구성, 결합 표, Widget 풀, lane 모델 등을 바로잡았다.

이 개정에 맞춰 다음 문서를 고쳤다.

| 문서 · 절 | 개정 전 | 개정 후 |
|---|---|---|
| 전체 계획 표지 · 1.4 · 1.9 · 2.5 · 3.1 · 3.2 | 단일 층 툴킷, 하위 viewport widget, UI·TextLayout 모듈 | 개정 1.9 기록, UE 5.8 두 층 구조, viewport Element, UiCore·UiFramework·UiWidget·UiScreens 모듈 |
| 전체 계획 21 · 21.2 · 21.4 · 21.5 · 22.1 | 단일 층 Control 체계, 자식 widget 배치 속성, 단일 층 입력 전파 순서, GO1–GO6 | Element·Widget 층, slot 배치 속성, tunnel·bubble 라우팅, UV1–UV9 |
| 전체 계획 29.2 · 30.1 · 30.2 · 31 · 32 · 33.2 · 33.9 · 33.14 · 34 | B12·M3·R08·추적표의 GO1–GO6, 외부 엔진 참고 자료 S75–S77 | UV1–UV9, 개정 1.9 기록, S75–S77 본문 인용 철회 표시 |
| 상세안 9 · 19.2 · 22.1 · 요구 추적 | 하위 viewport widget, 외부 엔진 비교 시험 | viewport Element·사용자 focus 경로, UV1–UV9 |
