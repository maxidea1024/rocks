using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Rocks.Build.Descriptors;
using Rocks.Tooling.Common;

namespace Rocks.Build;

/// <summary>
/// Modular 타깃의 모듈 manifest(<c>&lt;Target&gt;[-&lt;Platform&gt;-&lt;Configuration&gt;].modules.json</c>)를 쓴다(Docs/Design/BuildTargets.ko.md
/// 3.8절 BS8d1, Docs/Design/ModuleLoader.ko.md 3.5절). Monolithic 실행 파일의 모듈 표 TU와 같은 행(<see cref="ModuleTables.Rows"/>)을 파일
/// 이름·크기와 함께 싣는다. 링크가 모두 성공하고 입력이 바뀌지 않은 빌드 끝에서만 쓴다(원자적 교체, 내용이 같으면 쓰지 않는다). 키는 영어이고
/// 모듈 로더가 모르는 키를 거부한다(ML4a).
/// </summary>
public static class ModuleManifests
{
    /// <summary>manifest 파일 이름의 접미사다.</summary>
    public const string FileSuffix = ".modules.json";

    /// <summary>manifest 형식 판이다. 키를 바꾸면 올리고 모듈 로더의 읽기도 함께 바꾼다.</summary>
    public const int Schema = 1;

    // buildId 길이(GUID 32자 소문자 16진)다.
    private const int BuildIdLength = 32;

    /// <summary>
    /// 타깃 <paramref name="target"/>의 manifest 파일 이름이다. 공유 라이브러리 이름 규칙(<see cref="LinkTypes.SharedLibraryStem"/>)과 같은
    /// 접미사를 쓴다: Development는 붙이지 않고 그 밖은 <c>-&lt;Platform&gt;-&lt;Configuration&gt;</c>다. 사용 예:
    /// <c>ModuleManifests.FileName("FoundationTests", NativePlatform.Windows, NativeConfiguration.Debug)</c>는
    /// <c>"FoundationTests-Windows-Debug.modules.json"</c>이다.
    /// </summary>
    public static string FileName(string target, NativePlatform platform, NativeConfiguration configuration) =>
        target + (configuration == NativeConfiguration.Development ? "" : "-" + platform + "-" + configuration) + FileSuffix;

    /// <summary>
    /// 빌드 끝에 manifest를 쓰고 그 경로를 돌려준다. Modular가 아니거나, 모듈 표 제공 모듈(<see cref="NativeModuleInput.ProvidesModuleTable"/>)에
    /// 닿는 대상 실행 파일이 없으면 쓰지 않고 null이다. Modular 요청은 대상이 하나다(입력 검사). <c>buildId</c>는 같은 경로의 이전 manifest가
    /// 같은 엔진 버전·툴체인 지문이면 다시 쓰고(MD13 — UE <c>CanRecycleBuildId</c>), 아니면 새 GUID다. 엔진 버전이 없으면
    /// <see cref="ArgumentException"/>이다(빌드 정보와 같은 규칙).
    /// </summary>
    internal static string? Write(NativeBuildRequest request, NativePlan plan)
    {
        if (request.LinkType != NativeLinkType.Modular)
        {
            return null;
        }

        var modules = new Dictionary<string, NativeModuleInput>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput module in request.Modules)
        {
            modules.TryAdd(module.Name, module);
        }

        if (!modules.Values.Any(module => module.ProvidesModuleTable))
        {
            return null;
        }

        HashSet<string> entries = ModuleTables.EntryModules(modules);
        // 모듈 표 TU와 같은 대상 규칙이다(ModuleTables.Add — 대상이 비면 전부). 입력 검사가 그런 실행 파일을 하나 이하로 막는다.
        string? executable = ModuleTables.TableExecutables(request, modules, entries).SingleOrDefault();
        if (executable is null)
        {
            return null;
        }

        string target = request.TargetName ?? executable;
        NativePlatform platform = request.Toolchain.Platform;
        string bin = Path.Combine(request.OutputDirectory, "bin");
        string path = Path.Combine(bin, FileName(target, platform, request.Configuration));
        EngineVersion engineVersion = BuildInfoGenerator.RequireEngineVersion(request);
        string version = $"{engineVersion.Major}.{engineVersion.Minor}.{engineVersion.Patch}";
        string fingerprint = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(ToolchainFingerprint.CanonicalText(request.Toolchain))));
        string buildId = RecycledBuildId(path, version, fingerprint) ?? Guid.NewGuid().ToString("N");

        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartObject();
            writer.WriteNumber("schema", Schema);
            writer.WriteString("buildId", buildId);
            writer.WriteString("engineVersion", version);
            writer.WriteString("toolchainFingerprint", fingerprint);
            writer.WriteStartArray("modules");
            foreach (ModuleRow row in ModuleTables.Rows(executable, modules, entries))
            {
                string file = row.Executable ? Path.GetFileName(plan.ModuleOutputs[row.Name])
                    : LinkTypes.SharedLibraryFileName(target, row.Name, platform, request.Configuration);
                writer.WriteStartObject();
                writer.WriteString("name", row.Name);
                writer.WriteString("file", file);
                writer.WriteNumber("size", new FileInfo(Path.Combine(bin, file)).Length);
                writer.WriteStartArray("dependencies");
                foreach (string dependency in row.Dependencies)
                {
                    writer.WriteStringValue(dependency);
                }

                writer.WriteEndArray();
                writer.WriteString("loadingPhase", row.Phase.ToString());
                writer.WriteBoolean("onRequest", row.OnRequest);
                writer.WriteEndObject();
            }

            writer.WriteEndArray();
            writer.WriteEndObject();
        }

        stream.WriteByte((byte)'\n');
        AtomicFile.WriteIfChanged(path, stream.ToArray());
        return path;
    }

    // 이전 manifest의 buildId를 다시 쓸 수 있으면 그 값이다. 파일이 없거나, 읽을 수 없거나, 형식 판·엔진 버전·툴체인 지문이 다르거나,
    // buildId 모양이 틀리면 null이다(새 GUID를 쓴다 — 읽기 오류로 빌드를 멈추지 않는다).
    private static string? RecycledBuildId(string path, string version, string fingerprint)
    {
        if (!File.Exists(path))
        {
            return null;
        }

        try
        {
            using JsonDocument previous = JsonDocument.Parse(File.ReadAllBytes(path));
            JsonElement root = previous.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !root.TryGetProperty("schema", out JsonElement schema) || schema.ValueKind != JsonValueKind.Number || schema.GetInt32() != Schema ||
                !root.TryGetProperty("engineVersion", out JsonElement engine) || engine.GetString() != version ||
                !root.TryGetProperty("toolchainFingerprint", out JsonElement toolchain) || toolchain.GetString() != fingerprint ||
                !root.TryGetProperty("buildId", out JsonElement id) || id.GetString() is not string text ||
                text.Length != BuildIdLength || !text.All(character => character is >= '0' and <= '9' or >= 'a' and <= 'f'))
            {
                return null;
            }

            return text;
        }
        catch (Exception error) when (error is JsonException or IOException or UnauthorizedAccessException or InvalidOperationException or FormatException)
        {
            return null;
        }
    }
}
