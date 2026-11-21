#pragma once

#include <AssetDatabase/ProjectLayout.h>
#include <AssetRuntime/PackageReader.h>
#include <Core/Commandlet/Commandlet.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/StringBuilder.h>

// 에셋 명령(asset-*)의 공용 도구다(Docs/Design/AssetSystem.ko.md 13절): 출력 쓰기, 실패 줄,
// 프로젝트 찾기·열기, 색인 읽기 옵션.
namespace rocks::commandlet_host {

// 출력 한 줄의 인라인 byte 수다(넘으면 allocator로 자란다).
constexpr usize LineBytes = 512;

inline Result<int32> Write(CommandletOutput& output, CommandletStream stream, StringView text,
                           int32 code) noexcept {
    auto written = output.write(output.context, stream, text);
    return written ? Result<int32>::Success(code) : Result<int32>::Failure(written.Error());
}

// "<command>: <서식>\n"을 표준 오류에 쓰고 code로 돌려준다.
template <class... TArgs>
Result<int32> Fail(CommandletOutput& output, Allocator& allocator, StringView command, int32 code,
                   StringView format, const TArgs&... args) noexcept {
    StringBuilder<LineBytes> text(allocator);
    if (auto formatted = text.TryAppendFormat("{}: ", command); !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    if (auto formatted = text.TryAppendFormat(format, args...); !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    if (auto formatted = text.TryAppend("\n"); !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    return Write(output, CommandletStream::Error, text.View(), code);
}

// 옵션의 프로젝트(없으면 현재 디렉터리에서 위로 찾은 것)로 layout을 연다. 실패하면 오류를 출력하고
// 그 종료 코드를 failure에 둔다.
bool OpenProject(CommandletOutput& output, Allocator& allocator, StringView command,
                 StringView project, ProjectLayout& layout, Result<int32>& failure) noexcept;

// 에디터 패키지 색인·열기의 읽기 옵션이다(에디터 종류, 이 엔진의 버전 — 더 새 엔진의 패키지는
// NewerVersion).
PackageReadOptions IndexReadOptions() noexcept;

} // namespace rocks::commandlet_host
