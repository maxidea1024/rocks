using Rocks.Build;
using Rocks.Build.Rules;

// Texture asset types (Docs/Design/AssetSystem section 12, stage A5d — section 12.8): the reflected settings enums (UE 5.8 values),
// TextureSource (editor-only decoded pixels), TexturePlatformData (cooked data), Texture and the five dimension classes (Texture2D,
// TextureCube, Texture2DArray, TextureCubeArray, VolumeTexture), and the contract/hidden setting check CheckTextureSettings. Engine
// Runtime module without a graphics device dependency: AssetRuntime for Asset, StreamableRenderAsset and BulkDataRef, CoreTypeBindings
// for the Vec2d/Vec4d/Color mirrors and GraphicsTypes for the PixelFormat numbers stored in TexturePlatformData. Generated headers are
// <Texture/<Header>.generated.h>, module registration is rocks::generated::Texture::RegisterModuleTypes (called by
// RegisterTextureTypes), and the identity spec is Texture.rocksids.json.
public sealed class Texture : EngineModuleRules
{
    protected override string ModuleName => "Texture";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ObjectRuntime", "CoreTypeBindings", "AssetRuntime", "GraphicsTypes"];
}
