#include "app/state_machine.hpp"
#include <stdexcept>

StateMachine::StateMachine() : current_(PublisherState::INIT) {
    build_transition_table();
}

void StateMachine::build_transition_table() {
    using S = PublisherState;
    valid_transitions_ = {
        {S::INIT,               S::IDLE},
        {S::INIT,               S::FATAL},
        {S::IDLE,               S::PROBING},
        {S::IDLE,               S::STOPPING},
        {S::PROBING,            S::IDLE},
        {S::PROBING,            S::CONNECTING_OUTPUT},
        {S::PROBING,            S::STOPPING},
        {S::CONNECTING_OUTPUT,  S::STREAMING},
        {S::CONNECTING_OUTPUT,  S::PROBING},
        {S::CONNECTING_OUTPUT,  S::STOPPING},
        {S::STREAMING,          S::STALLED},
        {S::STREAMING,          S::RECONFIGURING},
        {S::STREAMING,          S::RECONNECTING_OUTPUT},
        {S::STREAMING,          S::STOPPING},
        {S::STREAMING,          S::IDLE},
        {S::STREAMING,          S::PROBING},
        {S::STALLED,            S::STREAMING},
        {S::STALLED,            S::RECONNECTING_OUTPUT},
        {S::STALLED,            S::STOPPING},
        {S::STALLED,            S::IDLE},
        {S::STALLED,            S::PROBING},
        {S::RECONFIGURING,      S::STREAMING},
        {S::RECONFIGURING,      S::STOPPING},
        {S::RECONNECTING_OUTPUT,S::STREAMING},
        {S::RECONNECTING_OUTPUT,S::STOPPING},
        {S::RECONNECTING_OUTPUT,S::FATAL},
        {S::STOPPING,           S::IDLE},
    };
}

bool StateMachine::can_transition(PublisherState to) const {
    return valid_transitions_.count({current_, to}) > 0;
}

bool StateMachine::transition_to(PublisherState to) {
    if (!can_transition(to)) return false;
    PublisherState from = current_;
    current_ = to;
    if (callback_) callback_(from, to);
    return true;
}
