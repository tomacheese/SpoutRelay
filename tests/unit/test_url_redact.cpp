#include <cstdio>
#include <string>
#include "common/url_redact.hpp"
#include "test_utils.hpp"

int run_url_redact_tests() {
    {
        VERIFY(redact_url_userinfo("rtsp://user:pass@host:8554/live") ==
               "rtsp://***@host:8554/live");
        printf("[PASS] redact masks user:pass userinfo\n");
    }

    {
        VERIFY(redact_url_userinfo("rtsp://user@host/live") == "rtsp://***@host/live");
        printf("[PASS] redact masks username-only userinfo\n");
    }

    {
        VERIFY(redact_url_userinfo("rtsp://user:p@ss@host/live") == "rtsp://***@host/live");
        printf("[PASS] redact masks a password containing '@'\n");
    }

    {
        VERIFY(redact_url_userinfo("rtsp://host:8554/a@b") == "rtsp://host:8554/a@b");
        VERIFY(redact_url_userinfo("rtsp://host/live") == "rtsp://host/live");
        VERIFY(redact_url_userinfo("rtsp://host/live?x=a@b") == "rtsp://host/live?x=a@b");
        VERIFY(redact_url_userinfo("rtsp://host#a@b") == "rtsp://host#a@b");
        printf("[PASS] redact leaves URLs without userinfo unchanged\n");
    }

    {
        VERIFY(redact_url_userinfo("").empty());
        VERIFY(redact_url_userinfo("not-a-url") == "not-a-url");
        VERIFY(redact_url_userinfo("user:pass@host") == "user:pass@host");
        printf("[PASS] redact leaves malformed input unchanged\n");
    }

    {
        const std::string once = redact_url_userinfo("rtsp://user:s3cret@host/live");
        VERIFY(once == "rtsp://***@host/live");
        VERIFY(once.find("s3cret") == std::string::npos);
        VERIFY(redact_url_userinfo(once) == once);
        printf("[PASS] redact is idempotent and drops the secret\n");
    }

    return 0;
}
