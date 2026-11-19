using Rocks.Build;
using Rocks.Build.Rules;

// Device-independent input events and per-frame input state (keyboard, mouse and gamepad). The OS input backend in PlatformWindow
// converts input into this module's events. Headless tests inject events directly.
public sealed class Input : EngineModuleRules
{
    protected override string ModuleName => "Input";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] RequiredModules => ["Core"];
}
