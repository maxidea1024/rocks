# 엔진 셰이더 — 10. 결정과 버린 대안

[목차](README.ko.md)

## 10. 결정과 버린 대안

### 10.1 결정(권장안 확정)

사용자 지시(2026-11-19 "애매하면 UE 5.8 방식", 결정은 권장안으로 확정·보고)에 따라 모두 권장안으로 확정했다. 큰 기능(Nanite·Lumen·Substrate·Niagara·RT·PT)은 사용자가 후속으로 정했으므로 결정 항목이 아니다.

| ID | 결정 | 근거 | 절 |
|---|---|---|---|
| SLD1 | `Private` 아래를 영역 폴더로 나누고 최상위에 파일을 두지 않는다. `Public`은 플랫폼 머리만, `Shared`는 C++·HLSL 공용 `.h`만 | UE `Public`·`Private`·`Shared` 역할은 따르되 469개 최상위 파일 구조는 찾기 어렵다 | 3.1 |
| SLD2 | Rocks 서술형 파일 이름. 모든 셰이더가 include하는 머리(`ShaderPrelude.rsh`)는 작게 두고 view·장면 텍스처는 쓰는 pass만 include | UE `Common.ush`는 수학·view·장면 텍스처를 한 파일에 몰아 include 폐포가 커지고 SS 키(내용 해시)가 작은 변경에도 모든 셰이더를 무효화한다 | 3.5, 4.1 |
| SLD3 | BRDF·셰이딩 모델의 UE 근거는 `BRDF.ush`와 Substrate 단일 slab 평가의 역할이다. 5.7 deprecated 고전 함수는 근거로 쓰지 않는다 | 레거시 제외 규칙(엔진 기본 `r.Substrate`=0이지만 고전 함수에 deprecated 표시가 있고 새 프로젝트는 Substrate — `RenderUtils.cpp:2068–2083`), MS 결정 MD1(Substrate형 그래프 → `SurfaceAttributes`) | 4.4 |
| SLD4 | 1차 범위는 float만. half·`min16*`·lane 벡터화는 측정 뒤 후속 | SS 7.2절 5항이 상수 버퍼의 half를 막고 GLES는 모두 highp. 형식마다 half 지원이 달라(DXBC `min16`, SPIR-V 16-bit storage 기능) 결과 대조가 복잡해진다 | 4.1, 9.2 |
| SLD5 | 웨이브 연산은 CS 전용 `WaveOperations.rsh`로만 쓰고(SL1), 형식 기능 `waveOperations`가 없는 형식(지금 `D3D12_SM6` 밖 전부)은 groupshared 대체로 같은 결과를 낸다. 결과는 웨이브 단위가 아니라 그룹 단위로 정의한다. 정수 연산은 두 경로 bit 일치, float 합은 순서가 달라 허용 오차 안 일치이며 순서 무관 결정성이 필요한 곳은 정수 고정소수점을 쓴다 | GLES·D3D11 필수(SS 1절), 형식 정의 `features`가 `D3D12_SM6`에만 `waveOperations`를 준다. 결과가 웨이브 크기(32·64)에 따라 달라지면 backend 대조가 깨진다 | 4.1, 8.5 |
| SLD6 | 큰 월드: 카메라 상대 float + 확장 정밀도(float 둘) 하나만. tile·offset 방식은 두지 않는다 | EIP 2.2절 "렌더링은 카메라 상대 float". UE도 `WorldSpaceMath.ush`가 두 방식 중 하나를 고르며, 확장 정밀도 하나면 충분하다(no backward compat) | 4.2 |
| SLD7 | SDR 톤 매핑은 파라미터 filmic 곡선(UE 후처리 설정과 같은 의미), HDR은 ACES 2.0 출력 변환(후속). ACES 1.3은 들이지 않는다 | UE 5.8 ACES 버전 기본값 2(`RenderCore.cpp:399–405`), 하위 호환 금지 | 4.3 |
| SLD8 | UE 셰이더 안 서드파티 코드(FXAA 3.11·SMAA·ACES·Bend·ShaderFastLibs 등)도 원문을 가져오지 않고 원 저작자 공개 문서로 구현한다. 원 저작자 참조 구현이 필요하면 ThirdParty 절차로 시험 대조 자료로만 쓴다 | 사용자 지시(원문 복사 금지), 출처·라이선스 단순화 | 2.2 |
| SLD9 | 같은 pass 계열의 VS·PS·CS 엔트리는 한 `.rsf`에 둔다. 다만 성격이 다른 pass(Forward+ 불투명·GBuffer·투명)는 파일을 나눈다 | UE는 한 파일 다중 엔트리를 쓰지만 `BasePassPixelShader.usf`처럼 경로를 모두 한 파일에 섞으면 정의 분기가 많아진다 | 3.5, 4.7 |
| SLD10 | CPU 기준 구현은 시험 쪽에 따로 쓴 `double` 구현이다. 셰이더 원문을 C++로 컴파일해 재사용하지 않는다 | 같은 원문을 대조하면 원문 오류를 잡지 못한다 | 8.2 |
| SLD11 | UE 모바일 렌더러 전용 파일은 대응하지 않는다. GLES도 같은 렌더러·원문을 형식 사실로 거른다 | RN 1절(렌더러 하나, view별 경로) | 1.4 |
| SLD12 | Cascade 파티클, 단독 렌더러, Chaos 물리 필드, Bink, 콘솔 전용, 미사용 파일은 제외한다 | CC CD5·CD16, SS 1절(HLSL 하나·데스크톱 GL 제외), EIP 16절(Jolt), 콘솔 후속 | 1.4 |
| SLD13 | 벤더 셰이더 확장(AMD AGS·Intel·NVAPI)은 쓰지 않고 SM 6 표준 웨이브 연산으로 같은 일을 한다 | 벤더 SDK 헤더·라이선스, 형식 하나에 묶인 원문 분기 | 1.4 |
| SLD14 | GPU 메시지·셰이더 안 print·GPU assert 기록을 SL2(첫 렌더러 단계)에 둔다. CS는 모든 형식에서 기록하고, VS·PS 쪽은 SLD18을 따른다 | 이후 모든 단계의 진단 수단. UE도 ShaderPrint·GPUMessaging을 기반 기능으로 둔다 | 4.20 |
| SLD15 | SL0(공통 라이브러리 1차)을 SS SH2와 함께 한다(선택이 아니라 권장 일정). 하지 못하면 SL2에서 같은 범위·시험으로 한다 | 렌더러 기능이 없는 순수 함수라 고정 순서를 앞당기지 않으며 SH2 워커 규칙을 다양한 원문으로 먼저 검증한다 | 6, 7 |
| SLD16 | 시험 셰이더는 엔진 셰이더 트리(`Engine/Shaders`) 밖, 시험 전용 플러그인 루트(`/Plugin/EngineShaderTests`)에 두고 엔진 내장 셰이더 라이브러리(SS 12.3절 쿠킹 산출물)에 넣지 않는다 | 출시 라이브러리 크기·쿠킹 시간, 시험 셰이더의 의도적 오류 fixture | 7.6 |
| SLD17 | morph delta 누적은 정수 고정소수점 원자 합(32-bit `InterlockedAdd`)으로 한다. 배율·범위는 이름 있는 상수, 넘침 없음은 쿠킹 때 morph 최대 delta로 검증한다 | 정수 덧셈은 순서와 무관하게 결정적이라 GLES 포함 모든 형식·dispatch 순서에서 bit 일치한다. float 원자 합은 형식마다 지원이 다르고 순서에 따라 결과가 달라진다 | 4.22 |
| SLD18 | VS·PS는 UAV를 쓰지 않는다(SS 7.2절, 형식 정의 `uavStages` = `compute`). PS가 쓰는 디버그 기능(GPU assert·print·quad overdraw)은 "디버그 전용 PS UAV" 형식 기능(SLR12)이 있는 형식·개발 빌드에서만 기록하고, 없는 형식(D3D11·GLES 포함)에서는 no-op이다. CS 경로는 그대로다 | SS·RN의 금지 규칙을 디버그 때문에 일반 규칙으로 풀지 않는다. 출시 셰이더에는 영향이 없다 | 3.2, 4.1, 4.20 |
| SLD19 | (개정 3) 시험을 둘로 나눈다. 컴파일 행렬(C)은 EngineTests가 실제 워커로 형식 정의 전부에 컴파일하고, GPU probe(N·G·S·P)는 실제 장치가 있는 Sandbox self-test가 여덟 장치(D3D12·WARP, D3D11·WARP, OpenGL ES 두 context, Vulkan 하드웨어·소프트웨어)에서 돌린다. 시험 셰이더 폴더는 `Programs/Sandbox/EngineShaderTests` 하나이고 두 runner가 `/Plugin/EngineShaderTests`로 mount한다. CPU 기준 구현은 Sandbox 쪽 `EngineShaderReference*.cpp`다 | EngineTests에는 GPU 장치 시험이 없고(장치 계약은 Sandbox self-test 몫 — SS P11·S4), Sandbox self-test는 장치마다 그 형식 하나만 컴파일하므로 `GLES31_ANDROID`·`VULKAN_ANDROID`·Metal 컴파일은 워커를 직접 부르는 EngineTests가 맞다 | 7.6 |
| SLD20 | (개정 3) probe 입력은 시험 전용 정수 생성기(사례 번호·흐름 번호 → 32 bit, 곱셈·xorshift만)와 24 bit 가수 변환으로 만든다. 라이브러리 해시(`IntegerHash.rsh`)를 입력 생성에 쓰지 않는다 | 시험 대상 함수로 입력을 만들면 그 함수의 오류가 입력에 섞인다(SLD10과 같은 이유). 정수 연산만 쓰므로 GPU·CPU 입력이 bit 단위로 같고 CPU 기준은 같은 입력을 `double`로 계산한다 | 7.6, 8.4 |
| SLD21 | (개정 3) SL0 probe 결과는 `Device::ReadTexture`로 `RGBA32Float`·`R32Uint` 렌더 타깃을 바로 읽는다. `Utility/TextureReadback.rsf`는 SL1로 옮긴다 | 두 형식은 네 backend가 모두 렌더 타깃·readback을 지원한다(A0-G, RB GX8). 변환 셰이더는 readback 불가 형식(블록 압축·GLES의 비색 형식)에만 필요하다 | 7.6, 4.18 |
| SLD22–SLD28 | (개정 6) SL1 나머지(RB SM13): 범위(SM13 행 밖 SL1 항목은 소비자가 생길 때), 밉 생성 두 경로·정확한 면적 박스, 부분 clear·UAV clear 퍼뮤테이션, 그룹 연산 웨이브 배치 가정의 장치별 확인, 65,535 고정 폭 감싸기, 상수는 생성 구조체·자원은 원문 선언, 기본 텍스처 캐시 | 14.7절 표(근거·버린 대안 함께) | 14 |

