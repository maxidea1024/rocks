## 9. 셰이더 런타임

셰이더 시스템 스펙은 오프라인 컴파일·쿠킹·바인딩 규약·라이브러리 형식과 조회 키까지 정한다. RenderCore의 런타임 객체는 거의 없다. 이 절이 그 빈자리를 채우고, 셰이더 시스템 문서를 고쳐야 할 곳은 9.9절에 모은다.

### 9.1 UE 5.8 흐름

| 단계 | UE 5.8 | 근거 |
|---|---|---|
| 등록 | 셰이더 타입 하나 = 단계 하나, 정적 등록. 타입 종류는 Global·Material·MeshMaterial 등 7종. 타입 함수표에 컴파일 거르기와 PSO 미리 준비 여부 | `RC/Public/GlobalShader.h:433`, `RC/Public/Shader.h:1219–1226, 1256–1257` |
| 퍼뮤테이션 | 차원 도메인. specialization constant 차원도 있다 | `RC/Public/ShaderPermutation.h:272, 381–387, 636, 651` |
| 셰이더 객체 | 바인딩 표·파라미터 정보·UB 목록·타입 참조·자원 번호. 바인딩 표는 셰이더 생성자에서 컴파일 결과의 파라미터 지도로 만들어 셰이더 맵과 함께 저장(오프라인) | `Shader.h:733–785, 950–995`, `RC/Public/ShaderParameterStruct.h:50–56` |
| 셰이더 맵 | 내용(셰이더 객체·바인딩·타입 → 셰이더 표, memory image) / 포인터 표 / 코드 / 장치 셰이더 배열. 쿠킹된 경우 코드는 라이브러리에서 붙이고, 없으면 Error 로그를 남기고 내용을 확정하지 않는다 | `Shader.h:2535–2636`, `RC/Private/ShaderMap.cpp:291–376` |
| 엔진 셰이더 맵 로드 | 셰이더 타입 초기화 직후. 쿠킹 실행은 캐시 파일과 "Global" 라이브러리. 없고 컴파일도 안 되면 창 지원 플랫폼은 오류 대화상자 뒤 종료 요청, 나머지는 Fatal | `Launch/Private/LaunchEngineLoop.cpp:3301–3307`, `Engine/Private/ShaderCompiler/ShaderCompiler.cpp:5314–5316, 5518–5528, 5715–5724`, `RC/Private/ShaderLibrary/ShaderCodeLibrary.cpp:2272` |
| 장치 셰이더 | 처음 쓸 때 만든다(원자 포인터, 잠금 밖 생성·경쟁에서 진 쪽 해제, 생성 뒤 코드 해제). 로드 때 모두 만드는 선택(기본 끔) | `Shader.h:405–417`, `RC/Private/ShaderResource.cpp:566–710`, `ShaderCompiler.cpp:532–536` |
| 라이브러리 API | 열기·닫기·자원 로드·preload·셰이더 생성. 셰이더 맵 hash → 셰이더 인덱스 범위로 묶는다 | `RC/Public/ShaderCodeLibrary.h:213–288`, `RC/Public/ShaderCodeArchive.h:48–79, 206–230` |
| 파라미터 메타데이터 | 구조체 매크로가 멤버별 정렬 검사와 멤버 기술을 만든다. 용도는 파라미터 구조체·UB·데이터 기반 UB. UB는 생성 HLSL을 include하고, 파라미터 구조체는 생성 HLSL 없이 이름으로 매칭한다(느슨한 전역, 형식에 따라 루트 cbuffer로 옮김) | `RC/Public/ShaderParameterMacros.h:1308–1495`, `RC/Public/ShaderParameterMetadata.h:139–176`, `RC/Private/ShaderParameters.cpp:85–99`, `RC/Private/ShaderParameterStruct.cpp:251–330`, `RC/Private/ShaderParameterParser.cpp:1145–1290` |
| 그리기 때 설정 | 바인딩 표대로 상수 칸·자원 칸을 묶음 객체에 넣어 한 번에 올린다. Development에서 구조체 layout hash가 다르면 실패 | `ShaderParameterStruct.cpp:520–527, 912–990`, `RHI/Public/RHIShaderParameters.h:246–330` |
| UB 런타임 | 수명 3종, layout 객체, 렌더 자원 래퍼. 정적 slot(최대 255)을 명령 목록에 묶고 셰이더별 slot 표로 연결. OpenGL은 slot별로 저장했다가 파이프라인 설정 때 셰이더 slot 표대로 묶고, 그리기 때 UB 고유 ID를 비교해 바뀐 것만 packed 배열에 복사 | `RHI/Public/RHIDefinitions.h:517–525, 721`, `RC/Public/UniformBuffer.h:20–77`, `ShaderParameterMacros.h:1511–1590`, `RHI/Public/RHIResources.h:823–835`, `D3D12RHI/Private/D3D12Commands.cpp:180–211, 420–427`, `OpenGLDrv/Private/OpenGLCommands.cpp:896–909, 2574–2589`, `OpenGLDrv/Private/OpenGLShaders.cpp:3289–3370` |
| PSO | 초기화자(정점 선언·셰이더·상태·RT 형식). 캐시는 RHI 모듈에 있고 비동기 미리 준비·캐시 확인을 둔다. 기록 파일 캐시와 시작 precompile | `RHIResources.h:4440, 4655, 4835–4842`, `RHI/Public/PipelineStateCache.h:165–280`, `RHI/Public/PipelineFileCache.h:314–405`, `RC/Public/ShaderPipelineCache.h:4–66` |

### 9.2 셰이더 집합

- **내용.** 셰이더 라이브러리(셰이더 시스템 12.2절)에서 연 항목마다: 키(셰이더 타입, 정점 입력 타입, 퍼뮤테이션 ID, 단계 — 셰이더 시스템 12.2절 키에 SX4의 정점 입력 타입을 더한 것), 코드 참조, 정규화 바인딩 layout(`ShaderBindingLayout`, 셰이더 시스템 7.2절 3항), 파라미터 레이아웃 서명, 파라미터 바인딩 지도(9.3절), 장치 셰이더 자리.
- **표현.** UE frozen memory image를 들이지 않는다. 정규화 표는 Core Archive·Record로 읽는다. memory image의 이득에 대한 Epic의 설계 근거는 확인하지 못했고(미확인), 로드 비용이 문제가 되면 측정 M3로 다시 본다.
- **수명.** 셰이더 집합은 `DeviceResourceOwner`다. 쓰는 쪽(파이프라인 캐시 항목, 기록 packet)은 집합의 lease를 잡는다. `Retire`된 집합은 새 lease와 새 장치 셰이더 생성을 막는다.
- **장치 셰이더 생성.** 처음 쓸 때 만든다. 만드는 스레드는 GX11 규칙을 따른다: 스레드 안전 생성 backend는 요청한 스레드에서, OpenGL ES는 렌더 lane 작업으로. 경쟁 시 생성이 두 번 일어날 수 있으나 게시는 한 번이고 진 쪽은 `Retire`한다(UE와 같다, `ShaderResource.cpp:579–657`). 만든 뒤 코드 바이트를 놓는다. 로드 때 일괄 생성 선택을 콘솔 설정으로 두며 병렬 일괄 생성은 CP2를 쓴다.
- **런타임 확인 기능(개정 16).** 셰이더 시스템 7.2절 7항의 런타임 확인 기능(`vertexViewportLayerOutput`)을 요구하는 셰이더는 라이브러리 항목에 그 기능 비트가 있다(셰이더 시스템 12.2절). 셰이더 집합은 장치 셰이더를 만들기 전에 이 비트를 `DeviceCaps.features`와 대조하고, 없으면 만들지 않고 `Unsupported`를 돌려준다. 그 셰이더를 쓰는 파이프라인 캐시 항목도 만들지 않는다. 완결성 검사(아래)는 장치와 무관하게 그 항목이 라이브러리에 있는지 본다(쿠킹은 형식 기준이다). 호출자는 능력을 먼저 확인해 대체 경로를 고른다.
- **엔진 셰이더 집합과 완결성 검사.** 렌더 lane 시작 단계(5.5절)에서 엔진 셰이더 집합을 연다(엔진 호스트 연결은 9.12절). 형식마다 공급 경로가 둘이다.
  - **쿠킹 라이브러리 형식**(셰이더 시스템 12.3절 엔진 내장 라이브러리): 등록된 엔진 셰이더 타입 × 정점 입력 타입 × 그 형식에서 컴파일되는 퍼뮤테이션(P14 거르기 결과) 전체가 라이브러리에 있는지 검사한다. 하나라도 없으면 시작이 `IncompatibleCook`으로 실패한다. UE는 엔진 셰이더 맵이 없으면 종료하고(창 지원 플랫폼은 오류 대화상자 뒤 종료 요청, 9.1절) 개별 셰이더는 로드 때 오류다. Rocks는 정적 등록으로 전체를 알 수 있으므로 시작 때 결정적으로 실패시킨다.
  - **필요 시 컴파일 형식**(`GLES31_PC`, 개발 트리에서 내장 라이브러리가 없는 형식, 셰이더 시스템 12.3절): Runtime 모듈은 Developer 셰이더 컴파일러에 의존할 수 없으므로 RenderBase가 **셰이더 공급자** interface를 정의하고, Development 실행 파일이 Developer/ShaderCompiler 구현을 등록한다(SX14). 집합은 "공급 대기" 상태로 열리고, 공급자가 엔진 셰이더 전체를 컴파일해 완료 통지를 보내면 같은 완결성 검사를 한 뒤 `Ready`가 된다. 공급자가 없는 실행 파일(Release)에서 이 형식이 선택되면 `UnsupportedCapability`다.
