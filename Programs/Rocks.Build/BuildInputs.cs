using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Rocks.Build;

/// <summary>
/// 한 빌드가 읽은 입력 상태의 메모다(Docs/Design/UnityBuildPch.ko.md 4.1·4.3절). action key와 의존 기록 검증이 같은 파일을 action마다
/// 다시 읽지 않게 한다.
/// <list type="bullet">
/// <item><see cref="Content"/>: 파일 내용의 SHA-256이다. 빌드 안에서 파일마다 한 번 읽는다. 이 plan의 action 출력(object·archive·PCH)은
/// 파일을 다시 읽지 않고 그 action이 게시하거나 검증한 기록 해시(<see cref="RecordProduced"/>)를 쓴다.</item>
/// <item><see cref="Listing"/>: include·소스 디렉터리의 헤더 이름 목록(존재 집합) digest다. 내용은 담지 않는다 — 새 shadow header,
/// <c>__has_include</c> 결과 변화, 파일 삭제를 잡는다.</item>
/// <item><see cref="Declared"/>: 선언 추가 입력(파일 또는 디렉터리 전체)의 내용 digest다.</item>
/// <item><see cref="IsToolchainOwned"/>: 툴체인 소유 헤더인가. 그런 헤더는 의존 기록에 넣지 않는다(툴체인 key가 대신한다).</item>
/// </list>
/// 메모는 빌드 시작 때의 상태를 뜻한다. 빌드 중 입력이 바뀌었는지는 엔진의 InputSnapshot 전후 비교와
/// <see cref="ChangedDependencies"/>가 본다. 여러 action이 함께 부른다(스레드 안전).
/// </summary>
internal sealed class BuildInputs
{
    private readonly string root;

    private readonly string outputRoot;

    private readonly string[] ownedRoots;

    private readonly HashSet<string> produced;

    private readonly ConcurrentDictionary<string, Lazy<string>> contents = new(NativeToolchains.PathComparer);

    private readonly ConcurrentDictionary<string, string> producedHashes = new(NativeToolchains.PathComparer);

    private readonly ConcurrentDictionary<string, Lazy<string>> listings = new(NativeToolchains.PathComparer);

    private readonly ConcurrentDictionary<string, Lazy<string>> declared = new(NativeToolchains.PathComparer);

    // 이번 빌드가 의존 기록에 적은 파일 가운데 엔진의 InputSnapshot(선언 입력·header 디렉터리)이 덮지 않는 것이다.
    private readonly ConcurrentDictionary<string, string> uncoveredDependencies = new(NativeToolchains.PathComparer);

    private readonly string[] coveredDirectories;

    // 빌드 호출의 입력 메모다(메타 단계와 함께 쓴다 — Docs/Design/BuildReuse 2.6절). null이면 쓰지 않는다.
    private readonly InputMemo? memo;

    /// <param name="plan">이 빌드의 계획이다. null이면(action 하나만 실행하는 시험) 산출물·header 디렉터리 정보 없이 동작한다.</param>
    /// <param name="seed">BuildInfo Prepare와 첫 InputSnapshot이 읽은 digest다. 같은 파일을 다시 읽지 않는다.</param>
    public BuildInputs(string root, string outputRoot, NativeToolchain? toolchain, NativePlan? plan = null, FileDigests? seed = null,
        InputMemo? memo = null)
    {
        this.memo = memo;
        this.root = Path.GetFullPath(root);
        this.outputRoot = Path.GetFullPath(outputRoot);
        ownedRoots = toolchain is null ? [] : OwnedRoots(toolchain);
        produced = plan is null ? new(NativeToolchains.PathComparer)
            : plan.Actions.SelectMany(action => action.Outputs).ToHashSet(NativeToolchains.PathComparer);
        coveredDirectories = plan is null ? []
            : plan.Actions.SelectMany(action => action.HeaderDirectories).Distinct(NativeToolchains.PathComparer).ToArray();
        if (seed is not null)
        {
            foreach ((string path, string hash) in seed.RawEntries())
            {
                if (!produced.Contains(path))
                {
                    contents.TryAdd(path, new Lazy<string>(hash));
                }
            }
        }
    }

