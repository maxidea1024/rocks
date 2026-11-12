#pragma once

#include <Core/Error.h>
#include <Platform/File.h>

// 파일 API의 상태(platform::FileStatus)를 Core 오류 코드로 바꾸는 공용 대응이다. 파일 IO 결과를
// Result로 올리는 모든 모듈(에셋 파일, 비동기 IO 이벤트 등)이 같은 표를 쓴다.
//
// 대응: 성공 None, 인자·대상 종류 오류·파일 끝 InvalidArgument, 이미 열림·사용 중 Busy, 없음
// NotFound, 이미 있음·비어 있지 않은 디렉터리 AlreadyExists, 메모리 부족 OutOfMemory, overflow
// Overflow, 미지원 Unsupported, 취소 Cancelled, 그 밖의 IO 실패(진행 없음·열리지 않음·접근
// 거부·시스템 오류·내구성 미확인) DependencyFailed.
//
// 사용 예:
//   auto status = file.Open(fileSystem, path, pathBytes, platform::FileOpenMode::Read);
//   if (!status) {
//       return Result<void>::Failure(FileErrorCode(status));
//   }
namespace rocks {

ErrorCode FileErrorCode(platform::FileStatus status) noexcept;

} // namespace rocks
