## 5. 렌더 lane

### 5.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 스레드 | 전용 OS 스레드 하나를 task graph의 렌더 스레드로 붙이고 그 큐를 처리한다 | `RC/Private/RenderingThread.cpp:321, 227–262` |
| 시작 | 스트리밍 정지 → flush → 게임 스레드가 RHI 소유권을 놓음 → RHI 스레드 모드 결정 → 스레드 생성·붙음 대기 → 설정 전파기 등록 → 펜스 왕복 → heartbeat 스레드 | `:561–654`(소유권 `:582`) |
| 종료 | heartbeat 정지 → flush → RHI 스레드 삭제 → 렌더 루프 탈출 작업 → Join → 게임 스레드가 소유권을 다시 잡음 → 지연 정리 묶음 삭제 | `:668–760`(소유권 `:746`) |
| 스레드 없는 경로 | 렌더 스레드가 없으면 명령을 바로 실행한다 | `RC/Public/RenderingThread.h:149, 504–525`, `Launch/Private/LaunchEngineLoop.cpp:2869–2872` |
| 명령 | 이동 전용 함수 세 형태(인자 없음·명령 목록·immediate 명령 목록) | `RC/Public/RenderingThread.h:180–184, 1087–1089` |
| 명령 큐 | 데스크톱 기본 모드: 뮤텍스로 보호하는 큐에 넣고, 비어 있다 처음 찰 때만 렌더 스레드용 task 하나를 띄워 FIFO로 비운다. 모바일 셰이더 플랫폼과 렌더하지 않는 앱: 명령마다 task 하나 | `RC/Private/RenderingThread.cpp:129–149, 1905–1935`, `RC/Public/RenderingThread.h:394–435, 508–515` |
| 보조 직렬 pipe | worker에서 직렬 실행하는 pipe. Engine 모듈의 SkeletalMesh와 플러그인 Niagara·Cable·Groom이 쓴다 | `RC/Public/RenderingThread.h:561`, `RC/Private/RenderingThread.cpp:2380–2416`, `Engine/Private/Rendering/RenderCommandPipes.cpp:5`, `Plugins/FX/Niagara/…/NiagaraComponent.cpp:58`, `Plugins/Runtime/CableComponent/…/CableComponent.cpp:20`, `Plugins/Runtime/HairStrands/…/GroomResources.cpp:45` |
| 지연 기록 명령 목록 | 다른 스레드에서 잠금 없이 기록해 두었다가 나중에 끼워 넣는다 | `RC/Public/RenderingThread.h:725–740, 803–832` |
| 동기화 | 펜스는 깊이 셋(렌더 스레드 처리·GPU 제출·스왑체인 flip). 게임 대기는 1 ms 단위로 기다리며 메시지 펌프와 상태 검사를 돌리고, 120초를 넘기면 hang 보고 후 종료 | `RC/Public/RenderCommandFence.h:14–53`, `RC/Private/RenderingThread.cpp:977–1051, 1089, 1096–1257` |
| 프레임 지연 한도 | 렌더 스레드 펜스는 N−1 프레임, 더 깊은 단계는 설정에 따라 기본 N−2까지 겹친다 | `:2435–2451, 2454–2572` |
| 실패 | Windows SEH로 잡아 상태를 기록하고 게임 스레드가 감지해 종료 | `:381–408, 816–842` |
| 렌더 스레드 tickable | heartbeat 스레드가 sleep 폴링으로 tick 명령을 넣는다 | `:466–484`, `RC/Public/TickableObjectRenderThread.h:61–95` |

### 5.2 Rocks 구조

```text
게임 lane                                렌더 lane(DedicatedLane)                   장치
RenderNotices 비우기(완료 결과 수신)  ◀── 완료 통지
FrameSlot 확보(메시지 펌프하며 대기)
World → FramePacket 추출
RenderLane::Post(프레임 packet) ──────▶ 그래프 구성·기록 ──────────────────────▶ Submit → GpuFencePoint
RenderLane::Post(자원·Retire) ────────▶ DeviceResourceOwner 갱신                   │
RenderLane::Post(표면 작업) ──────────▶ 스왑체인 생성·resize·파괴                  │
                                       fence 완료 이벤트 수신 → slot 완료·회수 ◀──┘
```

