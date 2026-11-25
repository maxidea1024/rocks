## 9. 결정 사항

모두 권장안으로 확정했다(2026-11-24). 원칙·확정 결정과 겹쳐 사용자 확인을 받은 FD9(원칙 "범용 기능 중복 금지")와 FD14(UiToolkit UD3의 근거 "UE와 결과 일치")는 사용자 결정(2026-11-24 "FD9·FD14 권장안대로 진행")으로 권장안이 확정됐다.

| ID | 사안 | 결정 | 버린 대안 | 근거 |
|---|---|---|---|---|
| FD1 | 에셋 구조 | **확정(UE):** FontFace(원본 파일)와 Font(조합·미리 굽기) 두 에셋 | Font 하나에 원본 byte 포함 | face를 여러 Font가 공유하고, face마다 로딩 방식을 따로 둔다(UE와 같다) |
| FD2 | 조합 글꼴 표현 | 저장 기술(`CompositeFontDesc`, Font 모듈, 에셋 참조)과 실행용(`ResolvedCompositeFont`, TextLayout, face handle)을 나눈다 | UE처럼 한 struct가 에셋 참조와 실행 탐색을 겸함 | TextLayout은 Object에 의존하지 않는다(UiToolkit 2절). 실행용은 구간 표를 정렬·병합한 탐색 구조라 저장 형태와 달라도 된다 |
| FD3 | 원본 저장 형식 | 파일 byte 그대로(SHA-256) | 텍스처처럼 정규형으로 풀어 저장 | FreeType·HarfBuzz가 sfnt 표를 직접 읽는다. 정규형이 없다 |
| FD4 | cache 방식 이름 | `Prebaked`·`OnDemand` | UE `Offline`·`Runtime` | Rocks에서 Runtime은 모듈 층 이름이고, Offline은 "연결 없음"으로 읽힌다(Rocks 고유 용어 규칙) |
| FD5 | 원본 경로 필드 | 두지 않음(ImportRecord·쿠킹 데이터가 가짐) | UE `SourceFilename`(편집 원본 경로와 쿠킹 `.ufont` 경로 겸용) | 한 필드가 두 의미를 가지면 쿠킹 전후 해석이 달라진다 |
| FD6 | 파일 경로 face | 두지 않음, 엔진 기본 글꼴도 FontFace 에셋. **예외**: 내장 UI 글꼴(7.5절)은 에셋 활성화 전에 필요하므로 원본 byte를 `FontFaceData`로 직접 등록 | UE `FFontData::FontFilename` | 모든 글꼴이 같은 쿠킹·너겟·의존 경로를 탄다. 내장 글꼴은 계획 9.6절이 "asset 활성화 전·에셋 너겟 없이"를 요구한다 |
| FD7 | 미리 굽기 문자 키 | code point(`uint32`) | UE `uint16`(UTF-16 단위) | 보조 평면 문자(이모지·CJK 확장)를 굽는다 |
| FD8 | 지연 원본 byte 위치 | 같은 패키지의 지연 bulk entry(geometry도 같은 로딩 방식). AssetSystem이 텍스처 스트리밍 덩어리를 별도 파일로 정하면 글꼴도 따른다 | UE `.ufont` 별도 파일 고정 | 스트리밍 데이터의 위치 규칙을 텍스처·메시·글꼴이 하나로 공유한다 |
| FD9 | 컬러 비트맵 글리프 PNG | **확정(사용자 결정 2026-11-24):** libpng를 FreeType 전용 의존으로 들임(UE와 같다) | (a) ImageCodec PNG 디코더를 런타임으로 옮기고 FreeType `pngshim.c` 대체 (b) PNG 끔(컬러 이모지 미지원) | FreeType의 PNG 경로(`pngshim.c`)는 libpng API에 고정이다. (a)는 추적 가능한 수정으로 할 수는 있으나 런타임 PNG 디코더가 필요한데 ImageCodec은 Developer 모듈이라 런타임으로 옮겨야 하고, FreeType 판을 올릴 때마다 shim 수정을 다시 맞춰야 한다. (b)는 UE 기능 미달. 대가: PNG 해제기가 둘(ImageCodec·libpng) |
| FD10 | WOFF·WOFF2 | 1단계 범위 밖, Rocks 추가 기능으로 FT8에서 받음(원본 byte 그대로, Stream 불가 — 열 때 전체 해제) | 처음부터 받음 / 받지 않음 | UE는 받지 않는다(UE FreeType은 Brotli도 연결하지 않는다). FreeType이 zlib·Brotli로 직접 연다(FT1에서 열기 시험만 한다) |
| FD11 | 미리 굽기 입력 | FontFace 또는 글꼴 파일 + FreeType, 모든 플랫폼 같은 경로, 임포트 때 생성 | UE: 설치된 시스템 글꼴 이름 + Windows GDI | 결과가 기기·OS에 따라 달라지지 않는다(FR6·FR7) |
| FD12 | ICU 데이터 | 공식 데이터 `.dat`를 `icupkg`로 부분집합 → 메모리 등록 | 전체 데이터(수십 MB) / ICU 데이터 빌드 도구로 소스에서 생성 | 크기, 그리고 원본 byte 보존(공식 바이너리 + 항목 목록만 Rocks 파일) |
| FD13 | 서버 | 라이브러리를 쓰는 부분을 ClientOnly `TextLayoutBackend`로 떼고 표 등록으로 연결. `TextLayout`·`Font`는 서버에도 링크. 쿠킹은 글리프 데이터를 빈 참조로 | 타깃 종류 매크로로 같은 모듈 안에서 분기 / 서버에서 TextLayout 자체 제외 | 빌드 타깃 BD2·BD3(타깃 매크로는 넷뿐)·BD25(Runtime → ClientOnly 의존 금지), 오디오 AD24와 같은 방식. 데이터 제외는 계획 9.6절 908행 |
| FD14 | 라이브러리 버전 | **확정(사용자 결정 2026-11-24):** 도입 시점 최신 안정판(FreeType 2.15.0·HarfBuzz 15.2.0·ICU 79.1·msdfgen 1.13.1·libpng 1.6.59), 하한은 UE 트리 최신판 | UE가 고른 판(HarfBuzz 2.4.0·ICU 64.1) | 하위호환 고려 없음 원칙, UE도 새 판을 트리에 들여 옮기는 중. 대가: UD3의 "UE와 줄바꿈·shaping 결과 일치"는 판 차이만큼 어긋날 수 있어, 시험 기준값은 UE 실행 덤프가 아니라 소스 근거 + 차이 목록으로 둔다(10.3절 FV2·FV4) |
| FD15 | HarfBuzz 유니코드 함수 | ICU(`hb-icu`, `HAVE_ICU_BUILTIN`) — UE 12.2.0 빌드와 같다 | HarfBuzz 내장 UCD | ICU의 script 판정·문자 속성과 같은 Unicode 판을 쓴다 |
| FD16 | LazyLoad 기본 | 처음 쓸 때 동기 읽기, `ui.font.async_lazy_load`로 비동기(편집기·Tryout 제외) | 늘 비동기 | UE 기본과 같다. 측정이 처음부터 맞다 |
| FD17 | 설정 이름 | `ui.font.*`·`ui.text.*`, 각 마디는 `[a-z][a-z0-9_]*`(11.1절 대응표) | UE 이름 그대로(`Slate.*`·`UI.SlateSDFText.*`) | Rocks CVar·Config 이름 문법([Config](../ConfigSystem.ko.md) 132행, [CVar](../CVarSystem.ko.md) 164행), Slate는 UE 고유어 |
| FD18 | 미리 구운 글꼴 페이지 | 별도 Texture2D 패키지(`<Font>_Page<N>`), ImportRecord 소유자 = Font | UE처럼 Font 안 하위 Texture2D / 비 Asset 페이지 타입 신설 / AssetSystem 규칙 개정 | Rocks 규칙 "Asset의 Outer는 Package·패키지당 대표 에셋 하나"를 지키면서 텍스처 파이프라인을 그대로 쓴다. 다중 출력 임포트의 소유 관계(ImportOwner)가 이미 있다 |
| FD19 | KerningOnly의 GPOS kerning | FreeType `TT_CONFIG_OPTION_GPOS_KERNING` 켬 | UE처럼 끔(GPOS만 있는 글꼴은 KerningOnly에서 kerning 없음) | 요즘 글꼴 대부분이 GPOS kerning만 가진다. UE와 결과가 다르므로 11.2절에 적는다 |
