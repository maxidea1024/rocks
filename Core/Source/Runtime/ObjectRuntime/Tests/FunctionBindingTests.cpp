#include <ReflectionTestFixtures/FunctionFixture.generated.h>
#include <Base/Compiler.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/FunctionBinding.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 리플렉션 함수 바인딩 시험이다(Docs/Design/EventReflection.ko.md EV1a — ED3·ED5): 기대 frame
// 배치가 생성 frame과 같은지(offset·크기·칸 서술), 바인딩 시점 거부(함수
// 없음·static·반환·Result·형식·칸 수·ObjectHandle과 WeakObjectRef), 같은 frame으로 여러 바인딩
// 호출, 상속 함수, 다른 배치의 frame, 인자 할당 실패, 대상 생존·회수·문맥 오류.
using namespace rocks;
using reflection_fixture::CallMode;
using reflection_fixture::CallPoint;
using reflection_fixture::CallTarget;
using reflection_fixture::CallTargetChild;

namespace function_binding_tests_file {

namespace {

const FunctionSchema* FindSchema(TypeRegistry& types, const char* name) noexcept {
    auto type = types.Acquire(TypeIdOf<CallTarget>());
    if (!type) {
        return nullptr;
    }

    for (const FunctionSchema& function : type.Value().Schema().functions) {
        if (StringView(function.name.Data(), function.name.Size()) == StringView(name)) {
            return &function;
        }
    }

    return nullptr;
}

Guid IdOf(TypeRegistry& types, const char* name) noexcept {
    const FunctionSchema* schema = FindSchema(types, name);
    return schema != nullptr ? schema->id : Guid{};
}

ErrorCode BindError(const Result<FunctionBinding>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

ErrorCode CallError(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

bool SameType(const ValueType& first, const ValueType& second) noexcept {
    return first.kind == second.kind && first.type == second.type;
}

int32 TotalOf(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    auto borrow = registry.Resolve<CallTarget>(registry.OwnerAccess(), handle);
    if (!borrow) {
        return -1;
    }

    auto target = borrow.Value().Get();
    return target ? target.Value()->total : -1;
}

} // namespace

// 템플릿이 계산한 기대 배치가 생성 frame 배치·칸 서술(schema offset·값 형식·flag)과 같고 서명
// 비교가 받는다.
TEST(FunctionBinding_FrameLayoutMatchesGeneratedFrames) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(GeneratedType<CallTarget>::Register(types));
        struct Case {
            const char* name;
            FrameLayout layout;
        };
        const Case cases[] = {{"Add", detail::FrameLayoutOf<int32>()},
                              {"Bump", detail::FrameLayoutOf<>()},
                              {"Relay", detail::FrameLayoutOf<>()},
                              {"Remember", detail::FrameLayoutOf<WeakObjectRef<CallTarget>>()},
                              {"Note", detail::FrameLayoutOf<const String&>()},
                              {"Place", detail::FrameLayoutOf<const CallPoint&>()},
                              {"Choose", detail::FrameLayoutOf<CallMode>()},
                              {"Mark", detail::FrameLayoutOf<ObjectHandle>()}};
        for (const Case& item : cases) {
            const FunctionSchema* schema = FindSchema(types, item.name);
            REQUIRE(schema != nullptr);
            CHECK(schema->frameSize == item.layout.size);
            CHECK(schema->frameAlignment == item.layout.alignment);
            REQUIRE(schema->parameters.Size() == item.layout.count);
            for (uint32 index = 0; index < item.layout.count; ++index) {
                const ParameterSchema& parameter = schema->parameters[index];
                const FrameSlotLayout& slot = item.layout.slots[index];
                CHECK(parameter.offset == slot.offset);
                CHECK(SameType(parameter.value, slot.value));
                CHECK(SameType(parameter.element, slot.element));
                CHECK(SameType(parameter.key, slot.key));
                CHECK(parameter.flags == slot.flags);
            }

            auto found = types.FindFunction(TypeIdOf<CallTarget>(), schema->id);
            REQUIRE(found);
            CHECK(
                FunctionSignatureMatches(found.Value().Schema(), found.Value().Ops(), item.layout));
        }

        // ObjectHandle 칸은 형식 flag를 싣는다.
        CHECK(FindSchema(types, "Mark")->parameters[0].flags ==
              static_cast<uint32>(ParameterFlags::ObjectHandle));
    }
}

TEST(FunctionBinding_BindTimeChecks) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(GeneratedType<CallTarget>::Register(types));
        ObjectRegistry registry(allocator, types, {0x46424e44, 1});
        const auto access = registry.OwnerAccess();
        auto created = registry.NewObject<CallTarget>(access, TypeIdOf<CallTarget>(), {}, {});
        REQUIRE(created);
        const ObjectHandle self = created.Value().Handle();
        const Guid add = IdOf(types, "Add");

