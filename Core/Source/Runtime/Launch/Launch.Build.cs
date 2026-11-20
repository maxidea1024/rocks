using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

// Common process entry layer (UE Launch counterpart, Docs/Design/CrashHandlingAndLaunch.ko.md sections 6-8): the platform entry macro
// (ROCKS_IMPLEMENT_APPLICATION), early crash reporting installation, process command line capture, build identity, the tool log
// service, stage marks, termination requests and the exit code. It never depends on Engine, so console tools link only this module.
public sealed class Launch : CoreModuleRules
{
    protected override string ModuleName => "Launch";

    // Launch owns the process module loader (Docs/Design/ModuleLoader.ko.md sections 3.9 and 3.11).
    protected override string[] RequiredModules => ["Core", "ModuleLoader", "PlatformBase"];

    // Boot module: started by the loader right after it is created, together with its whole dependency closure.
    protected override LoadingPhase LoadingPhase => LoadingPhase.EarliestPossible;

    // The link closure of this module is the boot module set; every module in it must use the EarliestPossible phase.
    public override NativeModuleInput Configure(BuildContext context) => base.Configure(context) with { ProvidesLaunch = true };
}
