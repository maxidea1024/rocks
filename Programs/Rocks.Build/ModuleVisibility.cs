namespace Rocks.Build;

/// <summary>
/// 모듈의 컴파일 환경에 보이는 의존 모듈을 계산한다(Docs/Design/BuildTargets 3.5절, UE <c>UEBuildModule.cs:607-647</c>와 같은 규칙).
/// 대상 모듈은 공개·비공개 두 목록을 모두 따라가고, 그 밖의 모듈은 공개 목록만 따라간다. 그래서 비공개 의존의 공개 의존은 대상에게
/// 보이지만 대상의 소비자에게는 보이지 않는다. include·정의·<see cref="NativeModuleInput.AdditionalInputs"/>·공유 PCH 후보·메타데이터
/// 범위가 모두 이 결과를 쓴다. 링크와 빌드 대상은 보임이 아니라 도달(<see cref="NativeModuleInput.AllDependencies"/>)을 따른다.
/// </summary>
public static class ModuleVisibility
{
    /// <summary>
    /// <paramref name="module"/>의 컴파일 환경에 보이는 의존 모듈(자기 제외)을 의존 우선 후위 순서로 돌려준다. 각 모듈은 공개 목록을 먼저,
    /// 대상 모듈은 그다음 비공개 목록을 따라간다(목록 순서대로). 모든 의존이 공개면 전환 전(한 목록) 도달 순서와 같다. 이름을 모르는
    /// 의존은 <paramref name="unknown"/>(모르는 이름, 그 이름을 적은 모듈)를 부르고 건너뛴다. null이면 조용히 건너뛴다(그 오류는 입력
    /// 검사·액션 그래프가 알린다).
    /// </summary>
    public static List<NativeModuleInput> Of(NativeModuleInput module, IReadOnlyDictionary<string, NativeModuleInput> modules,
        Action<string, string>? unknown = null)
    {
        var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { module.Name };
        var result = new List<NativeModuleInput>();
        void Visit(string name, string from)
        {
            if (!modules.TryGetValue(name, out NativeModuleInput? current))
            {
                unknown?.Invoke(name, from);
                return;
            }

            if (!visited.Add(current.Name))
            {
                return;
            }

            foreach (string dependency in current.PublicDependencies)
            {
                Visit(dependency, current.Name);
            }

            result.Add(current);
        }

        foreach (string dependency in module.AllDependencies)
        {
            Visit(dependency, module.Name);
        }

        return result;
    }

    /// <summary>
    /// <paramref name="name"/> 모듈의 공개 환경을 이루는 모듈이다: 자기 자신 다음, 공개 목록만 따라간 의존 모듈을 의존 우선 후위 순서로
    /// 담는다. 공유 PCH 제공 모듈의 public include·정의 closure가 이것이다(Docs/Design/UnityBuildPch.ko.md 8.4절 — 비공개 의존은 소비자에게
    /// 보이지 않으므로 넣지 않는다). 모든 의존이 공개면 도달 closure와 같다. 이름을 모르는 모듈은 건너뛴다.
    /// </summary>
    public static List<string> PublicClosure(string name, IReadOnlyDictionary<string, NativeModuleInput> modules)
    {
        var result = new List<string> { name };
        if (!modules.TryGetValue(name, out NativeModuleInput? root))
        {
            return result;
        }

        var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { root.Name };
        var order = new List<string>();
        void Visit(string dependency)
        {
            if (!modules.TryGetValue(dependency, out NativeModuleInput? current) || !visited.Add(current.Name))
            {
                return;
            }

            foreach (string next in current.PublicDependencies)
            {
                Visit(next);
            }

            order.Add(current.Name);
        }

        foreach (string dependency in root.PublicDependencies)
        {
            Visit(dependency);
        }

        result.AddRange(order);
        return result;
    }

    /// <summary><paramref name="module"/>의 컴파일 환경에 <paramref name="target"/> 모듈이 보이는지다.</summary>
    public static bool Sees(NativeModuleInput module, string target, IReadOnlyDictionary<string, NativeModuleInput> modules) =>
        Of(module, modules).Any(candidate => string.Equals(candidate.Name, target, StringComparison.OrdinalIgnoreCase));
}
