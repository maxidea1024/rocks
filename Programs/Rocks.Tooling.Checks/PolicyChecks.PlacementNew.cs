namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // placement new 선언만 여는 헤더다. MSVC 런타임 헤더 <vcruntime_new.h>를 직접 include할 수 있는 유일한 파일이다.
    private const string PlacementNewHeader = "Core/Source/Runtime/Base/Public/Base/PlacementNew.h";

    // 표준 헤더 <new>와, 그 선언 부분만 담은 MSVC 런타임 헤더의 철자다.
    private const string NewHeaderSpelling = "new";

    private const string RuntimeNewHeaderSpelling = "vcruntime_new.h";

    /// <summary>
    /// 시험 소스 밖에서 <c>&lt;new&gt;</c>를 include할 수 있는 파일(저장소 루트 기준 경로)과 그 사유다
    /// (Docs/Design/UnityBuildPch.ko.md 8.4절 "placement new 헤더"). <c>&lt;Base/PlacementNew.h&gt;</c>가 보장하지 않는 이름
    /// (<c>std::launder</c>, <c>std::bad_alloc</c>, <c>std::hardware_destructive_interference_size</c> 등)이 꼭 필요한 소스만 사유와 함께
    /// 올린다.
    /// </summary>
    public static readonly IReadOnlyDictionary<string, string> PlacementNewAllowances = new Dictionary<string, string>(StringComparer.Ordinal)
    {
        [PlacementNewHeader] = "the placement new header itself: it includes <new> where the runtime is not the MSVC runtime"
    };

    /// <summary>
    /// <c>&lt;new&gt;</c>와 <c>&lt;vcruntime_new.h&gt;</c>를 직접 include하는 곳을 찾는다(Docs/Design/UnityBuildPch.ko.md 8.4절 "placement new
    /// 헤더", 규칙 <c>placement-new-header</c>).
    /// <list type="bullet">
    /// <item><c>&lt;new&gt;</c>: MSVC STL에서는 <c>&lt;exception&gt;</c> 사슬을 끌어와 PCH를 쓰지 않는 번역 단위마다 0.1 s 넘게 든다. 엔진
    /// 소스는 placement new 선언만 여는 <c>&lt;Base/PlacementNew.h&gt;</c>를 쓴다. 시험 소스(<see cref="IsTestSource"/>)와
    /// <paramref name="allowances"/>에 올린 파일만 include할 수 있다.</item>
    /// <item><c>&lt;vcruntime_new.h&gt;</c>: MSVC 런타임에만 있어 공통 소스가 쓰면 macOS·Android에서만 깨진다. <c>Base/PlacementNew.h</c>
    /// 말고는 시험 소스도 include할 수 없다.</item>
    /// </list>
    /// 주석을 지우고 줄 이음을 이은 뒤 include 줄을 본다(주석 속 언급은 걸리지 않는다). 철자는 대소문자를 구분하지 않는다.
    /// <paramref name="allowances"/>를 주지 않으면 <see cref="PlacementNewAllowances"/>를 쓴다. 사용 예:
    /// <c>PolicyChecks.PlacementNewHeaders(root)</c>.
    /// </summary>
    public static IReadOnlyList<CheckFailure> PlacementNewHeaders(string root, IReadOnlyDictionary<string, string>? allowances = null)
    {
        allowances ??= PlacementNewAllowances;
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            bool newAllowed = IsTestSource(relative) || allowances.ContainsKey(relative);
            foreach (IncludeLine include in IncludeLines(File.ReadAllText(path, StrictUtf8)))
            {
                if (include.Kind == IncludeKind.NonLiteral)
                {
                    continue;
                }

                if (!newAllowed && string.Equals(include.Spelling, NewHeaderSpelling, StringComparison.OrdinalIgnoreCase))
                {
                    failures.Add(new(relative, include.Line, "placement-new-header",
                        "include <Base/PlacementNew.h> instead of <new>: the MSVC <new> pulls in <exception> and its chain in every translation " +
                        "unit without a PCH. If the source needs more of <new> than placement new, add it to PolicyChecks.PlacementNewAllowances " +
                        "with a reason"));
                }
                else if (relative != PlacementNewHeader &&
                    string.Equals(include.Spelling, RuntimeNewHeaderSpelling, StringComparison.OrdinalIgnoreCase))
                {
                    failures.Add(new(relative, include.Line, "placement-new-header",
                        "include <Base/PlacementNew.h> instead of <vcruntime_new.h>: that header exists only in the MSVC runtime, so the " +
                        "source would not compile for macOS or Android"));
                }
            }
        }

        return failures;
    }
}
