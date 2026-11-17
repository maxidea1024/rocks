using Rocks.Build.Rules;

// Engine module test executable target. Foundation tests up to the Object family; Core tests only the lower modules without Object.
// The ReflectionTestFixtures module owns the reflection fixtures and their identity spec (the test executable depends on that module).
public sealed class FoundationTarget : TargetRules
{
    public override string Name => "Foundation";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["FoundationTests"];

    public override bool CheckPublicHeaders => true;

    public override string TestExecutable => "FoundationTests";
}

// Engine layer (World, Engine, assets) test target. Also runs all Foundation tests. Tests that need a GPU do not go into
// this target (Sandbox --self-test).
public sealed class EngineTestsTarget : TargetRules
{
    public override string Name => "EngineTests";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["EngineTests"];

    public override bool CheckPublicHeaders => true;

    public override string TestExecutable => "EngineTests";
}

// SDK target holding only the reflection fixture modules (and their dependencies). Exported when Rocks.Build.IntegrationTests tests SDK descriptor import
// (an external game module derives from and references reflection types of SDK modules without regenerating those modules).
// ObjectSerialization: the game saves and restores array fields of SDK types as a graph (B5c integration test).
public sealed class ReflectionFixturesTarget : TargetRules
{
    public override string Name => "ReflectionFixtures";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["ReflectionTestFixtures", "ObjectSerialization"];
}

public sealed class CoreTarget : TargetRules
{
    public override string Name => "Core";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["CoreFoundationTests"];

    public override bool CheckPublicHeaders => true;

    public override string TestExecutable => "CoreFoundationTests";
}