- **타입 있는 셰이더 참조** `TypedShader<T>`: (셰이더 타입, 집합)의 쌍. 렌더 그래프 pass와 그리기 유틸리티가 이것으로 셰이더를 고른다.
- **라이브러리 런타임 API.** 열기·닫기, 엔진 → 프로젝트 순 조회(같은 키가 둘 이상이면 쿠킹 오류), 열 때 레이아웃 서명 검사(9.3절). 플러그인 라이브러리 분할과 묶음 단위 preload는 셰이더 시스템 12.3절대로 후속이다.

### 9.3 파라미터 바인딩 지도와 묶음 설정

- **바인딩 지도.** 파라미터 구조체의 멤버 오프셋 → (단계, 종류, 단계 안 순번)이다. 상수 멤버는 (상수 버퍼 순번, 바이트 오프셋, 크기), 자원 멤버는 (종류, 순번), 상수 블록 참조 멤버는 (상수 버퍼 순번), 그래프 자원 멤버는 (종류, 순번, 그래프 자원 표시)다.
- **만드는 시점.** 셰이더 집합을 열 때 라이브러리의 바인딩 layout과 파라미터 구조체 스키마로 만든다(결정 R11). 스키마는 MetaSchema `TypeOf<T>()`(`Meta/GeneratedType.h`)로 얻는다. RenderBase는 MetaSchema에 의존하고 ObjectRuntime에는 의존하지 않는다(계획 3.1절 렌더 층 "Object 없음"). 스키마를 오래 붙잡는 경우의 타입 lease 수명은 집합 수명과 같게 한다. 만드는 비용은 측정 M3로 확인하고, 문제가 되면 라이브러리 부분으로 굽는다(UE는 오프라인에서 만들어 셰이더 맵에 굽는다).
- **레이아웃 서명 런타임 검증.** 셰이더 시스템은 레이아웃 서명을 job 키에만 넣는다(10절 8항, 13절). 그러면 실행 파일과 쿠킹 라이브러리의 빌드가 어긋날 때 잘못된 오프셋으로 조용히 올라간다. 라이브러리 항목에 서명을 넣고(SX6), 셰이더 집합을 열 때 실행 파일의 구조체 서명과 다르면 `IncompatibleCook`이다. UE는 그리기 때 Development에서만 검사한다(`ShaderParameterStruct.cpp:520–527`). Rocks는 열 때 모든 구성에서 검사한다.
- **묶음 설정** `ParameterBatch`: 상수 칸·자원·상수 블록 참조를 모아 장치에 한 번에 올리는 기록 객체(GX6). Development에서 null 자원·종류 불일치를 검증한다. P7(셰이더 시스템 13절) 전에는 손으로 쓴 레이아웃 표(7.2절 5항)를 입력으로 쓴다.
- **그리기별 바인딩(개정 16, Renderer RQ7).** 그리기마다 바뀌는 상수·자원(머티리얼 파라미터와 텍스처, 정적 조명 `PrecomputedLighting`)은 공유 slot이나 pass 구조체가 아니라 그리기 항목(`MeshDrawItem`)이 가진 바인딩으로 `ParameterBatch`에 넣는다. 상수는 상수 블록 참조, 자원은 그리기별 자원 구조체 `DrawResourceStruct`(9.6절)다. 그래프 의존은 그리기마다 선언하지 않는다. pass가 그 pass의 그리기들이 쓸 수 있는 import 자원 집합(예: 상주 라이트맵 전체, 스트리밍 바인딩 snapshot의 텍스처)을 접근만 선언(9.6절 표)으로 한 번 선언한다. 그래서 pass 구조체가 그리기 수만큼 커지지 않고, 그래프는 그 집합을 pass 동안 SRV 상태로 둔다. 머티리얼 텍스처가 이미 쓰는 경로와 같다(Renderer 14.5절). UE도 라이트맵 자원 구조체와 정적 조명 상수를 그리기 바인딩으로 그리기마다 넣는다(`Renderer/Private/LightMapRendering.cpp:312–333, 346–360`, 구조체 `Engine/Public/SceneManagement.h:725`).

### 9.4 상수 블록

- 수명 3종: `PerDraw`(그리기 한 번), `PerFrame`(프레임 slot 하나, slot GPU 완료 때 회수), `Persistent`(명시 `Retire`).
- backend 표현:
  - D3D12·Vulkan·Metal·D3D11: GPU 버퍼. `PerDraw`·`PerFrame`은 업로드 ring(GX8)에서 할당한다.
  - OpenGL ES: CPU 사본(셰이더 시스템 7.2절 4항 에뮬레이션). `PerDraw`·`PerFrame` 사본은 그래프가 pass를 실행할 때까지 살아 있어야 하므로 프레임 arena(CP3)에 둔다.
- **GLES 업로드 판단.** GL uniform은 program 상태다. 그래서 "버퍼가 바뀌었는가"가 아니라 (program, 상수 버퍼 순번)마다 마지막으로 올린 (상수 블록 identity, version)을 기억하고, 그리기 때 다르면 복사 표대로 `glUniform4fv/4iv/4uiv`로 올린다. 반례: 블록 B를 program P1에 올린 뒤 P2로 바꾸면 B가 그대로여도 P2에는 올려야 한다. 공유 slot도 같다. UE도 그리기 때 UB 고유 ID를 비교해 바뀐 것만 복사한다(9.1절 UB 런타임 행).
- layout 객체: 파라미터 구조체 스키마에서 만든다(크기·멤버 오프셋·자원 멤버 목록·그래프 자원 멤버 목록). 렌더 그래프가 이 목록만 훑어 의존을 만든다(UE layout이 그래프 자원 오프셋을 미리 계산해 두는 것과 같다, `RHI/Public/RHIResources.h:1183–1245`).
- **그리기별 상수(결정 R7 확정, 개정 21).** 파라미터 상수가 루트 상수로 가는지 상수 버퍼(cbuffer)로 가는지는 **선언이 정한다**.
  - 구조: 파라미터 구조체 종류에 "그리기별 상수" 표시가 있는 구조체만 루트 상수(D3D12 root 32-bit 상수, Vulkan·Metal push constant, D3D11 그리기마다 갈아 끼우는 상수 버퍼, GLES `<단계>u` 배열)로 간다. 셰이더 원문은 `ROCKS_ROOT_CONSTANTS(Type, Name)`(셰이더 시스템 7.2절 2항)로, C++ 쪽은 P7 뒤 `ROCKS_SHADER_PARAMETERS`의 종류 표시로 선언한다(SH2 전 개발 경로는 이름 규칙 `cbuffer DrawConstants`). 표시가 없는 구조체는 크기와 무관하게 이름 있는 cbuffer 하나다(셰이더 시스템 7.2절 1항). 파이프라인 하나에 그리기별 상수 블록은 하나이고 모든 단계(VS·PS 또는 CS)가 같은 값을 본다. 파이프라인 layout이 그 크기를 선언한다(Graphics `PipelineDesc::drawConstantBytes`, 4.4절 GX4 SX2 구현).
  - 한계: 0–128 byte, 16 byte 배수(32 DWORD = Vulkan `maxPushConstantsSize` 최소 보장 = `RootConstantContractBytes`). 넘으면 MetaCompiler·셰이더 워커 오류이고(조용히 cbuffer로 옮기지 않는다), 런타임 파이프라인 생성은 `InvalidArgument`다. 멤버 규칙은 셰이더 시스템 7.2절 5항이다. 비용은 backend마다 다르다: D3D11은 `SetDrawConstants`마다 상수 버퍼 `WRITE_DISCARD` 하나, GLES는 program 전환·값 변경 때 `glUniform4fv`다. 그래서 그리기마다 바뀌는 작은 값(인스턴스 목록 offset `MeshDrawConstants`(10.5절), 그리기 유틸리티의 변환·색)에만 쓰고 머티리얼 파라미터처럼 큰 값은 상수 블록 참조(9.3절 그리기별 바인딩)로 둔다.
  - 근거: 배치가 루트 시그니처·pipeline layout·GLES 복사 표·바인딩 지도의 모양을 정하므로, 멤버 하나를 더했다고 배치가 바뀌면 안 된다. 크기 기준이면 경계(128 byte)를 넘는 순간 쿠킹 결과·root 예산·업로드 경로가 조용히 바뀌고, 쓰는 쪽(`SetDrawConstants` 대 상수 블록)도 바뀌어야 한다. 선언 명시는 그 변화가 MetaCompiler 오류로 드러난다. UE도 셰이더의 루트 파라미터를 크기와 무관하게 cbuffer `_RootShaderParameters`(binding 0)에 둔다(`RenderCore/Public/ShaderParameterMetadata.h:172–176`).
  - 버린 대안: 크기 기준 자동 배치(128 byte 이하면 루트 상수, 넘으면 cbuffer). 위 근거대로 배치가 구조체 크기 변화에 흔들리고, backend마다 루트 상수 한도가 달라(D3D12 root 예산 64 DWORD를 다른 인자와 나눔, Vulkan 기기별 `maxPushConstantsSize`) 같은 구조체가 형식마다 다른 곳에 갈 수 있다.

### 9.5 공유 상수 slot

