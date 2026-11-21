namespace Rocks.Build.Cli;

/// <summary>
/// <c>sdk --configurations &lt;목록&gt; --destination &lt;새 디렉터리&gt;</c>: 구성마다 엔진을 빌드해 설치 SDK 하나로 조립한다(Docs/Design/ProjectFiles.ko.md
/// 10.3절). 목록은 쉼표(또는 세미콜론)로 나누고 기본은 Debug·Development·Release다(Asan·Coverage는 적을 때만). 구성 빌드는 각자의 기본 출력
/// (<c>out/dotnet-&lt;platform&gt;-&lt;target&gt;-&lt;구성&gt;</c>)과 증거를 쓴다. <c>--output</c>은 구성이 하나일 때만 받는다. 하나라도 실패하면 SDK를
/// 만들지 않는다. <c>--register on</c>이면 게시한 뒤 등록한다(기본 off, 10.2절).
/// </summary>
internal static class SdkExportCommand
{
    /// <summary>이 명령만 쓰는 옵션이다(공통 파서가 받는 이름 목록에 더한다).</summary>
    internal static readonly string[] Options = ["configurations"];

    /// <summary>구성 목록을 주지 않았을 때의 구성이다(게임 솔루션의 기본 구성).</summary>
    internal static readonly NativeConfiguration[] DefaultConfigurations = [NativeConfiguration.Debug, NativeConfiguration.Development, NativeConfiguration.Release];

    public static async Task<int> RunAsync(string[] args, Dictionary<string, string> options, CancellationToken cancellationToken)
    {
        if (options.ContainsKey("configuration"))
        {
            throw new ArgumentException("sdk takes --configurations <list> (for example Debug,Development,Release), not --configuration.");
        }

        NativeConfiguration[] configurations = options.TryGetValue("configurations", out string? list) ? Parse(list) : DefaultConfigurations;
        if (configurations.Length > 1 && options.ContainsKey("output"))
        {
            throw new ArgumentException("--output names one build output; it is accepted only with a single SDK configuration.");
        }

        string register = options.GetValueOrDefault("register", "off");
        if (register is not ("on" or "off"))
        {
            throw new ArgumentException("register must be on or off.");
        }

        using var assembly = new SdkAssembly(options["destination"]);
        foreach (NativeConfiguration configuration in configurations)
        {
            Console.WriteLine($"SDK configuration {configuration}:");
            int code = await BuildCommand.RunAsync([.. Without(args, "configurations", "register"), "--configuration", configuration.ToString()],
                cancellationToken, null, assembly);
            if (code != 0)
            {
                return code;
            }
        }

        assembly.Publish();
        Console.WriteLine($"Installed SDK: {assembly.Destination} ({string.Join(", ", configurations)})");
        // 등록은 명시적으로만 한다(기본 off — 시험·CI·임시 SDK가 사용자 등록부에 쌓이지 않게, 10.2절).
        if (register == "on")
        {
            Console.WriteLine($"Registered {assembly.Destination} as {SdkRegistries.Default().Register(assembly.Destination)}.");
        }

        return 0;
    }

    /// <summary>구성 목록을 읽는다(쉼표·세미콜론, 이름은 대소문자까지 정확히, 중복·빈 목록 거부). 결과는 구성 순서다.</summary>
    internal static NativeConfiguration[] Parse(string list)
    {
        string[] names = list.Split([',', ';'], StringSplitOptions.TrimEntries);
        var configurations = new SortedSet<NativeConfiguration>();
        foreach (string name in names)
        {
            if (!Enum.GetNames<NativeConfiguration>().Contains(name, StringComparer.Ordinal) || !configurations.Add(Enum.Parse<NativeConfiguration>(name)))
            {
                throw new ArgumentException($"--configurations '{list}': each entry must be one of {string.Join(", ", Enum.GetNames<NativeConfiguration>())}, " +
                    "listed once.");
            }
        }

        return [.. configurations];
    }

    // 명령줄에서 그 옵션(이름과 값)을 뺀다.
    private static IEnumerable<string> Without(string[] args, params string[] names)
    {
        yield return args[0];
        for (int index = 1; index + 1 < args.Length; index += 2)
        {
            if (!names.Contains(args[index].TrimStart('-'), StringComparer.Ordinal))
            {
                yield return args[index];
                yield return args[index + 1];
            }
        }
    }
}
