## 12. 콘솔 설정(CP4·CP8)

- RenderCore는 설정 변수로 기능 전환·디버그 모드·예산을 다룬다(그래프만 해도 31개, `RC/Private/RenderGraphPrivate.cpp:18–426`). Rocks에는 이 체계가 없고 Config 계층도 후속이다(UE-C21·C29).
- 이 문서는 렌더 쪽 요구만 정한다. 체계 자체는 [설정 계층·콘솔 설정 설계](../ConfigSystem.ko.md)(CP8·CP4, 단계 CF1–CF5)가 정한다. 계획 24.2절 Config 우선순위(engine default → platform → project → device profile → user → session override), 런타임 변경 가능·재시작 필요 구분, 값 출처 표시가 그 스펙의 범위다.
- 렌더 요구:
  - 타입 있는 변수(bool·int·float·열거·문자열)와 명령, 이름 공간(렌더 그래프·셰이더·렌더 lane 등).
  - **렌더 snapshot.** 렌더 lane이 읽는 변수는 "렌더 읽기" 표시를 가지고, 게임 lane이 프레임 packet을 만들 때 그 값들의 snapshot을 담는다. 렌더 lane은 packet의 값만 읽는다. UE는 렌더 스레드 사본 주소에 쓰는 명령으로 전파한다(`RC/Private/RenderingThread.cpp:493–530`).
  - 셰이더 형식별 값(UE `RC/Public/ShaderPlatformCachedIniValue.h:13–95`)은 셰이더 시스템의 형식 정의(셰이더 시스템 12.1절)나 TargetProfile 쪽으로 두고 콘솔 설정에 두지 않는다. 쿠킹 결과에 영향을 주는 값은 키에 들어가야 하기 때문이다.
  - 헤드리스·시험에서 결정적 값을 고정할 수 있어야 한다.
- **구현(2026-11-17, RB1 설정 snapshot).** 게임 lane은 sync 지점(`ConfigContext::Sync`) 뒤 `ConfigContext::Snapshot()`을 `RenderSettingsRef::Share`로 감싸 `RenderLane::AttachSettings(ticket, ...)`로 그 프레임 slot에 담는다. 렌더 lane은 프레임 작업 안에서 `RenderFrameContext::Settings().Get<config::ConfigSnapshot>()`만 읽는다(registry·문맥은 읽지 않는다). slot의 참조는 그 slot을 다음에 다시 확보할 때(`BeginFrame`, GPU 완료 뒤) 또는 lane 정지 때 게임 lane에서 놓는다. 그래서 snapshot 수명은 그 프레임 slot이 끝날 때까지이고 파괴는 게임 lane에서 일어난다. 시험(`RenderBase/Tests/RenderSettingsTests.cpp`): sync 지점에서 바꾼 값은 다음 packet부터 보이고 이전 packet은 옛 값을 읽는다. GPU 완료 gate가 닫힌 동안 옛 snapshot이 살아 있고, slot을 다시 확보하면 놓인다.
  - 스펙과 다른 점: RenderBase는 ObjectRuntime에 의존하지 않으므로(9.3절) Config 타입을 직접 이름 짓지 않는다. `RenderSettingsRef`는 snapshot의 공유 소유권(`SharedPtr` aliasing)과 타입 표지만 들고, 꺼낼 때 같은 타입으로만 돌려준다(다른 타입이면 null). 타입 표지는 Core `TypeKeyOf<T>()`(Core/TypeKey.h)이고 `ConfigSnapshot`은 export 키를 선언한다. 공개 헤더의 변수 템플릿 주소를 표지로 쓰면 Modular 구성에서 DLL마다 주소가 달라 다른 모듈의 `Get<ConfigSnapshot>()`이 조용히 null이 되므로 버렸다(BuildTargets BS8e, 정책 `public-header-type-tag`). 타입은 Config를 아는 쪽(실행 파일·렌더러 모듈)이 정한다. 담지 않은 프레임은 빈 참조를 읽는다(직전 프레임 값을 물려받지 않는다).

- **CVar 이관(설계, 구현 전).** [CVar·치트 명령 설계](../CVarSystem.ko.md)(개정 2)가 콘솔 설정을 타입 있는 핸들 `CVar<T>`로 바꾼다. 착수 시점은 사용자가 정한다. 그때 RB1 연결은 다음처럼 바뀐다.
  - 렌더 snapshot은 "렌더 읽기" 표시가 있는 값만이 아니라 **모든 CVar**를 담는다(CVar VD6). 설정 struct 필드의 `RenderRead` 블록은 같은 snapshot 안에 남는다.
  - 게임 lane은 `ConfigContext::Snapshot()` 대신 `Acquire()`로 얻은 참조를 지금처럼 `RenderSettingsRef`로 프레임 slot에 담는다(CVar 7.2절).
  - 렌더 lane은 프레임 작업을 시작할 때 packet의 snapshot을 `cvar::SnapshotBinding`으로 그 스레드에 묶고, 작업이 끝날 때 놓는다. 그 사이 렌더 코드는 이름 탐색 없이 `ShadowDistance.Get()`처럼 읽는다(CVar 7.1·16절).
  - 이관 뒤에는 게임 lane이 **모든 프레임**에 snapshot을 담는다. 바뀐 것이 없으면 같은 snapshot 참조다. 그래서 렌더 lane은 늘 묶을 snapshot이 있고, 묶음 없는 `Get()`은 Fatal이라는 CVar 규칙(VD7)과 어긋나지 않는다. 지금의 "담지 않은 프레임은 빈 참조" 규칙은 이관 때 없앤다.
  - 렌더 lane이 만든 worker 작업은 묶인 snapshot의 참조를 작업 문맥으로 넘겨받는다(CVar 10.1절).
