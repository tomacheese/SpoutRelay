#include <cstdio>
#include <cstdlib>
#include <fstream>
#include "metrics/metrics_store.hpp"
#include "test_utils.hpp"

int run_metrics_tests() {
    printf("=== Metrics Store Tests ===\n");

    {
        // Counters start at zero
        MetricsStore ms;
        VERIFY(ms.frames_received()    == 0);
        VERIFY(ms.frames_encoded()     == 0);
        VERIFY(ms.frames_dropped()     == 0);
        VERIFY(ms.rtsp_errors()        == 0);
        VERIFY(ms.reconnect_attempts() == 0);
        printf("[PASS] Counters initialised to zero\n");
    }

    {
        // Increment counters
        MetricsStore ms;
        ms.increment_frames_received();
        ms.increment_frames_received();
        ms.increment_frames_encoded();
        ms.increment_frames_dropped();
        ms.increment_rtsp_errors();
        ms.increment_reconnect_attempts();
        VERIFY(ms.frames_received()    == 2);
        VERIFY(ms.frames_encoded()     == 1);
        VERIFY(ms.frames_dropped()     == 1);
        VERIFY(ms.rtsp_errors()        == 1);
        VERIFY(ms.reconnect_attempts() == 1);
        printf("[PASS] Increment counters work\n");
    }

    {
        // Reset clears counters
        MetricsStore ms;
        ms.increment_frames_received();
        ms.increment_frames_encoded();
        ms.reset_session_counters();
        VERIFY(ms.frames_received() == 0);
        VERIFY(ms.frames_encoded()  == 0);
        printf("[PASS] reset_session_counters clears\n");
    }

    {
        // save_metrics writes valid JSON file
        MetricsStore ms;
        ms.mark_session_start();
        ms.set_state("STREAMING");
        ms.set_sender_info("TestSender", 1920, 1080, 60.0f);
        ms.increment_frames_received();

        std::string path = "test_metrics_tmp.json";
        bool ok = ms.save_metrics(path);
        VERIFY_MSG(ok, "save_metrics should succeed");
        { std::ifstream chk(path); VERIFY_MSG(chk.is_open(), "metrics file should exist"); }

        // Check content contains expected fields
        std::ifstream f(path);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        VERIFY(content.find("\"state\"") != std::string::npos);
        VERIFY(content.find("\"frames_received\"") != std::string::npos);
        VERIFY(content.find("TestSender") != std::string::npos);

        std::remove(path.c_str());
        printf("[PASS] save_metrics writes correct JSON\n");
    }

    {
        // save_health writes valid JSON
        MetricsStore ms;
        ms.set_state("STREAMING");
        std::string path = "test_health_tmp.json";
        bool ok = ms.save_health(path);
        VERIFY(ok);
        { std::ifstream chk(path); VERIFY(chk.is_open()); }

        std::ifstream f(path);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        VERIFY(content.find("\"healthy\"") != std::string::npos);
        VERIFY(content.find("true") != std::string::npos);
        std::remove(path.c_str());
        printf("[PASS] save_health marks STREAMING as healthy\n");
    }

    {
        // FATAL state is not healthy
        MetricsStore ms;
        ms.set_state("FATAL");
        std::string path = "test_health_fatal_tmp.json";
        ms.save_health(path);

        std::ifstream f(path);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        VERIFY(content.find("\"healthy\": false") != std::string::npos ||
               content.find("\"healthy\":false") != std::string::npos);
        std::remove(path.c_str());
        printf("[PASS] save_health marks FATAL as not healthy\n");
    }

    return 0;
}
