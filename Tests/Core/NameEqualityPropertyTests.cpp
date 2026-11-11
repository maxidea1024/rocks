#include <Core/Hash.h>
#include <Core/Text/Name.h>
#include <Tests/Support/Test.h>

using namespace rocks;

TEST(NameEqualityAndHashFormConsistentEquivalenceClasses) {
    // 정상 None·표시별 None·번호·일반 이름과 잘못된 토큰을 함께 다룬다.
    // 위조 토큰은 구조적 검사에만 해당하고 실제 발급은 NamePool::Acquire가 확인한다.
    const Name values[] = {
        {},
        {1, 0, 7, 0, NameKind::NoneDisplay},
        {2, 0, 8, 0, NameKind::NoneDisplay},
        {1, 0, 7, 1, NameKind::NoneDisplay},
        {2, 0, 8, 1, NameKind::NoneDisplay},
        {1, 3, 7, 0, NameKind::Normal},
        {1, 3, 8, 0, NameKind::Normal},
        {2, 3, 7, 0, NameKind::Normal},
        {1, 0, 0, 0, NameKind::NoneDisplay},
        {1, 0, 0, 0, NameKind::Normal},
        {0, 0, 0, 1, NameKind::None},
        {0, 0, 0, 0, static_cast<NameKind>(255)},
        {1, 0, 7, 2147483648u, NameKind::NoneDisplay},
        {1, 3, 0, 0, NameKind::Normal},
        {1, 3, 7, 1, NameKind::Normal},
    };
    constexpr usize count = sizeof(values) / sizeof(values[0]);
    for (usize i = 0; i < count; ++i) {
        CHECK(values[i] == values[i]);
        for (usize j = 0; j < count; ++j) {
            const bool equal = values[i] == values[j];
            CHECK(equal == (values[j] == values[i]));
            if (equal) {
                CHECK(Hash<Name>{}(values[i]) == Hash<Name>{}(values[j]));
            }
            for (usize k = 0; k < count; ++k) {
                if (equal && values[j] == values[k]) {
                    CHECK(values[i] == values[k]);
                }
            }
        }
    }

    CHECK(values[0].IsNone());
    CHECK(values[0] == values[1] && values[1] == values[2]);
    CHECK(!values[3].IsNone() && values[3] == values[4]);
    CHECK(values[5] == values[6] && values[5] != values[7]);
    CHECK(!values[13].IsValid() && values[13] != values[5]);
    CHECK(!values[12].IsValid() && values[12] != values[1]);
}
