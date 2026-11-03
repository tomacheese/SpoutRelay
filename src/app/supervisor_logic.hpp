#pragma once
#include <cstdint>

/// @brief Pure condition checks extracted from the Supervisor's state-transition logic.
///
///        The Supervisor itself depends on real I/O (Spout, encoder, RTSP) and is hard to
///        unit-test; the checks here are pure functions of their inputs and can be
///        covered exhaustively by unit tests.
namespace supervisor_logic {

/// @brief Decides whether a seamless PLACEHOLDER <-> real-source switch is possible.
///
///        Shared by handle_placeholder() (real source returns) and
///        handle_connecting_output() (first frame after returning from PLACEHOLDER).
///
///        Returns true only if all of the following hold:
///          - a seamless handoff was requested (seamless_handoff_requested)
///          - the encoder is alive (encoder_alive)
///          - the RTSP client is connected (rtsp_connected)
///          - the new resolution equals the current one
///
///        If any of these is false, the caller must re-initialize the encoder/RTSP
///        client.
///
/// @param seamless_handoff_requested whether a seamless handoff was requested
/// @param encoder_alive   whether the encoder instance is alive
/// @param rtsp_connected  whether the RTSP client is connected
/// @param new_width       width of the new video
/// @param new_height      height of the new video
/// @param current_width   width currently used by the encoder
/// @param current_height  height currently used by the encoder
/// @return true if a seamless switch is possible
inline bool can_seamless_handoff(bool seamless_handoff_requested,
                                  bool encoder_alive,
                                  bool rtsp_connected,
                                  uint32_t new_width, uint32_t new_height,
                                  uint32_t current_width, uint32_t current_height) {
    return seamless_handoff_requested &&
           encoder_alive && rtsp_connected &&
           new_width == current_width && new_height == current_height;
}

/// @brief Decides whether the received frame's resolution differs from the one
///        the encoder currently uses.
///
///        Called by encode_publish_thread_func() on each new frame; a true result
///        triggers the transition to RECONFIGURING.
///
/// @param frame_width    width of the received frame
/// @param frame_height   height of the received frame
/// @param current_width  width currently used by the encoder
/// @param current_height height currently used by the encoder
/// @return true if the resolution changed
inline bool resolution_changed(uint32_t frame_width, uint32_t frame_height,
                                uint32_t current_width, uint32_t current_height) {
    return frame_width != current_width || frame_height != current_height;
}

/// @brief In handle_connecting_output()'s wait loop for the first frame, decides whether
///        to reset the timeout timer, once, on the first ok=true seen after the GPU
///        connection event has been consumed.
///
///        On the GPU zero-copy path, the first argument-less ReceiveTexture() call
///        consumes the internal connection event (IsUpdated=true) and yields is_new=false.
///        Until the frame counter updates, ok=true/is_new=false can repeat; resetting the
///        timer every time would loop forever, so only the first reset is allowed.
///
///        Call only when ok=true and is_new=false.
///        If it returns true, the caller may call Stopwatch::reset().
///
/// @param source_responded [in/out] whether the first ok=true was already seen
/// @return true if the timer should be reset (only for the first ok=true)
inline bool should_reset_connect_timer_once(bool& source_responded) {
    if (source_responded) return false;
    source_responded = true;
    return true;
}

/// @brief Decides whether to force a full reconnect (restarting from PROBING, including
///        the Spout receiver) when repeated STALLED recovery attempts (RTSP reconnects)
///        never restored actual frame reception within the allowed number of attempts.
///
///        A safety-net watchdog, separate from handle_stalled()'s sender-missing check
///        (stall_timer_ + sender_missing_timeout_ms, confirmed via probe_sender()).
///        It covers the case where the sender still exists but frame_pump_ can never
///        receive frames again (e.g. spoutDX's internal update-event flag is not
///        re-initialized when the GPU zero-copy and CPU paths switch within one session,
///        so CPU-path reception keeps failing; RTSP-layer reconnects do not fix this).
///
///        max_attempts <= 0 disables the watchdog.
///
/// @param consecutive_stall_recoveries consecutive STALLED -> RECONNECTING_OUTPUT attempts
///        that never led to a real recovery (stall_recovered)
/// @param max_attempts attempts allowed before forcing a reset (from config; <= 0 disables)
/// @return true if a full reconnect including the Spout side should be forced
inline bool should_force_spout_recovery(int consecutive_stall_recoveries,
                                        int max_attempts) {
    return max_attempts > 0 && consecutive_stall_recoveries >= max_attempts;
}

} // namespace supervisor_logic
