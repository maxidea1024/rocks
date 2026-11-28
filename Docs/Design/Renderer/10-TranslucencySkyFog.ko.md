# 렌더러(Renderer) 설계 — 10. 투명·하늘·안개

[목차](README.ko.md)

## 10. 투명·하늘·안개

### 10.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 투명 pass 종류 | Standard(DOF 전), Standard Modulate, AfterDOF(+Modulate), AfterMotionBlur, Holdout, All | `Renderer/Internal/TranslucentPassResource.h:12–23` |
| 위치 | 투명 → distortion → light shaft bloom → 투명 upscale | `DSR:4012, 4026, 4095, 4101` |
| 해상도 | 별도 투명 화면 비율(기본 100) | `R/TranslucentRendering.cpp:89–95, 324, 2250` |
| 정렬 | 그리기 명령 정렬 키, OIT 선택 | `R/MeshDrawCommands.cpp:1137–1166`, `R/OIT/OIT.cpp:24, 52` |
| sky atmosphere | LUT 5종(transmittance·다중 산란·먼 하늘빛·sky view·aerial perspective 볼륨), deferred만 전체 화면 pass | `R/SkyAtmosphereRendering.cpp:925–1081, 1478, 2155`, `DSR:3782–3785` |
| height fog | 전체 화면 pass(경로 무관) | `R/FogRendering.cpp:276, 688`, `DSR:3823` |
| 볼륨 안개 | froxel(16 px, Z 64), light grid 사용 | `R/VolumetricFog.cpp:118–129, 576, 1375, 1497`, `SH/VolumetricFog.usf:1054–1055` |
| 볼륨 구름 | 별도 렌더 타깃에 그린 뒤 합성 | `R/VolumetricCloudRendering.cpp:1695, 2702`, `R/VolumetricRenderTarget.cpp:1033` |

### 10.2 Rocks 구조

- **투명 pass 종류.** `TranslucentStandard`(DOF 전), `TranslucentAfterDof`, `TranslucentAfterMotionBlur` 셋이다. `Modulate` 블렌드 모드는 이 세 pass 안에서 그린다(RS5). UE의 Modulate 짝 pass(`…Modulate`, 분리 투명의 modulate를 따로 합성)는 후속이고, Holdout pass는 `AlphaHoldout` 블렌드 모드와 함께 후속이다(14.2절). 머티리얼이 어느 pass에 들어갈지는 머티리얼 설정(`MaterialShaderTraits`의 투명 pass 종류)이다(14.5절).
- **조명.** 투명은 Forward+ base pass와 같은 조명 함수(8.3절)로 표면 조명을 평가한다. 반투명 조명 볼륨(UE `RenderTranslucencyLightingVolume`, `DSR:3479`)은 후속이다.
- **정렬.** 인스턴스 bounds 중심의 view 깊이로 뒤 → 앞 정렬, 같은 깊이는 `RenderInstanceId` 순서로 결정적이다. 인스턴스 안 삼각형 정렬과 OIT는 후속(계획 14.4절 "OIT는 선택 후속").
- **깊이.** 투명은 깊이 쓰기 없음·깊이 `GreaterEqual` 읽기다. velocity는 머티리얼 설정이 켠 경우에만 별도 pass로 쓴다(계획 14.4절 "velocity/DOF 정책").
- **안개 적용.** 투명은 자기 셰이더에서 height fog·aerial perspective를 적용한다(불투명 전체 화면 안개를 덮어쓰지 않으므로).
- **distortion·굴절.** 투명 뒤 별도 pass(SceneColor 복사본을 읽고 offset 버퍼로 합성). 복사는 그래프 Copy pass다.
- **하늘·안개(결정 RN8).** 두 경로 모두 불투명 조명 뒤(F13) 전체 화면 pass로 sky atmosphere와 height fog를 그린다. sky는 깊이가 원평면(reversed-Z 0)인 픽셀만, fog는 깊이에서 거리를 복원해 SceneColor에 혼합한다. UE forward는 sky atmosphere 전체 화면 pass를 건너뛰는데(`DSR:3782`) 그 경우 하늘을 무엇이 그리는지는 확인하지 못했다(**미확인**). Rocks는 두 경로 결과를 같게 하려고 둘 다 pass로 그린다.
- 볼륨 안개(light grid 사용)·볼륨 구름·지역 안개 볼륨은 후속이며 각 스펙이 F13 앞뒤 위치를 정한다.
