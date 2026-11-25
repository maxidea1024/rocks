# 엔진 셰이더 설계(UE 5.8 `Engine/Shaders` 대응)

기준일: 2026-11-25, 개정 5(초안, 교차 검증 1차 반영 — 13장). 결정 SLD1–SLD21은 권장안으로 확정했다(3·10장). SL0(공통 라이브러리 1차, SS SH2g(SL0))만 구현한다(7장, 개정 3). 나머지는 구현하지 않는다. 착수 시점은 고정 구현 순서(리플렉션 B9 → Curve → A0-G → A1–A6 → 화면 마일스톤)를 따르며, 이 문서는 그 순서를 바꾸지 않는다. 다만 렌더러 없이 쓸 수 있는 공통 라이브러리(SL0)는 셰이더 시스템 SH2와 함께 할 수 있다(6·7장).

**사용자 지시(2026-11-23).** "UE `C:\github\UnrealEngine\Engine\Shaders`에 있는 것 대부분을 Rocks도 구현해야 한다." 원문 복사가 아니라 같은 기능을 Rocks가 직접 구현한다는 뜻이다. 같은 날 추가 지시: Nanite·Lumen·Substrate·Niagara·레이 트레이싱·패스 트레이싱 같은 큰 기능은 **후속**으로 확정한다(결정 항목으로 올리지 않는다). 문서의 중심은 기본(플랫폼·공통, 수학·인코딩, 색·톤매핑, BRDF·기본 셰이딩 모델, 머티리얼 템플릿·정점 입력, 베이스 패스, 라이트 컬링, 기본 라이팅·섀도, 스카이·포그·반투명, 기본 포스트 프로세스, 유틸리티, 컴퓨트 유틸)이다. 또 [컴포넌트 도입](../ComponentCatalog/README.ko.md)의 렌더링 필수 컴포넌트가 화면에 나오는 데 필요한 셰이더를 기본 영역에 빠짐없이 넣는다(5장).

**범위.** UE 5.8.4 `Engine/Shaders`(`Public`·`Private`·`Shared`·`StandaloneRenderer`, 1,227개 — 셰이더 1,198·서드파티 기록 `.tps` 28·빌드 규칙 1) 전수를 영역 35개와 제외 10종으로 나누고(1·4·12장), 영역마다 역할·UE 파일·의존 렌더러 기능·Rocks 파일·단계·근거 문헌을 적는다. 엔진 플러그인 셰이더(`Engine/Plugins/*/Shaders`, Niagara 141개 등)와 플랫폼 폴더(`Engine/Platforms/*`)는 범위 밖이다(1.5절).

