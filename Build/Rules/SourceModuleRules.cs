using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

/// <summary>
/// Common rules that build paths from a single module folder (counterpart of UE ModuleRules.ModuleDirectory).
/// Module folder layout:
/// <code>
/// &lt;Layer&gt;/Source/&lt;Group&gt;/&lt;Module&gt;/
///   &lt;Module&gt;.Build.cs   rules for this module (derives from CoreModuleRules or EngineModuleRules)
///   Public/              headers that other modules include
///   Private/             implementation (.cpp) and module-internal headers
///   Tests/               module tests (collected by the test executable rules; not part of the module library)
/// </code>
/// Layer is Core (foundation libraries) or Engine (game engine layer). Core modules never reference Engine modules.
/// </summary>
public abstract class SourceModuleRules : ModuleRules
{
    /// <summary>Top-level layer folder the module belongs to (Core or Engine).</summary>
    protected abstract string Layer { get; }

    /// <summary>Module name. Must match the folder name.</summary>
    protected abstract string ModuleName { get; }

    /// <summary>
    /// Public dependencies (counterpart of UE PublicDependencyModuleNames, Docs/Design/BuildTargets.ko.md section 3.5): modules whose headers this
    /// module's public headers use. Their public include paths and defines reach this module and its consumers. Only lower modules of the same
    /// layer or modules of a lower layer. A module may appear in only one of the two lists.
    /// </summary>
    protected abstract string[] PublicDependencies { get; }

    /// <summary>
    /// Private dependencies (counterpart of UE PrivateDependencyModuleNames): modules used only by this module's implementation. Their public
    /// include paths and defines reach this module but not its consumers; they are still linked. Example:
    /// <code>
    /// protected override string[] PrivateDependencies => ["Compression"];
    /// </code>
    /// </summary>
    protected virtual string[] PrivateDependencies => [];

    /// <summary>
    /// Modules loaded at run time with <c>ModuleLoader::Load(name)</c> instead of being linked as dependencies (counterpart of UE
    /// ModuleRules.DynamicallyLoadedModuleNames, Docs/Design/ModuleLoader.ko.md section 3.8). Their headers are not on this module's include
    /// path; code reaches them through an interface in a linked module and <c>Find&lt;T&gt;(name)</c>. A monolithic executable that reaches this
    /// module links them too. A listed module may depend back on this module. Example:
    /// <code>
    /// protected override string[] DynamicModules => ["DynamicModuleTestFixture"];
    /// </code>
    /// </summary>
    protected virtual string[] DynamicModules => [];

    /// <summary>
    /// Loading phase of this module (counterpart of UE ELoadingPhase, Docs/Design/ModuleLoader.ko.md sections 3.3 and 3.11). The module loader starts
    /// the module when that phase runs: EarliestPossible right after the loader is created (boot modules), PostConfigInit after the host
    /// settings and log start, PreDefault, Default and PostDefault inside Engine::Initialize, PostEngineInit after the engine client starts,
    /// None only when code calls ModuleLoader::Load. A dependency must not use a later phase than this module (None is allowed). The layer rules
    /// set the default: Core modules PostConfigInit (host services and foundation libraries, like UE LoadCoreModules), Engine modules PreDefault
    /// (before game and plugin Default modules, like UE LoadPreInitModules). Example:
    /// <code>
    /// protected override LoadingPhase LoadingPhase => LoadingPhase.None;
    /// </code>
    /// </summary>
    protected virtual LoadingPhase LoadingPhase => LoadingPhase.Default;

    /// <summary>Group under Source (Runtime, Developer or Editor).</summary>
    protected virtual string Group => "Runtime";

    /// <summary>
    /// Module type (Docs/Design/BuildTargets.ko.md section 3.9, BD38). Engine modules take it from the Group folder; plugin modules take it
    /// from the plugin descriptor. The Release exclusion policy reads it. Rules that override Configure start from base.Configure so the
    /// type is kept.
    /// </summary>
    protected virtual ModuleType ModuleType => Group switch
    {
        "Developer" => ModuleType.Developer,
        "Editor" => ModuleType.Editor,
        _ => ModuleType.Runtime
    };

