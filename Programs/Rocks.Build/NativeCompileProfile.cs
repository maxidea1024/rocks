namespace Rocks.Build;

/// <summary>native compile과 Clang AST 생성이 공유하는 단일 target profile이다.</summary>
public static class NativeCompileProfile
{
    /// <param name="optimization">모듈의 최적화 재정의다. AST 생성도 같은 값을 넘겨 최적화 관련 predefined
    /// macro(__OPTIMIZE__ 등)가 native compile과 같게 한다.</param>
    public static IReadOnlyList<string> Arguments(NativeToolchain toolchain, NativeConfiguration configuration,
        CodeOptimization optimization = CodeOptimization.Default)
    {
        if (!Enum.IsDefined(configuration)) throw new ArgumentException($"Invalid native configuration: {configuration}");
        if (!Enum.IsDefined(optimization)) throw new ArgumentException($"Invalid code optimization: {optimization}");
        return toolchain.CompileArguments.Concat(ActionGraph.CompileFlags(toolchain, configuration, optimization)).ToArray();
    }
}
