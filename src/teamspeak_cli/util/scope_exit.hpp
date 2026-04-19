#pragma once

#include <utility>

namespace teamspeak_cli::util {

template <typename F>
class ScopeExit {
  public:
    explicit ScopeExit(F&& callback) : callback_(std::forward<F>(callback)) {}

    ScopeExit(const ScopeExit&) = delete;
    auto operator=(const ScopeExit&) -> ScopeExit& = delete;

    ScopeExit(ScopeExit&& other) noexcept : callback_(std::move(other.callback_)), active_(other.active_) {
        other.active_ = false;
    }

    ~ScopeExit() {
        if (active_) {
            callback_();
        }
    }

    void release() { active_ = false; }

  private:
    F callback_;
    bool active_ = true;
};

template <typename F>
auto make_scope_exit(F&& callback) -> ScopeExit<F> {
    return ScopeExit<F>(std::forward<F>(callback));
}

}  // namespace teamspeak_cli::util
