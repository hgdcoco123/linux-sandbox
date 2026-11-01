// A small trusted fixture. It is NOT part of the supervisor's control path.
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "echo";
    if (!std::strcmp(mode, "proc-view")) {
        char actual[64]{}, expected[64]{};
        auto count = ::readlink("/proc/self", actual, sizeof(actual) - 1);
        if (count < 0)
            return 51;
        actual[count] = '\0';
        std::snprintf(expected, sizeof(expected), "%ld", static_cast<long>(::getpid()));
        // A host proc bind would expose the host PID rather than this PID.
        if (std::strcmp(actual, expected))
            return 52;
        int status = ::open("/proc/1/status", O_RDONLY | O_CLOEXEC);
        if (status < 0)
            return 53;
        ::close(status);
        int old = ::open("/.oldroot/proc/self/status", O_RDONLY | O_CLOEXEC);
        if (old >= 0) {
            ::close(old);
            return 54;
        }
        std::puts("sandbox proc namespace and old-root detachment checks passed");
        return 0;
    }
    if (!std::strcmp(mode, "session-write")) {
        int fd = ::open("/output/value", O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
            return 41;
        bool ok = ::write(fd, "persistent", 10) == 10;
        ::close(fd);
        return ok ? 0 : 42;
    }
    if (!std::strcmp(mode, "session-read")) {
        int fd = ::open("/output/value", O_RDONLY);
        char text[16]{};
        if (fd < 0)
            return 43;
        auto n = ::read(fd, text, sizeof(text));
        ::close(fd);
        return n == 10 && !std::memcmp(text, "persistent", 10) ? 0 : 44;
    }
    if (!std::strcmp(mode, "session-copy-executable")) {
        int source = ::open("/proc/self/exe", O_RDONLY);
        int target = ::open("/output/copied-probe", O_WRONLY | O_CREAT | O_EXCL, 0700);
        if (source < 0 || target < 0)
            return 45;
        char data[65536];
        ssize_t n;
        while ((n = ::read(source, data, sizeof(data))) > 0) {
            ssize_t at = 0;
            while (at < n) {
                auto written = ::write(target, data + at, n - at);
                if (written <= 0)
                    return 46;
                at += written;
            }
        }
        if (n < 0)
            return 47;
        ::close(source);
        ::close(target);
        return 0;
    }
    if (!std::strcmp(mode, "echo")) {
        ::write(1, "hello\n", 6);
        ::write(2, "error\n", 6);
        return 0;
    }
    if (!std::strcmp(mode, "files")) {
        int input = ::open("/input/sample.txt", O_RDONLY);
        if (input < 0)
            return 21;
        ::close(input);
        input = ::open("/input/sample.txt", O_WRONLY);
        if (input >= 0)
            return 22;
        int output = ::open("/output/result.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (output < 0)
            return 23;
        if (::write(output, "converted\n", 10) != 10)
            return 24;
        ::close(output);
        if (::getpid() != 2)
            return 25;
        if (::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1)
            return 26;
        if (::fcntl(3, F_GETFD) >= 0)
            return 27;
        std::puts("file boundary checks passed");
        return 0;
    }
    if (!std::strcmp(mode, "sleep")) {
        for (;;)
            ::pause();
    }
    if (!std::strcmp(mode, "ignore-term")) {
        ::signal(SIGTERM, SIG_IGN);
        for (;;)
            ::pause();
    }
    if (!std::strcmp(mode, "orphan")) {
        pid_t p = ::fork();
        if (p < 0)
            return 31;
        if (p == 0) {
            ::setsid();
            ::signal(SIGTERM, SIG_IGN);
            for (;;)
                ::pause();
        }
        return 0;
    }
    if (!std::strcmp(mode, "flood")) {
        char b[4096];
        std::memset(b, 'x', sizeof(b));
        for (int i = 0; i < 100; ++i) {
            if (::write(1, b, sizeof(b)) < 0)
                return 32;
            if (::write(2, b, sizeof(b)) < 0)
                return 33;
        }
        return 0;
    }
    // Quota probes: the workspace is a tmpfs, so going over its byte or inode budget
    // fails the write with ENOSPC. Return 60 so the caller can tell "the quota
    // stopped me" from any other failure.
    if (!std::strcmp(mode, "fill-bytes")) {
        long long wanted = argc > 2 ? std::atoll(argv[2]) : 0;
        int fd = ::open("/output/fill", O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
            return 61;
        char block[65536];
        std::memset(block, 'x', sizeof(block));
        long long written = 0;
        while (written < wanted) {
            auto chunk = wanted - written < static_cast<long long>(sizeof(block))
                             ? static_cast<std::size_t>(wanted - written)
                             : sizeof(block);
            auto n = ::write(fd, block, chunk);
            if (n <= 0) {
                int saved = errno;
                ::close(fd);
                return (saved == ENOSPC || saved == EDQUOT) ? 60 : 62;
            }
            written += n;
        }
        ::close(fd);
        return 0;
    }
    if (!std::strcmp(mode, "fill-files")) {
        int wanted = argc > 2 ? std::atoi(argv[2]) : 0;
        for (int i = 0; i < wanted; ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "/output/file-%d", i);
            int fd = ::open(name, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0) {
                int saved = errno;
                return (saved == ENOSPC || saved == EDQUOT) ? 60 : 63;
            }
            ::close(fd);
        }
        return 0;
    }
    if (!std::strcmp(mode, "memory")) {
        for (;;) {
            auto *p = static_cast<volatile char *>(std::malloc(1024 * 1024));
            if (!p)
                return 34;
            for (int i = 0; i < 1024 * 1024; i += 4096)
                p[i] = 1;
        }
    }
    if (!std::strcmp(mode, "network")) {
        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0)
            return 35;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(9);
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        int r = ::connect(s, reinterpret_cast<sockaddr *>(&a), sizeof(a));
        int e = errno;
        ::close(s);
        if (r == 0)
            return 36;
        return e == ENETUNREACH ? 0 : 37;
    }
    if (!std::strcmp(mode, "seccomp")) {
        errno = 0;
        auto r = ::syscall(SYS_unshare, 0);
        return r == -1 && errno == EPERM ? 0 : 38;
    }
    if (!std::strcmp(mode, "nonzero"))
        return 7;
    return 99;
}
