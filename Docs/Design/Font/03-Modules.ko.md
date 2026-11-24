## 3. 모듈과 의존

서버 타깃은 글꼴 라이브러리를 링크하지 않는다(FR8). [빌드 타깃](../BuildTargets/README.ko.md) BD25는 Runtime 모듈이 Server에서 빠지는 모듈에 의존하는 것을 막고, 타깃 종류 매크로는 BD2·BD3의 넷만 둔다. 그래서 오디오(AD24 — [오디오](../AudioSystem.ko.md) 장치 backend·`AudioOpus`)와 같은 방식으로 **라이브러리를 쓰는 부분을 ClientOnly backend 모듈로 떼고 표 등록으로 연결한다**(FD13).

| 모듈 | type | 위치 | 직접 의존 | 책임 | 금지 |
|---|---|---|---|---|---|
| `TextLayout` | Runtime | `Engine/Source/Runtime/TextLayout`(UiToolkit 2절) | Core·Task(Localization이 생기면 Localization — UiToolkit 2절) | 6장 글꼴 코어의 라이브러리 중립 부분(글꼴 지정·조합 탐색 구조·글리프 cache 관리·측정 API·atlas 페이지 할당) + UiToolkit 3.12절 레이아웃 모델, **backend 표**(`TextBackend` — face 열기·metric·shaping·래스터·전처리 geometry·거리장 생성·경계·BiDi). 표가 비면 해당 호출은 `Unsupported` | Object·Graphics·AssetRuntime 의존, 라이브러리 타입 공개 |
| `TextLayoutBackend` | **ClientOnly** | `Engine/Source/Runtime/TextLayoutBackend` | TextLayout, Private: FreeType·HarfBuzz·Icu·Msdfgen·LibPng | `TextBackend` 구현. 모듈 `Startup`에서 `HostServices`에 backend 서비스를 등록하고, TextLayout 인스턴스를 만드는 쪽(UI 앱·Canvas·도구)이 찾아 넣는다(AD24 등록 경로와 같다). Modular 빌드에서 vtable이 이 모듈에 있으므로 `SupportsUnload` = false | 공개 헤더에 라이브러리 타입 |
| `Font` | Runtime | `Engine/Source/Runtime/Font` | AssetRuntime·Texture·TextLayout | `FontFace`·`Font` 에셋, 저장 기술 struct, 에셋 → TextLayout 연결(face 공급자) | Graphics·UiCore 의존 |
| `FontTools` | Developer | `Engine/Source/Developer/FontTools` | Font·TextLayout·AssetDatabase·AssetCooker·AssetNugget·TextureTools | 임포터·재임포트·쿠킹 처리기·미리 구운 글꼴 생성. 래스터·검사는 호스트가 등록한 backend를 쓴다(Developer는 ClientOnly에 의존할 수 없다 — BD25 표). 도구 프로그램(Program 타깃은 ClientOnly를 포함한다)이 `TextLayoutBackend`를 링크하고 등록한다 | Shipping 링크, backend 직접 참조 |
| `UiRender` | (UiToolkit) | UiToolkit 2절 | — | 글리프 atlas GPU 업로드·SDF 셰이더(7.1절), SVG 래스터(nanosvg) | — |
| 서드파티 | — | `Engine/Source/ThirdParty/{FreeType,HarfBuzz,Icu,Msdfgen,NanoSvg,LibPng}`(8장) | Core ThirdParty Zlib·Brotli | — | 엔진 공개 헤더에서 include |

의존 방향(→는 의존):

```text
FontTools(Developer) → { Font, TextLayout, TextureTools, AssetCooker, AssetDatabase, AssetNugget }
Font(Runtime) → { AssetRuntime, Texture, TextLayout }
TextLayout(Runtime) → { Core, Task }
TextLayoutBackend(ClientOnly) → TextLayout + Private { FreeType, HarfBuzz, Icu, Msdfgen, LibPng }
FreeType → { Zlib, Brotli, LibPng }     HarfBuzz → { FreeType, Icu }     LibPng → { Zlib }
UiCore → TextLayout(글꼴 계약)           UiRender → { UiCore, Graphics, NanoSvg }
Engine(Canvas·TextRenderComponent, 후속) → { Font, TextLayout }
```

- `TextLayout`이 Object를 모르는 것은 UiToolkit 2절 규칙(UiCore와 같은 층)이다. 그래서 에셋 참조가 든 저장 기술은 `Font`에, 해석이 끝난 실행용 조합 글꼴(`ResolvedCompositeFont`)은 `TextLayout`에 둔다(FD2). UE는 두 역할을 `FCompositeFont` 하나가 맡는다.
- `TextLayout`의 culture는 지금은 인자로 주입한다(FP3). Localization 모듈이 생기면 UiToolkit 2절 표대로 의존을 더하고 culture 변경 통지를 받는다.
- `Font`가 `Texture`에 의존하는 것은 미리 구운 글꼴의 페이지가 `Texture2D` 에셋이기 때문이다(4.3.2절).
- `FontTools`가 미리 구운 글꼴을 만들 때 라이브러리를 직접 부르지 않고 TextLayout backend 표를 쓴다(중복 구현 금지). 페이지 텍스처의 빌드·쿠킹은 `TextureTools`의 처리기를 그대로 쓴다.
- **서버 타깃**: `TextLayout`·`Font`는 링크하고(에셋 참조·의존 그래프가 타깃마다 바뀌지 않게) `TextLayoutBackend`·`UiRender`·글꼴 서드파티는 빠진다. backend가 없으므로 face 열기·shaping·래스터·측정·경계는 `Unsupported`이고 가짜 metric을 만들지 않는다. ICU도 서버에 들이지 않는다 — UE도 SlateCore의 ICU·nanosvg는 비서버에서만 붙이고, 서버의 ICU는 Core 국제화 쪽이다(1.9절). 서버 쿠킹은 글리프 데이터를 뺀다(5.4절).
- **ICU 전역 상태**: `udata_setCommonData`·`u_setMemoryFunctions`가 프로세스 전역이라 `Icu` 매니페스트는 `globalState` = true이고, 직접 소비하는 Rocks 모듈은 `TextLayoutBackend` 하나다([빌드 타깃](../BuildTargets/03-Design/03.08-LinkType.ko.md) 3.8절 단일 소비 규칙). HarfBuzz(`hb-icu`)는 서드파티 사이 의존이다. Localization이 ICU를 쓰게 되면 그때 소비 모듈을 하나로 모으는 방법(ICU 등록을 Localization으로 옮기고 backend가 그것을 씀)을 그 설계에서 정한다.
