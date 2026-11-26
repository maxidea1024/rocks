# UI 툴킷 — 게임 UI와 에디터의 단일 UI 체계

UE 5.8의 두 층 UI 구조(비객체 Element 층 + Object Widget 층)를 Rocks Core 위에 재구현

**설계 기준일 2026.11.26 · 초안 2.7 · 상태: 설계 제안 / 결정 UD1–UD12 확정, UD15는 `NotPlaceable`만 확정, UD13–UD17 권장안 · 선행 구현을 UT1 진행 중으로 재작업 중(9.1절 UW 단계, 에디터 룩·갤러리 우선)**

Rocks의 게임 UI와 에디터(Rocks Studio)는 하나의 UI 툴킷을 사용한다. 구조는 UE 5.8의 UI 체계를 그대로 따른다. 하위 층은 Object가 아닌 경량 Element로 레이아웃·그리기·입력·focus·텍스트를 처리하고, 상위 층은 `rocks::Object`인 Widget이 Element를 감싸 에셋·리플렉션·GC·자체 언어 바인딩·Designer 편집을 제공한다. 에디터는 Element 층을 직접 사용하고, 게임 UI는 Widget 층으로 작성한다. 기본 스타일과 룩도 최초 구성에서는 UE 5.8의 기본값을 따른다(4.2절).

초안 1.0(2026.11.11)의 단일 층 설계는 검토 결과 기준 구조로 삼지 않기로 하였다. 변경 내역은 12절에 기록한다.

| 항목 | 내용 |
|---|---|
| 기준 문서 | [전체 구현 계획](../EngineImplementationPlan.ko.md) 개정 1.9 · [Core·Object·Input 상세 구현 계획](../CoreObjectInputDetailedPlan.ko.md) · [Clang 생성 Reflection](../../Implementation/ReflectionMetadata.ko.md) · [리플렉션 B9](../ReflectionExtensions.ko.md) · [Sparks 언어 설계](../SparksLanguage/README.ko.md) |
| UE 기준 | 로컬 클론 `C:\github\UnrealEngine` 5.8.4-release(commit `f45729c0b6f0`). 이 문서의 UE 경로는 `Engine/Source/` 기준 상대 경로이며, 행 번호는 해당 클론의 값이다 |
| Rocks 상태 | 단계 순서보다 먼저 만든 UI 코드(UiCore·UiFramework·UiWidget·UiScreens·UiRender·UiWorld·UiDemo, 약 34k줄)가 있다. 9.1절의 리뷰 결과대로 UT1 진행 중으로 보고 UW 단계로 이 문서에 맞춘다. 글꼴은 [글꼴 설계](../Font/README.ko.md) FT1·FT2가 구현됐다. `PlatformWindow`은 Win64·Android 창, Win64 clipboard·cursor·IMM 조합 창(DPI 없음) |
| 이름 | Rocks 고유 이름을 사용한다(UD12). UI 계층 매크로는 `ROCKS_UI_` 접두. UE 이름(모듈·타입·매크로)은 대응표와 근거 열에만 적는다 |
| UE 원본 취급 | UE 소스는 구조·동작 확인을 위한 열람·대조에만 사용하고 코드를 옮기지 않는다. Epic 저작 자원(아이콘·이미지·`Engine/Content`)은 포함하지 않는다 |
| 식별자 | 단계 UT0–UT6, Core 선행 계약 CU1–CU9, Core·플랫폼 보강 CB1–CB4·PW1–PW8, 검증 UV1–UV9, 위험 UR1–UR8, 결정 UD1–UD17 |

## 요약

1. **구조:** UE 5.8과 같은 두 층이다. Element 층(UiCore·UiFramework·TextLayout)은 공유 포인터로 소유되는 비객체 트리이고, Widget 층(UiWidget)은 Element를 감싸는 Object다. 배치 속성은 부모·자식 관계인 slot에 둔다(UD1·UD2 확정).
2. **프레임:** 입력 라우팅 → 원하는 크기 계산(자식 → 부모) → 배치(부모 → 자식)와 그리기 목록 기록 → 그리기 중 hit grid 갱신 → batch → Render lane 제출 순서다(3.4절). UE와 같이 기본은 매 프레임 전체를 다시 계산하는 경로이며, 무효화 사유 플래그는 cache root 안에서 작업을 줄인다.
3. **에디터:** 독·명령·메뉴 기반·Inspector·Transaction·viewport를 UE 에디터와 같은 분담으로 구성한다(6절). 독·명령은 런타임 층(UiFramework), Object 모델에 닿는 Inspector·Transaction·자산 편집기·메뉴 등록은 에디터 전용(EditorFramework)이다.
4. **기본 룩:** 최초 구성의 색·글꼴·크기·여백·모서리 규칙은 UE 5.8 기본 스타일 값과 같게 맞춘다. 글꼴은 UE와 같은 공개 글꼴을 원 배포처에서 받고, 아이콘은 Epic 자원 대신 Rocks가 직접 그린 SVG를 nanosvg로 래스터화한다(UD10, 17장).
5. **현 단계:** Core 작업 중 Widget 층과 에디터가 의존하는 리플렉션 계약 CU1–CU9를 확정한다(7절). UiCore·UiFramework는 창·GPU 없이 시험할 수 있으므로 Foundation Gate 직후 RHI와 병행 착수한다(9절). 검증은 UE 5.8 소스 근거를 단 고정 기준값으로 수행한다(8절).

## 장 목록

- [1. 방향과 원칙](01-Principles.ko.md)
- [2. 계층 구조와 모듈](02-LayersAndModules.ko.md)
- [3. Element 층](03-ElementLayer.ko.md)
- [4. 스타일과 기본 룩](04-Style.ko.md)
- [5. Widget 층](05-WidgetLayer.ko.md)
- [6. 에디터 프레임워크](06-EditorFramework.ko.md)
- [7. Core 선행 계약](07-CorePrerequisites.ko.md)
- [8. 검증](08-Verification.ko.md)
- [9. 단계 계획](09-Stages.ko.md)
- [10. 위험과 대응](10-Risks.ko.md)
- [11. 결정 항목](11-Decisions.ko.md)
- [12. 개정 기록과 기존 스펙의 개정 위치](12-Revisions.ko.md)
- [13. 참고 자료](13-References.ko.md)
- [14. 에디터 스타일 값(UE 5.8.4 Starship)](14-EditorStyleValues.ko.md)
- [15. 에디터 부품 빈칸 목록](15-EditorPartsGap.ko.md)
- [16. UI 갤러리 구성](16-Gallery.ko.md)
- [17. 벡터 이미지(SVG)와 아이콘](17-VectorImages.ko.md)
