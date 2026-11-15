#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Delegate.h>
#include <Core/Hash.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Crypto/KeyBlob.h>
#include <Crypto/KeyId.h>
#include <Crypto/Secret.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/Sync.h>

// 키 ID → 키 표(KeyRing)와 없는 키를 가져오는 공급자다(Docs/Design/Crypto.ko.md 2·10.3절). 형식은
// 키 ID를 기록하고(KeyBlob·세그먼트 헤더), 복호는 그 ID로 키를 찾는다. 시도 복호를 하지 않는다.
//
// - 키 회전: 암호화는 현재 키(TrySetCurrent·TryAcquireCurrent), 복호는 등록된 키 전체(ID로)다. 새
//   키를 TryAdd한 뒤 TrySetCurrent로 바꾸고, 옛 키는 옛 데이터를 다 읽을 때까지 남겨 둔다.
// - 타입: KeyRing은 키 타입별 템플릿이다. 한 ID가 두 알고리즘·용도에 쓰이는 일을 타입으로 막는다
//   (KEK는 KeyRing<KdfKey>, 세그먼트 스트림은 KeyRing<SegmentStreamKey>).
// - lease: TryAcquire는 키를 빌려 주는 KeyLease를 돌려준다. 항목은 등록 뒤 바뀌지 않고, 제거한
//   키라도 이미 받은 lease는 끝날 때까지 유효하다. 마지막 lease가 끝나면 키를 지운다(키 소멸자).
//
// 사용 예(회전과 복호):
//   crypto::KeyRing<crypto::SegmentStreamKey> ring;
//   auto key = crypto::SegmentStreamKey::TryGenerate(id);
//   ring.TryAdd(id, Move(key.Value()));
//   ring.TrySetCurrent(id);
//   auto sealing = ring.TryAcquireCurrent();                 // 암호화
//   auto info = crypto::TryPeekSegmentHeader(header);
//   auto opening = ring.TryAcquire(info.Value().keyId);      // 복호(없으면 공급자)
//   if (opening) { Use(opening.Value().Key()); }
namespace rocks::crypto {

template <typename TKey>
class KeyRing;

namespace detail {

// KeyRing 항목이다. 등록 뒤 바뀌지 않는다(SharedPtr로 lease와 공유한다).
template <typename TKey>
struct KeyRingEntry {
    KeyRingEntry(KeyId entryId, TKey&& entryKey) noexcept : id(entryId), key(Move(entryKey)) {
    }

    KeyId id;
    TKey key;
};

// 키가 자기 ID를 품는 타입(SegmentStreamKey)인가.
template <typename TKey>
concept KeyCarriesId = requires(const TKey& key) { key.Id(); };

} // namespace detail

// KeyRing에서 빌린 키다. 키 객체를 공유 소유하므로 KeyRing이 키를 제거하거나 KeyRing이 먼저
// 소멸해도 lease가 끝날 때까지 유효하다. 마지막 lease(와 KeyRing의 항목)가 사라지는 스레드에서 키를
// 지운다.
//
// - 복사하면 같은 키를 함께 빌린다(키 byte를 복사하지 않는다). 서로 다른 lease 객체는 여러
// 스레드에서
//   동시에 복사·소멸해도 된다. 같은 lease 객체를 동시에 바꾸지 않는다.
// - Key()는 const 참조다. 키 객체는 생성 뒤 바뀌지 않으므로 여러 스레드가 함께 써도 된다(11절).
// - 빈 lease(기본 생성·Reset·이동된 뒤)의 Key()는 Fatal이다. IsEmpty로 먼저 확인한다.
// - KeyRing을 만든 할당기(기본 GetSecretAllocator)는 마지막 lease보다 오래 살아야 한다.
template <typename TKey>
class KeyLease {
public:
    KeyLease() noexcept = default;

    [[nodiscard]] bool IsEmpty() const noexcept {
        return !entry_;
    }

    // 키 ID다. 빈 lease는 Nil이다.
    [[nodiscard]] KeyId Id() const noexcept {
        return entry_ ? entry_->id : KeyId{};
    }

    // 빌린 키다. 수명은 이 lease(또는 그 복사본)와 같다.
    [[nodiscard]] const TKey& Key() const noexcept {
        return entry_->key;
    }

