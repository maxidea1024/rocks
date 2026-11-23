using Rocks.Build;
using Rocks.Build.Rules;

// Host-side logging setup (Docs/Design/LoggingSystem.ko.md sections 5.1 and 9.1): registers the log.* console settings and builds the
// LogSystem with the stderr sink, the optional file sink and the optional sealed file sink from them. It sits above Core, Config and Crypto
// so Core depends on neither. Hosts (Sandbox, game and editor mains) use it; the engine itself never creates or installs a LogSystem.
public sealed class LogHost : CoreModuleRules
{
    protected override string ModuleName => "LogHost";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Config", "Crypto", "Core"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["Compression"];
}
