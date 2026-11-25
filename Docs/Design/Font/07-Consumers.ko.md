## 7. 사용처

### 7.1 GPU 글리프 atlas(UiRender)

- TextLayout(6.6절)이 프레임마다 내는 변경 목록(새 페이지, 페이지별 갱신 사각형과 CPU 픽셀, atlas 밖 글리프 텍스처, 비운 페이지)을 UiRender가 Render lane에서 텍스처로 올린다. 형식: Alpha → `R8`(UE는 `PF_A8` — Rocks는 모든 backend에 공통인 단일 채널 형식을 쓰고 셰이더가 R을 읽는다, 11.2절), Color → RGBA8 sRGB, Msdf → RGBA8 linear(MTSDF 4채널). mip·이방성 없음(UE `TextureAtlas.h` 주석 "we don't support mips or aniso filtering on atlas textures").
- 거리장 글자 셰이더(두께·외곽선·부드러움)는 [엔진 셰이더](../EngineShaders/README.ko.md) 4.19절(UI, SDF·MSDF 근거 문헌 있음)이 맡는다. UE `MsdfFont` Slate material shader 대응이다.
- atlas 텍스처의 상주 예산은 계획 9.6절 906행("font atlas의 residency·성장 상한도 계상한다")에 따라 UiRender가 계상한다.

### 7.2 Canvas 텍스트

Canvas(UE `UCanvas`·`FCanvas`·`FCanvasTextItem` — 디버그 표시·stat·콘솔·HUD)는 아직 Rocks 설계가 없다(FP5). Canvas 설계가 따를 글꼴 계약만 정한다.
- 입력: `Font` 에셋(+ 크기·typeface가 없으면 `defaultSize`·`defaultTypeface`) 또는 `FontInfo`, 배율, 색, 그림자(색·오프셋), 외곽선(색), 가운데 정렬, 잘라내기.
- Prebaked 글꼴: `prebaked.characters`·`pages`로 사각형을 직접 만든다(shaping 없음, `kerning`·`scalingFactor` 적용). OnDemand 글꼴: TextLayout shaping·글리프 cache·atlas를 UI와 공유한다(UE `CI` 984–1011행과 같은 두 갈래).
- DPI: OnDemand는 DPI 배율을 적용하고 Prebaked는 적용하지 않는다(UE `Canvas.cpp` 1811행).
- 셰이더 디버그 출력(`Debug/ShaderDebugPrint`, 엔진 셰이더 4.20절)은 셰이더 안 고정 비트맵 글꼴 표를 쓰며 이 문서와 무관하다.

### 7.3 `TextRenderComponent`(컴포넌트 목록 CR18)

- UE와 같이 **Prebaked 글꼴만** 그린다(`TRC` 324·432·704행). OnDemand 글꼴이면 UE는 경고 없이 그리지 않는다(729–734행). Rocks는 같은 동작에 저장 검증 Warning을 더한다(Rocks 추가, 11.2절).
- World 객체는 에셋에 `ObjectRef`를 걸 수 없으므로([에셋 시스템](../AssetSystem/06-AssetTypes.ko.md) 6.3절) 글꼴 필드는 `SoftObjectRef<Font>`이고 런타임 보유는 `AssetLease`다. 레벨·prefab 패키지의 Hard 의존이 함께 로드·쿠킹을 보장한다.
- 필요한 것: `Font::prebaked`의 문자 표·페이지 Texture2D·metric, 그리고 머티리얼의 글꼴 페이지 파라미터(7.4절). 컴포넌트·프록시·정렬(가로·세로)·`worldSize`·`xScale`·`yScale`·간격 조정·페이지별 동적 머티리얼 인스턴스 cache(UE `TRC` 314–376행)는 컴포넌트 목록 CM5 몫이다.

### 7.4 머티리얼 글꼴 표현식