    // 빌린 것을 놓는다. 마지막 lease였고 KeyRing에서도 제거됐으면 여기서 키를 지운다.
    void Reset() noexcept {
        entry_.Reset();
    }

private:
    friend class KeyRing<TKey>;

    explicit KeyLease(const SharedPtr<detail::KeyRingEntry<TKey>>& entry) noexcept : entry_(entry) {
    }

    SharedPtr<detail::KeyRingEntry<TKey>> entry_;
};

// KeyId → 키 표다. TKey는 move-only 키 타입(AeadKey·MacKey·KdfKey·SecretStreamKey·
// SegmentStreamKey 등)이다.
//
// 스레드: 모든 함수는 여러 스레드가 동시에 불러도 된다. 표는 잠금 하나로 지키고, 잠근 구간은 조회·
// 삽입·lease 복사뿐이다(키 연산·공급자 호출은 잠금 밖이다).
//
// 공급자(SetProvider): 표에 없는 ID를 TryAcquire하면 공급자를 부른다(서버, OS 키 저장소, 빌드 시
// 주입 표; KeyBlobProvider는 KeyBlob 표 구현이다).
// - 같은 ID의 동시 요청은 공급자를 한 번만 부르고 그 결과(성공·실패)를 함께 받는다(single-flight).
// - Delegate는 동시 실행을 허용하지 않으므로 이 KeyRing의 공급자 호출은 한 번에 하나다(다른 ID도
//   차례로 부른다). 표 잠금 밖에서 부르므로 공급자 안에서 다른 KeyRing(예: KEK의
//   KeyRing<KdfKey>)이나 이 KeyRing의 등록된 키를 TryAcquire해도 된다. 이 KeyRing의 없는 키를
//   공급자 안에서 TryAcquire하면 기다릴 수 없으므로 Busy다. 공급자 안에서 SetProvider를 부르면
//   Fatal이다. 두 KeyRing의 공급자가 서로의 없는 키를 요청하는 순환은 만들지 않는다(KEK 계층처럼
//   한 방향으로만 잇는다). Busy 감지는 같은 스레드만 보므로, 공급자가 다른 스레드에 일을 넘기고
//   기다릴 때 그 일이 이 KeyRing의 없는 키를 요청하면 교착한다. 그런 일을 만들지 않는다.
// - 공급자가 돌려준 키를 KeyRing이 요청한 ID로 등록한다. 공급자가 오래 걸리면(네트워크) 그동안 이
//   KeyRing의 다른 없는 키 요청도 기다린다. 실패는 기억하지 않는다(다음 요청이 다시 부른다).
//
// 수명: KeyRing보다 lease가 오래 살아도 된다(위 KeyLease). 다른 스레드가 이 KeyRing의 함수를 쓰는
// 동안 소멸시키지 않는다. 공급자 호출이 진행 중인데 소멸하면 Fatal이다(결함을 조용히 넘기지
// 않는다). 할당기는 여러 스레드에서 동시에 써도 되는 것이어야
// 한다(System·Binned·GetSecretAllocator).
template <typename TKey>
class KeyRing {
public:
    // 공급자다. 요청한 ID의 키를 돌려준다. 키가 없으면 NotFound, 그 밖의 실패(서버 오류
    // DependencyFailed, blob 변조 AuthenticationFailed 등)는 그 오류를 돌려준다.
    using Provider = Delegate<Result<TKey>(KeyId)>;

    // allocator는 항목·lease 공유 블록·표에 쓴다(키 byte는 키 저장소에 있다). 기본값은 프로세스
    // 끝까지 사는 GetSecretAllocator()다.
    explicit KeyRing(Allocator& allocator = GetSecretAllocator()) noexcept
        : allocator_(allocator), entries_(allocator), flights_(allocator) {
    }

    ~KeyRing() noexcept {
        platform::LockGuard lock(mutex_);
        if (flights_.Count() != 0) {
            platform::Fatal("KeyRing destroyed while a provider fetch is in flight");
        }
    }

    KeyRing(const KeyRing&) = delete;
    KeyRing& operator=(const KeyRing&) = delete;

