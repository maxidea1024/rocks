using Rocks.Build;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

/// <summary>
/// 셰이더 도구 표 모듈이다(Docs/Design/ShaderSystem.ko.md 3·9절). Engine 루트 <c>kind: binary</c> manifest에서 만든 생성 번역 단위
/// (<see cref="ShaderToolTableSource"/>)가 빌드 대상 호스트의 도구 이름·버전·저장소 디렉터리 이름·파일 SHA-256·role을 담는다. 메타데이터만
/// 가지며 파일을 열지 않는다. 메인 프로세스(Developer/ShaderCompiler)는 job 키를, 워커는 도구 열기를 이 표로 한다. 이 모듈을 참조할 수
/// 있는 것은 ShaderCompiler·워커·ShaderFormat 모듈과 워커 자리 모듈 ShaderToolLoader뿐이다(정책 검사 shader-tool-table).
/// </summary>
public sealed class ShaderToolTable : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderToolTable";

    protected override string[] RequiredModules => ["Core"];

    public override NativeModuleInput Configure(BuildContext context) => base.Configure(context) with
    {
        GeneratedSources = [new(ShaderToolTableSource.FileName, ShaderToolTableSource.Generate(context.RootDirectory, HostKey(context)))]
    };

    // 빌드 대상의 binary 호스트 키다(ThirdPartyManifest.HostKeys). 도구는 개발 호스트(Windows·macOS)에서만 돌므로 Android는 null이다.
    private static string? HostKey(BuildContext context)
    {
        string? platform = context.Platform switch
        {
            NativePlatform.Windows => "windows",
            NativePlatform.MacOS => "macos",
            _ => null
        };
        string architecture = context.Architecture == NativeArchitecture.Arm64 ? "arm64" : "x64";
        return platform is null ? null : $"{platform}-{architecture}";
    }
}
