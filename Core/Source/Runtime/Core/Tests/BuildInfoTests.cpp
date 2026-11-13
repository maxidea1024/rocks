#include <Core/Build/BuildInfo.h>
#include <TestSupport/Test.h>

using namespace rocks;

// 빌드가 이 시험 실행 파일에 주입한 두 값이 비어 있지 않고 서로 다르며, 부를 때마다 같아야 한다.
TEST(BuildInfo_InjectedValuesAreDistinctAndStable) {
    const Sha256Digest zero{};
    const Sha256Digest fingerprint = build::ToolchainFingerprint();
    const Sha256Digest id = build::ToolBuildId();
    CHECK(fingerprint != zero);
    CHECK(id != zero);
    CHECK(fingerprint != id);
    CHECK(build::ToolchainFingerprint() == fingerprint);
    CHECK(build::ToolBuildId() == id);
}