    /// <summary>
    /// 파일 내용의 SHA-256(소문자 hex)이다. 이 plan의 산출물이면 기록 해시를 쓰고(선행 action이 끝난 뒤에만 부른다), 기록이 없으면
    /// 파일을 읽는다(메모하지 않는다 — 산출물은 빌드 중 바뀐다).
    /// </summary>
    public string Content(string path)
    {
        string full = Path.GetFullPath(path);
        if (produced.Contains(full))
        {
            return producedHashes.TryGetValue(full, out string? recorded) ? recorded : Fingerprints.FileHash(full);
        }

        return contents.GetOrAdd(full, key => new Lazy<string>(() =>
            memo is not null && memo.TryCurrent(key, out string remembered) ? remembered : Fingerprints.FileHash(key))).Value;
    }

    /// <summary>이 plan의 action 출력인가.</summary>
    public bool IsProduced(string path) => produced.Contains(Path.GetFullPath(path));

    /// <summary>action이 방금 게시했거나 cache 검사로 확인한 산출물의 해시를 적는다. 그 산출물을 입력으로 받는 action이 쓴다.</summary>
    public void RecordProduced(string path, string hash) => producedHashes[Path.GetFullPath(path)] = hash;

    /// <summary>
    /// 디렉터리의 헤더 이름 목록 digest다(include 디렉터리 inventory와 같은 열거 — <see cref="Fingerprints.TreeFiles"/>의 headersOnly,
    /// 디렉터리 기준 상대 경로). 디렉터리가 없으면 <see cref="DirectoryNotFoundException"/>이다.
    /// </summary>
    public string Listing(string directory)
    {
        string full = Path.GetFullPath(directory);
        return listings.GetOrAdd(full, key => new Lazy<string>(() =>
        {
            if (!Directory.Exists(key))
            {
                throw new DirectoryNotFoundException(key);
            }

            return Fingerprints.Strings(Fingerprints.TreeFiles(key, true, outputRoot, root).Select(file => Path.GetRelativePath(key, file)));
        })).Value;
    }

    /// <summary>선언 추가 입력의 digest다: 파일이면 내용, 디렉터리면 그 아래 모든 파일의 경로와 내용이다.</summary>
    public string Declared(string path)
    {
        string full = Path.GetFullPath(path);
        if (produced.Contains(full))
        {
            return Content(full);
        }

        return declared.GetOrAdd(full, key => new Lazy<string>(() => Fingerprints.Paths([key]))).Value;
    }

    /// <summary>
    /// 툴체인 소유 헤더인가: <see cref="NativeToolchain.OwnedRoots"/>, <see cref="NativeToolchain.FingerprintPaths"/>, keyed 환경
    /// <c>INCLUDE</c>가 가리키는 디렉터리 아래다. 이 파일들의 변화는 툴체인 key가 잡는다.
    /// </summary>
    public bool IsToolchainOwned(string path)
    {
        string full = Path.GetFullPath(path);
        foreach (string owned in ownedRoots)
        {
            if (Fingerprints.IsUnder(full, owned))
            {
                return true;
            }
        }

        return false;
    }

    /// <summary>
    /// 의존 기록에 적은 파일을 알린다. 엔진의 InputSnapshot이 덮지 않는 파일(header 디렉터리 밖 — 상대 경로로 벗어난 include 등)은
    /// 빌드 끝에 <see cref="ChangedDependencies"/>가 다시 읽어 대조한다.
    /// </summary>
    public void NoteDependency(string path, string hash)
    {
        string full = Path.GetFullPath(path);
        if (!coveredDirectories.Any(directory => Fingerprints.IsUnder(full, directory)))
        {
            uncoveredDependencies.TryAdd(full, hash);
        }
    }

    /// <summary>InputSnapshot 밖의 의존 파일 가운데 기록한 해시와 지금 내용이 다른(또는 없어진) 파일이다.</summary>
    public IReadOnlyList<string> ChangedDependencies()
    {
        var changed = new List<string>();
        foreach ((string path, string hash) in uncoveredDependencies.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            if (!File.Exists(path) || Fingerprints.FileHash(path) != hash)
            {
                changed.Add(path);
            }
        }

        return changed;
    }

    internal static string[] OwnedRoots(NativeToolchain toolchain)
    {
        var roots = new List<string>(toolchain.OwnedRoots.Values);
        roots.AddRange(toolchain.FingerprintPaths);
        foreach ((string name, string? value) in Fingerprints.KeyedEnvironment(toolchain))
        {
            if (name == "INCLUDE" && value is not null)
            {
                roots.AddRange(value.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries));
            }
        }

