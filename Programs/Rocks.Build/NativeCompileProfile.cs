namespace Rocks.Build;

/// <summary>native compile과 Clang AST 생성이 공유하는 단일 target profile이다.</summary>
public static class NativeCompileProfile
{
    /// <param name="optimization">모듈의 최적화 재정의다. AST 생성도 같은 값을 넘겨 최적화 관련 predefined
    /// macro(__OPTIMIZE__ 등)가 native compile과 같게 한다.</param>
    /// <param name="language">번역 단위 언어다. AST·reflection 생성은 C++만 쓰므로 기본값을 유지한다.</param>
    public static IReadOnlyList<string> Arguments(NativeToolchain toolchain, NativeConfiguration configuration,
        CodeOptimization optimization = CodeOptimization.Default, NativeLanguage language = NativeLanguage.Cpp)
    {
        if (!Enum.IsDefined(configuration))
        {
            throw new ArgumentException($"Invalid native configuration: {configuration}");
        }

        if (!Enum.IsDefined(optimization))
        {
            throw new ArgumentException($"Invalid code optimization: {optimization}");
        }

        if (!Enum.IsDefined(language))
        {
            throw new ArgumentException($"Invalid native language: {language}");
        }

        return toolchain.CompileArguments.Concat(ActionGraph.CompileFlags(toolchain, configuration, optimization, language)).ToArray();
    }

    /// <summary>
    /// 모듈 하나의 compile 프로필이다. 모듈의 최적화 재정의를 따르고, 모듈이 sanitizer 계측을 끄면
    /// (<see cref="NativeModuleInput.SanitizerInstrumentation"/>) 계측 인자를 뺀다. native compile(ActionGraph)과 AST 생성
    /// (MetadataRunner)이 같은 인자를 쓰도록 둘 다 이 함수를 거친다.
    /// </summary>
    public static IReadOnlyList<string> ModuleArguments(NativeToolchain toolchain, NativeConfiguration configuration,
        NativeModuleInput module, NativeLanguage language = NativeLanguage.Cpp)
    {
        IReadOnlyList<string> profile = Arguments(toolchain, configuration, module.Optimization, language);
        return module.SanitizerInstrumentation ? profile : profile.Where(argument => !IsSanitizerArgument(argument)).ToArray();
    }

    /// <summary>sanitizer 계측 인자인가(<c>-fsanitize</c>·<c>/fsanitize</c>로 시작하는 모든 철자, 예: <c>-fsanitize=address</c>,
    /// <c>-fsanitize-address-use-after-scope</c>).</summary>
    public static bool IsSanitizerArgument(string argument) =>
        argument.StartsWith("-fsanitize", StringComparison.Ordinal) || argument.StartsWith("/fsanitize", StringComparison.Ordinal);
}
