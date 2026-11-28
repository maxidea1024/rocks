#include <Core/Lifetime/ModuleLease.h>
#include <MaterialCook/MaterialCookCatalog.h>
#include <MaterialSchema/MaterialSchemaModule.h>

// 쿠킹의 머티리얼 노드 카탈로그다(Docs/Design/MaterialSystem 12.8절 MS3c).
namespace rocks {

MaterialCookCatalog::MaterialCookCatalog(Allocator& allocator, NamePool& names) noexcept
    : names_(&names), registry_(allocator, names), catalog_(allocator),
      materialSchema_(material::MaterialGraphKind::Material, catalog_, names, allocator),
      functionSchema_(material::MaterialGraphKind::Function, catalog_, names, allocator) {
}

Result<void> MaterialCookCatalog::Initialize() noexcept {
    if (!materialSchema_.Ready() || !functionSchema_.Ready()) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    if (auto opened = registry_.Initialize(); !opened) {
        return opened;
    }

    if (auto registered = registry_.RegisterSchema(materialSchema_, ModuleLease()); !registered) {
        return Result<void>::Failure(registered.Error());
    }

    if (auto registered = registry_.RegisterSchema(functionSchema_, ModuleLease()); !registered) {
        return Result<void>::Failure(registered.Error());
    }

    // 함수 그래프 전용 노드(FunctionInput·FunctionOutput)도 함수 도메인에 든다.
    if (auto groups = catalog_.RegisterGroups(registry_, materialSchema_,
                                              material::BuiltinMaterialNodeGroups(), ModuleLease());
        !groups) {
        return Result<void>::Failure(groups.Error());
    }

    auto functions = catalog_.RegisterGroups(registry_, functionSchema_,
                                             material::BuiltinMaterialNodeGroups(), ModuleLease());
    return functions ? Result<void>::Success() : Result<void>::Failure(functions.Error());
}

} // namespace rocks
