using System.Text.Json;

namespace Rocks.Build;

internal static class CompilationDatabase
{
    public static string Write(string outputDirectory, string root, NativePlan plan)
    {
        string path = Path.Combine(outputDirectory, "compile_commands.json");
        // 항상 펼친 인자를 쓴다. 실행 때 쓰는 response file(NativeCommandLine)은 여기 나타나지 않는다 — clangd·정책 감사가 실제
        // 인자를 읽어야 하고, policy는 '@' 인자를 펼치지 않은 response file로 거부한다.
        // 원본 소스마다 논리 명령 하나다(Docs/Design/UnityBuildPch.ko.md 8.6절). depfile 인자 같은 실행 전용 인자는 없다.
        var records = plan.LogicalCompiles.Select(compile => new
        {
            directory = root,
            file = compile.Source,
            output = compile.Object,
            arguments = new[] { compile.Tool }.Concat(NativeAction.Resolve(compile.Arguments, [compile.Object])).ToArray()
        });
        AtomicFiles.Write(path, JsonSerializer.Serialize(records, new JsonSerializerOptions { WriteIndented = true }));
        return path;
    }
}
