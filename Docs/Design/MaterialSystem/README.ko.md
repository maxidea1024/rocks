# 머티리얼 시스템(MaterialSystem) 설계

기준일: 2026-11-26, 개정 15(초안 — 노드 범위를 UE 5.8 전체로 19장, MS3b 리뷰 반영 4.6.1·4.6.2절, 노드 정의 코드 19.13절). [엔진 구현 계획](../EngineImplementationPlan.ko.md) 14.1·14.4–14.9절의 머티리얼 요구를 구현 스펙으로 정한다. 범위는 다음과 같다.
- 머티리얼 에셋 모델(머티리얼·인스턴스·함수·레이어·파라미터 컬렉션)과 쿠킹 데이터
- 그래프 스키마, typed IR, IR에서 HLSL을 만드는 코드 생성
- 퍼뮤테이션, 파라미터 layout과 상수 식, 인스턴스 override와 static 변형
- 표면 출력 모델(shading model·Substrate 대응)과 렌더러에 넘기는 계약
- 비동기 컴파일·대체 머티리얼·미리보기·hot reload, 쿠킹·에셋 너겟·실행 시 로드
- backend 제약, 에디터가 요구하는 런타임·도구 기능

머티리얼 에디터 화면(그래프 편집 UI)은 에디터 단계의 일이라 범위 밖이다. 이 문서는 에디터가 쓸 런타임·도구 기능만 요구로 적는다(14절).

UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론) 원본이다. 경로 약어는 다음과 같다.
- `RE/` = `Engine/Source/Runtime/Engine/`
- `RR/` = `Engine/Source/Runtime/Renderer/`
- `RC/` = `Engine/Source/Runtime/RenderCore/`
- `ED/` = `Engine/Source/Editor/`
- `SH/` = `Engine/Shaders/`
- `CFG/` = `Engine/Config/`
- 그 밖은 `Engine/Source/` 기준으로 적는다.

확인하지 못한 사실은 "미확인"으로 적는다. 근거 없이 채운 항목은 없다.

**관계 문서.**
- [셰이더 시스템](../ShaderSystem/README.ko.md)(개정 12): 원문 규약·바인딩 규약 7.2절·job 키·셰이더 타입과 퍼뮤테이션(P14)·파라미터 구조체(P7)·라이브러리. 이 문서는 그 위에 머티리얼이 만드는 원문과 layout을 얹는다. 충돌은 그 문서의 결정을 따르고, 고칠 곳은 16절에 모은다.
- [렌더 기반](../RenderBase/README.ko.md)(개정 9): 셰이더 집합·바인딩 지도·상수 블록·공유 상수 slot·파이프라인 캐시·정점 입력 타입.
- [에셋 시스템](../AssetSystem/README.ko.md)(개정 8): 에셋·참조 종류·에셋 너겟 키·Cooker.
- [텍스처 스트리밍](../TextureStreaming.ko.md)(개정 3) 결정 T4: 재질 sampling scale 공급.
- [Renderer](../Renderer.ko.md)(개정 3): GBuffer 배치·조명 평가·pass 목록과 pass 셰이더, 그리고 14절 "렌더러가 머티리얼에 요구하는 계약"(`SurfaceAttributes`와 머티리얼 함수, shading model·블렌드 모드·영역 번호표, pass별 경로·퍼뮤테이션, 경계표, 이 문서 요구의 처리). 이 문서는 그 계약에 맞춘다(10.1절). 두 문서의 정합 결과는 10.4절이다. 이 문서에서 "Renderer 스펙"이라 부른다.
- [노드 그래프](../NodeGraph/README.ko.md)(개정 2): 그래프 문서 모델(`GraphDocument`)·노드 서술 등록표·`GraphSchema`·변경·undo·복사·의미 바이트·migration. 머티리얼 그래프는 그 위의 도메인 schema다(3.2·4.3·5.2·5.5·14절).

**용어 방침.** UE 구조는 따르되 이름은 Rocks 이름을 쓴다. UE의 클래스·함수·CVar 이름은 대응표(2절)와 "UE 5.8 방식" 표·근거에만 적는다. shader·pass·BSDF·closure·GBuffer·uniform·sampler·permutation처럼 업계에서 함께 쓰는 말은 그대로 쓴다. 계획 문서에 이미 있는 Rocks 이름(MaterialSchema·MaterialCompiler·MaterialRuntime·MaterialCook·CompileCoordinator·PreviewSession)은 그대로 쓴다.

**UE 레거시 제외.** UE가 하위 호환을 위해 남긴 경로는 따르지 않는다(사용자 원칙). 다만 UE 5.8은 "이름은 legacy이지만 기본값인" 경로를 갖고 있어(6.1절 번역기, 10.2절 shading model) 조사에서 기본 여부와 레거시 표시를 따로 확인했다.

상태 표기는 AssetSystem과 같다: **구현**(동작·시험), **계약**(API·형식이 있고 미지원 값은 명시 오류), **후속**(API도 없음). MS3a(에셋 클래스 골격) 말고는 **설계**다. 노드 목록의 상태 "구현"·"TODO"는 19.2절 정의다.

## 장 목록

- [1. 결정 요약](01-DecisionSummary.ko.md)
- [2. 용어 대응](02-Terms.ko.md)
- [3. 모듈 배치와 의존](03-Modules.ko.md)
- [4. 에셋 모델](04-AssetModel.ko.md)
- [5. 그래프 스키마](05-GraphSchema.ko.md)
- [6. IR](06-IR.ko.md)
- [7. 코드 생성](07-CodeGeneration.ko.md)
- [8. 퍼뮤테이션](08-Permutations.ko.md)
- [9. 파라미터](09-Parameters.ko.md)
- [10. shading model과 출력](10-SurfaceModel.ko.md)
- [11. 비동기 컴파일·대체 머티리얼·미리보기·hot reload](11-CompilePreviewReload.ko.md)
- [12. 쿠킹과 에셋 너겟, 실행 시 로드](12-CookingAndLoad.ko.md)
- [13. backend 지원표와 제약](13-BackendSupport.ko.md)
- [14. 에디터 요구](14-EditorRequirements.ko.md)
- [15. 단계와 완료 기준](15-Stages.ko.md)
- [16. 다른 문서에 반영할 요구](16-OtherDocRevisions.ko.md)
- [17. 결정 사항](17-Decisions.ko.md)
- [18. 개정 이력](18-Revisions.ko.md)
- [19. 노드 목록과 머티리얼 체계 대응(UE 5.8 전수)](19-NodeCatalog/README.ko.md)
