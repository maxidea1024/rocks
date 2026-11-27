#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Object/FunctionCall.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 리플렉션 함수의 등록·조회·범용 호출 시험이다(Docs/Design/FunctionReflection.ko.md FD7–FD10 —
// FR1). 함수표는 손으로 만든다(생성 코드가 만드는 것과 같은 모양 — frame
// struct·construct·destroy·invoke· 매개변수 연산). 메타 컴파일러가 만드는 함수표는 meta 시험과
// 리플렉션 fixture 시험이 본다.
using namespace rocks;
namespace function_call_tests_file {

namespace {

constexpr TypeId CounterType{0x46434e5452303031ULL, 1};
constexpr TypeId OtherType{0x46434f5448303031ULL, 1};
constexpr TypeId DerivedType{0x4643445256303031ULL, 1};
constexpr Guid AddId{0x4643464e41444431ULL, 1};
constexpr Guid ValueId{0x4643464e56414c31ULL, 1};
constexpr Guid TwiceId{0x4643464e54574931ULL, 1};
constexpr Guid RenameId{0x4643464e52454e31ULL, 1};
constexpr Guid LinkId{0x4643464e4c4e4b31ULL, 1};
constexpr Guid DerivedId{0x4643464e44525631ULL, 1};

// Add가 넘침을 알리는 경계다.
constexpr int32 CounterLimit = 1000;

struct Counter : Object {
    explicit Counter(Allocator& allocator) noexcept : label(allocator) {
    }

    int32 value = 0;
    String label;
    ObjectHandle peer;

    Result<void> Add(int32 amount) noexcept {
        if (value + amount > CounterLimit) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        value += amount;
        return Result<void>::Success();
    }

    int32 Value() const noexcept {
        return value;
    }

    static int32 Twice(int32 input) noexcept {
        return input * 2;
    }

    Result<void> Rename(StringView text) noexcept {
        return label.TryAssign(text);
    }

    void Link(ObjectHandle other) noexcept {
        peer = other;
    }
};

struct Other final : Object {};

struct Derived final : Counter {
    using Counter::Counter;
};

// 생성 코드와 같은 frame들이다(매개변수 → 결과 → 실패 코드 순).
struct AddFrame {
    int32 amount = 0;
    ErrorCode error = ErrorCode::None;
};

struct ValueFrame {
    int32 result = 0;
};

struct TwiceFrame {
    int32 input = 0;
    int32 result = 0;
};

struct RenameFrame {
    explicit RenameFrame(Allocator& allocator) noexcept : text(allocator) {
    }

