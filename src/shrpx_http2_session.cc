/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "shrpx_http2_session.h"

#include <netinet/tcp.h>
#include <unistd.h>

#include <vector>

#include <openssl/err.h>

#include "shrpx_upstream.h"
#include "shrpx_downstream.h"
#include "shrpx_config.h"
#include "shrpx_error.h"
#include "shrpx_http2_downstream_connection.h"
#include "shrpx_client_handler.h"
#include "shrpx_ssl.h"
#include "shrpx_http.h"
#include "shrpx_worker_config.h"
#include "http2.h"
#include "util.h"
#include "base64.h"

using namespace nghttp2;

namespace shrpx {

namespace {
void connchk_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto http2session = static_cast<Http2Session *>(w->data);
  SSLOG(INFO, http2session) << "connection check required";
  ev_timer_stop(loop, w);
  http2session->set_connection_check_state(
      Http2Session::CONNECTION_CHECK_REQUIRED);
}
} // namespace

namespace {
void settings_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto http2session = static_cast<Http2Session *>(w->data);
  http2session->stop_settings_timer();
  SSLOG(INFO, http2session) << "SETTINGS timeout";
  if (http2session->terminate_session(NGHTTP2_SETTINGS_TIMEOUT) != 0) {
    http2session->disconnect();
    return;
  }
  http2session->signal_write();
}
} // namespace

namespace {
void timeoutcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto http2session = static_cast<Http2Session *>(w->data);

  if (LOG_ENABLED(INFO)) {
    SSLOG(INFO, http2session) << "Timeout";
  }

  http2session->disconnect(http2session->get_state() ==
                           Http2Session::CONNECTING);
}
} // namespace

namespace {
void readcb(struct ev_loop *loop, ev_io *w, int revents) {
  int rv;
  auto http2session = static_cast<Http2Session *>(w->data);
  http2session->connection_alive();
  rv = http2session->do_read();
  if (rv != 0) {
    http2session->disconnect(http2session->should_hard_fail());
  }
}
} // namespace

namespace {
void writecb(struct ev_loop *loop, ev_io *w, int revents) {
  int rv;
  auto http2session = static_cast<Http2Session *>(w->data);
  http2session->clear_write_request();
  http2session->connection_alive();
  rv = http2session->do_write();
  if (rv != 0) {
    http2session->disconnect(http2session->should_hard_fail());
  }
}
} // namespace

namespace {
void wrschedcb(struct ev_loop *loop, ev_prepare *w, int revents) {
  auto http2session = static_cast<Http2Session *>(w->data);
  if (!http2session->write_requested()) {
    return;
  }
  http2session->clear_write_request();
  switch (http2session->get_state()) {
  case Http2Session::DISCONNECTED:
    LOG(INFO) << "wrschedcb start connect";
    if (http2session->initiate_connection() != 0) {
      SSLOG(FATAL, http2session) << "Could not initiate backend connection";
      http2session->disconnect(true);
    }
    break;
  case Http2Session::CONNECTED:
    writecb(loop, http2session->get_wev(), revents);
    break;
  }
}
} // namespace

Http2Session::Http2Session(struct ev_loop *loop, SSL_CTX *ssl_ctx)
    : loop_(loop), ssl_ctx_(ssl_ctx), ssl_(nullptr), session_(nullptr),
      data_pending_(nullptr), data_pendinglen_(0), fd_(-1),
      state_(DISCONNECTED), connection_check_state_(CONNECTION_CHECK_NONE),
      flow_control_(false), write_requested_(false) {
  // We do not know fd yet, so just set dummy fd 0
  ev_io_init(&wev_, writecb, 0, EV_WRITE);
  ev_io_init(&rev_, readcb, 0, EV_READ);

  wev_.data = this;
  rev_.data = this;

  read_ = write_ = &Http2Session::noop;
  on_read_ = on_write_ = &Http2Session::noop;

  ev_timer_init(&wt_, timeoutcb, 0., get_config()->downstream_write_timeout);
  ev_timer_init(&rt_, timeoutcb, 0., get_config()->downstream_read_timeout);

  wt_.data = this;
  rt_.data = this;

  // We will resuse this many times, so use repeat timeout value.
  ev_timer_init(&connchk_timer_, connchk_timeout_cb, 0., 5.);

  connchk_timer_.data = this;

  // SETTINGS ACK timeout is 10 seconds for now.  We will resuse this
  // many times, so use repeat timeout value.
  ev_timer_init(&settings_timer_, settings_timeout_cb, 0., 10.);

  settings_timer_.data = this;

  ev_prepare_init(&wrsched_prep_, &wrschedcb);
  wrsched_prep_.data = this;

  ev_prepare_start(loop_, &wrsched_prep_);
}

Http2Session::~Http2Session() { disconnect(); }

int Http2Session::disconnect(bool hard) {
  if (LOG_ENABLED(INFO)) {
    SSLOG(INFO, this) << "Disconnecting";
  }
  nghttp2_session_del(session_);
  session_ = nullptr;

  rb_.reset();
  wb_.reset();

  ev_timer_stop(loop_, &settings_timer_);
  ev_timer_stop(loop_, &connchk_timer_);

  ev_timer_stop(loop_, &rt_);
  ev_timer_stop(loop_, &wt_);

  read_ = write_ = &Http2Session::noop;
  on_read_ = on_write_ = &Http2Session::noop;

  ev_io_stop(loop_, &rev_);
  ev_io_stop(loop_, &wev_);

  if (ssl_) {
    SSL_set_shutdown(ssl_, SSL_RECEIVED_SHUTDOWN);
    ERR_clear_error();
    SSL_shutdown(ssl_);
    SSL_free(ssl_);
    ssl_ = nullptr;
  }

  if (fd_ != -1) {
    if (LOG_ENABLED(INFO)) {
      SSLOG(INFO, this) << "Closing fd=" << fd_;
    }
    shutdown(fd_, SHUT_WR);
    close(fd_);
    fd_ = -1;
  }

  if (proxy_htp_) {
    proxy_htp_.reset();
  }

  connection_check_state_ = CONNECTION_CHECK_NONE;
  state_ = DISCONNECTED;

  // Delete all client handler associated to Downstream. When deleting
  // Http2DownstreamConnection, it calls this object's
  // remove_downstream_connection(). The multiple
  // Http2DownstreamConnection objects belong to the same
  // ClientHandler object. So first dump ClientHandler objects.  We
  // want to allow creating new pending Http2DownstreamConnection with
  // this object.  In order to achieve this, we first swap dconns_ and
  // streams_.  Upstream::on_downstream_reset() may add
  // Http2DownstreamConnection.
  std::set<Http2DownstreamConnection *> dconns;
  dconns.swap(dconns_);
  std::set<StreamData *> streams;
  streams.swap(streams_);

  std::set<ClientHandler *> handlers;
  for (auto dc : dconns) {
    if (!dc->get_client_handler()) {
      continue;
    }
    handlers.insert(dc->get_client_handler());
  }
  for (auto h : handlers) {
    if (h->get_upstream()->on_downstream_reset(hard) != 0) {
      delete h;
    }
  }

  for (auto &s : streams) {
    delete s;
  }

  return 0;
}

