## 15. 다른 문서 개정

이 문서 확정과 함께 고친다. 위치는 절 번호와 인용 문구로 적는다. A0-G에 걸리는 행은 A0-G 착수 전에 먼저 고친다.

| 문서 | 위치 | 현재 | 개정 |
|---|---|---|---|
| AssetSystem | 1절 A0-G(샘플러 확장) | 샘플러 확장 범위 | **A0-G 착수 전**: 비교 함수(SX13), 텍스처 desc 샘플러는 기본값이고 바인딩 때 지정 가능(결정 R12) |
| 〃 | 메시 에셋 단계(정해지면) | 정점 형식 Float만 | 패킹 법선·half·정규화 색 형식(10.2절). **메시 에셋 스펙 [MeshAnimationAssets](../MeshAnimationAssets.ko.md)이 stream 집합을 정했고 이 문서 10.4절 끝 "메시 에셋 요구"로 받았다(개정 29)** |
| 〃 | A5 텍스처 플랫폼 데이터(`TexturePlatformData`) | 형식·swizzle·mip 배치 서술 | **A5 착수 전**: 6.5절 입력 계약(mip-major 배치, 상주 꼬리·스트리밍 덩어리, 4 KiB 정렬, 고정 번호표 직렬화). 에셋 세션이 반영한다 |
| ShaderSystem | 9.9절 SX1–SX14 | — | 표대로. SX3·SX4·SX5·SX6은 P14 전, SX13은 A0-G 전, SX14는 SH1 전 |
| 〃 | 3절 구조 | ShaderCore에 "라이브러리 로드" | ShaderCore는 장치 없는 메타데이터·라이브러리 형식 읽기, 장치 셰이더 객체는 RenderBase |
| 〃 | 12.3절 | `GLES31_PC`·개발 트리는 필요 시 컴파일 | 셰이더 공급자 interface 경로(SX14) |
| EngineImplementationPlan | 3.1절 모듈 표 "RHI", "RenderGraph / Renderer" 행, 3.2절 트리 | RHI/{Null,D3D11,…}, RenderGraph, Renderer | Graphics(기존 이름)와 backend 모듈, RenderBase 행 추가(의존: Graphics·ShaderCore·MetaSchema·Task·Core), RenderGraph는 RenderBase 위 |
| 〃 | 3.2절 Shutdown 순서 | GPU fence 완료 → … → closure 확인 → scheduler 종료 | 렌더 lane은 closure 확인까지 살아 있고 그 뒤 Drain·소유권 반납·Join(5.5절) |
| 〃 | 8.1절 `TaskDesc.lane`·Lane 표 Render 행 | `TaskLane`을 가진 Task 하나의 모델 | 전용 스레드 lane(`DedicatedLane`)과 Worker lane(`WorkerScheduler`)의 관계, lane 작업 네 종류, 전달 가능 타입 등록 |
| 〃 | 8.2절 | in-flight slot | 게임 대기 지점을 프레임 slot 확보로 한정하고 메시지 펌프 대기, 표면 소실 예외(5.3절) |
| 〃 | 9.6절 초기화 | manifest → DeviceCaps → … → BuiltinReady | 렌더 lane 시작 단계에서 엔진 셰이더 집합 검사 → 팩 → fence → BuiltinReady(5.5절) |
| 〃 | 11.2절 RenderDevice·`Retire(ResourceHandle, lastUse)` | 별도 인터페이스 이름, 호출자가 lastUse를 준다 | Graphics `Device`에 Submit·Retire·GpuFencePoint를 들임(GX7). 게임 lane의 `Retire`는 handle만 받는 lane 작업이고 last-use는 내부 집계(6.2절). OpenGL ES도 fence sync |
| 〃 | 11.3절 | "DAG 검증", copy queue → compute overlap → alias | 선언 순서 실행·의존 정합성 검증(결정 R5), 풀 단계, render pass 병합(tiled), copy queue 위치(결정 R6) |
| 〃 | 11.5절 OpenGL 행 | core 4.5급 | 셰이더 시스템 14절 개정과 같이 OpenGL ES. 그래프 lowering 대상도 같다 |
| EngineRuntime | 6절 RunFrame ⑥ "FrameEnd(렌더 추출·제출)" | 게임 스레드 동기 제출 | 프레임 packet 게시와 slot 확보(5.2·5.3절) |
| 〃 | 10.1절 계약 | 생성 스레드 전용 | 장치 스레드 모델(5.4절) |
| 〃 | 11절 "GPU 텍스처는 WorldIntegration 텍스처 캐시가 만든다" | 게임 스레드가 장치 자원을 만든다 | `RenderHandle` 예약과 렌더 lane 생성(6.2절) |
| 〃 | 12절 5·6단계 | 종료 시 GPU idle 대기 → 장치·창 정리 | 5.5절 종료 순서 |
| 〃 | 13절 GPU 시험 | D3D12·OpenGL self-test | 기록 backend CPU 시험, D3D12·Vulkan·GLES 미리보기 |
| 〃 | 14절 제한 | 렌더 스레드는 후속 | RB1 |
| PlatformConcurrency(구현 문서) | :42 | 스레드 이름·우선순위·affinity·스택은 후속 | CP1에서 이름·우선순위·스택 크기 |
| UnrealCoreFeatureAudit | UE-C21·C23·C29 행 | 후속·부분 | CP8·CP5·CP4의 선행 관계를 적는다 |
| RepositoryLayout | 트리 | Engine/Source/Runtime에 렌더 기반 모듈 없음 | RenderBase·RenderGraph·ShaderCore, Developer 디버그 모듈, CP4·CP8이 들어갈 Core 모듈 |
