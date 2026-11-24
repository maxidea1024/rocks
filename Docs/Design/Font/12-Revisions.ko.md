## 12. 다른 문서 개정·범위 밖·개정 이력

### 12.1 다른 문서 개정

개정 1에서 반영한 것:

| 문서 | 바꾼 곳 | 내용 |
|---|---|---|
| [UI 툴킷](../UiToolkit.ko.md) | 2절 모듈 표 TextLayout 행, 3.12절 표 앞, UT2 행, UD3 | TextLayout의 라이브러리 의존을 ClientOnly `TextLayoutBackend`로 옮김, 글꼴 상세의 소유 문서, 글꼴 단계 연결, UD3 버전 문구에 FD14(사용자 확인 대상) 연결 |
| [컴포넌트 목록](../ComponentCatalog/04-Prerequisites.ko.md) | CR18 행 | 선행 문서를 이 문서 7.3절로, 단계 FT5·FT7 |
| [머티리얼](../MaterialSystem.ko.md) | 파라미터 종류 "font — 후속" 행 | `FontSampleParameter`, 노드 단계 FT7 |
| [압축](../Compression.ko.md) | 13절 "WOFF2 글꼴 importer는 글꼴 모듈의 몫" | FD10·FT8 연결 |
| [렌더 기반](../RenderBase.ko.md) | "UI 글꼴" 행 | 7.5절 내장 UI 글꼴 연결 |

해당 단계 착수 전에 고칠 것(그 문서 소유 세션이 반영):

| 문서 | 바꿀 곳 | 내용 | 막는 단계 |
|---|---|---|---|
| [에셋 시스템](../AssetSystem/README.ko.md) 3장 | 모듈 의존 그래프 | `Font`·`FontTools`·`TextLayoutBackend` 행 | FT3 |
| 에셋 시스템 5·10·11장 | 패키지 형식·쿠커·로더 | 지연 bulk entry와 범위 읽기 handle(FP1), 타깃별 payload 제외·빈 `BulkDataRef`(FP6) | FT3·FT4 |
| 에셋 시스템 4장 | 마운트 | `/Engine` 마운트(FP7) | FT7 |
| [빌드 타깃](../BuildTargets/README.ko.md) 3.1절 | "Server 기능 축소" 문단 | 글꼴(`TextLayoutBackend` ClientOnly, 서버 쿠킹 글리프 제외)을 오디오와 나란히 적음 | FT2 |
| [전체 계획](../EngineImplementationPlan.ko.md) 9.6절 | 내장 자원 | 내장 UI 글꼴은 에셋을 거치지 않는 원본 byte 등록(7.5절, FD6 예외) | FT7 |

### 12.2 범위 밖

- 변수 글꼴(variable font) 축 지정·명명 인스턴스 선택, COLRv1·OT-SVG 컬러 글꼴: UE도 지원하지 않는다(1.9절). 요구가 생기면 FreeType `FT_Set_Var_*`·`FT_Get_Color_Glyph_Paint`로 후속 개정한다.
- OS 설치 글꼴 열거·시스템 fallback(플랫폼 글꼴 API): UE 런타임도 쓰지 않는다.
- Canvas 자체 설계(7.2절은 계약만).
- 지역화 글꼴 패키지(UE `FontFace.cpp` 370–377행 — 지역화 패키지 경로를 따르는 쿠킹 파일 이름): Rocks 지역화 에셋 체계가 정해질 때 그 설계에서 다룬다.
- 글꼴 서브셋 생성(쿠킹 때 쓰는 글리프만 남기기): UE에 없다. 필요하면 HarfBuzz subset으로 후속.

### 12.3 개정 이력

- **개정 1(2026-11-24, 초안).** 사용자 지시로 작성. UE 5.8.4 소스(FontFace·Font·CompositeFont·SlateFontInfo·FontCache·FontCacheCompositeFont·SlateTextShaper·SlateFontRenderer·FontImportOptions·임포트 팩토리·TTFontImport·TextRenderComponent·CanvasItem·머티리얼 글꼴 표현식·SlateRHIRendererModule·서드파티 Build.cs)를 대조했다. 사용자 확인으로 FT1(서드파티 도입)은 고정 순서와 무관하게 지금 착수한다. 초안 뒤 서브에이전트 1개로 교차 검증하고 반영했다: 필수 16건(FontInfo 기본값 `All`·크기 단위 96 DPI 포인트, hinting 플래그 표, 조합 탐색 순서(작은 구간 우선·보조 하나만·속성 best match·U+FFFD 재탐색·grapheme 단위), Auto shaping 판정, `_Font` 이름, LazyLoad 기본 동기 표 정리, 서버 분리를 타깃 매크로에서 ClientOnly backend로(BD2·BD3·BD25), CVar 이름 snake_case, `hb-icu.cc` 별도 번역 단위, 할당 hook 한계, 미리 굽기 페이지를 별도 패키지로(FD18), 미리 굽기 시점을 임포트로·문자 파일을 임포트 입력으로, FT1 완료 조건(Linux·서버 검사 시점), 내장 UI 글꼴 FD6 예외)과 권장·참고 다수(SDF 자리표시·spread·MTSDF, 컬러 strike, atlas 밖 글리프·flush 기본값, 자간 예외, 거리장 CVar 기본값, 거리장 alpha 강제값, 페이지 채널, msdfgen 하한 1.12·FreeType 윤곽 연결·STL 예외, ICU 빌드 정의·stubdata·데이터 위치·전역 상태, 서버 ICU 제외, 서버 쿠킹 빈 참조, geometry를 로딩 방식에 따름, 결정성 범위, UE 덤프 미사용, 다른 문서 개정 목록 보강). 결정 FD1–FD19(FD9·FD14 사용자 확인 필요), 단계 FT1–FT8, 선행 FP1–FP7, 시험 FV1–FV17.
