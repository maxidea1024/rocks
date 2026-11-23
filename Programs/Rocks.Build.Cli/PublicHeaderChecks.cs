using Rocks.Build;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// 공개 헤더 검사 모듈이다(Docs/Design/BuildTargets.ko.md 3.5절 BD9·BS1a). 검사 대상 모듈 M마다 검사 모듈 <c>PublicHeaderChecks_&lt;M&gt;</c>를
/// 만든다. 공개 의존은 M 하나이므로 M의 공개 헤더는 M의 공개 환경(M과 그 공개 의존의 public include·define)만으로 컴파일된다. 헤더가
/// 의존하지 않은 모듈의 헤더를 include하면 그 검사 컴파일이 실패한다. 공개 리플렉션 헤더가 있는 모듈은 생성 Public 디렉터리가 public include에
/// 있으므로 생성 헤더(<c>.generated.h</c>)도 단독으로 검사한다(의도 — 생성 헤더도 스스로 컴파일되어야 한다).
/// </summary>
internal static class PublicHeaderChecks
{
    /// <summary>검사 모듈 이름의 앞부분이다: <c>PublicHeaderChecks_&lt;M&gt;</c>.</summary>
    public const string Prefix = "PublicHeaderChecks_";

    /// <summary>
    /// <paramref name="targets"/>에서 닿는(실행 중 적재 모듈 포함) 서드파티가 아닌 라이브러리 모듈마다 검사 모듈을 <paramref name="modules"/>에
    /// 더하고 그 이름을 돌려준다(호출자가 빌드 대상에 더한다). 헤더마다 검사 TU 하나를 <c>&lt;output&gt;/HeaderChecks/&lt;M&gt;/</c>에 쓴다(내용이
    /// 같으면 쓰지 않는다). 공개 헤더가 없는 모듈은 검사 모듈이 없다. 모르는 의존 이름은 <see cref="ArgumentException"/>이다.
    /// </summary>
    public static IReadOnlyList<string> Add(List<NativeModuleInput> modules, IReadOnlyCollection<string> targets, string output, NativeToolchain toolchain)
    {
        // 실행 중 적재 모듈도 빌드하므로 공개 헤더 검사 대상이다(Docs/Design/ModuleLoader.ko.md 3.8절).
        HashSet<string> reached = LinkClosure.Reachable(targets, modules.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase),
            (name, _) => throw new ArgumentException($"Unknown dependency {name}"));
        // 서드파티 모듈은 뺀다: 공개 include가 없고(서드파티 규칙 계약) 전역 상태 서드파티의 직접 소비 모듈이 하나여야 한다(BD29·BS8c).
        NativeModuleInput[] checkedModules = [.. modules.Where(module => reached.Contains(module.Name) &&
                module.Kind == NativeModuleKind.StaticLibrary && !module.ThirdParty)
            .OrderBy(module => module.Name, StringComparer.Ordinal)];
        var added = new List<string>();
        foreach (NativeModuleInput checkedModule in checkedModules)
        {
            var sources = new List<string>();
            foreach (string includeDirectory in checkedModule.PublicIncludeDirectories.Distinct())
            {
                foreach (string header in Directory.GetFiles(includeDirectory, "*.h", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
                {
                    string include = Path.GetRelativePath(includeDirectory, header).Replace('\\', '/');
                    string key = Hashing.Sha256Text(checkedModule.Name + "|" + include);
                    string path = Path.Combine(output, "HeaderChecks", checkedModule.Name, key + ".cpp");
                    AtomicFile.WriteIfChanged(path, $"#include <{include}>\nint RocksHeaderCheck_{key}() noexcept {{ return 0; }}\n");
                    sources.Add(path);
                }
            }

            if (sources.Count == 0)
            {
                continue;
            }

            string name = Prefix + checkedModule.Name;
            modules.Add(new()
            {
                Name = name,
                Sources = sources,
                // 검사 모듈의 종류는 검사 대상 모듈의 것이다(Release 배제·type 의존 규칙이 같게 본다 — BD20·BD25).
                Type = checkedModule.Type,
                // 헤더 하나가 스스로 컴파일되는지 보는 검사이므로 유니티와 PCH 없이 돈다(Docs/Design/UnityBuildPch.ko.md 10절).
                // 공유 PCH를 쓰면 PCH에 든 헤더가 빠진 include를 메워 검사가 무력해진다.
                Unity = UnityMode.Off,
                PchUsage = PchUsage.Off,
                // 공개 헤더의 쓰기 가능한 정적 저장소(공유 라이브러리마다 복제된다)를 Clang 진단으로 거부한다(Docs/Design/BuildTargets.ko.md
                // 3.8절 BD29·BS8c). Windows 대상만 켠다(다른 대상에서는 숨김 가시성 객체만 알린다 — thread_local은 정책 검사가 따로 본다).
                AdditionalCompileArguments = ToolchainCheck.SupportsUniqueObjectDuplication(toolchain.Platform, toolchain.CompilerVersion)
                    ? [ToolchainCheck.UniqueObjectDuplicationWarning] : [],
                // 저장소 루트를 include하지 않는다: 검사 TU는 M의 공개 환경만으로 헤더를 연다.
                PublicDependencies = [checkedModule.Name]
            });
            added.Add(name);
        }

        return added;
    }
}
