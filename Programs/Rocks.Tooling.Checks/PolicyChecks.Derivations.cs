using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    /// <summary>외부 원본에서 파생한 엔진 소스의 출처 기록 파일 이름이다. 파생 소스와 같은 디렉터리에 둔다.</summary>
    public const string DerivationRecordName = "Derivation.json";

    private static readonly Regex LowerSha256 = Pattern("^[0-9a-f]{64}$");

    /// <summary>
    /// 외부 원본(ThirdParty upstream 파일)을 옮겨 와 고친 엔진 소스의 출처를 고정한다(Docs/Design/Crypto.ko.md 5.2절, P8). 원본
    /// 라이브러리를 올리면서 파생 원본이 바뀌면 기록한 SHA-256이 맞지 않아 실패하므로, 업그레이드하는 쪽이 원본 diff를 검토해 파생
    /// 소스에 반영한 뒤 기록을 고친다(rule id derived-source). 기록 형식:
    /// <code>
    /// { "schema": 1, "derivations": [ { "file": "AesX64.cpp",
    ///   "upstream": [ { "path": "Core/Source/ThirdParty/Sodium/.../aead_aes256gcm_aesni.c", "sha256": "..." } ] } ] }
    /// </code>
    /// <list type="bullet">
    /// <item>file: 기록 파일 디렉터리 기준 상대 경로('/' 구분, '..'·절대 경로 금지)이고 있어야 하며 중복되지 않는다.</item>
    /// <item>upstream: 하나 이상. path는 유효한 manifest가 선언한 upstream 디렉터리 안의 파일(저장소 상대)이고, sha256은 그 파일의
    /// 소문자 hex SHA-256이다.</item>
    /// <item>파생 소스는 본문(출처 주석)에 각 upstream 파일 이름을 적는다.</item>
    /// <item>알 수 없는 필드는 거부한다(오타가 조용히 무시되지 않게).</item>
    /// </list>
    /// </summary>
    public static IReadOnlyList<CheckFailure> DerivedSources(string root)
    {
        List<CheckFailure> failures = [];
        string fullRoot = Path.GetFullPath(root);
        IReadOnlyList<string> upstreamDirectories = UpstreamDirectories(root);
        foreach (string path in Files(root, OwnedSourceRoots).Where(path => Path.GetFileName(path) == DerivationRecordName))
        {
            string relative = Relative(root, path);
            if (upstreamDirectories.Any(directory => relative.StartsWith(directory, StringComparison.Ordinal)))
            {
                continue;
            }

            string directoryRelative = relative[..^DerivationRecordName.Length];
            void Fail(string message) => failures.Add(new(relative, 0, "derived-source", message));
            JsonDocument document;
            try
            {
                document = JsonDocument.Parse(File.ReadAllText(path, StrictUtf8));
            }
            catch (JsonException error)
            {
                Fail("invalid JSON: " + error.Message);
                continue;
            }

            using (document)
            {
                JsonElement record = document.RootElement;
                if (record.ValueKind != JsonValueKind.Object || !HasOnly(record, "schema", "derivations") ||
                    !record.TryGetProperty("schema", out JsonElement schema) || schema.ValueKind != JsonValueKind.Number ||
                    schema.GetRawText() != "1" ||
                    !record.TryGetProperty("derivations", out JsonElement derivations) || derivations.ValueKind != JsonValueKind.Array ||
                    derivations.GetArrayLength() == 0)
                {
                    Fail("expected { \"schema\": 1, \"derivations\": [ ... ] } with at least one derivation and no other fields");
                    continue;
                }

                HashSet<string> seen = new(StringComparer.Ordinal);
                foreach (JsonElement derivation in derivations.EnumerateArray())
                {
                    if (derivation.ValueKind != JsonValueKind.Object || !HasOnly(derivation, "file", "upstream") ||
                        !derivation.TryGetProperty("file", out JsonElement fileElement) || fileElement.ValueKind != JsonValueKind.String ||
                        !derivation.TryGetProperty("upstream", out JsonElement upstream) || upstream.ValueKind != JsonValueKind.Array ||
                        upstream.GetArrayLength() == 0)
                    {
                        Fail("each derivation needs \"file\" (string) and a non-empty \"upstream\" array and no other fields");
                        continue;
                    }

                    string file = fileElement.GetString()!;
                    if (!IsPlainRelativePath(file) || !seen.Add(file))
                    {
                        Fail($"derived file '{file}' must be a unique relative path without '..'");
                        continue;
                    }

                    string derivedPath = Path.Combine(fullRoot, directoryRelative + file);
                    if (!File.Exists(derivedPath))
                    {
                        Fail($"derived file '{file}' does not exist");
                        continue;
                    }

                    string derivedText = File.ReadAllText(derivedPath, StrictUtf8);
                    foreach (JsonElement source in upstream.EnumerateArray())
                    {
                        if (source.ValueKind != JsonValueKind.Object || !HasOnly(source, "path", "sha256") ||
                            !source.TryGetProperty("path", out JsonElement sourcePath) || sourcePath.ValueKind != JsonValueKind.String ||
                            !source.TryGetProperty("sha256", out JsonElement sourceHash) || sourceHash.ValueKind != JsonValueKind.String ||
                            !LowerSha256.IsMatch(sourceHash.GetString()!))
                        {
                            Fail($"'{file}': each upstream entry needs \"path\" and a lowercase hex \"sha256\" and no other fields");
                            continue;
                        }

                        string upstreamPath = sourcePath.GetString()!;
                        if (!IsPlainRelativePath(upstreamPath) ||
                            !upstreamDirectories.Any(directory => upstreamPath.StartsWith(directory, StringComparison.Ordinal)))
                        {
                            Fail($"'{file}': upstream '{upstreamPath}' is not inside a third-party upstream directory");
                            continue;
                        }

                        string full = Path.Combine(fullRoot, upstreamPath);
                        if (!File.Exists(full))
                        {
                            Fail($"'{file}': upstream '{upstreamPath}' does not exist");
                            continue;
                        }

                        if (Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(full))) != sourceHash.GetString())
                        {
                            Fail($"'{file}': upstream '{upstreamPath}' changed; review the upstream diff, port it to the derived " +
                                 "file, then update the recorded sha256");
                        }

                        if (!derivedText.Contains(Path.GetFileName(upstreamPath), StringComparison.Ordinal))
                        {
                            Fail($"'{file}' must name its upstream source '{Path.GetFileName(upstreamPath)}' in a provenance comment");
                        }
                    }
                }
            }
        }

        return failures;
    }

    private static bool HasOnly(JsonElement element, params string[] names) =>
        element.EnumerateObject().All(property => names.Contains(property.Name, StringComparer.Ordinal));

    // '/'로 나눈 상대 경로이고 빈 조각·'.'·'..'·'\'·드라이브가 없다.
    private static bool IsPlainRelativePath(string path) =>
        path.Length != 0 && !path.Contains('\\') && !path.Contains(':') && !path.StartsWith('/') &&
        path.Split('/').All(part => part.Length != 0 && part != "." && part != "..");
}
