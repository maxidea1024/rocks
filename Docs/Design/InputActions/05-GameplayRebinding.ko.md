# 입력 액션 계층 — 5. 게임플레이 연결·리바인딩

[목차](README.ko.md)

## 5. 게임플레이 연결·리바인딩

### 5.1 PlayerController·바인딩

`PlayerController`는 `PlayerActionInput`과 `InputActionBindings`(UE `UEnhancedInputComponent`)를 가진다. 둘 다 NATIVE(실행 중 상태)다.

컨텍스트 객체 쥐기(2.7절 — UE `AppliedInputContextData`가 컨텍스트를 강하게 붙잡는 역할):
- IA3은 bundle 수준 API다: `PlayerController::ActionInput()`(`PlayerActionInput`)에 `AddContext(bundle, …)`로 넣는다. 코드로 만든 객체는 호출자가 handle로 쥐고(PlayerController의 기존 NATIVE handle + `World::With` 방식) 모은 bundle을 넣는다.
- 에셋 컨텍스트는 적용한 동안 PlayerController가 `AssetLease`로 쥔다(5.4절 — IA6b).

평가 시점:
- `PlayerController::OnTick` 안에서 한다. 입력 스냅숏과 제스처 다음, `OnPlayerTick` 전이다(UE `TickActor`·`PlayerTick`이 `TickPlayerInput`을 먼저 한다 — `PlayerController.cpp:2326·5504-5510`).
- 순서: 입력 스냅숏 → 제스처 → `Evaluate` → `InputActionBindings::Dispatch` → `FinishDispatch` → `OnPlayerTick`(pause 중 생략) → `OnControllerTick`. 평가 실패(다시 만들기 할당 실패 — 이전 목록으로 평가된다 — 또는 이벤트 기록 실패)는 Warning `InputEvaluationFailed`로 남기고 그 프레임 결과로 전달한다. 경고는 실패가 이어지는 동안 한 번이다.
- 평가 문맥: `deltaSeconds` = `TickContext::unscaledDeltaSeconds`, `timeDilation` = World 시계 `TimeScale()`(UE `GetActorTimeDilation` 대응 — pause는 따로), `paused` = `TickContext::paused`, `fov` = CameraDirector 뷰의 fov(없으면 0 — 카메라 없음), `hasPlayer` 참, `gamepad` = LocalPlayer `ControllerId`(음수면 0 — `SetPlayer`가 받고 `ClearPlayer`가 0으로 되돌린다). fov는 앞 프레임 카메라 뷰다(카메라는 LateUpdate에 갱신 — UE FovScaling도 PlayerCameraManager의 직전 값을 읽는다).
- 일시정지 중에도 평가하고 전달한다. 막는 것은 액션의 `triggerWhenPaused`다. `OnPlayerTick`은 pause 중 건너뛰지만 전달은 하므로, pause 메뉴의 "재개" 액션이 동작한다. UE도 pause Tick에서 `TickPlayerInput(…, bGamePaused = true)`로 입력을 처리한다.

바인딩 API(UE `EI/Public/EnhancedInputComponent.h`):
- `BindAction(actionId, ActionEvent events, Function<void(const ActionEventData&)>, bool consumes = false) → ActionBindingHandle`
  - `ActionEventData`는 액션 id, 이벤트, 값, 경과·Triggered 시간이다(UE `(const FInputActionInstance&)` 형태).
  - 빈 함수·None 마스크는 `InvalidArgument`다(UE는 `ETriggerEvent::None` 바인딩을 이벤트 없는 틱마다 부른다 — 버린 대안: 쓰임이 불분명하고 매 평가 호출이 된다). 할당 실패면 `OutOfMemory`다.
