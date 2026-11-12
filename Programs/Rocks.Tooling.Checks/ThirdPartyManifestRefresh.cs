using System.Security.Cryptography;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using Rocks.Tooling.Common;

namespace Rocks.Tooling.Checks;

/// <summary>
/// 외부 라이브러리 업그레이드 도구다. manifest의 <c>files</c>(SHA-256)와 <c>pragmaDirectives</c>만 원본 디렉터리에서 다시
/// 계산하고 나머지 필드(출처·라이선스·번역 단위·정의·사유)는 그대로 둔다. 새 지시문이 생기면 사람이 검토해야 하므로
/// 기본 실행은 차이만 보고하고 <c>write</c>일 때만 파일을 고친다.
/// 사용: <c>Rocks.ToolTests third-party --root . [--write]</c>
/// </summary>
public static class ThirdPartyManifestRefresh
{
    /// <summary>라이브러리마다 갱신이 필요한지 돌려준다. write면 manifest를 원자적으로 다시 쓴다.</summary>
    public static IReadOnlyList<string> Run(string root, bool write)
    {
        root = Path.GetFullPath(root);
        string thirdParty = Path.Combine(root, ThirdPartyManifest.ThirdPartyRoot);
        List<string> changed = [];
        if (!Directory.Exists(thirdParty))
            return changed;

        foreach (string library in Directory.EnumerateDirectories(thirdParty).Order(StringComparer.Ordinal))
        {
            // 엄격한 읽기로 먼저 검증한다(나머지 필드가 계약을 지키는지).
            ThirdPartyManifest manifest = ThirdPartyManifest.Read(library);
            string path = Path.Combine(library, ThirdPartyManifest.FileName);
            string original = File.ReadAllText(path, new UTF8Encoding(false, true));
            JsonObject document = JsonNode.Parse(original)!.AsObject();

            string upstream = Path.Combine(library, manifest.UpstreamDirectory);
            var files = new JsonObject();
            var pragmas = new JsonArray();
            foreach (string file in Directory.EnumerateFiles(upstream, "*", SearchOption.AllDirectories)
                         .Select(file => Path.GetRelativePath(upstream, file).Replace('\\', '/'))
                         .Order(StringComparer.Ordinal))
            {
                byte[] bytes = File.ReadAllBytes(Path.Combine(upstream, file));
                files[file] = Convert.ToHexStringLower(SHA256.HashData(bytes));
                foreach (ThirdPartyPragma pragma in ThirdPartyManifest.ScanPragmaDirectives(file, bytes))
                    pragmas.Add(new JsonObject { ["file"] = pragma.File, ["line"] = pragma.Line, ["text"] = pragma.Text });
            }
            document["files"] = files;
            document["pragmaDirectives"] = pragmas;

            string updated = document.ToJsonString(new JsonSerializerOptions
            {
                // 줄바꿈은 플랫폼과 무관하게 LF다(저장소 규칙, 결과 byte가 플랫폼마다 같아야 한다).
                WriteIndented = true, NewLine = "\n", Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping
            }) + "\n";
            if (updated == original)
                continue;
            changed.Add(Path.GetRelativePath(root, path).Replace('\\', '/'));
            if (write)
            {
                string temporary = path + ".tmp";
                File.WriteAllText(temporary, updated, new UTF8Encoding(false));
                File.Move(temporary, path, overwrite: true);
                ThirdPartyManifest.Read(library); // 쓴 결과도 엄격한 읽기를 통과해야 한다.
            }
        }
        return changed;
    }
}
