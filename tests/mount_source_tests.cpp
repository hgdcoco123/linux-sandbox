#include "sandbox/mount_source.hpp"
#include <iostream>
using namespace sandbox;
void require(bool ok) {
    if (!ok)
        throw std::runtime_error("mount source test failed");
}
template <class F> void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (...) {
        rejected = true;
    }
    require(rejected);
}
int main() {
    try {
        char pattern[] = "/tmp/sbx-mount-source-XXXXXX";
        auto temp = ::mkdtemp(pattern);
        if (!temp)
            system_error("mkdtemp");
        fs::path root = temp;
        try {
            auto original = root / "original";
            fs::create_directory(original);
            auto pinned = open_path(original, O_PATH | O_DIRECTORY);
            {
                auto local = reopen_mount_source(pinned.get(), original);
                require(local.get() != pinned.get());
            }
            fs::rename(original, root / "moved");
            fs::create_directory(original);
            rejects([&] { reopen_mount_source(pinned.get(), original); });
            {
                auto local = reopen_mount_source(pinned.get(), root / "moved");
                require(static_cast<bool>(local));
            }
            fs::create_directory_symlink(root / "moved", root / "link");
            rejects([&] { reopen_mount_source(pinned.get(), root / "link"); });
            rejects([&] { reopen_mount_source(pinned.get(), root / "link" / "."); });
            rejects([&] { reopen_mount_source(pinned.get(), "relative"); });
        } catch (...) {
            fs::remove_all(root);
            throw;
        }
        fs::remove_all(root);
        std::cout << "mount source identity and symlink tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
