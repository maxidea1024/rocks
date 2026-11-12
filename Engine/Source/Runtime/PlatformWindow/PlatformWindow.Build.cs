using Rocks.Build;
using Rocks.Build.Rules;

// 창 생성·이벤트 펌프와 입력 backend(키보드·마우스·게임패드)다. 공개 인터페이스는 플랫폼 독립이고
// 구현은 Private/<플랫폼>/에 둔다. 그래픽·World를 모른다(네이티브 창 핸들만 노출한다).
public sealed class PlatformWindow : EngineModuleRules
{
    protected override string ModuleName => "PlatformWindow";
    protected override string[] RequiredModules => ["Input"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        bool windows = context.Platform == NativePlatform.Windows;
        return new()
        {
            Name = ModuleName,
            // Windows는 Win32 창과 XInput, 그 밖의 플랫폼은 아직 창 backend가 없어 Unsupported를 반환한다.
            Sources = context.Sources(windows ? $"{backend}/Win64" : $"{backend}/Unsupported"),
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            Dependencies = RequiredModules,
            // XInput은 실행 시 동적 로드한다(SDK의 xinput.lib는 lld-link에서 DllMain 가져오기 경고를 낸다).
            AdditionalLinkArguments = windows ? ["user32.lib"] : []
        };
    }
}