뷰·장면처럼 pass 전체에 한 번 묶는 상수 블록이다. 렌더 그래프·장면 렌더러 단계에서 필요하다.
- slot은 정적 등록(이름, 파라미터 구조체 identity)이다. 최대 수는 형식 정의 상한 안이다.
- 셰이더 쪽: 컴파일러가 셰이더마다 정한 상수 버퍼 번호와 slot을 잇는 표가 필요하므로 `ShaderBindingLayout`의 상수 버퍼 항목에 **파라미터 구조체 identity**를 둔다(SX8).
- 명령 쪽: 기록 객체에 slot 표를 묶고, 파이프라인 설정 때 셰이더의 slot 표로 적용한다(UE와 같다, 9.1절 UB 런타임 행). GLES도 같은 방식이고 업로드는 9.4절 판단을 따른다(UE OpenGL과 같다).
- **렌더러 slot(Renderer RQ7, 개정 15·16).** 렌더러가 등록하는 공유 상수 slot은 아래 다섯이다(Renderer 15.2절). 선언(이름·파라미터 구조체 identity·수명)은 장치 없는 `RendererShaders`(Renderer 3.2절)에 정적으로 둔다. 쿠커는 이 선언으로 바인딩 layout의 구조체 identity를 맞추고(SX8), RenderBase slot 표는 시작 때 정적 등록 표로 받는다(10.2절 정점 입력 타입 등록과 같은 시점, 늦은 등록은 거부).

  | slot | 상수 내용(Renderer 15.2절) | 수명 | 묶는 단위 | 짝 공유 자원 구조체(9.6절) |
  |---|---|---|---|---|
  | `View` | 행렬(현재·이전, jitter 유무), 카메라 이동 보정 상위·하위, render 크기, 시간, 노출, mip bias, 정적 간접 배율 둘 | `PerFrame` | view | — |
  | `Scene` | 인스턴스 수 | `PerFrame` | `ViewSet`(장면 snapshot 하나) | `SceneResources`: GPU Scene typed buffer, 인스턴스 번호 목록(10.5절) |
  | `ForwardLight` | grid 크기·Z 인자, local 광원 수, directional 목록(최대 4) | `PerFrame` | view | `ForwardLightResources`: 광원 데이터·grid typed buffer |
  | `Shadow` | CSM 행렬·분할, atlas 크기 | `PerFrame` | view | `ShadowResources`: shadow atlas(비교 샘플러 짝) |
  | `Fog` | height fog 인자 | `PerFrame` | view | `FogResources`: aerial perspective LUT |

  - `PrecomputedLighting`(정적 조명 상수, 라이트맵·프로브 텍스처)은 개정 16에서 slot에서 뺐다. 그리기마다 바뀌므로 그리기별 바인딩(9.3절)의 상수 블록 하나와 `DrawResourceStruct` 하나다.
  - `SceneTextures`는 상수 부분이 없어 slot이 아니다. 공유 자원 구조체 `SceneTextureResources`(깊이·GBuffer·AO·velocity·HZB)만 있다.
  - **상수 버퍼 예산(개정 16).** pass 셰이더 하나가 쓰는 상수 버퍼는 공유 slot 5 + `PrecomputedLighting` 1 + 머티리얼 상수 블록 1 + 머티리얼 파라미터 컬렉션 최대 2(머티리얼 시스템 9.7절, 각각 cbuffer) + pass 파라미터 구조체 1 = 최대 10이다. 형식 정의 `constantBuffers`(GLES·Vulkan·Metal 12, D3D11 13, D3D12 14, 셰이더 시스템 12.1절) 안이고, 루트 상수는 따로다(셰이더 시스템 7.2절 2항). 이 수는 job 형식 상한으로 워커가 검사한다(셰이더 시스템 7.2절 4항).
  - **GLES uniform 예산.** GLES는 상수 버퍼와 루트 상수가 모두 uniform `vec4` 배열이므로(9.4절), PS 에뮬레이션 `vec4` 224(ES 3.2 최소 보장)를 공유 slot·`PrecomputedLighting`·머티리얼 블록·컬렉션·pass 상수·루트 상수(8 `vec4`)의 **합**으로 검사한다. 렌더러 쪽 합은 구조체를 정한 뒤 정하고(Renderer 15.2절 추정 약 100), 머티리얼 블록과 컬렉션 크기 상한은 그 나머지 안에서 정한다(머티리얼 시스템 9.7절).
  - HLSL 이름은 slot 이름과 같은 cbuffer 이름이다(SX1 "파라미터 구조체 하나 = 이름 있는 cbuffer 하나"). 원문은 공용 `.rsh`의 선언을 include한다. UE도 정적 slot을 이름으로 묶는다(`RC/Public/ShaderParameterMacros.h:1511–1590`).
  - 렌더러는 `ViewSet`마다 `Scene`을, view마다 나머지 slot을 기록 객체의 slot 표에 다시 묶는다(7.5절 선언 순서). GLES는 9.4절의 (program, 순번)별 identity·version 판단으로 바뀐 것만 올린다.
  - 머티리얼 파라미터 컬렉션의 동적 slot(머티리얼 시스템 MRB4)은 이 정적 표 밖이며 머티리얼 단계에서 정한다. 위 예산에는 넣는다.

### 9.6 파라미터 구조체와 렌더 그래프

셰이더 시스템 13절은 파라미터 구조체 멤버를 "셰이더 전용 정렬 타입 + 셰이더 자원 값 타입"으로 닫아 두었다. 렌더 그래프가 같은 구조체를 의존 선언으로 쓰려면(결정 R4) 멤버 종류가 다음과 같아야 한다.

| 멤버 종류 | 뜻 | UE 5.8 대응 |
|---|---|---|
| 상수(정렬 타입) | 상수 버퍼 멤버 | `SHADER_PARAMETER` |
| 장치 자원(텍스처·버퍼 SRV·UAV) | 그래프 밖 영구 자원. 쓰기 자원이 있으면 그 pass는 외부 출력(자동 `HasSideEffects`) | `SHADER_PARAMETER_TEXTURE`·`_SRV`·`_UAV`, `RC/Private/ShaderParameterMetadata.cpp:652–669, 822` |
| 그래프 자원(텍스처·버퍼 SRV·UAV) | 그래프가 추적하는 자원 | `SHADER_PARAMETER_RDG_TEXTURE`·`_SRV`·`_UAV`·`_RDG_BUFFER_*`, `ShaderParameterMacros.h:1697–1760` |
| 샘플러 지정(텍스처 멤버에 붙음) | 그 텍스처를 읽을 샘플러. 없으면 텍스처 desc의 기본 샘플러 | `SHADER_PARAMETER_SAMPLER`, `:1677` |
| 접근만 선언 | 셰이더에 묶지 않고 의존만 선언(복사·간접 인자 등) | `RDG_TEXTURE_ACCESS`·`RDG_BUFFER_ACCESS`, `:1868–1884` |
| 렌더 타깃 slot 묶음 | 색 MRT·깊이 attachment, load/store, mip·slice | `RENDER_TARGET_BINDING_SLOTS`, `:1900` |
| 상수 블록 참조 | 별도 상수 버퍼(그래프 소유 또는 영구) | `SHADER_PARAMETER_RDG_UNIFORM_BUFFER` `:1768`, `SHADER_PARAMETER_STRUCT_REF` `:1854` |
| 펼쳐 넣기 | 다른 파라미터 구조체를 평탄화해 포함 | `SHADER_PARAMETER_STRUCT_INCLUDE`, `:1840` |

규칙:
- **샘플러(결정 R12).** HLSL 쪽 텍스처·샘플러 짝 규칙(셰이더 시스템 7.2절 9항, 텍스처 `X` ↔ `XSampler` 1:1)은 그대로 둔다. 생성기는 텍스처 멤버마다 짝 `XSampler` 선언을 자동으로 낸다. 바뀌는 것은 샘플러 **값**의 출처다: 텍스처 desc의 샘플러는 기본값이고, 파라미터 멤버의 샘플러 지정이 있으면 그것을 쓴다. 그래서 같은 렌더 타깃을 pass마다 point·linear로 다르게 읽을 수 있다. 깊이 비교(그림자) 샘플러는 `SamplerComparisonState XSampler`로 짝을 맺는다(SX13). UE는 샘플러를 별개 멤버로 두고 공유도 허용한다. 한 샘플러를 여러 텍스처가 공유하는 것과 정적 샘플러는 셰이더 시스템대로 후속이다.
- "상수 블록 참조 = 별도 cbuffer", "펼쳐 넣기 = 평탄화"로 정하면 셰이더 시스템 7.2절 5항의 금지(cbuffer 안 구조체 배열·2단 중첩)와 충돌하지 않는다.
- 그래프 수명 구조체는 프레임 arena에 할당하고 pass에 넘긴 뒤 바꾸지 않는다.
- 셰이더가 쓰지 않는 멤버는 바인딩 layout으로 걸러 의존에서 뺀다(UE `RC/Private/RenderGraphUtils.cpp:22–101`). 그래서 바인딩 지도에 멤버 오프셋이 함께 있어야 한다(9.3절).
- **공유 자원 구조체 `SharedResourceStruct`(Renderer RQ7, 개정 15·16).** 공유 상수 slot은 상수 블록이라 자원을 담지 못한다(GLES는 uniform `vec4` 배열, 9.4절). 그래서 렌더러 자원은 상수 부분(9.5절 slot)과 자원 부분으로 나누고, 자원 부분은 공유 자원 구조체로 정의해 그 자원을 쓰는 pass의 파라미터 구조체가 포함한다.
  - **정의.** 그래프 자원·장치 자원·샘플러 지정 멤버만 가진 파라미터 구조체다. 상수 멤버와 렌더 타깃 slot은 넣지 않는다(상수는 slot 쪽, 렌더 타깃은 pass 쪽이다).
  - **포함.** 같은 구조체를 여러 pass 구조체가 멤버 하나로 포함한다. 값은 handle 복사다. 렌더러는 구조체 값을 그래프 구성 때 한 번 채우고(장면 텍스처는 `GraphSharedData`의 `SceneTextureTable`에서, 7.5절) pass마다 복사해 넣는다.
  - **이름(개정 16).** 생성 HLSL 전역 자원 이름은 `<포함 멤버 이름>_<자원 멤버 이름>`이다. pass 구조체가 `SceneTextureResources` 타입 멤버를 `SceneTextures`라는 이름으로 포함하면 `SceneTextures_Depth`, 짝 샘플러는 `SceneTextures_DepthSampler`다(셰이더 시스템 7.2절 9항). 공유 자원 구조체는 렌더러가 정한 표준 멤버 이름(`SceneTextures`·`SceneResources`·`ForwardLight`·`Shadow`·`Fog`)으로만 포함해 모든 셰이더에서 같은 자원이 같은 이름이다. 접두어는 UE 중첩 구조체의 `멤버_` 규칙과 같다(`RC/Private/ShaderParameterStruct.cpp:123–130`). UE의 펼쳐 넣기(`SHADER_PARAMETER_STRUCT_INCLUDE`)는 접두어 없이 평탄화하지만(`:100–107`), Rocks는 한 pass에 둘을 포함해도 이름이 겹치지 않게 접두어를 쓴다. 그래도 겹치면 생성기 오류다.
  - **의존은 쓰는 멤버만.** 바로 위 미사용 멤버 거르기가 포함 멤버에도 적용된다. `SceneTextureResources`를 포함한 pass가 깊이만 읽으면 깊이에만 의존한다. 깊이를 attachment로 쓰는 pass가 같은 구조체를 포함해도 셰이더가 깊이를 읽지 않으면 상충이 아니고, 읽으면 렌더 그래프가 한 pass 안 상충 접근으로 거부한다(7.2절, Renderer 6.2절 순환 금지).
  - **단계 상한.** 포함 멤버도 그 단계의 SRV 텍스처 수에 든다. 렌더러 예산표(Renderer 7.4절)는 셰이더가 실제로 읽는 멤버로 센다.
  - **영구 자원.** 그래프 밖 자원(스트리밍 텍스처 등)은 import한 그래프 자원으로 넣는다(Renderer 15.1절). 구조체 값의 수명은 그래프 수명이다.
  - **view마다.** view가 여럿이면 view마다 다른 값을 담은 pass 구조체가 따로 생긴다(7.5절).
