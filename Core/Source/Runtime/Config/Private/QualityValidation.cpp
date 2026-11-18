#include <Config/QualityValidation.h>

#include <Core/Text/StringBuilder.h>

namespace rocks::config {

namespace {

bool LessBytes(StringView left, StringView right) noexcept {
    const usize count = left.ByteCount() < right.ByteCount() ? left.ByteCount() : right.ByteCount();
    for (usize index = 0; index < count; ++index) {
        const auto a = static_cast<uint8>(left.Data()[index]);
        const auto b = static_cast<uint8>(right.Data()[index]);
        if (a != b) {
            return a < b;
        }
    }

    return left.ByteCount() < right.ByteCount();
}

// 요청 하나의 계산 상태다. rule이 null이면 규칙 없는 묶음이다. position은 rule->levels 안의 펼칠
// 등급 위치다.
struct Plan {
    const QualityGroupRule* rule = nullptr;
    usize request = 0;
    usize position = 0;
};

const QualityLevelRule* LevelRule(const QualityGroupRule& rule, int32 level) noexcept {
    for (const QualityLevelRule& candidate : rule.rules) {
        if (candidate.level == level) {
            return &candidate;
        }
    }

    return nullptr;
}

double CostOf(const QualityGroupRule& rule, usize position, StringView budget) noexcept {
    const QualityLevelRule* level = LevelRule(rule, rule.levels[position]);
    if (level == nullptr) {
        return 0;
    }

    double total = 0;
    for (const QualityCost& cost : level->costs) {
        total += cost.budget == budget ? cost.amount : 0;
    }

    return total;
}

// reason에 why를 덧붙인다(`; `로 잇는다).
Result<void> AppendReason(String& reason, StringView why) noexcept {
    if (!reason.IsEmpty()) {
        if (auto appended = reason.TryAppend("; "); !appended) {
            return appended;
        }
    }

    return reason.TryAppend(why);
}

} // namespace

Result<void> ResolveQualityLevels(Span<const QualityGroupRule> groups,
                                  Span<const QualityBudgetRule> budgets,
                                  Span<const QualityRequest> requests, const DeviceTraits* traits,
                                  QualityResolutionSet& out) noexcept {
    out.groups.Clear();
    out.unmetBudgets.Clear();
    Allocator& allocator = out.groups.GetAllocator();
    auto fail = [&](ErrorCode error) noexcept {
        out.groups.Clear();
        out.unmetBudgets.Clear();
        return Result<void>::Failure(error);
    };

    // 1. 요청을 결과와 계산 상태로 옮긴다.
    Array<Plan> plans(allocator);
    for (usize index = 0; index < requests.Size(); ++index) {
        const QualityRequest& request = requests[index];
        for (usize earlier = 0; earlier < index; ++earlier) {
            if (requests[earlier].group == request.group) {
                return fail(ErrorCode::InvalidArgument);
            }
        }

        auto made = UniquePtr<QualityResolution>::TryMake(allocator, allocator);
        if (!made || !made.Value()->group.TryAssign(request.group)) {
            return fail(ErrorCode::OutOfMemory);
        }

        made.Value()->requested = request.level;
        made.Value()->applied = request.level;
        if (!out.groups.TryPushBack(Move(made.Value()))) {
            return fail(ErrorCode::OutOfMemory);
        }

        const QualityGroupRule* rule = nullptr;
        for (const QualityGroupRule& candidate : groups) {
            rule = candidate.group == request.group ? &candidate : rule;
        }

        if (rule == nullptr) {
            continue;
        }

        Plan plan;
        plan.rule = rule;
        plan.request = index;
        plan.position = IndexNone;
        for (usize position = 0; position < rule->levels.Size(); ++position) {
            plan.position = rule->levels[position] == request.level ? position : plan.position;
        }

        if (plan.position == IndexNone) {
            return fail(ErrorCode::InvalidArgument);
        }

        if (!plans.TryPushBack(plan)) {
            return fail(ErrorCode::OutOfMemory);
        }
    }

    if (traits == nullptr) {
        return Result<void>::Success();
    }

    // 낮춤 순서: 우선순위 오름차순, 같으면 묶음 이름 순서.
    plans.Sort([](const Plan& left, const Plan& right) noexcept {
        return left.rule->priority != right.rule->priority
                   ? left.rule->priority < right.rule->priority
                   : LessBytes(left.rule->group, right.rule->group);
    });

    String why(allocator);
    auto lower = [&](Plan& plan, StringView because) noexcept -> Result<void> {
        QualityResolution& resolution = *out.groups[plan.request];
        --plan.position;
        resolution.applied = plan.rule->levels[plan.position];
        return AppendReason(resolution.reason, because);
    };

    // 2. 등급 요구.
    for (Plan& plan : plans) {
        for (;;) {
            const QualityLevelRule* level = LevelRule(*plan.rule, plan.rule->levels[plan.position]);
            if (level == nullptr) {
                break;
            }

            auto matched = MatchDeviceConditions(*traits, level->requirements, why);
            if (!matched) {
                return fail(matched.Error());
            }

            if (matched.Value()) {
                break;
            }

            if (plan.position == 0) {
                QualityResolution& resolution = *out.groups[plan.request];
                resolution.unmet = true;
                if (!AppendReason(resolution.reason, why.View())) {
                    return fail(ErrorCode::OutOfMemory);
                }

                break;
            }

            if (!lower(plan, why.View())) {
                return fail(ErrorCode::OutOfMemory);
            }
        }
    }

    // 3. 예산.
    for (const QualityBudgetRule& budget : budgets) {
        DeviceTrait trait;
        if (!traits->Find(budget.trait, trait) || !trait.numeric) {
            continue;
        }

        const double limit = trait.number * budget.fraction;
        for (;;) {
            double total = 0;
            for (const Plan& plan : plans) {
                total += CostOf(*plan.rule, plan.position, budget.name);
            }

            if (total <= limit) {
                break;
            }

            StringBuilder<192> text(allocator);
            static_cast<void>(text.TryAppendFormat("budget {} needs {} but the device allows {}",
                                                   budget.name, total, limit));
            if (!text.Status()) {
                return fail(ErrorCode::OutOfMemory);
            }

            Plan* victim = nullptr;
            for (Plan& plan : plans) {
                if (victim == nullptr && plan.position != 0 &&
                    CostOf(*plan.rule, plan.position - 1, budget.name) <
                        CostOf(*plan.rule, plan.position, budget.name)) {
                    victim = &plan;
                }
            }

            if (victim == nullptr) {
                String note(allocator);
                if (!note.TryAssign(text.View()) || !out.unmetBudgets.TryPushBack(Move(note))) {
                    return fail(ErrorCode::OutOfMemory);
                }

                break;
            }

            if (!lower(*victim, text.View())) {
                return fail(ErrorCode::OutOfMemory);
            }
        }
    }

    return Result<void>::Success();
}

Result<void> FormatQualityResolution(const QualityResolution& resolution, String& out) noexcept {
    StringBuilder<192> text(out.GetAllocator());
    static_cast<void>(text.TryAppendFormat("{}@{}", resolution.group.View(), resolution.applied));
    if (resolution.unmet) {
        static_cast<void>(text.TryAppendFormat(" (requested {}, unmet: {})", resolution.requested,
                                               resolution.reason.View()));
    } else if (resolution.applied != resolution.requested) {
        static_cast<void>(text.TryAppendFormat(" (requested {}, lowered: {})", resolution.requested,
                                               resolution.reason.View()));
    }

    if (auto status = text.Status(); !status) {
        return status;
    }

    return out.TryAssign(text.View());
}

} // namespace rocks::config
