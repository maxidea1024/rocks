#include <Platform/File.h>
#include <TestSupport/DynamicLibraryFixture.h>

// UTF-8·UTF-16 변환은 PlatformBase Win64 backend의 것을 그대로 쓴다(시험 도구에 따로 두지 않는다).
#include <PlatformBase/Private/Win64/WindowsString.h>

#include <Windows.h>
#include <string.h>

// 데이터 전용 PE32+ DLL을 쓴다. 구성(파일 정렬 0x200, 메모리 정렬 0x1000):
//   [0x000] DOS 머리(MZ, e_lfanew = 0x40) → PE 서명 → COFF 머리 → PE32+ 선택 머리 → 구역 표(1개)
//   [0x200] 구역 ".data"(RVA 0x1000, 읽기·쓰기 데이터):
//           export 값(uint32) → import 칸(IAT) → import 이름 표(ILT) → import 서술자 → hint/name →
//           import DLL 이름 → export 디렉터리 → 주소 표 → 이름 표(정렬) → 서수 표 → 이름 문자열
// 코드·재배치·진입점이 없다. 데이터 안에 절대 주소가 없으므로(모두 RVA) 어느 주소에 올려도
// 재배치가 필요 없고, import 칸은 로더가 채운다. export 값과 import 칸은 export 디렉터리 범위 밖에
// 두어야 한다(범위 안을 가리키는 export는 로더가 forwarder 문자열로 해석한다).
namespace rocks::test {

namespace {

constexpr DWORD FileAlignment = 0x200;
constexpr DWORD SectionAlignment = 0x1000;
constexpr DWORD SectionRva = 0x1000;
constexpr usize MaxImageBytes = 16u * 1024u;
constexpr usize MaxExports = 16;

constexpr DWORD AlignUp(DWORD value, DWORD alignment) noexcept {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

// 구역 내용을 차례로 쌓는 버퍼다. 위치는 구역 시작 기준 offset이고 RVA는 SectionRva를 더한다.
class SectionWriter {
public:
    explicit SectionWriter(uint8* data, usize capacity) noexcept
        : data_(data), capacity_(capacity) {
    }

    // alignment로 맞춘 뒤 bytes만큼 자리를 잡고 그 offset을 준다. 넘치면 ok가 false가 된다.
    usize Reserve(usize bytes, usize alignment) noexcept {
        size_ = (size_ + alignment - 1u) & ~(alignment - 1u);
        const usize offset = size_;
        if (offset + bytes > capacity_) {
            ok_ = false;
            return 0;
        }

        size_ += bytes;
        return offset;
    }

    usize String(const char* text) noexcept {
        const usize bytes = strlen(text) + 1u;
        const usize offset = Reserve(bytes, 1);
        if (ok_) {
            memcpy(data_ + offset, text, bytes);
        }

        return offset;
    }

    template <class T>
    void Put(usize offset, T value) noexcept {
        if (ok_) {
            memcpy(data_ + offset, &value, sizeof(T));
        }
    }

    static DWORD Rva(usize offset) noexcept {
        return SectionRva + static_cast<DWORD>(offset);
    }

    usize Size() const noexcept {
        return size_;
    }

    bool Ok() const noexcept {
        return ok_;
    }

private:
    uint8* data_;
    usize capacity_;
    usize size_ = 0;
    bool ok_ = true;
};

// 실행 중인 프로세스 이미지의 machine 값이다(x64 0x8664, ARM64 0xAA64). 같은 값이어야 로더가
// 싣는다.
WORD CurrentMachine() noexcept {
    const auto* base = reinterpret_cast<const uint8*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->FileHeader.Machine;
}

// 경로의 마지막 구성 요소(파일 이름)다.
const char* BaseName(const char* path) noexcept {
    const char* name = path;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            name = cursor + 1;
        }
    }

    return name;
}

struct NamedExport {
    const char* name;
    usize valueOffset;
};

// Rocks 파일 API로 쓴다(긴 경로의 확장 경로 처리를 파일 backend와 같이 한다).
bool WriteBytes(const char* path, const void* data, usize bytes) noexcept {
    platform::File file;
    if (!file.Open(platform::PhysicalFileSystem(), path, strlen(path),
                   platform::FileOpenMode::CreateAlways)) {
        return false;
    }

    const platform::FileTransfer written = file.WriteAll(data, bytes);
    const bool closed = static_cast<bool>(file.Close());
    return written.status && written.bytes == bytes && closed;
}

} // namespace

