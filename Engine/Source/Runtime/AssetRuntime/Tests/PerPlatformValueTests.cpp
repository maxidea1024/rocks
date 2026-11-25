#include <AssetRuntime/PerPlatformBool.h>
#include <AssetRuntime/PerPlatformFloat.h>
#include <AssetRuntime/PerPlatformInt32.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <TestSupport/Test.h>

// 대상 프로파일별 값 시험이다(Docs/Design/MeshAnimationAssets 5.5.1절 2): 덮어쓰기 표에 있으면 그
// 값, 없거나 None이면 기본값, 키는 Name 비교 규칙(ASCII 대소문자 무시).
using namespace rocks;
namespace per_platform_value_tests_file {

TEST(PerPlatform_ResolveUsesOverrideOrDefault) {
    SystemAllocator allocator;
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    const Name android = names.TryIntern("Android").Value();
    const Name windows = names.TryIntern("Windows").Value();
    PerPlatformFloat size(allocator);
    CHECK(size.defaultValue == 0.0f && size.Resolve(android) == 0.0f);
    size.defaultValue = 1.0f;
    REQUIRE(size.perPlatform.Insert(android, 0.25f));
    CHECK(size.Resolve(android) == 0.25f);
    CHECK(size.Resolve(windows) == 1.0f);
    CHECK(size.Resolve(Name{}) == 1.0f);
    CHECK(size.Resolve(names.TryIntern("ANDROID").Value()) == 0.25f);

    PerPlatformInt32 lod(allocator);
    CHECK(lod.defaultValue == 0);
    lod.defaultValue = -1;
    REQUIRE(lod.perPlatform.Insert(windows, 3));
    CHECK(lod.Resolve(windows) == 3 && lod.Resolve(android) == -1 && lod.Resolve(Name{}) == -1);
}

// PerPlatformBool(UE FPerPlatformBool — 기본 거짓, 맵 EditorOnly): 기본값, 덮어쓰기, None 프로파일.
TEST(PerPlatform_BoolResolves) {
    SystemAllocator allocator;
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    PerPlatformBool value(allocator);
    const Name android = names.TryIntern("Android").Value();
    CHECK(!value.defaultValue && !value.Resolve(android));
    REQUIRE(value.perPlatform.Insert(android, true));
    CHECK(value.Resolve(android) && !value.Resolve(names.TryIntern("Windows").Value()) &&
          !value.Resolve(Name{}));
    value.perPlatform.Clear();
    CHECK(names.Shutdown());
}

} // namespace per_platform_value_tests_file
