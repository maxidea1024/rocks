using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>메타 단계 증분 모드다(Docs/Design/BuildReuse 2.7절 — <c>ROCKS_META_INCREMENTAL</c>).</summary>
public enum MetadataIncrementalMode
{
    /// <summary>판정·기록 없음(계측만).</summary>
    Off,

    /// <summary>판정하고 기록하지만 늘 생성한다. would-hit인데 생성물이 다르면 알린다.</summary>
    Shadow
}

/// <summary>
/// 빌드 호출 하나의 메타 단계(리플렉션 생성·셰이더 파라미터 생성·안정 ID 검사) 상태다(Docs/Design/BuildReuse 2장).
/// <list type="bullet">
/// <item>입력 메모(<see cref="Memo"/>): 판정과 기록이 읽은 입력. native 엔진도 같은 메모를 받는다(<see cref="NativeBuildRequest.InputMemo"/>).</item>
/// <item>기록(<c>&lt;out&gt;/Metadata/meta-cache.json</c>): 끝(<see cref="Finish"/>)의 안정 검사를 통과한 실행만 저장한다.</item>
/// <item>보고(<c>&lt;out&gt;/Metadata/meta-report.json</c>, 2.8절): 모듈별 시간·CPU·최대 working set·MetaCompiler 단계 시간·판정. 늘 쓴다.</item>
/// <item>호출마다 한 번 푸는 값: clang-format 실행 파일(<see cref="ClangFormat"/> — MetaCompiler에 <c>--clang-format</c>으로 넘긴다), dotnet
/// 공유 런타임 목록, 툴체인 key, 도구 디렉터리 digest.</item>
/// </list>
/// 사용 예: <c>var session = MetadataSession.Create(context, toolchain); … MetadataRunner.GenerateAsync(…, session: session); …
/// session.Finish(succeeded: true);</c>.
/// </summary>
public sealed class MetadataSession
{
    /// <summary>모드 환경 변수 이름이다.</summary>
    public const string ModeVariable = "ROCKS_META_INCREMENTAL";

    /// <summary>메타 action key의 형식 판이다(바꾸면 모든 메타 기록이 miss다).</summary>
    public const string KeyFormat = "rocks-meta-v1";

    /// <summary>보고 파일 형식 판이다.</summary>
    public const int ReportFormat = 1;

    private const string RecordFileName = "meta-cache.json";

    private const string ReportFileName = "meta-report.json";

    private readonly NativeToolchain toolchain;

    private readonly List<(string Id, StepRecord Record)> pending = [];

    private readonly List<JsonObject> modules = [];

    private readonly System.Diagnostics.Stopwatch total = System.Diagnostics.Stopwatch.StartNew();

    private readonly Lock gate = new();

    private MetadataSession(MetadataIncrementalMode mode, string root, string output, NativeToolchain toolchain, string clangFormat)
    {
        Mode = mode;
        Root = Path.GetFullPath(root);
        OutputDirectory = Path.GetFullPath(output);
        this.toolchain = toolchain;
        ClangFormat = clangFormat;
        Directory = Path.Combine(OutputDirectory, "Metadata");
        Store = mode == MetadataIncrementalMode.Off ? null : new StepRecordStore(Path.Combine(Directory, RecordFileName));
        ToolchainOwned = ToolchainIdentity.OwnedFilter(toolchain);
    }

    /// <summary>모드다.</summary>
    public MetadataIncrementalMode Mode { get; }

    /// <summary>입력 메모다(메타 단계와 native 엔진이 함께 쓴다).</summary>
    public InputMemo Memo { get; } = new();

    /// <summary>작업 사본 루트다.</summary>
    public string Root { get; }

    /// <summary>빌드 출력 폴더다.</summary>
    public string OutputDirectory { get; }

    /// <summary>기록·보고 디렉터리(<c>&lt;out&gt;/Metadata</c>)다.</summary>
    public string Directory { get; }

    /// <summary>판정 기록이다(Off면 null).</summary>
    public StepRecordStore? Store { get; }

    /// <summary>clang-format 실행 파일의 절대 경로다.</summary>
    public string ClangFormat { get; }

    /// <summary>툴체인 소유 파일인가(의존 기록에서 뺀다).</summary>
    public Func<string, bool> ToolchainOwned { get; }

    /// <summary>shadow에서 would-hit인데 생성물이 달랐던 action 수다.</summary>
    public int ShadowMismatches { get; private set; }

    /// <summary>
    /// 모드(<see cref="ModeVariable"/>: 없음·<c>shadow</c>·<c>off</c>)와 clang-format(<c>ROCKS_CLANG_FORMAT</c>, 없으면 툴체인 환경 PATH의
    /// <c>clang-format</c>)을 정해 세션을 만든다.
    /// </summary>
    /// <exception cref="ArgumentException">모드 값이 틀렸거나 아직 쓸 수 없다(<c>on</c>).</exception>
    /// <exception cref="FileNotFoundException">clang-format을 찾지 못했다.</exception>
    public static MetadataSession Create(BuildContext context, NativeToolchain toolchain)
    {
        string? value = Environment.GetEnvironmentVariable(ModeVariable);
        MetadataIncrementalMode mode = value switch
        {
            null or "" or "shadow" => MetadataIncrementalMode.Shadow,
            "off" => MetadataIncrementalMode.Off,
            "on" => throw new ArgumentException($"{ModeVariable}=on is not available yet: shadow mode must first show zero mismatches on real builds " +
                "(Docs/Design/BuildReuse section 2.7)."),
            _ => throw new ArgumentException($"{ModeVariable} must be shadow or off (got '{value}').")
        };
        string formatterName = toolchain.Environment.FirstOrDefault(pair => string.Equals(pair.Key, "ROCKS_CLANG_FORMAT",
            OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal)).Value is { Length: > 0 } configured
            ? configured : "clang-format";
        string formatter = NativeEnvironment.FindExecutable(formatterName, toolchain.Environment);
        return new MetadataSession(mode, context.RootDirectory, context.OutputDirectory, toolchain, formatter);
    }

