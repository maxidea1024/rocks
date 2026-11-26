#include <Base/InitializerList.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <Input/InputEvent.h>
#include <Input/InputState.h>
#include <InputActions/ActionEventData.generated.h>
#include <InputActions/InputActionBindings.h>
#include <InputActions/InputValue.generated.h>
#include <InputActions/PlayerActionInput.h>
#include <InputActionsTestFixtures/BindingTarget.generated.h>
#include <InputActionsTestFixtures/BindingTarget.h>
#include <InputActionsTestFixtures/InputActionsTestFixtures.module.generated.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>
#include <string.h>

// 입력 함수 바인딩 시험이다(Docs/Design/InputActions 5.1절 — IA7): 리플렉션 함수 호출과 받은
// 데이터(이벤트 마스크 적용), 콜백과 섞인 consumes, 바인딩 거부(없음·다른 인자·반환·static·None
// 마스크·null·회수된 대상), 파괴 대기 대상 건너뛰기(경고 없음·consumes는 건다)와 사라진 대상의
// 바인딩 정리, 다른 스레드 전달의 Warning 한 번, Remove·Clear.
using namespace rocks;
using namespace rocks::input;
using rocks::input_fixture::BindingTarget;
namespace input_function_binding_tests_file {

namespace {

constexpr uint64 IdHigh = 0x49414637;
constexpr uint64 Jump = 1;
constexpr uint64 ContextLow = 100;
constexpr uint64 RegistryContextHigh = 0x494137ull;
constexpr double Step = 0.1;

Guid Id(uint64 low) noexcept {
    return Guid{IdHigh, low};
}

// 함수 바인딩 실패 경고 수를 센다.
class FailureCounter final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        const char needle[] = "(InputFunctionBindingFailed)";
        const usize bytes = sizeof(needle) - 1;
        for (usize start = 0; start + bytes <= event.message.ByteCount(); ++start) {
            if (memcmp(event.message.Data() + start, needle, bytes) == 0) {
                ++failures;
                break;
            }
        }

        return Result<void>::Success();
    }

    uint32 failures = 0;
};

// 경고를 세는 수동 로그 시스템이다(Pump가 sink에 보낸다).
struct LogRig {
    LogRig() noexcept : logs(allocator, Config()) {
        started = logs.Start().IsOk() && logs.AddSink(counter).IsOk();
    }

    static LogSystemConfig Config() noexcept {
        LogSystemConfig config;
        config.mode = LogMode::Manual;
        config.globalMinimum = LogLevel::Debug;
        return config;
    }

    uint32 Failures() noexcept {
        (void)logs.Pump();
        return counter.failures;
    }

    FailureCounter counter;
    SystemAllocator allocator;
    LogSystem logs;
    bool started = false;
};

// 평가기·바인딩과 함수 바인딩 대상 Registry다.
struct Rig {
    Rig() noexcept
        : types(allocator), modules(types, allocator), input(allocator), bindings(allocator),
          bundle(allocator) {
        registered = generated::InputActionsTestFixtures::RegisterModuleTypes(modules).IsOk();
        bundle.id = Id(ContextLow);
        InputActionData action(allocator);
        action.id = Id(Jump);
        InputBinding binding(allocator);
        binding.action = Id(Jump);
        registered = registered && bundle.actions.TryPushBack(Move(action)).IsOk() &&
                     binding.key.TryAssign("SpaceBar").IsOk() &&
                     bundle.data.bindings.TryPushBack(Move(binding)).IsOk();
    }

    ~Rig() {
        target = RootRef<BindingTarget>();
        if (registry) {
            CHECK(registry->Collect(registry->OwnerAccess()).IsOk());
            CHECK(registry->Shutdown(registry->OwnerAccess()).IsOk());
        }

        registry.Reset();
        CHECK(modules.Unregister().IsOk());
    }

    bool Start() noexcept {
        if (!registered) {
            return false;
        }

        auto made = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                       ContextId{RegistryContextHigh, 1});
        if (!made) {
            return false;
        }

        registry = Move(made.Value());
        auto created = registry->NewObject<BindingTarget>(Access(), TypeIdOf<BindingTarget>());
        if (!created) {
            return false;
        }