int Http2Session::check_cert() { return ssl::check_cert(ssl_); }

int Http2Session::initiate_connection() {
  int rv = 0;
  if (get_config()->downstream_http_proxy_host && state_ == DISCONNECTED) {
    if (LOG_ENABLED(INFO)) {
      SSLOG(INFO, this) << "Connecting to the proxy "
                        << get_config()->downstream_http_proxy_host.get() << ":"
                        << get_config()->downstream_http_proxy_port;
    }

    fd_ = util::create_nonblock_socket(
        get_config()->downstream_http_proxy_addr.storage.ss_family);

    if (fd_ == -1) {
      return -1;
    }

    rv = connect(fd_, const_cast<sockaddr *>(
                          &get_config()->downstream_http_proxy_addr.sa),
                 get_config()->downstream_http_proxy_addrlen);
    if (rv != 0 && errno != EINPROGRESS) {
      SSLOG(ERROR, this) << "Failed to connect to the proxy "
                         << get_config()->downstream_http_proxy_host.get()
                         << ":" << get_config()->downstream_http_proxy_port;
      return -1;
    }

    ev_io_set(&rev_, fd_, EV_READ);
    ev_io_set(&wev_, fd_, EV_WRITE);

    ev_io_start(loop_, &wev_);

    // TODO we should have timeout for connection establishment
    ev_timer_again(loop_, &wt_);

    write_ = &Http2Session::connected;

    on_read_ = &Http2Session::downstream_read_proxy;
    on_write_ = &Http2Session::downstream_connect_proxy;

    proxy_htp_ = util::make_unique<http_parser>();
    http_parser_init(proxy_htp_.get(), HTTP_RESPONSE);
    proxy_htp_->data = this;

    state_ = PROXY_CONNECTING;

    return 0;
  }

  if (state_ == DISCONNECTED || state_ == PROXY_CONNECTED) {
    if (LOG_ENABLED(INFO)) {
      SSLOG(INFO, this) << "Connecting to downstream server";
    }
    if (ssl_ctx_) {
      // We are establishing TLS connection.
      ssl_ = SSL_new(ssl_ctx_);
      if (!ssl_) {
        SSLOG(ERROR, this) << "SSL_new() failed: "
                           << ERR_error_string(ERR_get_error(), NULL);
        return -1;
      }

      const char *sni_name = nullptr;
      if (get_config()->backend_tls_sni_name) {
        sni_name = get_config()->backend_tls_sni_name.get();
      } else {
        sni_name = get_config()->downstream_addrs[0].host.get();
      }

      if (sni_name && !util::numeric_host(sni_name)) {
        // TLS extensions: SNI. There is no documentation about the return
        // code for this function (actually this is macro wrapping SSL_ctrl
        // at the time of this writing).
        SSL_set_tlsext_host_name(ssl_, sni_name);
      }
      // If state_ == PROXY_CONNECTED, we has connected to the proxy
      // using fd_ and tunnel has been established.
      if (state_ == DISCONNECTED) {
        assert(fd_ == -1);

        fd_ = util::create_nonblock_socket(
            get_config()->downstream_addrs[0].addr.storage.ss_family);
        if (fd_ == -1) {
          return -1;
        }

        rv = connect(
            fd_,
            // TODO maybe not thread-safe?
            const_cast<sockaddr *>(&get_config()->downstream_addrs[0].addr.sa),
            get_config()->downstream_addrs[0].addrlen);
        if (rv != 0 && errno != EINPROGRESS) {
          return -1;
        }
      }

      if (SSL_set_fd(ssl_, fd_) == 0) {
        return -1;
      }

      SSL_set_connect_state(ssl_);
    } else {
      if (state_ == DISCONNECTED) {
        // Without TLS and proxy.
        assert(fd_ == -1);

        fd_ = util::create_nonblock_socket(
            get_config()->downstream_addrs[0].addr.storage.ss_family);

        if (fd_ == -1) {
          return -1;
        }

        rv = connect(fd_, const_cast<sockaddr *>(
                              &get_config()->downstream_addrs[0].addr.sa),
                     get_config()->downstream_addrs[0].addrlen);
        if (rv != 0 && errno != EINPROGRESS) {
          return -1;
        }
      } else {
        // Without TLS but with proxy.  Connection already
        // established.
        if (on_connect() != -1) {
          state_ = CONNECT_FAILING;
          return -1;
        }
      }
    }

    // rev_ and wev_ could possibly be active here.  Since calling
    // ev_io_set is not allowed while watcher is active, we have to
    // stop them just in case.
    ev_io_stop(loop_, &rev_);
    ev_io_stop(loop_, &wev_);

    ev_io_set(&rev_, fd_, EV_READ);
    ev_io_set(&wev_, fd_, EV_WRITE);

    ev_io_start(loop_, &wev_);

    write_ = &Http2Session::connected;

    on_write_ = &Http2Session::downstream_write;
    on_read_ = &Http2Session::downstream_read;

    // We have been already connected when no TLS and proxy is used.
    if (state_ != CONNECTED) {
      state_ = CONNECTING;
      ev_io_start(loop_, &wev_);
      // TODO we should have timeout for connection establishment
      ev_timer_again(loop_, &wt_);
    } else {
      ev_timer_again(loop_, &rt_);
    }

    return 0;
  }

  // Unreachable
  DIE();
  return 0;
}

namespace {
int htp_hdrs_completecb(http_parser *htp) {
  auto http2session = static_cast<Http2Session *>(htp->data);
  // We just check status code here
  if (htp->status_code == 200) {
    if (LOG_ENABLED(INFO)) {
      SSLOG(INFO, http2session) << "Tunneling success";
    }
    http2session->set_state(Http2Session::PROXY_CONNECTED);

    return 0;
  }

  SSLOG(WARN, http2session) << "Tunneling failed: " << htp->status_code;
  http2session->set_state(Http2Session::PROXY_FAILED);

  return 0;
}
} // namespace

