using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

internal static class BoundedFileTests
{
    public static void Register(TestSuite suite)
    {
        suite.Add("BoundedRead.EmptyExactAndExcess", () =>
        {
            using var empty = new MemoryStream([]);
            Verify.Equal(0, BoundedFile.Read(empty, 0).Length, "empty at zero limit");
            using var exact = new MemoryStream([1, 2, 3, 4]);
            Verify.That(BoundedFile.Read(exact, 4).SequenceEqual(new byte[] { 1, 2, 3, 4 }), "exact limit contents");
            using var exceeded = new MemoryStream([1, 2, 3, 4, 5, 6]);
            Verify.Throws<InvalidDataException>(() => BoundedFile.Read(exceeded, 4));
            Verify.Equal(5L, exceeded.Position, "read exceeded the limit plus one byte");
            using var invalid = new MemoryStream();
            Verify.Throws<ArgumentOutOfRangeException>(() => BoundedFile.Read(invalid, -1));
        });
        suite.Add("BoundedRead.StreamGrowingAfterLengthCheck", () =>
        {
            using var growing = new GrowingStream(1037);
            Verify.Equal(0L, growing.Length, "fixture initial length");
            Verify.Throws<InvalidDataException>(() => BoundedFile.Read(growing, 1024));
            Verify.Equal(1025, growing.BytesRead, "growing stream exceeded the read budget");
        });
        suite.Add("BoundedRead.RealIdentityFileBoundary", () =>
        {
            using TemporaryDirectory temporary = new();
            string path = temporary.FilePath("boundary.rocksids.json");
            using (var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write))
            {
                file.SetLength(BoundedFile.MaximumIdentityBytes);
            }

            Verify.Equal(BoundedFile.MaximumIdentityBytes,
                BoundedFile.Read(path, BoundedFile.MaximumIdentityBytes).Length, "exact real file limit rejected");
            using (var file = new FileStream(path, FileMode.Open, FileAccess.Write))
            {
                file.SetLength((long)BoundedFile.MaximumIdentityBytes + 1);
            }

            Verify.Throws<InvalidDataException>(() => BoundedFile.Read(path, BoundedFile.MaximumIdentityBytes));
        });
    }

    // 공개 길이가 0이어도 읽기 중 데이터가 계속 생기는 파일의 경계 사례를 재현한다.
    private sealed class GrowingStream(int available) : Stream
    {
        public int BytesRead { get; private set; }
        public override bool CanRead => true;
        public override bool CanSeek => true;
        public override bool CanWrite => false;
        public override long Length => 0;
        public override long Position { get => BytesRead; set => throw new NotSupportedException(); }
        public override int Read(byte[] buffer, int offset, int count)
        {
            int read = Math.Min(count, available - BytesRead);
            Array.Fill(buffer, (byte)0x5a, offset, read);
            BytesRead += read;
            return read;
        }
        public override void Flush() => throw new NotSupportedException();
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    }
}