bool DynamicLibraryFixturesSupported() noexcept {
    return true;
}

const char* DynamicLibraryExtension() noexcept {
    return ".dll";
}

bool WriteFixtureLibrary(const char* path, const FixtureLibrary& library) noexcept {
    const bool imports = library.importLibrary != nullptr;
    if (path == nullptr || library.exportCount + (imports ? 1u : 0u) == 0 ||
        library.exportCount + 1u > MaxExports ||
        (library.exportCount != 0 && library.exports == nullptr) ||
        (imports && (library.importSymbol == nullptr || library.importSlotExport == nullptr))) {
        return false;
    }

    static uint8 image[MaxImageBytes];
    memset(image, 0, sizeof(image));
    SectionWriter section(image + FileAlignment, sizeof(image) - FileAlignment);

    // export 값과 import 칸(export 디렉터리 범위 밖).
    NamedExport named[MaxExports]{};
    usize namedCount = 0;
    for (usize i = 0; i < library.exportCount; ++i) {
        const usize offset = section.Reserve(sizeof(uint32), sizeof(uint32));
        section.Put(offset, library.exports[i].value);
        named[namedCount++] = {library.exports[i].name, offset};
    }

    usize importDescriptor = 0;
    if (imports) {
        // PE32+의 thunk는 8 byte이며 표는 0으로 끝난다. IAT와 ILT는 처음에 같은 hint/name RVA를
        // 담고, 로더가 IAT 칸을 실제 주소로 바꾼다.
        const usize iat = section.Reserve(2u * sizeof(uint64), sizeof(uint64));
        const usize ilt = section.Reserve(2u * sizeof(uint64), sizeof(uint64));
        importDescriptor = section.Reserve(2u * sizeof(IMAGE_IMPORT_DESCRIPTOR), sizeof(DWORD));
        const usize hintName = section.Reserve(sizeof(WORD), sizeof(WORD));
        section.String(library.importSymbol);
        const usize dllName = section.String(library.importLibrary);

        section.Put(iat, static_cast<uint64>(SectionWriter::Rva(hintName)));
        section.Put(ilt, static_cast<uint64>(SectionWriter::Rva(hintName)));
        IMAGE_IMPORT_DESCRIPTOR descriptor{};
        descriptor.OriginalFirstThunk = SectionWriter::Rva(ilt);
        descriptor.Name = SectionWriter::Rva(dllName);
        descriptor.FirstThunk = SectionWriter::Rva(iat);
        section.Put(importDescriptor, descriptor);
        named[namedCount++] = {library.importSlotExport, iat};
    }

    // 로더는 이름 표를 이진 탐색하므로 byte 순서로 정렬한다.
    for (usize i = 1; i < namedCount; ++i) {
        for (usize j = i; j > 0 && strcmp(named[j - 1].name, named[j].name) > 0; --j) {
            const NamedExport swapped = named[j - 1];
            named[j - 1] = named[j];
            named[j] = swapped;
        }
    }

    const usize exportDirectory = section.Reserve(sizeof(IMAGE_EXPORT_DIRECTORY), sizeof(DWORD));
    const usize functions = section.Reserve(namedCount * sizeof(DWORD), sizeof(DWORD));
    const usize names = section.Reserve(namedCount * sizeof(DWORD), sizeof(DWORD));
    const usize ordinals = section.Reserve(namedCount * sizeof(WORD), sizeof(WORD));
    for (usize i = 0; i < namedCount; ++i) {
        const usize name = section.String(named[i].name);
        section.Put(functions + i * sizeof(DWORD), SectionWriter::Rva(named[i].valueOffset));
        section.Put(names + i * sizeof(DWORD), SectionWriter::Rva(name));
        section.Put(ordinals + i * sizeof(WORD), static_cast<WORD>(i));
    }

    const usize moduleName = section.String(BaseName(path));
    const usize exportEnd = section.Size();
    if (!section.Ok()) {
        return false;
    }

    IMAGE_EXPORT_DIRECTORY exports{};
    exports.Name = SectionWriter::Rva(moduleName);
    exports.Base = 1;
    exports.NumberOfFunctions = static_cast<DWORD>(namedCount);
    exports.NumberOfNames = static_cast<DWORD>(namedCount);
    exports.AddressOfFunctions = SectionWriter::Rva(functions);
    exports.AddressOfNames = SectionWriter::Rva(names);
    exports.AddressOfNameOrdinals = SectionWriter::Rva(ordinals);
    section.Put(exportDirectory, exports);

    const DWORD virtualSize = static_cast<DWORD>(section.Size());
    const DWORD rawSize = AlignUp(virtualSize, FileAlignment);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x40;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = CurrentMachine();
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->FileHeader.Characteristics =
        IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE | IMAGE_FILE_DLL;

    IMAGE_OPTIONAL_HEADER64& optional = nt->OptionalHeader;
    optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    optional.SizeOfInitializedData = rawSize;
    optional.BaseOfCode = SectionRva;
    optional.ImageBase = 0x180000000ull;
    optional.SectionAlignment = SectionAlignment;
    optional.FileAlignment = FileAlignment;
    optional.MajorOperatingSystemVersion = 6;
    optional.MajorSubsystemVersion = 6;
    optional.SizeOfImage = SectionRva + AlignUp(virtualSize, SectionAlignment);
    optional.SizeOfHeaders = FileAlignment;
    optional.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
    optional.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE |
                                  IMAGE_DLLCHARACTERISTICS_NX_COMPAT |
                                  IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA;
    optional.SizeOfStackReserve = 0x100000;
    optional.SizeOfStackCommit = 0x1000;
    optional.SizeOfHeapReserve = 0x100000;
    optional.SizeOfHeapCommit = 0x1000;
    optional.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {
        SectionWriter::Rva(exportDirectory), static_cast<DWORD>(exportEnd - exportDirectory)};
    if (imports) {
        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {
            SectionWriter::Rva(importDescriptor), 2u * sizeof(IMAGE_IMPORT_DESCRIPTOR)};
    }

    auto* header = reinterpret_cast<IMAGE_SECTION_HEADER*>(nt + 1);
    memcpy(header->Name, ".data", 5);
    header->Misc.VirtualSize = virtualSize;
    header->VirtualAddress = SectionRva;
    header->SizeOfRawData = rawSize;
    header->PointerToRawData = FileAlignment;
    header->Characteristics =
        IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

    return WriteBytes(path, image, FileAlignment + rawSize);
}

