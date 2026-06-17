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
        VERIFY(ms.frames_received()        == 0);
        VERIFY(ms.frames_encoded()         == 0);
        VERIFY(ms.frames_dropped()         == 0);
        VERIFY(ms.rtsp_errors()            == 0);
        VERIFY(ms.reconnect_attempts()     == 0);
        VERIFY(ms.device_lost_recoveries() == 0);
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
        ms.increment_device_lost_recoveries();
        VERIFY(ms.frames_received()        == 2);
        VERIFY(ms.frames_encoded()         == 1);
        VERIFY(ms.frames_dropped()         == 1);
        VERIFY(ms.rtsp_errors()            == 1);
        VERIFY(ms.reconnect_attempts()     == 1);
        VERIFY(ms.device_lost_recoveries() == 1);
        printf("[PASS] Increment counters work\n");
    }

    {
        // Reset clears counters
        MetricsStore ms;
        ms.increment_frames_received();
        ms.increment_frames_encoded();
        ms.increment_device_lost_recoveries();
        ms.reset_session_counters();
        VERIFY(ms.frames_received()        == 0);
        VERIFY(ms.frames_encoded()         == 0);
        VERIFY(ms.device_lost_recoveries() == 0);
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

    {
        // PLACEHOLDER state (NO SIGNAL 映像を継続配信中) is healthy
        MetricsStore ms;
        ms.set_state("PLACEHOLDER");
        std::string path = "test_health_placeholder_tmp.json";
        ms.save_health(path);

        std::ifstream f(path);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        VERIFY(content.find("\"healthy\": true") != std::string::npos ||
               content.find("\"healthy\":true") != std::string::npos);
        std::remove(path.c_str());
        printf("[PASS] save_health marks PLACEHOLDER as healthy\n");
    }

    // ---- Diff-skip tests ------------------------------------------------

    {
        // save_health: second call with unchanged state must skip write
        MetricsStore ms;
        ms.set_state("STREAMING");
        std::string path = "test_health_skip_tmp.json";

        bool first = ms.save_health(path);
        VERIFY_MSG(first, "first save_health should write (payload changed from empty)");

        // Read the timestamp written on first write
        std::string ts1;
        {
            std::ifstream f(path);
            std::string c((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
            auto p = c.find("\"ts\":");
            if (p != std::string::npos) ts1 = c.substr(p, 40);
        }

        bool second = ms.save_health(path);
        VERIFY_MSG(!second, "second save_health with same state should be skipped");

        // File should still exist and contain the original ts
        {
            std::ifstream f(path);
            std::string c((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
            VERIFY_MSG(!c.empty(), "file should still exist after skip");
            if (!ts1.empty())
                VERIFY_MSG(c.find(ts1) != std::string::npos,
                           "ts should be unchanged when write is skipped");
        }
        std::remove(path.c_str());
        printf("[PASS] save_health skips write when state is unchanged\n");
    }

    {
        // save_health: write resumes after state changes
        MetricsStore ms;
        ms.set_state("STREAMING");
        std::string path = "test_health_resume_tmp.json";

        ms.save_health(path);         // first write
        ms.save_health(path);         // skipped
        ms.set_state("FATAL");
        bool third = ms.save_health(path);
        VERIFY_MSG(third, "save_health should write again after state changes to FATAL");

        std::ifstream f(path);
        std::string c((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
        VERIFY(c.find("\"state\": \"FATAL\"") != std::string::npos ||
               c.find("\"state\":\"FATAL\"") != std::string::npos);
        std::remove(path.c_str());
        printf("[PASS] save_health resumes write after state transition\n");
    }

    {
        // save_metrics: second call with unchanged counters must skip write
        MetricsStore ms;
        ms.mark_session_start();
        ms.set_state("IDLE");
        std::string path = "test_metrics_skip_tmp.json";

        bool first = ms.save_metrics(path);
        VERIFY_MSG(first, "first save_metrics should write");

        bool second = ms.save_metrics(path);
        VERIFY_MSG(!second, "second save_metrics with same counters should be skipped");

        std::remove(path.c_str());
        printf("[PASS] save_metrics skips write when counters are unchanged\n");
    }

    {
        // save_metrics: write resumes after counter increments
        MetricsStore ms;
        ms.mark_session_start();
        ms.set_state("STREAMING");
        std::string path = "test_metrics_resume_tmp.json";

        ms.save_metrics(path);               // first write
        ms.save_metrics(path);               // skipped (no change)
        ms.increment_frames_received();
        bool third = ms.save_metrics(path);
        VERIFY_MSG(third, "save_metrics should write again after counter increments");

        std::ifstream f(path);
        std::string c((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
        VERIFY(c.find("\"frames_received\": 1") != std::string::npos ||
               c.find("\"frames_received\":1") != std::string::npos);
        std::remove(path.c_str());
        printf("[PASS] save_metrics resumes write after counter increment\n");
    }

    {
        // save_metrics: uptime_ms and ts must be present in written file
        // even though they are excluded from the diff comparison.
        MetricsStore ms;
        ms.mark_session_start();
        ms.set_state("STREAMING");
        std::string path = "test_metrics_fields_tmp.json";

        ms.save_metrics(path);

        std::ifstream f(path);
        std::string c((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
        VERIFY_MSG(c.find("\"uptime_ms\"") != std::string::npos,
                   "uptime_ms must be present in written metrics.json");
        VERIFY_MSG(c.find("\"ts\"") != std::string::npos,
                   "ts must be present in written metrics.json");
        std::remove(path.c_str());
        printf("[PASS] save_metrics includes uptime_ms and ts in written file\n");
    }

    {
        // device_lost_recoveries カウンターが metrics.json に出力されること
        MetricsStore ms;
        ms.mark_session_start();
        ms.set_state("STREAMING");
        ms.increment_device_lost_recoveries();
        std::string path = "test_metrics_device_lost_tmp.json";

        bool ok = ms.save_metrics(path);
        VERIFY_MSG(ok, "save_metrics should succeed");

        std::ifstream f(path);
        std::string c((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
        VERIFY_MSG(c.find("\"device_lost_recoveries\"") != std::string::npos,
                   "device_lost_recoveries must be present in metrics.json");
        VERIFY(c.find("\"device_lost_recoveries\": 1") != std::string::npos ||
               c.find("\"device_lost_recoveries\":1") != std::string::npos);
        std::remove(path.c_str());
        printf("[PASS] device_lost_recoveries is included in metrics.json\n");
    }

    return 0;
}