    /// <summary>Whether Private/ contains implementation sources. Header-only modules set this to false.</summary>
    protected virtual bool HasSources => true;

    /// <summary>
    /// Optimization override for this module (counterpart of UE ModuleRules.OptimizeCode). The default follows the configuration.
    /// Example: to step through only this module in a Development build, temporarily set Never.
    /// <code>
    /// protected override CodeOptimization Optimization => CodeOptimization.Never;
    /// </code>
    /// Never also disables optimization and ThinLTO in Release, so do not commit it. Always optimizes even in Debug.
    /// </summary>
    protected virtual CodeOptimization Optimization => CodeOptimization.Default;

    /// <summary>
    /// Unity build setting of this module (Docs/Design/UnityBuildPch.ko.md section 6.1). On compiles the module's C++ sources as
    /// unity batches and makes the module a target of the unity collision policy checks (file-local names, file-scope using, .cpp
    /// macros, pragma balance). Turn it on only after the module passes those checks:
    /// <code>
    /// protected override UnityMode Unity => UnityMode.On;
    /// </code>
    /// The setting takes effect only on the platforms listed in <see cref="RepositoryUnity.VerifiedPlatforms"/>.
    /// </summary>
    protected virtual UnityMode Unity => UnityMode.Default;

    /// <summary>
    /// Sources that stay standalone translation units when Unity is on (absolute paths, same spelling as Sources). Use it for files
    /// that need their own translation unit: object-level checks, derived third-party code, special target attributes. Example:
    /// <code>
    /// protected override IReadOnlyList&lt;string&gt; UnityExclude(BuildContext context) =>
    ///     [context.PathInRoot($"{ModuleDirectory}/Private/Aes/AesX64.cpp")];
    /// </code>
    /// </summary>
    protected virtual IReadOnlyList<string> UnityExclude(BuildContext context) => [];

    /// <summary>
    /// Module PCH header relative to the module folder, or null (Docs/Design/UnityBuildPch.ko.md section 8.1). The build precompiles it and
    /// force-includes it into every C++ translation unit of the module; sources never include it themselves and must compile without it
    /// (--pch off). Example:
    /// <code>
    /// protected override string? PrivatePchHeader => "Private/CorePch.h";
    /// </code>
    /// </summary>
    protected virtual string? PrivatePchHeader => null;

    /// <summary>
    /// Shared PCH header of this module, spelled relative to Public/, or null (section 8.4). Modules without their own PCH that reach this
    /// module precompile the header with their own arguments and use it. Example:
    /// <code>
    /// protected override string? SharedPchHeader => "Core/CoreSharedPch.h";
    /// </code>
    /// </summary>
    protected virtual string? SharedPchHeader => null;

    /// <summary>The Unity setting to put into NativeModuleInput.Unity for this build context. Rules that build their own NativeModuleInput use it.</summary>
    protected UnityMode UnityFor(BuildContext context) => RepositoryUnity.Effective(Unity, context);

    /// <summary>Module folder relative to the repository root. Example: Core/Source/Runtime/Core.</summary>
    protected virtual string ModuleDirectory => $"{Layer}/Source/{Group}/{ModuleName}";

    /// <summary>
    /// Absolute path of the module entry point source <c>Private/&lt;ModuleName&gt;Module.cpp</c>, which holds the module's
    /// <c>ROCKS_IMPLEMENT_MODULE</c> (Docs/Design/ModuleLoader.ko.md section 3.2). Rules that compile only platform subdirectories of
    /// Private/ add it so that the entry point is built on every platform:
    /// <code>
    /// Sources = [ModuleEntrySource(context), .. context.Sources($"{ModuleDirectory}/Private/Win64")],
    /// </code>
    /// </summary>
    protected string ModuleEntrySource(BuildContext context) => context.PathInRoot($"{ModuleDirectory}/Private/{ModuleName}Module.cpp");

