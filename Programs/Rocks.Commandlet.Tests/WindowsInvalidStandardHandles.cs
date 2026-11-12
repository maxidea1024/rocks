using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

internal static class WindowsInvalidStandardHandles
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct StartupInfo
    {
        public uint Size;
        public IntPtr Reserved, Desktop, Title;
        public uint X, Y, XSize, YSize, XCountChars, YCountChars, FillAttribute, Flags;
        public ushort ShowWindow, Reserved2Size;
        public IntPtr Reserved2, StdInput, StdOutput, StdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation
    {
        public IntPtr Process, Thread;
        public uint ProcessId, ThreadId;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateProcessW(string applicationName, StringBuilder commandLine,
        IntPtr processAttributes, IntPtr threadAttributes, [MarshalAs(UnmanagedType.Bool)] bool inheritHandles,
        uint creationFlags, IntPtr environment, string currentDirectory, ref StartupInfo startup,
        out ProcessInformation process);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TerminateProcess(IntPtr process, uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    internal static uint Run(string executable, string argument, string workingDirectory)
    {
        var invalid = new IntPtr(-1);
        var startup = new StartupInfo
        {
            Size = (uint)Marshal.SizeOf<StartupInfo>(),
            Flags = 0x100, // STARTF_USESTDHANDLES
            StdInput = invalid,
            StdOutput = invalid,
            StdError = invalid
        };
        var commandLine = new StringBuilder($"\"{executable}\" {argument}");
        if (!CreateProcessW(executable, commandLine, IntPtr.Zero, IntPtr.Zero, true, 0x08000000,
                IntPtr.Zero, workingDirectory, ref startup, out var child))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateProcessW failed");
        try
        {
            uint wait = WaitForSingleObject(child.Process, 15000);
            if (wait == 258)
            {
                if (!TerminateProcess(child.Process, 1))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "TerminateProcess failed");
                _ = WaitForSingleObject(child.Process, 3000);
                throw new TimeoutException("Commandlet process exceeded deadline");
            }
            if (wait != 0 || !GetExitCodeProcess(child.Process, out uint code))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Commandlet wait failed");
            return code;
        }
        finally
        {
            _ = CloseHandle(child.Thread);
            _ = CloseHandle(child.Process);
        }
    }
}
