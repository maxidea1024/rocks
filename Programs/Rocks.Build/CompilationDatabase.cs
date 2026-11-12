using System.Text.Json;

namespace Rocks.Build;

internal static class CompilationDatabase
{
    public static string Write(string outputDirectory, string root, NativePlan plan)
    {
        string path = Path.Combine(outputDirectory, "compile_commands.json");
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
