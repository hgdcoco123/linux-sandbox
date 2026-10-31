// Portable tests for the workspace boundary: budgets, structural refusals and
// the precedence between them.
#include "sandbox/core/workspace.hpp"

#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace sandbox;

namespace {
int failures = 0;

void check(bool condition, const char *what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

EntryFacts file(std::uint64_t size, std::uint8_t depth = 0) {
    EntryFacts entry;
    entry.size = size;
    entry.depth = depth;
    entry.regular = true;
    return entry;
}

WorkspacePolicy policy() {
    WorkspacePolicy value;
    value.max_bytes = 1000;
    value.max_entries = 3;
    value.max_depth = 2;
    return value;
}

void test_budget_boundaries() {
    auto p = policy();

    auto first = admit(p, 0, 0, file(400));
    check(first.accepted(), "a regular file inside all budgets is accepted");
    check(first.bytes_after == 400 && first.entries_after == 1, "counters advance");

    // Exactly at the limit is still acceptable; one byte more is not.
    auto exact = admit(p, 600, 0, file(400));
    check(exact.accepted() && exact.bytes_after == 1000, "exactly at max_bytes is accepted");
    auto over = admit(p, 601, 0, file(400));
    check(over.rejection == Rejection::byte_limit, "one byte over max_bytes is refused");
    check(over.bytes_after == p.max_bytes, "the reported total clamps instead of wrapping");

    auto last_entry = admit(p, 0, 2, file(1));
    check(last_entry.accepted() && last_entry.entries_after == 3, "last entry slot is usable");
    auto no_room = admit(p, 0, 3, file(1));
    check(no_room.rejection == Rejection::entry_limit, "entry budget is enforced");
    check(no_room.entries_after == p.max_entries, "entry total clamps");

    auto deep = admit(p, 0, 0, file(1, 3));
    check(deep.rejection == Rejection::depth_limit, "depth budget is enforced");
    check(admit(p, 0, 0, file(1, 2)).accepted(), "depth at the limit is accepted");
}

void test_structural_refusals() {
    auto p = policy();

    EntryFacts symlink;
    symlink.symlink = true;
    check(admit(p, 0, 0, symlink).rejection == Rejection::symlink, "symlinks are refused");

    EntryFacts hardlink = file(10);
    hardlink.hardlink = true;
    check(admit(p, 0, 0, hardlink).rejection == Rejection::hardlink, "hardlinks are refused");

    EntryFacts directory;
    check(admit(p, 0, 0, directory).rejection == Rejection::not_regular,
          "non-regular entries are refused");

    EntryFacts escape = file(10);
    escape.escapes_root = true;
    escape.symlink = true;
    check(admit(p, 0, 0, escape).rejection == Rejection::escapes_root,
          "escaping the root outranks the symlink reason");

    // A structural refusal must not be reported as a budget problem even when the
    // budgets are also exhausted.
    EntryFacts both = file(9999, 9);
    both.symlink = true;
    check(admit(p, 1000, 3, both).rejection == Rejection::symlink,
          "structure is reported before budget");
}

void test_no_overflow() {
    auto p = policy();
    // A corrupt or hostile counter must not wrap around into "plenty of room".
    const auto huge = std::numeric_limits<std::uint64_t>::max();
    auto result = admit(p, huge, 0, file(1));
    check(result.rejection == Rejection::byte_limit, "an overflowed counter does not admit");
    check(result.bytes_after == p.max_bytes, "reported total stays bounded");
}

void test_policy_validation() {
    auto p = policy();
    p.validate(); // defaults are valid

    auto zero_bytes = p;
    zero_bytes.max_bytes = 0;
    bool rejected = false;
    try {
        zero_bytes.validate();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "zero byte budget is rejected");

    auto zero_entries = p;
    zero_entries.max_entries = 0;
    rejected = false;
    try {
        zero_entries.validate();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "zero entry budget is rejected");

    auto zero_depth = p;
    zero_depth.max_depth = 0;
    rejected = false;
    try {
        zero_depth.validate();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "zero depth budget is rejected");

    auto unbounded_snapshot = p;
    unbounded_snapshot.input = InputMode::snapshot_copy;
    unbounded_snapshot.max_bytes = std::numeric_limits<std::uint64_t>::max();
    rejected = false;
    try {
        unbounded_snapshot.validate();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "an unbounded snapshot is rejected");
}
// Deterministic property check: whatever the policy and the entry, an accepted
// admission must stay inside every budget, and a rejection must name a reason.
void test_properties() {
    std::uint64_t state = 0x9e3779b97f4a7c15ull;
    auto next = [&state](std::uint64_t limit) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return limit == 0 ? 0 : (state >> 33) % limit;
    };
    for (int iteration = 0; iteration < 2000; ++iteration) {
        WorkspacePolicy p;
        p.max_bytes = 1 + next(5000);
        p.max_entries = static_cast<std::uint32_t>(1 + next(50));
        p.max_depth = static_cast<std::uint8_t>(1 + next(8));

        EntryFacts entry;
        entry.size = next(4000);
        entry.depth = static_cast<std::uint8_t>(next(10));
        const auto kind = next(6);
        entry.regular = kind == 0;
        entry.symlink = kind == 1;
        entry.hardlink = kind == 2;
        entry.escapes_root = kind == 3;

        const auto bytes_used = next(p.max_bytes + 1);
        const auto entries_used = static_cast<std::uint32_t>(next(p.max_entries + 1));
        const auto admission = admit(p, bytes_used, entries_used, entry);

        const bool structural =
            entry.symlink || entry.hardlink || !entry.regular || entry.escapes_root;
        if (admission.accepted()) {
            check(!structural, "an accepted entry is a plain file inside the root");
            check(entry.depth <= p.max_depth, "an accepted entry is within the depth budget");
            check(entry.size <= p.max_bytes - bytes_used, "an accepted entry fits the byte budget");
            check(entries_used < p.max_entries, "an accepted entry has an entry slot");
        }
        check(admission.bytes_after <= p.max_bytes, "reported bytes stay inside the budget");
        check(admission.entries_after <= p.max_entries, "reported count stays inside the budget");
    }
}
} // namespace

int main() {
    test_budget_boundaries();
    test_structural_refusals();
    test_no_overflow();
    test_policy_validation();
    test_properties();
    if (failures != 0) {
        std::printf("%d workspace check(s) failed\n", failures);
        return 1;
    }
    std::printf("workspace boundary budgets and structural refusals passed\n");
    return 0;
}