namespace {
http_parser_settings htp_hooks = {
    nullptr,             // http_cb      on_message_begin;
    nullptr,             // http_data_cb on_url;
    nullptr,             // http_data_cb on_status;
    nullptr,             // http_data_cb on_header_field;
    nullptr,             // http_data_cb on_header_value;
    htp_hdrs_completecb, // http_cb      on_headers_complete;
    nullptr,             // http_data_cb on_body;
    nullptr              // http_cb      on_message_complete;
};
} // namespace

int Http2Session::downstream_read_proxy() {
  for (;;) {
    const void *data;
    size_t datalen;
    std::tie(data, datalen) = rb_.get();

    if (datalen == 0) {
      break;
    }

    size_t nread =
        http_parser_execute(proxy_htp_.get(), &htp_hooks,
                            reinterpret_cast<const char *>(data), datalen);

    rb_.drain(nread);

    auto htperr = HTTP_PARSER_ERRNO(proxy_htp_.get());

    if (htperr != HPE_OK) {
      return -1;
    }

    switch (state_) {
    case Http2Session::PROXY_CONNECTED:
      // Initiate SSL/TLS handshake through established tunnel.
      if (initiate_connection() != 0) {
        return -1;
      }
      break;
    case Http2Session::PROXY_FAILED:
      return -1;
    }
  }
  return 0;
}

int Http2Session::downstream_connect_proxy() {
  if (LOG_ENABLED(INFO)) {
    SSLOG(INFO, this) << "Connected to the proxy";
  }
  std::string req = "CONNECT ";
  req += get_config()->downstream_addrs[0].hostport.get();
  req += " HTTP/1.1\r\nHost: ";
  req += get_config()->downstream_addrs[0].host.get();
  req += "\r\n";
  if (get_config()->downstream_http_proxy_userinfo) {
    req += "Proxy-Authorization: Basic ";
    size_t len = strlen(get_config()->downstream_http_proxy_userinfo.get());
    req += base64::encode(get_config()->downstream_http_proxy_userinfo.get(),
                          get_config()->downstream_http_proxy_userinfo.get() +
                              len);
    req += "\r\n";
  }
  req += "\r\n";
  if (LOG_ENABLED(INFO)) {
    SSLOG(INFO, this) << "HTTP proxy request headers\n" << req;
  }
  auto nwrite = wb_.write(req.c_str(), req.size());
  if (nwrite != req.size()) {
    SSLOG(WARN, this) << "HTTP proxy request is too large";
    return -1;
  }
  on_write_ = &Http2Session::noop;

  signal_write();
  return 0;
}

void Http2Session::add_downstream_connection(Http2DownstreamConnection *dconn) {
  dconns_.insert(dconn);
}

void
Http2Session::remove_downstream_connection(Http2DownstreamConnection *dconn) {
  dconns_.erase(dconn);
  dconn->detach_stream_data();
}

void Http2Session::remove_stream_data(StreamData *sd) {
  streams_.erase(sd);
  if (sd->dconn) {
    sd->dconn->detach_stream_data();
  }
  delete sd;
}

int Http2Session::submit_request(Http2DownstreamConnection *dconn, int32_t pri,
                                 const nghttp2_nv *nva, size_t nvlen,
                                 const nghttp2_data_provider *data_prd) {
  assert(state_ == CONNECTED);
  auto sd = util::make_unique<StreamData>();
  // TODO Specify nullptr to pri_spec for now
  auto stream_id =
      nghttp2_submit_request(session_, nullptr, nva, nvlen, data_prd, sd.get());
  if (stream_id < 0) {
    SSLOG(FATAL, this) << "nghttp2_submit_request() failed: "
                       << nghttp2_strerror(stream_id);
    return -1;
  }

  dconn->attach_stream_data(sd.get());
  dconn->get_downstream()->set_downstream_stream_id(stream_id);
  streams_.insert(sd.release());

  return 0;
}

int Http2Session::submit_rst_stream(int32_t stream_id, uint32_t error_code) {
  assert(state_ == CONNECTED);
  if (LOG_ENABLED(INFO)) {
    SSLOG(INFO, this) << "RST_STREAM stream_id=" << stream_id
                      << " with error_code=" << error_code;
  }
  int rv = nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, stream_id,
                                     error_code);
  if (rv != 0) {
    SSLOG(FATAL, this) << "nghttp2_submit_rst_stream() failed: "
                       << nghttp2_strerror(rv);
    return -1;
  }
  return 0;
}

int Http2Session::submit_priority(Http2DownstreamConnection *dconn,
                                  int32_t pri) {
  assert(state_ == CONNECTED);
  if (!dconn) {
    return 0;
  }
  int rv;

  // TODO Disabled temporarily

  // rv = nghttp2_submit_priority(session_, NGHTTP2_FLAG_NONE,
  //                              dconn->get_downstream()->
  //                              get_downstream_stream_id(), pri);

  rv = 0;

  if (rv < NGHTTP2_ERR_FATAL) {
    SSLOG(FATAL, this) << "nghttp2_submit_priority() failed: "
                       << nghttp2_strerror(rv);
    return -1;
  }
  return 0;
}

nghttp2_session *Http2Session::get_session() const { return session_; }

bool Http2Session::get_flow_control() const { return flow_control_; }

int Http2Session::resume_data(Http2DownstreamConnection *dconn) {
  assert(state_ == CONNECTED);
  auto downstream = dconn->get_downstream();
  int rv = nghttp2_session_resume_data(session_,
                                       downstream->get_downstream_stream_id());
  switch (rv) {
  case 0:
  case NGHTTP2_ERR_INVALID_ARGUMENT:
    return 0;
  default:
    SSLOG(FATAL, this) << "nghttp2_resume_session() failed: "
                       << nghttp2_strerror(rv);
    return -1;
  }
}

namespace {
void call_downstream_readcb(Http2Session *http2session,
                            Downstream *downstream) {
  auto upstream = downstream->get_upstream();
  if (!upstream) {
    return;
  }
  if (upstream->downstream_read(downstream->get_downstream_connection()) != 0) {
    delete upstream->get_client_handler();
  }
}
} // namespace

