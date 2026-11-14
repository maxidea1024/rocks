using System.Text.Json;

namespace Rocks.Build;

internal static class CompilationDatabase
{
    public static string Write(string outputDirectory, string root, NativePlan plan)
    {
        string path = Path.Combine(outputDirectory, "compile_commands.json");
        // 항상 펼친 인자를 쓴다. 실행 때 쓰는 response file(NativeCommandLine)은 여기 나타나지 않는다 — clangd·정책 감사가 실제
        // 인자를 읽어야 하고, policy는 '@' 인자를 펼치지 않은 response file로 거부한다.
        var records = plan.Actions.Where(action => action.Kind == "Compile").Select(action => new
        {
            directory = root,
            file = action.Source,
            output = action.Outputs[0],
            arguments = new[] { action.Tool }.Concat(action.ResolveArguments(action.Outputs)).ToArray()
        });
        AtomicFiles.Write(path, JsonSerializer.Serialize(records, new JsonSerializerOptions { WriteIndented = true }));
        return path;
    }
}
