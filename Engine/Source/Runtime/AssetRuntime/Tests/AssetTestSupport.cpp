#include "AssetTestSupport.h"

#include <AssetTestFixtures/AssetTestFixtures.module.generated.h>

// AssetEnv의 생성자다. 모듈 타입 등록 함수(모듈 생성 헤더에 선언)를 부르므로 번역 단위에 둔다 —
// 손으로 쓴 헤더는 모듈 생성 헤더를 include하지 않는다(Docs/Design/ReflectionGeneratedSources.ko.md
// 8절).
namespace rocks::asset_test {

Result<void> RegisterAssetTestTypes(ModuleRegistration& modules) noexcept {
    if (auto runtime = RegisterAssetRuntimeTypes(modules); !runtime) {
        return runtime;
    }

    return generated::AssetTestFixtures::RegisterModuleTypes(modules);
}

AssetEnv::AssetEnv() noexcept : types(allocator), modules(types, allocator), names(allocator) {
    registered = RegisterAssetTestTypes(modules).IsOk() && names.Initialize().IsOk();
    RegistryOptions options;
    options.names = &names;
    options.policy.packageNativeType = NativeTypeKey<Package>();
    options.policy.rejectWorldOnly = true;
    auto made = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                   ContextId{0x4153534554ull, 1}, options);
    auto other = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                    ContextId{0x4153534554ull, 2}, options);
    if (made && other) {
        source = Move(made.Value());
        target = Move(other.Value());
    }
}

} // namespace rocks::asset_test