    /// <summary>툴체인 key다(처음 값을 메모하고 끝에 다시 대조한다).</summary>
    public string ToolchainKey => Memo.Track("toolchain", () => ToolchainIdentity.Key(toolchain));

    /// <summary>clang-format 실행 파일의 경로와 내용 digest다.</summary>
    public string ClangFormatDigest => ToolchainIdentity.Digest([ClangFormat, Memo.Content(ClangFormat)]);

    /// <summary>
    /// MetaCompiler 도구 ID다: compilerTool이 있는 디렉터리 전체(하위 <c>runtimes/</c>·위성 어셈블리 포함)의 경로와 내용, 그리고 dotnet 공유
    /// 런타임 목록(<c>dotnet --list-runtimes</c>). 둘 다 끝의 안정 검사가 다시 잰다.
    /// </summary>
    public string ToolId(string compilerTool)
    {
        string directory = Path.GetDirectoryName(Path.GetFullPath(compilerTool))!;
        string tool = Memo.Track("tool:" + directory, () => ToolchainIdentity.ContentDigest([directory]));
        string runtimes = Memo.Track("dotnet-runtimes", ListRuntimes);
        return ToolchainIdentity.Digest([tool, runtimes]);
    }

    /// <summary>판정·기록 대상인가(Off가 아니다).</summary>
    public bool Records => Mode != MetadataIncrementalMode.Off;

    /// <summary>action의 새 기록을 끝의 저장 대기에 둔다.</summary>
    public void Pend(string id, StepRecord record)
    {
        lock (gate)
        {
            pending.Add((id, record));
        }
    }

    /// <summary>shadow 불일치 하나를 센다(호출자가 이미 출력 줄을 냈다).</summary>
    public void NoteShadowMismatch()
    {
        lock (gate)
        {
            ++ShadowMismatches;
        }
    }

    /// <summary>보고에 모듈 하나를 더한다(모듈 순서 그대로).</summary>
    public void Report(JsonObject entry)
    {
        lock (gate)
        {
            modules.Add(entry);
        }
    }

    /// <summary>
    /// 메타 단계를 끝낸다: 안정 검사(<see cref="InputMemo.FirstChange"/> — 입력·도구 디렉터리·clang-format·툴체인 key)를 통과하면 대기 기록을
    /// 저장하고, 보고를 쓰고, 요약 줄을 낸다. succeeded가 거짓이면(메타 단계 실패) 기록을 저장하지 않는다.
    /// </summary>
    public void Finish(bool succeeded, IReadOnlyDictionary<string, long>? stepMilliseconds = null)
    {
        string? change = succeeded && Records ? Memo.FirstChange() : null;
        bool saved = false;
        if (succeeded && Records && Store is not null)
        {
            if (change is null)
            {
                foreach ((string id, StepRecord record) in pending)
                {
                    Store.Put(id, record);
                }

                Store.Save();
                saved = true;
            }
            else
            {
                Console.WriteLine($"  Rocks metadata: inputs changed during the build ({change}); incremental records were not saved.");
            }
        }

        int hits = modules.Count(entry => entry["decision"]?["wouldHit"]?.GetValue<bool>() == true);
        var report = new JsonObject
        {
            ["format"] = ReportFormat,
            ["mode"] = Mode.ToString().ToLowerInvariant(),
            ["succeeded"] = succeeded,
            ["totalMilliseconds"] = (long)total.Elapsed.TotalMilliseconds,
            ["modules"] = new JsonArray([.. modules.Select(entry => (JsonNode)entry.DeepClone())]),
            ["steps"] = stepMilliseconds is null ? null : new JsonObject(stepMilliseconds.Select(pair => KeyValuePair.Create(pair.Key, (JsonNode?)pair.Value))),
            ["shadowMismatches"] = ShadowMismatches,
            ["stabilityChange"] = change,
            ["recordsSaved"] = saved ? pending.Count : 0
        };
        System.IO.Directory.CreateDirectory(Directory);
        AtomicFile.WriteIfChanged(Path.Combine(Directory, ReportFileName),
            new UTF8Encoding(false).GetBytes(report.ToJsonString(new JsonSerializerOptions { WriteIndented = true }).Replace("\r\n", "\n", StringComparison.Ordinal) + "\n"));
        string judged = Records ? $"; {hits} would be up to date; {ShadowMismatches} shadow mismatches" : "";
        Console.WriteLine(string.Create(System.Globalization.CultureInfo.InvariantCulture,
            $"  Rocks metadata: {modules.Count} actions{judged}; {total.Elapsed.TotalSeconds:F1} s (mode {Mode.ToString().ToLowerInvariant()})"));
    }

    // dotnet 공유 런타임 목록이다(도구의 runtimeconfig와 함께 실제로 묶일 런타임을 정한다). 실패는 숨기지 않는다.
    private string ListRuntimes()
    {
        ProcessResult result = ProcessRunner.RunAsync(new("dotnet", ["--list-runtimes"], Root, toolchain.Environment, Timeout: TimeSpan.FromMinutes(1)))
            .GetAwaiter().GetResult();
        if (result.ExitCode != 0)
        {
            throw new InvalidOperationException("dotnet --list-runtimes failed: " + result.StdErr);
        }

        return ToolchainIdentity.Digest([result.StdOut.Replace("\r\n", "\n", StringComparison.Ordinal)]);
    }
}
