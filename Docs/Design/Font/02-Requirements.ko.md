## 2. 요구와 범위

### 2.1 요구

| ID | 요구 | 출처 |
|---|---|---|
| FR1 | UE 5.8 글꼴 기능 전부: FontFace·Font 에셋, 조합 글꼴(기본·fallback·문자 범위·culture 보조 typeface), Offline·Runtime 두 cache 방식, 로딩 방식 셋, hinting 다섯, 배치 방식 둘, ascent·descent 재정의, 취소선 높이, 래스터 모드 넷(Bitmap·MSDF·SDF·근사 SDF)과 기기별 재정의, 외곽선·그림자에 외곽선·기울임·자간·고정폭·last resort fallback, 컬러 글리프, TTC·OTC 하위 face | 사용자 지시 2026-11-24 |
| FR2 | 임포트: `ttf`·`ttc`·`otf`·`otc` → FontFace, 같은 이름 Font 자동 생성(묶음 선택 포함), 미리 구운(Offline) 글꼴 생성, 재임포트 | 1.3절 |
| FR3 | 쿠킹: 로딩 방식별 저장 위치, 거리장 전처리 geometry를 쿠킹 데이터에, 서버 타깃은 글리프 데이터 없음 | 1.2절, [전체 계획](../EngineImplementationPlan.ko.md) 9.6절 908행 |
| FR4 | 사용처: UI 텍스트(UiToolkit), Canvas 텍스트, `TextRenderComponent`, 머티리얼 글꼴 표현식 셋, 엔진 글꼴 설정, 엔진 기본 글꼴과 내장 UI 글꼴 | 1.4·1.5절, 계획 9.6절 |
| FR5 | 라이브러리는 UE와 같은 구성(FreeType·HarfBuzz·ICU·msdfgen·nanosvg), 공개 API에 라이브러리 타입을 노출하지 않음 | UiToolkit UD3 |
| FR6 | 시험은 OS 글꼴 설치 여부와 무관(고정 글꼴 파일), CPU만으로 face·shaping·래스터·거리장·조합 탐색을 검증 | 계획 2531행, [String·Text](../../Implementation/StringAndText.ko.md) 45행 |
| FR7 | 결정성: 같은 원본·설정·라이브러리 버전이면 미리 구운 글꼴(임포트 결과)과 전처리 geometry(쿠킹 결과)가 byte 단위로 같음(float 경로의 범위는 5.2·5.3절) | 에셋 너겟 키([에셋 시스템](../AssetSystem/09-AssetNuggets.ko.md)) |
| FR8 | 서버 타깃(Windows·macOS·Linux)은 FreeType·HarfBuzz·ICU·msdfgen·글꼴 renderer를 링크하지 않음(UE SlateCore와 같다) | [빌드 타깃](../BuildTargets/README.ko.md) 3.7절, UE 같음 |

### 2.2 범위

이 문서가 정한다:
- `Font` 런타임 모듈의 에셋 타입(4장)
- `FontTools` 개발 모듈의 임포터·쿠킹 처리기·미리 구운 글꼴 생성기(5장)
- `TextLayout` 모듈 중 글꼴 코어와 라이브러리를 쓰는 ClientOnly `TextLayoutBackend`(face·metric·래스터·거리장·조합 탐색·글리프 cache CPU 부분·shaping 호출 경계 — 6장). UiToolkit 3.12절의 레이아웃 모델·BiDi·줄바꿈·IME·Element는 UiToolkit이 정한다.
- 사용처 계약(7장)과 서드파티 도입(8장)

정하지 않는다(다른 문서 소유):
- GPU atlas 업로드·batch·셰이더 — UiToolkit `UiRender`, [엔진 셰이더](../EngineShaders/README.ko.md) 4.19절(UI)
- Canvas 자체(그리기 목록·HUD·stat 표시) — 아직 설계가 없다. 이 문서는 Canvas가 쓸 글꼴 API만 정한다(7.2절).
- 지역화(culture·`Text`) — Localization 설계. 이 문서는 culture 이름을 인자로 받는다.
- `TextRenderComponent`의 컴포넌트·프록시 — [컴포넌트 목록](../ComponentCatalog/README.ko.md) CM5
