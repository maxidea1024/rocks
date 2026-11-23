# 엔진 셰이더 — 4. 영역별 대응

[목차](../README.ko.md)

## 4. 영역별 대응

영역마다 같은 틀로 적는다.

- **역할**: 이 영역이 하는 일.
- **UE 파일**: 이 영역에 분류한 UE 파일 이름(전체 목록은 12장). 경로가 `Private/` 바로 아래면 폴더를 생략한다.
- **의존**: 이 영역을 쓰는 렌더러 기능(RN·RB 절·단계)과 이 영역이 쓰는 다른 영역.
- **Rocks 파일**: Rocks 가상 경로(`/Engine/` 생략)와 담는 내용. 이름은 3.5절 규칙이다.
- **단계**: 6장 단계 ID(SL0–SL10, UI는 SLU, 후속).
- **근거 문헌**: 공개 논문·발표·표준. UE 원문은 근거로 적지 않는다(근거로 쓰는 것은 UE 파일의 역할과 위치뿐이다).

기본 영역(A01–A23)은 절 하나씩, 후속 영역(A24–A35)은 4.24절 한 곳에 모은다.

| 절 | 영역 | 단계 |
|---|---|---|
| [4.1](04.01-PlatformCommon.ko.md) | A01 플랫폼·공통 매크로 | SL0 |
| [4.2](04.02-MathEncoding.ko.md) | A02 수학·인코딩·패킹·난수·표본 | SL0(1차)·SL2–SL9 |
| [4.3](04.03-Color.ko.md) | A03 색·전달 함수·톤 곡선 | SL0(1차)·SL1·SL8 |
| [4.4](04.04-Shading.ko.md) | A04 BRDF·셰이딩 모델·광원 적분 | SL0(1차)·SL3·SL10 |
| [4.5](04.05-MaterialVertexInput.ko.md) | A05 머티리얼 템플릿·정점 입력 | SL2·SL6 |
| [4.6](04.06-SceneViewCulling.ko.md) | A06 장면 데이터·view·컬링·HZB | SL2·SL6 |
| [4.7](04.07-BasePass.ko.md) | A07 깊이·velocity·베이스 패스·GBuffer | SL2·SL4 |
| [4.8](04.08-LightData.ko.md) | A08 광원 데이터·light grid·광원 함수 | SL3·SL10 |
| [4.9](04.09-DirectLighting.ko.md) | A09 직접 조명·Deferred 조명 | SL3·SL4 |
| [4.10](04.10-Shadows.ko.md) | A10 그림자 | SL5·SL10 |
| [4.11](04.11-Reflections.ko.md) | A11 반사·sky light·캡처·SSR | SL3·SL9·SL10 |
| [4.12](04.12-AmbientOcclusion.ko.md) | A12 AO | SL9 |
| [4.13](04.13-Decals.ko.md) | A13 데칼 | SL9 |
| [4.14](04.14-StaticLighting.ko.md) | A14 정적 조명 표본 | SL9 |
| [4.15](04.15-Translucency.ko.md) | A15 반투명·굴절 | SL7 |
| [4.16](04.16-Atmosphere.ko.md) | A16 하늘 대기·높이 포그 | SL7 |
| [4.17](04.17-PostProcess.ko.md) | A17 포스트 프로세스 기본 | SL2(최소)·SL8 |
| [4.18](04.18-Utility.ko.md) | A18 유틸리티 | SL1·SL10 |
| [4.19](04.19-UserInterface.ko.md) | A19 2D·UI·단순 요소 | SLU·SL10 |
| [4.20](04.20-DebugVisualization.ko.md) | A20 디버그·시각화·편집 표시 | SL1·SL2·SL10 |
| [4.21](04.21-ComputeUtilities.ko.md) | A21 컴퓨트 유틸 | SL3·SL6·후속 |
| [4.22](04.22-Skinning.ko.md) | A22 스키닝·정점 변형 | SL10 |
| [4.23](04.23-AdvancedShading.ko.md) | A23 고급 셰이딩 모델(SSS·물·투과) | SL10 |
| [4.24](04.24-FollowupAreas.ko.md) | A24–A35 후속 영역 | 후속 |
