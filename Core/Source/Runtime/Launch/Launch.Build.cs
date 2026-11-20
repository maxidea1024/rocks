using Rocks.Build.Rules;

// Common process entry layer (UE Launch counterpart, Docs/Design/CrashHandlingAndLaunch.ko.md sections 6-8): the platform entry macro
// (ROCKS_IMPLEMENT_APPLICATION), early crash reporting installation, process command line capture, build identity, the tool log
// service, stage marks, termination requests and the exit code. It never depends on Engine, so console tools link only this module.
public sealed class Launch : CoreModuleRules
{
    protected override string ModuleName => "Launch";

    protected override string[] RequiredModules => ["Core", "PlatformBase"];
}
