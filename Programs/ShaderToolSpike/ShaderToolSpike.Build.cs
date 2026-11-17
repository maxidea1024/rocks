using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// 셰이더 도구 스파이크(SH0, Docs/Design/ShaderSystem.ko.md 16절)다. 지금의 내장 Renderer 셰이더를 DXIL·SPIR-V·DXBC·MSL·ESSL·HLSL 5.0으로
/// 만들고 검증(spirv-val·DXIL 서명 hash·리플렉션 덤프)과 측정(S2·S3·S8·S12)을 한다. 워커(Programs/ShaderCompileWorker)가 생기기 전에
/// 도구를 직접 여는 측정 프로그램이라 셰이더 도구 참조 정책의 허용 위치다. 제품 런타임에 들어가지 않는다.
/// OS 의존 코드(DXC COM·FXC·D3D12·WGL)는 Private/Win64에 두고, 다른 플랫폼은 Private/Unsupported가 미지원을 보고한다.
/// </summary>
public sealed class ShaderToolSpikeProgram : ModuleRules
{
    // 이 프로그램이 include하는 도구 공개 헤더다(외부 헤더는 이 모듈 소스에만 보인다, adapter 방식).
    private static readonly string[] ToolHeaders = ["DirectXShaderCompiler", "SpirvCross", "SpirvTools", "SpirvReflect"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        const string root = "Programs/ShaderToolSpike/Private";
        bool windows = context.Platform == NativePlatform.Windows;
        return new()
        {
            Name = "ShaderToolSpike",
            Kind = NativeModuleKind.Executable,
            Sources = context.Sources($"{root}/Common", windows ? $"{root}/Win64" : $"{root}/Unsupported"),
            PrivateIncludeDirectories = [context.PathInRoot($"{root}/Common"), .. ThirdPartyModuleRules.ConsumerIncludeDirectories(context, ToolHeaders)],
            PrivateDefines = ThirdPartyModuleRules.ConsumerDefines(context, ToolHeaders),
            Dependencies = ["ShaderToolLoader", "SpirvReflect"],
            // D3D12 셰이더 모델 조회(S2)와 숨김 창 GL context(S12)에 쓴다. 도구 DLL은 링크하지 않고 실행 중에 연다.
            SystemLibraries = windows ? ["d3d12", "dxgi", "opengl32", "gdi32", "user32"] : []
        };
    }
}
