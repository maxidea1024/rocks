#pragma once

#include <Base/TypeTraits.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/TypeKey.h>

// 소유자(Engine·World)와 수명을 같이하는 Subsystem 묶음이다. Subsystem은 의존을 선언하고, 묶음은
// 의존 순서대로 만들고 초기화하며 역순으로 정리한다. 모든 시스템을 Subsystem으로 만들 필요는 없다
// (GameDirector·GameState처럼 게임별 상속과 월드 수명을 갖는 객체는 Subsystem이 아니다).
//
// 사용 예(World Subsystem):
//   class NavigationSubsystem final : public WorldSubsystem {
//   public:
//       Result<void> Initialize(World& world) noexcept override;
//   };
//   class AiSubsystem final : public WorldSubsystem { ... };
//
//   static const SubsystemKey aiDependencies[] = {SubsystemKeyOf<NavigationSubsystem>()};
//   const SubsystemType<WorldSubsystem> types[] = {
//       MakeSubsystemType<WorldSubsystem, AiSubsystem>("Ai", aiDependencies),
//       MakeSubsystemType<WorldSubsystem, NavigationSubsystem>("Navigation"),
//   };
//   // WorldDesc.subsystems = types; → Navigation이 먼저 초기화되고 나중에 정리된다.
//   auto* navigation = world.GetSubsystem<NavigationSubsystem>();   // 없으면 nullptr
namespace rocks {

// Subsystem 타입의 실행 중 식별 키다. 저장하지 않는다. 키는 TypeKeyOf<T>()다(Core/TypeKey.h, UE는
// UClass* — Docs/Design/BuildTargets 3.8절 BS8e). 다른 모듈이 GetSubsystem<T>()·의존 목록으로
// 찾는 Subsystem은 공개 헤더에 ROCKS_DECLARE_NATIVE_TYPE_KEY(T, 모듈 API 매크로)를 두고 .cpp 하나에
// ROCKS_DEFINE_NATIVE_TYPE_KEY(T)로 정의한다 — 그래야 Modular(모듈마다 공유 라이브러리)에서 모듈
// 사이에 키가 같다. 선언하지 않은 타입의 키는 실행 이미지마다 다르다(한 모듈 안에서만 쓰는
// Subsystem·시험용).
using SubsystemKey = const void*;

template <class T>
SubsystemKey SubsystemKeyOf() noexcept {
    return TypeKeyOf<T>();
}

// 생성 여부 판정이다. owner는 묶음의 소유자(Engine·World·GameInstance·LocalPlayer)를 가리킨다.
using SubsystemShouldCreate = bool (*)(const void* owner) noexcept;

// Subsystem 한 종류의 생성 정보다. name은 진단용 정적 문자열이다. 배열은 묶음이 초기화를 마칠
// 때까지 살아 있어야 한다(보통 정적 배열). Docs/Design/EngineRuntime 16.3절(ERD8·ERD9).
// - dependencies: 필수 의존이다. 목록에 없는 키는 선언 오류(InvalidArgument)이고, 목록에 있지만
//   생성되지 않은(shouldCreate 거짓·의존 누락) 의존이 있으면 이 Subsystem도 만들지 않는다(생략 —
//   Skipped()에 남는다. UE는 의존자를 그래도 만든다 — 필수 의존이 언제나 있다는 계약을 지키려는
//   차이).
// - optionalDependencies: 있으면 순서만 맞춘다(목록에 없거나 생성되지 않아도 된다).
// - shouldCreate: 거짓이면 만들지 않는다. 없으면 소유자의 기본 판정(World는 World 종류
// — 16.4절)이다.
template <class Base>
struct SubsystemType {
    SubsystemKey key = nullptr;
    const char* name = "";
    Result<UniquePtr<Base>> (*create)(Allocator&) noexcept = nullptr;
    Span<const SubsystemKey> dependencies;
    Span<const SubsystemKey> optionalDependencies;
    SubsystemShouldCreate shouldCreate = nullptr;
};

// 생략된 Subsystem의 이유다(진단 — 오류가 아니다).
enum class SubsystemSkipReason : uint8 {
    // shouldCreate(또는 소유자 기본 판정)가 거짓이었다.
    NotWanted,
    // 필수 의존이 생성되지 않았다(missing이 그 의존 이름).
    MissingDependency
};

struct SkippedSubsystem {
    const char* name = "";
    SubsystemSkipReason reason = SubsystemSkipReason::NotWanted;
    const char* missing = "";
};

template <class Base, class T>
SubsystemType<Base> MakeSubsystemType(const char* name, Span<const SubsystemKey> dependencies = {},
                                      Span<const SubsystemKey> optionalDependencies = {},
                                      SubsystemShouldCreate shouldCreate = nullptr) noexcept {
    static_assert(IsBaseOfV<Base, T>);
    SubsystemType<Base> type;
    type.key = SubsystemKeyOf<T>();
    type.name = name;
    type.create = [](Allocator& allocator) noexcept -> Result<UniquePtr<Base>> {
        auto created = UniquePtr<T>::TryMake(allocator);
        if (!created) {
            return Result<UniquePtr<Base>>::Failure(created.Error());
        }
        return Result<UniquePtr<Base>>::Success(UniquePtr<Base>(Move(created.Value())));
    };
    type.dependencies = dependencies;
    type.optionalDependencies = optionalDependencies;
    type.shouldCreate = shouldCreate;
    return type;
}

// Base는 다음을 제공해야 한다: 가상 소멸자, `Result<void> Initialize(Owner&) noexcept`,
// `void Deinitialize(Owner&) noexcept`.
template <class Base, class Owner>
class SubsystemCollection {
public:
    explicit SubsystemCollection(Allocator& allocator) noexcept
        : allocator_(allocator), instances_(allocator), keys_(allocator), skipped_(allocator) {
    }