        target = Move(created.Value());
        return input.AddContext(Move(bundle), 0).IsOk() && Frame(false);
    }

    ObjectAccessContext Access() const noexcept {
        return registry->OwnerAccess();
    }

    Guid FunctionId(const char* name) noexcept {
        auto function = types.FindFunctionByName(TypeIdOf<BindingTarget>(),
                                                 Span<const char>(name, strlen(name)));
        return function ? function.Value().Schema().id : Guid{};
    }

    Result<ActionBindingHandle> BindFunction(const char* name, ActionEvent events,
                                             bool consumes = false) noexcept {
        return bindings.BindFunction(Id(Jump), events, *registry, Access(), target.Handle(),
                                     FunctionId(name), consumes);
    }

    // space를 누르거나(down) 놓고 한 번 평가·전달한다.
    bool Frame(bool down) noexcept {
        const bool evaluated = Evaluate(down);
        bindings.Dispatch(input);
        input.FinishDispatch();
        return evaluated;
    }

    // 평가만 한다(전달은 호출자 — 다른 스레드 전달 시험).
    bool Evaluate(bool down) noexcept {
        const InputEvent pressed[] = {down ? InputEvent::KeyDown(Key::Space)
                                           : InputEvent::KeyUp(Key::Space)};
        state.BeginFrame(Span<const InputEvent>(pressed, 1));
        return input.Evaluate(state, ActionEvalContext{Step, 1.0, false, 0.0f, false, 0}).IsOk();
    }

    uint32 Calls() noexcept {
        auto borrowed = registry->Resolve<BindingTarget>(Access(), target.Handle());
        if (!borrowed) {
            return 0;
        }

        auto pointer = borrowed.Value().Get();
        return pointer ? pointer.Value()->calls : 0;
    }

    ActionEventData Last() noexcept {
        auto borrowed = registry->Resolve<BindingTarget>(Access(), target.Handle());
        auto pointer = borrowed ? borrowed.Value().Get()
                                : Result<BindingTarget*>::Failure(ErrorCode::StaleHandle);
        return pointer ? pointer.Value()->last : ActionEventData{};
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    UniquePtr<ObjectRegistry> registry;
    RootRef<BindingTarget> target;
    InputState state;
    PlayerActionInput input;
    InputActionBindings bindings;
    InputContextBundle bundle;
    bool registered = false;
};

void DispatchOnOtherThread(void* context) noexcept {
    auto* rig = static_cast<Rig*>(context);
    rig->bindings.Dispatch(rig->input);
}

} // namespace

// 함수는 마스크와 겹친 이벤트로 불리고 데이터(액션·값)를 받는다. 콜백과 섞이면 consumes가 같은
// 액션의 뒤 바인딩을 막는다.
TEST(InputFunctionBinding_CallsHandlerWithMaskedData) {
    Rig rig;
    REQUIRE(rig.Start());
    REQUIRE(rig.BindFunction("OnAction", ActionEvent::Started, true));
    uint32 callbacks = 0;
    auto callback = InputActionBindings::Callback::TryCreate(
        rig.allocator, [&callbacks](const ActionEventData&) noexcept { ++callbacks; });
    REQUIRE(callback);
    REQUIRE(rig.bindings.BindAction(Id(Jump), ActionEvent::Started, Move(callback.Value())));
    CHECK(rig.bindings.BindingCount() == 2);

    REQUIRE(rig.Frame(true));
    CHECK(rig.Calls() == 1);
    const ActionEventData last = rig.Last();
    CHECK(last.action == Id(Jump));
    CHECK(last.events == ActionEvent::Started);
    CHECK(last.value.AsBool());
    // Started 바인딩은 등록 역순으로 앞에 끼워진다 — 콜백이 먼저 불리고, 함수 바인딩의 consumes는
    // 그 뒤에 걸린다(앞선 콜백은 막지 못한다).
    CHECK(callbacks == 1);

    // 누르고 있는 동안 Started는 다시 나지 않는다.
    REQUIRE(rig.Frame(true));
    CHECK(rig.Calls() == 1);
}

// consumes인 함수 바인딩이 먼저 실행되면 같은 액션의 뒤 콜백을 막는다(실행 실패여도 소비는 건다).
TEST(InputFunctionBinding_ConsumesLaterBindings) {
    Rig rig;
    REQUIRE(rig.Start());
    uint32 callbacks = 0;
    auto callback = InputActionBindings::Callback::TryCreate(
        rig.allocator, [&callbacks](const ActionEventData&) noexcept { ++callbacks; });
    REQUIRE(callback);
    REQUIRE(rig.bindings.BindAction(Id(Jump), ActionEvent::Triggered, Move(callback.Value())));
    // Started 바인딩은 실행 목록 앞으로 가므로 Triggered 콜백보다 먼저다.
    REQUIRE(rig.BindFunction("OnAction", ActionEvent::Started, true));
    REQUIRE(rig.Frame(true));
    CHECK(rig.Calls() == 1);
    CHECK(callbacks == 0);
}