    String text;
    ErrorCode error = ErrorCode::None;
};

struct LinkFrame {
    ObjectHandle other;
};

template <class Frame>
Result<void> ConstructFrame(void* frame, Allocator& allocator) noexcept {
    if constexpr (IsConstructibleV<Frame, Allocator&>) {
        ::new (frame) Frame(allocator);
    } else {
        ::new (frame) Frame();
    }

    return Result<void>::Success();
}

template <class Frame>
void DestroyFrame(void* frame) noexcept {
    static_cast<Frame*>(frame)->~Frame();
}

Result<void> InvokeAdd(ObjectRegistry& registry, const ObjectAccessContext& access,
                       ObjectHandle self, void* frame) noexcept {
    auto borrow = registry.ResolveForWrite<Counter>(access, self);
    if (!borrow) {
        return Result<void>::Failure(borrow.Error());
    }

    auto target = borrow.Value().Get();
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    auto& f = *static_cast<AddFrame*>(frame);
    auto done = target.Value()->Add(f.amount);
    f.error = done ? ErrorCode::None : done.Error();
    return Result<void>::Success();
}

Result<void> InvokeValue(ObjectRegistry& registry, const ObjectAccessContext& access,
                         ObjectHandle self, void* frame) noexcept {
    auto borrow = registry.Resolve<Counter>(access, self);
    if (!borrow) {
        return Result<void>::Failure(borrow.Error());
    }

    auto target = borrow.Value().Get();
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    static_cast<ValueFrame*>(frame)->result = target.Value()->Value();
    return Result<void>::Success();
}

Result<void> InvokeTwice(ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle,
                         void* frame) noexcept {
    if (auto owned = registry.VerifyAccess(access); !owned) {
        return owned;
    }

    auto& f = *static_cast<TwiceFrame*>(frame);
    f.result = Counter::Twice(f.input);
    return Result<void>::Success();
}

Result<void> InvokeRename(ObjectRegistry& registry, const ObjectAccessContext& access,
                          ObjectHandle self, void* frame) noexcept {
    auto borrow = registry.ResolveForWrite<Counter>(access, self);
    if (!borrow) {
        return Result<void>::Failure(borrow.Error());
    }

    auto target = borrow.Value().Get();
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    auto& f = *static_cast<RenameFrame*>(frame);
    auto done = target.Value()->Rename(f.text.View());
    f.error = done ? ErrorCode::None : done.Error();
    return Result<void>::Success();
}

Result<void> InvokeLink(ObjectRegistry& registry, const ObjectAccessContext& access,
                        ObjectHandle self, void* frame) noexcept {
    auto borrow = registry.ResolveForWrite<Counter>(access, self);
    if (!borrow) {
        return Result<void>::Failure(borrow.Error());
    }

    auto target = borrow.Value().Get();
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    target.Value()->Link(static_cast<LinkFrame*>(frame)->other);
    return Result<void>::Success();
}

constexpr uint32 Flag(FunctionFlags flag) noexcept {
    return static_cast<uint32>(flag);
}

ParameterSchema Slot(const char* name, ValueKind kind, usize offset) noexcept {
    ParameterSchema slot;
    slot.name = Span<const char>(name, StringView(name).ByteCount());
    slot.value.kind = kind;
    slot.offset = static_cast<uint32>(offset);
    return slot;
}

Span<const char> Text(const char* text) noexcept {
    return Span<const char>(text, StringView(text).ByteCount());
}

// Counter의 함수 다섯이다. 표는 수정 시험이 고칠 수 있게 값으로 만든다.
struct CounterFunctions {
    ParameterSchema addParameters[1] = {
        Slot("amount", ValueKind::Int32, offsetof(AddFrame, amount))};
    ParameterSchema twiceParameters[1] = {
        Slot("input", ValueKind::Int32, offsetof(TwiceFrame, input))};
    ParameterSchema renameParameters[1] = {
        Slot("text", ValueKind::Utf8String, offsetof(RenameFrame, text))};
    ParameterSchema linkParameters[1] = {
        Slot("other", ValueKind::ObjectReference, offsetof(LinkFrame, other))};
    RuntimeParameterOps intOps[1] = {detail::ParameterOpsOf<int32>()};
    RuntimeParameterOps stringOps[1] = {detail::ParameterOpsOf<String>()};
    RuntimeParameterOps handleOps[1] = {detail::ParameterOpsOf<ObjectHandle>()};
    FunctionSchema schemas[5];
    RuntimeFunctionOps ops[5];

