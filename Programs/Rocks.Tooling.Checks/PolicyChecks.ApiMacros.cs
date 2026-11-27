using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    private const string ModuleApiMacroRule = "module-api-macro";

    // 주석·리터럴을 지운 텍스트의 모듈 API 매크로 토큰이다(ROCKS_<MODULE>_API).
    private static readonly Regex ModuleApiMacroToken = new(@"\bROCKS_([A-Z0-9]+)_API\b", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(10));

    /// <summary>
    /// 비공개 헤더지만 선언에 API 매크로를 붙이는 내부 헤더다(Docs/Design/BuildTargets 3.8절 BS8d2 규칙 5의 예외). 둘이다:
    /// <list type="bullet">
    /// <item>규칙 파일이 비공개 include 디렉터리로 다른 모듈에 일부러 보여 주는 헤더 — 다른 모듈이 링크한다(BS8d2 — AsyncCompression이 같은
    /// 코덱·블록 인코딩을 쓴다).</item>
    /// <item>시험이 부르는 내부 선언의 헤더 — Modular에서 시험은 호스트 공유 라이브러리 하나에 있으므로 그 모듈이 export해야 한다(BS8d5 —
    /// 사용자 결정: 시험용 내부 export). Modular는 개발 구성만이라 출시 게임(Release·Monolithic)에는 나가지 않는다(Editor·SDK 개발 라이브러리에는 실린다 — 한계).</item>
    /// </list>
    /// 목록에 없는 비공개 헤더의 매크로는 오류다.
    /// </summary>
    public static readonly IReadOnlyList<string> ExportedPrivateHeaders =
    [
        "Core/Source/Runtime/Compression/Private/CompressionInternal.h",
        "Core/Source/Runtime/Compression/Private/CompressedBlocksInternal.h",
        "Core/Source/Runtime/Core/Private/App/CommandLineStorage.h",
        "Core/Source/Runtime/Core/Private/Diagnostics/LogInternal.h",
        "Core/Source/Runtime/Core/Private/Memory/AddressLedger.h",
        "Core/Source/Runtime/Core/Private/Memory/BinnedAllocatorTestHooks.h",
        "Core/Source/Runtime/Core/Private/Text/DecimalConversion.h",
        "Core/Source/Runtime/Crypto/Private/AeadInternal.h",
        "Core/Source/Runtime/Crypto/Private/Aes/AesKernels.h",
        "Core/Source/Runtime/Crypto/Private/Aes/AesModesInternal.h",
        "Core/Source/Runtime/Crypto/Private/AsyncCryptoInternal.h",
        "Core/Source/Runtime/Crypto/Private/ChannelInternal.h",
        "Core/Source/Runtime/Crypto/Private/SecretArena.h",
        "Core/Source/Runtime/Crypto/Private/SegmentStreamInternal.h",
        "Core/Source/Runtime/Launch/Private/BuildIdentity.h",
        "Core/Source/Runtime/Launch/Private/TerminationRequest.h",
        "Core/Source/Runtime/ModuleLoader/Private/ModuleManifest.h",
        "Core/Source/Runtime/PlatformBase/Private/Common/CrashReportingInternal.h",
        "Core/Source/Runtime/PlatformBase/Private/Common/DirectoryWatchInternal.h",
        "Core/Source/Runtime/PlatformBase/Private/Common/ProcessEntryInternal.h",
        "Core/Source/Runtime/PlatformBase/Private/Common/Sync/QueueMutex.h",
        "Core/Source/Runtime/PlatformBase/Private/Common/Sync/SyncSeams.h",
        "Core/Source/Runtime/PlatformBase/Private/Common/VirtualMemoryBackend.h",
        "Core/Source/Runtime/PlatformBase/Private/Win64/CrashFiles.h",
        "Core/Source/Runtime/PlatformBase/Private/Win64/CrashHelper.h",
        "Core/Source/Runtime/ReflectionText/Private/TextAssignInternal.h",
        "Engine/Source/Runtime/AssetRuntime/Private/PackageInternal.h",
        "Engine/Source/Runtime/AudioMiniaudio/Private/MiniaudioSupport.h",
        "Engine/Source/Runtime/EngineAssets/Private/EngineAssetsInternal.h",
        "Engine/Source/Runtime/PlatformWindow/Private/Common/GamepadMotorMapping.h",
        "Engine/Source/Runtime/PlatformWindow/Private/Common/GamepadSlotTable.h",
        "Engine/Source/Runtime/PlatformWindow/Private/Common/PointerTouchMapping.h",
        "Engine/Source/Runtime/PlatformWindow/Private/Common/TextInputSession.h",
        "Engine/Source/Developer/MeshTools/Private/MeshBuildInternal.h",
        "Engine/Source/Developer/TextureTools/Private/TextureImportInternal.h"
    ];

    /// <summary>
    /// 클래스 전체를 export해야 하는 비리플렉션 클래스다(Docs/Design/BuildTargets 3.8절 BS8d3). 리플렉션 클래스의 루트 rocks::Object는
    /// 모든 모듈의 리플렉션 클래스가 상속하지만 리플렉션 타입이 아니어서 메타 컴파일러의 상속 export 진단(RMC2002)이 보지 못한다.
    /// </summary>
    public static readonly IReadOnlyList<(string Path, string Class)> RequiredClassExports =
    [
        ("Core/Source/Runtime/ObjectRuntime/Public/Object/Object.h", "Object")
    ];

    /// <summary>
    /// 모듈 API 매크로의 쓰임을 검사한다(Docs/Design/BuildTargets 3.8절 BD27·BS8d2, 규칙 <c>module-api-macro</c>). 엔진 소유 모듈
    /// (<c>&lt;루트&gt;/Source/&lt;그룹&gt;/&lt;모듈&gt;/</c>, 서드파티 제외)과 엔진 플러그인 모듈의 소스·헤더에서:
    /// <list type="bullet">
    /// <item>다른 모듈 이름의 매크로(<c>ROCKS_&lt;다른 모듈&gt;_API</c>)를 쓰면 오류다 — 매크로는 그 선언을 정의하는 모듈의 것이어야 한다
    /// (Modular에서 다른 모듈 매크로는 import가 되어 정의가 export되지 않는다).</item>
    /// <item>비공개 헤더(<c>Private/</c>)의 매크로는 오류다(공개 API만 export한다). 예외는 <see cref="ExportedPrivateHeaders"/>다.</item>
    /// <item>소스(<c>.cpp</c>)의 매크로는 오류다 — 선언의 속성이 정의에 이어지므로 정의에는 붙이지 않는다.</item>
    /// </list>
    /// 주석·문자열·문자 리터럴은 보지 않는다. 사용 예: <c>PolicyChecks.ModuleApiMacros(root)</c>.
    /// </summary>
    public static IReadOnlyList<CheckFailure> ModuleApiMacros(string root, IReadOnlyList<string>? exportedPrivateHeaders = null,
        IReadOnlyList<(string Path, string Class)>? requiredClassExports = null)
    {
        exportedPrivateHeaders ??= ExportedPrivateHeaders;
        requiredClassExports ??= RequiredClassExports;
        List<CheckFailure> failures = [];
        // 필수 클래스 export: 그 모듈의 매크로가 클래스 선언부에 있어야 한다(파일이 없으면 검사할 저장소가 아니다 — 시험 루트).
        foreach ((string required, string className) in requiredClassExports)
        {
            string path = Path.Combine(root, required);
            if (!File.Exists(path) || ModuleFolderOf(required) is not (string owner, _))
            {
                continue;
            }

            string code = WithoutCommentsAndLiterals(File.ReadAllText(path, StrictUtf8));
            string macro = "ROCKS_" + owner.ToUpperInvariant() + "_API";
            if (!Regex.IsMatch(code, @"\bclass\s+" + macro + @"\s+" + Regex.Escape(className) + @"\b", RegexOptions.CultureInvariant,
                TimeSpan.FromSeconds(10)))
            {
                failures.Add(new(required, 1, ModuleApiMacroRule,
                    $"class {className} must be exported as a whole (class {macro} {className}): every module's reflected classes derive from it"));
            }
        }

        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            if (ModuleFolderOf(relative) is not (string module, string folder))
            {
                continue;
            }

            string code = WithoutCommentsAndLiterals(File.ReadAllText(path, StrictUtf8));
            bool header = relative.EndsWith(".h", StringComparison.Ordinal);
            foreach (Match match in ModuleApiMacroToken.Matches(code))
            {
                int line = 1 + code.AsSpan(0, match.Index).Count('\n');
                string owner = module.ToUpperInvariant();
                if (match.Groups[1].Value != owner)
                {
                    failures.Add(new(relative, line, ModuleApiMacroRule,
                        $"{match.Value} belongs to another module; a declaration of module {module} uses ROCKS_{owner}_API (the macro of the " +
                        "module that defines it)"));
                }
                else if (!header)
                {
                    failures.Add(new(relative, line, ModuleApiMacroRule,
                        $"{match.Value} is not used in a source file; put it on the declaration in the public header (the definition inherits it)"));
                }
                else if (folder == "Private" && !exportedPrivateHeaders.Contains(relative, StringComparer.Ordinal))
                {
                    failures.Add(new(relative, line, ModuleApiMacroRule,
                        $"{match.Value} is not used in a private header; only public declarations are exported (move the declaration to Public/)"));
                }
            }
        }

        return failures;
    }

    // 엔진 소유 모듈 파일의 (모듈 이름, 모듈 아래 첫 폴더)다: <루트>/Source/<그룹>/<모듈>/<폴더>/… (그룹이 ThirdParty면 아니다) 또는 엔진 플러그인
    // 모듈의 Source/<모듈>/<폴더>/…. 아니면 null이다.
    private static (string Module, string Folder)? ModuleFolderOf(string relative)
    {
        string[] parts = relative.Split('/');
        if (parts.Length > 5 && parts[1] == "Source" && parts[2] != "ThirdParty")
        {
            return (parts[3], parts[4]);
        }

        if (parts.Length >= 7 && parts[0] == "Engine" && parts[1] == "Plugins")
        {
            for (int index = 3; index + 3 < parts.Length; ++index)
            {
                if (parts[index] == "Source")
                {
                    return (parts[index + 1], parts[index + 2]);
                }
            }
        }

        return null;
    }
}
