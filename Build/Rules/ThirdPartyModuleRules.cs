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
/// <item>Third-party modules are dependencies like any other module (Docs/Design/BuildTargets section 3.5 BS3a, UE External modules): the
/// manifest include directories are public includes and its consumerDefines are consumer defines (dependents only; the library's own build
/// defines stay private).
/// Consuming modules list the library as a private dependency (engine public headers never include third-party headers; the public header
/// check catches it). Header-only (<c>kind: headers</c>) and binary (<c>kind: binary</c>, loaded at run time) libraries with include
/// directories are modules without sources. The library folder is the ModuleDirectory, so the include ownership check sees third-party
/// headers.</item>
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
        if (manifest.Kind == ThirdPartyKind.Data || manifest.Kind != ThirdPartyKind.Code && manifest.IncludeDirectories.Count == 0)
        {
            throw new FormatException($"{library}: only a code library or a header-only or binary library with include directories has a module rule " +
                $"(kind is {manifest.Kind})");
        }

        bool code = manifest.Kind == ThirdPartyKind.Code;
        return new()
        {
            Name = ModuleName,
            ModuleDirectory = library,
            Sources = code ? manifest.TranslationUnits.Select(unit => context.PathInRoot($"{library}/{unit}")).ToArray() : [],
            PublicIncludeDirectories = manifest.IncludeDirectories.Select(directory => context.PathInRoot($"{library}/{directory}")).ToArray(),
            // Consumer defines go to dependents only: the library sources #define some of them themselves (BS3a).
            ConsumerDefines = manifest.ConsumerDefines,
            PrivateDefines = code ? manifest.DefinesFor(context.Platform.ToString(), context.Architecture.ToString()) : [],
            // Manifest compileArguments use GCC-style spellings, which clang-cl accepts as is (ThirdPartyManifest.AllowedCompileArguments).
            // The /clang: escape is not used because build input validation rejects it.
            AdditionalCompileArguments = manifest.CompileArguments,
            Optimization = CodeOptimization.Always,
            LinkTimeOptimization = manifest.LinkTimeOptimization,
            SanitizerInstrumentation = manifest.SanitizerInstrumentation,
            // Process-wide state limits direct consumers to one module (Docs/Design/BuildTargets section 3.8, BD29).
            HasGlobalState = manifest.GlobalState,
            // RTTI only for the C++ translation units of a third-party library that needs it (ICU4C, Docs/Design/Compression.ko.md section 3.3).
            RuntimeTypeInformation = manifest.RuntimeTypeInformation,
            // Third-party code has no module lifetime object, so it stays out of the module table (Docs/Design/ModuleLoader section 3.1).
            ThirdParty = true
        };
    }
}
