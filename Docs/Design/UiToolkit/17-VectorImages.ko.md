## 17. 벡터 이미지(SVG)와 아이콘

UE 5.8 에디터의 아이콘은 거의 모두 SVG 벡터 이미지다(`FSlateVectorImageBrush` — StarshipCoreStyle·StarshipStyle의 `IMAGE_BRUSH_SVG`). 체크 표시·라디오·콤보 화살표·펼침 화살표·탭 닫기·검색 돋보기가 모두 SVG라, SVG 경로 없이는 UE 에디터 룩을 낼 수 없다. 이 장은 SVG를 읽고 필요한 화소 크기로 래스터화해 그리는 경로(9.1절 UW4s)와 Rocks 아이콘 세트를 정한다.

### 17.1 UE 구조

| 부분 | UE 5.8 | 하는 일 |
|---|---|---|
| brush | `FSlateVectorImageBrush`(SlateCore `Styling/SlateBrush.h` — `ImageType = Vector`, `ResourceName` = SVG 파일 경로) | 이름과 논리 크기(`ImageSize`, 보통 16×16)만 든다 |
| 해석 시점 | `FSlateElementBatcher`가 batch를 만들 때 `ResourceManager->GetShaderResource(Brush, LocalSize, DrawScale)`(SlateRHIResourceManager.cpp 630–647행) | 그릴 크기 × 배율로 화소 크기를 정한다 |
| cache | `FSlateVectorGraphicsCache`(SlateCore `Rendering/SlateVectorGraphicsCache.cpp`) | 키 (이름, 화소 크기). 없으면 요청을 쌓고 `UpdateCache`에서 래스터화해 1024×1024 RGBA atlas(여백 1, `DilateBorder`)에 넣는다. 1024보다 크면 atlas 밖 단독 텍스처 |
| 래스터 | `FSlateSVGRasterizer::RasterizeSVGFromFile`(SlateSVGRasterizer.cpp 26–60행) | nanosvg `nsvgParse(…, "px", 96)` → `nsvgRasterizeFull`(가로·세로 배율 = 화소 크기 / 그림 크기) |
| 비우기 | `FSlateFlushableAtlasCache`(`Slate.MaxSVGAtlasPagesBeforeFlush` 기본 1) | 페이지가 한도를 넘으면 cache를 비우고 다음 프레임에 다시 만든다 |
| 서버 | `!UE_SERVER`에서만 nanosvg를 넣는다 | 서버는 래스터화하지 않는다 |

### 17.2 Rocks 구성

**외부 라이브러리.** nanosvg(memononen/nanosvg, zlib 라이선스, UE와 같은 라이브러리 — UD3)를 `Engine/Source/ThirdParty/NanoSvg`에 들인다. 공식 릴리스가 없어 commit `239e102ec2c6`(master, 2026-07-09)과 archive SHA-256으로 고정한다(Minimp3와 같은 방식). 들이는 파일은 `nanosvg.h`·`nanosvgrast.h`·`LICENSE.txt`다. glue 파일 하나가 구현을 한 번 컴파일하고, 표준 헤더를 먼저 넣은 다음 `malloc`·`realloc`·`free`를 Rocks hook으로 바꾼다. hook은 UiRender가 Core `LibraryHeap`(C 라이브러리용 malloc 호환 공용 힙 — HarfBuzz·ICU hook도 이것으로 옮겼다)으로 정의하므로 모든 할당이 Rocks 할당기를 지난다. 소비 모듈(UiRender)은 nanosvg 헤더를 보지 않고 자기 Private 헤더 `RocksNanoSvg.h`(불투명 handle과 파싱·크기·그라데이션 여부·비균등 배율·래스터 함수 선언)만 include한다 — nanosvg.h의 MSVC 경고 pragma가 소비자에게 닿지 않는다. 외부 원본 폴더에는 glue 소스만 둘 수 있어(third-party policy) 선언은 UiRender에 두고 glue에는 같은 원형을 둔다(둘을 함께 고친다). 원본은 한 줄만 고쳤다(`nsvgParseFromFile`의 `size == -1l` 부호 비교 — `ThirdParty.json` modifications·`Rocks.patch`). 파일 API(`nsvgParseFromFile` — `fopen`)는 부르지 않는다. 색 해석의 `sscanf`(`%x`·`%u`)와 수 해석(`nsvg__atof` — 자체 구현)은 locale에 기대지 않는다.

