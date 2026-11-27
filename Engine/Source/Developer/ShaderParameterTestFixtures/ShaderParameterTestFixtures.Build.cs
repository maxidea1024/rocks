using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

// Shader parameter struct test fixture module (Docs/Design/ShaderSystem section 16.2 SSD58). Owns ROCKS_SHADER_PARAMETERS structs that cover
// the packing edges (a scalar after a 3-component vector, int and uint slots, arrays, a nested struct, matrices, root constants) and resource
// members (RenderBase device and RenderGraph graph resource member types, section 16.2 SSD59-SSD66). The build
// generates their layout tables and HLSL; only the EngineTests executable depends on it.
public sealed class ShaderParameterTestFixtures : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderParameterTestFixtures";

    // Test fixture: not started by phase (Docs/Design/ModuleLoader section 3.11).
    protected override LoadingPhase LoadingPhase => LoadingPhase.None;

    protected override string[] PublicDependencies => ["ShaderCore", "RenderBase", "RenderGraph"];
}
