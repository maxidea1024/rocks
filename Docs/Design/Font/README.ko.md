# 글꼴 설계 — 글꼴 에셋·임포트·쿠킹·글리프 처리

기준일: 2026-11-24, 개정 1(초안). 사용자 지시 "현재 엔진에 폰트(ttf 같은거) 처리하는 코드가 하나도 없음 … UE5.8에서 지원하는 기능을 갖춰야함"으로 쓴 설계다. 결정은 권장안으로 확정했다(애매하면 UE 5.8 방식). 원칙·확정 결정과 겹치는 두 항목(FD9 PNG 디코더 중복, FD14 라이브러리 판)은 사용자 확인을 받아 권장안으로 확정했다(2026-11-24).

**왜 이 문서가 따로 필요한가.** 글꼴은 [UI 툴킷](../UiToolkit/README.ko.md)(이하 UiToolkit) 2절 `TextLayout` 모듈과 3.12절 텍스트 표, UD3(라이브러리 구성)에 들어 있다. 그러나 거기에는 **글리프를 그리는 쪽**만 있고, 다음이 빠져 있다.
- 글꼴 에셋 타입(UE `UFontFace`·`UFont`)과 그 저장·쿠킹·로딩 방식(LazyLoad·Stream·Inline)
- 임포터(ttf·ttc·otf·otc → FontFace, 미리 구운 글꼴 atlas 생성)와 재임포트
- UI 밖 사용처: Canvas 텍스트, `TextRenderComponent`([컴포넌트 목록](../ComponentCatalog/README.ko.md) CR18), 머티리얼 글꼴 표현식([머티리얼](../MaterialSystem.ko.md) "후속"), 엔진 기본 글꼴([전체 계획](../EngineImplementationPlan.ko.md) 9.6절 내장 UI font)
- 서드파티 라이브러리 도입 절차와 서버 타깃 제외 규칙
- WOFF2 해제를 "글꼴 모듈의 몫"으로 넘긴 [압축](../Compression.ko.md) 13절의 받는 쪽

이 문서는 위를 정하고, UiToolkit의 TextLayout 글꼴 부분을 상세화한다. UiToolkit 3.12절의 레이아웃 모델·BiDi·IME·Element는 그대로 UiToolkit 소유다.

**착수 시점.** 고정 순서(에셋 A5–A6 → 화면 마일스톤 → UI 툴킷)는 바꾸지 않는다. 단 **FT1(서드파티 도입)은 순서와 무관하게 지금 할 수 있다**(사용자 확인 2026-11-24). 엔진 코드를 바꾸지 않고 라이브러리·빌드 규칙·고지만 더하기 때문이다. FT2 이후의 착수는 10절 선행 조건을 따른다.

**현재 상태(2026-11-24).** 글꼴 코드는 없다. `Engine/Source/ThirdParty`·`Core/Source/ThirdParty`에 FreeType·HarfBuzz·ICU·msdfgen·nanosvg·libpng가 없다. 글꼴과 이어지는 기반은 셋이다: `Core/Encoding/ByteReader.h`(TrueType big-endian 읽기를 용도로 적었다), Core ThirdParty의 Zlib·Brotli(해제), 텍스처 압축 설정 `DistanceFieldFont`([에셋 시스템](../AssetSystem/12-Textures.ko.md) 12.4.3절).

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`)다. 경로는 `Engine/Source/` 기준이며 약어는 아래 표를 쓴다. 레거시·deprecated 경로(`FontFaceData_DEPRECATED`·`UFontBulkData`·`BulkDataPtr_DEPRECATED`·`FontData_DEPRECATED`·`FSlateFontInfo::FontName_DEPRECATED`·`bEnableLegacyMode`)는 따르지 않는다. UE 이름은 대응표(11장)와 근거에만 적는다.

| 약자 | 파일 |
|---|---|
| `FF` / `FFC` | `Runtime/Engine/Classes/Engine/FontFace.h` / `Runtime/Engine/Private/FontFace.cpp` |
| `FN` / `FNC` | `Runtime/Engine/Classes/Engine/Font.h` / `Runtime/Engine/Private/Font.cpp` |
| `FIO` | `Runtime/Engine/Classes/Engine/FontImportOptions.h` |
| `CF` / `CFC` | `Runtime/SlateCore/Public/Fonts/CompositeFont.h` / `Runtime/SlateCore/Private/Fonts/CompositeFont.cpp` |
| `SFI` | `Runtime/SlateCore/Public/Fonts/SlateFontInfo.h` |
| `FC` | `Runtime/SlateCore/Public/Fonts/FontCache.h` |
| `FRM` | `Runtime/SlateCore/Public/Fonts/FontRasterizationMode.h` |
| `SFP` | `Runtime/SlateCore/Private/Fonts/`(FontCacheFreeType·FontCacheHarfBuzz·SlateFontRenderer·SlateSdfGenerator·FontGeometryPreprocessing·LegacySlateFontInfoCache) |
| `FFI` · `TTF` · `EF` | `Editor/UnrealEd/Classes/Factories/FontFileImportFactory.h` · `TrueTypeFontFactory.h` · `Editor/UnrealEd/Private/Factories/EditorFactories.cpp`(5120–5300행) |
| `TTI` | `Editor/UnrealEd/Private/Factories/TTFontImport.cpp` |
| `TRC` | `Runtime/Engine/Classes/Components/TextRenderComponent.h` · `Private/Components/TextRenderComponent.cpp` |
| `CI` | `Runtime/Engine/Public/CanvasItem.h` · `Private/UserInterface/CanvasItem.cpp` · `Canvas.cpp` |
| `SRM` | `Runtime/SlateRHIRenderer/Private/SlateRHIRendererModule.cpp` |

## 목차

- [1. UE 5.8의 글꼴 구조](01-UnrealStructure.ko.md)
- [2. 요구와 범위](02-Requirements.ko.md)
- [3. 모듈과 의존](03-Modules.ko.md)
- [4. 글꼴 에셋](04-Assets.ko.md)
- [5. 임포트·재임포트·쿠킹](05-ImportCook.ko.md)
- [6. TextLayout 글꼴 코어](06-TextLayoutCore.ko.md)
- [7. 사용처: GPU atlas·Canvas·TextRender·머티리얼·기본 글꼴·편집기](07-Consumers.ko.md)
- [8. 서드파티 라이브러리(FT1)](08-ThirdParty.ko.md)
- [9. 결정 사항](09-Decisions.ko.md)
- [10. 단계와 시험](10-Stages.ko.md)
- [11. 대응표와 UE와 다르게 정한 점](11-UnrealMapping.ko.md)
- [12. 다른 문서 개정·범위 밖·개정 이력](12-Revisions.ko.md)
