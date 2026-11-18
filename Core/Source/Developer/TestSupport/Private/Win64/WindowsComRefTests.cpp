#include <Core/Move.h>
#include <Platform/Win64/ComRef.h>
#include <TestSupport/Test.h>

#include <Windows.h>

// Platform/Win64/ComRef.h 시험이다. 가짜 COM 객체로 AddRef·Release 수를 직접 세어 소유 규칙을
// 단언한다(Windows 전용 시험이라 TestSupport/Private/Win64에 둔다).
using rocks::Move;
using rocks::platform::windows::ComRef;

namespace {

struct __declspec(uuid("6b0c2a51-8a1e-4c38-9a62-3f1d2a7e0b11")) IFakeFirst : IUnknown {
    virtual int STDMETHODCALLTYPE First() = 0;
};

struct __declspec(uuid("0f4e7d3c-2b6a-4f81-8c9d-5e3a1b7c6d02")) IFakeSecond : IUnknown {
    virtual int STDMETHODCALLTYPE Second() = 0;
};

// 아무도 구현하지 않는 interface다(E_NOINTERFACE 경로).
struct __declspec(uuid("9a1d5c7e-3f2b-4e6a-b8c4-7d0e2f1a3b93")) IFakeMissing : IUnknown {
    virtual int STDMETHODCALLTYPE Missing() = 0;
};

// 참조 수만 세는 가짜 COM 객체다. Release가 0이 되어도 지우지 않는다(시험이 수명을 쥔다).
class FakeObject final : public IFakeFirst, public IFakeSecond {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }

        if (IsEqualIID(id, __uuidof(IUnknown)) || IsEqualIID(id, __uuidof(IFakeFirst))) {
            *out = static_cast<IFakeFirst*>(this);
        } else if (IsEqualIID(id, __uuidof(IFakeSecond))) {
            *out = static_cast<IFakeSecond*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }

        ++references;
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return --references;
    }

    int STDMETHODCALLTYPE First() override {
        return 1;
    }

    int STDMETHODCALLTYPE Second() override {
        return 2;
    }

    // 생성한 쪽이 쥔 참조 하나로 시작한다(COM 생성 함수가 돌려주는 상태).
    ULONG references = 1;
};

} // namespace

TEST(PlatformWindowsComRefAttachResetAndDestructorReleaseOnce) {
    FakeObject object;
    {
        ComRef<IFakeFirst> ref;
        CHECK(!ref && ref.Get() == nullptr);
        ref.Attach(&object);
        CHECK(ref && ref.Get() == &object && object.references == 1);
        CHECK(ref->First() == 1);
        ref.Reset();
        CHECK(!ref && object.references == 0);
        // 빈 ComRef의 Reset은 아무것도 하지 않는다.
        ref.Reset();
        CHECK(object.references == 0);

        object.references = 1;
        ref.Attach(&object);
    }

    // 소멸자가 한 번 놓는다.
    CHECK(object.references == 0);
}

TEST(PlatformWindowsComRefMoveTransfersWithoutCountChange) {
    FakeObject object;
    ComRef<IFakeFirst> first;
    first.Attach(&object);

    ComRef<IFakeFirst> second(Move(first));
    CHECK(!first && second.Get() == &object && object.references == 1);

    ComRef<IFakeFirst> third;
    third = Move(second);
    CHECK(!second && third.Get() == &object && object.references == 1);

    // 이미 쥔 쪽으로 이동 대입하면 기존 참조를 놓는다.
    FakeObject other;
    ComRef<IFakeFirst> fourth;
    fourth.Attach(&other);
    fourth = Move(third);
    CHECK(other.references == 0 && object.references == 1 && fourth.Get() == &object);

    // 자기 이동 대입은 아무것도 바꾸지 않는다.
    ComRef<IFakeFirst>& alias = fourth;
    fourth = Move(alias);
    CHECK(fourth.Get() == &object && object.references == 1);

    fourth.Reset();
    CHECK(object.references == 0);
}

TEST(PlatformWindowsComRefOutReleasesPreviousReference) {
    FakeObject first;
    FakeObject second;
    ComRef<IFakeFirst> ref;
    ref.Attach(&first);

    // 생성 함수처럼 Out()으로 다시 채운다. 이전 참조는 Out()이 놓는다.
    IFakeFirst** slot = ref.Out();
    CHECK(first.references == 0 && *slot == nullptr && !ref);
    *slot = &second;
    CHECK(ref.Get() == &second && second.references == 1);

    ref.Reset();
    CHECK(second.references == 0);
}

TEST(PlatformWindowsComRefDetachHandsOverOwnership) {
    FakeObject object;
    ComRef<IFakeFirst> ref;
    ref.Attach(&object);

    IFakeFirst* raw = ref.Detach();
    CHECK(raw == &object && !ref && object.references == 1);

    // 다른 interface 형식 ComRef로 옮길 때의 형태다(참조 수 그대로).
    ComRef<IUnknown> unknown;
    unknown.Attach(raw);
    CHECK(object.references == 1);
    unknown.Reset();
    CHECK(object.references == 0);

    ComRef<IFakeFirst> empty;
    CHECK(empty.Detach() == nullptr);
}

TEST(PlatformWindowsComRefShareAddsOneReference) {
    FakeObject object;
    ComRef<IFakeFirst> first;
    first.Attach(&object);

    ComRef<IFakeFirst> second = first.Share();
    CHECK(first.Get() == &object && second.Get() == &object && object.references == 2);

    first.Reset();
    CHECK(object.references == 1 && second);
    second.Reset();
    CHECK(object.references == 0);

    ComRef<IFakeFirst> empty;
    ComRef<IFakeFirst> sharedEmpty = empty.Share();
    CHECK(!sharedEmpty);
}

TEST(PlatformWindowsComRefTryQueryAsKeepsHresultAndCounts) {
    FakeObject object;
    ComRef<IFakeFirst> first;
    first.Attach(&object);

    ComRef<IFakeSecond> second;
    CHECK(first.TryQueryAs(second) == S_OK);
    CHECK(second && second->Second() == 2 && object.references == 2);

    // 다시 조회하면 out의 기존 참조를 먼저 놓는다.
    CHECK(first.TryQueryAs(second) == S_OK);
    CHECK(object.references == 2);

    // 지원하지 않는 interface는 E_NOINTERFACE이고 out은 비며, 기존 참조는 놓인다.
    ComRef<IFakeMissing> missing;
    CHECK(first.TryQueryAs(missing) == E_NOINTERFACE);
    CHECK(!missing && object.references == 2);
    CHECK(second.TryQueryAs(missing) == E_NOINTERFACE && object.references == 2);

    // 빈 ComRef의 조회는 E_POINTER이고 out을 비운다.
    ComRef<IFakeFirst> empty;
    CHECK(empty.TryQueryAs(second) == E_POINTER);
    CHECK(!second && object.references == 1);

    first.Reset();
    CHECK(object.references == 0);
}
