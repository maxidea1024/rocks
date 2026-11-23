using Rocks.Tooling.Common;

namespace Rocks.Build;

/// <summary>
/// 소스 묶음(<see cref="SourceGroup"/> — Docs/Design/BuildTargets 3.5절 BS1b, BD22 개정)의 도구다.
/// </summary>
public static class SourceGroups
{
    /// <summary>
    /// <paramref name="source"/>(절대 경로)가 든 <paramref name="module"/>의 소스 묶음이다. 폴더가 겹치면 가장 긴 폴더가 이긴다. 없으면 null이다.
    /// </summary>
    public static SourceGroup? Of(NativeModuleInput module, string source, string root)
    {
        SourceGroup? found = null;
        int length = -1;
        foreach (SourceGroup group in module.SourceGroups)
        {
            string directory = Path.TrimEndingDirectorySeparator(Path.GetFullPath(group.Directory, root));
            if (directory.Length > length && DirectoryPath.IsAtOrUnder(source, directory))
            {
                found = group;
                length = directory.Length;
            }
        }

        return found;
    }

    /// <summary>
    /// 소스 묶음을 가진 모듈마다 묶음 대상 모듈과 그 두 의존 목록을 비공개 의존에 더한 목록이다(이미 적은 이름은 건너뛴다). 그래서 시험
    /// 실행 파일의 컴파일 환경은 묶음 대상 모듈 환경의 합집합이다 — 대상 모듈의 의존이 비공개로 내려가도 시험 규칙을 고치지 않는다. 묶음이
    /// 없는 모듈은 그대로다. 서드파티 모듈은 더하지 않는다. 이름을 모르는 대상은 건너뛴다(그 오류는 입력 검사가 알린다).
    /// </summary>
    public static IReadOnlyList<NativeModuleInput> WithTargetDependencies(IReadOnlyList<NativeModuleInput> modules)
    {
        if (modules.All(module => module.SourceGroups.Count == 0))
        {
            return modules;
        }

        var byName = new Dictionary<string, NativeModuleInput>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput module in modules)
        {
            byName.TryAdd(module.Name, module);
        }

        return [.. modules.Select(module =>
        {
            if (module.SourceGroups.Count == 0)
            {
                return module;
            }

            var listed = new HashSet<string>(module.AllDependencies, StringComparer.OrdinalIgnoreCase) { module.Name };
            var added = new List<string>();
            foreach (SourceGroup group in module.SourceGroups)
            {
                if (!byName.TryGetValue(group.Target, out NativeModuleInput? target))
                {
                    continue;
                }

                foreach (string name in new[] { target.Name }.Concat(target.AllDependencies))
                {
                    // 서드파티는 더하지 않는다: 공개 include가 없고(서드파티 규칙 계약) 전역 상태 서드파티는 직접 소비 모듈이 하나여야 한다(BD29).
                    // 링크는 대상 모듈을 거쳐 된다.
                    if (byName.TryGetValue(name, out NativeModuleInput? dependency) && !dependency.ThirdParty && listed.Add(dependency.Name))
                    {
                        added.Add(dependency.Name);
                    }
                }
            }

            return added.Count == 0 ? module : module with { PrivateDependencies = [.. module.PrivateDependencies, .. added] };
        })];
    }
}
