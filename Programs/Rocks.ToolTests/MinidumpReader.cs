using System.Buffers.Binary;
using System.Text;

namespace Rocks.ToolTests;

/// <summary>
/// 크래시 시험(Docs/Design/CrashHandlingAndLaunch.ko.md 17절)이 쓰는 최소 minidump 판독기다. 파일 머리·stream 목록을 검사하고
/// 예외 stream(실패 스레드 id·예외 코드), 스레드 목록, 모듈 목록(CodeView PDB GUID·age), 사용자 stream을 읽는다. 형식은
/// Windows SDK <c>minidumpapiset.h</c>의 구조체 배치(little-endian, 압축 없음)를 따른다. 잘못된 파일은
/// <see cref="InvalidDataException"/>이다.
/// </summary>
internal sealed class MinidumpReader
{
    // MINIDUMP_HEADER.Signature("MDMP")와 MINIDUMP_STREAM_TYPE 값이다.
    private const uint Signature = 0x504D444D;

    private const uint ThreadListStream = 3;

    private const uint ModuleListStream = 4;

    private const uint ExceptionStream = 6;

    private const uint CodeViewSignature = 0x53445352;

    private const int HeaderBytes = 32;

    private const int DirectoryEntryBytes = 12;

    private const int ThreadBytes = 48;

    private const int ModuleBytes = 108;

    private readonly byte[] data;

    private readonly Dictionary<uint, (uint Rva, uint Size)> streams = [];

    private MinidumpReader(byte[] data)
    {
        this.data = data;
        if (data.Length < HeaderBytes || ReadUInt32(0) != Signature)
        {
            throw new InvalidDataException("Not a minidump (MDMP signature missing)");
        }

        uint count = ReadUInt32(8);
        uint directory = ReadUInt32(12);
        Require(directory, (long)count * DirectoryEntryBytes, "stream directory");
        for (uint index = 0; index < count; index++)
        {
            int entry = checked((int)(directory + index * DirectoryEntryBytes));
            uint type = ReadUInt32(entry);
            uint size = ReadUInt32(entry + 4);
            uint rva = ReadUInt32(entry + 8);
            if (type == 0)
            {
                continue;
            }

            Require(rva, size, "stream " + type);
            streams.TryAdd(type, (rva, size));
        }

        StreamTypes = streams.Keys.ToArray();
    }

    public IReadOnlyList<uint> StreamTypes { get; }

    public bool HasException => streams.ContainsKey(ExceptionStream);

    public static MinidumpReader Open(string path) => new(File.ReadAllBytes(path));

    /// <summary>예외 stream의 실패 스레드 id와 예외 코드다. 없으면 <see cref="InvalidDataException"/>이다.</summary>
    public (uint ThreadId, uint Code, ulong Address) Exception()
    {
        if (!streams.TryGetValue(ExceptionStream, out var stream) || stream.Size < 32)
        {
            throw new InvalidDataException("Exception stream missing");
        }

        int offset = checked((int)stream.Rva);
        // MINIDUMP_EXCEPTION_STREAM: ThreadId, __alignment, MINIDUMP_EXCEPTION(Code, Flags, Record, Address, ...)
        return (ReadUInt32(offset), ReadUInt32(offset + 8), ReadUInt64(offset + 8 + 16));
    }

    /// <summary>스레드 목록 stream의 스레드 id들이다.</summary>
    public IReadOnlyList<uint> ThreadIds()
    {
        if (!streams.TryGetValue(ThreadListStream, out var stream))
        {
            throw new InvalidDataException("Thread list stream missing");
        }

        int offset = checked((int)stream.Rva);
        uint count = ReadUInt32(offset);
        Require((uint)(offset + 4), (long)count * ThreadBytes, "thread list");
        var ids = new List<uint>();
        for (uint index = 0; index < count; index++)
        {
            ids.Add(ReadUInt32(checked((int)(offset + 4 + index * ThreadBytes))));
        }

        return ids;
    }

    /// <summary>모듈 목록에서 파일 이름이 <paramref name="fileName"/>인 모듈의 CodeView PDB GUID(대문자 레지스트리 형식)와 age다.</summary>
    public (string Guid, uint Age, string PdbName) ModuleCodeView(string fileName)
    {
        if (!streams.TryGetValue(ModuleListStream, out var stream))
        {
            throw new InvalidDataException("Module list stream missing");
        }

        int offset = checked((int)stream.Rva);
        uint count = ReadUInt32(offset);
        Require((uint)(offset + 4), (long)count * ModuleBytes, "module list");
        for (uint index = 0; index < count; index++)
        {
            int module = checked((int)(offset + 4 + index * ModuleBytes));
            string name = ReadString(ReadUInt32(module + 20));
            if (!string.Equals(Path.GetFileName(name), fileName, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            // CvRecord는 BaseOfImage(8)·SizeOfImage·CheckSum·TimeDateStamp·ModuleNameRva(4×4)·VS_FIXEDFILEINFO(52) 뒤다.
            uint cvSize = ReadUInt32(module + 76);
            uint cvRva = ReadUInt32(module + 80);
            Require(cvRva, cvSize, "CodeView record");
            if (cvSize < 24 || ReadUInt32(checked((int)cvRva)) != CodeViewSignature)
            {
                throw new InvalidDataException("Module " + fileName + " has no RSDS CodeView record");
            }

            var guid = new Guid(data.AsSpan(checked((int)cvRva + 4), 16));
            uint age = ReadUInt32(checked((int)cvRva + 20));
            int nameStart = checked((int)cvRva + 24);
            int nameEnd = Array.IndexOf(data, (byte)0, nameStart, checked((int)cvSize - 24));
            string pdb = Encoding.UTF8.GetString(data, nameStart, (nameEnd < 0 ? nameStart + (int)cvSize - 24 : nameEnd) - nameStart);
            return (guid.ToString("D").ToUpperInvariant(), age, pdb);
        }

        throw new InvalidDataException("Module " + fileName + " missing from the module list");
    }

    /// <summary>종류가 <paramref name="type"/>인 stream의 내용이다. 없으면 null이다.</summary>
    public byte[]? Stream(uint type)
    {
        return streams.TryGetValue(type, out var stream) ? data.AsSpan(checked((int)stream.Rva), checked((int)stream.Size)).ToArray() : null;
    }

    private void Require(uint rva, long size, string what)
    {
        if (size < 0 || rva + size > data.Length)
        {
            throw new InvalidDataException("Minidump " + what + " is outside the file");
        }
    }

    private uint ReadUInt32(int offset)
    {
        Require((uint)offset, 4, "field");
        return BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(offset, 4));
    }

    private ulong ReadUInt64(int offset)
    {
        Require((uint)offset, 8, "field");
        return BinaryPrimitives.ReadUInt64LittleEndian(data.AsSpan(offset, 8));
    }

    // MINIDUMP_STRING: Length(byte 수, NUL 제외)와 UTF-16 문자열이다.
    private string ReadString(uint rva)
    {
        uint length = ReadUInt32(checked((int)rva));
        Require(rva + 4, length, "string");
        return Encoding.Unicode.GetString(data, checked((int)rva + 4), checked((int)length));
    }
}
