using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // placement new 선언만 여는 헤더다. <new>를 include할 수 있는 유일한 엔진 헤더다(MSVC 런타임이 아닌 곳에서 쓴다).
    private const string PlacementNewHeader = "Core/Source/Runtime/Base/Public/Base/PlacementNew.h";

    // <new>를 include하는 줄이다(꺾쇠·따옴표). 주석 속 언급은 줄 머리가 #이 아니라 맞지 않는다.
    private static readonly Regex NewHeaderInclude = Pattern("^[ \\t]*#[ \\t]*include[ \\t]*[<\"]new[>\"]");

    /// <summary>
    /// 시험 소스 밖에서 <c>&lt;new&gt;</c>를 include하는 곳을 찾는다(Docs/Design/UnityBuildPch.ko.md 8.4절 "placement new 헤더", 규칙
    /// <c>placement-new-header</c>). MSVC STL의 <c>&lt;new&gt;</c>는 <c>&lt;exception&gt;</c> 사슬을 끌어와 PCH를 쓰지 않는 번역 단위마다
    /// 0.1 s 넘게 들므로, 엔진 소스는 placement new 선언만 여는 <c>&lt;Base/PlacementNew.h&gt;</c>를 쓴다. 시험 소스
    /// (<see cref="IsTestSource"/>)는 전역 <c>operator new</c> 교체처럼 <c>&lt;new&gt;</c>의 다른 것이 필요할 수 있어 대상이 아니다.
    /// 사용 예: <c>PolicyChecks.PlacementNewHeaders(root)</c>.
    /// </summary>
    public static IReadOnlyList<CheckFailure> PlacementNewHeaders(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            if (relative == PlacementNewHeader || IsTestSource(relative))
            {
                continue;
            }

            ScanFile(path, relative, NewHeaderInclude, "placement-new-header",
                "include <Base/PlacementNew.h> instead of <new>: the MSVC <new> pulls in <exception> and its chain in every translation unit " +
                "without a PCH", failures);
        }

        return failures;
    }
}