- **그리기별 자원 구조체 `DrawResourceStruct`(개정 16).** 그리기마다 바뀌는 자원(라이트맵·프로브 텍스처 등 `PrecomputedLighting` 자원)을 담는 구조체다. 멤버 종류는 공유 자원 구조체와 같되 그래프 자원은 import 자원만 가리킨다. pass 구조체에 넣지 않고 그리기 항목의 바인딩(9.3절)으로 그리기마다 묶는다. 그래프 의존은 pass가 그 그리기들이 쓸 수 있는 import 자원 집합을 접근만 선언으로 한 번 선언한다(9.3절). HLSL 이름 규칙은 위와 같다(표준 멤버 이름 `PrecomputedLighting`).

### 9.7 파이프라인 캐시

- 파이프라인 객체는 셰이더(VS·PS 또는 CS), 정점 배치, 래스터·깊이·블렌드 상태, 렌더 타깃 형식·샘플 수, 기본도형 종류를 담는다. 키는 그 내용과 셰이더 집합 epoch이며 계획 14.1절 PSO 캐시 키 항목(셰이더 hash·root signature·형식·샘플 수)을 포함한다. 키 hash는 Core `Core/Hash.h`, 디스크에 남기는 키는 SHA-256을 쓴다(공용 기능 중복 금지). root signature 캐시(셰이더 시스템 7.2절 4항)와 같은 계층에 둔다.
- 항목은 셰이더 집합의 lease를 잡는다. 집합이 `Retire`되면 그 집합 epoch의 항목도 함께 `Retire`한다.
- UE는 이 캐시를 RHI 모듈에 둔다(`RHI/Public/PipelineStateCache.h`). Rocks는 RenderBase에 둔다: Graphics는 만들기만 하고, 캐시·수명·통계는 렌더 쪽 일이다.
- `PipelineDesc`의 원문 경로는 SH2에서 없어진다(셰이더 시스템 결정 9).
- compute 파이프라인(CS 항목·그리기별 상수 크기·slot 이름 표)도 같은 캐시가 받는다(개정 36, 4.4절 "GX3·GX4 compute 연결 설계" (1)).
- 후속: 비동기 미리 준비(`PipelinePrewarm`, 정점 입력 타입의 정적 배치를 쓴다, 10.2절), 사용 기록 파일과 시작 precompile, 드라이버 캐시 연계.

### 9.8 셰이더 교체

셰이더 시스템 SH4(감시 재컴파일·PSO 교체)와 계획 14.1절 "hot reload 실패 시 이전 검증 버전 유지"를 받는 규칙이다.
- 새 셰이더는 새 셰이더 집합(새 epoch)으로 연다. 완결성·서명 검사를 통과해야 교체 후보가 된다.
- 교체는 프레임 경계의 lane 작업이다. 이전 집합은 `Retire`되고, 진행 중 프레임은 이전 집합 lease로 끝까지 그린다. 파이프라인 캐시 항목은 epoch 키 때문에 섞이지 않는다.
- 검사가 실패하면 이전 집합을 유지하고 진단을 낸다.
- **구현 설계(개정 40, 18.3절 RR8).** RenderBase `ShaderSetSlot`(`RenderBase/ShaderSetSlot.h`)이 한 종류(엔진·묶음)의 현재 집합과 교체 후보를 가진다. UE는 셰이더 맵을 새로 만들어 끼우고 옛 맵은 참조가 사라질 때 지운다(재컴파일 뒤 `FShaderMapBase` 교체). Rocks는 그 시점을 lease와 프레임 경계로 명시한다.
  - `Install(set)`: 첫 집합. `Ready`가 아니면 거부(`DependencyFailed`)하고 비어 있다.
  - `Propose(set)`: 후보를 받는다(어느 스레드에서나 — SH4 감시 재컴파일이 부른다). 이미 후보가 있으면 옛 후보를 버린다(마지막 것이 이긴다). `Failed`면 바로 거부하고 현재를 유지한다(거부 수·영어 진단).
  - `ApplyPending(cache)`: 프레임 경계의 렌더 lane 작업에서 부른다(프레임 packet 작업 맨 앞). 후보가 `WaitingForProvider`면 다음 경계로 미룬다. `Ready`면 교체한다: 옛 집합의 캐시 항목을 `PipelineCache::RetireSet`, 옛 집합 `Retire`, 새 집합이 현재가 된다(epoch가 다르므로 캐시 항목이 섞이지 않는다). `Failed`면 버리고 현재를 유지한다. 돌려주는 값은 교체했는지다.
  - `CollectRetired()`: Retire한 옛 집합 가운데 lease가 0인 것을 파괴한다(진행 중 프레임의 기록 packet·그래프가 잡은 lease가 풀린 뒤). 종료 4단계(5.5절)는 `Shutdown`이 현재 집합까지 Retire하고 lease가 남으면 `Busy`다(closure 실패 — 강제 해제하지 않는다).
  - 통계: 교체 수·거부 수·대기 중인 옛 집합 수.
  - **구현(2026-11-25, 개정 44).** `RenderBase/ShaderSetSlot.h`(`ShaderSetSlot` — `Install`·`Propose`·`ApplyPending`·`CollectRetired`·`Shutdown`, 통계 `Swaps`·`Rejections`·`RetiredCount`·`LastRejection`). 집합은 `UniquePtr<ShaderSet>`으로 넘긴다. 스펙과 다르게 정한 것: 공급 대기 후보를 버릴 때(새 후보가 오거나 종료) 그 공급이 끝날 때까지 기다린다(`WaitUntilSettled` — 공급자가 파괴된 집합에 `CompleteProvision`을 부르지 않게). 시험 `ShaderSetSlot_SwapsAtFrameBoundaryAndKeepsTheCurrentOnFailure`(첫 집합 null·실패·중복 거부, 실패 후보의 거부와 현재 유지, lease를 잡은 채 교체 — 옛 집합의 캐시 항목 0·Retired·새 lease 거부·회수 대기, 새 epoch 캐시 미적중, lease가 풀린 뒤 파괴, 종료의 Busy와 성공, 마지막 후보 우선)·`ShaderSetSlot_DefersWaitingCandidatesAndRejectsFailedProvision`(공급 대기 후보는 미루고 공급 완료 뒤 교체, 공급 실패 후보는 적용 때 버리고 현재 유지). 감시 재컴파일(SH4)이 `Propose`를 부르는 연결은 SH4에서 한다.

### 9.9 셰이더 시스템 문서 개정(SX)

