/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2013 Tatsuhiro Tsujikawa
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
#include "http2_test.h"

#include <cassert>
#include <cstring>
#include <iostream>

#include <CUnit/CUnit.h>

#include "http-parser/http_parser.h"

#include "http2.h"
#include "util.h"

using namespace nghttp2;

#define MAKE_NV(K, V)                                                          \
  {                                                                            \
    (uint8_t *) K, (uint8_t *)V, sizeof(K) - 1, sizeof(V) - 1,                 \
        NGHTTP2_NV_FLAG_NONE                                                   \
  }

namespace shrpx {

namespace {
void check_nv(const Header &a, const nghttp2_nv *b) {
  CU_ASSERT(a.name.size() == b->namelen);
  CU_ASSERT(a.value.size() == b->valuelen);
  CU_ASSERT(memcmp(a.name.c_str(), b->name, b->namelen) == 0);
  CU_ASSERT(memcmp(a.value.c_str(), b->value, b->valuelen) == 0);
}
} // namespace

void test_http2_add_header(void) {
  auto nva = Headers();

  http2::add_header(nva, (const uint8_t *)"alpha", 5, (const uint8_t *)"123", 3,
                    false);
  CU_ASSERT(Headers::value_type("alpha", "123") == nva[0]);
  CU_ASSERT(!nva[0].no_index);

  nva.clear();

  http2::add_header(nva, (const uint8_t *)"alpha", 5, (const uint8_t *)"", 0,
                    true);
  CU_ASSERT(Headers::value_type("alpha", "") == nva[0]);
  CU_ASSERT(nva[0].no_index);

  nva.clear();

  http2::add_header(nva, (const uint8_t *)"a", 1, (const uint8_t *)" b", 2,
                    false);
  CU_ASSERT(Headers::value_type("a", "b") == nva[0]);

  nva.clear();

  http2::add_header(nva, (const uint8_t *)"a", 1, (const uint8_t *)"b ", 2,
                    false);
  CU_ASSERT(Headers::value_type("a", "b") == nva[0]);

  nva.clear();

  http2::add_header(nva, (const uint8_t *)"a", 1, (const uint8_t *)"  b  ", 5,
                    false);
  CU_ASSERT(Headers::value_type("a", "b") == nva[0]);

  nva.clear();

  http2::add_header(nva, (const uint8_t *)"a", 1, (const uint8_t *)"  bravo  ",
                    9, false);
  CU_ASSERT(Headers::value_type("a", "bravo") == nva[0]);

  nva.clear();

  http2::add_header(nva, (const uint8_t *)"a", 1, (const uint8_t *)"    ", 4,
                    false);
  CU_ASSERT(Headers::value_type("a", "") == nva[0]);
}

void test_http2_get_header(void) {
  auto nva = Headers{{"alpha", "1"},
                     {"bravo", "2"},
                     {"bravo", "3"},
                     {"charlie", "4"},
                     {"delta", "5"},
                     {"echo", "6"},
                     {"content-length", "7"}};
  const Headers::value_type *rv;
  rv = http2::get_header(nva, "delta");
  CU_ASSERT(rv != nullptr);
  CU_ASSERT("delta" == rv->name);

  rv = http2::get_header(nva, "bravo");
  CU_ASSERT(rv != nullptr);
  CU_ASSERT("bravo" == rv->name);

  rv = http2::get_header(nva, "foxtrot");
  CU_ASSERT(rv == nullptr);

  int hdidx[http2::HD_MAXIDX];
  http2::init_hdidx(hdidx);
  hdidx[http2::HD_CONTENT_LENGTH] = 6;
  rv = http2::get_header(hdidx, http2::HD_CONTENT_LENGTH, nva);
  CU_ASSERT("content-length" == rv->name);
}

namespace {
auto headers = Headers{{"alpha", "0", true},
                       {"bravo", "1"},
                       {"connection", "2"},
                       {"connection", "3"},
                       {"delta", "4"},
                       {"expect", "5"},
                       {"foxtrot", "6"},
                       {"tango", "7"},
                       {"te", "8"},
                       {"te", "9"},
                       {"x-forwarded-proto", "10"},
                       {"x-forwarded-proto", "11"},
                       {"zulu", "12"}};
} // namespace

void test_http2_copy_headers_to_nva(void) {
  std::vector<nghttp2_nv> nva;
  http2::copy_headers_to_nva(nva, headers);
  CU_ASSERT(9 == nva.size());
  auto ans = std::vector<int>{0, 1, 4, 5, 6, 7, 8, 9, 12};
  for (size_t i = 0; i < ans.size(); ++i) {
    check_nv(headers[ans[i]], &nva[i]);

    if (ans[i] == 0) {
      CU_ASSERT(nva[i].flags & NGHTTP2_NV_FLAG_NO_INDEX);
    } else {
      CU_ASSERT(NGHTTP2_NV_FLAG_NONE == nva[i].flags);
    }
  }
}

void test_http2_build_http1_headers_from_headers(void) {
  std::string hdrs;
  http2::build_http1_headers_from_headers(hdrs, headers);
  CU_ASSERT(hdrs == "Alpha: 0\r\n"
                    "Bravo: 1\r\n"
                    "Delta: 4\r\n"
                    "Expect: 5\r\n"
                    "Foxtrot: 6\r\n"
                    "Tango: 7\r\n"
                    "Te: 8\r\n"
                    "Te: 9\r\n"
                    "Zulu: 12\r\n");
}

void test_http2_lws(void) {
  CU_ASSERT(!http2::lws("alpha"));
  CU_ASSERT(http2::lws(" "));
  CU_ASSERT(http2::lws(""));
}

namespace {
void check_rewrite_location_uri(const std::string &new_uri,
                                const std::string &uri,
                                const std::string &req_host,
                                const std::string &upstream_scheme,
                                uint16_t upstream_port) {
  http_parser_url u;
  memset(&u, 0, sizeof(u));
  CU_ASSERT(0 == http_parser_parse_url(uri.c_str(), uri.size(), 0, &u));
  CU_ASSERT(new_uri == http2::rewrite_location_uri(
                           uri, u, req_host, upstream_scheme, upstream_port));
}
} // namespace

void test_http2_rewrite_location_uri(void) {
  check_rewrite_location_uri("https://localhost:3000/alpha?bravo#charlie",
                             "http://localhost:3001/alpha?bravo#charlie",
                             "localhost:3001", "https", 3000);
  check_rewrite_location_uri("https://localhost/", "http://localhost:3001/",
                             "localhost:3001", "https", 443);
  check_rewrite_location_uri("http://localhost/", "http://localhost:3001/",
                             "localhost:3001", "http", 80);
  check_rewrite_location_uri("http://localhost:443/", "http://localhost:3001/",
                             "localhost:3001", "http", 443);
  check_rewrite_location_uri("https://localhost:80/", "http://localhost:3001/",
                             "localhost:3001", "https", 80);
  check_rewrite_location_uri("", "http://localhost:3001/", "127.0.0.1", "https",
                             3000);
  check_rewrite_location_uri("https://localhost:3000/",
                             "http://localhost:3001/", "localhost", "https",
                             3000);
  check_rewrite_location_uri("", "https://localhost:3001/", "localhost",
                             "https", 3000);
  check_rewrite_location_uri("https://localhost:3000/", "http://localhost/",
                             "localhost", "https", 3000);
}

void test_http2_parse_http_status_code(void) {
  CU_ASSERT(200 == http2::parse_http_status_code("200"));
  CU_ASSERT(102 == http2::parse_http_status_code("102"));
  CU_ASSERT(-1 == http2::parse_http_status_code("099"));
  CU_ASSERT(-1 == http2::parse_http_status_code("99"));
  CU_ASSERT(-1 == http2::parse_http_status_code("-1"));
  CU_ASSERT(-1 == http2::parse_http_status_code("20a"));
  CU_ASSERT(-1 == http2::parse_http_status_code(""));
}

void test_http2_index_header(void) {
  int hdidx[http2::HD_MAXIDX];
  http2::init_hdidx(hdidx);

  http2::index_header(hdidx, http2::HD__AUTHORITY, 0);
  http2::index_header(hdidx, -1, 1);

  CU_ASSERT(0 == hdidx[http2::HD__AUTHORITY]);
}

void test_http2_lookup_token(void) {
  CU_ASSERT(http2::HD__AUTHORITY == http2::lookup_token(":authority"));
  CU_ASSERT(-1 == http2::lookup_token(":authorit"));
  CU_ASSERT(-1 == http2::lookup_token(":Authority"));
  CU_ASSERT(http2::HD_EXPECT == http2::lookup_token("expect"));
}

void test_http2_check_http2_pseudo_header(void) {
  int hdidx[http2::HD_MAXIDX];
  http2::init_hdidx(hdidx);

  CU_ASSERT(http2::check_http2_request_pseudo_header(hdidx, http2::HD__METHOD));
  hdidx[http2::HD__PATH] = 0;
  CU_ASSERT(http2::check_http2_request_pseudo_header(hdidx, http2::HD__METHOD));
  hdidx[http2::HD__METHOD] = 1;
  CU_ASSERT(
      !http2::check_http2_request_pseudo_header(hdidx, http2::HD__METHOD));
  CU_ASSERT(!http2::check_http2_request_pseudo_header(hdidx, http2::HD_VIA));

  http2::init_hdidx(hdidx);

  CU_ASSERT(
      http2::check_http2_response_pseudo_header(hdidx, http2::HD__STATUS));
  hdidx[http2::HD__STATUS] = 0;
  CU_ASSERT(
      !http2::check_http2_response_pseudo_header(hdidx, http2::HD__STATUS));
  CU_ASSERT(!http2::check_http2_response_pseudo_header(hdidx, http2::HD_VIA));
}

void test_http2_http2_header_allowed(void) {
  CU_ASSERT(http2::http2_header_allowed(http2::HD__PATH));
  CU_ASSERT(http2::http2_header_allowed(http2::HD_CONTENT_LENGTH));
  CU_ASSERT(!http2::http2_header_allowed(http2::HD_CONNECTION));
}

void test_http2_mandatory_request_headers_presence(void) {
  int hdidx[http2::HD_MAXIDX];
  http2::init_hdidx(hdidx);

  CU_ASSERT(!http2::http2_mandatory_request_headers_presence(hdidx));
  hdidx[http2::HD__AUTHORITY] = 0;
  CU_ASSERT(!http2::http2_mandatory_request_headers_presence(hdidx));
  hdidx[http2::HD__METHOD] = 1;
  CU_ASSERT(!http2::http2_mandatory_request_headers_presence(hdidx));
  hdidx[http2::HD__PATH] = 2;
  CU_ASSERT(!http2::http2_mandatory_request_headers_presence(hdidx));
  hdidx[http2::HD__SCHEME] = 3;
  CU_ASSERT(http2::http2_mandatory_request_headers_presence(hdidx));

  hdidx[http2::HD__AUTHORITY] = -1;
  hdidx[http2::HD_HOST] = 0;
  CU_ASSERT(http2::http2_mandatory_request_headers_presence(hdidx));
}

} // namespace shrpx