namespace {
int on_stream_close_callback(nghttp2_session *session, int32_t stream_id,
                             uint32_t error_code, void *user_data) {
  auto http2session = static_cast<Http2Session *>(user_data);
  if (LOG_ENABLED(INFO)) {
    SSLOG(INFO, http2session) << "Stream stream_id=" << stream_id
                              << " is being closed";
  }
  auto sd = static_cast<StreamData *>(
      nghttp2_session_get_stream_user_data(session, stream_id));
  if (sd == 0) {
    // We might get this close callback when pushed streams are
    // closed.
    return 0;
  }
  auto dconn = sd->dconn;
  if (dconn) {
    auto downstream = dconn->get_downstream();
    if (downstream && downstream->get_downstream_stream_id() == stream_id) {

      if (downstream->get_upgraded() &&
          downstream->get_response_state() == Downstream::HEADER_COMPLETE) {
        // For tunneled connection, we have to submit RST_STREAM to
        // upstream *after* whole response body is sent. We just set
        // MSG_COMPLETE here. Upstream will take care of that.
        downstream->get_upstream()->on_downstream_body_complete(downstream);
        downstream->set_response_state(Downstream::MSG_COMPLETE);
      } else if (error_code == NGHTTP2_NO_ERROR) {
        switch (downstream->get_response_state()) {
        case Downstream::MSG_COMPLETE:
        case Downstream::MSG_BAD_HEADER:
          break;
        default:
          downstream->set_response_state(Downstream::MSG_RESET);
        }
      } else if (downstream->get_response_state() !=
                 Downstream::MSG_BAD_HEADER) {
        downstream->set_response_state(Downstream::MSG_RESET);
      }
      call_downstream_readcb(http2session, downstream);
      // dconn may be deleted
    }
  }
  // The life time of StreamData ends here
  http2session->remove_stream_data(sd);
  return 0;
}
} // namespace

void Http2Session::start_settings_timer() {
  ev_timer_again(loop_, &settings_timer_);
}

void Http2Session::stop_settings_timer() {
  ev_timer_stop(loop_, &settings_timer_);
}

