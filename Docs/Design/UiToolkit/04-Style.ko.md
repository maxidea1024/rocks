## 4. 스타일과 기본 룩

### 4.1 스타일 체계

| Rocks | UE 5.8 대응 | 계약 |
|---|---|---|
| `StyleSet` · `StyleRegistry` | `ISlateStyle` · `FSlateStyleSet` · `FSlateStyleRegistry` | 이름(+지정자)으로 Element 스타일·brush·색·실수·여백·글꼴을 조회하는 형식별 표. 부모 style 이름, content root. 등록부는 전역이 아니라 `ui::Application`이 소유한다 |
| 활성 앱 스타일 | `FAppStyle` | 앱 스타일 집합 이름을 바꿔 끼우는 간접 참조. 에디터는 에디터 스타일로 교체 |
| `Brush` | `FSlateBrush`(`SlateCore/Public/Styling/SlateBrush.h:235`) | 그리기 방식(없음·9분할 box·border·image·rounded box), UV 여백, 이미지 크기, tint, 반복, 반전, 이미지 종류(full color·linear·vector), 외곽선(모서리 반경 4개·색·두께·반경 방식) |
| `ColorRef` | `FSlateColor`(`SlateColor.h:17-126`) | 지정 색·테마 색 slot·상속 전경색·약한 전경색·스타일 색 |
| 테마 색 slot · `ThemeColors` | `EStyleColor` · `USlateThemeManager`(`StyleColors.h:31-358`) | 의미 slot(배경·패널·주색·전경 등 + 사용자 16개). UE의 테마 관리자는 UObject이므로 Rocks는 비객체 설정 객체와 JSON 테마 파일(엔진·프로젝트·사용자 3단계)로 구현한다 |
| Element 스타일 구조체 | `FSlateWidgetStyle` 파생(`SlateTypes.h`: 버튼·체크·텍스트·편집·스크롤·진행·슬라이더·콤보·표·독 탭·창 등) | `ButtonStyle` 등. `ROCKS_STRUCT`로 선언하여 Widget 층에서 속성으로 편집·저장 |
| SVG brush | `FSlateVectorImageBrush` · nanosvg 래스터 | 요청 크기·배율로 래스터화하여 cache |

### 4.2 기본 룩: UE 5.8 기본값

최초 구성의 기본 스타일과 룩은 UE 5.8과 같게 맞춘다. UE는 용도별로 기본 스타일을 나누며 Rocks도 같은 세 집합을 둔다.

| Rocks 집합 | UE 5.8 위치 | 용도 |
|---|---|---|
| 게임 Widget 기본 스타일 | `Runtime/SlateCore/Private/Styling/UMGCoreStyle.cpp` | 게임 UI Widget의 기본값(UE `DefaultStyleCache.cpp:25-75`가 버튼·텍스트 등 runtime 기본값을 여기서 가져간다) |
| 핵심 스타일 | `Runtime/SlateCore/Private/Styling/StarshipCoreStyle.cpp` | 독립 UI 앱(도구 프로그램)의 앱 스타일(`SlateApplication.cpp:7688`). 글꼴 크기: 본문 10, 소 8, 대 14, 제목 BoldCondensed 33·21·15(34–44행) |
| 에디터 스타일 | `Editor/EditorStyle/Private/StarshipStyle.cpp` | 에디터 앱 스타일(`EditorStyleModule.cpp:30`). 앱 배율(에디터 설정)도 같은 기본값 |

- **테마 색:** 기본값은 `Runtime/SlateCore/Private/Styling/StyleColors.cpp:82-114`에 있다(예: 배경 `#151515`, 패널 `#242424`, 주색 `#0070E0`, 전경 `#C0C0C0`). 값 표는 sRGB hex로 기록하고, UE와 같이 linear로 변환하여 사용한다(`StyleColors.h:25`). 엔진 기본 테마 파일(UE `Engine/Content/Slate/Themes/Dark.json`)에 해당하는 Rocks 기본 테마 파일을 둔다.
- **글꼴:** UE 기본 조합 글꼴은 Roboto 계열(Regular·Italic·Bold·BoldItalic·BoldCondensed·BoldCondensedItalic·Black·BlackItalic·Medium·Light, VeryLight는 Light 파일), Mono는 DroidSansMono, fallback은 DroidSansFallback이다. 보조 typeface는 박스 그리기 등 문자 범위에 DroidSansFallback, 아랍어에 NotoNaskhArabicUI를 둔다(`SlateCore/Private/Fonts/LegacySlateFontInfoCache.cpp:111-148`). 에디터에서만 일본어(GenEiGothicPro, `ja`), 중국어 간체(DroidSansFallback, `zh-Hans`), 한국어(NanumGothic 계열, `ko`), Devanagari·Gurmukhi(Noto) 보조 typeface를 더한다(152–260행). 즉 UE 게임 runtime의 한글은 fallback인 DroidSansFallback이 그린다. 모두 공개 라이선스 글꼴이므로 원 배포처에서 받아 같은 구성으로 둔다. Rocks 게임 runtime에 한국어 보조 typeface를 둘지는 이 기본값 구현 후 별도로 정한다.
- **아이콘:** Epic 저작 자원이므로 포함하지 않는다. 클론에는 `Engine/Content`가 없어, 원본을 확인하려면 의존성을 내려받아야 한다. 공개 라이선스 아이콘 세트로 시작하고(UD10), style 키 이름과 크기 격자(16·20 px 등)는 UE 구성과 1:1로 맞춘다.
- **검증:** 기본 값 표의 Rocks 값과 UE 출처 행을 시험으로 대조한다(UV8). 같은 구성 화면을 UE와 나란히 놓은 비교 캡처를 기록한다.

이후 Rocks 고유 룩으로의 변경은 이 기본값이 구현·검증된 뒤 별도 개정으로 다룬다.
