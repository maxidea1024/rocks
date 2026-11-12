namespace Rocks.Tooling.Common;

// 신뢰하지 않는 명세/인벤토리는 크기 조회 뒤 커질 수 있으므로 읽기 자체를 제한한다.
public static class BoundedFile
{
    public const int MaximumIdentityBytes = 16 * 1024 * 1024;

    public static byte[] Read(string path, int maximumBytes)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        using var stream = File.OpenRead(path);
        try
        {
            return Read(stream, maximumBytes);
        }
        catch (InvalidDataException error)
        {
            throw new InvalidDataException($"File exceeds {maximumBytes} byte limit: {path}", error);
        }
    }

    // stream은 caller가 소유한다. 길이를 미리 믿지 않고 최대 maximumBytes+1까지만 소비한다.
    public static byte[] Read(Stream stream, int maximumBytes)
    {
        ArgumentNullException.ThrowIfNull(stream);
        if (maximumBytes < 0 || maximumBytes == int.MaxValue)
        {
            throw new ArgumentOutOfRangeException(nameof(maximumBytes));
        }

        if (!stream.CanRead)
        {
            throw new ArgumentException("Stream must be readable.", nameof(stream));
        }

        byte[] buffer = new byte[Math.Min(8192, maximumBytes + 1)];
        using var output = new MemoryStream();
        int total = 0;
        while (true)
        {
            int requested = Math.Min(buffer.Length, maximumBytes - total + 1);
            int count = stream.Read(buffer, 0, requested);
            if (count == 0)
            {
                return output.ToArray();
            }

            if (count > maximumBytes - total)
            {
                throw new InvalidDataException($"Stream exceeds {maximumBytes} byte limit.");
            }

            int required = total + count;
            if (required > output.Capacity)
            {
                long grown = Math.Max((long)required, Math.Max(8192L, (long)output.Capacity * 2));
                output.Capacity = (int)Math.Min(maximumBytes, grown);
            }

            output.Write(buffer, 0, count);
            total = required;
        }
    }
}
