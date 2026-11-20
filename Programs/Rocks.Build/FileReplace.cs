using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace Rocks.Build;

/// <summary>
/// action 출력 게시(<see cref="Executor"/>)의 파일 교체다. 원본을 대상 경로로 옮기고 대상이 있으면 바꾼다. Windows에서는 POSIX 의미의 이름
/// 바꾸기(<c>SetFileInformationByHandle</c>, <c>FileRenameInfoEx</c>, <c>REPLACE_IF_EXISTS | POSIX_SEMANTICS</c>)로 바꿔, 다른 프로세스(백신·
/// 인덱서)가 대상을 삭제 공유(<c>FILE_SHARE_DELETE</c>)로 열고 있어도 성공한다 — <see cref="File.Move(string, string, bool)"/>는 그때 경로 없는
/// "Access to the path is denied"로 실패한다. 대기·재시도는 하지 않는다. 그 이름 바꾸기를 지원하지 않는 파일 시스템이면 File.Move로 돌아간다.
/// 실패하면 원본·대상 경로를 담은 IOException이다(Docs/Design/ClangVersionSupport.ko.md 개정 6 (자)).
/// </summary>
internal static class FileReplace
{
    // FILE_RENAME_INFO_EX 플래그와 정보 클래스다(winbase.h).
    private const int FileRenameInfoEx = 22;

    private const uint ReplaceIfExists = 0x1;

    private const uint PosixSemantics = 0x2;

    private const uint DeleteAccess = 0x00010000;

    private const uint ShareAll = 0x7;

    private const uint OpenExisting = 3;

    // 이 이름 바꾸기를 지원하지 않는 파일 시스템·Windows가 내는 오류다(ERROR_INVALID_PARAMETER, ERROR_NOT_SUPPORTED, ERROR_INVALID_FUNCTION).
    private static readonly int[] UnsupportedErrors = [87, 50, 1];

    public static void Move(string source, string destination)
    {
        try
        {
            if (!OperatingSystem.IsWindows() || !TryPosixRename(source, destination))
            {
                File.Move(source, destination, true);
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            throw new IOException($"Cannot publish {source} -> {destination}: {error.Message}", error);
        }
    }

    // 지원하지 않으면 거짓이고, 그 밖의 실패는 IOException이다.
    private static bool TryPosixRename(string source, string destination)
    {
        using SafeFileHandle handle = CreateFileW(ExtendedPath(source), DeleteAccess, ShareAll, IntPtr.Zero, OpenExisting, 0, IntPtr.Zero);
        if (handle.IsInvalid)
        {
            throw new IOException(new Win32Exception(Marshal.GetLastPInvokeError()).Message);
        }

        // FILE_RENAME_INFO: Flags(4) + 맞춤(4) + RootDirectory(포인터) + FileNameLength(4) + FileName(UTF-16, 끝 0 포함).
        byte[] name = Encoding.Unicode.GetBytes(ExtendedPath(destination));
        int nameOffset = 8 + IntPtr.Size + 4;
        int size = nameOffset + name.Length + 2;
        IntPtr buffer = Marshal.AllocHGlobal(size);
        try
        {
            for (int index = 0; index < size; ++index)
            {
                Marshal.WriteByte(buffer, index, 0);
            }

            Marshal.WriteInt32(buffer, 0, unchecked((int)(ReplaceIfExists | PosixSemantics)));
            Marshal.WriteInt32(buffer, 8 + IntPtr.Size, name.Length);
            Marshal.Copy(name, 0, buffer + nameOffset, name.Length);
            if (SetFileInformationByHandle(handle, FileRenameInfoEx, buffer, size))
            {
                return true;
            }

            int error = Marshal.GetLastPInvokeError();
            return UnsupportedErrors.Contains(error) ? false : throw new IOException(new Win32Exception(error).Message);
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    // Win32 API에 넘길 확장 길이 경로다(`\\?\C:\…`, UNC는 `\\?\UNC\서버\…`). .NET 파일 API는 긴 경로를 스스로 바꾸지만 직접 부르는 Win32
    // 함수는 MAX_PATH(260자)를 넘는 경로를 찾지 못한다.
    private static string ExtendedPath(string path)
    {
        string full = Path.GetFullPath(path);
        return full.StartsWith(@"\\?\", StringComparison.Ordinal) ? full
            : full.StartsWith(@"\\", StringComparison.Ordinal) ? @"\\?\UNC\" + full[2..]
            : @"\\?\" + full;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetFileInformationByHandle(SafeFileHandle handle, int informationClass, IntPtr information, int size);
}