| ID | 셰이더 시스템 위치 | 개정 | 근거 |
|---|---|---|---|
| SX1 | 7.2절 1항(이름 없는 전역 uniform 금지) | 파라미터 구조체 하나 = 이름 있는 cbuffer 하나라는 규칙, 그 cbuffer와 루트 상수의 관계, 구조체 안 자원 멤버의 HLSL 생성 규칙(전역 `Texture2D X; SamplerState XSampler;`는 `$Globals`가 아니므로 1항 위반이 아님)을 적는다 | UE는 파라미터 구조체 상수를 느슨한 전역으로 쓰고 이름으로 매칭한다(`ShaderParameterStruct.cpp:251–330`). Rocks는 생성 cbuffer라 모순은 없으나 규칙이 비어 있다 |
| SX2 | 7.2절 2항(루트 상수) | 파라미터 구조체 상수가 루트 상수로 가는지 cbuffer로 가는지 정하는 규칙(결정 R7). Graphics `SetConstants(DrawConstants)`를 파이프라인 layout이 아는 크기의 바이트로 일반화. **구현(개정 21)**: R7 확정(9.4절), 셰이더 시스템 7.2절 2항 개정, Graphics `drawConstantBytes`·`SetDrawConstants`(4.4절 GX4) | UE의 셰이더별 상수 cbuffer(`ShaderParameterMetadata.h:173–176`)와 루트 상수(4 DWORD)는 다르다 |
| SX3 | 13절(P7) | 9.6절 멤버 종류 표와 규칙(그래프 자원·샘플러 지정·접근 선언·렌더 타깃 slot·상수 블록 참조·펼쳐 넣기·샘플러 짝 자동 생성). `ValueKind` 추가 항목이 바뀐다. 셰이더 시스템 결정 4와 같은 기한(P14 전) | 정하지 않으면 레이아웃 서명 정의와 `ValueKind`를 다시 고쳐야 한다 |
| SX4 | 3절(셰이더 맵 키), 12.2절(셰이더 타입·키) | 정점 입력 타입 축(10.2절): 런타임 조회 키 (셰이더 타입, 정점 입력 타입, 퍼뮤테이션 ID, 단계), 셰이더 맵 키에도 정점 입력 타입(고정 가상 경로의 include 해석이 달라지므로), 거르기는 셰이더 타입 조건 AND 정점 입력 타입 조건 | UE `RC/Public/Shader.h:644–671`, `Engine/Private/Materials/MeshMaterialShader.cpp:178–213`. **반영(셰이더 시스템 개정 16, 12.2절)**: 조회 키·라이브러리 항목 키에 정점 입력 타입, 셰이더 타입 거르기 AND 정점 입력 타입 거르기 |
| SX5 | 12.2절(라이브러리 형식) | 항목 표에 묶음 id 열(엔진 셰이더 = 0, 머티리얼은 셰이더 집합 id). 라이브러리 형식 버전 1을 확정하기 전에 넣는다 | UE 라이브러리는 셰이더 맵 hash → 셰이더 인덱스 범위로 묶는다(`ShaderCodeArchive.h:48–51, 221–230`) |
| SX6 | 12.2절 | 항목마다 파라미터 레이아웃 서명. 열 때 검사(9.3절) | — |
| SX7 | 12.2·12.3절 | 엔진 셰이더 집합 완결성 검사(쿠킹 라이브러리 형식은 시작 때, 필요 시 컴파일 형식은 공급 완료 뒤)를 완료 기준으로(9.2절) | — |
| SX8 | 7.2절 3항(`ShaderBindingLayout`) | 상수 버퍼 항목에 파라미터 구조체 identity, 자원 항목에 파라미터 구조체 멤버 오프셋 | 9.5·9.6절 |
| SX9 | 7.2절 7항(정점 입력) | `ATTRIBUTEn` 상한 n ≤ 15(16개), 정점 배치 ⊇ 셰이더 입력 마스크 검사, 형식 기본 타입과 셰이더 입력 타입 일치 검사(정규화 → float, 정수 → int·uint, GLES 정수 attribute는 `glVertexAttribIPointer`) | 10.2절. 16은 Vulkan `maxVertexInputAttributes` 최소 보장(Vulkan 사양 Required Limits)과 OpenGL ES 3.x `GL_MAX_VERTEX_ATTRIBS` 최소값(glGet 참조 "must be at least 16")이다. UE는 선언당 17개(`RHI/Public/RHIDefinitions.h:269`) |
| SX10 | 16절 SH2 | SH2 완료 기준에 지연 생성 경쟁 시험(게시 1회, 명시 gate), 완결성 음성 대조, 서명 불일치 음성 대조, 파이프라인 캐시 적중 계수를 더한다. SH2 범위는 결정 R8 | — |
| SX11 | 6절 4항(구현 순서) | 셰이더 런타임 객체(RB3)를 SH2 안에 넣는다(결정 R8 권장안) | — |
| SX12 | 2절(SM6 기능) | specialization constant 차원은 후속으로 적는다 | UE `ShaderPermutation.h:636, 651` |
| SX13 | 7.2절 9항(텍스처·샘플러 짝) | 짝 상대로 `SamplerComparisonState XSampler`를 허용한다. 샘플러 값은 파라미터 멤버 지정 또는 텍스처 desc 기본값(결정 R12). **A0-G 착수 전**에 AssetSystem A0-G 샘플러 확장에 비교 함수를 넣는다(2026-11-18 A0-G에서 `SamplerDesc::comparison`·`compare`로 구현, 네 backend) | 계획 11.1절 그림자 필수, `GraphicsTypes/Public/GraphicsTypes/Sampler.h:42–52`에 비교 모드 없음 |
| SX14 | 12.3절(`GLES31_PC`·필요 시 컴파일), 16절 SH1 | 셰이더 공급자 interface(9.2절)를 SH1 산출물로. Development 실행 파일만 등록, Release 정책 검사(9절)는 그대로 | Runtime이 Developer에 의존할 수 없다 |

### 9.10 완료 기준

- RB3(셰이더 집합): 첫 사용 생성 경쟁(두 스레드가 gate로 동시에 요청해도 게시 1회, 진 쪽 `Retire`), 완결성 검사 음성 대조(타입 하나를 뺀 라이브러리로 시작 실패), 서명 불일치 음성 대조, 공급자 경로(공급 대기 → 완료 → 검사 → `Ready`, 공급자 없는 실행 파일의 `UnsupportedCapability`), 라이브러리 조회 순서와 중복 키 오류, `Retire`된 집합의 새 장치 셰이더 생성 거부.
- RB4(바인딩·상수 블록·파이프라인 캐시): 셰이더 시스템 7.3절 (a)·(d)를 런타임 경로로, 파이프라인 캐시 적중·미적중 계수, 집합 `Retire` 때 캐시 항목 회수, `PerFrame` 상수 블록의 slot 회수(fence gate), GLES 업로드 판단(program 전환 뒤 같은 블록을 다시 올리는지, 바뀌지 않은 블록은 올리지 않는지 호출 수 계측, 7.3절 (i)), null·종류 불일치 검증의 음성 대조, 교체(9.8절) 성공·실패.

### 9.11 상수 버퍼 묶기(GX6, 개정 38)

9.3–9.4절의 상수 블록을 장치에 올려 셰이더의 상수 버퍼(cbuffer) slot에 묶는 경로다. 그리기별 상수(루트 상수, 9.4절)는 그대로 두고, 이름 있는 cbuffer(셰이더 시스템 7.2절 1항)를 대상으로 한다. 파라미터 묶음 `ParameterBatch`의 자원 부분과 공유 상수 slot 표(9.5절)는 이 절 밖이다(아래 9항).

**UE 5.8 방식.**

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 수명 | `UniformBuffer_SingleDraw`(그리기 한 번)·`SingleFrame`(그 프레임)·`MultiFrame`(여러 프레임) | `RHI/Public/RHIDefinitions.h:517–525` |
| D3D12 할당 | `MultiFrame`은 영구 upload heap 할당, 나머지는 빠른 상수 ring. 갱신은 새 자리를 받아 옮긴다(rename). 256 B 정렬(`D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT`). GPU는 upload heap을 그대로 읽는다 | `D3D12RHI/Private/D3D12UniformBuffer.cpp:76–90, 194–204` |
| D3D12 묶기 | 단계별 root CBV(최대 16), 넘치면 CBV table | `D3D12RHI/Private/D3D12DescriptorCache.cpp:681–750`, `D3D12RootSignature.cpp:298–308, 332` |
| Vulkan | `SingleDraw`·`SingleFrame`은 임시 블록 ring(정렬 max(`minUniformBufferOffsetAlignment`, 16)), `MultiFrame`은 메모리 관리자 할당. descriptor는 기본으로 모두 `UNIFORM_BUFFER_DYNAMIC`(`maxDescriptorSetUniformBuffersDynamic`까지, 나머지는 일반 UBO) | `VulkanRHI/Private/VulkanUniformBuffer.cpp:39–48`, `VulkanDescriptorSets.cpp:7–14, 24–25, 60–65` |
| D3D11 | 크기별 pool의 `DYNAMIC` 버퍼, 만들 때·갱신 때 `MAP_WRITE_DISCARD` | `Windows/D3D11RHI/Private/D3D11UniformBuffer.cpp:141–161, 234–289` |
| OpenGL | 늘 에뮬레이션: program마다 마지막으로 올린 UB 고유 ID를 기억하고 다를 때만 복사 표대로 `glUniform4*v` | `OpenGLDrv/Private/OpenGLShaders.cpp:1298, 2058, 3335–3351` |

**결정(권장안 확정).**
1. **수명.** Graphics에 `ConstantBufferLifetime`(`SingleFrame`·`MultiFrame`)과 handle `ConstantBufferHandle`을 두고, UE `SingleDraw`는 handle 없는 기록 명령(3항)으로 둔다. 9.4절 수명과는 `PerDraw` → 기록 명령, `PerFrame` → `SingleFrame`, `Persistent` → `MultiFrame`으로 잇는다.
   - `Device::CreateConstantBuffer(lifetime, contents)`: 내용은 16 byte 배수, 16–65536 byte(`MaxConstantBufferBytes` = 셰이더 시스템 `MaxShaderConstantBufferBytes`). 기록 중에도 만들 수 있다(GX8 생성 규칙과 같다).
   - `UpdateConstantBuffer(handle, contents)`: `MultiFrame`만, 같은 크기만(아니면 `InvalidArgument`). 갱신은 **기록 순서**를 따른다: 그 앞에 기록한 그리기는 옛 내용, 뒤의 그리기는 새 내용을 읽는다(UE rename과 같다). `SingleFrame`은 만든 뒤 바꾸지 않는다(바꿀 값은 새 버퍼).
   - `Retire(ConstantBufferHandle)`: 다른 자원과 같이 마지막 사용 fence 뒤 해제한다(GX7). `SingleFrame`의 "그 프레임"은 Graphics가 모르는 단위이므로 쓰는 쪽(9.4절 `PerFrame` 블록 주인, 렌더 그래프)이 프레임 끝에 Retire한다. 프레임 slot의 GPU 완료 회수(9.10절 RB4)는 마지막 사용 fence가 맡는다.
   - 버린 대안: `SingleFrame`을 장치 기록(Begin…EndFrame) 수명으로. 렌더 그래프는 Raster pass마다 기록을 따로 열므로(7절) 한 프레임의 뷰 상수를 여러 기록이 함께 읽는다.