**키.** `ui::VectorImageKey`(UiCore)는 아이콘 경로의 64비트 해시다. 경로는 아이콘 루트 기준 상대 경로에서 확장자를 뺀 것이고 구분자는 `/`다(예: `Common/Check`). `VectorImageKey::FromPath("Common/Check")`는 constexpr이라(Core `HashStringBytes`) 정적 스타일 값에 그대로 쓴다. 해시가 겹치면 등록 때 `AlreadyExists`로 막는다.

**brush.** `Brush`에 `vectorImage`(키)를 더한다. `mode == Image`이고 키가 있으면 벡터 brush다(UE `ESlateBrushImageType::Vector`). `imageSize`는 논리 크기, `tint`·`mirrorX/Y`는 래스터 brush와 같은 뜻이고 `uv`는 래스터 자리 안의 부분 범위다. 9분할(Box·Border) 벡터 brush(UE `FSlateVectorBoxBrush`)는 후속이라, 키가 있는데 Image 모드가 아니면 `Brush::IsValid`가 거부한다(17.6절). `StyleBrush::Svg(path, size, ThemeSlot tint)`가 UE `IMAGE_BRUSH_SVG(path, size, color)`에 대응한다.

**해석 시점 — 그리기 목록 기록 때.** UE는 batch를 만들 때 해석하지만, Rocks는 `PaintList`가 brush를 기록할 때 해석한다. `PaintList`는 테마처럼 `VectorImageResolver*`를 들고(Application이 넣는다), 벡터 brush를 만나면 화소 크기를 정해 `Resolve(key, width, height)`로 (텍스처 handle, UV)를 받아 래스터 brush처럼 기록한다.
- 화소 크기 = 반올림(로컬 크기 × 배율), 최소 1. 배율은 누적 변환의 두 열 길이다(회전·기울임이 있어도 늘어난 길이만 반영). 가로·세로 각각 정한다. UE `FVectorCacheKey`는 `IntPoint()`로 버리지만, 배율 1.0000001 같은 오차로 16 px 아이콘이 15·17 px가 되지 않게 반올림한다. 유한하지 않으면 `InvalidArgument`, `MaxVectorImagePixels`(UiCore, 2048 — OpenGL ES 3.0이 보장하는 최소 텍스처 크기)를 넘으면 그 값으로 자른다. 그릴 영역이 늘어나면 그림도 늘어난다(UE와 같다) — 아이콘을 줄 안에 둘 때는 가운데 정렬로 원래 크기를 지킨다.
- 그리기 목록 cache: 이미 기록한 항목에는 해석한 handle과 UV가 들어 있다. resolver가 atlas를 비우면 `Revision()`이 바뀌고, Application이 테마 revision을 올려 cache root와 지역 캐시(CachePanel)를 모두 다시 만든다(3.4절·9.1절 UW3c의 테마 revision 경로를 쓴다). `SetVectorImageResolver`도 같은 경로를 탄다. 지역 캐시는 바깥 목록의 resolver를 따라가고, 따로 그리는 목록(보관 표면 `WidgetSurfaceHost`, 도킹 끌기 미리보기)도 같은 resolver를 받는다.
- resolver가 없으면(창·GPU 없는 시험) 벡터 brush는 아무것도 그리지 않고 성공한다(UE null renderer와 같다).
- 모르는 키는 `NotFound`로 그리기를 실패시킨다. 스타일 시험이 오타를 바로 잡게 하려는 것이다(UE는 경고만 남기고 빈칸을 그린다 — Rocks가 엄격한 쪽이다).

