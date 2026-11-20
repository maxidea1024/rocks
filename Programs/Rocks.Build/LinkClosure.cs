namespace Rocks.Build;

/// <summary>
/// 실행 파일이 링크하는 모듈 집합이다(Docs/Design/ModuleLoader.ko.md 3.8절, Monolithic). 링크 의존(두 의존 목록 —
/// <see cref="NativeModuleInput.AllDependencies"/>) 폐포에, 그 폐포의 모듈(과 이어서 더해진 모듈)이 <see cref="NativeModuleInput.DynamicModules"/>로 둔 모듈과 그 링크 폐포를 더한다.
/// 실행 파일은 링크 폐포 뒤에 켠 플러그인 모듈(<see cref="NativeModuleInput.PluginModules"/>)과 그 링크 폐포를 링크한다. 동적 관계는 순환 검사의 참조 사슬이 아니므로(UE와 같다) 이미 닿은 모듈로 돌아가는 관계는 건너뛴다. 이름을 모르는 모듈은 건너뛴다
/// (그 오류는 입력 검사·액션 그래프가 알린다).
/// </summary>
public static class LinkClosure
{
    /// <summary>
    /// root를 뺀 링크 모듈 이름을 의존 우선 순서로 돌려준다. 앞부분은 링크 의존 폐포의 후위 순서(의존 목록 순서대로 — 컴파일 의존 순서와
    /// 같다)이고, 그 뒤에 동적 모듈 쪽이 같은 규칙으로 붙는다. 정적 라이브러리 링크는 이 순서를 뒤집어 의존하는 쪽을 앞에 둔다.
    /// </summary>
    public static List<string> Of(string root, IReadOnlyDictionary<string, NativeModuleInput> modules) => Collect(root, modules, out _);

    /// <summary>
    /// <paramref name="withDynamic"/>이면 <see cref="Of(string, IReadOnlyDictionary{string, NativeModuleInput})"/>와 같고, 아니면 링크 의존
    /// 폐포(앞부분)만이다(라이브러리 모듈의 도달 집합).
    /// </summary>
    public static List<string> Of(string root, IReadOnlyDictionary<string, NativeModuleInput> modules, bool withDynamic)
    {
        List<string> order = Collect(root, modules, out int linked);
        return withDynamic ? order : order.GetRange(0, linked);
    }

    /// <summary>
    /// <see cref="Of"/>와 같은 목록과, 그 가운데 링크 의존으로는 닿지 않고 동적 관계로만 닿는 모듈 이름 집합(요청 시 적재 — 모듈 표 행의
    /// 표지, Docs/Design/ModuleLoader.ko.md 3.8절)을 돌려준다.
    /// </summary>
    public static List<string> Of(string root, IReadOnlyDictionary<string, NativeModuleInput> modules, out HashSet<string> onRequest)
    {
        List<string> order = Collect(root, modules, out int linked);
        onRequest = new HashSet<string>(order.Skip(linked), StringComparer.OrdinalIgnoreCase);
        return order;
    }

    // linked는 order 앞부분 가운데 링크 의존 폐포의 길이다.
    private static List<string> Collect(string root, IReadOnlyDictionary<string, NativeModuleInput> modules, out int linked)
    {
        var order = new List<string>();
        linked = 0;
        if (!modules.TryGetValue(root, out NativeModuleInput? rootModule))
        {
            return order;
        }

        var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { rootModule.Name };
        void Visit(string name)
        {
            if (!modules.TryGetValue(name, out NativeModuleInput? module) || !visited.Add(module.Name))
            {
                return;
            }

            foreach (string dependency in module.AllDependencies)
            {
                Visit(dependency);
            }

            order.Add(module.Name);
        }

        foreach (string dependency in rootModule.AllDependencies)
        {
            Visit(dependency);
        }

        // 실행 파일에 링크하는 켠 플러그인 모듈(Docs/Design/BuildTargets.ko.md 3.9절 BD36)은 링크 폐포 뒤, 동적 모듈 앞이다. 링크하므로
        // 요청 시 적재 행이 아니다.
        foreach (string plugin in rootModule.PluginModules)
        {
            Visit(plugin);
        }

        linked = order.Count;
        // 동적 모듈은 링크 폐포를 다 모은 뒤에 더한다. 더해진 모듈의 동적 모듈도 따라간다(목록이 자라는 동안 끝까지 돈다). index -1은
        // root 자신의 동적 모듈이다.
        for (int index = -1; index < order.Count; ++index)
        {
            NativeModuleInput module = index < 0 ? rootModule : modules[order[index]];
            foreach (string dynamic in module.DynamicModules)
            {
                Visit(dynamic);
            }
        }

        return order;
    }

    /// <summary>
    /// roots에서 링크 의존·실행 파일의 플러그인 모듈(<see cref="NativeModuleInput.PluginModules"/>)·동적 모듈 관계로 닿는 모듈 이름 집합이다(빌드 대상·메타데이터·검사 범위·설치 SDK). 이름을 모르는 모듈은
    /// unknown(모르는 이름, 그 이름을 가리킨 모듈 — roots면 null)을 부르고 건너뛴다. unknown이 null이면 조용히 건너뛴다. 이름 비교는
    /// 대소문자를 무시한다(액션 그래프와 같다).
    /// </summary>
    public static HashSet<string> Reachable(IEnumerable<string> roots, IReadOnlyDictionary<string, NativeModuleInput> modules,
        Action<string, string?>? unknown = null)
    {
        var reached = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var pending = new Stack<(string Name, string? From)>(roots.Reverse().Select(root => (root, (string?)null)));
        while (pending.Count != 0)
        {
            (string name, string? from) = pending.Pop();
            if (!modules.TryGetValue(name, out NativeModuleInput? module))
            {
                unknown?.Invoke(name, from);
                continue;
            }

            if (!reached.Add(module.Name))
            {
                continue;
            }

            foreach (string next in module.AllDependencies.Concat(module.PluginModules).Concat(module.DynamicModules).Reverse())
            {
                pending.Push((next, module.Name));
            }
        }

        return reached;
    }
}