namespace {
int on_header_callback(nghttp2_session *session, const nghttp2_frame *frame,
                       const uint8_t *name, size_t namelen,
                       const uint8_t *value, size_t valuelen, uint8_t flags,
                       void *user_data) {
  auto http2session = static_cast<Http2Session *>(user_data);
  auto sd = static_cast<StreamData *>(
      nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
  if (!sd || !sd->dconn) {
    return 0;
  }
  auto downstream = sd->dconn->get_downstream();
  if (!downstream) {
    return 0;
  }

  if (frame->hd.type != NGHTTP2_HEADERS ||
      (frame->headers.cat != NGHTTP2_HCAT_RESPONSE &&
       !downstream->get_expect_final_response())) {
    return 0;
  }

  if (downstream->get_response_headers_sum() > Downstream::MAX_HEADERS_SUM) {
    if (LOG_ENABLED(INFO)) {
      DLOG(INFO, downstream) << "Too large header block size="
                             << downstream->get_response_headers_sum();
    }
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  }
  if (!http2::check_nv(name, namelen, value, valuelen)) {
    return 0;
  }

  auto token = http2::lookup_token(name, namelen);

  if (name[0] == ':') {
    if (!downstream->response_pseudo_header_allowed(token)) {
      http2session->submit_rst_stream(frame->hd.stream_id,
                                      NGHTTP2_PROTOCOL_ERROR);
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
  }

  if (!http2::http2_header_allowed(token)) {
    http2session->submit_rst_stream(frame->hd.stream_id,
                                    NGHTTP2_PROTOCOL_ERROR);
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  }

  if (token == http2::HD_CONTENT_LENGTH) {
    auto len = util::parse_uint(value, valuelen);
    if (len == -1) {
      http2session->submit_rst_stream(frame->hd.stream_id,
                                      NGHTTP2_PROTOCOL_ERROR);
      downstream->set_response_state(Downstream::MSG_BAD_HEADER);
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    if (downstream->get_response_content_length() != -1) {
      http2session->submit_rst_stream(frame->hd.stream_id,
                                      NGHTTP2_PROTOCOL_ERROR);
      downstream->set_response_state(Downstream::MSG_BAD_HEADER);
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    downstream->set_response_content_length(len);
  }

  downstream->add_response_header(name, namelen, value, valuelen,
                                  flags & NGHTTP2_NV_FLAG_NO_INDEX, token);
  return 0;
}
} // namespace

namespace {
int on_begin_headers_callback(nghttp2_session *session,
                              const nghttp2_frame *frame, void *user_data) {
  auto http2session = static_cast<Http2Session *>(user_data);
  if (frame->headers.cat != NGHTTP2_HCAT_RESPONSE) {
    return 0;
  }
  auto sd = static_cast<StreamData *>(
      nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
  if (!sd || !sd->dconn) {
    http2session->submit_rst_stream(frame->hd.stream_id,
                                    NGHTTP2_INTERNAL_ERROR);
    return 0;
  }
  auto downstream = sd->dconn->get_downstream();
  if (!downstream ||
      downstream->get_downstream_stream_id() != frame->hd.stream_id) {
    http2session->submit_rst_stream(frame->hd.stream_id,
                                    NGHTTP2_INTERNAL_ERROR);
    return 0;
  }
  return 0;
}
} // namespace

namespace {
int on_response_headers(Http2Session *http2session, Downstream *downstream,
                        nghttp2_session *session, const nghttp2_frame *frame) {
  int rv;

  auto upstream = downstream->get_upstream();

  auto &nva = downstream->get_response_headers();

  downstream->set_expect_final_response(false);

  auto status = downstream->get_response_header(http2::HD__STATUS);
  int status_code;

  if (!http2::non_empty_value(status) ||
      (status_code = http2::parse_http_status_code(status->value)) == -1) {

    http2session->submit_rst_stream(frame->hd.stream_id,
                                    NGHTTP2_PROTOCOL_ERROR);
    downstream->set_response_state(Downstream::MSG_RESET);
    call_downstream_readcb(http2session, downstream);

    return 0;
  }

  downstream->set_response_http_status(status_code);
  downstream->set_response_major(2);
  downstream->set_response_minor(0);

  if (LOG_ENABLED(INFO)) {
    std::stringstream ss;
    for (auto &nv : nva) {
      ss << TTY_HTTP_HD << nv.name << TTY_RST << ": " << nv.value << "\n";
    }
    SSLOG(INFO, http2session)
        << "HTTP response headers. stream_id=" << frame->hd.stream_id << "\n"
        << ss.str();
  }

  if (downstream->get_non_final_response()) {

    if (LOG_ENABLED(INFO)) {
      SSLOG(INFO, http2session) << "This is non-final response.";
    }

    downstream->set_expect_final_response(true);
    rv = upstream->on_downstream_header_complete(downstream);

    // Now Dowstream's response headers are erased.

    if (rv != 0) {
      http2session->submit_rst_stream(frame->hd.stream_id,
                                      NGHTTP2_PROTOCOL_ERROR);
      downstream->set_response_state(Downstream::MSG_RESET);
    }

    return 0;
  }

  if (downstream->get_response_content_length() == -1 &&
      downstream->expect_response_body()) {
    // Here we have response body but Content-Length is not known in
    // advance.
    if (downstream->get_request_major() <= 0 ||
        (downstream->get_request_major() <= 1 &&
         downstream->get_request_minor() <= 0)) {
      // We simply close connection for pre-HTTP/1.1 in this case.
      downstream->set_response_connection_close(true);
    } else if (downstream->get_request_method() != "CONNECT") {
      // Otherwise, use chunked encoding to keep upstream connection
      // open.  In HTTP2, we are supporsed not to receive
      // transfer-encoding.
      downstream->add_response_header("transfer-encoding", "chunked");
      downstream->set_chunked_response(true);
    }
  }

  downstream->set_response_state(Downstream::HEADER_COMPLETE);
  downstream->check_upgrade_fulfilled();
  if (downstream->get_upgraded()) {
    downstream->set_response_connection_close(true);
    // On upgrade sucess, both ends can send data
    if (upstream->resume_read(SHRPX_MSG_BLOCK, downstream, 0) != 0) {
      // If resume_read fails, just drop connection. Not ideal.
      delete upstream->get_client_handler();
      return -1;
    }
    downstream->set_request_state(Downstream::HEADER_COMPLETE);
    if (LOG_ENABLED(INFO)) {
      SSLOG(INFO, http2session)
          << "HTTP upgrade success. stream_id=" << frame->hd.stream_id;
    }
  } else if (downstream->get_request_method() == "CONNECT") {
    // If request is CONNECT, terminate request body to avoid for
    // stream to stall.
    downstream->end_upload_data();
  }
  rv = upstream->on_downstream_header_complete(downstream);
  if (rv != 0) {
    http2session->submit_rst_stream(frame->hd.stream_id,
                                    NGHTTP2_PROTOCOL_ERROR);
    downstream->set_response_state(Downstream::MSG_RESET);
  }

  return 0;
}
} // namespace

namespace {
int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame,
                           void *user_data) {
  int rv;
  auto http2session = static_cast<Http2Session *>(user_data);

  switch (frame->hd.type) {
  case NGHTTP2_DATA: {
    auto sd = static_cast<StreamData *>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    if (!sd || !sd->dconn) {
      break;
    }
    auto downstream = sd->dconn->get_downstream();
    if (!downstream ||
        downstream->get_downstream_stream_id() != frame->hd.stream_id) {
      break;
    }

    auto upstream = downstream->get_upstream();
    rv = upstream->on_downstream_body(downstream, nullptr, 0, true);
    if (rv != 0) {
      http2session->submit_rst_stream(frame->hd.stream_id,
                                      NGHTTP2_INTERNAL_ERROR);
      downstream->set_response_state(Downstream::MSG_RESET);

    } else if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {

      downstream->disable_downstream_rtimer();

      if (downstream->get_response_state() == Downstream::HEADER_COMPLETE) {

        downstream->set_response_state(Downstream::MSG_COMPLETE);

        rv = upstream->on_downstream_body_complete(downstream);

        if (rv != 0) {
          downstream->set_response_state(Downstream::MSG_RESET);
        }
      }
    }

    call_downstream_readcb(http2session, downstream);
    break;
  }
  case NGHTTP2_HEADERS: {
    auto sd = static_cast<StreamData *>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    if (!sd || !sd->dconn) {
      break;
    }
    auto downstream = sd->dconn->get_downstream();

    if (!downstream) {
      return 0;
    }

    if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
      rv = on_response_headers(http2session, downstream, session, frame);

      if (rv != 0) {
        return 0;
      }
    } else if (frame->headers.cat == NGHTTP2_HCAT_HEADERS) {
      if (downstream->get_expect_final_response()) {
        rv = on_response_headers(http2session, downstream, session, frame);

        if (rv != 0) {
          return 0;
        }
      } else if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) == 0) {
        http2session->submit_rst_stream(frame->hd.stream_id,
                                        NGHTTP2_PROTOCOL_ERROR);
        return 0;
      }
    }

    if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {

      downstream->disable_downstream_rtimer();

      if (downstream->get_response_state() == Downstream::HEADER_COMPLETE) {
        downstream->set_response_state(Downstream::MSG_COMPLETE);

        auto upstream = downstream->get_upstream();

        rv = upstream->on_downstream_body_complete(downstream);

        if (rv != 0) {
          downstream->set_response_state(Downstream::MSG_RESET);
        }
      }
    } else {
      downstream->reset_downstream_rtimer();
    }

    // This may delete downstream
    call_downstream_readcb(http2session, downstream);

    break;
  }
  case NGHTTP2_RST_STREAM: {
    auto sd = static_cast<StreamData *>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    if (sd && sd->dconn) {
      auto downstream = sd->dconn->get_downstream();
      if (downstream &&
          downstream->get_downstream_stream_id() == frame->hd.stream_id) {

        downstream->set_response_rst_stream_error_code(
            frame->rst_stream.error_code);
        call_downstream_readcb(http2session, downstream);
      }
    }
    break;
  }
  case NGHTTP2_SETTINGS:
    if ((frame->hd.flags & NGHTTP2_FLAG_ACK) == 0) {
      break;
    }
    http2session->stop_settings_timer();
    break;
  case NGHTTP2_PUSH_PROMISE:
    if (LOG_ENABLED(INFO)) {
      SSLOG(INFO, http2session)
          << "Received downstream PUSH_PROMISE stream_id="
          << frame->hd.stream_id
          << ", promised_stream_id=" << frame->push_promise.promised_stream_id;
    }
    // We just respond with RST_STREAM.
    http2session->submit_rst_stream(frame->push_promise.promised_stream_id,
                                    NGHTTP2_REFUSED_STREAM);
    break;
  default:
    break;
  }
  return 0;
}
} // namespace

namespace {
int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags,
                                int32_t stream_id, const uint8_t *data,
                                size_t len, void *user_data) {
  int rv;
  auto http2session = static_cast<Http2Session *>(user_data);
  auto sd = static_cast<StreamData *>(
      nghttp2_session_get_stream_user_data(session, stream_id));
  if (!sd || !sd->dconn) {
    http2session->submit_rst_stream(stream_id, NGHTTP2_INTERNAL_ERROR);

    if (http2session->consume(stream_id, len) != 0) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
  }
  auto downstream = sd->dconn->get_downstream();
  if (!downstream || downstream->get_downstream_stream_id() != stream_id ||
      !downstream->expect_response_body()) {

    http2session->submit_rst_stream(stream_id, NGHTTP2_INTERNAL_ERROR);

    if (http2session->consume(stream_id, len) != 0) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
  }

  // We don't want DATA after non-final response, which is illegal in
  // HTTP.
  if (downstream->get_non_final_response()) {
    http2session->submit_rst_stream(stream_id, NGHTTP2_PROTOCOL_ERROR);

    if (http2session->consume(stream_id, len) != 0) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
  }

  downstream->reset_downstream_rtimer();

  downstream->add_response_bodylen(len);

  auto upstream = downstream->get_upstream();
  rv = upstream->on_downstream_body(downstream, data, len, false);
  if (rv != 0) {
    http2session->submit_rst_stream(stream_id, NGHTTP2_INTERNAL_ERROR);

    if (http2session->consume(stream_id, len) != 0) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    downstream->set_response_state(Downstream::MSG_RESET);
  }

  downstream->add_response_datalen(len);

  call_downstream_readcb(http2session, downstream);
  return 0;
}
} // namespace