2. **정렬과 업로드.** 모든 backend가 상수 버퍼 자리를 `ConstantBufferAlignment` = 256 byte 단위로 잡는다(D3D12 CBV 주소 규칙, Vulkan `minUniformBufferOffsetAlignment`의 사양 최대값). D3D12·Vulkan은 CPU 가시 메모리(D3D12 upload heap, Vulkan host-visible coherent)를 GPU가 그대로 읽는다(UE와 같다, 복사 명령 없음).
   - 기록 명령용 ring: queue마다 하나(그래픽·async compute — fence 값이 queue마다 따로라서), Graphics `UploadRingAllocator`. 묶음은 그 기록의 제출 fence로 닫고 완료 뒤 회수한다(GX8 ring과 같은 규칙). 자리가 없으면 끝난 제출을 회수하고, 그래도 없으면 가장 오래된 닫힌 묶음을 기다린다. 지금 기록 하나가 ring을 다 쓰면 그 명령은 `CapacityExceeded`이고 slot을 비운다(뒤 그리기는 무시된다).
   - handle용 page: CPU 가시 버퍼 page(1 MiB)를 Graphics `GpuPageAllocator`로 나눈다. `UpdateConstantBuffer`는 새 구간을 받아 내용을 쓰고 옛 구간을 마지막 사용 fence 뒤에 돌려준다.
   - D3D11: handle마다 `DYNAMIC` 버퍼(만들 때 초기 내용, 갱신은 `MAP_WRITE_DISCARD` — 드라이버 rename이 기록 순서를 지킨다), 기록 명령은 (단계, slot)마다 `DYNAMIC` 버퍼 하나를 `WRITE_DISCARD`로 갈아 쓴다(UE 크기별 pool은 측정 뒤). OpenGL ES: 모두 CPU 사본이다(9.4절 표현).
3. **묶기.** `CommandContext::SetConstantBuffer(stage, slot, handle)`과 `SetConstantData(stage, slot, bytes)`(기록 명령 — bytes를 그 기록의 ring에 복사해 묶는다, UE `SingleDraw`). `slot`은 그 단계 바인딩 layout(값 1)의 상수 버퍼 slot(셰이더 시스템 16.1절 SSD30 — backend 위치)이고 0–13(`MaxConstantBufferSlots`, 형식 정의 최대인 D3D12 14)이다. 위치가 형식마다 다르므로 쓰는 쪽은 값 1에서 얻는다: 렌더 쪽은 바인딩 지도(9.3절 `ShaderBindingMap`의 단계별 자리)이고, 이름으로 찾으려면 `FindConstantBufferSlot(layout, name)`이다.
   - 묶음은 단계마다 따로이고 그 기록 안에서 `SetPipeline`을 넘어 유지된다(D3D11·UE와 같다). 기록 시작 때 모두 빈다. null handle은 그 slot을 비운다.
   - handle 묶음은 그리기 때 그 handle의 현재 자리로 푼다(그래서 갱신 뒤의 그리기가 새 내용을 본다). Retire된 handle은 빈 slot이다.
   - 그리기·dispatch 조건: 파이프라인이 읽는 상수 버퍼(단계, slot)마다 셰이더 선언 크기(값 1 `constantBufferBytes`) 이상인 버퍼가 묶여야 한다. 아니면 무시하고 `IgnoredDraws`를 올린다(버퍼 view slot과 같은 규칙, `ConstantBuffersSatisfied`).
   - 파이프라인 생성: 값 1의 상수 버퍼를 모두 받는다(이름 표에 넣지 않는다). slot이 범위 밖이면 `InvalidArgument`다.
   - 버린 대안: 이름 표(`PipelineBindingNames::constantBuffers[n]`)로 고정 번호를 주기. 이름 표는 묶음 설정 전의 개발 경로이고, 바인딩 지도가 이미 단계별 위치를 내므로 표를 늘리지 않는다(UE도 셰이더 파라미터 지도의 index로 묶는다).
4. **backend 매핑.**

   | backend | 기록 명령 | handle | 셰이더 쪽 묶기 |
   |---|---|---|---|
   | D3D12 | queue별 upload ring | upload page 구간 | root CBV(GPU 주소), 예산을 넘는 slot은 그 단계 CBV table(5항) |
   | D3D11 | (단계, slot)별 `DYNAMIC` 버퍼 `WRITE_DISCARD` | handle별 `DYNAMIC` 버퍼 | `VS·PS·CSSetConstantBuffers`(b0–b12, b13은 루트 상수) |
   | Vulkan | queue별 host-visible ring | host-visible page 구간 | set = 단계의 binding = slot. `UNIFORM_BUFFER_DYNAMIC`(descriptor는 (버퍼, 0, 선언 크기), 동적 offset = 구간 위치)를 `maxDescriptorSetUniformBuffersDynamic`까지, 넘는 것은 일반 UBO. 버퍼(VkBuffer)가 같으면 set을 다시 쓰지 않고 offset만 바꿔 다시 묶는다 |
   | OpenGL ES | (단계, slot)별 CPU 사본 | CPU 사본 | 값 2 복사 표의 `<단계>c<slot>_<타입>` 배열에 `glUniform4fv/4iv/4uiv`. (program, 단계, slot)마다 마지막으로 올린 (identity, version)을 기억하고 다를 때만 올린다(9.4절 판단, UE와 같다) |

5. **D3D12 root signature 배치와 예산(셰이더 시스템 7.2절 4항 개정 47).** 파이프라인마다 VS·PS(또는 CS) slot 표로 만들고 캐시한다.
   - 순서: 단계마다 [SRV table][Sampler table][UAV table(CS만)][CBV table(넘친 것)] → root CBV((slot, 단계) 오름차순, 같은 slot은 VS·PS 순) → PS 진단 root UAV(6항) → 루트 상수(그 파이프라인 `drawConstantBytes` ÷ 4 DWORD, 0이면 없음, visibility ALL).
   - 단계의 SRV table은 그 단계가 읽는 SRV(텍스처·버퍼 view) 전부를 register마다 range 하나로 담고, 그리기 때 CPU 전용 heap의 descriptor를 shader-visible ring으로 복사해 연속 구간을 만든다(UE offline → online heap 복사와 같다). 바로 앞 그리기와 원본 목록이 같으면 복사하지 않고 그 구간을 다시 쓴다. UAV table도 같다. Sampler table은 개발 경로에서 단계당 하나라 영구 descriptor를 가리킨다. 지금까지의 "자원마다 table 하나"(셰이더 시스템 16.1절 SH2e2 메모)는 table 수가 자원 수만큼 늘어 예산을 넘기므로 버린다.
   - 예산식 `루트 상수 DWORD + table 수 + 2 × root CBV 수 + 2 × 진단 root UAV ≤ 64`. 모든 CBV가 root로 들어가면 CBV table이 없다. 아니면 넘치는 CBV가 생기는 단계마다 CBV table 하나(1 DWORD)를 더 세고, 식을 만족하는 가장 큰 root CBV 수 R을 고른다. root는 (slot, 단계) 순 앞 R개, 나머지는 각 단계의 높은 slot이며 그 단계 CBV table의 range [가장 낮은 넘친 slot, 가장 높은 slot]에 들어간다(사이 빈 slot은 null CBV). CBV table의 descriptor는 그리기마다 ring에 `CreateConstantBufferView`로 만든다(크기는 256 byte 올림).
   - 보장값(루트 상수 32 DWORD, 단계 table 최대 — VS·PS는 SRV·Sampler·CBV 셋, UAV table은 CS만이라 CS는 넷): 그래픽 64 − 32 − 6 = 26 → root CBV 13, 진단 퍼뮤테이션 12, CS 64 − 32 − 4 = 28 → 14. 어느 경우도 항상 만들 수 있다. 셰이더 시스템 개정 32의 12·11은 VS·PS에도 UAV table을 세어 table 8로 계산한 값이라 고친다.
6. **PS 진단 버퍼 자리(셰이더 시스템 7.2절 12항).** D3D12는 PS 값 1에 `RocksDebugBuffer`가 있으면 root UAV(u0 space999, visibility ALL)를 두고, 장치가 0으로 채운 내부 버퍼(`PixelDebugBufferBytes` = 4 KiB)를 묶는다. 버퍼를 읽고 비우는 일·크기 결정은 엔진 셰이더 SL2다. Vulkan(set 3)·Metal의 진단 자리는 SL2에서 만들고, 그때까지 그 backend의 진단 파이프라인 생성은 `Unsupported`다(D3D11·GLES 형식은 진단 퍼뮤테이션이 없다).
7. **RenderBase.** `ConstantBlock`이 장치 버퍼를 갖는다: `Commit(device)`는 `PerFrame`이면 `SingleFrame` 버퍼를 만들고(이미 있고 값이 바뀌었으면 새로 만들고 옛것을 Retire), `Persistent`면 `MultiFrame`을 만들거나 바뀐 version만 `Update`한다. `PerDraw`는 장치 버퍼가 없다. `Release(device)`가 Retire한다. `ApplyConstantBlock(commands, map, block)`은 지도의 상수 버퍼 자리마다 `PerDraw`면 `SetConstantData`, 아니면 commit된 버퍼를 `SetConstantBuffer`로 묶는다(commit 전이거나 값이 commit보다 새로우면 `InvalidArgument`). 루트 상수 표는 지금처럼 `ApplyDrawConstants`다.
8. **RenderGraph.** `RenderGraph::CreateConstants(bytes, name)`(또는 `ConstantBlock`)이 그래프 상수 `GraphConstantBuffer`를 선언한다. 내용은 프레임 arena에 복사하고, 장치 `SingleFrame` 버퍼는 실행 시작(Immediate는 선언 때)에 소유 스레드가 만들어 그래프 끝에 Retire한다 — 병렬 기록(RG3) pass 함수는 장치 생성을 하지 않기 때문이다. 상수 버퍼는 CPU 가시 메모리이고 바뀌지 않으므로 접근 선언·전이가 없다(어느 pass나 읽는다). pass는 `GraphPassContext::Constants(c)`로 handle을 받거나 `SetConstants(map, c)`로 지도의 자리마다 묶는다. pass 함수 안의 `SetConstantData`도 쓸 수 있다(기록 목록이 byte를 arena에 복사해 재생한다).
9. **이 절 밖.** 공유 상수 slot 표(9.5절 — 바인딩 layout의 구조체 identity SX8과 렌더러 slot 등록이 먼저다), `ParameterBatch`의 자원 묶음, D3D11 크기별 pool·ring 크기 조정은 측정 뒤다.

