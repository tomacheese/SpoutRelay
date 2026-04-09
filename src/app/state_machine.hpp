#pragma once
#include "common/types.hpp"
#include <functional>
#include <unordered_set>
#include <utility>

class StateMachine {
public:
    using TransitionCallback = std::function<void(PublisherState /*from*/,
                                                   PublisherState /*to*/)>;

    StateMachine();

    PublisherState current_state() const { return current_; }

    bool can_transition(PublisherState to) const;
    bool transition_to(PublisherState to);

    void on_transition(TransitionCallback cb) { callback_ = std::move(cb); }

private:
    PublisherState current_;
    TransitionCallback callback_;

    struct PairHash {
        size_t operator()(const std::pair<PublisherState, PublisherState>& p) const {
            auto h1 = std::hash<int>{}(static_cast<int>(p.first));
            auto h2 = std::hash<int>{}(static_cast<int>(p.second));
            return h1 ^ (h2 << 16);
        }
    };

    std::unordered_set<std::pair<PublisherState, PublisherState>, PairHash> valid_transitions_;

    void build_transition_table();
};
