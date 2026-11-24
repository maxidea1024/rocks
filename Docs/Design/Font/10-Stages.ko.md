## 10. 단계와 시험

### 10.1 선행 조건

| ID | 내용 | 소유 | 막는 단계 |
|---|---|---|---|
| FP1 | 쿠킹 패키지의 **지연 bulk entry**(패키지 로드가 읽지 않음)와 범위 읽기 handle(비압축 entry 임의 위치 읽기, 수명은 handle 참조 수). 텍스처 mip 스트리밍([텍스처 스트리밍](../TextureStreaming.ko.md) 5.4절)과 같은 기능이라 먼저 하는 쪽이 만든다 | 에셋 세션(AssetSystem 5·10·11장 개정) | FT3의 LazyLoad·Stream(Inline은 막지 않음) |
| FP2 | 기기 프로필별 CVar 값([Config](../ConfigSystem.ko.md) CF4) | Config 세션 | 없음 — 없으면 기본값으로 동작 |
| FP3 | culture 공급(Localization) | Localization 설계 | 없음 — culture를 인자로 주입 |
| FP4 | RHI 기본·UiRender(UiToolkit UT2) | 렌더 세션·UI 단계 | FT6 |
| FP5 | Canvas 설계 | 미정 | FT7 Canvas 부분 |
| FP6 | 쿠커의 타깃별 payload 제외와 빈 `BulkDataRef` 규칙(오디오 AD24 서버 쿠킹 제외와 공용) | 에셋 세션(AssetSystem 5·10장 개정) | FT4 서버 쿠킹 |
| FP7 | `/Engine` 엔진 콘텐츠 마운트([에셋 시스템](../AssetSystem/04-IdentityPaths.ko.md) 4장 "후속") | 에셋 세션 | FT7 기본 글꼴 콘텐츠 |

### 10.2 단계

| 단계 | 내용 | 선행 | 완료 조건 | 시기 |
|---|---|---|---|---|
| FT1 | 서드파티 도입(8장): FT1a FreeType·LibPng(Zlib·Brotli 연결), FT1b HarfBuzz(hb-icu는 FT1c 뒤 연결), FT1c ICU·데이터 부분집합·staging, FT1d msdfgen·nanosvg, FT1e 시험 글꼴·연결 시험·고지 | 없음 | 8.5절 | **지금 가능** |
| FT2 | `TextLayout` 글꼴 코어(6장 라이브러리 중립 부분)와 `TextLayoutBackend`(ClientOnly, 라이브러리 구현·등록): face·metric·hinting, 조합 탐색, shaping·측정, Bitmap·컬러·외곽선 래스터, 전처리 geometry·거리장, 글리프 cache CPU, CVar | FT1 | FV1–FV7 | UiToolkit UT2의 TextLayout 부분(고정 순서) |
| FT3 | Font 모듈: 열거형·FontFace·Font·저장 기술·검증·face 공급자·Resolve·로딩 방식 셋 | FT2, FP1(LazyLoad·Stream) | FV8–FV10 | FT2 뒤 |
| FT4 | FontTools: 임포터·재임포트·같은 이름 Font·쿠킹 처리기·서버 쿠킹 | FT3, FP6(서버 쿠킹) | FV11–FV13 | FT3 뒤 |
| FT5 | 미리 구운 글꼴 생성기(페이지 패키지 포함) | FT4 | FV14 | FT4 뒤 |
| FT6 | GPU atlas·SDF 셰이더 연결 | FT2, FP4 | FV15 | UiToolkit UT2 |
| FT7 | 사용처: 내장 UI 글꼴(FT2 뒤 가능), 기본 글꼴 콘텐츠·엔진 글꼴 설정(FT3·FP7), Canvas(FP5), `TextRenderComponent`(CM5), 머티리얼 노드(머티리얼 단계), 편집기(UT4) | 각 항목 | FV16 | 각 소비자 단계 |
| FT8 | (Rocks 추가) WOFF·WOFF2 임포트 | FT4 | FV17 | 요구 시 |

### 10.3 시험(사용 예·양성·음성·경계를 규칙마다)

기준값은 UE 소스 근거(행 번호 인용)로 정한다. UE 실행 덤프는 UiToolkit UD6이 허용한 UV1·UV3·UV4에만 쓰며, 이 문서의 시험에는 쓰지 않는다. 라이브러리 판이 UE와 다르므로(FD14) shaping·줄바꿈 결과가 UE와 다른 사례는 차이 목록(이유·판)으로 남긴다.