    /// <summary>
    /// Include spellings of reflection metadata headers (ROCKS_CLASS declarations), relative to Public/ or Private/. When not empty,
    /// the module folder must contain the identity manifest <c>&lt;ModuleName&gt;.rocksids.json</c>. Example:
    /// <code>
    /// protected override IReadOnlyList&lt;string&gt; Metadata =&gt; ["ReflectionTestFixtures/ReflectionFixture.h"];
    /// </code>
    /// The generated header is included as <c>&lt;ReflectionTestFixtures/ReflectionFixture.generated.h&gt;</c>.
    /// Write it as a list of string literals only (no identifiers, <c>new[] { }</c>, helper calls, spreads or interpolated strings). The policy check
    /// reads this list verbatim and rejects ROCKS_PROPERTY and ROCKS_NATIVE in files outside it, so anything other than literals is
    /// reported as <c>reflection-metadata-not-literal</c>.
    /// </summary>
    protected virtual IReadOnlyList<string> Metadata => [];

    /// <summary>
    /// Include paths used only by this module's sources (not propagated to dependents). Used by third-party library adapters.
    /// Example: <c>ThirdPartyModuleRules.ConsumerIncludeDirectories(context, "Zstd")</c>.
    /// </summary>
    protected virtual IReadOnlyList<string> PrivateIncludeDirectories(BuildContext context) => [];

    /// <summary>Defines used only by this module's sources (not propagated to dependents).</summary>
    protected virtual IReadOnlyList<string> PrivateDefines(BuildContext context) => [];

    public override NativeModuleInput Configure(BuildContext context) => HasSources
        ? new()
        {
            Name = ModuleName,
            Sources = context.Sources($"{ModuleDirectory}/Private"),
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            PrivateIncludeDirectories = PrivateIncludeDirectories(context),
            PrivateDefines = PrivateDefines(context),
            PublicDependencies = PublicDependencies,
            PrivateDependencies = PrivateDependencies,
            DynamicModules = DynamicModules,
            LoadingPhase = LoadingPhase,
            Optimization = Optimization,
            Unity = UnityFor(context),
            UnityExclude = UnityExclude(context),
            PrivatePchHeader = PrivatePchHeader is null ? null : context.PathInRoot($"{ModuleDirectory}/{PrivatePchHeader}"),
            SharedPchHeader = SharedPchHeader,
            Type = ModuleType,
            Metadata = Metadata,
            ModuleDirectory = ModuleDirectory
        }
        : new()
        {
            Name = ModuleName,
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            PublicDependencies = PublicDependencies,
            PrivateDependencies = PrivateDependencies,
            DynamicModules = DynamicModules,
            LoadingPhase = LoadingPhase,
            Type = ModuleType,
            Metadata = Metadata,
            ModuleDirectory = ModuleDirectory
        };
}

/// <summary>
/// Repository-wide unity build policy of the module rules (Docs/Design/UnityBuildPch.ko.md section 9.3).
/// </summary>
public static class RepositoryUnity
{
    /// <summary>
    /// Platforms where the unity batches of this repository have been compiled and verified. A module's Unity = On takes effect only
    /// on these platforms. On the others the hand-written sources stay standalone translation units until a build on that platform
    /// verifies the batches (platform-specific sources and platform-conditional code are not compiled on other hosts). Reflection
    /// generated sources are batched on every platform regardless of this list.
    /// Windows and Android (NDK cross build) are verified on the development host, macOS by the CI macOS job (there is no local
    /// macOS toolchain, so a change to macOS-only sources is verified by a CI run).
    /// </summary>
    public static readonly IReadOnlyList<NativePlatform> VerifiedPlatforms =
        [NativePlatform.Windows, NativePlatform.Android, NativePlatform.MacOS];

    /// <summary>The Unity setting to use for a build context: On is kept only on verified platforms.</summary>
    public static UnityMode Effective(UnityMode requested, BuildContext context) =>
        requested == UnityMode.On && !VerifiedPlatforms.Contains(context.Platform) ? UnityMode.Off : requested;
}