**atlas — UiRender `VectorImageAtlas`.** `VectorImageResolver`를 구현한다.
- 등록: `RegisterDirectory(fileSystem, root)`가 root 아래 `*.svg`를 모두 읽어 파싱해 두고, `Register(path, bytes)`는 프로그램·시험이 하나씩 넣는다. 파싱한 그림(nanosvg `NSVGimage`)은 키마다 하나다. 파싱 실패는 `InvalidArgument`다.
- 해석: 키 (그림, 화소 크기)로 찾고, 없으면 그 자리에서 래스터화해 atlas에 넣는다. 래스터 배율은 UE처럼 가로·세로 따로(화소 크기 / 그림 크기)다. 업스트림 nanosvg에는 UE가 덧댄 `nsvgRasterizeFull`이 없으므로, 가로·세로 배율이 다르면 그림을 먼저 비균등 변환한다(점 좌표에 배율을 곱한 사본).
- 페이지: TextLayout `FontAtlasPage`(UE `FSlateTextureAtlas`와 같은 guillotine 분할, 여백 1 px)를 4 byte 내용으로 다시 쓴다. 새 packer는 만들지 않는다. 페이지 크기는 1024×1024(UE와 같다)다. 여백을 더해 한 변이 1024를 넘는 그림은 UE `CreateNonAtlasedTexture`처럼 여백 없는 정확한 크기의 단독 텍스처(uv 0..1)다.
- 화소: nanosvg 결과(straight alpha RGBA, 투명 화소에 이웃 색을 번지게 한 것 — `nsvg__unpremultiplyAlpha`)를 그대로 쓴다. 텍스처 형식은 `RGBA8UnormSrgb`, 파이프라인은 image(straight alpha 섞기)다. SVG의 색은 sRGB다.
- GPU: 페이지를 만들 때 brush 등록부(`SlotArray<BrushTexture>`)에 자리를 받아 handle을 바로 돌려준다. 텍스처 생성과 업로드는 Render owner lane의 `Sync(imagePipeline)`가 한다. 순서는 그리기 목록 기록 → `Sync` → `DrawBatchRenderer::Draw`다(글리프 cache와 같다). 모든 Draw는 그 Draw가 쓰는 기록 뒤의 `Sync` 다음이어야 한다 — 창 그리기뿐 아니라 보관 표면(`WidgetSurfaceHost::SetVectorImages`)과 끌기 미리보기처럼 따로 그리는 경로도 자기 기록 뒤에 `Sync`한다. 텍스처가 아직 없는 자리를 그리면 `DrawBatchRenderer`가 `StaleHandle`로 실패한다.
- 비우기: 그 프레임의 모든 Draw 뒤 `AdvanceFrame`이 본다. atlas 페이지가 `MaxVectorAtlasPages`(4)를, 단독 텍스처가 `MaxVectorStandaloneImages`(8)를 넘으면 모두 놓고 revision을 올린다(UE `Slate.MaxSVGAtlasPagesBeforeFlush`·`MaxSVGNonAtlasTexturesBeforeFlush` 대응 — 따로 센다. UE 기본 1보다 넉넉히 둔다. 아이콘 수백 개를 여러 크기로 쓰는 에디터 화면에서 매 프레임 비우지 않게). Draw 뒤에 비우므로 이번 프레임이 가리키는 자리는 무효가 되지 않는다.
- 등록 순서: `RegisterDirectory`는 찾은 파일을 경로 byte 순으로 정렬해 등록한다(파일 시스템 열거 순서가 플랫폼마다 달라도 목록·캡처가 같다).
- 비균등 배율과 그라데이션: 가로·세로 배율이 다르면 점에 배율을 곱한 사본으로 그린다(UE처럼 늘인다). 그라데이션이 있는 그림은 그라데이션 변환까지 늘이지 않으므로 작은 배율로 가운데에 둔다(늘인 그라데이션이 어긋나는 것보다 비율을 지키는 쪽을 골랐다 — 엔진 아이콘에는 그라데이션이 없다).
- 모듈: nanosvg는 UiRender의 private 의존이다. 공개 헤더에 nanosvg 형식은 나오지 않는다. 서버 타깃이 UiRender를 넣게 되면 TextLayoutBackend처럼 ClientOnly 모듈로 나눈다(UE `!UE_SERVER` 대응, 지금은 서버가 UiRender를 쓰지 않는다).