        return roots.Where(Path.IsPathFullyQualified).Select(Path.GetFullPath).Distinct(NativeToolchains.PathComparer).ToArray();
    }
}

/// <summary>
/// 파일의 (크기, 수정 시각, 파일 ID)다(Docs/Design/UnityBuildPch.ko.md 4.3절). cache 검사는 셋이 모두 기록과 같을 때만 기록된 해시를
/// 믿고, 하나라도 다르거나 읽지 못하면 내용을 다시 해시한다. 파일 ID는 Windows에서 볼륨 일련번호와 파일 색인, 그 밖에서는 장치와
/// inode다. 파일 ID를 얻지 못하는 호스트에서는 <see cref="TryGet"/>이 거짓이라 언제나 다시 해시한다(느릴 뿐 판정은 같다).
/// </summary>
internal readonly record struct FileIdentity(long Size, long ModifiedTicks, string FileId)
{
    public static bool TryGet(string path, out FileIdentity identity)
    {
        identity = default;
        try
        {
            if (OperatingSystem.IsWindows())
            {
                using SafeFileHandle handle = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                if (!GetFileInformationByHandle(handle, out ByHandleFileInformation information))
                {
                    return false;
                }

                long size = ((long)information.FileSizeHigh << 32) | information.FileSizeLow;
                long modified = ((long)information.LastWriteTimeHigh << 32) | information.LastWriteTimeLow;
                identity = new(size, modified, FormattableString.Invariant(
                    $"{information.VolumeSerialNumber:x8}:{information.FileIndexHigh:x8}{information.FileIndexLow:x8}"));
                return true;
            }

            var file = new FileInfo(path);
            if (!file.Exists || !TryUnixFileId(path, out string fileId))
            {
                return false;
            }

            identity = new(file.Length, file.LastWriteTimeUtc.Ticks, fileId);
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or DllNotFoundException or EntryPointNotFoundException)
        {
            return false;
        }
    }

    // struct stat의 st_dev는 offset 0, st_ino는 offset 8의 64 bit다(Linux x86_64·aarch64, macOS의 64 bit inode 배치). macOS는 st_dev가
    // 32 bit이고 그 뒤 mode·nlink가 이어지므로 앞 4 byte만 장치로 읽는다. macOS x86_64의 64 bit inode 배치는 기호 stat$INODE64다.
    private const int StatBufferBytes = 512;

    private const int StatInodeOffset = 8;

    private static bool TryUnixFileId(string path, out string fileId)
    {
        fileId = "";
        byte[] buffer = new byte[StatBufferBytes];
        bool mac = OperatingSystem.IsMacOS();
        int result = mac && RuntimeInformation.ProcessArchitecture == Architecture.X64 ? StatInode64(path, buffer) : StatPlain(path, buffer);
        if (result != 0)
        {
            return false;
        }

        ulong device = mac ? BinaryPrimitives.ReadUInt32LittleEndian(buffer) : BinaryPrimitives.ReadUInt64LittleEndian(buffer);
        ulong inode = BinaryPrimitives.ReadUInt64LittleEndian(buffer.AsSpan(StatInodeOffset));
        fileId = FormattableString.Invariant($"{device:x}:{inode:x}");
        return true;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ByHandleFileInformation
    {
        public uint FileAttributes;
        public uint CreationTimeLow;
        public uint CreationTimeHigh;
        public uint LastAccessTimeLow;
        public uint LastAccessTimeHigh;
        public uint LastWriteTimeLow;
        public uint LastWriteTimeHigh;
        public uint VolumeSerialNumber;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint NumberOfLinks;
        public uint FileIndexHigh;
        public uint FileIndexLow;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandle(SafeFileHandle file, out ByHandleFileInformation information);

    [DllImport("libc", EntryPoint = "stat", SetLastError = true)]
    private static extern int StatPlain([MarshalAs(UnmanagedType.LPUTF8Str)] string path, byte[] buffer);

    [DllImport("libc", EntryPoint = "stat$INODE64", SetLastError = true)]
    private static extern int StatInode64([MarshalAs(UnmanagedType.LPUTF8Str)] string path, byte[] buffer);
}