- `BindFunction(actionId, ActionEvent events, ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle target, const Guid& functionId, bool consumes = false) → ActionBindingHandle`(IA7 — UE `BindAction(const UInputAction*, ETriggerEvent, UObject*, FName)`, `EI/Public/EnhancedInputComponent.h:497-503`):
  - 리플렉션 함수(`ROCKS_FUNCTION`)를 (객체 handle, FunctionId)로 바인딩한다. 재사용 값 `FunctionBinding`([이벤트 리플렉션](../EventReflection.ko.md) EV1a — `Object/FunctionBinding.h`)을 쥔다. 함수 id는 `TypeRegistry::FindFunctionByName(type, name)`의 `Schema().id`다.
  - 서명은 `void(const ActionEventData&)`다. 바인딩할 때 `FunctionBinding::Create<const ActionEventData&>`가 검사한다. 오류는 그대로다: 함수 없음 `NotFound`(게임 빌드의 EditorOnly 함수 포함), static 함수 `InvalidArgument`, 반환이 있거나 `Result` 함수·서명 불일치 `TypeMismatch`, 대상 해석 오류(`StaleHandle`·`WrongContext` 등 Registry 그대로). None 마스크는 `InvalidArgument`, 할당 실패는 `OutOfMemory`다.
  - UE는 이 경로에 인자 넷 `(FInputActionValue ActionValue, float ElapsedTime, float TriggeredTime, const UInputAction* SourceAction)`을 넘긴다(`FEnhancedInputActionHandlerDynamicSignature` — `EI/Public/EnhancedInputComponent.h:33`, Execute `:356-359`). 이벤트는 넘기지 않는다. Rocks는 콜백과 같은 `ActionEventData`(액션 id·이벤트(마스크 적용)·값·두 시간)를 struct 하나로 넘긴다. 버린 대안: UE처럼 인자 넷 따로. 그러면 이벤트 마스크를 받지 못하고, 콜백 판과 정보가 달라지며, frame 칸이 넷이 된다.
  - 바인딩은 registry 포인터와 access 사본을 쥔다. registry는 바인딩보다 오래 살아야 한다(World Registry는 PlayerController보다 오래 산다). 대상은 handle만 쥔다(수명을 늘리지 않는다).
  - 전달: 콜백 바인딩과 같은 실행 목록·Started 먼저·`consumes`·chord 억제·전달 중 지운 바인딩 규칙을 따른다. 바인딩마다 `BindingFrame<const ActionEventData&>`에 이번 데이터를 담아 `Invoke`한다(할당 없음 — ActionEventData는 힙을 쓰지 않는다).
  - 호출 실패: 대상이 사라졌으면(`StaleHandle`) 그 바인딩을 지운다(전달이 끝난 뒤 — 대상은 돌아오지 않는다). 그래서 그다음 전달부터는 그 바인딩의 `consumes`도 걸리지 않는다. UE는 바인딩을 남긴다. `TEnhancedInputUnifiedDelegate::Execute`가 `IsBound()`(대상 객체 유효)로 호출만 건너뛰고(`EI/Public/EnhancedInputComponent.h:121-140`), 소비는 계속 건다(`EI/Private/EnhancedPlayerInput.cpp:900-905`). 버린 대안: 남겨 두기. 목록이 자라고, 사라진 객체가 계속 입력을 막는다. 파괴 대기(`Destroyed`)면 이번 호출만 건너뛴다. 그 밖의 오류(다른 스레드 `WrongThread`·`WrongContext`·`Busy` 등)는 건너뛰고 바인딩마다 한 번 Warning `InputFunctionBindingFailed`(바인딩·액션·함수 id)를 남긴다. 호출이 실패해도 그 전달의 `consumes`는 건다(콜백의 "기록이 없으면 실행만 건너뛴다"와 같다).
  - 스레드: 함수 바인딩은 그 registry의 소유 스레드에서 전달해야 한다(아니면 `WrongThread` — Warning). 게임 World 객체면 게임 스레드다.
  - 쿠킹: 이 바인딩은 실행 중 값이라 저장·쿠킹하지 않는다(FunctionReflection FR4의 "EditorOnly 함수 바인딩 쿠킹 거부"는 저장하는 바인딩 몫 — 이벤트 리플렉션). 게임 빌드에서 EditorOnly 함수는 `NotFound`로 바인딩되지 않는다.
  - `ActionEventData`는 리플렉션 struct다(ROCKS_STRUCT — 칸 서술의 `TypeIdOf`). 그래서 `InputValue`도 리플렉션 struct이고 `ActionEvent`는 리플렉션 Flags enum이다. 강한 참조를 담지 않는다(FD4 — frame에 둘 수 없다).
  - 콜백 수명: `Function`은 캡처를 그대로 쥔다(UE delegate처럼 객체 파괴를 알지 못한다). 객체는 handle을 캡처해 `World::With`로 접근하거나, 없어지기 전에(OnUnPossess·EndPlay) `RemoveBinding`한다. 콜백을 만든 allocator는 바인딩보다 오래 살아야 한다.
