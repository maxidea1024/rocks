#include <AssetCooker/CookKeys.h>
#include <AssetRuntime/PackageFormat.h>
#include <Core/Algorithms.h>
#include <Core/Assert.h>
#include <Core/Containers/HashMap.h>
#include <Core/Graph/StrongComponents.h>

namespace rocks {

namespace {

using NodeMap = HashMap<AssetId, uint32, SeededHash<AssetId>>;

// SCC 묶음 판정에 쓰는 "아직 어느 묶음도 아님" 표지다.
constexpr uint32 NoComponent = NumericLimits<uint32>::Max();

// 계산 상태다. 진단 push의 할당 실패는 error_에 두고 계속한다(CookTargets와 같은 방식).
class KeyWalk {
public:
    KeyWalk(Allocator& allocator, const CookKeyRequest& request, CookKeySet& out) noexcept
        : allocator_(allocator), request_(request), out_(out), nodes_(allocator), map_(allocator),
          failed_(allocator), contentHashes_(allocator), localKeys_(allocator),
          buildKeys_(allocator), cookFailed_(allocator), cookLocal_(allocator),
          cookKeys_(allocator), component_(allocator), ownDefaults_(allocator) {
    }

    Result<void> Run() noexcept {
        auto typeTable = HashTypeTable(allocator_, *request_.types);
        if (!typeTable) {
            return Result<void>::Failure(typeTable.Error());
        }

        defaults_ = request_.classDefaults;
        if (defaults_ == nullptr) {
            if (auto built = BuildCookClassDefaults(allocator_, *request_.types, ownDefaults_);
                !built) {
                return built;
            }

            defaults_ = &ownDefaults_;
        }

        out_.typeTable = typeTable.Value();
        if (auto collected = CollectDomain(); !collected) {
            return collected;
        }

        if (auto local = ComputeLocalBuildKeys(); !local) {
            return local;
        }

        if (auto build = ComputeBuildKeys(); !build) {
            return build;
        }

        if (auto cook = ComputeCookKeys(); !cook) {
            return cook;
        }

        if (error_ != ErrorCode::None) {
            return Result<void>::Failure(error_);
        }

        const usize count = request_.targets->targets.Size();
        if (auto reserved = out_.keys.TryReserve(count); !reserved) {
            return reserved;
        }

        for (usize index = 0; index < count; ++index) {
            CookKey key;
            key.asset = nodes_[index]->id;
            key.contentHash = contentHashes_[index];
            key.buildKey = buildKeys_[index];
            key.cookKey = cookKeys_[index];
            key.hasKey = cookFailed_[index] == 0;
            (void)out_.keys.TryPushBack(key);
        }

        algo::Sort(out_.diagnostics.View(), CookDiagnosticBefore);
        return Result<void>::Success();
    }

private:
    // 여기 오는 실패는 모두 할당 실패다(어느 것을 두어도 같다).
    void Keep(const Result<void>& step) noexcept {
        if (!step) {
            error_ = step.Error();
        }
    }

    void Diagnose(AssetDiagnosticCode code, AssetId asset, AssetId related,
                  ErrorCode error) noexcept {
        Keep(out_.diagnostics.TryPushBack(CookDiagnostic{code, false, asset, related, 0, error}));
    }

    // 영역 표는 색인 항목 수만큼 미리 잡았다(영역 ⊆ 색인의 고유 항목) — 넣기는 할당하지 않는다.
    void AddNode(const AssetIndexEntry& entry) noexcept {
        auto inserted = map_.Insert(entry.id, static_cast<uint32>(nodes_.Size()));
        if (inserted.Value() == InsertOutcome::Inserted) {
            (void)nodes_.TryPushBack(&entry);
            (void)failed_.TryPushBack(0);
        }
    }

