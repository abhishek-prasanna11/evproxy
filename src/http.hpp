#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace evp {

// A proxied request. Clients send absolute-form request lines to a forward proxy:
//
//     GET http://example.com:8080/path HTTP/1.1
//
// which normal origin servers never see. We split that into host/port/path and forward it to the
// origin in the origin form it expects (`GET /path HTTP/1.1`).
struct Request {
    std::string method;
    std::string host;
    std::string port = "80";
    std::string path = "/";
    std::string version;

    std::vector<std::pair<std::string, std::string>> headers;

    size_t content_length = 0;      // request body length; 0 when absent
    size_t header_bytes   = 0;      // bytes consumed by the header block, including the final CRLFCRLF
};

enum class ParseResult {
    Incomplete,      // header terminator not seen yet -- read more
    Ok,
    BadRequest,      // 400
    NotImplemented,  // 501
};

// Parses the header block at the start of `buf`. Does not consume the body.
ParseResult parse_request(const std::string& buf, Request& out);

// Rebuilds the request for the origin: origin-form request line, a single Host header, hop-by-hop
// headers dropped, and `Connection: close` forced.
//
// Forcing close is what lets the relay treat upstream EOF as "response complete" without parsing
// Content-Length or chunked encoding on the response path. Keep-alive is out of scope for this
// project (blueprint 9); the cost is one connection per request, which is identical across all
// three backends and so does not bias the comparison.
std::string build_upstream_request(const Request& req, const std::string& body);

std::string error_response(int code, const char* reason);

// Case-insensitive header lookup. Returns empty string when absent.
std::string header_value(const Request& req, const std::string& name);

}  // namespace evp
