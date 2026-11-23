#include <Core/Build/BuildInfo.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace build_info_tests_file {

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

// 시험 실행 파일은 Program 타깃이다(Docs/Design/BuildTargets 3.2절 BS2a). 빌드 정보 TU가 타깃
// 종류를 주고 타깃 매크로와 맞는지 static_assert로 확인한다.
TEST(BuildInfo_CurrentTargetTypeIsProgram) {
    CHECK(build::CurrentTargetType() == build::TargetType::Program);
    CHECK(ROCKS_WITH_SERVER_CODE == 1 && ROCKS_WITH_CLIENT_CODE == 1 && ROCKS_WITH_EDITOR == 0);
}

// 주입한 엔진 버전의 불변식이다. 기대 값(Build/Build.version과 같음)은 빌드 도구 시험이 생성 TU와
// manifest로 확인하므로 여기서는 숫자를 적지 않는다(버전을 올려도 이 시험은 바뀌지 않는다).
TEST(BuildInfo_InjectedEngineVersionIsAVersion) {
    const EngineVersion version = build::CurrentEngineVersion();
    CHECK(!version.IsEmpty());
    CHECK(build::CurrentEngineVersion() == version);

    // 호환 기준은 같은 주·부의 패치 0이고, 현재 엔진은 자기 기준으로 저장한 패키지를 읽는다.
    const EngineVersion baseline = version.CompatibleBaseline();
    CHECK(baseline.majorVersion == version.majorVersion);
    CHECK(baseline.minorVersion == version.minorVersion);
    CHECK(baseline.patchVersion == 0);
    CHECK(baseline <= version);
    CHECK(version.CanLoadPackageCompatibleWith(baseline));
    CHECK(version.CanLoadPackageCompatibleWith(version));

    // 표기는 "주.부.패치"이며 점 두 개로 나뉜 세 10진 수가 칸 값과 같다.
    char text[EngineVersion::TextBytes];
    const usize length = version.ToChars(text);
    CHECK(length >= 5 && length < EngineVersion::TextBytes && text[length] == '\0');
    uint32 parts[3] = {};
    usize part = 0;
    for (usize index = 0; index < length; ++index) {
        if (text[index] == '.') {
            ++part;
            continue;
        }

        REQUIRE(part < 3 && text[index] >= '0' && text[index] <= '9');
        parts[part] = parts[part] * 10u + static_cast<uint32>(text[index] - '0');
    }

    CHECK(part == 2);
    CHECK(parts[0] == version.majorVersion);
    CHECK(parts[1] == version.minorVersion);
    CHECK(parts[2] == version.patchVersion);
}
} // namespace build_info_tests_file