| ID | 대상 | 검증 |
|---|---|---|
| FV1 | face 열기 | TTF·OTF(CFF)·TTC 하위 face 전부, 손상 파일(표 누락·잘린 파일·unitsPerEm 0) 거부와 원본 오류 코드 보존, Stream 콜백 범위 읽기 횟수·범위, 같은 키 한 번만 열기, backend 미등록 시 `Unsupported` |
| FV2 | metric | 크기 단위(96 DPI 포인트 → 픽셀), hinting 다섯 × 안티앨리어싱 켬·끔 플래그 표(6.2절), layout method 둘, ascent/descent 재정의(조합 스위치 켬·끔), 취소선 높이 경계(0·100) |
| FV3 | 조합 탐색 | culture 우선 → 일반 구간에서 하나만 고름, 겹치는 구간의 작은 구간 우선·같은 크기 선언 순서, typeface 안 이름 → 속성 → 첫 entry, 기본 → fallback(cmap 검사 없음) → last resort → U+FFFD 재탐색, grapheme 첫 code point(ZWJ 이모지 열), 공백류, 보조 평면, culture 변경 뒤 cache 무효화 |
| FV4 | shaping | Auto 판정(RTL, LTR+결합 문자·ZWJ·복합 script·서로게이트 → FullShaping, 그 밖 KerningOnly), `ffi` 합자, 자간 0이 아닐 때 합자 끔, RTL·다중 code point grapheme에서 자간 무시, 고정폭, GPOS kerning(KerningOnly, FD19), 아랍어·히브리어·한글 조합형, cluster 위치가 원문 byte와 맞음 |
| FV5 | 래스터 | Bitmap 크기·기울임별 golden(플랫폼별 허용), 컬러 글리프(CBDT strike 선택·축소, COLRv0), non-scalable 배율 무시, 외곽선 mitered·round, 단색(안티앨리어싱 끔) |
| FV6 | 거리장 | 전처리 geometry 결정성(플랫폼 사이 같은 byte), Msdf(MTSDF 4채널)·Sdf·근사 golden, 자리표시 → 교체(Msdf·Sdf)·근사는 자리표시만, ppem 단계 셋·모드별 표 선택·재정의 표, spread가 다른 키, 비트맵 전용 글꼴 진단, Task 생성 중 face 잠금 미사용 |
| FV7 | 글리프 cache | 페이지 할당·여백, atlas 밖 글리프, 상한 초과 시 다음 프레임 비움, 성장 창 안 재초과 시 늘림, 비울 때 face 닫기, 통계 덤프 |
| FV8 | 에셋 저장 | 모든 필드 왕복, 기본값(cacheType 예외 포함), 범위 밖 값 거부, `CompositeFontDesc` 검증(이름 중복·하위 face 번호·구간 역전·빈 기본 typeface) |
| FV9 | 로딩 방식 | Inline·LazyLoad(동기·비동기, 준비 사건·Layout 무효화)·Stream(패키지 해제 뒤에도 handle 수명), geometry가 로딩 방식을 따름, IO 실패 → Failed와 다음 단계, 편집기·Tryout에서 비동기 금지 |
| FV10 | Resolve | Font 편집·재임포트 뒤 cache 버림, DefaultStyle, 서버 타깃에서 Resolve 성공·측정 `Unsupported` |
| FV11 | 임포트 | 확장자 넷, 내용 판정(확장자 어긋남 Warning), Font 생성 Yes·No·IfMissing과 `_Font` 이름·충돌 번호, `entryPerSubFace`, 일괄 적용 |
| FV12 | 재임포트 | 같은 SHA-256이면 변경 없음, 하위 face 감소 시 Font 진단 |
| FV13 | 쿠킹 | 로딩 방식별 entry 배치·압축 여부, geometry는 거리장 켬일 때만, 너겟 키(처리기·코어·FreeType·msdfgen 판·프로젝트 설정), 서버 쿠킹의 빈 참조와 로드, 결정성(두 번·두 플랫폼 쿠킹 byte 같음) |
| FV14 | 미리 굽기 | 문자 지정 방식(문자열·구간·파일(결과 목록 저장)·문자 집합·인쇄 가능·ASCII 포함), 페이지 배치 결정성, 합성 굵게·기울임, 거리장 alpha 강제값, 페이지 채널(R·RGBA), 페이지 패키지 이름·소유자, 보조 평면 문자, 없는 문자 대체 |
| FV15 | GPU | atlas 형식 셋·atlas 밖 텍스처 업로드, 갱신 사각형, 비움 뒤 재생성, 거리장 셰이더 픽셀 비교(UiToolkit UV6) |
| FV16 | 사용처 | `TextRenderComponent`에 OnDemand 지정 시 Warning·그리지 않음, 머티리얼 Font 파라미터에 OnDemand·페이지 범위 밖 컴파일 오류, 엔진 글꼴 설정 로드, 내장 UI 글꼴이 에셋 없이 측정됨 |
| FV17 | WOFF | WOFF1·WOFF2 임포트, Stream 지정 거부 |

시험 글꼴은 고정 파일(FT1e)만 쓰고 OS 설치 글꼴을 읽지 않는다(FR6).
