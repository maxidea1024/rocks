using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// 규칙이 만든 생성 번역 단위(<see cref="NativeModuleInput.GeneratedSources"/>, 셰이더 도구 표가 쓴다)를 검사한다: 실제 빌드에서
/// <c>&lt;out&gt;/GeneratedSources/&lt;모듈&gt;/</c>에 쓰여 컴파일·링크되고, 내용이 같으면 다시 컴파일하지 않으며, 바뀌면 다시 컴파일하고,
/// 잘못된 이름·내용을 거부한다.
/// </summary>
internal static class GeneratedSourceTests
{
    public static async Task RunAsync(NativeBuildRequest request, string root)
    {
        string directory = Directory.CreateDirectory(Path.Combine(root, "generated-source")).FullName;
        string main = Path.Combine(directory, "main.cpp");
        File.WriteAllText(main, "int GeneratedAnswer() noexcept;\nint main() { return GeneratedAnswer() == 5 ? 0 : 9; }\n");
        NativeBuildRequest With(string body) => request with
        {
            OutputDirectory = Path.Combine(root, "generated-out"),
            Modules =
            [
                new() { Name = "GenLib", GeneratedSources = [new("Table.Generated.cpp", body)] },
                new() { Name = "GenGame", Kind = NativeModuleKind.Executable, Sources = [main], Dependencies = ["GenLib"] }
            ],
            Targets = ["GenGame"]
        };

        var builder = new NativeBuildEngine();
        async Task<NativeBuildResult> Build(NativeBuildRequest input)
        {
            NativeBuildResult result = await builder.BuildAsync(input);
            Check(result.Succeeded, "generated source build failed: " + result.Failure);
            return result;
        }

        const string five = "int GeneratedAnswer() noexcept { return 5; }\n";
        NativeBuildResult first = await Build(With(five));
        string written = Path.Combine(root, "generated-out", GeneratedSources.DirectoryName, "GenLib", "Table.Generated.cpp");
        Check(File.ReadAllText(written) == five, "generated source was not written under <out>/GeneratedSources/<module>/");
        Check((await ProcessRunner.RunAsync(new(first.ModuleOutputs["GenGame"], [], root, request.Toolchain.Environment))).ExitCode == 0,
            "generated source was not linked into the executable");

        // 같은 내용이면 다시 컴파일하지 않는다. 바뀌면 그 TU와 link가 다시 돈다.
        NativeBuildResult same = await Build(With(five));
        Check(same.ExecutedActions == 0, $"unchanged generated source rebuilt {same.ExecutedActions} actions");
        NativeBuildResult changed = await Build(With("int GeneratedAnswer() noexcept { return 6; }\n"));
        Check(changed.Actions.Any(action => action.Kind == "Compile" && action.Executed && action.Module == "GenLib"),
            "changed generated source was not recompiled");
        Check((await ProcessRunner.RunAsync(new(changed.ModuleOutputs["GenGame"], [], root, request.Toolchain.Environment))).ExitCode == 9,
            "changed generated source did not reach the executable");

        // 이름은 구분자 없는 .cpp·.c, 모듈 안에서 겹치지 않고, 내용에 NUL이 없다.
        foreach (NativeGeneratedSource[] invalid in new NativeGeneratedSource[][]
                 {
                     [new("", five)], [new("dir/Table.cpp", five)], [new("dir\\Table.cpp", five)], [new("C:Table.cpp", five)],
                     [new("Table.h", five)], [new("..", five)], [new(".cpp", five)], [new("Ta\nble.cpp", five)],
                     [new("Table.cpp", five), new("table.CPP", five)], [new("Table.cpp", "int x;\0")],
                     [new("Table.cpp", five), new("TABLE.cpp", five)]
                 })
        {
            NativeBuildRequest bad = request with { Modules = [new() { Name = "GenLib", GeneratedSources = invalid }] };
            try
            {
                InputValidation.Validate(bad);
            }
            catch (ArgumentException error) when (error.Message.Contains("generated source", StringComparison.Ordinal))
            {
                continue;
            }

            throw new InvalidOperationException($"invalid generated source accepted: {string.Join(", ", invalid.Select(item => item.FileName))}");
        }
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
