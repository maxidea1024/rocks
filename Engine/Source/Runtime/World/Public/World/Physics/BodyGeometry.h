#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <World/Physics/BodyShapeElement.h>

namespace rocks {

// 컴포넌트가 주는 body 형상이다(UE FKAggregateGeom 대응 — Docs/Design/PhysicsModule 2.4절,
// 7.1.21절). 원소 순서가 원소 번호(HitResult::elementIndex)다. 원소 0개는 형상 없음(body를 만들지
// 않는다)이다.
//
// 사용 예:
//   BodyGeometry geometry(allocator);
//   if (auto added = geometry.Add(BodyShapeElement::Sphere(50.0)); !added) { ... }
//   desc.geometry = geometry.View();
class BodyGeometry {
public:
    explicit BodyGeometry(Allocator& allocator) noexcept : elements_(allocator) {
    }

    // 원소를 끝에 더한다. 오류: 할당 실패.
    Result<void> Add(const BodyShapeElement& element) noexcept {
        return elements_.TryPushBack(element);
    }

    void Clear() noexcept {
        elements_.Clear();
    }

    bool IsEmpty() const noexcept {
        return elements_.IsEmpty();
    }

    usize Size() const noexcept {
        return elements_.Size();
    }

    Span<const BodyShapeElement> View() const noexcept {
        return elements_.View();
    }

private:
    Array<BodyShapeElement> elements_;
};

} // namespace rocks
