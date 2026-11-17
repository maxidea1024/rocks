using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// 셰이더 도구 열기 모듈이다(Docs/Design/ShaderSystem.ko.md 9절). 사용자 저장소의 도구 파일을 도구 표(ShaderToolTable)와 SHA-256으로
/// 대조한 뒤 P10 DynamicLibrary::Open으로 열고 함수 표를 얻는다. 도구를 여는 것은 워커와 ShaderFormat 모듈뿐이며, 워커
/// (Programs/ShaderCompileWorker)가 생기기 전까지 이 모듈이 그 자리를 맡는다(정책 검사 shader-tool-binary-load).
/// 플랫폼 구현은 Private/Win64(저장소 루트, dxcapi.h를 Windows 헤더와 함께 쓰는 DXC 버전 조회)·Private/Mac·Private/Unsupported
/// (Android)다.
/// </summary>
public sealed class ShaderToolLoader : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderToolLoader";

    protected override string[] RequiredModules => ["ShaderToolTable", "PlatformBase", "Core"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        string platform = context.Platform switch
        {
            NativePlatform.Windows => "Win64",
            NativePlatform.MacOS => "Mac",
            _ => "Unsupported"
        };
        return base.Configure(context) with
        {
            Sources = [.. Directory.GetFiles(context.PathInRoot(backend), "*.cpp").Order(StringComparer.Ordinal),
                .. context.Sources($"{backend}/{platform}")],
            // DXC 공개 헤더(dxcapi.h)는 Windows 구현만 include한다. 외부 헤더는 이 모듈 소스에만 보인다(adapter 방식).
            PrivateIncludeDirectories = ThirdPartyModuleRules.ConsumerIncludeDirectories(context, "DirectXShaderCompiler"),
            PrivateDefines = ThirdPartyModuleRules.ConsumerDefines(context, "DirectXShaderCompiler")
        };
    }
}