- `BindActionValue(actionId) → ActionValueHandle`과 `ActionValue(handle) → InputValue`: 평가마다 갱신한다. 포인터를 내주지 않는다(재배치 안전).
- `RemoveBinding(handle)`·`RemoveValueBinding(handle)`·`ClearBindings()`. 전달 중(콜백 안)에 지운 바인딩은 그 평가의 실행 목록에 있으면 실행되고, 전달이 끝난 뒤 지워진다(UE는 실행 목록을 복제본으로 만든다).
- 바인딩 마스크와 이벤트: 한 바인딩은 마스크(여러 비트 가능)와 겹치는 이벤트가 있으면 한 번 실행한다. 콜백이 받는 `events`는 (이번 이벤트 & 마스크)다. 그래서 Started와 Triggered가 한 평가에 나면 Started 바인딩은 Started를, Triggered 바인딩은 Triggered를 받는다(UE가 그 평가 동안 `TriggerEvent`를 바인딩 이벤트로 바꿔 부르는 것과 같다 — :868-883).
- 값 바인딩은 전달마다 그 액션의 현재 값이다. 표에 없는 액션은 0이고 타입은 마지막 값의 것이다(UE는 액션 타입의 0 — :911-925).
- `InputActionBindings`는 InputActions 모듈의 값 타입이다(Gameplay에 묶이지 않는다 — 시험이 PlayerController 없이 결정적이다). 바인딩은 하나씩 따로 할당해 콜백 실행 중 추가가 실행 중인 콜백을 옮기지 않는다.

전달 규칙(UE :720-1006):
- 바인딩 목록을 등록 순서로 돌며, 액션의 이번 이벤트와 바인딩 이벤트 마스크가 겹치는 것을 실행 목록에 넣는다.
- Started 바인딩은 실행 목록 **맨 앞에 차례로 끼워 넣는다**(UE `EmplaceAt(0)` — :801-808). 그래서 Started끼리는 등록 역순이고, Started 전체가 다른 이벤트보다 먼저 실행된다.
- `consumes`: 실행 목록 순으로 그 바인딩 뒤의 같은 액션 바인딩 실행을 막는다. chord 억제로 건너뛴 바인딩은 소비하지 않는다(UE 리스너 소비 — `EnhancedInput.EnableListenerConsumption` 기본 1, :838-844·863·900-905). 실행 직전에 기록을 다시 찾고, 없으면 실행만 건너뛴다(소비는 건다 — UE :865).
- chord 억제(4.4절)를 적용한다. "Triggered로 전달됨"은 Triggered 비트가 겹친 바인딩이 실행 목록에 든 액션이다(실행 전에 모은다 — UE :808-812). chord 짝은 바인딩마다 다시 읽는다(콜백의 즉시 다시 만들기가 목록을 바꾼다).
- 실행은 평가 시점 목록의 사본으로 한다. 그래서 콜백이 바인딩을 더하거나 지우면 다음 평가부터 반영된다.
- 입력 컴포넌트 **스택**(Pawn 등이 층을 쌓고 `bBlockInput`이 아래를 막는 것 — UE `PlayerInput.cpp:1500-1527`)은 CR26이다. 이번에는 PlayerController 한 층이다(7.4절).

### 5.2 사용자 설정·리바인딩(UE `EI/Public/UserSettings/EnhancedInputUserSettings.h`)

