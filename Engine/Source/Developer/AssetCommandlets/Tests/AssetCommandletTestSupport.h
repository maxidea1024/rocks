#pragma once

#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "../../AssetDatabase/Tests/AssetImportTestSupport.h"

#include <AssetCommandlets/AssetCommandletSupport.h>
#include <Core/Commandlet/Commandlet.h>
#include <Platform/File.h>

// 에셋 명령 시험의 공용 도구다(명령 함수를 이 프로세스에서 부른다): 출력 수집, 시험 임포터 등록,
// 디스크 프로젝트.
namespace rocks::commandlet_test {

// 표준 출력·오류를 모은다.
struct Captured {
    explicit Captured(Allocator& allocator) noexcept : out(allocator), error(allocator) {
    }

    static Result<void> Write(void* context, CommandletStream stream, StringView text) noexcept {
        Captured& captured = *static_cast<Captured*>(context);
        if (stream == captured.failStream && captured.failWrites) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        return (stream == CommandletStream::Output ? captured.out : captured.error).TryAppend(text);
    }

    CommandletOutput Output() noexcept {
        return CommandletOutput{&Captured::Write, this};
    }

    String out;
    String error;
    // 참이면 failStream 쓰기가 Busy로 실패한다(출력 실패 시험).
    bool failWrites = false;
    CommandletStream failStream = CommandletStream::Output;
};

// 시험 임포터 등록 상태다(failWith가 None이 아니면 등록이 그 오류로 실패한다).
struct ImporterState {
    import_test::TestImporter test{import_test::TestImporterInfo()};
    import_test::TestImporter cube{import_test::CubeImporterInfo()};
    ErrorCode failWith = ErrorCode::None;
};

inline Result<void> RegisterTestImporters(void* state, AssetImporterTable& table,
                                          Array<AssetImporterRegistration>& tokens) noexcept {
    ImporterState& importers = *static_cast<ImporterState*>(state);
    if (importers.failWith != ErrorCode::None) {
        return Result<void>::Failure(importers.failWith);
    }

    import_test::TestImporter* const all[] = {&importers.test, &importers.cube};
    for (import_test::TestImporter* importer : all) {
        auto registered = table.Register(*importer);
        if (!registered) {
            return Result<void>::Failure(registered.Error());
        }

        if (auto pushed = tokens.TryPushBack(rocks::Move(registered.Value())); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// 디스크 프로젝트(작업 디렉터리 아래 상대 이름 — `*.rocksproject`)와 원본 파일이다.
struct CommandScene {
    explicit CommandScene(const char* tag) noexcept : root(tag) {
        ready = root.made && WriteFile("Scene.rocksproject", "{}\n");
    }

    bool WriteFile(StringView relative, StringView text) noexcept {
        char path[1024] = {};
        FixedStringBuilder builder{Span<char>(path)};
        if (!builder.TryAppendFormat("{}/{}", root.Root(), relative)) {
            return false;
        }

        platform::File file;
        return static_cast<bool>(file.Open(platform::PhysicalFileSystem(), builder.View().Data(),
                                           builder.View().ByteCount(),
                                           platform::FileOpenMode::CreateAlways)) &&
               static_cast<bool>(file.WriteAll(text.Data(), text.ByteCount()).status) &&
               static_cast<bool>(file.Close());
    }

    bool MakeDirectory(StringView relative) noexcept {
        char path[1024] = {};
        FixedStringBuilder builder{Span<char>(path)};
        return builder.TryAppendFormat("{}/{}", root.Root(), relative) &&
               static_cast<bool>(platform::PhysicalFileSystem().MakeDirectory(
                   builder.View().Data(), builder.View().ByteCount()));
    }

    bool CopyFile(StringView from, StringView to) noexcept {
        char source[1024] = {};
        char target[1024] = {};
        FixedStringBuilder sourcePath{Span<char>(source)};
        FixedStringBuilder targetPath{Span<char>(target)};
        return sourcePath.TryAppendFormat("{}/{}", root.Root(), from) &&
               targetPath.TryAppendFormat("{}/{}", root.Root(), to) &&
               static_cast<bool>(platform::PhysicalFileSystem().Copy(
                   sourcePath.View().Data(), sourcePath.View().ByteCount(),
                   targetPath.View().Data(), targetPath.View().ByteCount(), false));
    }

    bool RemoveDirectory(StringView relative) noexcept {
        char path[1024] = {};
        FixedStringBuilder builder{Span<char>(path)};
        return builder.TryAppendFormat("{}/{}", root.Root(), relative) &&
               static_cast<bool>(platform::PhysicalFileSystem().DeleteDirectory(
                   builder.View().Data(), builder.View().ByteCount()));
    }

    bool RemoveFile(StringView relative) noexcept {
        char path[1024] = {};
        FixedStringBuilder builder{Span<char>(path)};
        return builder.TryAppendFormat("{}/{}", root.Root(), relative) &&
               static_cast<bool>(platform::PhysicalFileSystem().Delete(builder.View().Data(),
                                                                       builder.View().ByteCount()));
    }

    bool Exists(StringView relative) noexcept {
        char path[1024] = {};
        FixedStringBuilder builder{Span<char>(path)};
        platform::File file;
        return builder.TryAppendFormat("{}/{}", root.Root(), relative) &&
               static_cast<bool>(file.Open(platform::PhysicalFileSystem(), builder.View().Data(),
                                           builder.View().ByteCount(),
                                           platform::FileOpenMode::ReadExisting));
    }

    // 디렉터리가 있는지다(Exists는 파일만 연다).
    bool DirectoryExists(StringView relative) noexcept {
        char path[1024] = {};
        FixedStringBuilder builder{Span<char>(path)};
        bool exists = false;
        return builder.TryAppendFormat("{}/{}", root.Root(), relative) &&
               static_cast<bool>(platform::PhysicalFileSystem().DirectoryExists(
                   builder.View().Data(), builder.View().ByteCount(), exists)) &&
               exists;
    }

    asset_database_test::TempProject root;
    bool ready = false;
};

} // namespace rocks::commandlet_test
