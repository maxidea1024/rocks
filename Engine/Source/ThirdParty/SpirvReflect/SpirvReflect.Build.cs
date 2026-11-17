using Rocks.Build.Rules;

// Builds the SPIRV-Reflect (Vulkan SDK 1.4.363.0 tag) C translation unit under the Rocks warning rules (Apache-2.0; the bundled spirv.h is MIT).
// The shader worker uses it for SPIR-V reflection (Docs/Design/ShaderSystem.ko.md section 8, item 5). Consumer modules use only the C API (the C++ wrapper
// uses the STL, so SPIRV_REFLECT_DISABLE_CPP_BINDINGS in consumerDefines turns it off). The ThirdParty.json in the same folder
// is the single source of sources, definitions and file hashes.
public sealed class SpirvReflect : ThirdPartyModuleRules
{
    protected override string ModuleName => "SpirvReflect";
}
