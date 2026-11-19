using Rocks.Build;
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

    /// <summary>Names of direct module dependencies. Only lower modules of the same layer or modules of a lower layer.</summary>
    protected abstract string[] RequiredModules { get; }

    /// <summary>Group under Source (Runtime, Developer or Editor).</summary>
    protected virtual string Group => "Runtime";

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

    /// <summary>The Unity setting to put into NativeModuleInput.Unity for this build context. Rules that build their own NativeModuleInput use it.</summary>
    protected UnityMode UnityFor(BuildContext context) => RepositoryUnity.Effective(Unity, context);

    /// <summary>Module folder relative to the repository root. Example: Core/Source/Runtime/Core.</summary>
    protected string ModuleDirectory => $"{Layer}/Source/{Group}/{ModuleName}";

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
            Dependencies = RequiredModules,
            Optimization = Optimization,
            Unity = UnityFor(context),
            UnityExclude = UnityExclude(context),
            Metadata = Metadata,
            ModuleDirectory = ModuleDirectory
        }
        : new()
        {
            Name = ModuleName,
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            Dependencies = RequiredModules,
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
    /// Windows and Android (NDK cross build) are verified on the development host. macOS is not listed: there is no local macOS
    /// toolchain, so add it after a macOS build compiles every module's sources as one translation unit.
    /// </summary>
    public static readonly IReadOnlyList<NativePlatform> VerifiedPlatforms = [NativePlatform.Windows, NativePlatform.Android];

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
///     protected override string[] RequiredModules => ["PlatformBase"];
/// }
/// </code>
/// </summary>
public abstract class CoreModuleRules : SourceModuleRules
{
    protected override string Layer => "Core";
}

/// <summary>Rules for game engine layer (Engine/Source) modules. May depend on Core modules.</summary>
public abstract class EngineModuleRules : SourceModuleRules
{
    protected override string Layer => "Engine";
}