    SubsystemCollection(const SubsystemCollection&) = delete;
    SubsystemCollection& operator=(const SubsystemCollection&) = delete;

    ~SubsystemCollection() {
        if (instances_.Size() != 0) {
            platform::Fatal("SubsystemCollection destroyed before Deinitialize");
        }
    }

    // 의존 순서대로 만들고 Initialize한다. 같은 키 중복, 목록에 없는 필수 의존, 순환 의존(선택
    // 의존 포함)은 InvalidArgument이며 아무것도 만들지 않는다. 생성하지 않기로 한 것과 필수 의존이
    // 없는 것은 생략하고 Skipped()에 남긴다. 생성·초기화 실패는 그 오류를 반환하고 이미 초기화한
    // Subsystem을 역순으로 Deinitialize한 뒤 비운다. 이미 초기화된 묶음은 AlreadyExists다.
    // defaultShouldCreate는 type.shouldCreate가 없을 때의 소유자 기본 판정이다(없으면 생성).
    Result<void> Initialize(Span<const SubsystemType<Base>> types, Owner& owner,
                            bool (*defaultShouldCreate)(const Owner&) noexcept = nullptr) noexcept {
        if (instances_.Size() != 0) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }

        skipped_.Clear();
        Array<uint32> order(allocator_);
        auto sorted = Sort(types, order);
        if (!sorted) {
            return sorted;
        }

        auto reserved = instances_.TryReserve(types.Size());
        if (!reserved) {
            return reserved;
        }
        reserved = keys_.TryReserve(types.Size());
        if (!reserved) {
            return reserved;
        }
        reserved = skipped_.TryReserve(types.Size());
        if (!reserved) {
            return reserved;
        }

        for (uint32 index : order) {
            const SubsystemType<Base>& type = types[index];
            const bool wanted = type.shouldCreate != nullptr     ? type.shouldCreate(&owner)
                                : defaultShouldCreate != nullptr ? defaultShouldCreate(owner)
                                                                 : true;
            if (!wanted) {
                auto noted = skipped_.TryPushBack(
                    SkippedSubsystem{type.name, SubsystemSkipReason::NotWanted, ""});
                (void)noted;
                continue;
            }

            // 의존 순서로 처리하므로 필수 의존은 이미 만들어졌거나 생략됐다.
            const char* missing = nullptr;
            for (SubsystemKey dependency : type.dependencies) {
                if (Find(dependency) == nullptr) {
                    missing = NameOf(types, dependency);
                    break;
                }
            }
            if (missing != nullptr) {
                auto noted = skipped_.TryPushBack(
                    SkippedSubsystem{type.name, SubsystemSkipReason::MissingDependency, missing});
                (void)noted;
                continue;
            }

            auto created = type.create(allocator_);
            if (!created) {
                Deinitialize(owner);
                return Result<void>::Failure(created.Error());
            }
            auto initialized = created.Value()->Initialize(owner);
            if (!initialized) {
                // 초기화에 실패한 Subsystem은 Deinitialize하지 않는다(부분 정리는 Initialize 책임).
                // 그 소멸자가 Initialize 중 얻은 의존에 접근할 수 있으므로 의존을 정리하기 전에
                // 파괴한다.
                created.Value().Reset();
                Deinitialize(owner);
                return initialized;
            }
            // 위에서 용량을 확보했으므로 실패하지 않는다.
            auto pushed = instances_.TryPushBack(Move(created.Value()));
            auto keyed = keys_.TryPushBack(type.key);
            (void)pushed;
            (void)keyed;
        }