namespace {
int on_frame_send_callback(nghttp2_session *session, const nghttp2_frame *frame,
                           void *user_data) {
  auto http2session = static_cast<Http2Session *>(user_data);

  if (frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS) {
    if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) == 0) {
      return 0;
    }

    auto sd = static_cast<StreamData *>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));

    if (!sd || !sd->dconn) {
      return 0;
    }

    auto downstream = sd->dconn->get_downstream();

    if (!downstream ||
        downstream->get_downstream_stream_id() != frame->hd.stream_id) {
      return 0;
    }

    downstream->reset_downstream_rtimer();

    return 0;
  }

  if (frame->hd.type == NGHTTP2_SETTINGS &&
      (frame->hd.flags & NGHTTP2_FLAG_ACK) == 0) {
    http2session->start_settings_timer();
  }
  return 0;
}
} // namespace

namespace {
int on_frame_not_send_callback(nghttp2_session *session,
                               const nghttp2_frame *frame, int lib_error_code,
                               void *user_data) {
  auto http2session = static_cast<Http2Session *>(user_data);
  if (LOG_ENABLED(INFO)) {
    SSLOG(INFO, http2session) << "Failed to send control frame type="
                              << static_cast<uint32_t>(frame->hd.type)
                              << "lib_error_code=" << lib_error_code << ":"
                              << nghttp2_strerror(lib_error_code);
  }
  if (frame->hd.type == NGHTTP2_HEADERS &&
      frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
    // To avoid stream hanging around, flag Downstream::MSG_RESET and
    // terminate the upstream and downstream connections.
    auto sd = static_cast<StreamData *>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    if (!sd) {
      return 0;
    }
    if (sd->dconn) {
      auto downstream = sd->dconn->get_downstream();
      if (!downstream ||
          downstream->get_downstream_stream_id() != frame->hd.stream_id) {
        return 0;
      }
      downstream->set_response_state(Downstream::MSG_RESET);
      call_downstream_readcb(http2session, downstream);
    }
    http2session->remove_stream_data(sd);
  }
  return 0;
}
} // namespace

int Http2Session::on_connect() {
  int rv;

  state_ = Http2Session::CONNECTED;

  if (ssl_ctx_) {
    const unsigned char *next_proto = nullptr;
    unsigned int next_proto_len;
    SSL_get0_next_proto_negotiated(ssl_, &next_proto, &next_proto_len);
    for (int i = 0; i < 2; ++i) {
      if (next_proto) {
        if (LOG_ENABLED(INFO)) {
          std::string proto(next_proto, next_proto + next_proto_len);
          SSLOG(INFO, this) << "Negotiated next protocol: " << proto;
        }
        if (!util::check_h2_is_selected(next_proto, next_proto_len)) {
          return -1;
        }
        break;
      }
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
      SSL_get0_alpn_selected(ssl_, &next_proto, &next_proto_len);
#else  // OPENSSL_VERSION_NUMBER < 0x10002000L
      break;
#endif // OPENSSL_VERSION_NUMBER < 0x10002000L
    }
    if (!next_proto) {
      return -1;
    }
  }

  nghttp2_session_callbacks *callbacks;
  rv = nghttp2_session_callbacks_new(&callbacks);

  if (rv != 0) {
    return -1;
  }

  auto callbacks_deleter =
      util::defer(callbacks, nghttp2_session_callbacks_del);

  nghttp2_session_callbacks_set_on_stream_close_callback(
      callbacks, on_stream_close_callback);

  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                       on_frame_recv_callback);

  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
      callbacks, on_data_chunk_recv_callback);

  nghttp2_session_callbacks_set_on_frame_send_callback(callbacks,
                                                       on_frame_send_callback);

  nghttp2_session_callbacks_set_on_frame_not_send_callback(
      callbacks, on_frame_not_send_callback);

  nghttp2_session_callbacks_set_on_header_callback(callbacks,
                                                   on_header_callback);

  nghttp2_session_callbacks_set_on_begin_headers_callback(
      callbacks, on_begin_headers_callback);

  if (get_config()->padding) {
    nghttp2_session_callbacks_set_select_padding_callback(
        callbacks, http::select_padding_callback);
  }

  rv = nghttp2_session_client_new2(&session_, callbacks, this,
                                   get_config()->http2_option);

  if (rv != 0) {
    return -1;
  }

  flow_control_ = true;

  nghttp2_settings_entry entry[3];
  entry[0].settings_id = NGHTTP2_SETTINGS_ENABLE_PUSH;
  entry[0].value = 0;
  entry[1].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
  entry[1].value = get_config()->http2_max_concurrent_streams;

  entry[2].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
  entry[2].value = (1 << get_config()->http2_downstream_window_bits) - 1;

  rv = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, entry,
                               util::array_size(entry));
  if (rv != 0) {
    return -1;
  }

  if (get_config()->http2_downstream_connection_window_bits > 16) {
    int32_t delta =
        (1 << get_config()->http2_downstream_connection_window_bits) - 1 -
        NGHTTP2_INITIAL_CONNECTION_WINDOW_SIZE;
    rv = nghttp2_submit_window_update(session_, NGHTTP2_FLAG_NONE, 0, delta);
    if (rv != 0) {
      return -1;
    }
  }

  auto nwrite = wb_.write(NGHTTP2_CLIENT_CONNECTION_PREFACE,
                          NGHTTP2_CLIENT_CONNECTION_PREFACE_LEN);
  if (nwrite != NGHTTP2_CLIENT_CONNECTION_PREFACE_LEN) {
    SSLOG(FATAL, this) << "buffer is too small to send connection preface";
    return -1;
  }

  auto must_terminate =
      !get_config()->downstream_no_tls && !ssl::check_http2_requirement(ssl_);

  if (must_terminate) {
    rv = terminate_session(NGHTTP2_INADEQUATE_SECURITY);

    if (rv != 0) {
      return -1;
    }
  }

  if (must_terminate) {
    return 0;
  }

  reset_connection_check_timer();

  // submit pending request
  for (auto dconn : dconns_) {
    if (dconn->push_request_headers() == 0) {
      auto downstream = dconn->get_downstream();
      auto upstream = downstream->get_upstream();
      upstream->resume_read(SHRPX_NO_BUFFER, downstream, 0);
      continue;
    }

    if (LOG_ENABLED(INFO)) {
      SSLOG(INFO, this) << "backend request failed";
    }

    auto downstream = dconn->get_downstream();

    if (!downstream) {
      continue;
    }

    auto upstream = downstream->get_upstream();

    upstream->on_downstream_abort_request(downstream, 400);
  }
  signal_write();
  return 0;
}

