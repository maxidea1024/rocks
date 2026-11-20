#pragma once

#include <Core/Error.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Platform/AsyncFile.h>

// 로더(AssetLoader)가 쿠킹 패키지 파일을 읽는 IO 경계다(Docs/Design/AssetSystem.ko.md 11.6절 4).
// 로더는 파일 하나를 통째로 읽는 요청(CookedPackageRead)을 Begin으로 넘기고, 구현은 끝나면 아무
// 스레드에서 read.complete를 정확히 한 번 부른다. 로더는 그 결과를 Game 스레드에서 처리한 뒤
// Release로 구현의 요청별 자원을 돌려준다. 기본 구현은 PlatformBase AsyncReadFile을 쓰는
// AsyncCookedPackageIo이고, 시험은 완료 순서·오류를 명시 gate로 정하는 구현을 끼운다(UE 5.8
// IoDispatcher backend 대응).
namespace rocks {

// 요청 하나의 결과다. error가 None이면 파일 크기가 fileBytes이고, fileBytes가 요청의
// expectedBytes와 같을 때만 destination에 파일 전체가 있다(다르면 읽지 않아도 된다).
struct CookedPackageReadResult {
    ErrorCode error = ErrorCode::None;
    uint64 fileBytes = 0;
};

// 파일 하나를 읽는 요청이다. 로더가 채우고 완료(complete 호출)와 Release까지 살려 둔다.
struct CookedPackageRead {
    // NUL로 끝나는 UTF-8 경로와 그 byte 수(NUL 제외)다.
    const char* path = nullptr;
    usize pathBytes = 0;
    // 매니페스트 packageBytes다. 파일 크기가 이 값과 같으면 destination[0, expectedBytes)에 읽는다.
    uint64 expectedBytes = 0;
    uint8* destination = nullptr;
    // 완료 통지다. 아무 스레드에서 요청마다 정확히 한 번 부른다(Begin이 성공한 요청만). Begin이
    // 반환하기 전에 불러도 된다. 할당하지 않고 막히지 않아야 한다(로더는 큐에 넣기만 한다).
    void (*complete)(CookedPackageRead& read,
                     const CookedPackageReadResult& result) noexcept = nullptr;
    void* owner = nullptr;
    // 구현이 요청마다 쓰는 자리다(로더는 만지지 않는다).
    void* backendState = nullptr;
};

// IO 구현이다. Begin·Release는 Game 스레드에서 불리고, complete는 구현이 고른 스레드에서 불린다.
class CookedPackageIo {
public:
    // 읽기를 시작한다. 즉시 실패(경로를 받을 수 없음·자원 부족 등)는 그 오류를 돌려주며 complete를
    // 부르지 않는다. 성공하면 complete를 정확히 한 번 부른다(파일 없음·IO 오류도 결과로).
    virtual Result<void> Begin(CookedPackageRead& read) noexcept = 0;

    // complete가 불린 요청의 자원을 돌려준다(Begin이 성공한 요청마다 한 번, complete 뒤에만).
    virtual void Release(CookedPackageRead& read) noexcept = 0;

protected:
    ~CookedPackageIo() = default;
};

// PlatformBase AsyncReadFile로 읽는 기본 구현이다: 열기 → 크기 → 크기가 expectedBytes와 같으면
// 전체를 한 요청으로 읽는다(다르면 읽지 않고 그 크기를 결과로). 열기 실패는 FileErrorCode 대응
// 오류다(없음은 NotFound). 짧게 읽힘(파일이 그 사이 줄어듦)은 InvalidArgument다.
//
// 사용 예:
//   platform::AsyncIoContext io;
//   io.Start(platform::DefaultAsyncIoBackend());
//   AsyncCookedPackageIo files(allocator, io);
//   config.io = &files;
//
// 수명: io context와 allocator는 이 객체보다 오래 살아야 한다. 진행 중 요청이 남은 채로 파괴하면 안
// 된다(로더 Shutdown이 모두 끝낸다). allocator는 다른 스레드의 해제를 지원해야 한다.
class AsyncCookedPackageIo final : public CookedPackageIo {
public:
    // 한 번에 제출하는 읽기 조각의 기본 크기다(AsyncReadFile 한 요청의 상한 4 GiB − 1 아래 — 64 GiB
    // 패키지도 조각으로 읽는다).
    static constexpr uint64 DefaultChunkBytes = uint64{1} << 30;

    // chunkBytes는 조각 크기다(0이면 1 — 시험이 작은 조각으로 조각 경로를 본다).
    AsyncCookedPackageIo(Allocator& allocator, platform::AsyncIoContext& io,
                         uint64 chunkBytes = DefaultChunkBytes) noexcept;

    Result<void> Begin(CookedPackageRead& read) noexcept override;
    void Release(CookedPackageRead& read) noexcept override;

private:
    Allocator& allocator_;
    platform::AsyncIoContext& io_;
    uint64 chunkBytes_;
};

} // namespace rocks
