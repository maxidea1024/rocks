using Rocks.Build;

/// <summary>
/// 모듈의 플랫폼 시스템 라이브러리·Apple framework 선언(UE PublicSystemLibraries·PublicFrameworks 대응)이 최종 링크에서
/// 플랫폼 형식으로 바뀌고 의존 모듈로 전파되는지, 잘못된 이름과 Apple이 아닌 toolchain의 framework를 거부하는지 검사한다.
/// </summary>
internal static class SystemLibraryTests
{
    public static void Run(NativeBuildRequest request)
    {
        bool windows = request.Toolchain.Platform == NativePlatform.Windows;
        bool apple = request.Toolchain.Platform == NativePlatform.MacOS;

        NativeBuildRequest With(Func<NativeModuleInput, NativeModuleInput> engine, Func<NativeModuleInput, NativeModuleInput>? game = null) =>
            request with
            {
                Modules = request.Modules.Select(module => module.Name switch
                {
                    "Engine" => engine(module),
                    "Game" when game != null => game(module),
                    _ => module
                }).ToArray()
            };
        IReadOnlyList<string> Link(NativeBuildRequest input) =>
            ActionGraph.Create(input).Actions.Single(action => action.Kind == "Link").Arguments;
        string Library(string name) => windows ? name + ".lib" : "-l" + name;

        // 정적 라이브러리(Engine)의 선언이 실행 파일(Game) 링크로 전파되고, 두 모듈이 같은 이름을 선언해도 한 번만 들어간다.
        var declared = With(engine => engine with { SystemLibraries = ["rocks_sys_a", "rocks_sys_b"] },
            game => game with { SystemLibraries = ["rocks_sys_a"] });
        var link = Link(declared);
        Check(link.Count(argument => argument == Library("rocks_sys_a")) == 1, "duplicate system library must link once");
        Check(link.Contains(Library("rocks_sys_b")), "dependency system library must propagate to the executable link");
        // 선언이 바뀌면 link action의 재빌드 판정 키(인자 포함)가 바뀌어 다시 링크한다.
        string LinkKey(NativeBuildRequest input) => ActionGraph.Create(input).Actions.Single(action => action.Kind == "Link")
            .Key(request.Toolchain.Identity, request.OutputDirectory, request.RootDirectory);
        Check(LinkKey(request) != LinkKey(declared), "system library change must change the link action key");

        // framework는 Apple toolchain에서만 "-framework <이름>"이 되고, 다른 toolchain에서는 입력 오류다.
        var framework = With(engine => engine with { Frameworks = ["CoreServices"] });
        if (apple)
        {
            var arguments = Link(framework).ToArray();
            int index = Array.IndexOf(arguments, "-framework");
            Check(index >= 0 && index + 1 < arguments.Length && arguments[index + 1] == "CoreServices",
                "Apple link must pass -framework CoreServices");
        }
        else
        {
            Rejects(framework, "Apple-only", "framework accepted on a non-Apple toolchain");
        }

        // 이름만 받는다: 경로·플래그·확장자·빈 이름은 플랫폼 중립성을 깨므로 거부한다.
        foreach (string name in new[] { "", "-lfoo", "/DEFAULTLIB:foo", "foo bar", "dir/foo", "dir\\foo", "C:foo", "foo.lib",
                     "libfoo.a", "libfoo.so", "foo.dylib", "Foo.framework", "foo.tbd", "@response" })
        {
            Rejects(With(engine => engine with { SystemLibraries = [name] }), "bare library or framework name",
                $"system library '{name}' accepted");
            if (apple)
                Rejects(With(engine => engine with { Frameworks = [name] }), "bare library or framework name",
                    $"framework '{name}' accepted");
        }
        // 버전 접미사·밑줄·점·하이픈·더하기는 실제 시스템 라이브러리 이름에 쓰이므로 받는다.
        foreach (string name in new[] { "d3d12", "user32", "c++", "z", "android-log", "vulkan_1", "GL.1" })
            InputValidation.Validate(With(engine => engine with { SystemLibraries = [name] }));
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
                throw new InvalidOperationException(message + ": unexpected diagnostic " + error.Message);
            return;
        }
        throw new InvalidOperationException(message);
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }
}
