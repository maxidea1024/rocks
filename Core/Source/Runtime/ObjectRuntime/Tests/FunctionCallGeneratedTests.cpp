#include <ReflectionTestFixtures/FunctionFixture.generated.h>
#include <Base/Compiler.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/FunctionCall.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 메타 컴파일러가 만든 리플렉션 함수표의 등록·조회·호출
// 시험이다(Docs/Design/FunctionReflection.ko.md FR1 — fixture FunctionFixture.h). 생성
// frame·thunk·실행 schema를 GeneratedType<T>::Register로 등록하고 FunctionCall로 부른다:
// 인스턴스·const·static 함수, Result<void>·Result<T>와 함수 자신의 실패,
// StringView·enum·struct·Array·ObjectHandle·WeakObjectRef 매개변수, 도구 이름, private 함수,
// EditorOnly 함수, 대상 해석 오류.
using namespace rocks;
using reflection_fixture::CallMode;
using reflection_fixture::CallPoint;
using reflection_fixture::CallTarget;

namespace function_call_generated_tests_file {

namespace {

Span<const char> Text(const char* text) noexcept {
    return Span<const char>(text, StringView(text).ByteCount());
}

StringView View(Span<const char> text) noexcept {
    return StringView(text.Data(), text.Size());
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

constexpr uint32 Flags(FunctionFlags flag) noexcept {
    return static_cast<uint32>(flag);
}

// 시험 객체를 읽는다(Registry 해석 — 실패하면 null).
const CallTarget* Read(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    auto borrow = registry.Resolve<CallTarget>(registry.OwnerAccess(), handle);
    if (!borrow) {
        return nullptr;
    }

    auto target = borrow.Value().Get();
    return target ? target.Value() : nullptr;
}

// 이름(도구 이름, 대소문자 무시)으로 함수를 찾는다.
Result<FunctionHandle> Find(TypeRegistry& types, const char* name) noexcept {
    return types.FindFunctionByName(TypeIdOf<CallTarget>(), Text(name));
}

} // namespace

TEST(FunctionCall_GeneratedTablesRegisterAndCall) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(GeneratedType<CallTarget>::Register(types));
        ObjectRegistry registry(allocator, types, {0x46434701, 1});
        const auto access = registry.OwnerAccess();
        auto created = registry.NewObject<CallTarget>(access, TypeIdOf<CallTarget>(), {}, {});
        REQUIRE(created);
        const ObjectHandle self = created.Value().Handle();

        // 실행 schema: 선언 순서, flag, 도구 이름 조회(대소문자 무시 — C++ 이름 SetLabel은
        // Rename으로 찾는다), EditorOnly 함수는 에디터 조합에만 있다.
        auto type = types.Acquire(TypeIdOf<CallTarget>());
        REQUIRE(type);
        const Span<const FunctionSchema> functions = type.Value().Schema().functions;
#if ROCKS_WITH_EDITOR
        REQUIRE(functions.Size() == 8);
        CHECK(View(functions[6].name) == StringView("Reset"));
        CHECK(functions[6].flags == Flags(FunctionFlags::EditorOnly));
#else
        REQUIRE(functions.Size() == 7);
#endif
        CHECK(View(functions[0].name) == StringView("Add"));
        CHECK(functions[1].flags == (Flags(FunctionFlags::Const) | Flags(FunctionFlags::Pure)));
        CHECK(View(functions[2].toolName) == StringView("Rename"));
        CHECK(functions[2].flags == Flags(FunctionFlags::ReturnsResult));
        CHECK(functions[4].flags == Flags(FunctionFlags::Static));
        CHECK(functions[3].parameters.Size() == 3);
        CHECK(functions[3].parameters[1].value.kind == ValueKind::Enum);
        CHECK(types.FindFunctionByName(TypeIdOf<CallTarget>(), Text("rename")));
        CHECK(!types.FindFunctionByName(TypeIdOf<CallTarget>(), Text("SetLabel")));
        CHECK(types.FindFunction(TypeIdOf<CallTarget>(), functions[0].id));

        {
            // 인스턴스 함수. Invoke는 같은 frame으로 다시 부를 수 있다(인자가 남는다).
            auto addFunction = Find(types, "Add");
            REQUIRE(addFunction);
            FunctionCall add(allocator, Move(addFunction.Value()));
            REQUIRE(add.Prepare());
            REQUIRE(add.SetArgument(registry, access, 0, IntValue(allocator, 5)));
            REQUIRE(add.Invoke(registry, access, self));
            REQUIRE(add.Invoke(registry, access, self));
            CHECK(Read(registry, self)->total == 10);

            // const(Pure) 함수는 읽기로 해석한다.
            auto totalFunction = Find(types, "Total");
            REQUIRE(totalFunction);
            FunctionCall total(allocator, Move(totalFunction.Value()));
            REQUIRE(total.Prepare());
            REQUIRE(total.Invoke(registry, access, self));
            FieldValue result(allocator);
            REQUIRE(total.ReadResult(result));
            CHECK(result.kind == ValueKind::Int32 && result.signedInteger == 10);
            const auto* typed =
                static_cast<const int32*>(total.ResultValue(NativeTypeKey<int32>()));
            REQUIRE(typed != nullptr);
            CHECK(*typed == 10);
        }