`InputUserSettings`(로컬 플레이어마다 — InputActions 모듈의 값 타입, Gameplay `LocalPlayer`가 가진다):
- `profiles: Array<KeyProfile>`, `currentProfileId`
- `KeyProfile { id, displayName(String), rows: Array<KeyMappingRow> }`
- `KeyMappingRow { mappingName, Array<PlayerKeyMapping> }`
- `PlayerKeyMapping { slot, defaultKey, currentKey, dirty }`
  - `slot`은 0…6(First…Seventh — UE `EPlayerMappableKeySlot`, `KeySlotCount` 7)이다.
  - `IsCustomized()`는 currentKey ≠ defaultKey다. `dirty`는 저장 뒤 바뀌었다는 표지다(UE `bIsDirty` — 저장은 customized이거나 dirty인 매핑을 쓴다).
- 처음에는 기본 프로필 하나다(id `InputUserSettings.Profiles.Default`, 표시 이름 "Default Profile" — UE `DefaultProfileIdentifierString`과 같은 문자열이라 UE 컨텍스트의 프로필 override·supportedProfiles 이름과 맞는다). 그것이 현재 프로필이다.
- 바뀔 때마다 `Revision()`이 는다. 평가기(`PlayerActionInput::SetUserSettings`)는 `ApplyPendingChanges`에서 수가 바뀌었으면 다시 만들기를 한다(UE `OnSettingsChanged` → `RequestRebuildControlMappings`). 설정이 붙어 있으면 활성 프로필은 설정의 현재 프로필이다(`SetKeyProfile`은 설정이 없을 때 쓴다).
- 스레드: 게임 스레드 전용이다. 평가기는 설정을 가리키기만 한다(설정이 평가기보다 오래 살거나, 없어지기 전에 `SetUserSettings(nullptr)`).
- Gameplay 연결: `LocalPlayer::InputSettings()`가 그 플레이어의 설정이다(UE는 EnhancedInputLocalPlayerSubsystem이 `UEnhancedInputUserSettings`를 가진다). `PlayerController::SetPlayer`가 평가기에 붙이고 `ClearPlayer`가 뗀다(LocalPlayer 제거·EndPlay 때 — 설정이 평가기보다 먼저 없어지지 않는다). 게임의 컨텍스트 추가는 `notifyUserSettings`를 켜야 행이 생긴다(UE `bNotifyUserSettings` 기본 거짓과 같다).

UE는 slot을 하드웨어 장치 ID별로 둔다. 그런데 기본 구현이 Invalid를 돌려주므로(`UserSettings.cpp:1798-1801`) 모든 장치가 한 slot 열을 쓴다. Rocks도 장치 칸 없이 **이름마다 slot 한 열**이다(IAD9).

동작:
- **등록**(`RegisterContext(bundle)` — UE `RegisterInputMappingContext` :1598-1782): 컨텍스트를 `notifyUserSettings`로 추가하면 평가기가 부른다(빼면 `UnregisterContext` — 등록 목록에서만 빠지고 행은 남는다, UE와 같다).
  - 컨텍스트의 프로필 override 이름마다 없는 프로필을 만든다(현재 프로필은 그대로).
  - 모든 프로필에, 그 프로필의 바인딩(override 또는 기본) 중 리바인딩 가능한 것(매핑 설정이 있는 것 — `settingBehavior` Ignore가 아니고 설정이 있다, UE `IsPlayerMappable`)마다 이름(매핑 설정 `name`) 행을 찾거나 만든다. supportedProfiles가 그 프로필을 빼면 넣지 않는다.
  - 한 등록 안에서 같은 이름의 바인딩마다 slot을 0부터 차례로 배정한다. 7개(`KeySlotCount`)를 넘는 바인딩은 행에 넣지 않고 Warning `InputMappingSlotOverflow`로 알린다(UE는 8번째를 Unspecified, 그 뒤를 Max 칸에 넣는다 — Rocks는 칸을 7개만 둔다. 넘친 바인딩은 다시 만들기에서 그 이름 행의 키를 받는다). 그 slot의 매핑이 이미 있으면(불러온 사용자 키) 기본 키만 바인딩 키로 고치고(기본 키와 같아졌으면 dirty를 지운다 — UE `UpdateDefaultKeyFromActionKeyMapping`), 없으면 기본 키 = 현재 키로 만든다.