**완료 기준(9.10절 RB4의 GX6분).** EngineTests: 예산 계획(보장값 13·12·14, 경계 하나 위에서 table로 내림, 64 DWORD 이하, 결정적 배치), 값 1 상수 버퍼 수용·slot 범위·`ConstantBuffersSatisfied`, 기록 장치의 묶기·commit·갱신 순서, 그래프 상수의 생성·Retire·병렬 기록 재생. Sandbox `--self-test`(D3D12·D3D11·Vulkan·OpenGL ES, 각 소프트웨어 장치 포함): handle 두 수명과 기록 명령으로 그린 픽셀, 기록 순서 갱신(그리기 1은 옛 값·그리기 2는 새 값), 파이프라인 전환 뒤 묶음 유지, 안 묶은·작은 버퍼의 무시 계수, 상수 버퍼를 읽는 compute 결과, 단계마다 상수 버퍼 12개(D3D12는 14개와 진단 퍼뮤테이션)를 쓰는 그리기의 픽셀 — 장치 사이 결과가 같아야 한다.

**구현(2026-11-25, 개정 39).** 위 결정 그대로다.
- 파일: Graphics `Graphics/ConstantBuffer.h`(수명·handle·`ConstantBufferAlignment`·`MaxConstantBufferSlots`·`ConstantBufferSlots`·`ConstantBuffersSatisfied`·`ConstantBufferBytes`), `Graphics/RootBudget.h`(`PlanRootBudget` — 예산 계획을 API 객체 없이 계산해 CPU 시험으로 확인한다), `Graphics/ShaderBindings.h`(`PipelineSlots::constantSlots`·`debugBuffer`, `FindConstantBufferSlot`), `Device.h`(`CreateConstantBuffer`·`UpdateConstantBuffer`·`Retire`, `SetConstantBuffer`·`SetConstantData`), `DeviceFeatures::pixelDebugBuffer`. backend는 `D3D12Constants.cpp`·`D3D11Constants.cpp`·`VulkanConstants.cpp`·`GlesDeviceConstants.cpp`, D3D12 root 배치는 `D3D12Pipeline.cpp`. RenderBase `ConstantBlock::Commit`·`Release`·`IsCommitted`, `ApplyConstantBlock`·`ApplyConstantBuffer`·`ShaderStageOfBit`. RenderGraph `GraphConstantBuffer`, `RenderGraph::CreateConstants`(바이트·`ConstantBlock` 두 판), `GraphPassContext::Constants`·`SetConstants`, 기록 목록의 `SetConstantBuffer`·`SetConstantData` 재생.
- D3D12: SRV·UAV 원본 descriptor heap(`srvs_`)을 CPU 전용으로 바꾸고, shader-visible heap은 queue마다 131072칸인 ring(`OnlineDescriptorsPerQueue`, `UploadRingAllocator`)이다. SRV·UAV table 복사와 넘친 CBV table은 단계마다 바로 앞 원본 목록(CBV는 주소·크기 목록)과 같으면 그 구간을 다시 쓰고, 기록마다 비운다. `ClearWritableBufferView`의 GPU handle도 ring 사본이다. 넘친 CBV table의 빈 slot은 장치의 0 구간(256 byte)을 가리킨다.
- 스펙과 다르게 정한 것:
  1. D3D11 handle은 `DYNAMIC`이 아니라 SingleFrame `IMMUTABLE`·MultiFrame `DEFAULT`다. 기록은 deferred context이고 deferred context의 `DYNAMIC` 내용은 그 기록에서 `WRITE_DISCARD`로 쓰기 전까지 정해지지 않기 때문이다. 기록 중 소유 스레드의 갱신은 기록 context의 `UpdateSubresource`, 다른 스레드의 기록 중 갱신은 새 `DEFAULT` 버퍼로 옮기고(옛것은 그 기록 fence 뒤 회수), 기록 밖 갱신은 업로드 기록이다 — 어느 경우도 기록 순서를 지킨다. 기록 명령은 결정대로 (단계, slot)별 `DYNAMIC` 버퍼다.
  2. D3D12 Sampler table은 복사하지 않고 영구 descriptor를 가리킨다(개발 경로는 단계당 짝 샘플러 하나).
  3. 장치 능력 `DeviceFeatures::pixelDebugBuffer`를 더했다(D3D12만 참). 셰이더 집합이 `debugPixelUav` 런타임 확인 기능을 이것과 대조한다(9.2절 — 전에는 늘 거부).
  4. Sandbox Development 공급자가 개발 전용 타입도 만든다(`includeDevelopment`) — 진단 퍼뮤테이션 그리기 시험이 쓴다.
- 시험 결과: EngineTests `ConstantBuffer_SizeRulesAndSlotBookkeeping`·`ConstantBuffer_PipelineSlotsTakeConstantBuffersByLocation`·`ConstantBuffer_RootBudgetGuaranteesAndOverflow`(보장값 13·12·14, 경계 하나 위에서 넘친 단계 table, 루트 상수 없으면 28개 모두 root, 결정적 배치, 음성 입력)·`ConstantBlock_*` 2건·`RenderGraph_Constants*` 2건·`RenderGraph_ParallelRecordingReplaysConstantCommands` 통과, 기존 `ShaderBindings`·`ShaderSet`·`PipelineCache`·`ComputeDispatch`·`RenderGraph`·`RenderLane`·`GpuTrace` 묶음도 통과(`ShaderBindings`의 상수 버퍼 거부 대조는 수용 대조로 바꿨다). Sandbox `--self-test`의 `RunConstantChecks`(Programs/Sandbox/Private/ConstantSelfTest.cpp, 셰이더 Constant.rsf·ConstantMany.rsf·ConstantWide.rsf): D3D12(GTX 1060)·D3D12 WARP·D3D11(GTX 1060)·D3D11 WARP·OpenGL ES(ES 3.2 context)·GL 4.3 core·Vulkan(GTX 1060)·lavapipe 모두 0 failures — 기록 순서 갱신(왼쪽 옛 값·오른쪽 새 값), SetConstantData, 파이프라인 전환 뒤 묶음 유지, 안 묶은·작은(32 byte 선언에 16 byte) 버퍼 그리기 무시 2건, 정수 상수 버퍼 compute(handle·기록 명령), 단계마다 12개 그리기의 slot별 픽셀 16개. D3D12·WARP는 단계마다 14개 그리기와 그 진단 퍼뮤테이션(root UAV)도 같은 픽셀이고, 둘 다 루트 상수 32 DWORD를 선언해 넘친 CBV table 경로를 지난다(28개 중 root 15·진단 14). 그 밖 장치에서 14개는 형식 정의 상한 때문에 쿠킹하지 않는다(NOT RUN). 검증 계층 오류 0, 장치 사이 기존 픽셀 대조도 그대로 0 differing channels다.
- 리뷰 반영(같은 날): D3D11 다른 스레드 갱신 rename, GLES 생성·갱신의 소유 스레드 검사, D3D12 ring 크기(32768 → 131072)와 넘친 CBV table 재사용, Vulkan pool의 UBO 칸(set당 `MaxConstantBufferSlots`), `SetConstantData` 실패 계약(InvalidArgument만 앞 묶음 유지, 그 밖 실패는 slot을 비운다). 반영 뒤 self-test 여덟 장치 다시 0 failures.
- 남은 것: 9항(공유 상수 slot 표·`ParameterBatch` 자원 부분·D3D11 크기별 pool·ring 크기 조정), Vulkan·Metal 진단 버퍼 자리(SL2).

### 9.12 엔진 호스트 연결(RR11, 개정 45)

18.3절 RR11의 남은 부분이다. 9.2절의 "렌더 lane 시작 단계에서 엔진 셰이더 집합을 연다"를 엔진 호스트 공용 경로로 만든다. 지금까지는 Sandbox가 등록부·공급자·장치별 집합을 손으로 만들었다(`Programs/Sandbox/Private/SandboxShaders.cpp`). 결정은 권장안으로 확정했고 애매한 곳은 UE 5.8을 따랐다.

**UE 5.8 순서.** 모듈 적재(`Launch/Private/LaunchEngineLoop.cpp:2742` `LoadPreInitModules`, 셰이더를 가진 플러그인은 PostConfigInit 단계 — `:6815`) → 셰이더 컴파일 관리자 생성(셰이더 컴파일을 허용하는 실행만 실제로 일한다, `:3245–3260`) → 셰이더 타입 초기화(`:3295` `InitializeShaderTypes`, `RC/Private/ShaderCore.cpp:3671`) → 전역 셰이더 맵(`:3307` `CompileGlobalShaderMap`). 원문 디렉터리 대응표는 RenderCore 런타임 API이고(`RC/Public/ShaderCore.h:1654` `AddShaderSourceDirectoryMapping`) 쿠킹 데이터를 쓰거나 컴파일을 허용하지 않는 실행에서는 무시된다(`RC/Private/ShaderCore.cpp:4222`).

