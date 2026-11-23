# 엔진 컴포넌트 도입 — 2. 분류 결과

[목차](README.ko.md)

## 2. 분류 결과

### 2.1 요약

| 분류 | 수 | 뜻 |
|---|---|---|
| 필수 | 58 | Rocks에 같은 역할의 컴포넌트를 만든다(3절). 시점은 묶음 CM0–CM11(5절) |
| 대체 | 3 | 기능은 들이되 컴포넌트 형태가 아니다(2.3절) |
| 제외 | 4 | 들이지 않는다(2.2절) |
| 후속 프로필 | 1 | 계획상 P3 프로필이 열릴 때 다룬다(2.3절) |
| 컴포넌트 아님 | 7 헤더 | 2.4절 |

### 2.2 제외

| UE 클래스 | 근거 | 대신 |
|---|---|---|
| `UModelComponent` | BSP 기하의 렌더·충돌 표현(`EC/ModelComponent.h:27, 38`). 사용자 지시로 BSP 제외 | 정적 메시(모델링 도구는 에디터 문서) |
| `UBrushComponent` | BSP 빌드용 brush와 volume 형상을 겸한다(`EC/BrushComponent.h:16, 44`). BSP를 빼면 brush 편집 모델이 필요 없다 | volume 형상은 영역 형상 `VolumeShape`(CD1, CR17)로 대체 |
| `UVectorFieldComponent` | 쓰는 곳이 구 파티클(Cascade) FX 시스템뿐이다(`Private/Particles/FXSystem.cpp`·`FXSystemSet.cpp`, `VectorField/VectorFieldVolume.h`). Niagara는 이 컴포넌트를 쓰지 않는다. 클래스에 `UE_DEPRECATED` 표지는 없으나 UE5에서 Cascade는 Niagara로 대체된 계열이다 | VFX 문서가 Niagara식 vector field 데이터를 정할 때 다룬다 |
| `UPawnNoiseEmitterComponent` | UE가 스스로 legacy 컴포넌트로 표시한다(`Engine/Private/Actor.cpp:3402` — "legacy component. Use AISense_Hearing instead."). 짝인 `UPawnSensingComponent`도 5.5에서 deprecated다(`AIModule/Classes/Perception/PawnSensingComponent.h:20`). 기본 호환 모드에서 AI Perception에도 전달하지만(`EC/PawnNoiseEmitterComponent.h:24`) 그 길의 본체는 Perception의 Hearing 감각이다 | AI 문서의 Perception(소리 자극)이 맡는다 |

### 2.3 대체·후속 프로필

| UE 클래스 | 분류 | 근거 | Rocks |
|---|---|---|---|
| `UHierarchicalInstancedStaticMeshComponent` | 대체(CD2) | ISM 위에 CPU cluster tree(`FClusterNode`)로 컬링·LOD를 더한 것이다(`EC/HierarchicalInstancedStaticMeshComponent.h:71–141`). Rocks는 인스턴스 판정을 GPU 인스턴스 culling(RN RS4)과 RenderScene 계층 공간 해시(EIP 10.4절 "정적 mesh·instanced·foliage의 렌더 판정")가 맡는다 | `InstancedStaticMeshComponent` 하나. 대량 인스턴스 판정은 렌더러 몫 |
| `UApplicationLifecycleComponent` | 대체(CD3) | `FCoreDelegates`의 앱 수명(활성·비활성·저메모리 등)을 Blueprint에 노출하는 래퍼다 | 앱 수명 이벤트를 엔진 이벤트로 Sparks·C++에 직접 노출(AppInfo, SE EI 단계). Blueprint가 없으므로 래퍼 컴포넌트를 두지 않는다 |
| `UPlatformEventsComponent` | 대체(CD3) | 변환형 노트북의 노트북·태블릿 모드 조회·통지 래퍼다(`EC/PlatformEventsComponent.h:14–40`) | 같은 정보를 플랫폼 이벤트로 노출 |
| `UStereoLayerComponent` | 후속 프로필(CD4) | HMD 합성 layer(`EC/StereoLayerComponent.h:277`). 계획은 stereo/VR을 별도 P3 프로필로 둔다(EIP 14절 1352행) | VR 프로필 문서 |

### 2.4 컴포넌트가 아닌 헤더

| 헤더 | 내용 | Rocks |
|---|---|---|
| `ComponentInterfaces.h`, `ComponentInterfaceIterator.h`, `ActorPrimitiveComponentInterface.h`, `ActorStaticMeshComponentInterface.h`, `ActorSkinnedMeshComponentInterface.h` | UObject 컴포넌트가 아닌 primitive(대량 인스턴스 등)도 같은 질의로 다루기 위한 인터페이스 층 | 들이지 않는다. Rocks는 렌더·물리 상태를 시스템 쪽 표(`RenderScene`·물리 body 표)에 두고 컴포넌트는 변경만 보낸다(RN 4.2절). 같은 문제가 생기지 않는다 |
| `ExternalMorphSet.h` | skinned mesh 외부 morph target 묶음 | `SkinnedMeshComponent`(CM7)의 부속 |
| `UnregisterComponentContext.h` | 대량 등록 해제 묶음 도우미 | CR1(등록 묶음 처리)에서 필요하면 내부 도우미로 |