        {
            // Result<void> 함수의 실패는 호출 성공 + FunctionError다. StringView 인자는 String으로
            // 담는다.
            auto renameFunction = Find(types, "Rename");
            REQUIRE(renameFunction);
            FunctionCall rename(allocator, Move(renameFunction.Value()));
            REQUIRE(rename.Prepare());
            REQUIRE(rename.Invoke(registry, access, self));
            CHECK(rename.FunctionError() == ErrorCode::InvalidArgument);
            FieldValue text(allocator);
            text.kind = ValueKind::Utf8String;
            REQUIRE(text.string.TryAssign("north"));
            REQUIRE(rename.SetArgument(registry, access, 0, text));
            REQUIRE(rename.Invoke(registry, access, self));
            CHECK(rename.FunctionError() == ErrorCode::None);
            CHECK(Read(registry, self)->label.View() == StringView("north"));
        }

        {
            // Result<int32> 함수에 Array·enum·struct 인자(같은 C++ 형식의 값 복사).
            auto sumFunction = Find(types, "Sum");
            REQUIRE(sumFunction);
            FunctionCall sum(allocator, Move(sumFunction.Value()));
            REQUIRE(sum.Prepare());
            Array<int32> values(allocator);
            REQUIRE(values.TryPushBack(1));
            REQUIRE(values.TryPushBack(2));
            REQUIRE(sum.SetArgumentValue(0, &values, NativeTypeKey<Array<int32>>()));
            FieldValue mode(allocator);
            mode.kind = ValueKind::Enum;
            mode.type = GeneratedType<CallMode>::Id();
            mode.signedInteger = static_cast<int64>(CallMode::Doubled);
            REQUIRE(sum.SetArgument(registry, access, 1, mode));
            CallPoint point;
            point.x = 3;
            point.y = 4;
            REQUIRE(sum.SetArgumentValue(2, &point, NativeTypeKey<CallPoint>()));
            CHECK(CallError(sum.SetArgumentValue(2, &point, NativeTypeKey<int32>())) ==
                  ErrorCode::TypeMismatch);
            REQUIRE(sum.Invoke(registry, access, self));
            CHECK(sum.FunctionError() == ErrorCode::None);
            FieldValue result(allocator);
            REQUIRE(sum.ReadResult(result));
            CHECK(result.signedInteger == 20);

            // 함수 자신의 실패(음수 합)는 FunctionError다.
            point.x = -100;
            REQUIRE(sum.SetArgumentValue(2, &point, NativeTypeKey<CallPoint>()));
            REQUIRE(sum.Invoke(registry, access, self));
            CHECK(sum.FunctionError() == ErrorCode::Overflow);
        }

        {
            // static 함수는 대상 없이 부르고 접근 문맥을 확인한다.
            auto twiceFunction = Find(types, "Twice");
            REQUIRE(twiceFunction);
            FunctionCall twice(allocator, Move(twiceFunction.Value()));
            REQUIRE(twice.Prepare());
            REQUIRE(twice.SetArgument(registry, access, 0, IntValue(allocator, 21)));
            REQUIRE(twice.Invoke(registry, access, ObjectHandle{}));
            FieldValue result(allocator);
            REQUIRE(twice.ReadResult(result));
            CHECK(result.signedInteger == 42);
            CHECK(CallError(twice.Invoke(registry, ObjectAccessContext{{0x9999, 1}}, {})) ==
                  ErrorCode::WrongContext);
        }

        {
            // ObjectHandle·WeakObjectRef 인자는 이 Registry의 생존 객체여야 한다.
            auto sameFunction = Find(types, "Same");
            REQUIRE(sameFunction);
            FunctionCall same(allocator, Move(sameFunction.Value()));
            REQUIRE(same.Prepare());
            FieldValue reference(allocator);
            reference.kind = ValueKind::WeakObjectReference;
            reference.reference = self;
            REQUIRE(same.SetArgument(registry, access, 0, reference));
            REQUIRE(same.SetArgument(registry, access, 1, reference));
            REQUIRE(same.Invoke(registry, access, self));
            FieldValue result(allocator);
            REQUIRE(same.ReadResult(result));
            CHECK(result.kind == ValueKind::Bool && result.boolean);
            ++reference.reference.generation;
            CHECK(CallError(same.SetArgument(registry, access, 1, reference)) ==
                  ErrorCode::StaleHandle);
        }

        {
            // private 함수도 생성 thunk(GENERATED_BODY friend)로 부른다.
            auto bumpFunction = Find(types, "Bump");
            REQUIRE(bumpFunction);
            FunctionCall bump(allocator, Move(bumpFunction.Value()));
            REQUIRE(bump.Prepare());
            REQUIRE(bump.Invoke(registry, access, self));
            CHECK(Read(registry, self)->total == 110);
#if ROCKS_WITH_EDITOR
            auto resetFunction = Find(types, "Reset");
            REQUIRE(resetFunction);
            FunctionCall reset(allocator, Move(resetFunction.Value()));
            REQUIRE(reset.Prepare());
            REQUIRE(reset.Invoke(registry, access, self));
            CHECK(Read(registry, self)->total == 0);
#endif

            // 대상 해석 오류는 Registry 오류 그대로다.
            CHECK(CallError(bump.Invoke(registry, ObjectAccessContext{{0x9999, 1}}, self)) ==
                  ErrorCode::WrongContext);
            REQUIRE(registry.RequestDestroy(access, self));
            CHECK(CallError(bump.Invoke(registry, access, self)) == ErrorCode::Destroyed);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace function_call_generated_tests_file
