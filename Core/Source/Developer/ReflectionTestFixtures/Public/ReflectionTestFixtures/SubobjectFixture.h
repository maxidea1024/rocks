#pragma once

#include <Core/Result.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefs.h>
#include <Object/RuntimeTypeOps.h>

// 선언 subobject 시험 대상이다(Docs/Design/Prefab 10.2절 — UE CreateDefaultSubobject 대응). 소유
// 클래스 SubobjectOwner(소유 목록 parts·root 필드)와 부착 부모 필드를 가진 부품 SubobjectScenePart,
// 부착 지정자가 없는 부품 SubobjectPart, base 표를 이어받고 항목을 더하는 파생
// SubobjectOwnerChild다. 펼친 표: body(Root) → arm(body에 붙음) → plain(붙지 않음) → (파생)
// hand(Root에 붙음). 기본값 덮기: base가 body 1·arm 2, 파생이 arm 20(상속 덮기)·hand 3
// (SubobjectFixture.cpp).
namespace rocks::reflection_fixture {

ROCKS_CLASS()
class SubobjectPart : public Object {
    ROCKS_GENERATED_BODY()

public:
    ROCKS_PROPERTY() int32 count = 0;
};

ROCKS_CLASS(SubobjectAttach = "parent")
class SubobjectScenePart : public SubobjectPart {
    ROCKS_GENERATED_BODY()

public:
    ROCKS_PROPERTY() WeakObjectRef<SubobjectScenePart> parent;
};

ROCKS_CLASS(SubobjectList = "parts", SubobjectRoot = "root")
class SubobjectOwner : public Object {
    ROCKS_GENERATED_BODY()

public:
    explicit SubobjectOwner(Allocator& allocator) noexcept : parts(allocator) {
    }

    ROCKS_PROPERTY() ObjectRefArray<SubobjectPart> parts;
    ROCKS_PROPERTY() ObjectRef<SubobjectScenePart> root;

    // 시험이 선언 subobject handle을 읽는다.
    ObjectHandle Body() const noexcept {
        return body.Handle();
    }

    ObjectHandle Arm() const noexcept {
        return arm.Handle();
    }

    ObjectHandle Plain() const noexcept {
        return plain.Handle();
    }

    // 시험이 선언 subobject 소유 필드를 Registry API로 바꾼다(파괴 상태 저장 시험).
    ObjectRef<SubobjectScenePart>& ArmField() noexcept {
        return arm;
    }

    // 소유 클래스가 subobject에 주는 기본값 덮기다(값은 SubobjectFixture.cpp).
    ROCKS_REFLECTIONTESTFIXTURES_API static Result<void>
    InitSubobjectDefaults(SubobjectDefaultsContext& context) noexcept;

protected:
    ROCKS_PROPERTY(Subobject, Root) ObjectRef<SubobjectScenePart> body;
    ROCKS_PROPERTY(Subobject, AttachParent = "body") ObjectRef<SubobjectScenePart> arm;
    ROCKS_PROPERTY(Subobject) ObjectRef<SubobjectPart> plain;
};

ROCKS_CLASS()
class SubobjectOwnerChild : public SubobjectOwner {
    ROCKS_GENERATED_BODY()

public:
    using SubobjectOwner::SubobjectOwner;

    ObjectHandle Hand() const noexcept {
        return hand_.Handle();
    }

    // 상속 subobject의 기본값을 덮고 자기 subobject 값을 준다(값은 SubobjectFixture.cpp).
    ROCKS_REFLECTIONTESTFIXTURES_API static Result<void>
    InitSubobjectDefaults(SubobjectDefaultsContext& context) noexcept;

protected:
    ROCKS_PROPERTY(Subobject, Name = "hand") ObjectRef<SubobjectScenePart> hand_;
};

// 기본값 덮기 hook이 실패하는 파생이다(생성 실패가 트랜잭션 전체를 되돌리는지 본다).
ROCKS_CLASS()
class SubobjectFailingOwner : public SubobjectOwner {
    ROCKS_GENERATED_BODY()

public:
    using SubobjectOwner::SubobjectOwner;

    // 늘 Unsupported를 돌려준다(값은 SubobjectFixture.cpp).
    ROCKS_REFLECTIONTESTFIXTURES_API static Result<void>
    InitSubobjectDefaults(SubobjectDefaultsContext& context) noexcept;
};

} // namespace rocks::reflection_fixture