int Http2Session::do_read() { return read_(*this); }
int Http2Session::do_write() { return write_(*this); }

int Http2Session::on_read() { return on_read_(*this); }
int Http2Session::on_write() { return on_write_(*this); }

int Http2Session::downstream_read() {
  ssize_t rv = 0;

  for (;;) {
    const void *data;
    size_t nread;
    std::tie(data, nread) = rb_.get();
    if (nread == 0) {
      break;
    }

    rv = nghttp2_session_mem_recv(
        session_, reinterpret_cast<const uint8_t *>(data), nread);

    if (rv < 0) {
      SSLOG(ERROR, this) << "nghttp2_session_recv() returned error: "
                         << nghttp2_strerror(rv);
      return -1;
    }

    rb_.drain(nread);
  }

  if (nghttp2_session_want_read(session_) == 0 &&
      nghttp2_session_want_write(session_) == 0 && wb_.rleft() == 0) {
    if (LOG_ENABLED(INFO)) {
      SSLOG(INFO, this) << "No more read/write for this HTTP2 session";
    }
    return -1;
  }

  signal_write();
  return 0;
}

int Http2Session::downstream_write() {
  if (data_pending_) {
    auto n = std::min(wb_.wleft(), data_pendinglen_);
    wb_.write(data_pending_, n);
    if (n < data_pendinglen_) {
      data_pending_ += n;
      data_pendinglen_ -= n;
      return 0;
    }

    data_pending_ = nullptr;
    data_pendinglen_ = 0;
  }

  for (;;) {
    const uint8_t *data;
    auto datalen = nghttp2_session_mem_send(session_, &data);

    if (datalen < 0) {
      SSLOG(ERROR, this) << "nghttp2_session_mem_send() returned error: "
                         << nghttp2_strerror(datalen);
      return -1;
    }
    if (datalen == 0) {
      break;
    }
    auto n = wb_.write(data, datalen);
    if (n < static_cast<decltype(n)>(datalen)) {
      data_pending_ = data + n;
      data_pendinglen_ = datalen - n;
      return 0;
    }
  }

  if (nghttp2_session_want_read(session_) == 0 &&
      nghttp2_session_want_write(session_) == 0 && wb_.rleft() == 0) {
    if (LOG_ENABLED(INFO)) {
      SSLOG(INFO, this) << "No more read/write for this session";
    }
    return -1;
  }

  return 0;
}

void Http2Session::signal_write() { write_requested_ = true; }

void Http2Session::clear_write_request() { write_requested_ = false; }

bool Http2Session::write_requested() const { return write_requested_; }

struct ev_loop *Http2Session::get_loop() const {
  return loop_;
}

ev_io *Http2Session::get_wev() { return &wev_; }

int Http2Session::get_state() const { return state_; }

void Http2Session::set_state(int state) { state_ = state; }

int Http2Session::terminate_session(uint32_t error_code) {
  int rv;
  rv = nghttp2_session_terminate_session(session_, error_code);
  if (rv != 0) {
    return -1;
  }
  return 0;
}

SSL *Http2Session::get_ssl() const { return ssl_; }

int Http2Session::consume(int32_t stream_id, size_t len) {
  int rv;

  if (!session_) {
    return 0;
  }

  rv = nghttp2_session_consume(session_, stream_id, len);

  if (rv != 0) {
    SSLOG(WARN, this) << "nghttp2_session_consume() returned error: "
                      << nghttp2_strerror(rv);

    return -1;
  }

  return 0;
}

bool Http2Session::can_push_request() const {
  return state_ == CONNECTED &&
         connection_check_state_ == CONNECTION_CHECK_NONE;
}

void Http2Session::start_checking_connection() {
  if (state_ != CONNECTED ||
      connection_check_state_ != CONNECTION_CHECK_REQUIRED) {
    return;
  }
  connection_check_state_ = CONNECTION_CHECK_STARTED;

  SSLOG(INFO, this) << "Start checking connection";
  // If connection is down, we may get error when writing data.  Issue
  // ping frame to see whether connection is alive.
  nghttp2_submit_ping(session_, NGHTTP2_FLAG_NONE, NULL);

  signal_write();
}

void Http2Session::reset_connection_check_timer() {
  ev_timer_again(loop_, &connchk_timer_);
}

void Http2Session::connection_alive() {
  reset_connection_check_timer();

  if (connection_check_state_ == CONNECTION_CHECK_NONE) {
    return;
  }

  SSLOG(INFO, this) << "Connection alive";
  connection_check_state_ = CONNECTION_CHECK_NONE;

  // submit pending request
  for (auto dconn : dconns_) {
    auto downstream = dconn->get_downstream();
    if (!downstream ||
        (downstream->get_request_state() != Downstream::HEADER_COMPLETE &&
         downstream->get_request_state() != Downstream::MSG_COMPLETE) ||
        downstream->get_response_state() != Downstream::INITIAL) {
      continue;
    }

    auto upstream = downstream->get_upstream();

    if (dconn->push_request_headers() == 0) {
      upstream->resume_read(SHRPX_NO_BUFFER, downstream, 0);
      continue;
    }

    if (LOG_ENABLED(INFO)) {
      SSLOG(INFO, this) << "backend request failed";
    }

    upstream->on_downstream_abort_request(downstream, 400);
  }
}