    // Build 영역 = 대상(먼저 — 노드 번호 = 대상 번호) ∪ Build 간선으로 닿는 에셋.
    Result<void> CollectDomain() noexcept {
        const usize capacity = request_.index->Count();
        if (auto reserved = nodes_.TryReserve(capacity); !reserved) {
            return reserved;
        }

        if (auto reserved = failed_.TryReserve(capacity); !reserved) {
            return reserved;
        }

        if (auto reserved = map_.Reserve(capacity); !reserved) {
            return reserved;
        }

        for (const CookTarget& target : request_.targets->targets) {
            auto found = request_.index->FindById(target.id);
            if (!found) {
                // 대상은 색인의 고유 항목이다(ComputeCookTargets) — 다른 색인의 결과를 넘겼다.
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            AddNode(*found.Value());
        }

        for (usize at = 0; at < nodes_.Size(); ++at) {
            for (const PackageDependencyEntry& dependency : nodes_[at]->dependencies) {
                if (dependency.kind != PackageDependencyKind::Build) {
                    continue;
                }

                auto found = request_.index->FindById(dependency.asset);
                if (!found) {
                    // FindById의 오류는 NotFound·AlreadyExists뿐이다.
                    Diagnose(found.Error() == ErrorCode::NotFound
                                 ? AssetDiagnosticCode::MissingDependency
                                 : AssetDiagnosticCode::DuplicateAssetId,
                             nodes_[at]->id, dependency.asset, ErrorCode::None);
                    failed_[at] = 1;
                } else {
                    AddNode(*found.Value());
                }
            }
        }

        return Result<void>::Success();
    }

    // 노드마다 콘텐츠 해시·처리기·지역 build key다.
    Result<void> ComputeLocalBuildKeys() noexcept {
        const usize count = nodes_.Size();
        if (auto sized = contentHashes_.TryResize(count); !sized) {
            return sized;
        }

        if (auto sized = localKeys_.TryResize(count); !sized) {
            return sized;
        }

        if (auto sized = buildKeys_.TryResize(count); !sized) {
            return sized;
        }

        for (usize at = 0; at < count; ++at) {
            const AssetIndexEntry& entry = *nodes_[at];
            auto hash = request_.index->ContentHash(*request_.files, entry.id);
            if (!hash) {
                if (hash.Error() == ErrorCode::OutOfMemory) {
                    return Result<void>::Failure(hash.Error());
                }

                Diagnose(hash.Error() == ErrorCode::NotFound ? AssetDiagnosticCode::MissingFile
                                                             : AssetDiagnosticCode::None,
                         entry.id, {}, hash.Error());
                failed_[at] = 1;
                continue;
            }

            contentHashes_[at] = hash.Value();
            auto processor = request_.processors->Find(entry.type);
            if (!processor) {
                const ErrorCode error = processor.Error();
                if (error != ErrorCode::NotFound && error != ErrorCode::TypeMismatch) {
                    return Result<void>::Failure(error);
                }

                // 7.1절 8: 미등록 타입은 UnsupportedVersion, 종류가 다르면 TypeMismatch다.
                Diagnose(error == ErrorCode::NotFound ? AssetDiagnosticCode::UnsupportedVersion
                                                      : AssetDiagnosticCode::TypeMismatch,
                         entry.id, {}, error);
                failed_[at] = 1;
                continue;
            }

            NuggetKeyWriter writer;
            WriteNuggetKeyHeader(writer, processor.Value()->Identity(), request_.projectGeneration);
            processor.Value()->WriteSettingsKey(writer, entry);
            writer.WriteDigest(hash.Value());
            auto local = writer.Finish();
            if (!local) {
                return Result<void>::Failure(local.Error());
            }

            localKeys_[at] = local.Value();
        }

        return Result<void>::Success();
    }

    // kind 간선(대상이 노드 limit 미만인 것만)으로 CSR을 만든다.
    Result<void> BuildGraph(PackageDependencyKind kind, usize limit, Array<uint32>& first,
                            Array<uint32>& targets) const noexcept {
        if (auto sized = first.TryResize(limit + 1); !sized) {
            return sized;
        }

        for (usize at = 0; at < limit; ++at) {
            first[at] = static_cast<uint32>(targets.Size());
            for (const PackageDependencyEntry& dependency : nodes_[at]->dependencies) {
                const uint32* node =
                    dependency.kind == kind ? map_.Find(dependency.asset) : nullptr;
                if (node == nullptr || *node >= limit) {
                    continue;
                }

                if (auto pushed = targets.TryPushBack(*node); !pushed) {
                    return pushed;
                }
            }
        }

        first[limit] = static_cast<uint32>(targets.Size());
        return Result<void>::Success();
    }

    // 묶음 하나의 키를 구한다. 구성원·밖 의존 중 하나라도 실패면 모두 실패(진단 없음 — 원인에
    // 이미 있다). local·keys·failed는 노드 번호로 읽고 쓴다.
    Result<void> ComponentKeys(Span<const uint32> members, Span<const uint32> first,
                               Span<const uint32> targets, StringView role,
                               const Array<Sha256Digest>& local, Array<Sha256Digest>& keys,
                               Array<uint8>& failed, uint32 stamp) noexcept {
        bool broken = false;
        for (uint32 member : members) {
            component_[member] = stamp;
            broken = broken || failed[member] != 0;
        }

        Array<NuggetSccMember> sccMembers(allocator_);
        Array<NuggetSccDependency> external(allocator_);
        for (uint32 member : members) {
            for (uint32 edge = first[member]; edge < first[member + 1]; ++edge) {
                const uint32 dependency = targets[edge];
                if (component_[dependency] == stamp) {
                    continue;
                }

                broken = broken || failed[dependency] != 0;
                if (auto pushed = external.TryPushBack(NuggetSccDependency{
                        nodes_[member]->id, role, nodes_[dependency]->id, keys[dependency]});
                    !pushed) {
                    return pushed;
                }
            }

            if (auto pushed =
                    sccMembers.TryPushBack(NuggetSccMember{nodes_[member]->id, local[member]});
                !pushed) {
                return pushed;
            }
        }

        if (broken) {
            for (uint32 member : members) {
                failed[member] = 1;
            }

            return Result<void>::Success();
        }

        auto sccKey = NuggetSccKey(allocator_, sccMembers.View(), external.View());
        if (!sccKey) {
            return Result<void>::Failure(sccKey.Error());
        }

        for (uint32 member : members) {
            keys[member] = NuggetMemberKey(sccKey.Value(), nodes_[member]->id);
        }

        return Result<void>::Success();
    }

    Result<void> ComputeBuildKeys() noexcept {
        const usize count = nodes_.Size();
        Array<uint32> first(allocator_);
        Array<uint32> targets(allocator_);
        if (auto built = BuildGraph(PackageDependencyKind::Build, count, first, targets); !built) {
            return built;
        }

        // 묶음 표지는 노드마다 하나이고 번호는 Build·Hard 두 단계에서 이어 쓴다(다시 비우지
        // 않는다).
        if (auto sized = component_.TryResize(count); !sized) {
            return sized;
        }

        for (uint32& stamp : component_) {
            stamp = NoComponent;
        }

        return algo::ForEachStrongComponent(
            allocator_, static_cast<uint32>(count), first.View(), targets.View(),
            [&](Span<const uint32> members, bool) noexcept {
                return ComponentKeys(members, first.View(), targets.View(), CookBuildRole,
                                     localKeys_, buildKeys_, failed_, stamp_++);
            });
    }

    // 대상 하나의 cookKey 지역 입력이다. 문자열은 모두 유효한 UTF-8이고(경로 키는 LogicalPath,
    // bundle 이름은 PackageTags reader가 검증) 해시 기록기는 할당하지 않으므로 실패하지 않는다.
    Sha256Digest CookLocalKey(usize at) const noexcept {
        const CookTarget& target = request_.targets->targets[at];
        const AssetIndexEntry& entry = *nodes_[at];
        NuggetKeyWriter writer;
        WriteNuggetKeyHeader(writer, NuggetProcessorId{CookerProcessorGuid, CookerKeyVersion},
                             request_.projectGeneration);
        writer.WriteU32(request_.cookerVersion);
        writer.WriteDigest(contentHashes_[at]);
        writer.WriteString(entry.key.View());
        writer.WriteBool(target.primary);
        writer.WriteList(target.bundles.Size());
        for (const CookTargetBundle& bundle : target.bundles) {
            writer.WriteString(bundle.name.View());
            writer.WriteList(bundle.members.Size());
            for (AssetId member : bundle.members) {
                writer.WriteGuid(member.value);
            }
        }

        // 이 대상의 제외된 Soft 의존(저장 순서 = AssetId 순)이다.
        const Array<AssetId>& excluded = request_.targets->excluded;
        uint64 count = 0;
        for (const PackageDependencyEntry& dependency : entry.dependencies) {
            count += dependency.kind == PackageDependencyKind::Soft &&
                             IsExcluded(excluded, dependency.asset)
                         ? 1
                         : 0;
        }

        writer.WriteList(count);
        for (const PackageDependencyEntry& dependency : entry.dependencies) {
            if (dependency.kind == PackageDependencyKind::Soft &&
                IsExcluded(excluded, dependency.asset)) {
                writer.WriteGuid(dependency.asset.value);
            }
        }

        writer.WriteDigest(buildKeys_[at]);
        writer.WriteU32(PackageFormatVersion);
        writer.WriteU32(PackageSectionSchemaVersion);
        writer.WriteDigest(out_.typeTable);
        writer.WriteDigest(request_.profileSettings);
        // export 클래스 기본값(A5c): 타입마다 TypeId·유무 상태(0 = 레코드 있음, 1 = 레코드 없는
        // 타입)·cooked 지문(있을 때만). 표에 없거나 실패한 타입은 CheckClassDefaults가 먼저 걸렀다.
        writer.WriteList(entry.exportTypes.Size());
        for (TypeId type : entry.exportTypes) {
            const CookClassDefaults* row = defaults_->Find(type);
            ROCKS_ASSERT(row != nullptr);
            writer.WriteGuid(type);
            const bool present = row->availability == ClassDefaultsAvailability::Present;
            writer.WriteU32(present ? 0 : 1);
            if (present) {
                writer.WriteDigest(row->cooked);
            }
        }

        auto key = writer.Finish();
        ROCKS_ASSERT(key);
        return key.Value();
    }

    // 대상의 export 클래스가 모두 기본값 표에 있고 실패가 아닌지 본다(아니면 진단 — 미등록
    // UnsupportedVersion·레코드 실패 InvalidClassDefaults, 그 대상은 cookKey가 없다).
    bool CheckClassDefaults(const AssetIndexEntry& entry) noexcept {
        bool usable = true;
        for (TypeId type : entry.exportTypes) {
            const CookClassDefaults* row = defaults_->Find(type);
            if (row == nullptr) {
                Diagnose(AssetDiagnosticCode::UnsupportedVersion, entry.id, AssetId{type},
                         ErrorCode::NotFound);
                usable = false;
            } else if (row->availability == ClassDefaultsAvailability::Failed) {
                Diagnose(AssetDiagnosticCode::InvalidClassDefaults, entry.id, AssetId{type},
                         row->diagnostic.error);
                usable = false;
            }
        }

        return usable;
    }

    static bool IsExcluded(const Array<AssetId>& excluded, AssetId id) noexcept {
        return algo::BinarySearch(excluded.View(), id, [](AssetId left, AssetId right) noexcept {
                   return GuidLess(left.value, right.value);
               }) != algo::NotFound;
    }

    Result<void> ComputeCookKeys() noexcept {
        const usize count = request_.targets->targets.Size();
        if (auto sized = cookFailed_.TryResize(count); !sized) {
            return sized;
        }

        if (auto sized = cookLocal_.TryResize(count); !sized) {
            return sized;
        }

        if (auto sized = cookKeys_.TryResize(count); !sized) {
            return sized;
        }

        for (usize at = 0; at < count; ++at) {
            cookFailed_[at] = failed_[at];
            if (cookFailed_[at] == 0 && !CheckClassDefaults(*nodes_[at])) {
                cookFailed_[at] = 1;
            }

            if (cookFailed_[at] != 0) {
                continue;
            }

            cookLocal_[at] = CookLocalKey(at);
        }

        Array<uint32> first(allocator_);
        Array<uint32> targets(allocator_);
        if (auto built = BuildGraph(PackageDependencyKind::Hard, count, first, targets); !built) {
            return built;
        }

        return algo::ForEachStrongComponent(
            allocator_, static_cast<uint32>(count), first.View(), targets.View(),
            [&](Span<const uint32> members, bool) noexcept {
                return ComponentKeys(members, first.View(), targets.View(), CookHardRole,
                                     cookLocal_, cookKeys_, cookFailed_, stamp_++);
            });
    }

    Allocator& allocator_;
    const CookKeyRequest& request_;
    CookKeySet& out_;
    Array<const AssetIndexEntry*> nodes_;
    NodeMap map_;
    // 노드마다(앞 대상 수만큼은 대상과 같은 번호) Build 키 실패·콘텐츠 해시·지역·build key다.
    Array<uint8> failed_;
    Array<Sha256Digest> contentHashes_;
    Array<Sha256Digest> localKeys_;
    Array<Sha256Digest> buildKeys_;
    // 대상마다 cookKey 실패·지역·cookKey다.
    Array<uint8> cookFailed_;
    Array<Sha256Digest> cookLocal_;
    Array<Sha256Digest> cookKeys_;
    // SCC 구성원 표지(묶음 번호)와 다음 번호다.
    Array<uint32> component_;
    uint32 stamp_ = 0;
    ErrorCode error_ = ErrorCode::None;
    // 클래스 기본값 표다(요청의 것, 없으면 ownDefaults_).
    const CookClassDefaultsTable* defaults_ = nullptr;
    CookClassDefaultsTable ownDefaults_;
};

// ForEachClassDefaults 방문자다: 레코드 유무로 행을 더하고 일시 실패만 오류로 돌려준다.
Result<void> AddCookClassDefaults(void* user, TypeId type, const ClassDefaults* defaults,
                                  const ClassDefaultsDiagnostic& diagnostic) noexcept {
    auto& table = *static_cast<CookClassDefaultsTable*>(user);
    CookClassDefaults row;
    row.type = type;
    row.diagnostic = diagnostic;
    if (defaults != nullptr) {
        row.availability = ClassDefaultsAvailability::Present;
        row.cooked = defaults->Fingerprint(ClassDefaultsConfiguration::Cooked);
    } else if (!IsRememberedClassDefaultsFailure(diagnostic)) {
        return Result<void>::Failure(diagnostic.error != ErrorCode::None ? diagnostic.error
                                                                         : ErrorCode::OutOfMemory);
    } else {
        row.availability = IsClassWithoutDefaults(diagnostic.reason)
                               ? ClassDefaultsAvailability::WithoutRecord
                               : ClassDefaultsAvailability::Failed;
    }

    return table.rows.TryPushBack(row);
}

} // namespace

const CookClassDefaults* CookClassDefaultsTable::Find(TypeId type) const noexcept {
    const usize at =
        algo::LowerBound(rows.View(), type, [](const CookClassDefaults& row, TypeId key) noexcept {
            return GuidLess(row.type, key);
        });
    return at < rows.Size() && rows[at].type == type ? &rows[at] : nullptr;
}

Result<void> BuildCookClassDefaults(Allocator& allocator, TypeRegistry& types,
                                    CookClassDefaultsTable& out) noexcept {
    out.rows.Clear();
    if (auto visited = ForEachClassDefaults(types, allocator, &AddCookClassDefaults, &out);
        !visited) {
        out.rows.Clear();
        return visited;
    }

    // 표 찾기는 GuidLess 이진 탐색이다(ForEachClassDefaults 순서에 기대지 않는다).
    algo::Sort(out.rows.View(),
               [](const CookClassDefaults& left, const CookClassDefaults& right) noexcept {
                   return GuidLess(left.type, right.type);
               });
    return Result<void>::Success();
}

Result<Sha256Digest> HashTypeTable(Allocator& allocator, const TypeRegistry& types) noexcept {
    Array<TypeVersionEntry> list(allocator);
    if (auto listed = types.ListTypeVersions(list); !listed) {
        return Result<Sha256Digest>::Failure(listed.Error());
    }

    NuggetKeyWriter writer;
    writer.WriteList(list.Size());
    for (const TypeVersionEntry& entry : list) {
        writer.WriteGuid(entry.type);
        writer.WriteU32(static_cast<uint32>(entry.kind));
        writer.WriteU32(entry.version);
    }

    return writer.Finish();
}

Result<void> ComputeCookKeys(Allocator& allocator, const CookKeyRequest& request,
                             CookKeySet& out) noexcept {
    out.keys.Clear();
    out.diagnostics.Clear();
    out.typeTable = Sha256Digest{};
    if (request.index == nullptr || request.files == nullptr || request.types == nullptr ||
        request.processors == nullptr || request.targets == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    KeyWalk walk(allocator, request, out);
    return walk.Run();
}

} // namespace rocks
