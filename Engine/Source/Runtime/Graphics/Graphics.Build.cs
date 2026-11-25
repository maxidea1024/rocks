using Rocks.Build;
using Rocks.Build.Rules;

// Graphics device contract (device, swap chain, buffers, textures, pipelines, command recording and submission completion fences). Backend implementations
// are the D3D12Graphics, D3D11Graphics, OpenGLGraphics and VulkanGraphics modules. Metal only has a placeholder Backend enumerator; its module is
// added when implemented (the engine Runtime holds only modules actually in use). Knows nothing about World, Object or window modules (windows
// arrive only as NativeSurface handles). Task is used to chain fence completion to task events (Graphics/GpuFenceEvent.h). ShaderCore gives the
// cooked binding layout and GLES binding header formats that device shaders carry (Docs/Design/ShaderSystem section 12.2.1 SSD1).
public sealed class Graphics : EngineModuleRules
{
    protected override string ModuleName => "Graphics";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["GraphicsTypes", "ShaderCore", "Task"];
}
