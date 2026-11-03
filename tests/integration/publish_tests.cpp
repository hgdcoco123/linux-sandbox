// Tests for the descriptor-based workspace copy.
//
// publish_tree is the half of the transfer that needs no namespaces: two directory
// descriptors and a name. That is what lets it be checked here - staging, atomic
// publish, refusal of structural surprises and the guarantee that a failed copy
// never replaces a previous one - while the isolated suite covers the part that
// needs a delegated cgroup.
#include "sandbox/storage/artifacts.hpp"
#include "sandbox/platform/linux.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace sandbox;
namespace fs = std::filesystem;

namespace {
int failures = 0;

void check(bool condition, const char *what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

int open_directory(const fs::path &path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        std::runtime_error("open " + path.string());
    return fd;
}

void write_file(const fs::path &path, const char *text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool throws(const std::function<void()> &body, const char *expected) {
    try {
        body();
    } catch (const std::exception &failure) {
        return std::string(failure.what()).find(expected) != std::string::npos;
    }
    return false;
}

bool throws_any(const std::function<void()> &body) {
    try {
        body();
    } catch (const std::exception &) {
        return true;
    }
    return false;
}

struct Scratch {
    fs::path root;
    Scratch() {
        char pattern[] = "/tmp/sbx-publish-XXXXXX";
        char *created = ::mkdtemp(pattern);
        if (!created)
            throw std::runtime_error("mkdtemp");
        root = created;
        fs::create_directories(root / "source" / "nested");
        fs::create_directories(root / "dest");
        write_file(root / "source" / "top.txt", "top");
        write_file(root / "source" / "nested" / "deep.txt", "deep");
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    fs::path published() const {
        return root / "dest" / "copy";
    }
};

void test_publishes_the_whole_tree() {
    Scratch scratch;
    WorkspacePolicy policy;
    int source = open_directory(scratch.root / "source");
    int dest = open_directory(scratch.root / "dest");
    auto result = publish_tree(source, dest, "copy", policy);
    ::close(source);
    ::close(dest);
    check(read_file(scratch.published() / "top.txt") == "top", "top level file copied");
    check(read_file(scratch.published() / "nested" / "deep.txt") == "deep",
          "nested file copied");
    check(result.value("complete", false), "the copy reports completion");
    check(result.at("entries").size() == 3, "manifest lists directory and both files");
    // Staging is renamed into place, so nothing of it is left behind.
    for (const auto &entry : fs::directory_iterator(scratch.root / "dest")) {
        check(entry.path().filename().string().rfind(".sbx-copy-", 0) != 0,
              "no staging directory is left behind");
    }
}

void test_refuses_to_replace() {
    Scratch scratch;
    WorkspacePolicy policy;
    int source = open_directory(scratch.root / "source");
    int dest = open_directory(scratch.root / "dest");
    publish_tree(source, dest, "copy", policy);
    check(throws([&] { publish_tree(source, dest, "copy", policy); }, "already exists"),
          "publishing over an existing copy is refused");
    // The refusal must leave the first copy intact.
    check(read_file(scratch.published() / "top.txt") == "top",
          "a refused publish keeps the previous copy");
    ::close(source);
    ::close(dest);
}

void test_refuses_a_symlink() {
    Scratch scratch;
    WorkspacePolicy policy;
    fs::create_symlink("/etc/hostname", scratch.root / "source" / "link");
    int source = open_directory(scratch.root / "source");
    int dest = open_directory(scratch.root / "dest");
    check(throws([&] { publish_tree(source, dest, "copy", policy); }, "artifact refused"),
          "a symlink in the tree is refused");
    check(!fs::exists(scratch.published()), "a refused copy publishes nothing");
    ::close(source);
    ::close(dest);
}

void test_refuses_a_hardlink() {
    Scratch scratch;
    WorkspacePolicy policy;
    fs::create_hard_link(scratch.root / "source" / "top.txt", scratch.root / "source" / "alias.txt");
    int source = open_directory(scratch.root / "source");
    int dest = open_directory(scratch.root / "dest");
    check(throws([&] { publish_tree(source, dest, "copy", policy); }, "artifact refused"),
          "a hardlinked file in the tree is refused");
    ::close(source);
    ::close(dest);
}

void test_callback_cannot_poison_the_copy() {
    Scratch scratch;
    WorkspacePolicy policy;
    int source = open_directory(scratch.root / "source");
    int dest = open_directory(scratch.root / "dest");
    // A callback that talks to a socket leaves EAGAIN in errno; the copy must not
    // report that as a directory read failure.
    auto chatty = [] {
        errno = EAGAIN;
        return false;
    };
    publish_tree(source, dest, "copy", policy, chatty);
    check(read_file(scratch.published() / "top.txt") == "top",
          "a callback's errno does not corrupt the copy");
    ::close(source);
    ::close(dest);
}

void test_interrupt_leaves_nothing_published() {
    Scratch scratch;
    WorkspacePolicy policy;
    int source = open_directory(scratch.root / "source");
    int dest = open_directory(scratch.root / "dest");
    int seen = 0;
    auto interrupted = [&seen] {
        // Stop after the first entry: the point is that an interrupted copy publishes
        // nothing, not that it copies slowly.
        return ++seen > 1;
    };
    check(throws_any([&] { publish_tree(source, dest, "copy", policy, interrupted); }),
          "an interrupted copy aborts instead of completing");
    check(!fs::exists(scratch.published()), "an interrupted copy publishes nothing");
    ::close(source);
    ::close(dest);
}

void test_budget_applies() {
    Scratch scratch;
    WorkspacePolicy policy;
    policy.max_entries = 2;
    int source = open_directory(scratch.root / "source");
    int dest = open_directory(scratch.root / "dest");
    check(throws([&] { publish_tree(source, dest, "copy", policy); }, "artifact refused"),
          "the entry budget is enforced by the copier");
    check(!fs::exists(scratch.published()), "a copy that exceeds the budget publishes nothing");
    ::close(source);
    ::close(dest);
}

// --- publish_tree_over: the swap plus the three facts it reports -------------------

struct PublishPair {
    fs::path root, incoming, published;
    PublishPair() {
        char pattern[] = "/tmp/sbx-pubover-XXXXXX";
        char *created = ::mkdtemp(pattern);
        if (!created)
            throw std::runtime_error("mkdtemp");
        root = created;
        incoming = root / "incoming";
        published = root / "out";
        fs::create_directories(incoming);
        fs::create_directories(published);
        write_file(incoming / "new.txt", "new");
        write_file(published / "old.txt", "old");
    }
    ~PublishPair() {
        std::error_code ignored;
        fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace, ignored);
        fs::remove_all(published, ignored);
        fs::remove_all(incoming, ignored);
        fs::remove_all(root, ignored);
    }
};

void test_publish_over_swaps_and_reclaims() {
    PublishPair pair;
    auto result = publish_tree_over(pair.incoming, pair.published, WorkspacePolicy{});
    check(result.published, "a completed exchange reports the tree as published");
    check(result.durability_confirmed, "a completed exchange is synced");
    check(result.durability_error.empty(), "a synced exchange reports no durability error");
    check(result.cleanup_error.empty(), "the previous copy is reclaimed");
    check(read_file(pair.published / "new.txt") == "new", "the new tree is the visible one");
    check(!fs::exists(pair.published / "old.txt"), "the previous tree is gone from the destination");
    check(!fs::exists(pair.incoming), "a confirmed publish reclaims the previous copy");
}

void test_publish_over_missing_destination_throws() {
    PublishPair pair;
    check(throws([&] { publish_tree_over(pair.incoming, pair.published / "absent",
                                         WorkspacePolicy{}); },
                 "publish:"),
          "publishing over a destination that does not exist throws");
    check(read_file(pair.incoming / "new.txt") == "new", "a failed publish changes nothing");
}

void test_publish_over_reports_unconfirmed_durability() {
    PublishPair pair;
    // What this forces is "the sync cannot open the parent", not an fsync() I/O error:
    // an unprivileged test cannot make fsync fail on a healthy filesystem. The branch
    // under test (swap done, durability unconfirmed) is the same; the kernel-level I/O
    // error path is not covered.
    std::error_code ignored;
    fs::permissions(pair.root, fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace, ignored);
    auto result = publish_tree_over(pair.incoming, pair.published, WorkspacePolicy{});
    fs::permissions(pair.root, fs::perms::owner_all, fs::perm_options::replace, ignored);
    check(result.published, "the swap is still reported as published");
    check(!result.durability_confirmed, "an unopenable parent leaves durability unconfirmed");
    check(!result.durability_error.empty(), "the reason durability is unconfirmed is reported");
    check(result.cleanup_error.empty(), "an unconfirmed sync is not reported as a cleanup failure");
    check(read_file(pair.published / "new.txt") == "new", "the new tree is visible");
    check(fs::exists(pair.incoming / "old.txt"),
          "the previous copy is kept when durability is unconfirmed");
    check(result.retained_previous_path == pair.incoming,
          "the retained recovery point is reported by path");
}

void test_publish_over_reports_reclaim_failure() {
    PublishPair pair;
    // A directory the caller cannot empty: after the exchange it belongs to the previous
    // copy, so the reclaim fails while the publish itself succeeded.
    fs::create_directories(pair.published / "locked");
    write_file(pair.published / "locked" / "keep.txt", "keep");
    std::error_code ignored;
    fs::permissions(pair.published / "locked",
                    fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace,
                    ignored);
    auto result = publish_tree_over(pair.incoming, pair.published, WorkspacePolicy{});
    check(result.published && result.durability_confirmed,
          "a reclaim failure does not change what was published");
    check(!result.cleanup_error.empty(), "an unreclaimable previous copy is reported as cleanup");
    check(read_file(pair.published / "new.txt") == "new", "the publish itself still succeeded");
    check(fs::exists(pair.incoming / "locked" / "keep.txt"),
          "the copy that could not be reclaimed is left where it is");
}

void test_publish_over_across_filesystems() {
    // An unprivileged process cannot create a filesystem, so this uses the one tmpfs
    // that is (almost) always mounted apart from /tmp. If it shares the device, say so
    // instead of silently claiming coverage - the same-filesystem tests above then are
    // all the EXDEV branch gets.
    std::error_code ignored;
    auto shared = fs::path("/dev/shm");
    struct stat here {}, there {};
    if (::stat("/dev/shm", &there) != 0 || ::stat("/tmp", &here) != 0 ||
        there.st_dev == here.st_dev) {
        // Loud on purpose: this line is what an acceptance record has to quote, because
        // a passing test program here would otherwise be read as "the EXDEV branch ran".
        std::printf("cross-filesystem publish: NOT exercised (same device as /tmp); "
                    "the EXDEV path is unverified on this machine\n");
        return;
    }
    std::printf("cross-filesystem publish: exercised (EXDEV path, /dev/shm differs from /tmp)\n");
    PublishPair pair;
    auto shared_root = shared / ("sbx-pubover-" + std::to_string(::getpid()));
    fs::remove_all(shared_root, ignored);
    fs::create_directories(shared_root / "out");
    write_file(shared_root / "out" / "old.txt", "old");
    auto result = publish_tree_over(pair.incoming, shared_root / "out", WorkspacePolicy{});
    check(result.published, "a tree is published across filesystems");
    check(result.durability_confirmed, "the cross-filesystem swap is synced");
    check(result.cleanup_error.empty(), "the staged previous copy is reclaimed");
    check(read_file(shared_root / "out" / "new.txt") == "new", "the tree crossed filesystems");
    check(!fs::exists(shared_root / "out" / "old.txt"), "the previous copy is replaced");
    for (const auto &entry : fs::directory_iterator(shared_root))
        check(entry.path().filename().string().rfind(".sbx-publish-", 0) != 0,
              "no staging directory is left behind");
    fs::remove_all(shared_root, ignored);
}
// --- the directory transaction the prepare steps rely on ---------------------------

void test_directory_creation_is_all_or_nothing() {
    Scratch scratch;
    auto base = scratch.root / "layout";
    std::error_code ignored;
    fs::create_directories(base / "kept_dir");
    write_file(base / "kept_file", "not a directory");
    std::string message;
    bool threw = false;
    try {
        // First path is created by this call, the middle one already exists, the last
        // one cannot be created: exactly the case the prepare contract has to survive.
        create_directories_or_remove({base / "made", base / "kept_dir", base / "kept_file"});
    } catch (const std::exception &failure) {
        threw = true;
        message = failure.what();
    }
    check(threw, "a layout whose last path cannot be created throws");
    check(!fs::exists(base / "made"), "the directory this call created is removed again");
    check(fs::is_directory(base / "kept_dir"), "a pre-existing directory is kept");
    check(read_file(base / "kept_file") == "not a directory", "a pre-existing file is kept");
    check(message.find("create directory") != std::string::npos &&
              message.find((base / "kept_file").string()) != std::string::npos,
          "the original creation failure is reported, with the path that failed");
}

void test_transfer_layout_is_a_transaction() {
    Scratch scratch;
    // The real caller: a run directory whose inbox path is occupied must fail without
    // taking the pre-existing transfer directory with it.
    auto occupied = scratch.root / "occupied";
    fs::create_directories(occupied / "transfer");
    write_file(occupied / "transfer" / "in", "occupied");
    check(throws_any([&] { create_transfer_layout(occupied); }),
          "a transfer layout with an occupied inbox throws");
    check(fs::is_directory(occupied / "transfer"),
          "the pre-existing transfer directory is kept");
    check(read_file(occupied / "transfer" / "in") == "occupied",
          "the conflicting path is left untouched");

    auto fresh = scratch.root / "fresh";
    fs::create_directories(fresh);
    create_transfer_layout(fresh);
    check(fs::is_directory(fresh / "transfer" / "in"), "a fresh layout creates both directories");
    // A retried preparation reuses what is already there instead of failing on EEXIST.
    create_transfer_layout(fresh);
    check(fs::is_directory(fresh / "transfer" / "in"), "creating the layout twice is harmless");
}
void test_removal_keeps_going_after_a_failure() {
    Scratch scratch;
    std::error_code ignored;
    // A directory the test cannot empty: its removal fails, and the rule is that the
    // remaining paths are still attempted.
    auto stuck = scratch.root / "stuck";
    fs::create_directories(stuck / "locked");
    write_file(stuck / "locked" / "keep.txt", "keep");
    fs::permissions(stuck / "locked", fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace, ignored);
    auto plain = scratch.root / "plain";
    fs::create_directories(plain / "sub");
    write_file(plain / "sub" / "file.txt", "file");
    auto failures = remove_all_collect({stuck, plain, scratch.root / "absent", fs::path{}});
    fs::permissions(stuck / "locked", fs::perms::owner_all, fs::perm_options::replace, ignored);
    check(!failures.empty(), "a directory that cannot be emptied is reported as a failure");
    check(failures.find("stuck") != std::string::npos, "the failure names the path");
    check(!fs::exists(plain), "removal continues after the first failure");
    check(fs::exists(stuck / "locked" / "keep.txt"), "what could not be removed stays");
}
} // namespace

int main() {
    try {
        test_publishes_the_whole_tree();
        test_refuses_to_replace();
        test_refuses_a_symlink();
        test_refuses_a_hardlink();
        test_callback_cannot_poison_the_copy();
        test_interrupt_leaves_nothing_published();
        test_budget_applies();
        test_publish_over_swaps_and_reclaims();
        test_publish_over_missing_destination_throws();
        test_publish_over_reports_unconfirmed_durability();
        test_publish_over_reports_reclaim_failure();
        test_publish_over_across_filesystems();
        test_directory_creation_is_all_or_nothing();
        test_transfer_layout_is_a_transaction();
        test_removal_keeps_going_after_a_failure();
    } catch (const std::exception &failure) {
        std::printf("FAIL: unexpected exception: %s\n", failure.what());
        return 1;
    }
    if (failures != 0) {
        std::printf("%d publish check(s) failed\n", failures);
        return 1;
    }
    std::printf("descriptor-based workspace publishing passed\n");
    return 0;
}
