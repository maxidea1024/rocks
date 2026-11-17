#include <SampleGame/SampleGame.module.generated.h>
#include <Core/Memory/SystemAllocator.h>

int main() {
    rocks::SystemAllocator allocator;
    rocks::TypeRegistry types(allocator);
    // The module aggregate header registers the reflection types of this module and its
    // dependencies, bases first.
    rocks::ModuleRegistration registration(types, allocator);
    if (!rocks::generated::SampleGame::RegisterModuleTypes(registration)) {
        return 1;
    }
    rocks::ObjectRegistry objects(allocator, types, {100, 1});
    auto created = objects.NewObject<sample::GameObject>(
        objects.OwnerAccess(), rocks::GeneratedType<sample::GameObject>::Id());
    if (!created) {
        return 2;
    }
    auto borrowed =
        objects.Resolve<sample::GameObject>(objects.OwnerAccess(), created.Value().Handle());
    if (!borrowed) {
        return 3;
    }
    auto object = borrowed.Value().Get();
    return object && object.Value()->health == 100 ? 0 : 4;
}
