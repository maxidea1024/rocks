using System.Reflection.PortableExecutable;

namespace Rocks.Build;

/// <summary>
/// Modular Windows 공유 라이브러리의 export 수 진단이다(Docs/Design/BuildTargets.ko.md 3.8절 BS8d3, BD27 한계). Windows DLL 하나의 export
/// 상한은 65535이고 넘으면 링커가 오류를 낸다. 빌드가 끝나면 공유 라이브러리마다 PE export 디렉터리의 함수 수를 읽어 모듈별 수를 한 줄로
/// 알리고, 상한의 90% 이상인 모듈은 경고한다(멤버 단위 export로 줄이라는 안내). macOS·Linux는 상한이 없어 세지 않는다.
/// </summary>
public static class ModularExports
{
    /// <summary>Windows DLL 하나의 export 상한이다(PE export 서수가 16비트).</summary>
    public const int WindowsExportLimit = 65535;

    /// <summary>경고 문턱 비율(백분율)이다. 상한의 이만큼에 닿으면 경고한다.</summary>
    public const int WarningPercent = 90;

    /// <summary>경고 문턱(상한의 <see cref="WarningPercent"/>% — 올림, 58982)이다.</summary>
    public static int WarningThreshold => (WindowsExportLimit * WarningPercent + 99) / 100;

    /// <summary>
    /// PE 파일(DLL)의 export 함수 수(export 디렉터리의 NumberOfFunctions)다. export 디렉터리가 없으면 0이다. PE가 아니면
    /// <see cref="BadImageFormatException"/>·<see cref="InvalidDataException"/>, 읽을 수 없으면 <see cref="IOException"/>이다.
    /// </summary>
    public static int CountPeExports(string path)
    {
        using FileStream stream = File.OpenRead(path);
        using var reader = new PEReader(stream);
        PEHeader header = reader.PEHeaders.PEHeader ?? throw new InvalidDataException(path + " is not a PE image.");
        DirectoryEntry directory = header.ExportTableDirectory;
        if (directory.RelativeVirtualAddress == 0 || directory.Size == 0)
        {
            return 0;
        }

        // IMAGE_EXPORT_DIRECTORY의 NumberOfFunctions는 20 byte 위치의 uint32다.
        const int NumberOfFunctionsOffset = 20;
        PEMemoryBlock block = reader.GetSectionData(directory.RelativeVirtualAddress);
        System.Reflection.Metadata.BlobReader blob = block.GetReader();
        blob.Offset = NumberOfFunctionsOffset;
        return checked((int)blob.ReadUInt32());
    }

    /// <summary>
    /// Modular Windows 빌드의 진단 줄(빌드 결과 Notices — CLI가 note로 찍는다)이다: 모듈별 export 수 한 줄(<c>Modular exports: Core 4123, ...</c> — 큰 순서)과, 문턱
    /// (<paramref name="threshold"/>, 기본 <see cref="WarningThreshold"/>) 이상인 모듈마다 경고 한 줄. 공유 라이브러리가 없으면 빈 목록이다.
    /// </summary>
    public static IReadOnlyList<string> Diagnose(IReadOnlyDictionary<string, int> counts, int? threshold = null,
        IReadOnlyList<string>? unreadable = null)
    {
        unreadable ??= [];
        if (counts.Count == 0 && unreadable.Count == 0)
        {
            return [];
        }

        int limit = threshold ?? WarningThreshold;
        KeyValuePair<string, int>[] ordered = [.. counts.OrderByDescending(pair => pair.Value).ThenBy(pair => pair.Key, StringComparer.Ordinal)];
        var lines = new List<string> { "Modular exports: " + string.Join(", ", ordered.Select(pair => $"{pair.Key} {pair.Value}")) };
        foreach ((string module, int count) in ordered.Where(pair => pair.Value >= limit))
        {
            lines.Add($"Module {module} exports {count} symbols, at least {WarningPercent}% of the Windows DLL limit of {WindowsExportLimit}; " +
                "export members instead of whole classes where possible (Docs/Design/BuildTargets.ko.md section 3.8 BD27)");
        }

        foreach (string module in unreadable.Order(StringComparer.Ordinal))
        {
            lines.Add($"Module {module}: the export count could not be read from its shared library");
        }

        return lines;
    }

    /// <summary>
    /// 이번 계획이 공유 라이브러리로 링크한 Modular 모듈(Windows — <see cref="LinkTypes.CompilesAsSharedLibrary"/>, 컴파일할 코드가 있는 것)마다
    /// export 수를 센다. 읽지 못한 모듈은 unreadable에 담는다(링크가 끝난 빌드를 진단 때문에 실패시키지 않는다). Modular·Windows가 아니면 빈 표다.
    /// </summary>
    internal static (IReadOnlyDictionary<string, int> Counts, IReadOnlyList<string> Unreadable) Count(NativeBuildRequest request)
    {
        var counts = new Dictionary<string, int>(StringComparer.Ordinal);
        var unreadable = new List<string>();
        if (request.LinkType != NativeLinkType.Modular || request.Toolchain.Platform != NativePlatform.Windows)
        {
            return (counts, unreadable);
        }

        var modules = new Dictionary<string, NativeModuleInput>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput module in request.Modules)
        {
            modules.TryAdd(module.Name, module);
        }

        HashSet<string> entries = ModuleTables.EntryModules(modules);
        string bin = Path.Combine(request.OutputDirectory, "bin");
        string target = request.TargetName ?? request.Targets[0];
        foreach (NativeModuleInput module in modules.Values)
        {
            bool hasCode = module.Sources.Count != 0 || module.GeneratedSources.Count != 0 || module.ReflectionSources.Count != 0;
            if (!hasCode || !LinkTypes.CompilesAsSharedLibrary(module, request.LinkType, entries.Contains(module.Name)))
            {
                continue;
            }

            string path = Path.Combine(bin, LinkTypes.SharedLibraryFileName(target, module.Name, request.Toolchain.Platform, request.Configuration));
            if (!File.Exists(path))
            {
                continue;
            }

            try
            {
                counts[module.Name] = CountPeExports(path);
            }
            catch (Exception error) when (error is BadImageFormatException or InvalidDataException or IOException or UnauthorizedAccessException)
            {
                unreadable.Add(module.Name);
            }
        }

        return (counts, unreadable);
    }
}
