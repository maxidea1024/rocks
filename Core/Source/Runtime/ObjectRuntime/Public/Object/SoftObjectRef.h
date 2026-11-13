#pragma once

#include <Core/Text/String.h>
#include <Object/AssetId.h>
#include <Object/Object.h>

namespace rocks {

// 에셋 안 객체를 가리키는 약한(soft) 참조다(Docs/Design/ReflectionFoundations.ko.md 3.3절, UE
// TSoftObjectPtr 대응). GC 간선이 아니다 — 대상을 살려 두지 않고 로드도 하지 않는다. asset은 에셋
// ID, objectPath는 패키지 안 객체 경로(Outer 이름을 '.'로 이은 것 — Docs/Design/AssetSystem.ko.md
// 4.1절, 비면 대표 객체)이며, T는 기대 타입(리플렉션 필드의 value.type, Object면 0)이다. 경로
// 문법과 기대 타입은 대상을 찾을 때(A0-O·A1) 검사하며 이 타입은 아래 규칙만 검사한다.
//
// 규칙: null은 asset이 0이고 경로가 빈 것이다. asset이 0인데 경로가 있으면 잘못된
// 값(InvalidArgument)이다. 경로는 유효한 UTF-8이고 MaxObjectPathBytes(4 KiB) 이하다.
//
// 수명·소유: 경로 String이 생성자에 준 allocator를 빌리므로 allocator는 이 값보다 오래 살아야 한다.
// 이동 전용이며 이동하면 원본은 null이 된다. 이동 대입은 String처럼 원본의 allocator를
// 넘겨받으므로, 객체 필드처럼 allocator를 유지해야 하는 곳에는 TryAssign(복사)을 쓴다. 복제는
// 실패할 수 있는 TryClone(allocator)이다. public 멤버를 직접 바꾸면 규칙을 검사하지 않는다 —
// 리플렉션 쓰기·그래프 저장이 다시 검사해 잘못된 값을 InvalidArgument로 거부한다.
//
// 사용 예:
//   SoftObjectRef<Mesh> socket(allocator);
//   if (auto set = socket.TryAssign(meshAsset, "Body.Socket"); !set) { ... }   // 실패하면 값 불변
//   if (socket.IsNull()) { ... }
//   // holder의 allocator를 유지하며 복사한다.
//   if (auto copied = holder.socket.TryAssign(socket); !copied) { ... }
//
// 리플렉션 필드(ValueKind::SoftObjectReference)로 쓰면 소유 클래스에 noexcept T(Allocator&)
// 생성자가 필요하다 (String 필드와 같다). 저장 wire는 SoftObjectReference(Guid 16 byte + String)다.
template <class T = Object>
class SoftObjectRef {
public:
    static constexpr usize MaxObjectPathBytes = 4096;

    explicit SoftObjectRef(Allocator& allocator) noexcept : objectPath(allocator) {
    }

    // 원본은 null(asset 0, 빈 경로)이 된다. 경로만 비우면 원본이 같은 에셋의 대표 객체를 가리키는
    // 유효한 참조로 남기 때문이다.
    SoftObjectRef(SoftObjectRef&& other) noexcept
        : asset(other.asset), objectPath(Move(other.objectPath)) {
        other.asset = AssetId{};
    }

    // 원본의 경로 allocator를 넘겨받고 원본은 null이 된다. 자기 대입은 아무것도 바꾸지 않는다.
    SoftObjectRef& operator=(SoftObjectRef&& other) noexcept {
        if (this != &other) {
            asset = other.asset;
            objectPath = Move(other.objectPath);
            other.asset = AssetId{};
        }

        return *this;
    }

    SoftObjectRef(const SoftObjectRef&) = delete;
    SoftObjectRef& operator=(const SoftObjectRef&) = delete;

    bool IsNull() const noexcept {
        return asset.IsNull() && objectPath.ByteCount() == 0;
    }

    // (asset, path) 쌍이 규칙에 맞는지 본다. 맞지 않으면 InvalidArgument다.
    static Result<void> Validate(AssetId id, StringView path) noexcept {
        if ((id.IsNull() && path.ByteCount() != 0) || path.ByteCount() > MaxObjectPathBytes ||
            !path.ValidateUtf8()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<void>::Success();
    }

    Result<void> Validate() const noexcept {
        return Validate(asset, objectPath.View());
    }

    // 규칙을 검사하고 경로를 복사한 뒤 바꾼다. 실패(InvalidArgument·OutOfMemory)하면 값은 그대로다.
    Result<void> TryAssign(AssetId newAsset, StringView path) noexcept {
        if (auto valid = Validate(newAsset, path); !valid) {
            return valid;
        }

        String replacement(objectPath.GetAllocator());
        if (auto copied = replacement.TryAssign(path); !copied) {
            return copied;
        }

        asset = newAsset;
        objectPath.Swap(replacement);
        return Result<void>::Success();
    }

    // other의 값을 이 값의 allocator로 복사한다(자기 대입 포함). 실패(OutOfMemory)하면 값은
    // 그대로다.
    Result<void> TryAssign(const SoftObjectRef& other) noexcept {
        return TryAssign(other.asset, other.objectPath.View());
    }

    // allocator로 복제한다. allocator는 결과보다 오래 살아야 한다. 경로 복사가 실패하면
    // OutOfMemory다.
    Result<SoftObjectRef> TryClone(Allocator& allocator) const noexcept {
        SoftObjectRef copy(allocator);
        if (auto copied = copy.objectPath.TryAssign(objectPath.View()); !copied) {
            return Result<SoftObjectRef>::Failure(copied.Error());
        }

        copy.asset = asset;
        return Result<SoftObjectRef>::Success(Move(copy));
    }

    AssetId asset{};
    String objectPath;
};

} // namespace rocks
