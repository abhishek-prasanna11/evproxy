#include "http.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace evp {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

bool is_hop_by_hop(const std::string& lname) {
    return lname == "connection" || lname == "proxy-connection" || lname == "keep-alive" ||
           lname == "transfer-encoding" || lname == "upgrade" || lname == "te" ||
           lname == "trailer" || lname == "proxy-authorization" || lname == "proxy-authenticate";
}

// Splits `http://host:port/path` into its parts. A missing path is "/" -- `GET http://host HTTP/1.1`
// is legal and has no path at all, which is an easy off-by-one to get wrong.
bool split_absolute_url(const std::string& url, Request& out) {
    const std::string scheme = "http://";
    if (lower(url.substr(0, scheme.size())) != scheme) return false;

    size_t auth_start = scheme.size();
    size_t path_start = url.find('/', auth_start);

    std::string authority = (path_start == std::string::npos)
                                ? url.substr(auth_start)
                                : url.substr(auth_start, path_start - auth_start);

    out.path = (path_start == std::string::npos) ? "/" : url.substr(path_start);
    if (authority.empty()) return false;

    // IPv6 literals are bracketed: [::1]:8080
    if (authority.front() == '[') {
        size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':')
            out.port = authority.substr(close + 2);
    } else {
        size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            out.host = authority;
        } else {
            out.host = authority.substr(0, colon);
            out.port = authority.substr(colon + 1);
        }
    }

    if (out.host.empty() || out.port.empty()) return false;
    if (out.port.find_first_not_of("0123456789") != std::string::npos) return false;
    return true;
}

}  // namespace

ParseResult parse_request(const std::string& buf, Request& out) {
    size_t end = buf.find("\r\n\r\n");
    if (end == std::string::npos) return ParseResult::Incomplete;

    out.header_bytes = end + 4;

    size_t line_end = buf.find("\r\n");
    if (line_end == std::string::npos || line_end == 0) return ParseResult::BadRequest;

    // Request line: METHOD SP URI SP VERSION
    std::string line = buf.substr(0, line_end);
    size_t      sp1  = line.find(' ');
    if (sp1 == std::string::npos) return ParseResult::BadRequest;
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return ParseResult::BadRequest;

    out.method  = line.substr(0, sp1);
    out.version = line.substr(sp2 + 1);
    std::string url = line.substr(sp1 + 1, sp2 - sp1 - 1);

    if (out.method != "GET" && out.method != "HEAD" && out.method != "POST")
        return ParseResult::NotImplemented;

    if (!split_absolute_url(url, out)) return ParseResult::BadRequest;

    // Headers
    size_t pos = line_end + 2;
    while (pos < end) {
        size_t eol = buf.find("\r\n", pos);
        if (eol == std::string::npos || eol > end) break;

        std::string h = buf.substr(pos, eol - pos);
        pos = eol + 2;
        if (h.empty()) break;

        size_t colon = h.find(':');
        if (colon == std::string::npos) return ParseResult::BadRequest;

        out.headers.emplace_back(trim(h.substr(0, colon)), trim(h.substr(colon + 1)));
    }

    // A chunked request body would need dechunking before we could forward it; out of scope.
    if (lower(header_value(out, "Transfer-Encoding")).find("chunked") != std::string::npos)
        return ParseResult::NotImplemented;

    std::string cl = header_value(out, "Content-Length");
    if (!cl.empty()) {
        if (cl.find_first_not_of("0123456789") != std::string::npos) return ParseResult::BadRequest;
        out.content_length = std::strtoul(cl.c_str(), nullptr, 10);
    }

    return ParseResult::Ok;
}

std::string header_value(const Request& req, const std::string& name) {
    std::string want = lower(name);
    for (const auto& [k, v] : req.headers) {
        if (lower(k) == want) return v;
    }
    return {};
}

std::string build_upstream_request(const Request& req, const std::string& body) {
    std::string out;
    out.reserve(256 + body.size());

    out += req.method;
    out += ' ';
    out += req.path;
    out += " HTTP/1.1\r\n";

    // One Host header, ours -- the client's may be absent or disagree with the absolute URL.
    out += "Host: ";
    out += req.host;
    if (req.port != "80") {
        out += ':';
        out += req.port;
    }
    out += "\r\n";

    for (const auto& [k, v] : req.headers) {
        std::string lk = lower(k);
        if (lk == "host" || is_hop_by_hop(lk)) continue;
        out += k;
        out += ": ";
        out += v;
        out += "\r\n";
    }

    out += "Connection: close\r\n\r\n";
    out += body;
    return out;
}

std::string error_response(int code, const char* reason) {
    std::string body = std::to_string(code);
    body += ' ';
    body += reason;
    body += "\n";

    std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
    out += "Content-Type: text/plain\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += body;
    return out;
}

}  // namespace evp