- 렌더 lane은 CP1 `DedicatedLane`의 인스턴스다. lane 작업 종류는 넷으로 닫는다: 프레임 packet 처리, 장치 자원 생성·갱신·`Retire`, 표면 작업(스왑체인 생성·resize·파괴, 표면 소실·재생성), lane 표지.
- **전달 가능한 타입.** lane 작업이 받는 값 타입은 "렌더 전달 가능" 표시를 명시 등록한 것만 허용한다(등록되지 않은 타입은 컴파일 오류). 등록 타입이 리플렉션 스키마를 가지면 Development에서 Object 참조 필드가 없는지 검사한다. 원시 포인터는 C++ 타입 검사로 걸러낼 수 없으므로(trivially copyable 구조체 안의 포인터) 이 두 단계가 강제할 수 있는 범위다. 프레임 packet의 가변 길이 목록은 그 프레임 slot의 프레임 arena(CP3)에 둔다.
- **인라인 모드**는 CP1 인라인 모드(trampoline)를 쓴다. 두 모드의 결과(장치 호출 열, 자원 상태)가 같아야 한다.
- **완료 통지 큐** `RenderNotices`: readback 결과, timestamp, `Retire` 회수 완료, 셰이더 공급 완료, 텍스처 스트리밍의 보임 신호(사용 기록별 마지막으로 그린 프레임)와 mip 전송 완료([텍스처 스트리밍](../TextureStreaming.ko.md) 결정 T3) 같은 결과를 렌더 lane이 넣고, 게임 lane이 프레임 시작 때 비운다. 게임 쪽 CPU 데이터를 GPU 완료 뒤까지 붙잡아야 하는 경우(ObjectRuntime `BackendLease`, `ObjectRefs.h:216–220`) 그 해제는 게임 쪽 통합 계층(WorldIntegration)이 이 통지를 받아 한다. RenderBase는 ObjectRuntime에 의존하지 않는다.
- **fence 관찰.** 렌더 lane은 GX7 완료 이벤트를 lane 작업으로 받는다(폴링하지 않는다). 그래서 lane이 놀고 있어도 slot 완료와 회수가 진행된다.
- 보조 직렬 pipe와 지연 기록 명령 목록은 후속이다. 병렬 장면 준비(계획 8.4절)가 생길 때 CP2 직렬 체인으로 만든다.

### 5.3 프레임 slot과 대기 규칙

- 프레임 slot은 2–3개다(계획 8.2절, 개수는 설정). slot마다 완료 표지가 둘이다: **lane 처리 완료**와 **GPU 완료**(그 프레임의 모든 queue `GpuFencePoint` 완료). UE 펜스 깊이 셋 가운데 앞의 둘에 해당한다.
- 게임 lane은 프레임 시작 때 slot을 확보한다. 빈 slot이 없으면 가장 오래된 slot의 GPU 완료를 기다린다. 렌더 lane은 게임 lane을 기다리지 않으므로 한 방향 대기다.
- **대기 중 메시지 펌프.** 게임 lane이 창 스레드이면 slot 대기는 OS 메시지를 펌프하는 이벤트 대기다. DXGI의 present·resize는 창 스레드로 동기 메시지를 보내므로, 창 스레드가 펌프 없이 막히면 교착이 난다(Microsoft DXGI 문서 "Multithread Considerations"). UE도 대기 중 메시지를 펌프한다(5.1절 동기화 행).
- **예외.** (1) 표면 소실 handshake: Android가 surface를 거둘 때 앱 스레드는 렌더 lane의 surface 사용 중단(표면 작업 완료)을 동기로 기다려야 한다(P13). (2) 전체 비우기 `RenderLane::Drain`: 종료·장치 재생성·시험 전용이며 호출 이유(열거)를 인자로 받아 계측에 남긴다. 일반 자원 해제는 `Retire`로 하고 비우기를 쓰지 않는다. 자원 해제 뒤 비우기까지 하는 API는 두지 않는다.
- 대기 시간 상한(UE 120초 hang 판정, 5.1절)은 진단 전용이다. 시간 초과는 hang 보고 후 실패로 끝내되 기능 판정에 쓰지 않고, 디버거 연결·명시 정지 계수가 있으면 끈다. 시험은 시간 초과에 기대지 않는다.
- 렌더 설정(CP4)은 프레임 packet에 snapshot으로 담는다. 렌더 쪽 사본 주소에 쓰는 전파는 하지 않는다.

