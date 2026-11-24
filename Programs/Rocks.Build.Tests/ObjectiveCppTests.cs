using Rocks.Build;

/// <summary>
/// Objective-C++ 번역 단위(<c>.mm</c>, Docs/Design/MacPlatform.ko.md 6절 MD2)를 검사한다.
/// <list type="bullet">
/// <item>정확히 소문자 <c>.mm</c>만 Objective-C++다.</item>
/// <item>Apple 대상의 Objective-C++ 인자는 C++ 인자에 <c>-fno-objc-arc</c>(MRC — MD4)만 더한 것이다.</item>
/// <item>compile action은 <c>-x objective-c++</c>로 언어를 명시하고, 유니티 묶음·PCH에 들지 않는다.</item>
/// <item>Apple이 아닌 대상에 <c>.mm</c>을 넘기면 입력 검증이 거부한다.</item>
/// </list>
/// </summary>
internal static class ObjectiveCppTests
{
    public static void Run(NativeBuildRequest request, string root)
    {
        NativeToolchain toolchain = request.Toolchain;
        bool apple = toolchain.Platform == NativePlatform.MacOS;

        Check(ActionGraph.LanguageOf("a.mm") == NativeLanguage.ObjectiveCpp, ".mm must be Objective-C++");
        Check(ActionGraph.LanguageOf("a.MM") == NativeLanguage.Cpp && ActionGraph.LanguageOf("a.m") == NativeLanguage.Cpp &&
              ActionGraph.LanguageOf("a.cpp") == NativeLanguage.Cpp, "only the exact lowercase .mm extension is Objective-C++");

        // Apple 대상: C++ 프로필 + -fno-objc-arc(언어 인자 바로 뒤). 나머지 순서·값은 C++와 같다.
        if (toolchain.Platform != NativePlatform.Windows)
        {
            string[] cpp = ActionGraph.CompileFlags(toolchain, NativeConfiguration.Debug).ToArray();
            string[] objectiveCpp = ActionGraph.CompileFlags(toolchain, NativeConfiguration.Debug, CodeOptimization.Default,
                NativeLanguage.ObjectiveCpp).ToArray();
            Check(objectiveCpp.Count(flag => flag == "-fno-objc-arc") == 1, "Objective-C++ must disable ARC once");
            Check(objectiveCpp.Where(flag => flag != "-fno-objc-arc").SequenceEqual(cpp),
                "Objective-C++ flags must equal the C++ profile plus -fno-objc-arc:\n" + string.Join(' ', objectiveCpp));
        }

        // .mm 원본이 든 모듈: Apple은 -x objective-c++ 단독 번역 단위, 다른 대상은 입력 오류다.
        string fixture = Directory.CreateDirectory(Path.Combine(root, "objective-cpp")).FullName;
        string bridge = Path.Combine(fixture, "Bridge.mm");
        File.WriteAllText(bridge, "int RocksObjectiveCppValue() { return 7; }\n");
        NativeBuildRequest withBridge = request with
        {
            Modules = request.Modules.Select(module => module.Name == "Engine" ? module with { Sources = [.. module.Sources, bridge] } : module)
                .ToArray()
        };
        if (apple)
        {
            InputValidation.Validate(withBridge);
            NativeAction compile = ActionGraph.Create(withBridge).Actions.Single(action =>
                action.Kind == "Compile" && action.Arguments.Contains(bridge));
            int language = compile.Arguments.ToList().IndexOf("-x");
            Check(language >= 0 && compile.Arguments[language + 1] == "objective-c++",
                "Objective-C++ compile must pass -x objective-c++: " + string.Join(' ', compile.Arguments));
            Check(compile.Arguments.Contains("-fno-objc-arc"), "Objective-C++ compile must disable ARC");
            Check(!compile.Arguments.Any(argument => argument.EndsWith(".Unity.cpp", StringComparison.Ordinal)),
                "Objective-C++ sources must not join a unity batch");
        }
        else
        {
            try
            {
                InputValidation.Validate(withBridge);
                throw new InvalidOperationException("Objective-C++ source accepted on a non-Apple toolchain");
            }
            catch (ArgumentException error)
            {
                Check(error.Message.Contains("Apple-only", StringComparison.Ordinal) && error.Message.Contains("Bridge.mm", StringComparison.Ordinal),
                    "unexpected diagnostic for a non-Apple .mm source: " + error.Message);
            }
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
