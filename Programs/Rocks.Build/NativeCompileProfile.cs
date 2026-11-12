namespace Rocks.Build;

/// <summary>native compile과 Clang AST 생성이 공유하는 단일 target profile이다.</summary>
public static class NativeCompileProfile
{
    public static IReadOnlyList<string> Arguments(NativeToolchain toolchain, NativeConfiguration configuration)
    {
        if (!Enum.IsDefined(configuration)) throw new ArgumentException($"Invalid native configuration: {configuration}");
        return toolchain.CompileArguments.Concat(ActionGraph.CompileFlags(toolchain, configuration)).ToArray();
    }
}