### 5.4 장치 스레드 모델(GX10·GX11)

지금 `Device`와 최소 렌더러(현 Sandbox `SampleRenderer`)는 "생성한 스레드에서만 사용"이고(`Device.h:43`) D3D12·OpenGL backend 어디에도 호출 스레드 검사가 없다. 지연 셰이더 생성·병렬 기록·게임 lane handle 예약을 하려면 계약을 다음처럼 나눈다.

| 호출 종류 | 규칙 |
|---|---|
| 제출·present·fence 신호·장치 상태 변경 | 소유 스레드(렌더 lane) 전용 |
| 기록 객체 | 기록하는 스레드마다 하나. 기록 객체는 소유 스레드가 제출한다 |
| 자원·셰이더·파이프라인 생성 | backend가 `DeviceCaps`로 스레드 안전 여부를 공개한다. D3D12·Vulkan·Metal은 스레드 안전 생성(backend 내부 잠금). OpenGL ES는 context가 스레드에 묶이므로 소유 스레드에서만 만들고, 다른 스레드의 생성 요청은 lane 작업으로 넘긴다 |
| handle 예약 | 게임 lane은 장치 handle이 아니라 RenderBase의 `RenderHandle`을 예약받는다. RenderBase 표가 `RenderHandle` → 장치 handle을 잇고, 장치 내부 slot 표는 소유 스레드만 만진다 |
| 소유권 이관(GX10) | 소유 스레드가 놓고 새 스레드가 잡는다. 그 사이 소유 스레드 전용 호출은 `WrongThread`로 거부한다(Development 검사, Release에서는 검사 생략) |

렌더 lane 시작 때 장치 소유권을 lane으로 넘기고, 종료 때 되돌린다(UE도 시작·종료 때 소유권을 넘긴다, `RC/Private/RenderingThread.cpp:582, 746`, `RHI/Public/DynamicRHI.h:1497`). 장치를 렌더 lane 안에서 만드는 방식도 가능하지만 창·스왑체인 생성이 게임 lane 쪽 PlatformWindow와 엮이므로 이관 방식을 기본으로 한다.

### 5.5 시작·종료 순서

**시작:** 창·장치 생성(게임 lane) → `DeviceCaps` 조회 → 렌더 lane 시작(스레드 이름 붙임) → 장치 소유권 이관 → 스왑체인 생성(표면 작업) → 엔진 셰이더 집합 열기와 검사(9.2절) → 내장 자원 팩 생성·업로드(6.3절, fallback 셰이더 포함) → fence → `BuiltinReady` → 일반 에셋·프레임 시작.

**종료**(계획 3.2절 Shutdown 순서의 "GPU fence 완료"부터 "owner/retire/queue closure 확인"까지, CP6 hook):
1. 새 프레임 packet 차단.
2. 마지막 slot의 GPU 완료 대기.
3. 최종 GC와 `Retire` drain을 closure가 닫힐 때까지 반복한다. 최종 GC가 에셋을 파괴해 생긴 `Retire`도 이 반복에서 처리된다. 렌더 lane은 이 단계 내내 살아 있다.
4. 엔진 셰이더 집합·내장 자원 `Retire`, 파이프라인 캐시 비우기, 회수 완료 대기.
5. 스왑체인 파괴(표면 작업).
6. 렌더 lane Drain(남은 작업 0 확인).
7. 장치 소유권을 게임 lane으로 되돌림 → lane Join.
8. 장치 `WaitIdle`·소멸 → 창 파괴.

