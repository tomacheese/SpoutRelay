#pragma once
#include <string>

/// @brief Replace the userinfo of `scheme://userinfo@host/...` with `***`.
/// @details The authority ends at the first `/`, `?` or `#`; the last `@`
///          inside it delimits the userinfo, so a password containing `@` is
///          fully masked. URLs without userinfo are returned unchanged.
inline std::string redact_url_userinfo(const std::string& url) {
    const std::string kSchemeSep = "://";
    const auto scheme_end = url.find(kSchemeSep);
    if (scheme_end == std::string::npos) return url;

    const auto authority_start = scheme_end + kSchemeSep.size();
    auto authority_end = url.find_first_of("/?#", authority_start);
    if (authority_end == std::string::npos) authority_end = url.size();

    const auto at = url.rfind('@', authority_end - 1);
    if (at == std::string::npos || at < authority_start) return url;

    return url.substr(0, authority_start) + "***" + url.substr(at);
}
