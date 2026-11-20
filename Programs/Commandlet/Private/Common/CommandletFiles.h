#pragma once

#include <Core/Commandlet/Commandlet.h>
#include <Core/Containers/Array.h>
#include <Platform/File.h>

// 명령들이 함께 쓰는 파일 도우미다(log·content-sign). 실패는 "<command>: cannot <action> '<path>'
// (file code N, native error M)" 한 줄을 표준 오류에 쓰고 종료 코드 1로 돌려준다(출력 실패는 그
// 오류).
namespace rocks::commandlet_host {

// 인수 view는 NUL 종료를 보장하지 않으므로 경로를 복사한다(인수 상한까지).
struct PathText {
    explicit PathText(StringView path) noexcept;

    char text[CommandletRegistry::MaxArgumentBytes + 1];
};

Result<int32> ReportFileFailure(CommandletOutput& output, StringView command, const char* action,
                                StringView path, platform::FileStatus status) noexcept;

// 파일 전체를 읽는다. 실패하면 오류를 출력하고 그 종료 코드를 failure에 둔다(할당 실패는
// OutOfMemory 오류).
bool ReadCommandFile(CommandletOutput& output, StringView command, StringView path,
                     Array<uint8>& bytes, Result<int32>& failure) noexcept;

// 새 파일을 만든다(있으면 덮어쓰지 않는다 — 키를 실수로 바꾸지 않게). 실패하면 오류를 출력하고 그
// 종료 코드를 failure에 둔다.
bool CreateCommandFile(CommandletOutput& output, StringView command, const PathText& path,
                       StringView shown, platform::File& file, Result<int32>& failure) noexcept;

// 열어 둔 파일에 bytes를 다 쓰고 닫는다.
bool WriteCommandFileAndClose(CommandletOutput& output, StringView command, StringView shown,
                              platform::File& file, Span<const uint8> bytes,
                              Result<int32>& failure) noexcept;

// 새 파일을 만들어 bytes를 쓴다(있으면 덮어쓰지 않는다). 성공은 0이다.
Result<int32> WriteNewCommandFile(CommandletOutput& output, StringView command, StringView path,
                                  Span<const uint8> bytes) noexcept;

} // namespace rocks::commandlet_host