    CounterFunctions() noexcept {
        linkParameters[0].value.kind = ValueKind::WeakObjectReference;
        schemas[0] = {AddId,
                      Text("Add"),
                      {},
                      Flag(FunctionFlags::ReturnsResult),
                      Span<const ParameterSchema>(addParameters, 1),
                      {},
                      sizeof(AddFrame),
                      alignof(AddFrame)};
        ops[0].functionId = AddId;
        ops[0].construct = &ConstructFrame<AddFrame>;
        ops[0].destroy = &DestroyFrame<AddFrame>;
        ops[0].invoke = &InvokeAdd;
        ops[0].parameters = Span<const RuntimeParameterOps>(intOps, 1);
        ops[0].errorOffset = offsetof(AddFrame, error);

        schemas[1] = {ValueId,
                      Text("Value"),
                      {},
                      Flag(FunctionFlags::Const) | Flag(FunctionFlags::Pure),
                      {},
                      Slot("", ValueKind::Int32, offsetof(ValueFrame, result)),
                      sizeof(ValueFrame),
                      alignof(ValueFrame)};
        schemas[1].result.name = {};
        ops[1].functionId = ValueId;
        ops[1].construct = &ConstructFrame<ValueFrame>;
        ops[1].destroy = &DestroyFrame<ValueFrame>;
        ops[1].invoke = &InvokeValue;
        ops[1].result = detail::ParameterOpsOf<int32>();

        schemas[2] = {TwiceId,
                      Text("Twice"),
                      Text("Double"),
                      Flag(FunctionFlags::Static),
                      Span<const ParameterSchema>(twiceParameters, 1),
                      Slot("", ValueKind::Int32, offsetof(TwiceFrame, result)),
                      sizeof(TwiceFrame),
                      alignof(TwiceFrame)};
        schemas[2].result.name = {};
        ops[2].functionId = TwiceId;
        ops[2].construct = &ConstructFrame<TwiceFrame>;
        ops[2].destroy = &DestroyFrame<TwiceFrame>;
        ops[2].invoke = &InvokeTwice;
        ops[2].parameters = Span<const RuntimeParameterOps>(intOps, 1);
        ops[2].result = detail::ParameterOpsOf<int32>();

        schemas[3] = {RenameId,
                      Text("Rename"),
                      {},
                      Flag(FunctionFlags::ReturnsResult),
                      Span<const ParameterSchema>(renameParameters, 1),
                      {},
                      sizeof(RenameFrame),
                      alignof(RenameFrame)};
        ops[3].functionId = RenameId;
        ops[3].construct = &ConstructFrame<RenameFrame>;
        ops[3].destroy = &DestroyFrame<RenameFrame>;
        ops[3].invoke = &InvokeRename;
        ops[3].parameters = Span<const RuntimeParameterOps>(stringOps, 1);
        ops[3].errorOffset = offsetof(RenameFrame, error);

        schemas[4] = {LinkId,
                      Text("Link"),
                      {},
                      0,
                      Span<const ParameterSchema>(linkParameters, 1),
                      {},
                      sizeof(LinkFrame),
                      alignof(LinkFrame)};
        ops[4].functionId = LinkId;
        ops[4].construct = &ConstructFrame<LinkFrame>;
        ops[4].destroy = &DestroyFrame<LinkFrame>;
        ops[4].invoke = &InvokeLink;
        ops[4].parameters = Span<const RuntimeParameterOps>(handleOps, 1);
    }
};

Result<TypeRegistrationToken> RegisterCounter(TypeRegistry& types, CounterFunctions& functions,
                                              usize count = 5) noexcept {
    TypeSchema schema{CounterType, 1, {}, Text("Counter")};
    schema.functions = Span<const FunctionSchema>(functions.schemas, count);
    RuntimeTypeOps ops = MakeRuntimeTypeOps<Counter>();
    ops.functions = Span<const RuntimeFunctionOps>(functions.ops, count);
    return types.Register(schema, Move(ops));
}

FieldValue IntValue(Allocator& allocator, int32 value) noexcept {
    FieldValue field(allocator);
    field.kind = ValueKind::Int32;
    field.signedInteger = value;
    return field;
}

ErrorCode CallError(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 시험 객체 상태를 읽는다(Registry 해석 — 실패하면 표지 값).
constexpr int32 MissingValue = -1;

int32 ValueOf(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    auto borrow = registry.Resolve<Counter>(registry.OwnerAccess(), handle);
    if (!borrow) {
        return MissingValue;
    }

    auto target = borrow.Value().Get();
    return target ? target.Value()->value : MissingValue;
}

bool LabelIs(ObjectRegistry& registry, ObjectHandle handle, StringView expected) noexcept {
    auto borrow = registry.Resolve<Counter>(registry.OwnerAccess(), handle);
    if (!borrow) {
        return false;
    }

    auto target = borrow.Value().Get();
    return target && target.Value()->label.View() == expected;
}

ObjectHandle PeerOf(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    auto borrow = registry.Resolve<Counter>(registry.OwnerAccess(), handle);
    if (!borrow) {
        return {};
    }

    auto target = borrow.Value().Get();
    return target ? target.Value()->peer : ObjectHandle{};
}

struct Plain {
    int32 x = 0;
};

} // namespace

TEST(FunctionCall_ManualTableCallsResultsAndAccessErrors) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        CounterFunctions functions;
        REQUIRE(RegisterCounter(types, functions));
        REQUIRE(types.Register({OtherType, 1, {}, Text("Other")}, MakeRuntimeTypeOps<Other>()));
        ObjectRegistry registry(allocator, types, {0x46430001, 1});
        const auto access = registry.OwnerAccess();
        auto counter = registry.NewObject<Counter>(access, CounterType, {}, {});
        auto other = registry.NewObject<Other>(access, OtherType, {}, {});
        REQUIRE(counter && other);
        const ObjectHandle self = counter.Value().Handle();