**UE 근거.** UE 5.8.4(EpicGames/UnrealEngine `release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론 `C:\github\UnrealEngine`, 체크아웃하지 않고 읽기만). 경로 약어 `SH/` = `Engine/Shaders/`, `SRC/` = `Engine/Source/`. **UE 셰이더 원문은 Epic 저작물이므로 이 문서와 Rocks 원문 어디에도 옮기지 않는다.** 파일 이름·역할·구조·알고리즘 이름과 공개 참고 문헌만 적는다(2.2절). UE 셰이더에 들어 있는 서드파티 코드(FXAA·SMAA·ACES·Bend 화면 공간 그림자 등)도 원문을 가져오지 않는다(결정 SLD8).

**문서 이름.** 이 문서는 `Engine/Shaders` 원문(HLSL `.rsf`·`.rsh`·`.h`)을 다룬다. "셰이더 라이브러리"는 SS 12.2·12.3절에서 쿠킹 산출물(UE `ShaderCodeLibrary`·`.ushaderbytecode` 대응)을 가리키므로, 혼동을 피해 이 문서는 원문을 **엔진 셰이더**라 부른다(폴더 `Docs/Design/EngineShaders/`). 단계·결정 ID 접두 SL·SLD·SLR·SLP·SLQ는 초안 이름에서 온 것으로 그대로 둔다. 원문 안 순수 함수 묶음(A01–A04)은 "공통 라이브러리"라 부른다.

**용어.** UE 구조를 따르되 Rocks 파일·함수 이름은 Rocks 서술형 이름이다. UE 파일 이름은 각 영역의 "UE 파일" 칸과 대응표(11·12장)에만 둔다. GBuffer·Forward+·light grid·HZB·CSM·PCF·TAA·tone mapping·BRDF·GGX처럼 업계 공용어는 그대로 쓴다.

**근거 문서 약어.** SS = [셰이더 시스템](../ShaderSystem/README.ko.md)(개정 28), RN = [렌더러](../Renderer.ko.md)(개정 13), RB = [렌더 기반](../RenderBase/README.ko.md), MS = [머티리얼 시스템](../MaterialSystem.ko.md)(개정 9), EIP = [엔진 구현 계획](../EngineImplementationPlan.ko.md), CC = [컴포넌트 도입](../ComponentCatalog/README.ko.md)(개정 7), UI = [UI 툴킷](../UiToolkit.ko.md).

자주 인용하는 장 파일: SS [1절 결정](../ShaderSystem/01-Decisions.ko.md), [2절 형식](../ShaderSystem/02-FormatsAndConversion.ko.md), [3절 구조](../ShaderSystem/03-StructureSummary.ko.md), [7절 바인딩 규약](../ShaderSystem/07-BindingConventions.ko.md), [11절 워커·SH1(SC1–SC14)](../ShaderSystem/11-WorkerCommunication.ko.md), [12절 형식 정의·셰이더 타입·쿠킹](../ShaderSystem/12-FormatDefinitionsCooking.ko.md), [15절 결정 사항](../ShaderSystem/15-OpenDecisions.ko.md), [16절 이후 단계(SH0–SH6)](../ShaderSystem/16-LaterStages.ko.md); CC [3절 필수 컴포넌트](../ComponentCatalog/03-RequiredComponents.ko.md), [5절 묶음 순서](../ComponentCatalog/05-BundleOrder.ko.md), [8절 폴더 밖](../ComponentCatalog/08-OutsideFolder.ko.md), [9절 플러그인](../ComponentCatalog/09-Plugins.ko.md).

**좌표·단위 규약(절대 규칙).** UE 5.8과 같다: 왼손 +X 전방·+Y 오른쪽·+Z 위, 길이 cm, 행 벡터 `p·M`과 셰이더 `mul(v, M)`·행 우선 packing, view +X 오른쪽·+Y 위·+Z 전방, reversed-Z 무한 원평면, 앞면 화면 반시계(EIP 2.2절, SS 7.2절 6·8항, RN 1절). 엔진 셰이더 규약은 3.4절이다.

## 목차

- [1. UE 구조 조사](01-UnrealSurvey.ko.md)
- [2. 요구](02-Requirements.ko.md)
- [3. 디렉터리·이름·셰이더 규약](03-LayoutAndNaming.ko.md)
- [4. 영역별 대응](04-Areas/README.ko.md)
- [5. 렌더링 컴포넌트 → 셰이더 표](05-ComponentCoverage.ko.md)
- [6. 단계표](06-Stages.ko.md)
- [7. 공통 라이브러리 1차 범위(SL0, SH2와 함께)](07-CommonLibraryFirstScope.ko.md)
- [8. 시험 방침](08-Testing.ko.md)
- [9. 한계·후속·다른 문서 요구](09-LimitsAndFollowups.ko.md)
- [10. 결정과 버린 대안](10-DecisionsAndAlternatives.ko.md)
- [11. UE 대응표(Rocks 파일 → UE 파일)](11-UnrealMapping.ko.md)
- [12. UE 파일 전수 분류](12-UnrealInventory.ko.md)
- [13. 개정 기록](13-Revisions.ko.md)

## 요약

| 구분 | 영역 | 단계 |
|---|---|---|
| 공통 라이브러리 | A01 플랫폼·공통, A02 수학·인코딩·난수, A03 색·전달 함수, A04 BRDF·셰이딩 모델 | SL0(SH2와 함께), 나머지는 쓰는 RS 단계 |
| 화면 마일스톤 | A18 유틸리티, A20 텍스처 표시 | SL1 |
| 정식 렌더러 기본 | A05–A17, A19–A23 | SL2–SL10(RN RS0–RS13, UI UT2) |
| 후속 | A24 포스트 확장, A25 볼륨, A26 Nanite, A27 Lumen·거리장, A28 RT·PT, A29 Substrate, A30 VSM, A31 MegaLights, A32 Hair, A33 VT, A34 지형·물, A35 XR·미디어 등 | 후속(상세 설계 없음) |
| 제외 | 폐기·제거 예정, Cascade, 콘솔 전용, 벤더 확장, StandaloneRenderer, 미사용, Chaos 필드, Bink, UE 모바일 렌더러 전용, 빌드 규칙 | 없음(1.4절) |
