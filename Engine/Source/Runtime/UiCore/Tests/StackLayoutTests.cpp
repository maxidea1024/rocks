#include <TestSupport/Test.h>
#include <UiCore/StackLayout.h>

using namespace rocks;
using namespace rocks::ui;

TEST(UiCore_StackAllocatesAutoAndFillWithoutRedistribution) {
    StackSlot slots[] = {{.desired = 20, .before = 2, .after = 3},
                         {.rule = StackSizeRule::Fill, .weight = 1, .maxSize = 10},
                         {.rule = StackSizeRule::Fill, .weight = 3}};
    StackPlacement output[3]{};
    REQUIRE(ArrangeStack(slots, output, 125));
    CHECK(output[0].offset == 2 && output[0].size == 20);
    CHECK(output[1].offset == 25 && output[1].size == 10);
    CHECK(output[2].offset == 35 && output[2].size == 75);
}

TEST(UiCore_StackRejectsInvalidInputWithoutChangingOutput) {
    StackSlot slots[] = {{.minSize = 10, .maxSize = 5}};
    StackPlacement output[] = {{12, 34}};
    CHECK(!ArrangeStack(slots, output, 100));
    CHECK(output[0].offset == 12 && output[0].size == 34);
}