        return Result<void>::Success();
    }

    // 초기화 역순으로 Deinitialize하고 파괴한다. 여러 번 호출해도 안전하다.
    void Deinitialize(Owner& owner) noexcept {
        skipped_.Clear();
        // 자기 Deinitialize 중에도 Get<자기 타입>이 동작하도록 정리한 뒤에 목록에서 뺀다.
        while (instances_.Size() != 0) {
            instances_[instances_.Size() - 1]->Deinitialize(owner);
            auto last = instances_.TryPopBack();
            auto key = keys_.TryPopBack();
            (void)last;
            (void)key;
        }
    }

    // T 종류의 Subsystem이다. 없거나 아직 초기화되지 않았으면 nullptr이다. 반환 포인터는 묶음이
    // Deinitialize될 때까지 유효하다.
    template <class T>
    T* Get() const noexcept {
        return static_cast<T*>(Find(SubsystemKeyOf<T>()));
    }

    // 키로 찾는다(타입을 모르는 조회 — 바깥 계층 조회 창, 16.3절 ERD10). 없으면 nullptr이다.
    Base* Find(SubsystemKey key) const noexcept {
        for (usize index = 0; index < keys_.Size(); ++index) {
            if (keys_[index] == key) {
                return instances_[index].Get();
            }
        }
        return nullptr;
    }

    // 마지막 Initialize가 생략한 Subsystem이다(진단용, 다음 Initialize·Deinitialize까지 유효).
    Span<const SkippedSubsystem> Skipped() const noexcept {
        return Span<const SkippedSubsystem>(skipped_.Data(), skipped_.Size());
    }

    usize Count() const noexcept {
        return instances_.Size();
    }

    // 초기화 순서의 index번째 Subsystem 이름 확인 등 진단용 순회다.
    Base* At(usize index) const noexcept {
        return index < instances_.Size() ? instances_[index].Get() : nullptr;
    }

private:
    static const char* NameOf(Span<const SubsystemType<Base>> types, SubsystemKey key) noexcept {
        for (const SubsystemType<Base>& type : types) {
            if (type.key == key) {
                return type.name;
            }
        }
        return "";
    }

    // 의존이 먼저 오도록 index 순서를 만든다(깊이 우선 후위 방문, 표시 0/1/2). 선택 의존도 순서에
    // 넣되, 목록에 없는 선택 의존은 건너뛴다.
    Result<void> Sort(Span<const SubsystemType<Base>> types, Array<uint32>& order) noexcept {
        Array<uint8> marks(allocator_);
        auto resized = marks.TryResize(types.Size());
        if (!resized) {
            return resized;
        }
        for (usize left = 0; left < types.Size(); ++left) {
            marks[left] = 0;
            // 생성 함수가 없는 항목은 아무것도 만들기 전에 거부한다.
            if (types[left].create == nullptr) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
            for (usize right = left + 1; right < types.Size(); ++right) {
                if (types[left].key == types[right].key) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }
            }
        }

        for (usize index = 0; index < types.Size(); ++index) {
            auto visited = Visit(types, static_cast<uint32>(index), marks, order, 0);
            if (!visited) {
                return visited;
            }
        }

        return Result<void>::Success();
    }

    Result<void> Visit(Span<const SubsystemType<Base>> types, uint32 index, Array<uint8>& marks,
                       Array<uint32>& order, uint32 depth) noexcept {
        if (marks[index] == 2) {
            return Result<void>::Success();
        }
        // 방문 중인 노드를 다시 만나면 순환이다. 깊이는 타입 수를 넘을 수 없다.
        if (marks[index] == 1 || depth > types.Size()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        marks[index] = 1;
        for (uint32 pass = 0; pass < 2; ++pass) {
            const bool optional = pass == 1;
            const Span<const SubsystemKey> keys =
                optional ? types[index].optionalDependencies : types[index].dependencies;
            for (SubsystemKey dependency : keys) {
                uint32 found = static_cast<uint32>(types.Size());
                for (usize candidate = 0; candidate < types.Size(); ++candidate) {
                    if (types[candidate].key == dependency) {
                        found = static_cast<uint32>(candidate);
                        break;
                    }
                }
                if (found == types.Size()) {
                    if (optional) {
                        continue;
                    }
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }
                auto visited = Visit(types, found, marks, order, depth + 1);
                if (!visited) {
                    return visited;
                }
            }
        }

        marks[index] = 2;
        return order.TryPushBack(index);
    }

    Allocator& allocator_;
    Array<UniquePtr<Base>> instances_;
    Array<SubsystemKey> keys_;
    Array<SkippedSubsystem> skipped_;
};

} // namespace rocks
