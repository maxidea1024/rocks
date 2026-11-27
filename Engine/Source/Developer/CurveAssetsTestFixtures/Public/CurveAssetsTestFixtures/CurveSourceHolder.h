#pragma once

#include <AssetRuntime/Package.h>
#include <CurveAssets/ColorCurveSource.h>
#include <CurveAssets/CurveTableRowRef.h>
#include <CurveAssets/FloatCurveSource.h>
#include <CurveAssets/VectorCurveSource.h>
#include <Meta/Annotations.h>

// 커브 원천·표 행 참조 시험의 소유 객체다(Docs/Design/CurveAssets 3.1·3.3절 CA1·CA2). 둘 다 참조
// struct라 객체의 직접 필드로만 둔다.
namespace rocks::curve_fixture {

ROCKS_CLASS()
class CurveSourceHolder final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit CurveSourceHolder(Allocator& allocator) noexcept
        : floatSource(allocator), vectorSource(allocator), colorSource(allocator) {
    }

    ROCKS_PROPERTY() FloatCurveSource floatSource;
    ROCKS_PROPERTY() VectorCurveSource vectorSource;
    ROCKS_PROPERTY() ColorCurveSource colorSource;
    ROCKS_PROPERTY() CurveTableRowRef tableRow;
};

} // namespace rocks::curve_fixture
