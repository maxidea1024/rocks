#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>

// 비밀(키, 키 확장 상태, 평문 비밀)을 담는 메모리다(Docs/Design/Crypto.ko.md 10.1·10.2절). 저장소는
// 둘이다.
//
// - 키 저장소(SecretArena): 키·키 상태 전용이다. 앞뒤에 commit하지 않은 guard page를 둔 64 KiB
//   arena를 필요할 때 덧붙이고(최대 64개), 잠금과 덤프 제외는 libsodium sodium_mlock(VirtualLock,
//   또는 MADV_DONTDUMP와 mlock)에 맡긴다. 잠금 한도에 걸린 arena는 잠그지 않은 채 쓰고 상태로
//   보고한다. 버퍼는 이 저장소를 쓰지 않으므로 큰 평문 버퍼가 키 자리를 차지하지 않는다. 4 KiB
//   이하의 SecretBytes와 키 타입이 여기에 있다.
// - 버퍼 할당기(GetSecretAllocator): 평문이 지나가는 버퍼용 Allocator다. 일반 힙에서 할당하고
//   해제할 때 블록 전체를 지운다. 잠금·덤프 제외는 없다. 4 KiB를 넘는 SecretBytes도 여기에 있다.
//
// 한계: 프로세스 메모리를 직접 읽는 공격자, 레지스터·라이브러리 스택 잔재, 하이버네이트 파일은 막지
// 못한다. 이 장치는 스왑·덤프·해제 뒤 잔존 데이터를 줄이는 것이 목적이다. guard page는 arena 양
// 끝의 넘침만 잡고, 같은 arena 안의 슬롯끼리 넘치는 것은 잡지 못한다(ASan 구성은 슬롯 꼬리를
// poison해 잡는다). macOS에는 덤프 제외가 없고, Windows minidump 제외는 crash reporter 몫이다.
//
// 사용 예:
//   auto key = crypto::SecretBytes::TryGenerate(32);   // OS 난수 32 byte, 키 저장소
//   if (!key) { return key.Error(); }
//   UseKey(key.Value().View());                         // view 수명은 key 객체와 같다
//
//   // 성장 재할당·해제 때 버려지는 이전 버퍼를 지운다.
//   Array<uint8> plaintext(crypto::GetSecretAllocator());
namespace rocks::crypto {

// 비밀 메모리의 한 시점 상태다. 시험·진단용이며 값은 다른 스레드의 할당으로 곧 바뀔 수 있다.
struct SecretMemoryStatus {
    // 키 저장소의 arena 수와 전체 byte, 그중 페이지 잠금(스왑 방지)에 성공한 byte다. 잠금 한도가
    // 작은 기기에서는 일부 arena만 잠긴다.
    usize arenaCount = 0;
    usize arenaCapacityBytes = 0;
    usize arenaLockedBytes = 0;
    // 지금 쓰는 키 저장소 byte(슬롯 크기 등급 기준)다.
    usize arenaUsedBytes = 0;
    // arena를 코어 덤프에서 빼 달라고 OS에 요청했는가(Android MADV_DONTDUMP). libsodium은 결과를
    // 확인하지 않으므로 "요청함"까지만 뜻한다. macOS·Windows는 false다.
    bool dumpExclusionRequested = false;
    // 버퍼 할당기의 살아 있는 할당 수와 요청 byte 합이다.
    usize bufferAllocations = 0;
    usize bufferBytes = 0;
};

// 여러 스레드에서 불러도 된다. arena를 새로 만들지는 않는다.
[[nodiscard]] SecretMemoryStatus GetSecretMemoryStatus() noexcept;

// 해제할 때 내용을 지우는 프로세스 전역 버퍼 Allocator다.
//
// - 여러 스레드에서 동시에 써도 된다. 프로세스가 끝날 때까지 파괴되지 않으므로 정적 객체의
//   소멸자에서 해제해도 된다.
// - Allocate: alignment는 0이 아닌 2의 거듭제곱이다(아니면 InvalidArgument). 돌려준 주소는 최소 64
//   byte 정렬이다. 0 byte는 성공한 nullptr다. 크기 계산이 넘치면 Overflow, 힙이 모자라면
//   OutOfMemory다. 내용은 정해지지 않는다.
// - Deallocate: 요청 크기 전체를 지우고 돌려준다. 블록 크기는 블록 밖 표에 있으므로, 이 Allocator가
//   준 살아 있는 주소가 아니면 주소를 역참조하지 않고 NotFound다. nullptr는 아무 일도 하지 않는
//   성공이다.
[[nodiscard]] Allocator& GetSecretAllocator() noexcept;

// 비밀 byte를 소유하는 move-only 값이다. 복사할 수 없고, 이동하면 원본은 비며, 소멸·Reset 때
// 지운다. 4 KiB 이하는 키 저장소 슬롯(64 byte 정렬, guard page·잠금 시도), 그보다 크면 버퍼
// 할당기(소거만)에 둔다. 키는 모두 4 KiB 이하다(가장 큰 ML-KEM-768 비밀키가 2400 byte).
class SecretBytes {
public:
    SecretBytes() noexcept = default;
    ~SecretBytes() noexcept;

    SecretBytes(const SecretBytes&) = delete;
    SecretBytes& operator=(const SecretBytes&) = delete;

    // 이동하면 원본은 빈 값이 된다. 저장소를 넘겨받으므로 사본이 남지 않는다. 자기 자신으로의 이동
    // 대입은 아무 일도 하지 않는다.
    SecretBytes(SecretBytes&& other) noexcept;
    SecretBytes& operator=(SecretBytes&& other) noexcept;

    // bytes 크기의 0으로 채운 비밀이다. 0 byte는 빈 값이다. 키 저장소가 한도까지 찼거나 힙이
    // 모자라면 OutOfMemory, 크기 계산이 넘치면 Overflow다.
    [[nodiscard]] static Result<SecretBytes> TryCreate(usize bytes) noexcept;

    // OS 난수로 채운 비밀이다. TryCreate의 오류에 더해 OS 난수가 실패하면 DependencyFailed다(만든
    // 저장소는 지운다).
    [[nodiscard]] static Result<SecretBytes> TryGenerate(usize bytes) noexcept;

    // source를 복사한 비밀이다. 오류는 TryCreate와 같다. 일반 메모리에 있던 source는 호출자가
    // 지운다(SecureZero). 다른 SecretBytes의 view를 넘겨 사본을 만들어도 된다.
    [[nodiscard]] static Result<SecretBytes> TryCopyFrom(Span<const uint8> source) noexcept;

    // 비밀 내용의 view다. 수명은 이 객체와 같다(이동·Reset·소멸 뒤에는 무효).
    [[nodiscard]] Span<const uint8> View() const noexcept {
        return Span<const uint8>(data_, size_);
    }

    // 쓰기 view다. 생성 직후 내용을 채우는 경로(키 파생·가져오기)에서만 쓴다. 수명은 View()와 같다.
    [[nodiscard]] Span<uint8> MutableView() noexcept {
        return Span<uint8>(data_, size_);
    }

    [[nodiscard]] usize Size() const noexcept {
        return size_;
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return size_ == 0;
    }

    // 내용을 지우고 저장소를 돌려준다. 빈 값이 된다.
    void Reset() noexcept;

private:
    SecretBytes(uint8* data, usize size) noexcept : data_(data), size_(size) {
    }

    uint8* data_ = nullptr;
    usize size_ = 0;
};

} // namespace rocks::crypto
