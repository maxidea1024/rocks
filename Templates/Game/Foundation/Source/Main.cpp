#include <GameObject.generated.h>
#include <SampleGame/SampleGame.module.generated.h>
#include <Launch/EntryPoint.h>

namespace {

// Game body. Launch (the common entry layer) calls it after installing crash reporting and
// capturing the command line; the return value is the process exit code.
int Run(rocks::launch::ApplicationContext& context) noexcept {
    rocks::Allocator& allocator = context.SystemAllocator();
    rocks::TypeRegistry types(allocator);
    // RegisterModuleTypes (declared in the module generated header, defined in the module generated
    // source that the build compiles into this executable) registers the reflection types of this
    // module and its dependencies, bases first. GeneratedType<sample::GameObject> comes from the
    // generated header of that type, <GameObject.generated.h>.
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

// The name names crash report folders (Saved/Crashes/SampleGame-<start UTC>-<pid>/).
const rocks::launch::ApplicationDescription& Describe() noexcept {
    static constexpr rocks::launch::ApplicationDescription Description{
        .name = "SampleGame", .kind = rocks::launch::ApplicationKind::EngineHost, .run = &Run};
    return Description;
}

} // namespace

// Defines the platform entry point (wmain on Windows, main elsewhere).
ROCKS_IMPLEMENT_APPLICATION(Describe)
