using Rocks.Build.Rules;

// Reflection test fixture module (Docs/Design/ReflectionFoundations.ko.md section 1.9). The Foundation and Engine test executables
// depend on it, and this module owns the reflection headers and the identity spec (ReflectionTestFixtures.rocksids.json). Header only.
public sealed class ReflectionTestFixtures : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ReflectionTestFixtures";

    // CoreTypeBindings: the Core value type field fixture (CoreValueFixture.h) imports that module's Binds mirrors.
    // Localization: rocks::Text fields (TextFieldFixture.h).
    protected override string[] RequiredModules => ["ObjectRuntime", "CoreTypeBindings", "Localization"];

    protected override bool HasSources => false;

    protected override IReadOnlyList<string> Metadata =>
        ["ReflectionTestFixtures/ReflectionFixture.h", "ReflectionTestFixtures/AutoIdentityFixture.h", "ReflectionTestFixtures/StructFixture.h",
            "ReflectionTestFixtures/CoreValueFixture.h", "ReflectionTestFixtures/ArrayFixture.h", "ReflectionTestFixtures/PathFixture.h",
            "ReflectionTestFixtures/IntegerWidthFixture.h", "ReflectionTestFixtures/NameFieldFixture.h",
            "ReflectionTestFixtures/WeakRefFixture.h", "ReflectionTestFixtures/ClassRefFixture.h",
            "ReflectionTestFixtures/OptionalFixture.h", "ReflectionTestFixtures/MapFixture.h",
            "ReflectionTestFixtures/ObjectRefMapFixture.h", "ReflectionTestFixtures/StaticArrayFixture.h",
            "ReflectionTestFixtures/FieldFlagFixture.h",
            "ReflectionTestFixtures/ClassFlagFixture.h", "ReflectionTestFixtures/TextFixture.h",
            "ReflectionTestFixtures/AssignFixture.h", "ReflectionTestFixtures/InstancedStructFixture.h",
            "ReflectionTestFixtures/StructInheritanceFixture.h", "ReflectionTestFixtures/TextFieldFixture.h",
            "ReflectionTestFixtures/BitfieldFixture.h", "ReflectionTestFixtures/ValidateFixture.h",
            "ReflectionTestFixtures/BytesFixture.h"];
}
