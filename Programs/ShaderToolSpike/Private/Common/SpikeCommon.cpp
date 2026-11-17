#include "SpikeCommon.h"

#include <Core/Memory/SystemAllocator.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <stdio.h>
#include <string.h>

namespace rocks::spike {

namespace {

// DXIL container 머리: fourCC "DXBC"(little endian 0x43425844) 다음 16 byte digest다.
constexpr uint32 ContainerFourCc = 0x43425844u;
constexpr usize ContainerDigestOffset = 4;
// DXC 업스트림 include/dxc/DxilContainer/DxilContainer.h의 PreviewByPassHash 각 byte 값이다.
constexpr uint8 PreviewBypassHashByte = 0x02;

bool AppendPath(StringBuilderBase& path, const char* root, const char* name) noexcept {
    path.Append(StringView(root)).Append(StringView("/")).Append(StringView(name));
    return !path.HasFailed();
}

} // namespace

Allocator& SpikeAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

const char* StageProfilePrefix(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::Vertex:
        return "vs";
    case ShaderStage::Pixel:
        return "ps";
    case ShaderStage::Compute:
        return "cs";
    }

    return "xs";
}

Report::Report() noexcept : text_(SpikeAllocator()), writer_(text_) {
}

void Report::Remember(Result<void> result) noexcept {
    if (!result) {
        failed_ = true;
    }
}

void Report::BeginObject() noexcept {
    Remember(writer_.BeginObject());
}

void Report::BeginObject(StringView key) noexcept {
    Remember(writer_.Key(key));
    Remember(writer_.BeginObject());
}

void Report::EndObject() noexcept {
    Remember(writer_.EndObject());
}

void Report::BeginArray(StringView key) noexcept {
    Remember(writer_.Key(key));
    Remember(writer_.BeginArray());
}

void Report::EndArray() noexcept {
    Remember(writer_.EndArray());
}

void Report::String(StringView key, StringView value) noexcept {
    Remember(writer_.Key(key));
    Remember(writer_.String(value));
}

void Report::Bool(StringView key, bool value) noexcept {
    Remember(writer_.Key(key));
    Remember(writer_.Bool(value));
}

void Report::UInt(StringView key, uint64 value) noexcept {
    Remember(writer_.Key(key));
    Remember(writer_.UInt64(value));
}

void Report::Double(StringView key, double value) noexcept {
    Remember(writer_.Key(key));
    Remember(writer_.Double(value));
}

void Report::Element(StringView value) noexcept {
    Remember(writer_.String(value));
}

bool Report::Finish() noexcept {
    Remember(writer_.Finish());
    return !failed_;
}

OutputDirectory::OutputDirectory(const char* root) noexcept
    : root_(root), entries_(SpikeAllocator()) {
}

bool OutputDirectory::Create() noexcept {
    const platform::FileStatus made = platform::MakeDirectoryTree(
        platform::PhysicalFileSystem(), root_, platform::CStringLength(root_));
    if (!made) {
        fprintf(stderr, "cannot create output directory %s (file code %d, native %u)\n", root_,
                static_cast<int>(made.code), made.nativeError);
        return false;
    }

    return true;
}

bool OutputDirectory::WriteFile(const char* name, Span<const uint8> bytes) noexcept {
    StringBuilder<512> path(SpikeAllocator());
    if (!AppendPath(path, root_, name)) {
        return false;
    }

    platform::File file;
    if (const platform::FileStatus opened =
            file.Open(path.CString(), platform::FileOpenMode::CreateAlways);
        !opened) {
        fprintf(stderr, "cannot create %s (file code %d)\n", path.CString(),
                static_cast<int>(opened.code));
        return false;
    }

    const platform::FileTransfer written = file.WriteAll(bytes.Data(), bytes.Size());
    const platform::FileStatus closed = file.Close();
    if (!written.status || written.bytes != bytes.Size() || !closed) {
        fprintf(stderr, "cannot write %s\n", path.CString());
        return false;
    }

    return true;
}

bool OutputDirectory::Write(const char* name, Span<const uint8> bytes) noexcept {
    Entry entry;
    const usize length = platform::CStringLength(name);
    if (length + 1 > sizeof(entry.name)) {
        fprintf(stderr, "output name too long: %s\n", name);
        return false;
    }

    for (const Entry& existing : entries_) {
        if (platform::CStringEquals(existing.name, name)) {
            fprintf(stderr, "output written twice: %s\n", name);
            return false;
        }
    }

    if (!WriteFile(name, bytes)) {
        return false;
    }

    memcpy(entry.name, name, length + 1);
    entry.bytes = bytes.Size();
    entry.digest = Sha256::Hash(bytes);
    return static_cast<bool>(entries_.TryPushBack(entry));
}

bool OutputDirectory::WriteText(const char* name, StringView text) noexcept {
    return Write(name, AsBytes(text));
}

bool OutputDirectory::WriteUnlisted(const char* name, StringView text) noexcept {
    return WriteFile(name, AsBytes(text));
}

bool OutputDirectory::WriteManifest() noexcept {
    Report manifest;
    manifest.BeginObject();
    manifest.BeginArray("files");
    for (const Entry& entry : entries_) {
        char hex[Sha256Digest::HexBytes + 1];
        entry.digest.ToHex(hex);
        manifest.BeginObject();
        manifest.String("name", StringView(entry.name));
        manifest.UInt("bytes", entry.bytes);
        manifest.String("sha256", StringView(hex));
        manifest.EndObject();
    }

    manifest.EndArray();
    manifest.EndObject();
    if (!manifest.Finish()) {
        return false;
    }

    return WriteFile("manifest.json", AsBytes(manifest.Text()));
}

bool ReadWholeFile(const char* path, ByteArray& bytes) noexcept {
    platform::File file;
    if (!file.Open(path, platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    if (!file.Size(size) || !bytes.TryResize(static_cast<usize>(size))) {
        (void)file.Close();
        return false;
    }

    const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
    const platform::FileStatus closed = file.Close();
    return read.status && read.bytes == bytes.Size() && closed;
}

Span<const uint8> AsBytes(StringView text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount());
}

StringView AsText(Span<const uint8> bytes) noexcept {
    return StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
}

ContainerDigest ReadContainerDigest(Span<const uint8> container) noexcept {
    ContainerDigest digest;
    if (container.Size() < ContainerDigestOffset + ContainerDigest::Bytes) {
        return digest;
    }

    uint32 fourCc = 0;
    memcpy(&fourCc, container.Data(), sizeof(fourCc));
    if (fourCc != ContainerFourCc) {
        return digest;
    }

    digest.present = true;
    memcpy(digest.bytes, container.Data() + ContainerDigestOffset, ContainerDigest::Bytes);
    bool zero = true;
    bool bypass = true;
    for (const uint8 value : digest.bytes) {
        zero = zero && value == 0;
        bypass = bypass && value == PreviewBypassHashByte;
    }

    digest.isSigned = !zero && !bypass;
    return digest;
}

void ContainerDigestHex(const ContainerDigest& digest,
                        char (&out)[ContainerDigest::Bytes * 2 + 1]) noexcept {
    constexpr char Digits[] = "0123456789abcdef";
    for (usize index = 0; index < ContainerDigest::Bytes; ++index) {
        out[index * 2] = Digits[digest.bytes[index] >> 4];
        out[index * 2 + 1] = Digits[digest.bytes[index] & 0x0f];
    }

    out[ContainerDigest::Bytes * 2] = '\0';
}

} // namespace rocks::spike