        // 이름 조회는 대소문자 무시, 도구 이름이 있으면 그 이름이다.
        auto add = types.FindFunctionByName(CounterType, "add");
        REQUIRE(add);
        CHECK(add.Value().Schema().id == AddId);
        CHECK(!types.FindFunctionByName(CounterType, "Twice"));
        CHECK(types.FindFunctionByName(CounterType, "double"));
        CHECK(types.FindFunction(CounterType, ValueId));
        CHECK(types.FindFunction(CounterType, Guid{7, 7}).Error() == ErrorCode::NotFound);
        CHECK(types.FindFunction(TypeId{9, 9}, AddId).Error() == ErrorCode::NotFound);

        {
            FunctionCall call(allocator, Move(add.Value()));
            CHECK(CallError(call.Invoke(registry, access, self)) == ErrorCode::InvalidArgument);
            REQUIRE(call.Prepare());
            CHECK(CallError(call.Prepare()) == ErrorCode::InvalidArgument);
            REQUIRE(call.SetArgument(registry, access, 0, IntValue(allocator, 40)));
            REQUIRE(call.Invoke(registry, access, self));
            CHECK(call.FunctionError() == ErrorCode::None);
            CHECK(ValueOf(registry, self) == 40);

            // 함수 자신의 실패는 호출 성공 + FunctionError다.
            REQUIRE(call.SetArgument(registry, access, 0, IntValue(allocator, CounterLimit)));
            REQUIRE(call.Invoke(registry, access, self));
            CHECK(call.FunctionError() == ErrorCode::Overflow);
            CHECK(ValueOf(registry, self) == 40);

            // 인자 형식·범위와 대상 오류.
            FieldValue wrong(allocator);
            wrong.kind = ValueKind::Bool;
            CHECK(CallError(call.SetArgument(registry, access, 0, wrong)) ==
                  ErrorCode::TypeMismatch);
            CHECK(CallError(call.SetArgument(registry, access, 1, IntValue(allocator, 1))) ==
                  ErrorCode::InvalidArgument);
            CHECK(CallError(call.Invoke(registry, access, ObjectHandle{})) ==
                  ErrorCode::InvalidArgument);
            CHECK(CallError(call.Invoke(registry, access, other.Value().Handle())) ==
                  ErrorCode::TypeMismatch);
            CHECK(CallError(call.Invoke(registry, ObjectAccessContext{{0x9999, 1}}, self)) ==
                  ErrorCode::WrongContext);
        }

        {
            auto value = types.FindFunction(CounterType, ValueId);
            REQUIRE(value);
            FunctionCall call(allocator, Move(value.Value()));
            REQUIRE(call.Prepare());
            REQUIRE(call.Invoke(registry, access, self));
            FieldValue result(allocator);
            REQUIRE(call.ReadResult(result));
            CHECK(result.kind == ValueKind::Int32 && result.signedInteger == 40);
            CHECK(call.ResultValue<int32>() != nullptr);
            CHECK(call.ResultValue<int64>() == nullptr);
        }

        {
            // static 함수는 대상 없이 부르고 handle을 주면 InvalidArgument다.
            auto twice = types.FindFunctionByName(CounterType, "Double");
            REQUIRE(twice);
            FunctionCall call(allocator, Move(twice.Value()));
            REQUIRE(call.Prepare());
            REQUIRE(call.SetArgument(registry, access, 0, IntValue(allocator, 21)));
            CHECK(CallError(call.Invoke(registry, access, self)) == ErrorCode::InvalidArgument);
            REQUIRE(call.Invoke(registry, access, ObjectHandle{}));
            FieldValue result(allocator);
            REQUIRE(call.ReadResult(result));
            CHECK(result.signedInteger == 42);
            CHECK(CallError(call.Invoke(registry, ObjectAccessContext{{0x9999, 1}}, {})) ==
                  ErrorCode::WrongContext);
        }

