#pragma once

#include <ReflectionTestFixtures/AssignFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Meta/GeneratedType.h>
#include <Object/ConstructionContext.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistryOptions.h>
#include <Object/ReferenceObserver.h>
#include <ReflectionText/TextAssignments.h>
#include <TestSupport/Test.h>

// ReflectionText 대입 묶음 시험의 공용 준비다(리플렉션 B9i3c1–c4 — fixture
// ReflectionTestFixtures/AssignFixture.h): 할당 실패 주입 allocator, 논리 경로 resolver, 관찰자를
// 건 Registry·AssignHolder lease·진단 모음, 읽기 Borrow, 대입·override 경로 도우미.
namespace rocks::test::assign {

using namespace rocks::reflection_text;
using reflection_fixture::AssignHolder;
using reflection_fixture::AssignTarget;

inline ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

template <class T>
inline ErrorCode ErrorOf(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 할당을 세고 failAt번째 할당을 실패시키는 allocator다.
class FailingAllocator final : public Allocator {
public:
    explicit FailingAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        ++allocations;
        if (failAt != 0 && allocations == failAt) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

    uint64 allocations = 0;
    uint64 failAt = 0;

private:
    Allocator& inner_;
};

// 논리 경로 `/t`를 target으로 푸는 resolver다(Weak 대입).
class TargetResolver final : public TextReferenceResolver {
public:
    Result<void> Resolve(StringView text, TypeId, TextReferenceKind,
                         FieldValue& out) noexcept override {
        if (text != "/t" && text != "/u") {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        out.reference = text == "/t" ? target : second;
        return Result<void>::Success();
    }

    Result<void> Format(TextReferenceKind, const FieldValue&,
                        StringBuilderBase&) noexcept override {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    ObjectHandle target{};
    ObjectHandle second{};
};

// observed면 AssignTarget를 패키지 타입으로 삼고 관찰자를 건다(참조 단위의 관찰자 기록 시험).
inline RegistryOptions AssignOptions(ReferenceObserver& observer, bool observed) noexcept {
    RegistryOptions options;
    if (observed) {
        options.policy.packageNativeType = NativeTypeKey<AssignTarget>();
        options.referenceObserver = &observer;
    }

    return options;
}

// 모듈 등록·객체 Registry·AssignHolder lease·진단 모음이다(소멸: lease → Registry → 등록).
// observed면 AssignHolder는 패키지(AssignTarget) package 아래에 만들고, AssignTarget 대상은 각자
// 패키지다.
struct Fixture {
    explicit Fixture(Allocator& objectAllocator, bool observed_ = false,
                     Allocator* observerAllocator = nullptr) noexcept
        : observed(observed_), observerMemory(observerAllocator ? *observerAllocator : system),
          types(system), observer(observerMemory, ReferenceObserver::DefaultMaxKeys),
          registration(types, system),
          objects(objectAllocator, types, {0x4153, 1}, AssignOptions(observer, observed_)),
          access(objects.OwnerAccess()), diagnostics(system), roots(system) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        auto acquired = types.Acquire(GeneratedType<AssignHolder>::Schema().id);
        REQUIRE(acquired);
        holder = Move(acquired.Value());
        if (observed) {
            auto created = MakeTarget();
            package = created.Handle();
            REQUIRE(roots.TryPushBack(Move(created)));
        }
    }

    ~Fixture() noexcept {
        holder = TypeLease{};
        roots.Clear();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    RootRef<AssignHolder> Make() noexcept {
        ++holders;
        const char name[] = {'h', static_cast<char>('a' + holders % 26), '\0'};
        auto created =
            observed ? objects.NewObject<AssignHolder>(
                           Access(), GeneratedType<AssignHolder>::Schema().id, package, name)
                     : objects.NewObject<AssignHolder>(Access(),
                                                       GeneratedType<AssignHolder>::Schema().id);
        CHECK(created);
        return created ? Move(created.Value()) : RootRef<AssignHolder>{};
    }

    RootRef<AssignTarget> MakeTarget() noexcept {
        auto created =
            objects.NewObject<AssignTarget>(Access(), GeneratedType<AssignTarget>::Schema().id);
        CHECK(created);
        return created ? Move(created.Value()) : RootRef<AssignTarget>{};
    }

    Array<ReferenceEdgeDelta> Take() noexcept {
        // 기록은 관찰자 allocator의 배열로 받는다.
        Array<ReferenceEdgeDelta> changes(observerMemory);
        CHECK(observer.TakeChanges(changes));
        return changes;
    }

    const ObjectAccessContext& Access() const noexcept {
        return access;
    }

    Result<PreparedAssignments> Prepare(ObjectHandle object,
                                        Span<const TextAssignment> assignments) noexcept {
        diagnostics.Clear();
        return PrepareAssignments(AssignmentTarget::Live(objects, Access(), object), assignments,
                                  options, diagnostics);
    }

    bool observed;
    uint32 holders = 0;
    SystemAllocator system;
    Allocator& observerMemory;
    TypeRegistry types;
    ReferenceObserver observer;
    ModuleRegistration registration;
    ObjectRegistry objects;
    ObjectAccessContext access;
    TextDiagnostics diagnostics;
    TextCodecOptions options;
    TypeLease holder;
    ObjectHandle package{};
    Array<RootRef<AssignTarget>> roots;
};

// 객체를 읽기 Borrow로 들여다본다(묶음 게시 전에 놓는다).
class Peek {
public:
    Peek(Fixture& fixture, ObjectHandle object) noexcept
        : borrow_(fixture.objects.Resolve<AssignHolder>(fixture.Access(), object)) {
        REQUIRE(borrow_);
    }

    AssignHolder* operator->() noexcept {
        return borrow_.Value().Get().Value();
    }

private:
    Result<Borrow<AssignHolder>> borrow_;
};

inline TextAssignment Set(const char* path, const char* value, uint32 source = 0) noexcept {
    return TextAssignment{AssignmentKind::Set, StringView(path), StringView(value), source};
}

inline TextAssignment Other(AssignmentKind kind, const char* path, uint32 source = 0) noexcept {
    return TextAssignment{kind, StringView(path), StringView(), source};
}

// override 경로 하나가 text(AssignHolder 경로)와 같은지다.
inline bool HasOverride(Fixture& fixture, Span<const PropertyPath> paths,
                        const char* text) noexcept {
    PropertyPathBuffer expected;
    if (!ParsePropertyPath(fixture.holder, StringView(text), expected)) {
        return false;
    }

    for (usize index = 0; index < paths.Size(); ++index) {
        if (SamePropertyPath(paths[index], expected.View())) {
            return true;
        }
    }

    return false;
}

inline bool Touches(Fixture& fixture, const PreparedAssignments& prepared,
                    const char* text) noexcept {
    PropertyPathBuffer path;
    CHECK(ParsePropertyPath(fixture.holder, StringView(text), path));
    return prepared.Touches(path.View());
}

} // namespace rocks::test::assign