void Http2Session::set_connection_check_state(int state) {
  connection_check_state_ = state;
}

int Http2Session::noop() { return 0; }

int Http2Session::connected() {
  if (!util::check_socket_connected(fd_)) {
    return -1;
  }

  if (LOG_ENABLED(INFO)) {
    SSLOG(INFO, this) << "Connection established";
  }

  ev_io_start(loop_, &rev_);

  if (ssl_) {
    read_ = &Http2Session::tls_handshake;
    write_ = &Http2Session::tls_handshake;

    return do_write();
  }

  read_ = &Http2Session::read_clear;
  write_ = &Http2Session::write_clear;

  if (state_ == PROXY_CONNECTING) {
    return do_write();
  }

  if (on_connect() != 0) {
    state_ = CONNECT_FAILING;
    return -1;
  }

  return 0;
}

int Http2Session::read_clear() {
  ev_timer_again(loop_, &rt_);

  for (;;) {
    // we should process buffered data first before we read EOF.
    if (rb_.rleft() && on_read() != 0) {
      return -1;
    }
    if (rb_.rleft()) {
      return 0;
    }
    rb_.reset();
    struct iovec iov[2];
    auto iovcnt = rb_.wiovec(iov);

    if (iovcnt > 0) {
      ssize_t nread;
      while ((nread = readv(fd_, iov, iovcnt)) == -1 && errno == EINTR)
        ;
      if (nread == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        return -1;
      }

      if (nread == 0) {
        return -1;
      }

      rb_.write(nread);
    }
  }

  return 0;
}

int Http2Session::write_clear() {
  ev_timer_again(loop_, &rt_);

  for (;;) {
    if (wb_.rleft() > 0) {
      struct iovec iov[2];
      auto iovcnt = wb_.riovec(iov);

      ssize_t nwrite;
      while ((nwrite = writev(fd_, iov, iovcnt)) == -1 && errno == EINTR)
        ;
      if (nwrite == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          ev_io_start(loop_, &wev_);
          ev_timer_again(loop_, &wt_);
          return 0;
        }
        return -1;
      }
      wb_.drain(nwrite);
      continue;
    }

    wb_.reset();
    if (on_write() != 0) {
      return -1;
    }
    if (wb_.rleft() == 0) {
      break;
    }
  }

  ev_io_stop(loop_, &wev_);
  ev_timer_stop(loop_, &wt_);

  return 0;
}

int Http2Session::tls_handshake() {
  ev_timer_again(loop_, &rt_);

  ERR_clear_error();

  auto rv = SSL_do_handshake(ssl_);

  if (rv == 0) {
    return -1;
  }

  if (rv < 0) {
    auto err = SSL_get_error(ssl_, rv);
    switch (err) {
    case SSL_ERROR_WANT_READ:
      ev_io_stop(loop_, &wev_);
      ev_timer_stop(loop_, &wt_);
      return 0;
    case SSL_ERROR_WANT_WRITE:
      ev_io_start(loop_, &wev_);
      ev_timer_again(loop_, &wt_);
      return 0;
    default:
      return -1;
    }
  }

  ev_io_stop(loop_, &wev_);
  ev_timer_stop(loop_, &wt_);

  if (LOG_ENABLED(INFO)) {
    SSLOG(INFO, this) << "SSL/TLS handshake completed";
  }
  if (LOG_ENABLED(INFO)) {
    if (SSL_session_reused(ssl_)) {
      CLOG(INFO, this) << "SSL/TLS session reused";
    }
  }

  if (!get_config()->downstream_no_tls && !get_config()->insecure &&
      check_cert() != 0) {
    return -1;
  }

  read_ = &Http2Session::read_tls;
  write_ = &Http2Session::write_tls;

  if (on_connect() != 0) {
    state_ = CONNECT_FAILING;
    return -1;
  }

  return 0;
}

int Http2Session::read_tls() {
  ev_timer_again(loop_, &rt_);

  ERR_clear_error();

  for (;;) {
    // we should process buffered data first before we read EOF.
    if (rb_.rleft() && on_read() != 0) {
      return -1;
    }
    if (rb_.rleft()) {
      return 0;
    }
    rb_.reset();
    struct iovec iov[2];
    auto iovcnt = rb_.wiovec(iov);
    if (iovcnt == 0) {
      return 0;
    }

    auto rv = SSL_read(ssl_, iov[0].iov_base, iov[0].iov_len);

    if (rv == 0) {
      return -1;
    }

    if (rv < 0) {
      auto err = SSL_get_error(ssl_, rv);
      switch (err) {
      case SSL_ERROR_WANT_READ:
        return 0;
      case SSL_ERROR_WANT_WRITE:
        if (LOG_ENABLED(INFO)) {
          SSLOG(INFO, this) << "Close connection due to TLS renegotiation";
        }
        return -1;
      default:
        if (LOG_ENABLED(INFO)) {
          SSLOG(INFO, this) << "SSL_read: SSL_get_error returned " << err;
        }
        return -1;
      }
    }

    rb_.write(rv);
  }
}

int Http2Session::write_tls() {
  ev_timer_again(loop_, &rt_);

  ERR_clear_error();

  for (;;) {
    if (wb_.rleft() > 0) {
      const void *p;
      size_t len;
      std::tie(p, len) = wb_.get();

      auto rv = SSL_write(ssl_, p, len);

      if (rv == 0) {
        return -1;
      }

      if (rv < 0) {
        auto err = SSL_get_error(ssl_, rv);
        switch (err) {
        case SSL_ERROR_WANT_READ:
          if (LOG_ENABLED(INFO)) {
            SSLOG(INFO, this) << "Close connection due to TLS renegotiation";
          }
          return -1;
        case SSL_ERROR_WANT_WRITE:
          ev_io_start(loop_, &wev_);
          ev_timer_again(loop_, &wt_);
          return 0;
        default:
          if (LOG_ENABLED(INFO)) {
            SSLOG(INFO, this) << "SSL_write: SSL_get_error returned " << err;
          }
          return -1;
        }
      }

      wb_.drain(rv);

      continue;
    }
    wb_.reset();
    if (on_write() != 0) {
      return -1;
    }
    if (wb_.rleft() == 0) {
      break;
    }
  }

  ev_io_stop(loop_, &wev_);
  ev_timer_stop(loop_, &wt_);

  return 0;
}

bool Http2Session::should_hard_fail() const {
  switch (state_) {
  case PROXY_CONNECTING:
  case PROXY_FAILED:
  case CONNECTING:
  case CONNECT_FAILING:
    return true;
  default:
    return false;
  }
}

} // namespace shrpx