bool SystemLibraryForTest(char* path, usize capacity, const char*& systemName,
                          const char*& symbol) noexcept {
    wchar_t directory[MAX_PATH];
    const UINT units = GetSystemDirectoryW(directory, MAX_PATH);
    if (units == 0 || units >= MAX_PATH) {
        return false;
    }

    usize bytes = 0;
    if (!platform::detail::WideToUtf8(directory, static_cast<int>(units), path, capacity, bytes)) {
        return false;
    }

    constexpr const char* Name = "kernel32.dll";
    const usize nameBytes = strlen(Name);
    if (bytes + 1u + nameBytes + 1u > capacity) {
        return false;
    }

    path[bytes] = '\\';
    memcpy(path + bytes + 1u, Name, nameBytes + 1u);
    systemName = Name;
    symbol = "GetTickCount64";
    return true;
}

bool LibraryIsLoaded(const char* fileName) noexcept {
    platform::detail::WideString wide;
    return wide.AssignUtf8(fileName, strlen(fileName)) && GetModuleHandleW(wide.Value()) != nullptr;
}

bool LoadWithDefaultSearch(const char* path) noexcept {
    platform::detail::WideString wide;
    if (!wide.AssignUtf8(path, strlen(path))) {
        return false;
    }

    DWORD previous = 0;
    (void)SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous);
    const HMODULE module = LoadLibraryExW(wide.Value(), nullptr, 0);
    (void)SetThreadErrorMode(previous, nullptr);
    if (module == nullptr) {
        return false;
    }

    FreeLibrary(module);
    return true;
}

bool PrependSearchPath(const char* directory) noexcept {
    platform::detail::WideString wide;
    if (!wide.AssignUtf8(directory, strlen(directory))) {
        return false;
    }

    static wchar_t value[32768];
    const usize prefix = wide.Units();
    if (prefix + 2u >= sizeof(value) / sizeof(value[0])) {
        return false;
    }

    memcpy(value, wide.Value(), prefix * sizeof(wchar_t));
    value[prefix] = L';';
    const DWORD capacity = static_cast<DWORD>(sizeof(value) / sizeof(value[0]) - prefix - 1u);
    const DWORD existing = GetEnvironmentVariableW(L"PATH", value + prefix + 1u, capacity);
    if (existing >= capacity) {
        return false;
    }

    value[prefix + 1u + existing] = L'\0';
    return SetEnvironmentVariableW(L"PATH", value) != 0;
}

} // namespace rocks::test