- **`MapPlayerKey(mappingName, slot, key, profile = 현재, createSlotIfNeeded = 거짓)`**(UE cpp:1133-1250):
  - 그 (이름, slot)의 currentKey를 바꾼다. 같은 키면 아무 일도 없다(UE `SetCurrentKey` — 키가 다를 때만 dirty).
  - slot이 없고 `createSlotIfNeeded`면 새로 만든다. 이때 defaultKey는 없음이다. 같은 이름 행에 매핑이 하나 이상 있어야 한다(UE :1192).
  - 바꾸면 `Revision()`이 는다(평가기가 다시 만든다). 새 매핑과 바뀐 매핑은 dirty다.
  - 오류:
    - 이름이 비었다·키가 무효다·slot이 범위 밖이다: `InvalidArgument`(UE는 키를 보지 않는다 — Rocks는 무효 키를 막는다)
    - 프로필이 없다: `NotFound`
    - 이름 행이 없다: `NotFound`
    - slot이 없고 만들 수 없다: `NotFound`(UE는 아무 일 없이 넘어간다 — 호출자가 알 수 있게 Rocks는 오류)
    - 할당 실패: `OutOfMemory`(그대로)
- **`UnmapPlayerKey(mappingName, slot, profile = 현재)`**: 기본 키로 되돌린다(UE cpp:1252-). 매핑이 없으면 `NotFound`다. 이미 기본 키면 수가 늘지 않는다.
- **프로필**: `CreateProfile(id, displayName)`(같은 id는 `AlreadyExists`), `SetCurrentProfile(id)`(없으면 `NotFound`, 바꾸면 수가 는다), `ResetProfileToDefault(id = 현재)`(모든 매핑을 기본 키로). UE `CreateNewKeyProfile`·`SetActiveKeyProfile`·`ResetKeyProfileToDefault`다.
- **적용**: 다시 만들기(4.2절 2)에서 리바인딩 가능한 바인딩의 키를 바꾼다(UE `EnhancedInputSubsystemInterface.cpp:1004-1020`, UserSettings :479-500·535-546).
  - 활성 프로필의 같은 이름 행에서, 기본 키와 **같은 종류**(게임패드 키인지·터치인지 — UE `bMatchBasicKeyTypes`, 제스처는 Rocks 키에 없다)인 currentKey들로 바꾼다(`KeysForBinding`). 무효 키(slot을 비운 것)는 넣지 않는다.
  - 키 하나에 바인딩 하나를 만든다. 고를 키가 없으면 바인딩의 기본 키 그대로다(UE :1012-1016).
- **충돌**: 다른 이름이 이미 쓰는 키로 바꾸는 요청은 막지 않는다(UE와 같다). `FindMappingsUsingKey(key, out)`로 UI가 알린다(현재 프로필에서 그 키를 쓰는 이름 — UE `GetMappingNamesForKey`).

### 5.3 저장

UE는 `USaveGame` 슬롯 "EnhancedInputUserSettings"에 쓴다(cpp:842-895·948-1040). 형식은 버전 2이고, 프로필 헤더 다음에 바뀐 매핑만 {이름, 키 이름, slot}을 쓴다.

Rocks:
- 같은 내용을 JSON 텍스트로 읽고 쓴다(Core `JsonWriter`·`JsonDocument`). 리플렉션 JSON(`ReflectionText` ExportJson·ImportJson)은 객체 Registry를 대상으로 하므로 플레이어 설정 같은 순수 값에는 쓰지 않는다(버린 대안 — 설정이 Registry·타입 등록을 요구하게 된다).
  - `Save(StringBuilderBase& out)`·`Load(StringView text, Allocator& scratch)`
  - 형식(버전 1): `{"version":1,"currentProfile":id,"profiles":[{"id":…,"displayName":…,"mappings":[{"name":…,"slot":n,"key":키 이름},…]},…]}`. 사람이 고칠 수 있게 들여 쓴다(JsonWriter 기본). mappings는 customized이거나 dirty인 매핑만이고, 저장하면 dirty를 지운다. displayName이 없으면 "Default Profile"이다.