### 10.2 버린 대안

| 대안 | 버린 이유 |
|---|---|
| UE 파일 구성·이름을 1:1로 따르기(`Common.rsh`·`BasePassPixelShader.rsf` …) | Rocks 고유 이름 규칙에 어긋나고, 큰 공용 머리와 다경로 파일 구조가 SS 내용 해시 키와 맞지 않는다(SLD2·SLD9) |
| HLSL 2021 템플릿으로 벡터 폭별 함수를 한 번에 쓰기 | `D3D11_SM5`·GLES 형식에서 HLSL 2021이 꺼져 있다(SS 결정 2). 템플릿 원문은 이 형식들에서 컴파일되지 않는다 |
| 셰이더 원문을 C++로 컴파일해 CPU 기준으로 쓰기 | SLD10. 오류 공유, HLSL 내장 함수 의미 차이(정밀도·`saturate`) 흉내 비용 |
| 고전 shading model 함수(UE 5.7 deprecated)를 근거로 삼기 | 레거시 제외 규칙. Rocks 모델은 처음부터 Substrate형 그래프이고 렌더러 계약만 고전 속성이다 |
| UE처럼 모바일 전용 셰이더 세트를 두기 | 렌더러 하나 원칙(RN 1절). GLES 품질 등급은 퍼뮤테이션·형식 사실로 다룬다 |
| 웨이브 연산을 필수로 두고 GLES·D3D11에서는 해당 기능 끄기 | 기본 영역(light grid·histogram·컬링)이 GLES 필수 경로에 있다 |
| half를 기본 정밀도로 쓰기 | SLD4. 형식별 지원·검증 비용, GLES는 이미 highp |
| tile·offset 큰 월드 좌표 함께 두기 | SLD6. 두 방식을 함께 두면 함수가 두 벌이 된다 |
| 공개 셰이더 SDK(예: MIT 라이선스 후처리 모음)를 ThirdParty로 들여 기본 영역을 채우기 | 바인딩 규약(SS 7.2절)·HLSL 2018·GLES 제약·좌표 규약에 맞추는 수정 비용이 직접 구현보다 크고, 출처가 섞인다. 외부 SDK가 본질인 업스케일러(FSR·DLSS·XeSS)는 EIP 12절 adapter로 따로 둔다 |
| 큰 기능(Nanite·Lumen 등)도 지금 상세 설계 | 사용자 결정(후속). 영역·대응표만 남긴다 |
| (개정 3) GPU probe를 EngineTests에 두고 장치를 직접 만들기 | EngineTests는 CI에서 GPU 없이 돌고, 여덟 장치 생성·공급자·셰이더 집합 연결이 Sandbox self-test에 이미 있다. 같은 연결을 EngineTests에 다시 두면 장치 준비 코드가 두 벌이 된다 |
| (개정 3) probe 입력을 라이브러리 `HashPcg`로 만들기 | SLD20. 해시 오류가 입력에 섞이고 golden이 그 오류를 함께 굳힌다 |
| 플러그인 셰이더(Niagara 등)를 이 문서 범위에 넣기 | 지시 범위는 `Engine/Shaders`. 플러그인은 CC 9절·기능 문서가 정한다 |
| (개정 6) SL1 나머지의 버린 대안 | 14.7절 표 "버린 대안" 열 |
