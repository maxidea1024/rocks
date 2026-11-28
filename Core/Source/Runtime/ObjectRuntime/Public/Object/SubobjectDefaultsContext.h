#pragma once

#include <Base/TypeTraits.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Object/Object.h>
#include <Object/ObjectRefs.h>
#include <Object/TypeLease.h>

namespace rocks {

// 소유 클래스가 선언 subobject에 기본값을 주는 문맥이다(UE 생성자의 CreateDefaultSubobject 뒤
// 설정에 대응 — Docs/Design/Prefab 10.2절). 소유 클래스의 static InitSubobjectDefaults가 받는다.
// 두 모드가 같은 함수를 쓴다: instancing(ConstructionContext::CreateDeclaredSubobjects — staged
// 소유 객체·subobject)과 기록(subobject 기본값 레코드 — Registry 밖 임시 인스턴스, PF1c). 어느
// 모드든 World 장부 밖이다 — 값만 설정하고 Registry·World·다른 객체를 건드리지 않는다(생성자
// 결정성 계약과 같다, 두 번 실행한 인코딩이 같아야 한다).
//
// 수명: 그 hook 호출 동안만 유효하다(보관하지 않는다).
//
// 사용 예(파생 클래스가 상속 subobject와 자기 subobject를 덮는다 — protected 필드의 멤버 포인터는
// 자기 클래스 이름으로 만든다):
//   Result<void> Lamp::InitSubobjectDefaults(SubobjectDefaultsContext& context) noexcept {
//       if (auto bulb = context.With(&Lamp::bulb_, [](PointLightComponent& light) noexcept {
//               light.SetMobility(ComponentMobility::Stationary);
//               return Result<void>::Success();
//           });
//           !bulb) {
//           return bulb;
//       }
//
//       return context.With(&Lamp::shade_, [](StaticMeshComponent& mesh) noexcept {
//           mesh.SetCastShadow(false);
//           return Result<void>::Success();
//       });
//   }
class ROCKS_OBJECTRUNTIME_API SubobjectDefaultsContext {
public:
    // owner는 소유 객체, ownerType은 그 타입 lease(펼친 subobject 표·연산), subobjects는 표 순서의
    // subobject 객체다(크기 = 표 크기). 셋 다 이 문맥보다 오래 산다.
    SubobjectDefaultsContext(Object& owner, const TypeLease& ownerType,
                             Span<Object* const> subobjects) noexcept;

    SubobjectDefaultsContext(const SubobjectDefaultsContext&) = delete;
    SubobjectDefaultsContext& operator=(const SubobjectDefaultsContext&) = delete;

    // field(소유 클래스의 Subobject 필드 멤버 포인터 — base 필드도 된다)가 가리키는 subobject로
    // fn(T&) → Result<void>를 부르고 그 결과를 돌려준다.
    //
    // 오류: 소유 객체가 Owner가 아님, field가 선언 subobject 필드가 아님 — InvalidArgument(fn을
    // 부르지 않는다).
    template <class Owner, class T, class F>
    Result<void> With(ObjectRef<T> Owner::* field, F&& fn) noexcept {
        static_assert(
            IsBaseOfV<Object, Owner> && IsBaseOfV<Object, T>,
            "SubobjectDefaultsContext::With takes an ObjectRef member of an Object class");
        if (!ownerType_.IsNativeA(NativeTypeKey<Owner>())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        Owner& owner = static_cast<Owner&>(owner_);
        Object* subobject = Find(&(owner.*field));
        if (subobject == nullptr) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 표의 생성 타입은 필드의 T 그 자체다(MetaCompiler RMC3002 — Docs/Design/Prefab 10.2절).
        return fn(static_cast<T&>(*subobject));
    }

private:
    // field(소유 필드 주소)에 맞는 표 항목의 subobject다. 없으면 nullptr.
    Object* Find(const void* field) const noexcept;

    Object& owner_;
    const TypeLease& ownerType_;
    Span<Object* const> subobjects_;
};

class TypeRegistry;

// ownerType 소유 클래스의 base 사슬 InitSubobjectDefaults를 가장 먼 base부터 부른다(Docs/Design/
// Prefab 10.2절 — instancing·기록 두 모드가 쓴다). subobjects는 펼친 표 순서의 subobject 객체다.
// 오류: 사슬이 MaxSubobjectOwnerDepth보다 깊음 CapacityExceeded, base 타입 조회 오류, hook이 돌려준
// 오류.
ROCKS_OBJECTRUNTIME_API Result<void>
ApplySubobjectDefaults(TypeRegistry& types, const TypeLease& ownerType, Object& owner,
                       Span<Object* const> subobjects) noexcept;

} // namespace rocks
