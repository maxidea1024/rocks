#include <ReflectionTestFixtures/StructFixture.h>

namespace rocks::reflection_fixture {

Result<Array<uint8>> StructLeaf::Migrate(Allocator& allocator, TypeId type, uint32 from, uint32 to,
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

        if (auto added =
                builder.AddField(field.Value().id, field.Value().wireType, field.Value().payload);
            !added) {
            return Result<Array<uint8>>::Failure(added.Error());
        }
    }

    return builder.Build();
}

} // namespace rocks::reflection_fixture
