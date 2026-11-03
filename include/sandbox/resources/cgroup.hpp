#pragma once
#include "sandbox/platform/linux.hpp"
namespace sandbox {
class CgroupV2 {
  public:
    CgroupV2(const fs::path &delegated, const std::string &id, const Limits &);
    ~CgroupV2();
    void attach(pid_t);
    bool empty() const;
    void kill_all();
    void remove();
    const fs::path &path() const {
        return path_;
    }
    static Json probe(const fs::path &);
    static void recover(const fs::path &, const std::string &id);

  private:
    fs::path path_;
    bool removed_ = false;
};
} // namespace sandbox