### 17.3 아이콘 세트(UD10 개정)

아이콘은 Rocks가 직접 그린다(사용자 결정 2026-11-26). Epic 아이콘은 넣지 않는다(UR7).
- 위치: `Engine/Content/Ui/Icons/<묶음>/<이름>.svg`. 키 경로는 `Icons/` 아래 상대 경로다(예: `Common/Check`). 묶음은 UE 배치를 따른다: `Common`(범용), `CoreWidgets`(컨트롤 전용 — 체크·라디오·콤보·표 머리 정렬), `Docking`(탭).
- 격자: 16×16 `viewBox`, 획 두께 1.5 근처, 모서리는 정수·반정수 좌표에 맞춰 16 px에서 선명하게 그린다. 색은 흰색(`#FFFFFF`) 단색 채움이고 실제 색은 brush tint(스타일의 ThemeSlot)가 정한다. UE 아이콘도 회색·흰 단색에 tint를 곱한다.
- 이름: Rocks 이름(PascalCase)을 쓰고, UE 이름은 아래 표와 14장 값표에만 둔다.
- 첫 묶음(이 단계에서 쓰는 컨트롤과 에디터 기본 부품):

| Rocks 키 | UE 대응(14장) | 쓰는 곳 |
|---|---|---|
| `CoreWidgets/CheckBox/Check`·`Indeterminate` | `CheckBox/check`·`indeterminate` | 체크 상자 |
| `CoreWidgets/CheckBox/RadioOn`·`RadioOff` | `radio-on`·`radio-off` | 라디오 |
| `CoreWidgets/ComboBox/WideChevronDown` | `ComboBox/wide-chevron-down` | 콤보 버튼 |
| `CoreWidgets/Cursor/Grab` | (Rocks가 그림 — UE `SoftwareCursor_Grab`는 png) | 편집 텍스트 오른쪽 끌기 소프트웨어 커서(23.5절) |
| `CoreWidgets/TableView/SortUp`·`SortDown` | `sort-up-arrow`·`sort-down-arrow` | 표 머리 |
| `Common/CaretRight`·`CaretDown` | `caret-right`·`caret-down` | 펼침 영역·트리 |
| `Common/ChevronUp`·`ChevronDown`·`ChevronLeft`·`ChevronRight` | `chevron-*` | 메뉴 하위 표시·검색 상자 |
| `Common/Close`·`CloseSmall` | `close`·`close-small` | 검색 지우기·탭 닫기 |
| `Common/Check` | `Common/check` | 메뉴 체크 |
| `Common/Search`·`Plus`·`Minus`·`Ellipsis`·`Menu`·`Settings` | 같은 이름 | 검색·추가·제거·더 보기·메뉴·설정 |
| `Common/Folder`·`FolderOpen`·`File`·`Save`·`Undo`·`Redo` | 같은 뜻 | 툴바·콘텐츠 |
| `Common/Visible`·`Hidden`·`Lock`·`Unlock`·`Filter`·`Refresh` | 같은 뜻 | Outliner·디테일 |
| `Common/Info`·`Warning`·`Error`·`Success` | 같은 뜻 | 알림·오류 표시 |
| `CoreWidgets/Window/Minimize`·`Maximize`·`Restore`·`Close` | `Window/minimize`·`maximize`·`restore`·`close`(42×23 판) | 직접 그린 창 테두리 버튼(9.1절 UW7b) — 판 가운데 10 px 글리프, 1 px 선 |
| `Common/RocksLogo` | `Common/unreal-small`("AppIcon.Small", 24×24) | 제목 막대 앱 아이콘(Rocks 자체 마크) |
| `Common/Play`·`Pause`·`Stop` | 같은 뜻 | 툴바 재생 |