        CHECK(FunctionBinding::Create<int32>(registry, access, self, add));
        // 함수 없음, 빈 handle, static, 반환이 있음, Result 함수.
        CHECK(BindError(FunctionBinding::Create<int32>(registry, access, self, Guid{7, 7})) ==
              ErrorCode::NotFound);
        CHECK(BindError(FunctionBinding::Create<int32>(registry, access, ObjectHandle{}, add)) !=
              ErrorCode::None);
        CHECK(BindError(FunctionBinding::Create<int32>(
                  registry, access, self, IdOf(types, "Twice"))) == ErrorCode::InvalidArgument);
        CHECK(BindError(FunctionBinding::Create<>(registry, access, self, IdOf(types, "Total"))) ==
              ErrorCode::TypeMismatch);
        CHECK(BindError(FunctionBinding::Create<const String&>(
                  registry, access, self, IdOf(types, "SetLabel"))) == ErrorCode::TypeMismatch);
        // 형식·칸 수가 다르거나 같은 크기의 다른 형식(int32·float, ObjectHandle·WeakObjectRef —
        // 값 형식과 형식 flag로 가른다), enum과 그 underlying 정수.
        CHECK(BindError(FunctionBinding::Create<int64>(registry, access, self, add)) ==
              ErrorCode::TypeMismatch);
        CHECK(BindError(FunctionBinding::Create<int32, int32>(registry, access, self, add)) ==
              ErrorCode::TypeMismatch);
        CHECK(BindError(FunctionBinding::Create<float>(registry, access, self, add)) ==
              ErrorCode::TypeMismatch);
        CHECK(BindError(FunctionBinding::Create<ObjectHandle>(
                  registry, access, self, IdOf(types, "Remember"))) == ErrorCode::TypeMismatch);
        CHECK(BindError(FunctionBinding::Create<WeakObjectRef<Object>>(
                  registry, access, self, IdOf(types, "Mark"))) == ErrorCode::TypeMismatch);
        CHECK(BindError(FunctionBinding::Create<uint8>(
                  registry, access, self, IdOf(types, "Choose"))) == ErrorCode::TypeMismatch);
        CHECK(FunctionBinding::Create<ObjectHandle>(registry, access, self, IdOf(types, "Mark")));
        CHECK(FunctionBinding::Create<CallMode>(registry, access, self, IdOf(types, "Choose")));
        // 다른 문맥.
        CHECK(BindError(FunctionBinding::Create<int32>(registry, ObjectAccessContext{{0x9999, 1}},
                                                       self, add)) == ErrorCode::WrongContext);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(FunctionBinding_InvokeSharesOneFrame) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(GeneratedType<CallTarget>::Register(types));
        REQUIRE(GeneratedType<CallTargetChild>::Register(types));
        ObjectRegistry registry(allocator, types, {0x46424e45, 1});
        const auto access = registry.OwnerAccess();
        auto first = registry.NewObject<CallTarget>(access, TypeIdOf<CallTarget>(), {}, {});
        auto second =
            registry.NewObject<CallTargetChild>(access, TypeIdOf<CallTargetChild>(), {}, {});
        REQUIRE(first && second);
        const ObjectHandle a = first.Value().Handle();
        const ObjectHandle b = second.Value().Handle();

        // 같은 frame으로 두 대상을 부른다(b는 파생 — 기반 레코드의 연산표, thunk는 인자를 옮기지
        // 않는다).
        auto toA = FunctionBinding::Create<int32>(registry, access, a, IdOf(types, "Add"));
        auto toB = FunctionBinding::Create<int32>(registry, access, b, IdOf(types, "Add"));
        REQUIRE(toA && toB);
        CHECK(toA.Value().Target() == a && toA.Value().IsBound());
        BindingFrame<int32> frame(allocator);
        REQUIRE(frame.Assign(7));
        REQUIRE(toA.Value().Invoke(registry, access, frame));
        REQUIRE(toB.Value().Invoke(registry, access, frame));
        REQUIRE(toA.Value().Invoke(registry, access, frame));
        CHECK(TotalOf(registry, a) == 14 && TotalOf(registry, b) == 7);

