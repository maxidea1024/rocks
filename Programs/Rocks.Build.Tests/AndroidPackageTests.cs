using Rocks.Build;

/// <summary>
/// Android APK 실행 파일(<see cref="NativeModuleInput.AndroidPackage"/>, Docs/Design/AndroidPlatform.ko.md 3·7절)을 검사한다.
/// <list type="bullet">
/// <item>Android가 아닌 대상·정적 라이브러리·Modular·잘못된 패키지 이름은 입력 검증이 거부한다.</item>
/// <item>NDK가 있으면 Android 계획에서 실행 파일이 <c>bin/lib&lt;이름&gt;.so</c> 공유 라이브러리로 링크되고(-shared·soname·진입 기호
/// 강제 참조·16 KB 페이지), 그 모듈의 번역 단위가 <c>ROCKS_ANDROID_APK=1</c>을 받는다.</item>
/// </list>
/// </summary>
internal static class AndroidPackageTests
{
    public static async Task RunAsync(NativeBuildRequest request)
    {
        var package = new AndroidPackage("com.rocks.fixture");
        NativeBuildRequest WithPackage(NativeBuildRequest input, AndroidPackage value, string module = "Game") => input with
        {
            Modules = input.Modules.Select(candidate => candidate.Name == module ? candidate with { AndroidPackage = value } : candidate)
                .ToArray()
        };

        if (request.Toolchain.Platform != NativePlatform.Android)
        {
            Rejects(WithPackage(request, package), "Android-only", "AndroidPackage accepted on a non-Android toolchain");
        }

        NativeToolchain android;
        try
        {
            android = await NativeToolchains.DiscoverAndroidAsync("x86_64", 26);
        }
        catch (AndroidNdkNotConfiguredException)
        {
            Console.WriteLine("PASS Android APK link plan skipped (NDK not configured)");
            return;
        }

        NativeBuildRequest cross = request with { Toolchain = android };
        Rejects(WithPackage(cross, package, "Engine"), "applies only to executables", "AndroidPackage accepted on a static library");
        Rejects(WithPackage(cross, package) with { LinkType = NativeLinkType.Modular }, "Monolithic", "AndroidPackage accepted on a Modular target");
        foreach (string name in new[] { "rocks", "com..rocks", "com.1rocks", "com.rocks-sandbox", "com.rocks.", "" })
        {
            Rejects(WithPackage(cross, new AndroidPackage(name)), "invalid Android package name", $"package name '{name}' accepted");
        }

        NativeBuildRequest apk = WithPackage(cross, package);
        InputValidation.Validate(apk);
        NativePlan plan = ActionGraph.Create(apk);
        NativeAction link = plan.Actions.Single(action => action.Kind == "Link" && action.Module == "Game");
        string library = AndroidPackage.LibraryFileName("Game");
        Check(link.Outputs.Count != 0 && Path.GetFileName(link.Outputs[0]) == library && Path.GetFileName(Path.GetDirectoryName(link.Outputs[0])!) == "bin",
            "APK executable must link bin/" + library + ": " + string.Join(' ', link.Outputs));
        foreach (string argument in new[] { "-shared", "-Wl,-soname," + library, "-Wl,-u," + AndroidPackage.EntrySymbol, "-Wl,-z,max-page-size=16384" })
        {
            Check(link.Arguments.Contains(argument), "APK link must pass " + argument + ": " + string.Join(' ', link.Arguments));
        }

        // 정의는 모듈 정의 헤더(강제 include)로 들어간다. APK 모듈만 받고 의존 모듈로 전파하지 않는다.
        NativeModuleInput game = apk.Modules.Single(module => module.Name == "Game");
        NativeModuleInput engine = apk.Modules.Single(module => module.Name == "Engine");
        Check(ModuleDefinitions.Of(game, [engine], false, NativeLinkType.Monolithic, NativePlatform.Android).Contains(AndroidPackage.ApkDefine),
            "APK executable translation units must receive " + AndroidPackage.ApkDefine);
        Check(!ModuleDefinitions.Of(engine, [], false, NativeLinkType.Monolithic, NativePlatform.Android).Contains(AndroidPackage.ApkDefine),
            "the APK define must not propagate to dependencies");

        // 패키지가 없는 Android 실행 파일은 그대로 bin/Game(실행 파일)이다.
        NativeAction plain = ActionGraph.Create(cross).Actions.Single(action => action.Kind == "Link" && action.Module == "Game");
        Check(Path.GetFileName(plain.Outputs[0]) == "Game" && !plain.Arguments.Contains("-shared"), "plain Android executable changed");
        Console.WriteLine("PASS Android APK executables: validation, shared library link and APK define");
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
