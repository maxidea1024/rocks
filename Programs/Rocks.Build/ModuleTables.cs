using System.Globalization;
using System.Text;

namespace Rocks.Build;

/// <summary>
/// 실행 파일마다 모듈 표 TU를 만든다(Docs/Design/ModuleLoader.ko.md 3.2절, Monolithic manifest). 모듈 표 제공 모듈
/// (<see cref="NativeModuleInput.ProvidesModuleTable"/>, 엔진 Core)에 닿는 실행 파일마다 <c>&lt;실행 파일&gt;.ModuleTable.cpp</c>를
/// 그 실행 파일의 생성 소스(<see cref="NativeModuleInput.GeneratedSources"/>)로 더한다. 표는 실행 파일 도달 집합의 진입점 모듈(<see
/// cref="EntryModules"/>)을 의존 순서로 나열하고 행마다 의존 이름·로딩 단계를 싣는다. 모든 <c>RocksModuleEntry_&lt;이름&gt;</c>을 직접
/// 참조하므로 <c>ROCKS_IMPLEMENT_MODULE</c>이 빠진 모듈은 링크 오류다(정적 라이브러리에서 진입점 object가 빠지지도 않는다).
/// 내용은 호스트 경로를 담지 않고, 같은 입력이면 같은 byte다(내용이 같으면 다시 컴파일하지 않는다).
/// </summary>
public static class ModuleTables
{
    /// <summary>생성 TU 파일 이름의 접미사다(<c>&lt;실행 파일&gt;.ModuleTable.cpp</c>).</summary>
    public const string FileSuffix = ".ModuleTable.cpp";

    /// <summary>빌드가 진입점 모듈의 소스에 정의하는 모듈 이름 매크로다. 규칙·명령줄은 정의할 수 없다(InputValidation).</summary>
    public const string ModuleNameMacro = "ROCKS_MODULE_NAME";

    /// <summary>
    /// 진입점(<c>ROCKS_IMPLEMENT_MODULE</c>)을 가져야 하는 모듈 이름 집합이다(대소문자 무시 비교). 서드파티가 아니고, C++ 코드(소스·생성 소스·
    /// 설치 SDK의 미리 빌드한 라이브러리)가 있으며, 도달 집합에 모듈 표 제공 모듈이 있는 모듈이다. header-only 모듈과 Core 아래 모듈
    /// (PlatformBase 등)은 빠진다(3.1절 면제). 이름을 모르는 의존은 건너뛴다(그 오류는 액션 그래프가 알린다).
    /// </summary>
    public static HashSet<string> EntryModules(IReadOnlyDictionary<string, NativeModuleInput> modules)
    {
        var reachesProvider = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
        var visiting = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        bool Reaches(string name)
        {
            if (reachesProvider.TryGetValue(name, out bool known))
            {
                return known;
            }

            // 순환은 액션 그래프가 거부한다. 여기서는 끝없이 돌지 않게만 한다.
            if (!modules.TryGetValue(name, out NativeModuleInput? module) || !visiting.Add(name))
            {
                return false;
            }

            bool reaches = module.ProvidesModuleTable;
            foreach (string dependency in module.Dependencies)
            {
                reaches |= Reaches(dependency);
            }

            visiting.Remove(name);
            reachesProvider[name] = reaches;
            return reaches;
        }

        var entries = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput module in modules.Values)
        {
            bool hasCode = module.Sources.Count != 0 || module.GeneratedSources.Count != 0 || module.PrebuiltLibraries.Count != 0;
            if (!module.ThirdParty && hasCode && Reaches(module.Name))
            {
                entries.Add(module.Name);
            }
        }

