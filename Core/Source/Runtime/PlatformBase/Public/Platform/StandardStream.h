#pragma once

#include <Platform/File.h>

// 이 프로세스의 표준 스트림이다(Docs/Design/SparksTools.ko.md T2 — SparksEngineIntegration SE-B7).
// CLI 도구의 출력과 언어 서버의 stdio 통신이 쓴다. CRT stdio를 거치지 않는다(text 모드 변환이 없다
// — pipe·파일은 byte 그대로).
//
// 사용 예:
//   const char line[] = "ok\n";
//   if (!WriteStandardStream(StandardStream::Output, line, sizeof(line) - 1).status) { ... }
//   char buffer[4096];
//   const FileTransfer read = ReadStandardInput(buffer, sizeof(buffer));
//   // read.status.code == FileCode::EndOfFile면 입력이 끝났다.
//
// 스레드: 같은 스트림을 여러 스레드가 동시에 쓰면 조각이 섞일 수 있다(호출자가 직렬화한다).
namespace rocks::platform {

enum class StandardStream : uint8 {
    Output,
    Error
};

// bytes(UTF-8)를 stream에 모두 쓴다. 스트림이 콘솔이면 Windows는 UTF-16으로 바꿔 쓰고(글자 경계로
// 나눈다), pipe·파일이면 byte 그대로다. 결과 bytes는 이미 쓴 byte 수다. 실패: 스트림이 없으면
// NotOpen, 콘솔에 UTF-8이 아닌 byte열이면 InvalidArgument, 쓰기 실패는 그 OS 오류(받는 쪽이 닫힌
// pipe는 EndOfFile).
ROCKS_PLATFORMBASE_API FileTransfer WriteStandardStream(StandardStream stream, const char* bytes,
                                                        usize count) noexcept;

// 표준 입력에서 최대 capacity byte를 읽는다. 데이터가 올 때까지 기다리며 데이터가 있으면 Success와
// 1 byte 이상이다. 입력이 끝났으면(쓰는 쪽이 닫힘) EndOfFile과 0이다. 실패: 표준 입력이 없으면
// NotOpen, capacity가 0이거나 buffer가 null이면 InvalidArgument, 그 밖은 OS 오류.
ROCKS_PLATFORMBASE_API FileTransfer ReadStandardInput(void* buffer, usize capacity) noexcept;

} // namespace rocks::platform
