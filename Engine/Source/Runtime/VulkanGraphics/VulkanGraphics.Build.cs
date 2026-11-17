using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Vulkan 1.1 graphics backend (Docs/Design/VulkanGraphics.ko.md). The common device sources live in Private/; Private/Win64 and
/// Private/Android hold the loader name, surface creation and host time domain. macOS uses Metal, so device creation there is
/// Unsupported (Private/Unsupported). The Vulkan loader is opened at run time through DynamicLibrary::OpenSystem (vulkan-1.dll,
/// libvulkan.so); nothing links the SDK import library (VK_NO_PROTOTYPES from the VulkanHeaders manifest).
/// </summary>
public sealed class VulkanGraphics : EngineModuleRules
{
    protected override string ModuleName => "VulkanGraphics";

    protected override string[] RequiredModules => ["Graphics"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        string? platform = context.Platform switch
        {
            NativePlatform.Windows => "Win64",
            NativePlatform.Android => "Android",
            _ => null
        };
        IReadOnlyList<string> sources = platform is null
            ? context.Sources($"{backend}/Unsupported")
            : [.. Directory.GetFiles(context.PathInRoot(backend), "*.cpp").Order(StringComparer.Ordinal),
                .. context.Sources($"{backend}/{platform}")];
        return base.Configure(context) with
        {
            Sources = sources,
            // Vulkan headers are visible only to this module's sources (adapter style).
            PrivateIncludeDirectories = [context.PathInRoot(backend), .. ThirdPartyModuleRules.ConsumerIncludeDirectories(context, "VulkanHeaders")],
            PrivateDefines = ThirdPartyModuleRules.ConsumerDefines(context, "VulkanHeaders")
        };
    }
}
