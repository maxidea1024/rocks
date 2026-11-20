using Rocks.Build;
using Rocks.Build.Rules;

// General-purpose crypto module (Docs/Design/Crypto.ko.md): libsodium-based primitives, streaming and key management, a HW-only lightweight AES layer,
// and asynchronous segment processing on WorkerScheduler (AsyncCrypto.h, Task module).
// External library (Core/Source/ThirdParty/Sodium) headers and definitions are exposed only to this module's Private implementation. Public headers do not
// include sodium.h. The class is named CryptoModule so it is not confused with System.Security.Cryptography (same approach as CompressionModule).
public sealed class CryptoModule : CoreModuleRules
{
    protected override string ModuleName => "Crypto";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // The AES kernels stay standalone translation units: each is derived third-party code for one architecture (Derivation.json) with
    // per-function target attributes, and both declare the same file-local kernel names.
    protected override IReadOnlyList<string> UnityExclude(BuildContext context) =>
        [context.PathInRoot($"{ModuleDirectory}/Private/Aes/AesX64.cpp"), context.PathInRoot($"{ModuleDirectory}/Private/Aes/AesArm64.cpp")];

    protected override string[] PublicDependencies => ["Core", "Task", "Sodium"];

    protected override IReadOnlyList<string> PrivateIncludeDirectories(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerIncludeDirectories(context, "Sodium");

    protected override IReadOnlyList<string> PrivateDefines(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerDefines(context, "Sodium");
}