        return entries;
    }

    /// <summary>
    /// 모듈 표가 필요한 실행 파일에 생성 TU를 더한 요청을 돌려준다. 대상(<see cref="NativeBuildRequest.Targets"/>, 비면 전부)에서 닿는
    /// 실행 파일만 본다. 진입점 모듈 이름이 C 식별자가 아니면 <see cref="ArgumentException"/>이다(기호 이름을 만든다).
    /// </summary>
    public static NativeBuildRequest Add(NativeBuildRequest request)
    {
        var modules = new Dictionary<string, NativeModuleInput>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput module in request.Modules)
        {
            // 이름 중복은 InputValidation·액션 그래프가 거부한다. 여기서는 첫 것만 쓴다.
            modules.TryAdd(module.Name, module);
        }

        if (!modules.Values.Any(module => module.ProvidesModuleTable))
        {
            return request;
        }

        HashSet<string> entries = EntryModules(modules);
        IEnumerable<string> roots = request.Targets.Count == 0 ? modules.Keys : request.Targets;
        HashSet<string> reachable = Reachable(modules, roots);
        var tables = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput module in request.Modules)
        {
            if (module.Kind == NativeModuleKind.Executable && entries.Contains(module.Name) && reachable.Contains(module.Name))
            {
                tables[module.Name] = Generate(module.Name, modules, entries);
            }
        }

        if (tables.Count == 0)
        {
            return request;
        }

        return request with
        {
            Modules = request.Modules.Select(module => tables.TryGetValue(module.Name, out string? content)
                ? module with { GeneratedSources = [.. module.GeneratedSources, new NativeGeneratedSource(module.Name + FileSuffix, content)] }
                : module).ToArray()
        };
    }

    /// <summary>
    /// 실행 파일 <paramref name="executable"/>의 모듈 표 TU 내용이다. 행은 의존 우선 깊이 탐색의 후위 순서(의존 목록 순서대로)에서 진입점
    /// 모듈만 고른 것이고 실행 파일이 마지막이다. 행의 의존은 직접 의존 가운데 진입점 모듈이며, 면제 모듈을 거치는 의존은 그 너머의 진입점
    /// 모듈로 바꿔 싣는다(중복 제거, 처음 만난 순서).
    /// </summary>
    internal static string Generate(string executable, IReadOnlyDictionary<string, NativeModuleInput> modules, HashSet<string> entries)
    {
        var order = new List<NativeModuleInput>();
        var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        void Visit(string name)
        {
            if (!modules.TryGetValue(name, out NativeModuleInput? module) || !visited.Add(name))
            {
                return;
            }

            foreach (string dependency in module.Dependencies)
            {
                Visit(dependency);
            }

            if (entries.Contains(module.Name))
            {
                order.Add(module);
            }
        }

        Visit(executable);
        foreach (NativeModuleInput module in order)
        {
            if (!IsIdentifier(module.Name))
            {
                throw new ArgumentException($"Module {module.Name}: a module with a module entry point (ROCKS_IMPLEMENT_MODULE) must have a C " +
                    "identifier name (letters, digits and '_', not starting with a digit).");
            }
        }

        var text = new StringBuilder();
        text.Append("// Generated by Rocks.Build. Do not edit.\n");
        text.Append(CultureInfo.InvariantCulture, $"// Module table of executable {executable} (Docs/Design/ModuleLoader.ko.md section 3.2): every C++ module\n");
        text.Append("// of the executable in dependency order with its direct dependencies and loading phase. Referencing every\n");
        text.Append("// RocksModuleEntry_<Name> makes a missing ROCKS_IMPLEMENT_MODULE a link error.\n");
        text.Append("#include <Core/Module/ModuleEntry.h>\n\n");
        foreach (NativeModuleInput module in order)
        {
            text.Append(CultureInfo.InvariantCulture, $"const ::rocks::ModuleEntry& RocksModuleEntry_{module.Name}() noexcept;\n");
        }

        text.Append("\nnamespace {\n\n");
        var dependencies = new Dictionary<string, List<string>>(StringComparer.Ordinal);
        foreach (NativeModuleInput module in order)
        {
            List<string> direct = EntryDependencies(module, modules, entries);
            dependencies[module.Name] = direct;
            if (direct.Count != 0)
            {
                text.Append(CultureInfo.InvariantCulture, $"constexpr const char* RocksModuleDependencies_{module.Name}[] = {{");
                text.Append(string.Join(", ", direct.Select(name => "\"" + name + "\"")));
                text.Append("};\n");
            }
        }

        text.Append("\nconstexpr ::rocks::ModuleTableRow RocksModuleRows[] = {\n");
        foreach (NativeModuleInput module in order)
        {
            List<string> direct = dependencies[module.Name];
            string list = direct.Count == 0 ? "nullptr" : "RocksModuleDependencies_" + module.Name;
            text.Append(CultureInfo.InvariantCulture,
                $"    {{\"{module.Name}\", &RocksModuleEntry_{module.Name}, {list}, {direct.Count}, ::rocks::LoadingPhase::{module.LoadingPhase}}},\n");
        }

        text.Append("};\n\n} // namespace\n\n");
        text.Append("::rocks::ModuleTable rocks::GeneratedModuleTable() noexcept {\n");
        text.Append("    return ::rocks::ModuleTable(RocksModuleRows);\n");
        text.Append("}\n");
        return text.ToString();
    }

    // 직접 의존 가운데 진입점 모듈이다. 면제 모듈(header-only·서드파티·Core 아래)은 그 의존을 따라가 진입점 모듈로 바꾼다.
    private static List<string> EntryDependencies(NativeModuleInput module, IReadOnlyDictionary<string, NativeModuleInput> modules,
        HashSet<string> entries)
    {
        var result = new List<string>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        void Add(string name)
        {
            if (!modules.TryGetValue(name, out NativeModuleInput? dependency) || !seen.Add(name))
            {
                return;
            }

            if (entries.Contains(dependency.Name))
            {
                result.Add(dependency.Name);
                return;
            }

            foreach (string next in dependency.Dependencies)
            {
                Add(next);
            }
        }

        foreach (string dependency in module.Dependencies)
        {
            Add(dependency);
        }

        return result;
    }

    private static HashSet<string> Reachable(IReadOnlyDictionary<string, NativeModuleInput> modules, IEnumerable<string> roots)
    {
        var reached = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var pending = new Stack<string>(roots);
        while (pending.Count != 0)
        {
            string name = pending.Pop();
            if (!modules.TryGetValue(name, out NativeModuleInput? module) || !reached.Add(name))
            {
                continue;
            }

            foreach (string dependency in module.Dependencies)
            {
                pending.Push(dependency);
            }
        }

        return reached;
    }

    /// <summary>C 식별자(ASCII 영문자·숫자·'_', 숫자로 시작하지 않음)인지다.</summary>
    internal static bool IsIdentifier(string name) =>
        name.Length != 0 && !char.IsAsciiDigit(name[0]) && name.All(value => char.IsAsciiLetterOrDigit(value) || value == '_');
}