        // const String&·struct·약한 참조·enum·ObjectHandle 인자(값 ops 복사).
        auto note =
            FunctionBinding::Create<const String&>(registry, access, a, IdOf(types, "Note"));
        REQUIRE(note);
        BindingFrame<const String&> text(allocator);
        String hello(allocator);
        REQUIRE(hello.TryAssign("hello"));
        REQUIRE(text.Assign(hello));
        REQUIRE(note.Value().Invoke(registry, access, text));
        CHECK(TotalOf(registry, a) == 19);

        auto place =
            FunctionBinding::Create<const CallPoint&>(registry, access, b, IdOf(types, "Place"));
        REQUIRE(place);
        BindingFrame<const CallPoint&> where(allocator);
        CallPoint point;
        point.x = 2;
        point.y = 3;
        REQUIRE(where.Assign(point));
        REQUIRE(place.Value().Invoke(registry, access, where));
        CHECK(TotalOf(registry, b) == 30);

        auto remember = FunctionBinding::Create<WeakObjectRef<CallTarget>>(registry, access, b,
                                                                           IdOf(types, "Remember"));
        REQUIRE(remember);
        BindingFrame<WeakObjectRef<CallTarget>> other(allocator);
        REQUIRE(other.Assign(WeakObjectRef<CallTarget>(a)));
        REQUIRE(remember.Value().Invoke(registry, access, other));
        CHECK(TotalOf(registry, b) == 1030);

        auto choose = FunctionBinding::Create<CallMode>(registry, access, b, IdOf(types, "Choose"));
        REQUIRE(choose);
        BindingFrame<CallMode> mode(allocator);
        REQUIRE(mode.Assign(CallMode::Doubled));
        REQUIRE(choose.Value().Invoke(registry, access, mode));
        CHECK(TotalOf(registry, b) == 2060);

        auto mark = FunctionBinding::Create<ObjectHandle>(registry, access, b, IdOf(types, "Mark"));
        REQUIRE(mark);
        BindingFrame<ObjectHandle> handle(allocator);
        REQUIRE(handle.Assign(a));
        REQUIRE(mark.Value().Invoke(registry, access, handle));
        CHECK(TotalOf(registry, b) == 2061);

        // 인자 없는 private 함수.
        auto bump = FunctionBinding::Create<>(registry, access, a, IdOf(types, "Bump"));
        REQUIRE(bump);
        BindingFrame<> empty(allocator);
        REQUIRE(bump.Value().Invoke(registry, access, empty));
        CHECK(TotalOf(registry, a) == 119);

        // 빈 바인딩, 바인딩 때와 다른 배치의 frame, 다른 문맥.
        CHECK(CallError(FunctionBinding().Invoke(registry, access, frame)) ==
              ErrorCode::InvalidArgument);
        CHECK(CallError(toA.Value().Invoke(registry, access, text)) == ErrorCode::InvalidArgument);
        CHECK(TotalOf(registry, a) == 119);
        CHECK(CallError(toA.Value().Invoke(registry, ObjectAccessContext{{0x9999, 1}}, frame)) ==
              ErrorCode::WrongContext);

        // 파괴 요청(thunk의 Destroyed)과 회수(생존 조회의 StaleHandle — 캐시한 연산표를 부르지
        // 않는다).
        REQUIRE(registry.RequestDestroy(access, a));
        CHECK(CallError(toA.Value().Invoke(registry, access, frame)) == ErrorCode::Destroyed);
        first.Value().Reset();
        REQUIRE(registry.Collect(access));
        CHECK(CallError(toA.Value().Invoke(registry, access, frame)) == ErrorCode::StaleHandle);
        REQUIRE(toB.Value().Invoke(registry, access, frame));
        CHECK(TotalOf(registry, b) == 2068);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 인자 복사의 할당 실패는 그 오류를 돌려주고 frame은 이전 값 그대로 다시 쓸 수 있다.
TEST(FunctionBinding_FrameAssignFailure) {
    SystemAllocator allocator;
    {
        test::NFailAllocator failing(allocator);
        BindingFrame<const String&> frame(failing);
        String text(allocator);
        REQUIRE(text.TryAssign("a long enough text to need an allocation"));
        failing.FailOnAttempt(1);
        CHECK(CallError(frame.Assign(text)) == ErrorCode::OutOfMemory);
        REQUIRE(frame.Assign(text));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace function_binding_tests_file