- 키는 이름(2.2절)이다.
- 불러온 매핑은 행이 없어도 만든다(기본 키 없음 — 등록이 기본 키를 채운다, UE와 같다).
- 불러오기는 매핑·현재 프로필을 **덮어쓴다**(UE는 저장본에서 새 설정 객체를 만든다). 파일은 바꾼 매핑만 담으므로, 먼저 모든 매핑을 기본 키로 되돌리고(dirty 끔) 기본 키가 없는 매핑(사용자가 만든 slot)과 빈 행을 지운 뒤 파일의 매핑을 넣는다. 같은 id 프로필의 같은 (이름, slot)은 현재 키만 바꾼다. 파일에 없는 프로필은 남는다(등록이 만든 override 프로필일 수 있다). 수를 늘린다. 그래서 "저장본으로 되돌리기"는 `Load(저장본)`이다.
- 비운 slot(현재 키 무효 — 사용자가 만든 slot을 되돌린 것)은 빈 키 이름 `""`으로 쓰고, 불러오기는 빈 이름을 비운 slot으로 읽는다.
- 파일 위치(사용자 Saved 폴더)는 호출자(Gameplay `LocalPlayer`)가 정한다. Rocks에는 아직 SaveGame 체계가 없다.
- 모르는 키 이름·범위 밖 slot의 매핑은 건너뛰고 Warning `InputUserSettingsEntrySkipped`로 알린다(설정을 버리지 않는다). JSON이 깨졌거나 필수 칸(version·profiles·각 id)이 없거나 종류가 틀리면 `InvalidArgument`이고(Config의 JSON 오류와 같다) 기존 설정은 그대로다. 더 높은 version은 `Unsupported`다(그대로). 현재 프로필 id가 목록에 없으면 기본 프로필로 둔다.

### 5.4 에셋 컨텍스트(IA6b — IAD14)

`PlayerController`(Gameplay — 공개 의존에 `AssetRuntime`을 더한다)가 적용한 에셋 컨텍스트의 lease를 쥔다. UE `AppliedInputContextData`가 컨텍스트 객체를 강하게 쥐는 것과 같은 역할이다(`EI/Public/EnhancedPlayerInput.h`). 추가 API는 `EI/Public/EnhancedInputSubsystemInterface.h:265`다. 표는 (AssetId, 컨텍스트 id, lease)이고 NATIVE다. 모두 게임 스레드 전용이다(AssetManager와 같다).

- `AddInputContext(AssetManager& assets, AssetLease&& context, int32 priority, const ContextOptions& options = {})`(UE `AddMappingContext(const UInputMappingContext*, int32, const FModifyContextOptions&)`):
  - lease의 InputContext를 2.8절 도우미로 모아 `ActionInput().AddContext(bundle, priority, options)`에 넣는다. 성공하면 언제나 lease를 가져간다(호출자의 lease는 비게 된다).
  - 같은 에셋을 다시 넣으면 `AddContext`의 같은 id 규칙을 따른다(Untracked면 우선순위를 바꾸고, CountRegistrations면 등록 횟수만 는다 — 4.1절). 쥔 lease는 받은 새 lease로 바꾼다(옛 lease를 놓는다 — 같은 에셋이라 상주는 그대로다. 옛 lease가 scope 회수로 비었어도 다시 넣으면 살아난다). 모은 id가 쥘 때와 다르면 `InvalidArgument`다.
  - 다른 원천이 이미 그 컨텍스트 id를 넣었으면 `InvalidArgument`이고 아무것도 바꾸지 않는다. 다른 원천은 다른 에셋이거나 `ActionInput().AddContext`로 넣은 코드 bundle이다. 한 id는 한 원천이다(UE는 객체 포인터가 키라 겹치지 않는다).
  - 실패하면 상태와 lease는 그대로다(lease는 옮기지 않는다). 오류는 2.8절 모으기, `AddContext`(검증 `InvalidArgument`), 표 확보 실패 `OutOfMemory`다. 표 칸은 `AddContext` 전에 확보한다.
