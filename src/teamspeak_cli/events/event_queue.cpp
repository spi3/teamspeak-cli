#include "teamspeak_cli/events/event_queue.hpp"

namespace teamspeak_cli::events {

void EventQueue::push(domain::Event event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(event));
    }
    condition_.notify_one();
}

auto EventQueue::pop_for(std::chrono::milliseconds timeout) -> std::optional<domain::Event> {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!condition_.wait_for(lock, timeout, [this] { return !events_.empty(); })) {
        return std::nullopt;
    }

    domain::Event event = std::move(events_.front());
    events_.pop_front();
    return event;
}

auto EventQueue::drain() -> std::vector<domain::Event> {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<domain::Event> drained;
    drained.reserve(events_.size());
    while (!events_.empty()) {
        drained.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    return drained;
}

}  // namespace teamspeak_cli::events