        {
            auto rename = types.FindFunction(CounterType, RenameId);
            REQUIRE(rename);
            FunctionCall call(allocator, Move(rename.Value()));
            REQUIRE(call.Prepare());
            FieldValue text(allocator);
            text.kind = ValueKind::Utf8String;
            REQUIRE(text.string.TryAssign("north"));
            REQUIRE(call.SetArgument(registry, access, 0, text));
            REQUIRE(call.Invoke(registry, access, self));
            CHECK(LabelIs(registry, self, "north"));
            String copy(allocator);
            REQUIRE(copy.TryAssign("south"));
            REQUIRE(call.SetArgumentValue(0, copy));
            CHECK(CallError(call.SetArgumentValue(0, int32(1))) == ErrorCode::TypeMismatch);
            REQUIRE(call.Invoke(registry, access, self));
            CHECK(LabelIs(registry, self, "south"));
        }

        {
            // 참조 인자는 이 Registry의 생존 객체여야 한다.
            auto link = types.FindFunction(CounterType, LinkId);
            REQUIRE(link);
            FunctionCall call(allocator, Move(link.Value()));
            REQUIRE(call.Prepare());
            FieldValue reference(allocator);
            reference.kind = ValueKind::ObjectReference;
            reference.reference = other.Value().Handle();
            REQUIRE(call.SetArgument(registry, access, 0, reference));
            REQUIRE(call.Invoke(registry, access, self));
            CHECK(PeerOf(registry, self) == other.Value().Handle());
            reference.reference = other.Value().Handle();
            ++reference.reference.generation; // 같은 칸의 낡은 handle
            CHECK(CallError(call.SetArgument(registry, access, 0, reference)) ==
                  ErrorCode::StaleHandle);

            // 파괴 요청된 대상은 Registry 오류(Destroyed)다 — crash하지 않는다.
            REQUIRE(registry.RequestDestroy(access, self));
            CHECK(CallError(call.Invoke(registry, access, self)) == ErrorCode::Destroyed);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(FunctionCall_RegistrationRejectsBadTables) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        auto expect = [&](auto mutate, ErrorCode code) noexcept {
            CounterFunctions functions;
            mutate(functions);
            auto registered = RegisterCounter(types, functions);
            CHECK(!registered && registered.Error() == code);
            if (registered) {
                CHECK(types.Unregister(registered.Value()));
            }
        };

        // 같은 이름(대소문자 무시)·같은 ID는 AlreadyExists다.
        expect([](CounterFunctions& f) noexcept { f.schemas[1].name = Text("ADD"); },
               ErrorCode::AlreadyExists);
        expect(
            [](CounterFunctions& f) noexcept {
                f.schemas[1].id = AddId;
                f.ops[1].functionId = AddId;
            },
            ErrorCode::AlreadyExists);
        // 서술·연산 불일치와 규칙 위반은 InvalidArgument다.
        expect([](CounterFunctions& f) noexcept { f.ops[0].functionId = ValueId; },
               ErrorCode::InvalidArgument);
        expect(
            [](CounterFunctions& f) noexcept { f.schemas[3].flags |= Flag(FunctionFlags::Pure); },
            ErrorCode::InvalidArgument); // 반환 없는 Pure
        expect(
            [](CounterFunctions& f) noexcept { f.schemas[2].flags |= Flag(FunctionFlags::Const); },
            ErrorCode::InvalidArgument); // static + const
        expect([](CounterFunctions& f) noexcept { f.addParameters[0].offset = sizeof(AddFrame); },
               ErrorCode::InvalidArgument); // frame 밖
        expect([](CounterFunctions& f) noexcept { f.ops[0].invoke = nullptr; },
               ErrorCode::InvalidArgument);
        expect(
            [](CounterFunctions& f) noexcept {
                f.addParameters[0].value.kind = ValueKind::ObjectReference;
            },
            ErrorCode::InvalidArgument); // 강한 참조 칸
        expect([](CounterFunctions& f) noexcept { f.schemas[0].frameAlignment = 3; },
               ErrorCode::InvalidArgument);
        expect([](CounterFunctions& f) noexcept { f.ops[0].errorOffset = sizeof(AddFrame); },
               ErrorCode::InvalidArgument);
        expect([](CounterFunctions& f) noexcept { f.schemas[2].toolName = Text("bad name"); },
               ErrorCode::InvalidArgument);

        // struct 등록은 함수를 받지 않는다.
        {
            CounterFunctions functions;
            TypeSchema schema{TypeId{0x46435354ULL, 1}, 1, {}, Text("S")};
            schema.functions = Span<const FunctionSchema>(functions.schemas, 1);
            RuntimeStructOps ops = MakeRuntimeStructOps<Plain>();
            CHECK(types.RegisterStruct(schema, ops).Error() == ErrorCode::InvalidArgument);
        }

        // 기반 체인의 함수와 이름이 겹치면 AlreadyExists, 겹치지 않으면 파생에서 기반 함수를
        // 찾는다.
        {
            CounterFunctions functions;
            REQUIRE(RegisterCounter(types, functions));
            CounterFunctions derived;
            derived.schemas[0].id = DerivedId;
            derived.ops[0].functionId = DerivedId;
            TypeSchema schema{DerivedType, 1, {}, Text("Derived"), CounterType};
            schema.functions = Span<const FunctionSchema>(derived.schemas, 1);
            RuntimeTypeOps ops = MakeRuntimeTypeOps<Derived>();
            ops.nativeBaseType = NativeTypeKey<Counter>();
            ops.functions = Span<const RuntimeFunctionOps>(derived.ops, 1);
            CHECK(types.Register(schema, Move(ops)).Error() == ErrorCode::AlreadyExists);

            TypeSchema plain{DerivedType, 1, {}, Text("Derived"), CounterType};
            RuntimeTypeOps plainOps = MakeRuntimeTypeOps<Derived>();
            plainOps.nativeBaseType = NativeTypeKey<Counter>();
            REQUIRE(types.Register(plain, Move(plainOps)));
            auto inherited = types.FindFunctionByName(DerivedType, "Value");
            REQUIRE(inherited);
            CHECK(inherited.Value().Owner().Schema().id == CounterType);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(FunctionCall_HandleKeepsTheOwnerTypeAndFramesFreeOnFailure) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        CounterFunctions functions;
        auto token = RegisterCounter(types, functions);
        REQUIRE(token);
        {
            auto value = types.FindFunction(CounterType, ValueId);
            REQUIRE(value);
            // 호출 핸들이 선언 타입의 lease를 쥐는 동안 Unregister는 Busy다.
            CHECK(types.Unregister(token.Value()).Error() == ErrorCode::Busy);
        }

        CHECK(types.Unregister(token.Value()));
        REQUIRE(RegisterCounter(types, functions));
        ObjectRegistry registry(allocator, types, {0x46430002, 1});
        const auto access = registry.OwnerAccess();
        auto counter = registry.NewObject<Counter>(access, CounterType, {}, {});
        REQUIRE(counter);

        // frame 할당과 문자열 인자 쓰기의 할당 실패는 OutOfMemory이고 아무것도 남기지 않는다.
        for (uint64 attempt = 1; attempt <= 4; ++attempt) {
            test::NFailAllocator failing(allocator);
            failing.FailOnAttempt(attempt);
            auto rename = types.FindFunction(CounterType, RenameId);
            REQUIRE(rename);
            FunctionCall call(failing, Move(rename.Value()));
            auto prepared = call.Prepare();
            if (!prepared) {
                CHECK(prepared.Error() == ErrorCode::OutOfMemory);
                continue;
            }

            FieldValue text(allocator);
            text.kind = ValueKind::Utf8String;
            REQUIRE(text.string.TryAssign("a name long enough to allocate a buffer"));
            auto set = call.SetArgument(registry, access, 0, text);
            CHECK(set || set.Error() == ErrorCode::OutOfMemory);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace function_call_tests_file