실패한 단계는 뒤 단계를 건너뛰지 않고 오류로 보고한다. 계획 3.2절은 timeout을 실패로 보며 강제 free하지 않는다.

### 5.6 실패 전파

렌더 lane의 장치 손실·치명 오류는 lane 상태에 기록하고, 게임 lane은 다음 slot 확보 결과(`Result`)와 완료 통지로 관찰한다. 기다리던 fence 대기는 GX7이 오류로 깨운다. 렌더 lane 스레드의 치명 오류 포착은 플랫폼 Fatal 처리(`Platform/Fatal.h`) 쪽 일이다. 장치 손실 뒤 재생성은 계획 11.2절(device epoch, 구 장치 recovery domain)을 따른다.

### 5.7 완료 기준(RB1)

**구현(2026-11-17).** 새 모듈 `Engine/Source/Runtime/RenderBase`(의존 Graphics·Task·MetaSchema)에 `RenderLane`·`FrameTicket`·`RenderLaneProcessor`·`ROCKS_RENDER_TRANSFERABLE`·`RenderNotices`·`RenderHandleTable`·`RenderBlob`을 두었다. 아래 기준은 기록 backend(`RenderBase/Tests/RecordingDevice`) CPU 시험과 Sandbox self-test(D3D12·WARP·OpenGL, Thread·Inline)로 확인한다. 구현 메모:
- 작업 함수는 캡처 없는 함수 포인터만 받는다. 캡처로 게임 주소를 넘기는 길을 막기 위해서다. lane 쪽 객체는 `RenderLaneContext::Processor`로 얻는다.
- `ObjectRef`는 이동할 수 없으므로 그 필드를 가진 struct는 등록해도 C++ 타입 검사가 먼저 거부한다. Development 스키마 검사는 이동할 수 있는 `WeakObjectRef`·참조 컨테이너 필드를 거부한다(중첩 struct는 보지 않는다).
- slot의 GPU 완료 표지는 그 프레임 작업 안의 마지막 제출 fence이며 `NotifyFence`로 받는다. fence 완료는 lane inbox로 들어가 lane 작업(Inline 모드는 기다리는 게임 lane이 실행하는 lane 작업)이 장치 회수 → 통지 순서로 처리한다.
- 대기 중 펌프는 1 ms 간격(UE와 같다)이며 펌프 hook이 없으면 이벤트만 기다린다. 대기 진단 상한(120초)의 디버거 연결 감지는 아직 없다.
- `Start`는 시작 작업 완료를 기다리지 않는다(첫 `BeginFrame`이 기다린다). 그래서 이관 중에 게시한 표면 작업도 lane이 소유권을 얻은 뒤 FIFO로 실행된다.
- 설정 snapshot(12절)은 `RenderLane::AttachSettings(ticket, RenderSettingsRef)`로 slot에 담고 `RenderFrameContext::Settings`로 읽는다(개정 8).

- 인라인 모드와 lane 모드에서 같은 프레임 packet 열이 같은 장치 호출 열을 낸다(기록 backend 대조). lane 작업 안의 재진입 `Post` 순서도 같다.
- slot 2개 설정에서 GPU 완료 이벤트를 gate로 막으면 게임 lane이 세 번째 slot 확보에서 대기하고, gate를 열면 진행한다. 대기 중 메시지 펌프가 도는지 계수로 확인한다.
- 등록되지 않은 값 타입을 받는 lane 작업은 컴파일 오류다. Object 참조 필드가 있는 등록 타입은 Development 검사가 거부한다.
- 종료 순서: 진행 중 packet과 최종 GC가 만든 `Retire`가 있는 상태의 종료에서 남은 작업 0, 해제 누락 0, 장치 소유권 복귀, 스왑체인이 창보다 먼저 파괴.
- 소유권 이관 중 resize 요청이 표면 작업으로 처리된다(거부 0).
- 장치 손실 주입 뒤 다음 slot 확보가 오류를 돌려주고, 기다리던 fence 대기가 깨어난다.
