/* CURL
- Every libcurl handle Moroder makes goes through newCurl, so they all trust
  the same certificates
- Windows: the libcurl shipped next to moroder.exe (MSYS2's, on OpenSSL)
  looks for its CA bundle in an MSYS2 tree (..\etc\ssl\certs) that isn't
  there, and every HTTPS request fails (CURLE_SSL_CACERT_BADFILE). Windows'
  own certificate store instead, the one browsers use
*/
#pragma once

#include <curl/curl.h>

namespace utils {

// Again after curl_easy_reset, which clears it.
inline void curlTls(CURL* c) {
#ifdef _WIN32
    if (c) curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#else
    (void)c;
#endif
}

inline CURL* newCurl() {
    CURL* c = curl_easy_init();
    curlTls(c);
    return c;
}

}
