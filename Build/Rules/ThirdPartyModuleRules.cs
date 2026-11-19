using Rocks.Build;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

/// <summary>
/// Rules for pinned-version third-party library modules (counterpart of UE ThirdParty modules). Sources, defines and includes
/// are read from the library folder's <c>ThirdParty.json</c> (single source of truth). A library used by Core lives under
/// <c>Core/Source/ThirdParty</c>; one used only by the engine or tools lives under <c>Engine/Source/ThirdParty</c>
/// (<see cref="ThirdPartyManifest.ThirdPartyRoots"/>). Folder layout:
/// <code>
/// &lt;Root&gt;/&lt;Module&gt;/
///   &lt;Module&gt;.Build.cs   rule deriving from this class (states only the name)
///   ThirdParty.json      version, origin, license, per-file SHA-256, translation units, defines (checked by the third-party policy check)
///   &lt;lib&gt;-&lt;version&gt;/      release sources kept byte for byte (never modified; excluded from format and lexical checks)
///   *.c                  Rocks glue, only when needed (subject to policy checks)
/// </code>
/// Rule contract:
/// <list type="bullet">
/// <item>Third-party headers are not propagated. PublicIncludeDirectories is empty, and consuming modules take
/// <see cref="ConsumerIncludeDirectories"/> and <see cref="ConsumerDefines"/> privately. Engine public headers never include
/// third-party headers.</item>
/// <item>Always optimized (<see cref="CodeOptimization.Always"/>) so that asset decoding is not drastically slower in Debug
/// builds. CRT, configuration macros and NDEBUG follow the configuration. A manifest with <c>linkTimeOptimization: false</c> is
/// excluded from ThinLTO even in Release, and one with <c>sanitizerInstrumentation: false</c> is not instrumented even in Asan.</item>
/// <item>The rule loader runs Configure of every rule on each build, so a manifest error becomes a build error immediately.</item>
/// </list>
/// Usage example (Core/Source/ThirdParty/Zstd/Zstd.Build.cs):
/// <code>
/// public sealed class Zstd : ThirdPartyModuleRules
/// {
///     protected override string ModuleName => "Zstd";
/// }
/// </code>
/// </summary>
public abstract class ThirdPartyModuleRules : ModuleRules
{
    /// <summary>Module name. Must match the library folder name.</summary>
    protected abstract string ModuleName { get; }

    /// <summary>
    /// Library folder relative to the repository root, e.g. Core/Source/ThirdParty/Zstd. It is looked up in the root list;
    /// a missing library or one present under both roots throws <see cref="FormatException"/> (a build error).
    /// </summary>
    public static string LibraryDirectory(BuildContext context, string module) => ThirdPartyManifest.FindLibrary(context.RootDirectory, module);

    public override NativeModuleInput Configure(BuildContext context)
    {
        string library = LibraryDirectory(context, ModuleName);
        ThirdPartyManifest manifest = ThirdPartyManifest.Read(context.PathInRoot(library));
        if (manifest.Kind != ThirdPartyKind.Code)
        {
            throw new FormatException($"{library}: only a code library has a module rule (kind is {manifest.Kind})");
        }

        return new()
        {
            Name = ModuleName,
            Sources = manifest.TranslationUnits.Select(unit => context.PathInRoot($"{library}/{unit}")).ToArray(),
            PrivateIncludeDirectories = manifest.IncludeDirectories.Select(directory => context.PathInRoot($"{library}/{directory}")).ToArray(),
            PrivateDefines = manifest.DefinesFor(context.Platform.ToString(), context.Architecture.ToString()),
            // Manifest compileArguments use GCC-style spellings, which clang-cl accepts as is (ThirdPartyManifest.AllowedCompileArguments).
            // The /clang: escape is not used because build input validation rejects it.
            AdditionalCompileArguments = manifest.CompileArguments,
            Optimization = CodeOptimization.Always,
            LinkTimeOptimization = manifest.LinkTimeOptimization,
            SanitizerInstrumentation = manifest.SanitizerInstrumentation,
            // Third-party code has no module lifetime object, so it stays out of the module table (Docs/Design/ModuleLoader.ko.md section 3.1).
            ThirdParty = true
        };
    }

    /// <summary>
    /// Paths from which an adapter (a consuming module's Private sources) includes third-party headers. Multiple libraries are
    /// concatenated in order. Used for code libraries as well as header-only libraries (<c>kind: headers</c>, include only, no
    /// module dependency) and the committed public headers of binary libraries (<c>kind: binary</c>).
    /// Usage example: <c>ThirdPartyModuleRules.ConsumerIncludeDirectories(context, "VulkanHeaders", "SpirvReflect")</c>.
    /// </summary>
    public static IReadOnlyList<string> ConsumerIncludeDirectories(BuildContext context, params string[] modules) =>
        modules.SelectMany(module =>
        {
            string library = LibraryDirectory(context, module);
            return ThirdPartyManifest.Read(context.PathInRoot(library)).IncludeDirectories
                .Select(directory => context.PathInRoot($"{library}/{directory}"));
        }).ToArray();

    /// <summary>
    /// Defines that must match the library build when an adapter includes third-party headers (e.g. zlib Z_PREFIX). The build
    /// tool rejects the same name with conflicting values.
    /// </summary>
    public static IReadOnlyList<string> ConsumerDefines(BuildContext context, params string[] modules) =>
        modules.SelectMany(module => ThirdPartyManifest.Read(context.PathInRoot(LibraryDirectory(context, module))).ConsumerDefines)
            .Distinct(StringComparer.Ordinal).ToArray();
}
