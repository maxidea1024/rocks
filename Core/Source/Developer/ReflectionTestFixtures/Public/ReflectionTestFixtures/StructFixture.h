#pragma once

#include <ReflectionTestFixtures/ReflectionFixture.h>
#include <Serialization/Record.h>

namespace rocks::reflection_fixture {

struct StructOwner;

// B4 리플렉션 struct fixture다(Docs/Design/ReflectionFoundations.ko.md 4절). MetaCompiler 생성
// 코드로 StructGraphTests(손으로 쓴 특수화)와 같은 모양의 그래프를 만든다: 3단 중첩(StructHolder →
// StructMiddle → StructLeaf), 참조 struct(StructHolder::target, Required), struct 안 enum(다른
// 헤더의 TextureGroup)·String·EditorOnly·NATIVE 멤버, 타입 마이그레이션(StructLeaf::Migrate,
// Version 2), NATIVE 멤버로 둔 리플렉션 struct(StructOwner::scratch).
ROCKS_STRUCT(Version = 2)
struct StructLeaf {
    explicit StructLeaf(Allocator& allocator) noexcept : label(allocator) {
    }

    // v1 Record를 v2로 올린다(필드를 그대로 옮기고 버전만 올린다). 시험이 호출 수를 센다.
    static Result<Array<uint8>> Migrate(Allocator& allocator, TypeId type, uint32 from, uint32 to,
                                        Span<const uint8> source) noexcept {
        ++migrations;
        auto reader = RecordReader::Open(allocator, source, type, from, from);
        if (!reader) {
            return Result<Array<uint8>>::Failure(reader.Error());
        }

        RecordBuilder builder(allocator, type, to);
        for (usize index = 0; index < reader.Value().FieldCount(); ++index) {
            auto field = reader.Value().Next();
            if (!field) {
                return Result<Array<uint8>>::Failure(field.Error());
            }

            if (auto added = builder.AddField(field.Value().id, field.Value().wireType,
                                              field.Value().payload);
                !added) {
                return Result<Array<uint8>>::Failure(added.Error());
            }
        }

        return builder.Build();
    }

    static inline uint32 migrations = 0;
    ROCKS_PROPERTY() uint32 count = 0;
    ROCKS_PROPERTY() String label;
    ROCKS_PROPERTY() TextureGroup group = TextureGroup::World;
};

ROCKS_STRUCT()
struct StructMiddle {
    explicit StructMiddle(Allocator& allocator) noexcept : leaf(allocator) {
    }

    ROCKS_PROPERTY() StructLeaf leaf;
    ROCKS_PROPERTY() int64 offset = 0;
    ROCKS_PROPERTY(EditorOnly) uint16 note = 0;
    // 저장하지 않는 런타임 값이다(struct의 NATIVE 멤버).
    ROCKS_NATIVE() uint32 cache = 0;
};

ROCKS_STRUCT()
struct StructHolder {
    explicit StructHolder(Allocator& allocator) noexcept : middle(allocator) {
    }

    ROCKS_PROPERTY(Required) ObjectRef<StructOwner> target;
    ROCKS_PROPERTY() StructMiddle middle;
    ROCKS_PROPERTY() uint8 flag = 0;
};

ROCKS_CLASS()
struct StructOwner final : Object {
    explicit StructOwner(Allocator& allocator) noexcept
        : holder(allocator), extra(allocator), scratch(allocator) {
        ++constructed;
    }

    static inline uint32 constructed = 0;
    ROCKS_PROPERTY() StructHolder holder;
    ROCKS_PROPERTY() uint32 plain = 0;
    ROCKS_PROPERTY(EditorOnly) StructLeaf extra;
    // NATIVE 멤버로 둔 리플렉션 struct다(저장하지 않는다 — 생성 코드가 struct 형질로 RMC2205를
    // 확인한다).
    ROCKS_NATIVE() StructLeaf scratch;
};

} // namespace rocks::reflection_fixture
