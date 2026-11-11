#include <Engine/PlatformBase/Private/Win64/WindowsPathGuard.h>
#include <Platform/File.h>
#include <Tests/Support/Test.h>
#include <string.h>

using namespace rocks::platform::detail;

TEST(PlatformFileWindowsDevicePathsAreRejectedBeforeOsAccess) {
    const char* rejected[] = {R"(\\.\PhysicalDrive0)",
                              R"(\\.\C:)",
                              R"(\\?\C:\file)",
                              R"(\??\C:\file)",
                              "//./PhysicalDrive0",
                              "NUL.txt",
                              "C:NUL",
                              "C:/temp/COM1.txt",
                              "lpt¹.log",
                              "CONIN$",
                              "CONOUT$"};
    for (const char* path : rejected) {
        CHECK(IsWindowsDevicePath(path, strlen(path)));
    }
    const char* accepted[] = {"C:/temp/normal.txt", "C:/temp/COM10.txt", "./NULsafe.txt",
                              "//server/share/file.bin"};
    for (const char* path : accepted) {
        CHECK(!IsWindowsDevicePath(path, strlen(path)));
    }
}

TEST(PlatformFileWindowsReservedNameRejectedAcrossEntryPoints) {
    auto& system = rocks::platform::PhysicalFileSystem();
    constexpr const char* name = "NUL.txt";
    bool exists = true;
    CHECK(system.Exists(name, 7, exists).code == rocks::platform::FileCode::Unsupported);
    CHECK(exists);
    CHECK(system.Delete(name, 7).code == rocks::platform::FileCode::Unsupported);
    rocks::platform::File file;
    CHECK(file.Open(system, name, 7, rocks::platform::FileOpenMode::CreateNew).code ==
          rocks::platform::FileCode::Unsupported);
    CHECK(!file.IsOpen());
}