| Rocks 노드 | UE | 동작 |
|---|---|---|
| `FontSample` | `UMaterialExpressionFontSample`(`Font`, `FontTexturePage`) | Prebaked 글꼴 페이지 텍스처 샘플. 출력 RGB·R·G·B·A. 마스크 채널은 페이지 형식에 따라 R(단일 채널 페이지) 또는 A(RGBA 페이지)를 A 출력으로 준다(5.2절). Prebaked가 아니거나 페이지 번호가 범위 밖이면 컴파일 오류(UE와 같다) |
| `FontSampleParameter` | `UMaterialExpressionFontSampleParameter`(`ParameterName`·`Group`) | 위의 파라미터 판. 머티리얼 인스턴스에서 (Font, 페이지)를 바꾼다. **파라미터 종류 "font"는 이것 하나다** |
| `FontSignedDistance` | `UMaterialExpressionFontSignedDistance` | 파라미터가 아닌 입력 노드. UI 거리장 글자 머티리얼에 거리장 데이터(`FontSignedDistanceData`)를 공급 |

[머티리얼](../MaterialSystem.ko.md)의 파라미터 종류 표에서 "font — 후속"을 `FontSampleParameter`로 바꾼다(12장). 노드 구현은 머티리얼 단계의 FT7이다.

### 7.5 기본 글꼴·마지막 fallback·내장 UI 글꼴

- **엔진 기본 조합 글꼴**: UiToolkit 4.2절의 구성(Roboto 10종·DroidSansMono·DroidSansFallback·NotoNaskhArabicUI, 편집기 전용 보조 typeface)을 엔진 콘텐츠의 FontFace·Font 에셋으로 둔다. 원 배포처에서 받아 원본을 보존하고 라이선스 고지를 단다(SIL OFL 1.1·Apache-2.0 — 8.4절과 같은 고지 묶음). `FontRuntimeSource::DefaultStyle`이 가리킨다. 엔진 콘텐츠는 `/Engine` 마운트가 있어야 한다([에셋 시스템](../AssetSystem/04-IdentityPaths.ko.md) 4장 — 지금 "후속") — 선행 FP7.
- **엔진 글꼴 설정**(UE `UEngine` `TinyFont`·`SmallFont`·`MediumFont`·`LargeFont`·`SubtitleFont`·`MonospaceFont` 여섯과 배열 `AdditionalFonts`): Config 엔진 설정 struct에 `SoftObjectRef<Font>` 여섯 항목과 `Array<SoftObjectRef<Font>>` 하나로 둔다(부팅 때 로드, 바꾸면 재시작 — UE `ConfigRestartRequired`). Canvas·디버그 표시가 쓴다.
- **마지막 fallback**: UE `LastResort.ttf`(쿠킹 안 된 실행 전용) 대응으로 SIL OFL인 Unicode Last Resort 글꼴을 편집기 콘텐츠에 둔다. 쿠킹된 게임에는 넣지 않는다(UE와 같다). hinting은 `FORCE_AUTOHINT`(6.2절).
- **내장 UI 글꼴**(계획 9.6절 900·904·908행): 일반 에셋 활성화 전, 프로젝트·에셋 너겟·네트워크가 없어도 엔진이 띄울 수 있는 최소 글꼴이다. 그래서 **FD6의 예외**로 FontFace 에셋을 거치지 않는다: 엔진 바이너리 옆 내장 자원 팩의 원본 TTF byte(Roboto Regular·DroidSansMono)를 TextLayout `FontFaceData`로 직접 등록하고(Inline과 같은 메모리 열기), 이것으로 만든 `ResolvedCompositeFont`가 에셋 기본 글꼴이 준비되기 전의 기본값이다. 9.6절의 "device 수명 동안 pin"은 이 글꼴의 **GPU atlas**에만 해당하고, face byte는 프로세스 수명 동안 산다. 서버·CPU 전용 타깃에는 넣지 않는다(908행).
