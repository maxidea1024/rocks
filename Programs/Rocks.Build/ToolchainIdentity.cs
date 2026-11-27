namespace Rocks.Build;

/// <summary>
/// native 엔진 밖에서 native와 같은 툴체인 판정을 쓰는 곳(메타 단계 — Docs/Design/BuildReuse 2.3·2.4절)을 위한 공개 입구다. 판정 코드는
/// 엔진의 것을 그대로 부른다(두 벌 두지 않는다).
/// </summary>
public static class ToolchainIdentity
{
    /// <summary>툴체인 key다(컴파일러·archiver·linker·FingerprintPaths·런타임 파일 내용과 keyed 환경 — native action key의 첫 항목과 같다).</summary>
    public static string Key(NativeToolchain toolchain) => Fingerprints.Toolchain(toolchain);

    /// <summary>
    /// 툴체인 소유 파일인가: <see cref="NativeToolchain.OwnedRoots"/>, <see cref="NativeToolchain.FingerprintPaths"/>, keyed 환경 <c>INCLUDE</c>가
    /// 가리키는 디렉터리 아래다. 이런 파일은 의존 기록에 넣지 않는다(툴체인 key가 대신한다 — UnityBuildPch 4.1절).
    /// </summary>
    public static Func<string, bool> OwnedFilter(NativeToolchain toolchain)
    {
        string[] roots = BuildInputs.OwnedRoots(toolchain);
        return path =>
        {
            string full = Path.GetFullPath(path);
            return roots.Any(root => Fingerprints.IsUnder(full, root));
        };
    }

    /// <summary>파일·디렉터리 목록의 내용 digest다(디렉터리는 그 아래 모든 파일의 경로와 내용 — native 선언 추가 입력과 같은 규칙).</summary>
    public static string ContentDigest(IEnumerable<string> paths) => Fingerprints.Paths(paths);

    /// <summary>문자열 목록의 digest다(길이를 앞에 붙여 경계가 모호하지 않다).</summary>
    public static string Digest(IEnumerable<string> values) => Fingerprints.Strings(values);
}
