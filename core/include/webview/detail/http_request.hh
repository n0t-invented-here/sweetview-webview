/*
 * MIT License
 *
 * Copyright (c) 2026 Mrowr Purr
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// Shared by the backends that answer requests through the request handler
// (see webview_set_request_handler): request headers to JSON, response
// headers from JSON, and the reason phrase for a status code.

#ifndef WEBVIEW_DETAIL_HTTP_REQUEST_HH
#define WEBVIEW_DETAIL_HTTP_REQUEST_HH

#if defined(__cplusplus) && !defined(WEBVIEW_HEADER)

#include "json.hh"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace webview {
namespace detail {

using http_header_t = std::pair<std::string, std::string>;
using http_headers_t = std::vector<http_header_t>;

// [["name","value"],...] — an array of pairs, so repeated headers survive.
inline std::string http_headers_to_json(const http_headers_t &headers) {
  std::string out = "[";
  bool first = true;
  for (const auto &h : headers) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += '[';
    out += json_escape(h.first);
    out += ',';
    out += json_escape(h.second);
    out += ']';
  }
  out += ']';
  return out;
}

// RFC 9110 5.1: a field name is a token, one or more tchar.
inline bool http_is_tchar(unsigned char c) {
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z')) {
    return true;
  }
  switch (c) {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '|':
  case '~':
    return true;
  default:
    return false;
  }
}

inline bool http_is_valid_header_name(const std::string &name) {
  if (name.empty()) {
    return false;
  }
  for (char c : name) {
    if (!http_is_tchar(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

// RFC 9110 5.5: a field value never holds CR, LF or NUL, and no other control
// character except horizontal tab. Bytes from 0x80 up (UTF-8, obs-text) pass.
inline bool http_is_valid_header_value(const std::string &value) {
  for (char ch : value) {
    auto c = static_cast<unsigned char>(ch);
    if ((c < 0x20 && c != '\t') || c == 0x7F) {
      return false;
    }
  }
  return true;
}

// A strict reader for exactly what response headers need: a JSON object of
// name to value, or a JSON array of [name, value] pairs. Values may be
// strings, numbers, booleans or null (null drops the header). Follows the
// JSON grammar (RFC 8259) to the letter: literals end where the grammar says,
// strings hold no raw control characters, every \u escape is a whole code
// point (no lone surrogates), and the text is valid UTF-8. U+0000 is refused
// as well, raw or escaped, since names and values become C strings.
class http_headers_json_reader {
public:
  explicit http_headers_json_reader(const std::string &s) : m_s(s) {}

  bool read(http_headers_t &out) {
    skip_ws();
    if (m_pos >= m_s.size()) {
      return true; // empty input means no headers
    }
    bool ok = false;
    if (m_s[m_pos] == '{') {
      ok = read_object(out);
    } else if (m_s[m_pos] == '[') {
      ok = read_array(out);
    }
    if (!ok) {
      return false;
    }
    skip_ws();
    return m_pos == m_s.size();
  }

private:
  void skip_ws() {
    while (m_pos < m_s.size() &&
           (m_s[m_pos] == ' ' || m_s[m_pos] == '\t' || m_s[m_pos] == '\n' ||
            m_s[m_pos] == '\r')) {
      ++m_pos;
    }
  }

  bool consume(char c) {
    skip_ws();
    if (m_pos < m_s.size() && m_s[m_pos] == c) {
      ++m_pos;
      return true;
    }
    return false;
  }

  static void append_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool read_hex4(uint32_t &cp) {
    if (m_pos + 4 > m_s.size()) {
      return false;
    }
    cp = 0;
    for (int i = 0; i < 4; ++i) {
      char c = m_s[m_pos++];
      cp <<= 4;
      if (c >= '0' && c <= '9') {
        cp |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        cp |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        cp |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return false;
      }
    }
    return true;
  }

  // Copies one raw UTF-8 sequence starting at m_pos (its lead byte already
  // known to be 0x80 or above), refusing overlong forms, surrogates, and
  // anything above U+10FFFF.
  bool read_utf8_sequence(std::string &out) {
    auto lead = static_cast<unsigned char>(m_s[m_pos]);
    size_t len = 0;
    uint32_t cp = 0;
    uint32_t min = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
      len = 2;
      cp = lead & 0x1Fu;
      min = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      len = 3;
      cp = lead & 0x0Fu;
      min = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      len = 4;
      cp = lead & 0x07u;
      min = 0x10000;
    } else {
      return false;
    }
    if (m_pos + len > m_s.size()) {
      return false;
    }
    for (size_t i = 1; i < len; ++i) {
      auto c = static_cast<unsigned char>(m_s[m_pos + i]);
      if ((c & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | (c & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    out.append(m_s, m_pos, len);
    m_pos += len;
    return true;
  }

  bool read_string(std::string &out) {
    skip_ws();
    if (m_pos >= m_s.size() || m_s[m_pos] != '"') {
      return false;
    }
    ++m_pos;
    while (m_pos < m_s.size()) {
      auto c = static_cast<unsigned char>(m_s[m_pos]);
      if (c == '"') {
        ++m_pos;
        return true;
      }
      if (c < 0x20) {
        return false; // raw control characters, NUL included
      }
      if (c >= 0x80) {
        if (!read_utf8_sequence(out)) {
          return false;
        }
        continue;
      }
      ++m_pos;
      if (c != '\\') {
        out += static_cast<char>(c);
        continue;
      }
      if (m_pos >= m_s.size()) {
        return false;
      }
      char e = m_s[m_pos++];
      switch (e) {
      case '"':
        out += '"';
        break;
      case '\\':
        out += '\\';
        break;
      case '/':
        out += '/';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u': {
        uint32_t cp = 0;
        if (!read_hex4(cp)) {
          return false;
        }
        if (cp == 0) {
          return false; // U+0000 would cut the C string short
        }
        if (cp >= 0xDC00 && cp <= 0xDFFF) {
          return false; // a low surrogate with no high one before it
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          // A high surrogate must be followed at once by a low one.
          if (m_pos + 2 > m_s.size() || m_s[m_pos] != '\\' ||
              m_s[m_pos + 1] != 'u') {
            return false;
          }
          m_pos += 2;
          uint32_t lo = 0;
          if (!read_hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) {
            return false;
          }
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
        append_utf8(out, cp);
        break;
      }
      default:
        return false;
      }
    }
    return false;
  }

  bool read_digits() {
    auto start = m_pos;
    while (m_pos < m_s.size() && m_s[m_pos] >= '0' && m_s[m_pos] <= '9') {
      ++m_pos;
    }
    return m_pos > start;
  }

  // RFC 8259 6: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
  bool read_number(std::string &out) {
    auto start = m_pos;
    if (m_pos < m_s.size() && m_s[m_pos] == '-') {
      ++m_pos;
    }
    if (m_pos >= m_s.size()) {
      return false;
    }
    if (m_s[m_pos] == '0') {
      ++m_pos;
    } else if (!read_digits()) {
      return false;
    }
    if (m_pos < m_s.size() && m_s[m_pos] == '.') {
      ++m_pos;
      if (!read_digits()) {
        return false;
      }
    }
    if (m_pos < m_s.size() && (m_s[m_pos] == 'e' || m_s[m_pos] == 'E')) {
      ++m_pos;
      if (m_pos < m_s.size() && (m_s[m_pos] == '+' || m_s[m_pos] == '-')) {
        ++m_pos;
      }
      if (!read_digits()) {
        return false;
      }
    }
    out = m_s.substr(start, m_pos - start);
    return true;
  }

  bool read_literal(const char *word) {
    auto start = m_pos;
    for (const char *p = word; *p; ++p, ++m_pos) {
      if (m_pos >= m_s.size() || m_s[m_pos] != *p) {
        m_pos = start;
        return false;
      }
    }
    return true;
  }

  // A string, or the literal text of a number / true / false. null sets
  // is_null and yields an empty value. Whatever follows is left to the
  // caller, which accepts only ',' or the closing bracket, so "trueish" or
  // "12abc" fail there.
  bool read_value(std::string &out, bool &is_null) {
    is_null = false;
    skip_ws();
    if (m_pos >= m_s.size()) {
      return false;
    }
    char c = m_s[m_pos];
    if (c == '"') {
      return read_string(out);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      return read_number(out);
    }
    if (read_literal("true")) {
      out = "true";
      return true;
    }
    if (read_literal("false")) {
      out = "false";
      return true;
    }
    if (read_literal("null")) {
      out.clear();
      is_null = true;
      return true;
    }
    return false;
  }

  static bool keep_header(http_headers_t &out, std::string &name,
                          std::string &value, bool is_null) {
    if (!http_is_valid_header_name(name)) {
      return false;
    }
    if (is_null) {
      return true; // null drops the header
    }
    if (!http_is_valid_header_value(value)) {
      return false;
    }
    out.emplace_back(std::move(name), std::move(value));
    return true;
  }

  bool read_object(http_headers_t &out) {
    if (!consume('{')) {
      return false;
    }
    if (consume('}')) {
      return true;
    }
    for (;;) {
      std::string name;
      std::string value;
      bool is_null = false;
      if (!read_string(name) || !consume(':') || !read_value(value, is_null) ||
          !keep_header(out, name, value, is_null)) {
        return false;
      }
      if (consume(',')) {
        continue;
      }
      return consume('}');
    }
  }

  bool read_array(http_headers_t &out) {
    if (!consume('[')) {
      return false;
    }
    if (consume(']')) {
      return true;
    }
    for (;;) {
      std::string name;
      std::string value;
      bool is_null = false;
      if (!consume('[') || !read_string(name) || !consume(',') ||
          !read_value(value, is_null) || !consume(']') ||
          !keep_header(out, name, value, is_null)) {
        return false;
      }
      if (consume(',')) {
        continue;
      }
      return consume(']');
    }
  }

  const std::string &m_s;
  size_t m_pos = 0;
};

// Parses a JSON object {"Name": "value"} or a JSON array of pairs
// [["Name", "value"], ...] under the strict rules of
// http_headers_json_reader. Every name must be an RFC 9110 token and every
// value free of CR, LF, NUL and other control characters but tab; a header
// that breaks either rule fails the whole parse, so nothing can be injected.
// On success out holds the headers; on failure out is left as it was.
inline bool http_headers_from_json(const std::string &json,
                                   http_headers_t &out) {
  http_headers_t headers;
  http_headers_json_reader reader{json};
  if (!reader.read(headers)) {
    return false;
  }
  out.swap(headers);
  return true;
}

inline const char *http_reason_phrase(int status) {
  switch (status) {
  case 100:
    return "Continue";
  case 101:
    return "Switching Protocols";
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 202:
    return "Accepted";
  case 203:
    return "Non-Authoritative Information";
  case 204:
    return "No Content";
  case 205:
    return "Reset Content";
  case 206:
    return "Partial Content";
  case 300:
    return "Multiple Choices";
  case 301:
    return "Moved Permanently";
  case 302:
    return "Found";
  case 303:
    return "See Other";
  case 304:
    return "Not Modified";
  case 307:
    return "Temporary Redirect";
  case 308:
    return "Permanent Redirect";
  case 400:
    return "Bad Request";
  case 401:
    return "Unauthorized";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 406:
    return "Not Acceptable";
  case 408:
    return "Request Timeout";
  case 409:
    return "Conflict";
  case 410:
    return "Gone";
  case 411:
    return "Length Required";
  case 412:
    return "Precondition Failed";
  case 413:
    return "Content Too Large";
  case 414:
    return "URI Too Long";
  case 415:
    return "Unsupported Media Type";
  case 416:
    return "Range Not Satisfiable";
  case 422:
    return "Unprocessable Content";
  case 429:
    return "Too Many Requests";
  case 500:
    return "Internal Server Error";
  case 501:
    return "Not Implemented";
  case 502:
    return "Bad Gateway";
  case 503:
    return "Service Unavailable";
  case 504:
    return "Gateway Timeout";
  default:
    break;
  }
  if (status >= 200 && status < 300) {
    return "OK";
  }
  if (status >= 300 && status < 400) {
    return "Redirect";
  }
  if (status >= 400 && status < 500) {
    return "Client Error";
  }
  if (status >= 500 && status < 600) {
    return "Server Error";
  }
  return "Unknown";
}

} // namespace detail
} // namespace webview

#endif // defined(__cplusplus) && !defined(WEBVIEW_HEADER)
#endif // WEBVIEW_DETAIL_HTTP_REQUEST_HH
