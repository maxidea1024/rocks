#include <TestSupport/Test.h>
#include <UnrealExchange/UnrealExchangeFormat.h>

// Unreal 교환 플러그인 시험이다(Docs/Design/UnrealExchange.ko.md 14절). EngineTests가 이 플러그인을
// 켜고 Tests/를 모은다(BuildTargets 3.9절 BD40). 리더 시험은 UX1a에서 더한다.
using namespace rocks;
namespace unreal_exchange_format_tests_file {

TEST(UnrealExchange_BundleFormatVersion) {
    REQUIRE(UnrealExchangeBundleFormatVersion == 1);
}

} // namespace unreal_exchange_format_tests_file
