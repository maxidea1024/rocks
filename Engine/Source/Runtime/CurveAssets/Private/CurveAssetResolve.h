#pragma once

#include <Base/TypeTraits.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectRegistry.h>

// 원천의 에셋 참조 풀기다(Docs/Design/CurveAssets 3.1.1절). 세 원천이 함께 쓴다.
namespace rocks::curve_assets_detail {

// handle의 T를 읽기 borrow로 풀어 fn(const T&)를 부르고 그 Result를 돌려준다. borrow는 fn이 끝날
// 때까지 쥔다(포인터를 밖으로 내지 않는다). 풀리지 않으면 Resolve·Get의 오류다(인라인으로 대신하지
// 않는다 — 설계 CAD8).
template <class T, class Fn>
auto WithCurveAsset(ObjectRegistry& registry, const ObjectAccessContext& access,
                    ObjectHandle handle, Fn&& fn) noexcept -> decltype(fn(DeclVal<const T&>())) {
    using Returned = decltype(fn(DeclVal<const T&>()));
    auto borrowed = registry.Resolve<T>(access, handle);
    if (!borrowed) {
        return Returned::Failure(borrowed.Error());
    }

    auto pointer = borrowed.Value().Get();
    if (!pointer) {
        return Returned::Failure(pointer.Error());
    }

    return fn(*pointer.Value());
}

} // namespace rocks::curve_assets_detail