// 바인딩 거부: 함수 없음 NotFound, 다른 인자·반환 TypeMismatch, static InvalidArgument, None 마스크
// InvalidArgument, null 대상은 대상 해석 오류. 거부하면 바인딩이 늘지 않는다.
TEST(InputFunctionBinding_RejectsMismatches) {
    Rig rig;
    REQUIRE(rig.Start());
    CHECK(rig.bindings
              .BindFunction(Id(Jump), ActionEvent::Started, *rig.registry, rig.Access(),
                            rig.target.Handle(), Guid{IdHigh, 999})
              .Error() == ErrorCode::NotFound);
    CHECK(rig.BindFunction("OnNumber", ActionEvent::Started).Error() == ErrorCode::TypeMismatch);
    CHECK(rig.BindFunction("OnActionCounted", ActionEvent::Started).Error() ==
          ErrorCode::TypeMismatch);
    CHECK(rig.BindFunction("OnStaticAction", ActionEvent::Started).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(rig.BindFunction("OnAction", ActionEvent::None).Error() == ErrorCode::InvalidArgument);
    CHECK(!rig.bindings.BindFunction(Id(Jump), ActionEvent::Started, *rig.registry, rig.Access(),
                                     ObjectHandle{}, rig.FunctionId("OnAction")));

    // 회수된 대상은 StaleHandle이다.
    ObjectHandle gone;
    {
        auto extra =
            rig.registry->NewObject<BindingTarget>(rig.Access(), TypeIdOf<BindingTarget>());
        REQUIRE(extra);
        gone = extra.Value().Handle();
    }

    REQUIRE(rig.registry->Collect(rig.Access()));
    CHECK(rig.bindings
              .BindFunction(Id(Jump), ActionEvent::Started, *rig.registry, rig.Access(), gone,
                            rig.FunctionId("OnAction"))
              .Error() == ErrorCode::StaleHandle);
    CHECK(rig.bindings.BindingCount() == 0);
}

// 파괴 대기 대상은 건너뛰고 바인딩을 남긴다. 회수돼 사라지면(StaleHandle) 그 전달 뒤 바인딩을
// 지운다. RemoveBinding도 함수 바인딩을 지운다.
TEST(InputFunctionBinding_GoneTargetsAndRemove) {
    Rig rig;
    REQUIRE(rig.Start());
    LogRig logs;
    REQUIRE(logs.started);
    LogInstallScope installed(logs.logs);
    auto removable = rig.BindFunction("OnAction", ActionEvent::Started);
    REQUIRE(removable);
    rig.bindings.RemoveBinding(removable.Value());
    CHECK(rig.bindings.BindingCount() == 0);
    REQUIRE(rig.BindFunction("OnAction", ActionEvent::Started));
    rig.bindings.ClearBindings();
    CHECK(rig.bindings.BindingCount() == 0);

    REQUIRE(rig.BindFunction("OnAction", ActionEvent::Started));
    REQUIRE(rig.Frame(true));
    CHECK(rig.Calls() == 1);
    REQUIRE(rig.Frame(false));

    // 파괴 대기: 건너뛰고 바인딩은 남는다.
    const ObjectHandle handle = rig.target.Handle();
    REQUIRE(rig.registry->RequestDestroy(rig.Access(), handle));
    REQUIRE(rig.Frame(true));
    CHECK(rig.bindings.BindingCount() == 1);
    // 파괴 대기는 조용히 건너뛴다(경고 없음).
    CHECK(logs.Failures() == 0);
    REQUIRE(rig.Frame(false));

    // 회수: StaleHandle이라 전달 뒤 지운다.
    rig.target = RootRef<BindingTarget>();
    REQUIRE(rig.registry->Collect(rig.Access()));
    REQUIRE(rig.Frame(true));
    CHECK(rig.bindings.BindingCount() == 0);
}

// 호출이 실패해도(파괴 대기 대상) 그 전달의 consumes는 걸어 뒤 콜백을 막는다.
TEST(InputFunctionBinding_FailedCallStillConsumes) {
    Rig rig;
    REQUIRE(rig.Start());
    uint32 callbacks = 0;
    auto callback = InputActionBindings::Callback::TryCreate(
        rig.allocator, [&callbacks](const ActionEventData&) noexcept { ++callbacks; });
    REQUIRE(callback);
    REQUIRE(rig.bindings.BindAction(Id(Jump), ActionEvent::Triggered, Move(callback.Value())));
    REQUIRE(rig.BindFunction("OnAction", ActionEvent::Started, true));
    REQUIRE(rig.registry->RequestDestroy(rig.Access(), rig.target.Handle()));
    REQUIRE(rig.Frame(true));
    CHECK(callbacks == 0);
    CHECK(rig.bindings.BindingCount() == 2);
}

// 다른 스레드에서 전달하면 WrongThread로 호출을 건너뛰고 바인딩마다 Warning을 한 번만 남긴다
// (바인딩은 남는다). 스레드는 만들고 Join한다(결정적).
TEST(InputFunctionBinding_OtherThreadWarnsOnce) {
    Rig rig;
    REQUIRE(rig.Start());
    LogRig logs;
    REQUIRE(logs.started);
    LogInstallScope installed(logs.logs);
    REQUIRE(rig.BindFunction("OnAction", ActionEvent::Started | ActionEvent::Triggered));
    for (uint32 round = 0; round < 2; ++round) {
        REQUIRE(rig.Evaluate(round == 0));
        platform::Thread thread;
        REQUIRE(thread.Start(DispatchOnOtherThread, &rig) == platform::ThreadStatus::Success);
        REQUIRE(thread.Join() == platform::ThreadStatus::Success);
        rig.input.FinishDispatch();
    }

    CHECK(rig.Calls() == 0);
    CHECK(rig.bindings.BindingCount() == 1);
    CHECK(logs.Failures() == 1);
}

} // namespace input_function_binding_tests_file
