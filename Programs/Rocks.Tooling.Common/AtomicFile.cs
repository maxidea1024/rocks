using System.Security.Cryptography;
using System.Text;

namespace Rocks.Tooling.Common;

public static class Hashing
{
    public static string Sha256File(string path)
    {
        using var input = File.OpenRead(path);
        return Convert.ToHexStringLower(SHA256.HashData(input));
    }

    public static string Sha256Text(string text) =>
        Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(text)));
}

public static class AtomicFile
{
    public static bool WriteIfChanged(string path, string content) =>
        WriteIfChanged(path, Encoding.UTF8.GetBytes(content));

    public static bool WriteIfChanged(string path, byte[] content)
    {
        path = Path.GetFullPath(path);
        if (File.Exists(path) && File.ReadAllBytes(path).AsSpan().SequenceEqual(content))
        {
            return false;
        }

        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllBytes(temporary, content);
            File.Move(temporary, path, overwrite: true);
            return true;
        }
        finally
        {
            if (File.Exists(temporary))
            {
                File.Delete(temporary);
            }
        }
    }
}
