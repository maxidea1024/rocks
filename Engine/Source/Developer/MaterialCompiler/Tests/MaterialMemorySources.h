#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <MaterialCompiler/MaterialGraphSnapshot.h>
#include <MaterialCompiler/MaterialSourceProvider.h>

// 함수·레이어 시험의 메모리 원본 공급이다(Docs/Design/MaterialSystem 6.12절 MS2e2).
namespace material_function_fixture {

using namespace rocks;
using namespace rocks::material;

// 메모리 provider다(시험 — snapshot 포인터 목록).
class MemorySources final : public MaterialSourceProvider {
public:
    explicit MemorySources(Allocator& allocator) noexcept
        : functions_(allocator), materials_(allocator) {
    }

    bool AddFunction(const MaterialGraphSnapshot& snapshot) noexcept {
        return functions_.TryPushBack(&snapshot).IsOk();
    }

    bool AddMaterial(const MaterialGraphSnapshot& snapshot) noexcept {
        return materials_.TryPushBack(&snapshot).IsOk();
    }

    const MaterialGraphSnapshot* FindFunction(Guid asset) const noexcept override {
        return Find(functions_, asset);
    }

    const MaterialGraphSnapshot* FindMaterial(Guid asset) const noexcept override {
        return Find(materials_, asset);
    }

private:
    static const MaterialGraphSnapshot* Find(const Array<const MaterialGraphSnapshot*>& list,
                                             Guid asset) noexcept {
        for (const MaterialGraphSnapshot* snapshot : list) {
            if (snapshot->asset == asset) {
                return snapshot;
            }
        }

        return nullptr;
    }

    Array<const MaterialGraphSnapshot*> functions_;
    Array<const MaterialGraphSnapshot*> materials_;
};

} // namespace material_function_fixture
