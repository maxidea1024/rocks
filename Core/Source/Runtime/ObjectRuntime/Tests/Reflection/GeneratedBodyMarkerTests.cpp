#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Meta/Annotations.h>
#include <TestSupport/Test.h>

// ROCKS_GENERATED_BODY()가 남기는 표지는 정의 없는 선언뿐이라 타입의 모양을 바꾸지 않는다
// (Docs/Design/ReflectionGeneratedSources.ko.md 8.1절). 표지가 있는 struct와 없는 쌍둥이의 크기·
// 정렬·aggregate 여부·복사 형질·배치가 같고, 표지는 선언만으로 판정된다(private 구역, cv 한정자,
// 물려받은 표지는 세지 않음).
using namespace rocks;
namespace generated_body_marker_tests_file {

struct PlainTwin {
    int32 count = 0;
    float weight = 0.0f;
    uint8 flag = 0;
};

struct MarkedTwin {
    ROCKS_GENERATED_BODY()

    int32 count = 0;
    float weight = 0.0f;
    uint8 flag = 0;
};

class PrivatelyMarked {
    ROCKS_GENERATED_BODY()

    int32 hidden = 0;
};

struct InheritsMarker : MarkedTwin {};

static_assert(sizeof(MarkedTwin) == sizeof(PlainTwin) && alignof(MarkedTwin) == alignof(PlainTwin));
static_assert(IsAggregateV<MarkedTwin> && IsAggregateV<PlainTwin>);
static_assert(IsTriviallyCopyableV<MarkedTwin> && IsTriviallyCopyableV<PlainTwin>);
static_assert(IsStandardLayoutV<MarkedTwin> && IsStandardLayoutV<PlainTwin>);

static_assert(ReflectedDeclarationV<MarkedTwin> && ReflectedDeclarationV<const MarkedTwin>);
static_assert(ReflectedDeclarationV<PrivatelyMarked>);
static_assert(!ReflectedDeclarationV<PlainTwin> && !ReflectedDeclarationV<InheritsMarker>);
static_assert(!ReflectedDeclarationV<int32> && !ReflectedDeclarationV<MarkedTwin*>);

TEST(ReflectionGeneratedBodyMarkerKeepsAggregateInitialization) {
    const MarkedTwin marked{.count = 3, .weight = 1.5f, .flag = 1};
    const PlainTwin plain{.count = 3, .weight = 1.5f, .flag = 1};
    CHECK(marked.count == plain.count);
    CHECK(marked.weight == plain.weight);
    CHECK(marked.flag == plain.flag);
}

} // namespace generated_body_marker_tests_file
