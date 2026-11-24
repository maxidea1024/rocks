using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

// Common process entry layer (UE Launch counterpart, Docs/Design/CrashHandlingAndLaunch.ko.md sections 6-8): the platform entry macro
// (ROCKS_IMPLEMENT_APPLICATION), early crash reporting installation, process command line capture, build identity, the tool log
// service, stage marks, termination requests and the exit code. It never depends on Engine, so console tools link only this module.
public sealed class Launch : CoreModuleRules
{
    protected override string ModuleName => "Launch";

    // Launch owns the process module loader (Docs/Design/ModuleLoader sections 3.9 and 3.11).
    protected override string[] PublicDependencies => ["Core", "PlatformBase"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["ModuleLoader"];

    // Boot module: started by the loader right after it is created, together with its whole dependency closure.
    protected override LoadingPhase LoadingPhase => LoadingPhase.EarliestPossible;

    // The link closure of this module is the boot module set; every module in it must use the EarliestPossible phase.
    // The Android activity layer (Private/Android — ANativeActivity_onCreate, Docs/Design/AndroidPlatform.ko.md section 3) is built for
    // Android only, as its own translation unit: console executables do not reference it, so the static library member is not linked.
    public override NativeModuleInput Configure(BuildContext context)
    {
        NativeModuleInput module = base.Configure(context);
        string android = context.PathInRoot($"{ModuleDirectory}/Private/Android");
        bool androidTarget = context.Platform == NativePlatform.Android;
        return module with
        {
            ProvidesLaunch = true,
            Sources = [.. module.Sources.Where(source => androidTarget || !source.StartsWith(android, StringComparison.OrdinalIgnoreCase))],
            UnityExclude = androidTarget ? [.. module.UnityExclude, .. context.Sources(android)] : module.UnityExclude,
            SystemLibraries = androidTarget ? [.. module.SystemLibraries, "android", "log"] : module.SystemLibraries
        };
    }
}