    // key를 id로 등록한다. 성공하면 key를 소비하고, 실패하면 key는 호출자에게 그대로 남는다.
    // 빈 key는 StaleHandle, Nil id와 Id()를 품는 키(SegmentStreamKey)의 Id()가 id와 다르면
    // InvalidArgument, 이미 있으면 AlreadyExists(기존 항목은 바뀌지 않는다), 할당 실패는
    // OutOfMemory다.
    [[nodiscard]] Result<void> TryAdd(KeyId id, TKey&& key) noexcept {
        if (key.IsEmpty()) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        if (id.IsNil() || !CarriesId(key, id)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 표를 잠그기 전에 항목을 만든다. 실패하면 key를 되돌린다.
        auto entry = SharedPtr<Entry>::TryMake(allocator_, id, Move(key));
        if (!entry) {
            return Result<void>::Failure(entry.Error());
        }

        ErrorCode error = ErrorCode::None;
        {
            platform::LockGuard lock(mutex_);
            auto inserted = entries_.Insert(id.value, entry.Value());
            if (!inserted) {
                error = inserted.Error();
            } else if (inserted.Value() == InsertOutcome::AlreadyPresent) {
                error = ErrorCode::AlreadyExists;
            }
        }

        if (error != ErrorCode::None) {
            key = Move(entry.Value()->key);
            return Result<void>::Failure(error);
        }

        return Result<void>::Success();
    }

    // id의 키를 빌린다. 표에 있으면 그 항목, 없으면 공급자를 불러 등록한 뒤 빌린다.
    // - Nil id는 InvalidArgument다.
    // - 공급자가 없거나 공급자가 NotFound면 NotFound다. 공급자의 다른 오류는 그대로 돌려준다.
    // - 공급자가 빈 키를 돌려주면 StaleHandle, Id()를 품는 키의 Id()가 id와 다르면
    //   AuthenticationFailed(저장소에서 blob이 통째로 바뀐 경우)다.
    // - 이 KeyRing의 공급자 안에서 없는 키를 요청하면 Busy, 할당 실패는 OutOfMemory다.
    [[nodiscard]] Result<KeyLease<TKey>> TryAcquire(KeyId id) noexcept {
        if (id.IsNil()) {
            return Result<KeyLease<TKey>>::Failure(ErrorCode::InvalidArgument);
        }

        SharedPtr<Flight> flight;
        bool leader = false;
        {
            platform::LockGuard lock(mutex_);
            if (const SharedPtr<Entry>* found = entries_.Find(id.value)) {
                return Result<KeyLease<TKey>>::Success(KeyLease<TKey>(*found));
            }

            if (!hasProvider_) {
                return Result<KeyLease<TKey>>::Failure(ErrorCode::NotFound);
            }

            if (platform::AtomicLoad(&providerThread_) == platform::CurrentThreadId()) {
                return Result<KeyLease<TKey>>::Failure(ErrorCode::Busy);
            }

            if (const SharedPtr<Flight>* joined = flights_.Find(id.value)) {
                flight = *joined;
            } else {
                auto created = SharedPtr<Flight>::TryMake(allocator_);
                if (!created) {
                    return Result<KeyLease<TKey>>::Failure(created.Error());
                }

                auto inserted = flights_.Insert(id.value, created.Value());
                if (!inserted) {
                    return Result<KeyLease<TKey>>::Failure(inserted.Error());
                }

                flight = Move(created.Value());
                leader = true;
            }
        }

        if (!leader) {
            return WaitFlight(*flight);
        }

        return Fetch(id, *flight);
    }

    // 현재 키(TrySetCurrent)를 빌린다. 현재 키가 없으면 NotFound다(공급자를 부르지 않는다).
    [[nodiscard]] Result<KeyLease<TKey>> TryAcquireCurrent() noexcept {
        platform::LockGuard lock(mutex_);
        if (current_.IsNil()) {
            return Result<KeyLease<TKey>>::Failure(ErrorCode::NotFound);
        }

        const SharedPtr<Entry>* found = entries_.Find(current_.value);
        if (found == nullptr) {
            return Result<KeyLease<TKey>>::Failure(ErrorCode::NotFound);
        }

        return Result<KeyLease<TKey>>::Success(KeyLease<TKey>(*found));
    }

    // id를 표에서 뺀다. 이미 받은 lease는 끝날 때까지 유효하고, 마지막 lease가 끝나면 키를 지운다.
    // 현재 키였으면 현재 키가 없어진다. Nil id는 InvalidArgument, 없으면 NotFound다.
    [[nodiscard]] Result<void> TryRemove(KeyId id) noexcept {
        if (id.IsNil()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 항목은 잠금 밖에서 놓는다(마지막 참조면 키 소멸자가 키 저장소를 잠근다).
        SharedPtr<Entry> removed;
        {
            platform::LockGuard lock(mutex_);
            SharedPtr<Entry>* found = entries_.Find(id.value);
            if (found == nullptr) {
                return Result<void>::Failure(ErrorCode::NotFound);
            }

            removed = Move(*found);
            entries_.Erase(id.value);
            if (current_ == id) {
                current_ = KeyId{};
            }
        }

        return Result<void>::Success();
    }

    // 암호화에 쓸 현재 키를 정한다. 등록된 키만 받는다(공급자를 부르지 않는다). Nil id는
    // InvalidArgument, 없으면 NotFound다.
    [[nodiscard]] Result<void> TrySetCurrent(KeyId id) noexcept {
        if (id.IsNil()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        platform::LockGuard lock(mutex_);
        if (!entries_.Contains(id.value)) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        current_ = id;
        return Result<void>::Success();
    }

    // 현재 키를 없앤다(TryAcquireCurrent가 NotFound가 된다).
    void ClearCurrent() noexcept {
        platform::LockGuard lock(mutex_);
        current_ = KeyId{};
    }

    // 현재 키 ID다. 없으면 Nil이다. 다른 스레드가 바꿀 수 있으므로 한 시점의 값이다.
    [[nodiscard]] KeyId CurrentId() const noexcept {
        platform::LockGuard lock(mutex_);
        return current_;
    }

    // 등록된 키 수다(한 시점의 값).
    [[nodiscard]] usize Count() const noexcept {
        platform::LockGuard lock(mutex_);
        return entries_.Count();
    }

    // 공급자를 바꾼다. 빈 delegate는 공급자를 없앤다. 진행 중인 공급자 호출이 끝난 뒤에 바뀐다.
    // 이 KeyRing의 공급자 안에서 부르면 Fatal이다.
    void SetProvider(Provider&& provider) noexcept {
        if (platform::AtomicLoad(&providerThread_) == platform::CurrentThreadId()) {
            platform::Fatal("KeyRing::SetProvider called from its own provider");
        }

        Provider previous;
        {
            platform::LockGuard gate(providerGate_);
            previous = Move(provider_);
            provider_ = Move(provider);
            platform::LockGuard lock(mutex_);
            hasProvider_ = provider_.IsBound();
        }
    }

private:
    using Entry = detail::KeyRingEntry<TKey>;

    // 같은 ID의 공급자 호출 하나다. 기다리는 쪽은 done이 될 때까지 changed_를 기다린다.
    struct Flight {
        SharedPtr<Entry> entry;
        ErrorCode error = ErrorCode::None;
        bool done = false;
    };

    static bool CarriesId(const TKey& key, KeyId id) noexcept {
        if constexpr (detail::KeyCarriesId<TKey>) {
            return key.Id() == id;
        } else {
            (void)key;
            (void)id;
            return true;
        }
    }

    Result<KeyLease<TKey>> WaitFlight(const Flight& flight) noexcept {
        platform::LockGuard lock(mutex_);
        while (!flight.done) {
            if (changed_.Wait(mutex_) == platform::WaitResult::Failed) {
                platform::Fatal("KeyRing: condition variable wait failed");
            }
        }

        if (flight.error != ErrorCode::None) {
            return Result<KeyLease<TKey>>::Failure(flight.error);
        }

        return Result<KeyLease<TKey>>::Success(KeyLease<TKey>(flight.entry));
    }

    // 공급자를 부르고(공급자 잠금 안, 표 잠금 밖) 결과를 등록·게시한다.
    Result<KeyLease<TKey>> Fetch(KeyId id, Flight& flight) noexcept {
        ErrorCode error = ErrorCode::None;
        SharedPtr<Entry> entry;
        {
            platform::LockGuard gate(providerGate_);
            platform::AtomicStore(&providerThread_, platform::CurrentThreadId());
            Result<TKey> fetched = provider_.IsBound() ? provider_.Execute(id)
                                                       : Result<TKey>::Failure(ErrorCode::NotFound);
            platform::AtomicStore(&providerThread_, uint64{0});
            if (!fetched) {
                error = fetched.Error();
            } else if (fetched.Value().IsEmpty()) {
                error = ErrorCode::StaleHandle;
            } else if (!CarriesId(fetched.Value(), id)) {
                error = ErrorCode::AuthenticationFailed;
            } else {
                auto made = SharedPtr<Entry>::TryMake(allocator_, id, Move(fetched.Value()));
                if (made) {
                    entry = Move(made.Value());
                } else {
                    error = made.Error();
                }
            }
        }

        // 공급자의 키는 잠금 밖에서 놓는다(먼저 등록된 항목이 있으면 그쪽을 쓴다).
        SharedPtr<Entry> unused;
        {
            platform::LockGuard lock(mutex_);
            if (error == ErrorCode::None) {
                if (const SharedPtr<Entry>* existing = entries_.Find(id.value)) {
                    unused = Move(entry);
                    entry = *existing;
                } else {
                    auto inserted = entries_.Insert(id.value, entry);
                    if (!inserted) {
                        error = inserted.Error();
                        unused = Move(entry);
                    }
                }
            }

            flight.entry = entry;
            flight.error = error;
            flight.done = true;
            flights_.Erase(id.value);
            // 잠금 안에서 깨운다. 잠금을 푼 뒤 깨우면 done을 본 대기자가 돌아가 소유자가 KeyRing을
            // 소멸한 뒤 이 호출이 소멸한 객체를 쓸 수 있다.
            changed_.NotifyAll();
        }

        if (error != ErrorCode::None) {
            return Result<KeyLease<TKey>>::Failure(error);
        }

        return Result<KeyLease<TKey>>::Success(KeyLease<TKey>(entry));
    }

    Allocator& allocator_;
    mutable platform::Mutex mutex_; // entries_·flights_·current_·hasProvider_
    platform::ConditionVariable changed_;
    HashMap<Guid, SharedPtr<Entry>> entries_;
    HashMap<Guid, SharedPtr<Flight>> flights_;
    KeyId current_;
    bool hasProvider_ = false;
    // 공급자 호출을 한 번에 하나로 묶는다(provider_ 보호). 잠금 순서는 providerGate_ → mutex_다.
    platform::Mutex providerGate_;
    Provider provider_;
    // 공급자를 실행 중인 스레드 ID(없으면 0)다. 공급자 안의 재진입을 알아보는 데만 쓴다.
    uint64 providerThread_ = 0;
};

// KeyBlob 표 공급자다(10.3·10.4절). KeyId → KeyBlob(Key 방식) 표를 들고, 요청이 오면 blob의
// wrapKeyId로 KEK(KeyRing<KdfKey>)를 빌려 TryUnwrapKey<TKey>로 푼 뒤 blob의 keyId가 요청한 ID와
// 같은지 확인한다(다르면 AuthenticationFailed: 같은 KEK 아래 blob 통째 교체 방어). 빌드 시 주입한
// 표나 저장소에서 읽은 blob을 담는 데 쓴다. 비밀번호 방식 blob은 담지 않는다(비밀번호가 없다).
//
// - blob은 복사해 둔다(GetSecretAllocator, 해제 때 지운다). 표 변경과 TryFetch는 여러 스레드에서
//   동시에 불러도 된다(잠금 하나, 잠근 구간은 표 조회·복사뿐이다).
// - kekRing과 이 객체는 이 공급자를 묶은 KeyRing이 공급자를 쓰는 동안 살아 있어야 한다
//   (AsProvider는 수명을 추적하지 않는 CreateRaw다). 먼저 없애야 하면 KeyRing::SetProvider({})로
//   떼어 낸 뒤 없앤다.
//
// 사용 예:
//   crypto::KeyRing<crypto::KdfKey> keks;
//   keks.TryAdd(kekId, Move(kek));
//   crypto::KeyBlobProvider<crypto::SegmentStreamKey> blobs(keks);
//   blobs.TryAddBlob(storedBlob);                       // 저장소·빌드 시 표에서 읽은 blob
//   crypto::KeyRing<crypto::SegmentStreamKey> ring;
//   ring.SetProvider(blobs.AsProvider());
//   auto lease = ring.TryAcquire(headerKeyId);          // 처음 요청할 때 blob을 푼다
template <KeyBlobKey TKey>
class KeyBlobProvider {
public:
    explicit KeyBlobProvider(KeyRing<KdfKey>& kekRing) noexcept
        : kekRing_(kekRing), blobs_(GetSecretAllocator()) {
    }

    KeyBlobProvider(const KeyBlobProvider&) = delete;
    KeyBlobProvider& operator=(const KeyBlobProvider&) = delete;

    // blob을 표에 넣는다(인증은 TryFetch에서 한다). TryPeekKeyBlob 오류(형식 AuthenticationFailed,
    // 모르는 버전·타입 Unsupported)는 그대로, TKey와 다른 타입이나 blob 길이가 기록과 다르면
    // AuthenticationFailed, 비밀번호 방식은 Unsupported, 같은 keyId가 이미 있으면 AlreadyExists,
    // 할당 실패는 OutOfMemory다.
    [[nodiscard]] Result<void> TryAddBlob(Span<const uint8> blob) noexcept {
        auto info = TryPeekKeyBlob(blob);
        if (!info) {
            return Result<void>::Failure(info.Error());
        }

        if (info.Value().type != KeyBlobTraits<TKey>::Type ||
            info.Value().blobBytes != blob.Size()) {
            return Result<void>::Failure(ErrorCode::AuthenticationFailed);
        }

        if (info.Value().method != KeyBlobWrapMethod::Key) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        Array<uint8> copy(GetSecretAllocator());
        if (auto appended = copy.TryAppend(blob); !appended) {
            return Result<void>::Failure(appended.Error());
        }

        platform::LockGuard lock(mutex_);
        auto inserted = blobs_.Insert(info.Value().keyId.value, Move(copy));
        if (!inserted) {
            return Result<void>::Failure(inserted.Error());
        }

        if (inserted.Value() == InsertOutcome::AlreadyPresent) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }

        return Result<void>::Success();
    }

    // id의 blob을 표에서 뺀다(이미 등록된 키는 KeyRing에 남는다). 없으면 NotFound다.
    [[nodiscard]] Result<void> TryRemoveBlob(KeyId id) noexcept {
        platform::LockGuard lock(mutex_);
        if (!blobs_.Erase(id.value)) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        return Result<void>::Success();
    }

    // 공급자 본체다. 표에 없으면 NotFound, KEK가 없으면 KEK KeyRing의 TryAcquire 오류, blob
    // 검증(commitment·tag)·keyId 불일치는 AuthenticationFailed, 나머지는 TryUnwrapKey 오류다.
    [[nodiscard]] Result<TKey> TryFetch(KeyId id) noexcept {
        uint8 blob[KeyBlobMaxBytes];
        usize bytes = 0;
        {
            platform::LockGuard lock(mutex_);
            const Array<uint8>* found = blobs_.Find(id.value);
            if (found == nullptr) {
                return Result<TKey>::Failure(ErrorCode::NotFound);
            }

            bytes = found->Size();
            platform::MemoryCopy(blob, found->Data(), bytes);
        }

        const Span<const uint8> stored(blob, bytes);
        auto info = TryPeekKeyBlob(stored);
        if (!info) {
            return Result<TKey>::Failure(info.Error());
        }

        auto kek = kekRing_.TryAcquire(info.Value().wrapKeyId);
        if (!kek) {
            return Result<TKey>::Failure(kek.Error());
        }

        auto unwrapped = TryUnwrapKey<TKey>(kek.Value().Key(), stored);
        if (!unwrapped) {
            return Result<TKey>::Failure(unwrapped.Error());
        }

        if (unwrapped.Value().id != id) {
            return Result<TKey>::Failure(ErrorCode::AuthenticationFailed);
        }

        return Result<TKey>::Success(Move(unwrapped.Value().key));
    }

    // KeyRing::SetProvider에 넘길 delegate다(이 객체의 수명을 추적하지 않는다).
    [[nodiscard]] typename KeyRing<TKey>::Provider AsProvider() noexcept {
        return KeyRing<TKey>::Provider::CreateRaw(this, &KeyBlobProvider::TryFetch);
    }

private:
    KeyRing<KdfKey>& kekRing_;
    platform::Mutex mutex_;
    HashMap<Guid, Array<uint8>> blobs_;
};

} // namespace rocks::crypto