/// <summary>
/// Rules for foundation library (Core layer) modules: the base shared by the engine, tools and games rather than the engine itself.
/// Usage example (Core/Source/Runtime/Core/Core.Build.cs):
/// <code>
/// public sealed class Core : CoreModuleRules
/// {
///     protected override string ModuleName => "Core";
///     protected override string[] PublicDependencies => ["PlatformBase"];
/// }
/// </code>
/// </summary>
public abstract class CoreModuleRules : SourceModuleRules
{
    protected override string Layer => "Core";

    // Core modules start in the host group, after the host settings and the log (UE LoadCoreModules, Docs/Design/ModuleLoader.ko.md
    // section 3.11), so host service modules live across engines and PostConfigInit modules may depend on them.
    protected override LoadingPhase LoadingPhase => LoadingPhase.PostConfigInit;
}

/// <summary>Rules for game engine layer (Engine/Source) modules. May depend on Core modules.</summary>
public abstract class EngineModuleRules : SourceModuleRules
{
    protected override string Layer => "Engine";

    // Engine modules start first in the engine group, before game and plugin Default modules (UE LoadPreInitModules, Docs/Design/ModuleLoader.ko.md
    // section 3.11), so a PreDefault game or plugin module may depend on them.
    protected override LoadingPhase LoadingPhase => LoadingPhase.PreDefault;
}

/// <summary>
/// Rules for engine plugin modules (Engine/Plugins, Docs/Design/BuildTargets.ko.md section 3.9, BD37). The plugin descriptor
/// (&lt;Plugin&gt;.rocksplugin) is the single source of the module's folder, type and loading phase: the module lives in
/// &lt;Plugin&gt;/Source/&lt;Module&gt;/ (UE plugin layout) and its rules only name the module and list dependencies. A module that no
/// descriptor declares is an error. Usage example (Engine/Plugins/Importers/UnrealExchange/Source/UnrealExchange/UnrealExchange.Build.cs):
/// <code>
/// public sealed class UnrealExchange : PluginModuleRules
/// {
///     protected override string ModuleName => "UnrealExchange";
///     protected override string[] PublicDependencies => ["Core"];
/// }
/// </code>
/// Configure is sealed: the build of a disabled plugin still evaluates these rules, so they must not depend on anything but the context.
/// </summary>
public abstract class PluginModuleRules : SourceModuleRules
{
    private PluginDescriptor? plugin;

    private ModuleDescriptor? descriptor;

    private string? moduleDirectory;

    protected sealed override string Layer => "Engine";

    protected sealed override string ModuleDirectory =>
        moduleDirectory ?? throw new InvalidOperationException($"Module '{ModuleName}': ModuleDirectory is known only inside Configure.");

    protected sealed override ModuleType ModuleType => Descriptor.Type;

    protected sealed override LoadingPhase LoadingPhase => Descriptor.LoadingPhase;

    /// <summary>Plugin that declares this module. Available inside Configure.</summary>
    protected PluginDescriptor Plugin =>
        plugin ?? throw new InvalidOperationException($"Module '{ModuleName}': the plugin is known only inside Configure.");

    private ModuleDescriptor Descriptor =>
        descriptor ?? throw new InvalidOperationException($"Module '{ModuleName}': the descriptor is known only inside Configure.");

    public sealed override NativeModuleInput Configure(BuildContext context)
    {
        var owners = context.Plugins.Where(candidate => candidate.Modules.Any(module => module.Name == ModuleName)).ToArray();
        if (owners.Length != 1)
        {
            throw new ArgumentException(owners.Length == 0
                ? $"Module '{ModuleName}' derives from PluginModuleRules, but no plugin under {context.PluginRoot} declares it in \"modules\"."
                : $"Module '{ModuleName}' is declared by several plugins: {string.Join(", ", owners.Select(owner => owner.Name))}.");
        }

        plugin = owners[0];
        descriptor = plugin.Modules.Single(module => module.Name == ModuleName);
        moduleDirectory = Path.GetRelativePath(context.RootDirectory, Path.Combine(plugin.RootDirectory, "Source", ModuleName)).Replace(Path.DirectorySeparatorChar, '/');
        return base.Configure(context);
    }
}
