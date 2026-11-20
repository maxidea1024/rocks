using Rocks.Build;

/// <summary>
/// Windows 실행 파일 링크 인자(Docs/Design/CrashHandlingAndLaunch.ko.md 7.4·14.4절)를 검사한다. 실행 파일의 subsystem은 기본이
/// <c>/SUBSYSTEM:CONSOLE</c>이고 <see cref="WindowsSubsystem.Windows"/>이면 <c>/SUBSYSTEM:WINDOWS</c>다. 정확히 하나만 들어가고, 바꾸면
/// link action key가 바뀐다. 정적 라이브러리의 Windows subsystem은 입력 오류다. 모든 구성이 PDB 경로를 파일 이름만 남기도록
/// <c>/PDBALTPATH:%_PDB%</c>를 넣는다. 다른 플랫폼 toolchain은 subsystem 인자를 넣지 않는다.
/// </summary>
internal static class WindowsSubsystemTests
{
    public static void Run(NativeBuildRequest request)
    {
        bool windows = request.Toolchain.Platform == NativePlatform.Windows;

        NativeBuildRequest With(string name, Func<NativeModuleInput, NativeModuleInput> change) => request with
        {
            Modules = request.Modules.Select(module => module.Name == name ? change(module) : module).ToArray()
        };
        NativeAction Link(NativeBuildRequest input) => ActionGraph.Create(input).Actions.Single(action => action.Kind == "Link");
        string LinkKey(NativeBuildRequest input) => Link(input).Key(request.Toolchain.Identity, request.OutputDirectory, request.RootDirectory);
        int Count(IReadOnlyList<string> arguments, string prefix) =>
            arguments.Count(argument => argument.StartsWith(prefix, StringComparison.Ordinal));

        var console = With("Game", game => game with { WindowsSubsystem = WindowsSubsystem.Console });
        var gui = With("Game", game => game with { WindowsSubsystem = WindowsSubsystem.Windows });
        if (windows)
        {
            var defaults = Link(request).Arguments;
            Check(Count(defaults, "/SUBSYSTEM:") == 1 && defaults.Contains("/SUBSYSTEM:CONSOLE"), "default executable must link /SUBSYSTEM:CONSOLE");
            Check(Link(console).Arguments.SequenceEqual(defaults), "explicit Console must match the default link");
            var windowed = Link(gui).Arguments;
            Check(Count(windowed, "/SUBSYSTEM:") == 1 && windowed.Contains("/SUBSYSTEM:WINDOWS"), "Windows subsystem must link /SUBSYSTEM:WINDOWS");
            Check(LinkKey(request) != LinkKey(gui), "subsystem change must change the link action key");
            foreach (NativeConfiguration configuration in Enum.GetValues<NativeConfiguration>())
            {
                var arguments = Link(request with { Configuration = configuration }).Arguments;
                Check(arguments.Count(argument => argument == "/PDBALTPATH:%_PDB%") == 1 && Count(arguments, "/PDBALTPATH:") == 1,
                    configuration + " link must record only the PDB file name (/PDBALTPATH:%_PDB%)");
                Check(arguments.Contains("/PDB:{OUTPUT1}"), configuration + " link must keep the PDB output");
            }
        }
        else
        {
            // 다른 플랫폼은 subsystem을 쓰지 않는다(같은 규칙이 모든 플랫폼에서 돈다).
            Check(Link(gui).Arguments.SequenceEqual(Link(request).Arguments), "WindowsSubsystem must not change a non-Windows link");
            Check(Count(Link(request).Arguments, "/SUBSYSTEM:") == 0 && Count(Link(request).Arguments, "/PDBALTPATH:") == 0,
                "non-Windows link must not receive Windows linker options");
        }

        // subsystem은 실행 파일에만 뜻이 있다. 정적 라이브러리의 Windows subsystem과 정의되지 않은 값은 거부한다.
        Rejects(With("Engine", engine => engine with { WindowsSubsystem = WindowsSubsystem.Windows }), "applies only to executables",
            "static library accepted WindowsSubsystem.Windows");
        Rejects(With("Game", game => game with { WindowsSubsystem = (WindowsSubsystem)7 }), "unsupported WindowsSubsystem",
            "undefined WindowsSubsystem accepted");
        InputValidation.Validate(gui);
        InputValidation.Validate(With("Engine", engine => engine with { WindowsSubsystem = WindowsSubsystem.Console }));
    }

    private static void Rejects(NativeBuildRequest request, string diagnostic, string message)
    {
        try
        {
            InputValidation.Validate(request);
        }
        catch (ArgumentException error)
        {
            if (!error.Message.Contains(diagnostic, StringComparison.Ordinal))
            {
                throw new InvalidOperationException(message + ": unexpected diagnostic " + error.Message);
            }

            return;
        }

        throw new InvalidOperationException(message);
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
