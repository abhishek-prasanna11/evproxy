#include "connection.hpp"

#include "log.hpp"

namespace evp {

Connection::Connection(Fd client, const Config& cfg, uint64_t id)
    : client_(std::move(client)), cfg_(cfg), id_(id) {
    set_nonblocking(client_.get());
}

int Connection::want_fd() const {
    switch (want_) {
        case Want::ReadClient:
        case Want::WriteClient:
            return client_.get();
        case Want::ReadUpstream:
        case Want::WriteUpstream:
            return upstream_.get();
        case Want::None:
            return -1;
    }
    return -1;
}

void Connection::on_ready() {
    switch (want_) {
        case Want::ReadClient:    do_read_client();   break;
        case Want::WriteClient:   do_write_client();  break;
        case Want::ReadUpstream:  do_read_upstream(); break;
        case Want::WriteUpstream:
            // Writability on the upstream socket means two different things depending on state:
            // "the connect finished" while Connecting, "there is room to send" afterwards.
            if (state_ == State::Connecting) finish_connect();
            else                             do_write_upstream();
            break;
        case Want::None:          finish();           break;
    }
}

void Connection::fail_with(std::string response) {
    upstream_.reset();  // nothing left to relay
    out_client_     = std::move(response);
    out_client_off_ = 0;
    state_          = State::Failing;
    want_           = Want::WriteClient;
}

void Connection::finish() {
    state_ = State::Done;
    want_  = Want::None;
    upstream_.reset();
    client_.reset();
}

void Connection::do_read_client() {
    char buf[65536];
    size_t chunk = cfg_.io_chunk_bytes < sizeof(buf) ? cfg_.io_chunk_bytes : sizeof(buf);

    IoResult r = read_some(client_.get(), buf, chunk);
    if (r.status == IoStatus::WouldBlock) return;   // spurious wakeup; stay in this state
    if (r.status == IoStatus::Error) { finish(); return; }
    if (r.status == IoStatus::Closed) {
        // Client hung up before completing its request. Nothing to answer.
        finish();
        return;
    }

    in_client_.append(buf, r.bytes);

    if (!parsed_) {
        ParseResult pr = parse_request(in_client_, req_);
        if (pr == ParseResult::Incomplete) {
            if (in_client_.size() > cfg_.max_header_bytes)
                fail_with(error_response(400, "Bad Request"));
            return;
        }
        if (pr == ParseResult::BadRequest) {
            fail_with(error_response(400, "Bad Request"));
            return;
        }
        if (pr == ParseResult::NotImplemented) {
            fail_with(error_response(501, "Not Implemented"));
            return;
        }
        if (req_.content_length > cfg_.max_body_bytes) {
            fail_with(error_response(413, "Payload Too Large"));
            return;
        }
        parsed_ = true;
    }

    // Wait for the whole body before connecting. Simple, and it keeps the upstream connection out
    // of the picture until we know the request is complete.
    size_t needed = req_.header_bytes + req_.content_length;
    if (in_client_.size() < needed) return;

    std::string body  = in_client_.substr(req_.header_bytes, req_.content_length);
    out_upstream_     = build_upstream_request(req_, body);
    out_upstream_off_ = 0;

    // STILL BLOCKING, deliberately: getaddrinfo has no non-blocking form, and phase 4's experiment
    // is to measure the stall it inflicts on unrelated connections before fixing it.
    std::string err;
    if (!resolve(req_.host, req_.port, endpoints_, err)) {
        EVP_LOG_ERROR("conn %llu: resolve %s:%s failed: %s",
                      (unsigned long long)id_, req_.host.c_str(), req_.port.c_str(), err.c_str());
        fail_with(error_response(502, "Bad Gateway"));
        return;
    }

    endpoint_idx_ = 0;
    begin_connect();
}

void Connection::begin_connect() {
    std::string err;

    while (endpoint_idx_ < endpoints_.size()) {
        Fd            fd;
        ConnectStatus st = start_connect(endpoints_[endpoint_idx_], fd, err);
        ++endpoint_idx_;

        if (st == ConnectStatus::Failed) continue;  // try the next candidate address

        upstream_ = std::move(fd);

        if (st == ConnectStatus::Connected) {
            // Loopback usually completes immediately; skip straight to sending.
            state_ = State::SendingUpstream;
            want_  = Want::WriteUpstream;
            return;
        }

        // InProgress: completion arrives as writability, and writability alone is NOT proof of
        // success -- finish_connect() is where that gets checked.
        state_ = State::Connecting;
        want_  = Want::WriteUpstream;
        return;
    }

    EVP_LOG_ERROR("conn %llu: upstream %s:%s unreachable: %s",
                  (unsigned long long)id_, req_.host.c_str(), req_.port.c_str(), err.c_str());
    fail_with(error_response(502, "Bad Gateway"));
}

void Connection::finish_connect() {
    std::string err;

    // THE CHECK EVERYONE SKIPS. A socket whose connect failed also becomes writable, so without
    // this the proxy writes a request into a dead connection and fails confusingly later.
    if (!connect_succeeded(upstream_.get(), err)) {
        EVP_LOG_DEBUG("conn %llu: candidate %zu failed: %s",
                      (unsigned long long)id_, endpoint_idx_ - 1, err.c_str());
        upstream_.reset();
        begin_connect();  // fall through to the next address, or 502 when exhausted
        return;
    }

    EVP_LOG_DEBUG("conn %llu: %s %s%s -> fd %d",
                  (unsigned long long)id_, req_.method.c_str(), req_.host.c_str(),
                  req_.path.c_str(), upstream_.get());

    state_ = State::SendingUpstream;
    want_  = Want::WriteUpstream;
}

void Connection::do_write_upstream() {
    size_t remaining = out_upstream_.size() - out_upstream_off_;
    IoResult r = write_some(upstream_.get(), out_upstream_.data() + out_upstream_off_, remaining);

    if (r.status == IoStatus::WouldBlock) return;
    if (r.status != IoStatus::Ok) {
        fail_with(error_response(502, "Bad Gateway"));
        return;
    }

    // A short write is normal, not an error: buffer the remainder and come back when writable.
    out_upstream_off_ += r.bytes;
    if (out_upstream_off_ < out_upstream_.size()) return;

    out_upstream_.clear();
    out_upstream_off_ = 0;
    state_            = State::Relaying;
    want_             = Want::ReadUpstream;
}

void Connection::do_read_upstream() {
    char buf[65536];
    size_t chunk = cfg_.io_chunk_bytes < sizeof(buf) ? cfg_.io_chunk_bytes : sizeof(buf);

    IoResult r = read_some(upstream_.get(), buf, chunk);
    if (r.status == IoStatus::WouldBlock) return;
    if (r.status == IoStatus::Error) { finish(); return; }

    if (r.status == IoStatus::Closed) {
        // We forced `Connection: close` upstream, so EOF is how a complete response ends.
        upstream_eof_ = true;
        upstream_.reset();
        if (out_client_off_ >= out_client_.size()) finish();
        else want_ = Want::WriteClient;
        return;
    }

    out_client_.append(buf, r.bytes);
    want_ = Want::WriteClient;
}

void Connection::do_write_client() {
    size_t remaining = out_client_.size() - out_client_off_;
    IoResult r = write_some(client_.get(), out_client_.data() + out_client_off_, remaining);

    if (r.status == IoStatus::WouldBlock) return;
    if (r.status != IoStatus::Ok) { finish(); return; }

    out_client_off_ += r.bytes;
    if (out_client_off_ < out_client_.size()) return;  // partial write: flush the rest next time

    out_client_.clear();
    out_client_off_ = 0;

    if (state_ == State::Failing || upstream_eof_) {
        finish();
        return;
    }
    want_ = Want::ReadUpstream;
}

}  // namespace evp
