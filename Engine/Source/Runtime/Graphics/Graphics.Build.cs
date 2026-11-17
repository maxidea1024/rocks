using Rocks.Build.Rules;

// Graphics device contract (device, swap chain, buffers, textures, pipelines, command recording and submission completion fences). Backend implementations
// are the D3D12Graphics and OpenGLGraphics modules. Vulkan and Metal only have a placeholder Backend enumerator; their modules are added
// when implemented (the engine Runtime holds only modules actually in use). Knows nothing about World, Object or window modules (windows
// arrive only as NativeSurface handles). Task is used to chain fence completion to task events (Graphics/GpuFenceEvent.h).
public sealed class Graphics : EngineModuleRules
{
    protected override string ModuleName => "Graphics";

    protected override string[] RequiredModules => ["GraphicsTypes", "Task"];
}
