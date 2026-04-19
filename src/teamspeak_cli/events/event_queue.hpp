#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <condition_variable>

#include "teamspeak_cli/domain/models.hpp"

namespace teamspeak_cli::events {

class EventQueue {
  public:
    void push(domain::Event event);
    auto pop_for(std::chrono::milliseconds timeout) -> std::optional<domain::Event>;
    auto drain() -> std::vector<domain::Event>;

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<domain::Event> events_;
};

}  // namespace teamspeak_cli::events