### 17.4 갤러리와 적용

- Icons 탭(16.2절): 등록한 아이콘 전부를 16·20·32 px로 이름과 함께 보이고, tint(Foreground·Primary·Warning)를 바꿔 본다. "골라 쓰기" 원칙(16.1절)대로 키 문자열을 함께 적는다.
- 컨트롤 적용: 체크 상자 표시(지금의 선 두 개 근사를 아이콘으로 — 9.1절 UW3c), 라디오(원 16), 콤보 화살표(UW4b), 펼침·트리 화살표, 도킹 탭 닫기, 메뉴 하위 표시. 스타일 구조체가 아이콘 brush를 갖고(`CheckBoxStyle.checkedImage` 등 — UE `FCheckBoxStyle`과 같은 칸) 기본 스타일 함수가 14장 값으로 채운다.

### 17.5 검증

- NanoSvg: glue가 Rocks Allocator만 쓰는지(할당 횟수 계측), 손상 입력(잘린 태그·거대한 수·빈 문서)에서 실패하고 새지 않는지.
- UiRender `VectorImageAtlas`: 알려진 그림(가득 찬 사각형·반쪽 사각형)의 화소 값, 같은 키·크기의 재사용(래스터 1회), 크기별 다른 칸, 비균등 배율, 1024 초과 단독 페이지, 한도 초과 비우기와 revision, 모르는 키 `NotFound`, 해시 충돌 `AlreadyExists`.
- UiCore `PaintList`: 배율 2에서 화소 크기 2배, 회전 변환의 열 길이, resolver 없음 → 빈 기록, revision 변화 → cache 다시 만듦.
- 갤러리: Icons 탭 캡처, 체크 상자·라디오·콤보 화살표 캡처 비교.

### 17.6 버린 대안과 한계

- 버린 대안: brush에 경로 문자열을 직접 두기(수명 관리와 비교 비용, 정적 스타일에서 문자열 수명이 갈린다), Core `Name`(NamePool이 있어야 하고 constexpr이 안 된다), `DrawBatch`에서 해석(UE 방식이지만 Rocks `DrawBatch`의 정점만 갱신 경로와 atlas 교체가 얽힌다 — 기록 때 해석하면 cache revision 하나로 정리된다), 공개 아이콘 세트(사용자 결정으로 자체 제작 — UE 룩과 모양을 맞추기 쉽다), SVG 래스터라이저 자체 구현(경로·곡선·채움 규칙·AA 규모가 커서 외부 라이브러리 원칙), lunasvg·resvg(UE와 다른 라이브러리라 결과가 갈린다).
- 한계: nanosvg가 지원하는 SVG 부분집합만 된다(텍스트·필터·마스크·`<use>` 없음). 아이콘은 이 범위 안에서 그린다. 벡터 9분할은 후속이다. atlas 칸 여백은 0(투명)이라 UE `DilateBorder`(가장자리 화소 복제)와 다르다 — 아이콘은 정수 화소 위치에 화소 크기 그대로 그려 표본이 여백에 닿지 않지만, 소수 위치·확대로 그리면 가장자리에 닿는 그림 테두리가 조금 어두워질 수 있다. 페이지 갱신은 바뀐 사각형이 아니라 페이지 전체를 올린다(글리프 cache와 같다 — Graphics 업로드에 영역 API가 생기면 바꾼다). nanosvg 할당 hook은 NanoSvg를 링크하는 모듈(UiRender)이 정의하므로, 두 번째 모듈이 링크하게 되면 hook을 Core로 옮긴다. UE의 "grow window"(한도에 닿아도 몇 프레임 늘려 보고 비우기)는 두지 않았다. 화소 크기마다 따로 래스터화하므로 애니메이션으로 크기가 계속 바뀌는 아이콘은 atlas를 빨리 채운다(UE와 같은 한계 — 그런 곳은 래스터 brush를 쓴다).
