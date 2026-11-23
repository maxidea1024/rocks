using Rocks.Tooling.Common;

namespace Rocks.Build;

/// <summary>
/// 규칙이 만든 생성 번역 단위(<see cref="NativeModuleInput.GeneratedSources"/>)를 <c>&lt;out&gt;/GeneratedSources/&lt;모듈&gt;/</c>에 쓰고
/// 모듈 소스에 더한다(Docs/Design/ShaderSystem 9절 도구 표, BuildInfo 생성 TU와 같은 방식). 출력 잠금 아래에서만 부른다. 내용이
/// 같으면 파일을 다시 쓰지 않으므로(<see cref="AtomicFile.WriteIfChanged(string, string)"/>) 증분 빌드는 compile을 다시 하지 않는다.
/// 이번 실행에 없는 모듈의 파일은 지우지 않는다(입력이 아니다).
/// </summary>
internal static class GeneratedSources
{
    public const string DirectoryName = "GeneratedSources";

    public static string PathOf(string output, string module, string fileName) => Path.Combine(output, DirectoryName, module, fileName);

    /// <summary>생성 파일을 쓰고 그 경로를 소스에 더한 요청을 돌려준다. 돌려준 요청의 GeneratedSources는 비어 있다(두 번 더하지 않는다).</summary>
    public static NativeBuildRequest Write(NativeBuildRequest request)
    {
        if (request.Modules.All(module => module.GeneratedSources.Count == 0))
        {
            return request;
        }

        return request with
        {
            Modules = request.Modules.Select(module =>
            {
                if (module.GeneratedSources.Count == 0)
                {
                    return module;
                }

                var paths = new List<string>();
                foreach (NativeGeneratedSource source in module.GeneratedSources)
                {
                    string path = PathOf(request.OutputDirectory, module.Name, source.FileName);
                    AtomicFile.WriteIfChanged(path, source.Content);
                    paths.Add(path);
                }

                return module with { Sources = [.. module.Sources, .. paths], GeneratedSources = [] };
            }).ToArray()
        };
    }

    /// <summary>
    /// 생성 번역 단위 목록을 검사한다. 파일 이름은 경로 구분자·드라이브·NUL·제어 문자 없는 <c>.cpp</c>·<c>.c</c> 이름이고 모듈 안에서
    /// 대소문자 무시로 겹치지 않아야 하며, 내용에는 NUL이 없어야 한다.
    /// </summary>
    /// <exception cref="ArgumentException">위 조건 위반.</exception>
    public static void Validate(NativeModuleInput module)
    {
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeGeneratedSource source in module.GeneratedSources)
        {
            string name = source.FileName;
            bool extension = name.EndsWith(".cpp", StringComparison.Ordinal) || name.EndsWith(".c", StringComparison.Ordinal);
            if (name.Length == 0 || name is "." or ".." || name.IndexOfAny(['/', '\\', ':']) >= 0 || name.Any(char.IsControl) ||
                !extension || name.StartsWith('.'))
            {
                throw new ArgumentException($"Module {module.Name}: generated source name '{name}' must be a plain .cpp or .c file name.");
            }

            if (!names.Add(name))
            {
                throw new ArgumentException($"Module {module.Name}: generated source '{name}' appears twice.");
            }

            if (source.Content.Contains('\0'))
            {
                throw new ArgumentException($"Module {module.Name}: generated source '{name}' contains a NUL character.");
            }
        }
    }
}
