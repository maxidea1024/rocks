// 미리 빌드한 바이너리(Engine/Source/ThirdParty의 kind: binary)의 커밋한 공개 헤더를 엔진 경고
// 규칙으로 include하는지 확인한다 (Docs/Design/CompilerDiagnostics.ko.md 미리 빌드한 바이너리 절
// 2항). 바이너리는 링크하지 않는다(워커가 실행 중에 연다). SPIRV-Cross의 spirv.h와 SPIRV-Reflect의
// spirv.h는 include guard(spirv_H)가 같아 한 번역 단위에 섞지 않는다. DXC dxcapi.h는 Windows에서
// COM 형식(IUnknown·HRESULT)을 쓰므로 Windows 헤더를 먼저 include하는 플랫폼 구현이 확인한다.
#include <TestSupport/Test.h>
#include <spirv-tools/libspirv.h>
#include <spirv_cross_c.h>
#include <stdint.h>

TEST(ThirdParty_ToolPublicHeadersMatchPinnedVersions) {
    // 커밋한 공개 헤더가 오프라인 도구 빌드와 같은 판인지 컴파일 시점 상수로 본다(함수는 부르지
    // 않는다).
    CHECK(SPVC_C_API_VERSION_MAJOR == 0);
    CHECK(static_cast<int>(SPV_SUCCESS) == 0 &&
          static_cast<int>(SPV_ENV_VULKAN_1_1) != static_cast<int>(SPV_ENV_UNIVERSAL_1_0));
    CHECK(static_cast<uint32_t>(SpvVersion) >= 0x00010600u);
}