- **원천 확인(세대).** 표의 칸은 넣을 때 평가기 컨텍스트 칸의 세대(`PlayerActionInput::ContextSerial(id)` — 새 칸을 만들 때마다 바뀌고, 없으면 0)를 함께 적는다. 세대가 지금 값과 다르면 그 칸은 끝난 것이다. 평가기에서 직접 뺐거나(`ActionInput().RemoveContext`·`ClearContexts`), 뺀 뒤 같은 id를 다시 넣은 경우다. 끝난 칸은 `Add`·`Remove`·`Refresh`를 시작할 때와 `OnTick` 평가 전에 lease를 놓는다. `HasInputContext`는 세대를 비교해 판정만 한다(const). 그래서 끝난 칸에서 `HasInputContext`는 거짓, `Remove`·`Refresh`는 `NotFound`이고, `Add`는 새로 넣는 것으로 본다.
- 에셋이 쥔 컨텍스트 id에 `ActionInput()`을 직접 쓰는 것(`UpdateContext`, 같은 id `AddContext`로 우선순위·등록 횟수 바꾸기)은 계약 위반이다. 결과는 정하지 않는다(다음 `RefreshInputContext`가 에셋 데이터로 덮어쓴다). 세대가 같아 감지하지 못한다.
- `RemoveInputContext(AssetId asset, const ContextOptions& options = {})`(UE `RemoveMappingContext`): `ActionInput().RemoveContext(id, options)`를 부른다. 컨텍스트가 평가기에서 빠지면(CountRegistrations면 등록 횟수 0) lease를 놓는다. 쥐지 않은 에셋(끝난 칸 포함)은 `NotFound`다.
- `RefreshInputContext(AssetManager& assets, AssetId asset, bool forceImmediately = false)`: 쥔 lease로 다시 모아 `ActionInput().UpdateContext(bundle, forceImmediately)`에 넣는다(2.7절 — 우선순위·등록 횟수는 그대로). 에셋 데이터를 바꾼 뒤 부른다. 쥐지 않은 에셋은 `NotFound`다. 모은 컨텍스트 id가 쥘 때와 다르면 `InvalidArgument`다(키가 바뀐 데이터는 Remove 뒤 Add). 회수된 lease는 `StaleHandle`이다. 실패하면 평가기는 이전 사본 그대로다.
- `HasInputContext(AssetId asset)`: 쥐고 있고 그 칸이 끝나지 않았으면(세대가 같으면) 참이다.
- 수명: `OnFrameworkEndPlay`가 쥔 컨텍스트를 평가기에서 모두 빼고(등록 횟수를 세는 컨텍스트는 0이 될 때까지 기본 옵션으로 `RemoveContext`를 되풀이한다 — 사용자 설정 등록은 건드리지 않는다, 뒤의 `ClearPlayer`가 설정을 뗀다) lease를 놓는다. EngineAssets 종료 계약에서 World가 쥔 lease는 World 파괴가 놓는다(AssetSystem 11.8절 6). 그 뒤 `AddInputContext`는 `InvalidArgument`다(Actor 정리 뒤 — 쥘 수 없다).
- 컨텍스트를 모든 플레이어에 다시 모으기: `GameplayWorldSubsystem::RefreshInputContext(World&, AssetManager&, AssetId, bool forceImmediately = false)`. UE `UEnhancedInputLibrary::RequestRebuildControlMappingsUsingContext`(`EI/Private/EnhancedInputLibrary.cpp:55-67`) 대응이다.
  - `Controllers()` 중 그 에셋을 쥔 PlayerController마다 `RefreshInputContext`를 부른다. 모두 시도하고 첫 실패를 돌려준다. 파괴 대기 Controller와 PlayerController가 아닌 Controller는 건너뛴다.
  - 호출자는 World 게임 스레드에서 부른다(PlayerController를 `World::With`로 읽고, 그 안에서 2.8절 도우미가 `AssetManager::With`를 중첩해 연다).
  - 쥔 플레이어가 없으면 성공이다(UE와 같다).
  - 실행 중 데이터 편집(키 바꾸기 — UE 에디터·런타임에서 매핑 객체를 고친 뒤 이 함수를 부르는 흐름)에 쓴다. `notifyUserSettings`로 등록된 컨텍스트면 사용자 설정의 등록 사본도 바뀐다(`UpdateContext`가 한다 — 행은 다시 매기지 않는다, 5.2절).