**구조.**
1. **엔진 셰이더 호스트 `EngineShaderHost`**(`RenderBase/EngineShaderHost.h`). 엔진 호스트(Sandbox `--engine`·self-test, 이후 게임 실행 파일)가 하나 만드는 호스트 서비스다(모듈 로더 3.12절).
   - 소유하는 것: 셰이더 타입 등록부(셰이더 시스템 SSD3), 원문 디렉터리 표(가상 루트 → 디스크 폴더, UE 대응표와 같은 자리 — 공급자만 읽는다), 공급자 자리 하나(SX14), 쿠킹 라이브러리 원천 자리 하나(아래 3항).
   - `Attach(services)`: 등록부와 자신을 서비스로 올린다. 호스트는 다른 호스트 서비스와 함께 `StartPhase(PostConfigInit)` 전에 부른다 — PostConfigInit 모듈도 셰이더 타입을 등록할 수 있다(UE 플러그인 규칙과 같다). `Detach(services)`는 호스트 묶음 종료 뒤다.
   - `Freeze()`: 엔진 묶음과 PostEngineInit 모듈이 시작한 뒤(`Engine::Initialize` 뒤), 렌더 lane 시작 전에 로더 스레드에서 부른다(UE `InitializeShaderTypes` 자리). 등록부와 원문 디렉터리 표가 닫힌다. 성공하면 Info 한 줄(셰이더 타입 수·정점 입력 타입 수), 실패하면 Error(등록부 `LastFailure`)다. 두 번째 호출은 아무 일도 하지 않는다.
   - 늦은 등록: Freeze 뒤 시작하는 모듈의 등록은 `ScopeClosed`이고 그 모듈 시작이 실패한다(RenderBase 모듈은 Error 로그). 같은 등록부로 엔진 묶음을 다시 시작해도 같다 — 등록부는 엔진 묶음 한 번의 수명에 묶인다. 그래서 등록부를 쓰는 동안 호스트는 엔진 묶음을 시작 상태로 둔다(self-test는 `StartEngineGroup` 사용자 하나를 직접 잡는다).
   - 스레드: `Attach`·`Detach`·`Freeze`·원문 디렉터리 추가·자리 연결은 로더 스레드, Freeze 뒤 조회는 어느 스레드에서나 된다(자리는 잠금으로 읽는다 — lane 스레드가 연다).
2. **엔진 렌더 자원 `EngineRenderResources`**(같은 헤더). 렌더 lane 처리기가 멤버로 두고 `OnLaneStart`에서 `Open(device, host)`, `OnLaneStop`에서 `Retire(device)`를 부른다(5.5절 시작 순서의 "엔진 셰이더 집합 열기와 검사 → 내장 자원 팩"). 장치 소유 스레드에서 직접 그리는 self-test도 같은 함수를 쓴다. `Open` 순서:
   1. 등록부가 Freeze되지 않았으면 `NotReady`(진단: Freeze 자리).
   2. 형식 id `EngineShaderFormat(backend)`: D3D12 `D3D12_SM6`, D3D11 `D3D11_SM5`, Vulkan `VULKAN_SM5`, OpenGL ES `GLES31_PC`, Metal `METAL_SM5`. TargetProfile `shaderFormats`·장치 요구 비트에 따른 선택(셰이더 시스템 SSD10·SSD15)과 Android의 `GLES31_ANDROID`는 쿠킹 원천이 들어올 때 그 원천이 맡는다(RenderBase는 플랫폼 매크로로 가르지 않는다 — 플랫폼 경계 정책, Android에는 공급자도 없다).
   3. 쿠킹 원천이 붙어 있으면 그 형식의 엔진 라이브러리를 읽어 `OpenCooked`. 원천에 그 형식이 없으면(`NotFound`) 4로 간다. 다른 오류는 실패다.
   4. 공급자로 `OpenFromProvider`. 공급자가 없으면 `UnsupportedCapability`(집합 문제 `NoProvider`) — Release 실행 파일의 필요 시 컴파일 형식(9.2절).
   5. `WaitUntilSettled`. `Ready`가 아니면 Error 로그(형식·집합 문제 이름·설명)와 함께 실패다(`NoProvider`는 `UnsupportedCapability`, 나머지는 `DependencyFailed`).
   6. `BuiltinResources::Create`(11.4절 5항). 실패하면 집합을 Retire하고 Error 로그(팩 `LastFailure` — 빠진 fallback 셰이더 이름)와 함께 그 오류다. 엔진 셰이더 타입을 등록하는 모듈이 시작하지 않았으면(등록 누락) 여기서 `NotFound`로 드러난다.
   - 반환 뒤 RenderLane이 제출 fence를 기다려 `BuiltinReady`를 게시한다(기존 5.5절). 실패하면 준비 완료가 없고 프레임 slot 확보가 그 오류다.
   - `Retire(device)`: 팩 → 집합 순(5.5절 종료 4단계). 집합 객체는 `EngineRenderResources`가 파괴될 때(또는 다음 `Open`에서) 부순다 — 파이프라인 캐시 lease는 그 전에 주인이 비운다.
3. **쿠킹 라이브러리 원천 `CookedShaderLibrarySource`**(interface만). `ReadLibrary(formatId, library, out)` — 그 형식·종류의 라이브러리 byte를 넘기거나 `NotFound`. 매니페스트 v2·`.rshl`(셰이더 시스템 SSD11·SSD13) 구현은 에셋 쪽이 push된 뒤 붙인다(호스트 `AttachCookedLibraries`). 지금은 원천이 없으므로 모든 형식이 공급자 경로다.
4. **Development 공급자 모듈 `DevelopmentShaderProvider`**(Developer). Sandbox의 공급자를 옮겼다. 모듈 수명 객체 `Startup`이 `EngineShaderHost` 서비스를 찾아 공급자를 붙이고 `Shutdown`에서 떼고 워커를 멈춘다. 서비스가 없으면 아무 일도 하지 않고, 이미 다른 공급자가 붙어 있으면(호스트가 직접 붙인 공급자·시험) 붙이지 않고 Info만 남긴다. 실행 파일은 Release 밖에서만 이 모듈을 링크한다(Build.cs) — Runtime 모듈은 이 모듈을 모르고(SX14) 실행 파일 소스에도 `#if`가 없다.
   - 첫 요청에서 준비한다: `/Engine` = `<engine>/Shaders`와 호스트 원문 디렉터리 mount, 형식 정의 `<engine>/Config/ShaderFormats`, 워커(실행 파일 옆, 없으면 같은 출력 뿌리의 ShaderCompileWorker 타깃 출력 — 출력 폴더 이름 `<도구>-<플랫폼>-<타깃>-<구성>`의 타깃 자리를 `shadercompileworker`로, 빌드 타깃 3.11절), 너겟 캐시, 관리자.
   - 형식마다 `CompileShaderLibrary`(개발 전용 타입 포함 — 셰이더 시스템 SSD16)를 한 번 하고 byte를 들고 있다가 같은 형식의 다음 집합에 복사한다. 요청은 잠금으로 직렬화한다(장치 소유 스레드와 lane 스레드가 모두 부른다).
5. **Sandbox.** `--engine` 호스트는 다른 서비스와 함께 `EngineShaderHost`를 올리고 Sandbox 셰이더 타입과 `/Plugin/Sandbox` 원문 디렉터리를 등록한 뒤 `Engine::Initialize` 다음에 Freeze한다(장치가 아직 없으므로 여기까지 — 창 렌더 경로는 화면 마일스톤에서 lane과 잇는다). self-test는 같은 호스트를 엔진 묶음 사용자 하나와 함께 잡고, 장치마다 `EngineRenderResources`를 열며(직접 그리기 검사의 활성 집합·팩), 렌더 lane self-test 처리기는 `OnLaneStart`에서 연다.

**버린 안.**
- Engine 모듈이 셰이더 집합을 연다: Engine은 Graphics를 모른다(엔진 런타임 3절).
- 첫 `Open`에서 Freeze: `Open`은 lane 스레드이고 등록부는 로더 스레드 규칙이다. 늦게 시작한 모듈이 조용히 빠질 수 있다.
- 공급자를 실행 파일 소스의 `#if`로 만든다(이전 Sandbox): 실행 파일마다 사본이 생긴다. 모듈 링크로 개발 구성을 고르면 같은 코드 하나다.
- 공급자를 ShaderCompiler 모듈 안에 둔다: 쿠커·워커 도구까지 RenderBase·Graphics에 의존하게 된다.

**한계.** 형식은 backend 고정 표다(Android GLES는 쿠킹 원천 뒤). 프로젝트 라이브러리(RR9)와 셰이더 교체 연결(`ShaderSetSlot`, SH4 감시 재컴파일)은 그대로 후속이다. 장치 재생성(RR18)은 새 `EngineRenderResources`를 연다.

**완료 기준.** EngineTests(기록 장치): `Engine::Initialize`로 엔진 묶음이 시작하면 RenderBase가 호스트 등록부에 엔진 셰이더 타입을 넣고, Freeze 뒤 렌더 lane 시작이 집합 `Ready`·팩·`BuiltinReady`를 만들며 종료가 Retire 0 누수로 끝난다. 음성: 등록 누락(엔진 타입 없는 등록부) → `NotFound`와 빠진 셰이더 이름, `BuiltinReady` 없음, Freeze 전 `Open` `NotReady`, Freeze 뒤 엔진 묶음 재시작 실패(`RenderBase`), 공급자 없음 `UnsupportedCapability`, 쿠킹 원천 우선·`NotFound`면 공급자, 자리 중복 `Busy`. Sandbox `--self-test` 여덟 장치 PASS와 엔진 호스트 자식의 Freeze 로그.
