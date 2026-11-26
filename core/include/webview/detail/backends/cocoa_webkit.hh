/*
 * MIT License
 *
 * Copyright (c) 2017 Serge Zaitsev
 * Copyright (c) 2022 Steffen André Langnes
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

#ifndef WEBVIEW_BACKENDS_COCOA_WEBKIT_HH
#define WEBVIEW_BACKENDS_COCOA_WEBKIT_HH

#if defined(__cplusplus) && !defined(WEBVIEW_HEADER)

#include "../../macros.h"

#if defined(WEBVIEW_PLATFORM_DARWIN) && defined(WEBVIEW_COCOA)

//
// ====================================================================
//
// This implementation uses Cocoa WKWebView backend on macOS. It is
// written using ObjC runtime and uses WKWebView class as a browser runtime.
// You should pass "-framework Webkit" flag to the compiler.
//
// ====================================================================
//

#include "../../types.hh"
#include "../engine_base.hh"
#include "../http_request.hh"
#include "../platform/darwin/cocoa/cocoa.hh"
#include "../platform/darwin/objc/objc.hh"
#include "../platform/darwin/webkit/webkit.hh"
#include "../user_script.hh"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>
#include <objc/objc-runtime.h>

namespace webview {
namespace detail {

class user_script::impl {
public:
  impl(id script) : m_script{objc::retain(script)} {}

  ~impl() { objc::release(m_script); }

  impl(const impl &) = delete;
  impl &operator=(const impl &) = delete;
  impl(impl &&) = delete;
  impl &operator=(impl &&) = delete;

  id get_native() const { return m_script; }

private:
  id m_script{};
};

// Encapsulate backend in its own namespace to avoid polluting the parent
// namespace when pulling in commonly-used symbols from other namespaces.
// Since those commmon symbols are used a lot, this reduces the overall
// noise in the code.
namespace cocoa_webkit {

using namespace cocoa;
using namespace webkit;

class cocoa_wkwebview_engine : public engine_base {
  // Where one drag session over the WKWebView goes; see get_webview_class.
  enum class drag_route_t { webkit, file_drop, rejected };

public:
  cocoa_wkwebview_engine(bool debug, void *window)
      : engine_base{!window}, m_app{NSApplication_get_sharedApplication()} {
    window_init(window);
    window_settings(debug);
    dispatch_size_default();
  }

  cocoa_wkwebview_engine(const cocoa_wkwebview_engine &) = delete;
  cocoa_wkwebview_engine &operator=(const cocoa_wkwebview_engine &) = delete;
  cocoa_wkwebview_engine(cocoa_wkwebview_engine &&) = delete;
  cocoa_wkwebview_engine &operator=(cocoa_wkwebview_engine &&) = delete;

  virtual ~cocoa_wkwebview_engine() {
    objc::autoreleasepool arp;
    // Answers webview_complete_request queued on the main queue do nothing
    // from here on, even those that run after this engine is deleted.
    mark_destroying();
    release_url_scheme_state();
    if (m_window && m_widget &&
        m_widget == NSWindow_get_contentView(m_window)) {
      NSWindow_set_contentView(m_window, nullptr);
    }
    // Drags and script messages still reaching the views must not reach this
    // engine; does nothing when windowWillClose: already did it.
    detach_and_release_views();
    if (m_window) {
      if (owns_window()) {
        // Replace delegate to avoid callbacks and other bad things during
        // destruction.
        NSWindow_set_delegate(m_window, nullptr);
        NSWindow_close(m_window);
        on_window_destroyed(true);
      }
      m_window = nullptr;
    }
    if (m_window_delegate) {
      objc::release(m_window_delegate);
      m_window_delegate = nullptr;
    }
    if (m_app_delegate) {
      NSApplication_set_delegate(m_app, nullptr);
      // Make sure to release the delegate we created.
      objc::release(m_app_delegate);
      m_app_delegate = nullptr;
    }
    if (owns_window()) {
      // Needed for the window to close immediately.
      deplete_run_loop_event_queue();
    }
  }

protected:
  result<void *> window_impl() override {
    if (m_window) {
      return m_window;
    }
    return error_info{WEBVIEW_ERROR_INVALID_STATE};
  }

  result<void *> widget_impl() override {
    if (m_widget) {
      return m_widget;
    }
    return error_info{WEBVIEW_ERROR_INVALID_STATE};
  }

  result<void *> browser_controller_impl() override {
    if (m_webview) {
      return m_webview;
    }
    return error_info{WEBVIEW_ERROR_INVALID_STATE};
  }

  noresult terminate_impl() override {
    stop_run_loop();
    return {};
  }

  noresult run_impl() override {
    NSApplication_run(m_app);
    return {};
  }

  noresult dispatch_impl(std::function<void()> f) override {
    dispatch_async_f(dispatch_get_main_queue(), new dispatch_fn_t(f),
                     (dispatch_function_t)([](void *arg) {
                       auto f = static_cast<dispatch_fn_t *>(arg);
                       (*f)();
                       delete f;
                     }));
    return {};
  }

  noresult set_title_impl(const std::string &title) override {
    NSWindow_set_title(m_window, title);
    return {};
  }
  noresult set_background_color_impl(int r, int g, int b, int a) override {
    objc::autoreleasepool arp;
    // m_webview exists from the constructor (window_settings) until the
    // window closes, so a null here is a browser already gone, not one still
    // being made: there is nothing to keep for later.
    if (!m_webview) {
      return error_info{WEBVIEW_ERROR_INVALID_STATE, "WebView is null"};
    }
    // Kept so the SweetviewWKWebView updateLayer override can put the color
    // back after WebKit's own layer update resets it.
    m_has_background_color = true;
    m_bg_r = r;
    m_bg_g = g;
    m_bg_b = b;
    m_bg_a = a;
    // The color is painted by the WKWebView's own backing layer, behind the
    // page. WebKit's default (white) background is turned off for every
    // color, opaque or not, or it would cover the layer. WKWebView has no
    // public API for that on macOS; "drawsBackground" is the long-standing
    // KVC key Safari-based apps and wry use.
    NSObject_setValue_forKey(m_webview, NSNumber_numberWithBool(false),
                             NSString_stringWithUTF8String("drawsBackground"));
    objc::msg_send<void>(m_webview, objc::selector("setWantsLayer:"),
                         static_cast<BOOL>(true));
    apply_layer_background_color();
    objc::msg_send<void>(m_webview, objc::selector("setNeedsDisplay:"),
                         static_cast<BOOL>(true));
    // Autoreleased NSColor for the owned window's opaque background below.
    auto color = objc::msg_send<id>(
        objc::get_class("NSColor"),
        objc::selector("colorWithSRGBRed:green:blue:alpha:"),
        static_cast<CGFloat>(r / 255.0), static_cast<CGFloat>(g / 255.0),
        static_cast<CGFloat>(b / 255.0), static_cast<CGFloat>(a / 255.0));
    if (m_window) {
      if (a < 255) {
        // A translucent layer shows what is behind it only when the window
        // itself is clear, owned or adopted alike.
        objc::msg_send<void>(m_window, objc::selector("setOpaque:"),
                             static_cast<BOOL>(false));
        objc::msg_send<void>(
            m_window, objc::selector("setBackgroundColor:"),
            objc::msg_send<id>(objc::get_class("NSColor"),
                               objc::selector("clearColor")));
      } else if (owns_window()) {
        // An opaque color restores opacity only on a window this library
        // made; an adopted window's own transparency settings are the
        // host's and are left as they are.
        objc::msg_send<void>(m_window, objc::selector("setOpaque:"),
                             static_cast<BOOL>(true));
        objc::msg_send<void>(m_window, objc::selector("setBackgroundColor:"),
                             color);
      }
    }
    return {};
  }

  // Puts the stored background color on the WKWebView's backing layer.
  // Called from set_background_color_impl and after every WebKit
  // updateLayer pass, which resets the layer's backgroundColor.
  void apply_layer_background_color() {
    if (!m_has_background_color || !m_webview) {
      return;
    }
    objc::autoreleasepool arp;
    auto layer = objc::msg_send<id>(m_webview, objc::selector("layer"));
    if (!layer) {
      return;
    }
    // Autoreleased NSColor; its CGColor lives as long as it does (this
    // pool), and the layer retains what it is given.
    auto color = objc::msg_send<id>(
        objc::get_class("NSColor"),
        objc::selector("colorWithSRGBRed:green:blue:alpha:"),
        static_cast<CGFloat>(m_bg_r / 255.0),
        static_cast<CGFloat>(m_bg_g / 255.0),
        static_cast<CGFloat>(m_bg_b / 255.0),
        static_cast<CGFloat>(m_bg_a / 255.0));
    objc::msg_send<void>(
        layer, objc::selector("setBackgroundColor:"),
        objc::msg_send<void *>(color, objc::selector("CGColor")));
  }

  noresult set_size_impl(int width, int height, webview_hint_t hints) override {
    objc::autoreleasepool arp;

    if (!owns_window()) {
      // The host created the window; its style and placement are its own.
      return {};
    }

    auto style = static_cast<NSWindowStyleMask>(
        NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
        NSWindowStyleMaskMiniaturizable);
    if (hints != WEBVIEW_HINT_FIXED) {
      style =
          static_cast<NSWindowStyleMask>(style | NSWindowStyleMaskResizable);
    }
    NSWindow_set_styleMask(m_window, style);

    if (hints == WEBVIEW_HINT_MIN) {
      NSWindow_set_contentMinSize(m_window, NSSizeMake(width, height));
    } else if (hints == WEBVIEW_HINT_MAX) {
      NSWindow_set_contentMaxSize(m_window, NSSizeMake(width, height));
    } else {
      auto rect{NSWindow_get_frame(m_window)};
      NSWindow_setFrame(m_window,
                        NSRectMake(rect.origin.x, rect.origin.y, width, height),
                        true, false);
    }
    NSWindow_center(m_window);

    return window_show();
  }
  noresult navigate_impl(const std::string &url) override {
    objc::autoreleasepool arp;

    WKWebView_loadRequest(
        m_webview, NSURLRequest_requestWithURL(NSURL_URLWithString(url)));

    return {};
  }
  noresult set_html_impl(const std::string &html) override {
    objc::autoreleasepool arp;
    WKWebView_loadHTMLString(m_webview, NSString_stringWithUTF8String(html),
                             nullptr);
    return {};
  }
  noresult eval_impl(const std::string &js) override {
    objc::autoreleasepool arp;
    // URI is null before content has begun loading.
    auto nsurl{WKWebView_get_URL(m_webview)};
    if (!nsurl) {
      return {};
    }
    WKWebView_evaluateJavaScript(m_webview, NSString_stringWithUTF8String(js),
                                 nullptr);
    return {};
  }

  user_script add_user_script_impl(const std::string &js) override {
    objc::autoreleasepool arp;
    auto wk_script{WKUserScript_withSource(
        NSString_stringWithUTF8String(js),
        WKUserScriptInjectionTimeAtDocumentStart, true)};
    // Script is retained when added.
    WKUserContentController_addUserScript(m_manager, wk_script);
    user_script script{
        js, user_script::impl_ptr{new user_script::impl{wk_script},
                                  [](user_script::impl *p) { delete p; }}};
    return script;
  }

  void remove_all_user_scripts_impl(
      const std::list<user_script> & /*scripts*/) override {
    objc::autoreleasepool arp;
    // Removing scripts decreases the retain count of each script.
    WKUserContentController_removeAllUserScripts(m_manager);
  }

  bool are_user_scripts_equal_impl(const user_script &first,
                                   const user_script &second) override {
    auto *wk_first = first.get_impl().get_native();
    auto *wk_second = second.get_impl().get_native();
    return wk_first == wk_second;
  }

private:
  id create_app_delegate() {
    objc::autoreleasepool arp;
    constexpr auto class_name = "WebviewAppDelegate";
    // Avoid crash due to registering same class twice
    auto cls = objc_lookUpClass(class_name);
    if (!cls) {
      // Note: Avoid registering the class name "AppDelegate" as it is the
      // default name in projects created with Xcode, and using the same name
      // causes objc_registerClassPair to crash.
      cls =
          objc_allocateClassPair(objc::get_class("NSResponder"), class_name, 0);
      class_addProtocol(cls, objc_getProtocol("NSTouchBarProvider"));
      // BOOL is a C bool on arm64 and a signed char on x86_64; the type
      // encoding must say which.
#if defined(__arm64__) || defined(__aarch64__)
      constexpr auto should_terminate_types = "B@:@";
#else
      constexpr auto should_terminate_types = "c@:@";
#endif
      class_addMethod(
          cls,
          objc::selector("applicationShouldTerminateAfterLastWindowClosed:"),
          (IMP)(+[](id, SEL, id) -> BOOL { return NO; }),
          should_terminate_types);
      class_addMethod(cls, objc::selector("applicationDidFinishLaunching:"),
                      (IMP)(+[](id self, SEL, id notification) {
                        auto app{NSNotification_get_object(notification)};
                        auto w = get_associated_webview(self);
                        w->on_application_did_finish_launching(self, app);
                      }),
                      "v@:@");
      objc_registerClassPair(cls);
    }
    return objc::Class_new(cls);
  }
  // Answers webview://localhost/<path> from the asset table (see
  // webview_set_asset), and what it misses, plus every request to
  // webview-frames://localhost/<path>, from the request handler (see
  // webview_set_request_handler). Registered on the configuration before the
  // WKWebView exists, as WebKit requires.
  id create_url_scheme_handler() {
    objc::autoreleasepool arp;
    constexpr auto class_name = "WebviewWKURLSchemeHandler";
    // Avoid crash due to registering same class twice
    auto cls = objc_lookUpClass(class_name);
    if (!cls) {
      cls = objc_allocateClassPair(objc::get_class("NSObject"), class_name, 0);
      class_addProtocol(cls, objc_getProtocol("WKURLSchemeHandler"));
      class_addMethod(cls, objc::selector("webView:startURLSchemeTask:"),
                      (IMP)(+[](id self, SEL, id, id task) {
                        if (auto w = get_associated_webview(self)) {
                          w->on_url_scheme_task(task);
                          return;
                        }
                        // The engine is gone but something still holds the
                        // WKWebView (a host that retained the view): fail
                        // the task (NSURLErrorCancelled) rather than leave
                        // the page's request open forever.
                        objc::autoreleasepool arp;
                        objc::msg_send<void>(
                            task, objc::selector("didFailWithError:"),
                            objc::msg_send<id>(
                                objc::get_class("NSError"),
                                objc::selector(
                                    "errorWithDomain:code:userInfo:"),
                                NSString_stringWithUTF8String(
                                    "NSURLErrorDomain"),
                                static_cast<NSInteger>(-999),
                                static_cast<id>(nullptr)));
                      }),
                      "v@:@@");
      // WebKit stops a task when the page no longer wants it. Answering a
      // stopped task raises NSInternalInconsistencyException, so it is
      // forgotten here and webview_complete_request finds nothing to answer.
      class_addMethod(cls, objc::selector("webView:stopURLSchemeTask:"),
                      (IMP)(+[](id self, SEL, id, id task) {
                        if (auto w = get_associated_webview(self)) {
                          w->on_url_scheme_task_stopped(task);
                        }
                      }),
                      "v@:@@");
      objc_registerClassPair(cls);
    }
    auto instance{objc::Class_new(cls)};
    set_associated_webview(instance, this);
    return instance;
  }

  // Answers a task with a complete HTTP-style response.
  static void respond_to_url_scheme_task(id task, id url, int status,
                                         const http_headers_t &headers,
                                         const char *body, size_t body_len) {
    objc::autoreleasepool arp;
    send_url_scheme_response_head(task, url, status, headers);
    send_url_scheme_response_data(task, body, body_len);
    objc::msg_send<void>(task, objc::selector("didFinish"));
  }

  // Sends a task its status and headers (didReceiveResponse:).
  static void send_url_scheme_response_head(id task, id url, int status,
                                            const http_headers_t &headers) {
    objc::autoreleasepool arp;
    // An HTTP-style response, so WebKit reads the Content-Type (parameters
    // and all, "text/html; charset=utf-8") the way it would from a server. A
    // plain NSURLResponse takes a bare MIME type; given one with parameters
    // it shows HTML as text and refuses module scripts.
    //
    // NSHTTPURLResponse holds one value per name, so repeated names are
    // merged first. Names match without regard to case (RFC 9110 5.1) and
    // keep the casing first seen. Repeats are joined with ", " the way HTTP
    // allows, except Set-Cookie, which cannot be joined (RFC 9110 5.3): the
    // first Set-Cookie is kept and later ones are dropped.
    struct merged_header_t {
      std::string lower_name;
      std::string name;
      std::string value;
    };
    std::vector<merged_header_t> merged;
    for (const auto &h : headers) {
      auto lower_name = h.first;
      for (auto &c : lower_name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      auto found = std::find_if(
          merged.begin(), merged.end(),
          [&](const merged_header_t &m) { return m.lower_name == lower_name; });
      if (found == merged.end()) {
        merged.push_back(merged_header_t{lower_name, h.first, h.second});
      } else if (lower_name != "set-cookie") {
        found->value += ", ";
        found->value += h.second;
      }
    }
    auto dict = objc::msg_send<id>(objc::get_class("NSMutableDictionary"),
                                   objc::selector("dictionary"));
    for (const auto &m : merged) {
      auto key = NSString_stringWithUTF8String(m.name);
      auto value = NSString_stringWithUTF8String(m.value);
      if (!key || !value) {
        continue; // not valid UTF-8
      }
      objc::msg_send<void>(dict, objc::selector("setObject:forKey:"), value,
                           key);
    }
    auto response = objc::autorelease(objc::msg_send<id>(
        objc::msg_send<id>(objc::get_class("NSHTTPURLResponse"),
                           objc::selector("alloc")),
        objc::selector("initWithURL:statusCode:HTTPVersion:headerFields:"),
        url, static_cast<NSInteger>(status),
        NSString_stringWithUTF8String("HTTP/1.1"), dict));
    objc::msg_send<void>(task, objc::selector("didReceiveResponse:"), response);
  }

  // Sends a task part of its body (didReceiveData:); nothing when empty.
  static void send_url_scheme_response_data(id task, const char *body,
                                            size_t body_len) {
    if (!body || body_len == 0) {
      return;
    }
    objc::autoreleasepool arp;
    auto data = objc::msg_send<id>(
        objc::get_class("NSData"), objc::selector("dataWithBytes:length:"),
        static_cast<const void *>(body), static_cast<NSUInteger>(body_len));
    objc::msg_send<void>(task, objc::selector("didReceiveData:"), data);
  }

  static std::string nsstring_to_string(id s) {
    if (!s) {
      return {};
    }
    auto utf8 = NSString_get_UTF8String(s);
    return utf8 ? std::string(utf8) : std::string{};
  }

  void on_url_scheme_task(id task) {
    objc::autoreleasepool arp;
    auto request = objc::msg_send<id>(task, objc::selector("request"));
    auto url = objc::msg_send<id>(request, objc::selector("URL"));
    auto url_string = nsstring_to_string(
        objc::msg_send<id>(url, objc::selector("absoluteString")));

    // Split and matched the same way as on Windows (http_split_request_url):
    // the scheme and the exact authority "localhost" pick the origin, and
    // the raw path (not NSURL's percent-decoded path, which also drops a
    // trailing slash) without query or fragment is looked up. Any other
    // authority ("webview://other-host/index.html", a port, userinfo) is
    // refused here, before the asset table or the request handler.
    http_request_url_t parts;
    auto origin_kind = http_split_request_url(url_string, parts)
                           ? http_classify_request_origin(parts)
                           : http_request_origin_t::other;
    if (origin_kind == http_request_origin_t::other) {
      http_headers_t headers{{"Content-Type", "text/plain"},
                             {"Content-Length", "0"}};
      respond_to_url_scheme_task(task, url, 404, headers, nullptr, 0);
      return;
    }

    // The asset table answers the main origin first, at no extra cost.
    if (origin_kind == http_request_origin_t::main) {
      if (auto *asset = find_asset(parts.path)) {
        http_headers_t headers{
            {"Content-Type", asset->mime},
            {"Content-Length", std::to_string(asset->data.size())}};
        respond_to_url_scheme_task(task, url, 200, headers, asset->data.data(),
                                   asset->data.size());
        return;
      }
    }

    auto handler = request_handler();
    if (!handler) {
      http_headers_t headers{{"Content-Type", "text/plain"},
                             {"Content-Length", "0"}};
      respond_to_url_scheme_task(task, url, 404, headers, nullptr, 0);
      return;
    }

    auto method = nsstring_to_string(
        objc::msg_send<id>(request, objc::selector("HTTPMethod")));
    if (method.empty()) {
      method = "GET";
    }

    http_headers_t request_headers;
    auto fields =
        objc::msg_send<id>(request, objc::selector("allHTTPHeaderFields"));
    if (fields) {
      auto keys = objc::msg_send<id>(fields, objc::selector("allKeys"));
      auto count = objc::msg_send<NSUInteger>(keys, objc::selector("count"));
      for (NSUInteger i = 0; i < count; ++i) {
        auto key = objc::msg_send<id>(keys, objc::selector("objectAtIndex:"), i);
        auto value =
            objc::msg_send<id>(fields, objc::selector("objectForKey:"), key);
        request_headers.emplace_back(nsstring_to_string(key),
                                     nsstring_to_string(value));
      }
    }
    auto headers_json = http_headers_to_json(request_headers);

    std::string body;
    auto body_data = objc::msg_send<id>(request, objc::selector("HTTPBody"));
    if (body_data) {
      auto bytes =
          objc::msg_send<const void *>(body_data, objc::selector("bytes"));
      auto length =
          objc::msg_send<NSUInteger>(body_data, objc::selector("length"));
      if (bytes && length > 0) {
        body.assign(static_cast<const char *>(bytes),
                    static_cast<size_t>(length));
      }
    } else if (auto stream = objc::msg_send<id>(
                   request, objc::selector("HTTPBodyStream"))) {
      // Some bodies arrive only as a stream. read:maxLength: returns 0 at
      // the end and a negative count on error.
      objc::msg_send<void>(stream, objc::selector("open"));
      uint8_t buf[16384];
      bool failed = false;
      for (;;) {
        auto read = objc::msg_send<NSInteger>(
            stream, objc::selector("read:maxLength:"),
            static_cast<uint8_t *>(buf), static_cast<NSUInteger>(sizeof(buf)));
        if (read < 0) {
          failed = true;
          break;
        }
        if (read == 0) {
          break;
        }
        body.append(reinterpret_cast<const char *>(buf),
                    static_cast<size_t>(read));
      }
      id error = failed
                     ? objc::msg_send<id>(stream, objc::selector("streamError"))
                     : nullptr;
      objc::msg_send<void>(stream, objc::selector("close"));
      if (failed) {
        // A partial body must not reach the handler as if it were whole: the
        // task fails instead (NSURLErrorUnknown when the stream gives no
        // error of its own).
        if (!error) {
          error = objc::msg_send<id>(
              objc::get_class("NSError"),
              objc::selector("errorWithDomain:code:userInfo:"),
              NSString_stringWithUTF8String("NSURLErrorDomain"),
              static_cast<NSInteger>(-1), static_cast<id>(nullptr));
        }
        objc::msg_send<void>(task, objc::selector("didFailWithError:"), error);
        return;
      }
    }

    // Held until webview_complete_request answers it or WebKit stops it.
    auto request_id = next_request_id();
    m_pending_tasks[request_id] = objc::retain(task);

    try {
      handler(request_id, method.c_str(), url_string.c_str(),
              headers_json.c_str(), body.empty() ? nullptr : body.data(),
              body.size(), request_handler_arg());
    } catch (...) {
      // A handler that throws must not strand the request: unless it already
      // submitted an answer through webview_complete_request or began one
      // through webview_begin_response (queued, not yet run), answer it here
      // with a bare 500. The exception goes no further (it must not unwind
      // into WebKit).
      if (!is_request_submitted(request_id)) {
        try {
          complete_request_impl(request_id, 500, http_headers_t{},
                                std::string{});
        } catch (...) {
        }
      }
    }
  }

  void on_url_scheme_task_stopped(id task) {
    for (auto it = m_pending_tasks.begin(); it != m_pending_tasks.end(); ++it) {
      if (it->second == task) {
        auto stopped = it->second;
        m_streaming_tasks.erase(it->first);
        m_pending_tasks.erase(it);
        objc::release(stopped);
        return;
      }
    }
  }

  void complete_request_impl(uint64_t request_id, int status,
                             const http_headers_t &headers,
                             const std::string &body) override {
    auto found = m_pending_tasks.find(request_id);
    if (found == m_pending_tasks.end()) {
      return; // already answered, or stopped by WebKit
    }
    // NOLINTNEXTLINE(readability-container-contains): contains() requires C++20
    if (m_streaming_tasks.count(request_id) > 0) {
      return; // a streamed answer has begun; only webview_end_response ends it
    }
    auto task = found->second;
    m_pending_tasks.erase(found);
    {
      objc::autoreleasepool arp;
      auto request = objc::msg_send<id>(task, objc::selector("request"));
      auto url = objc::msg_send<id>(request, objc::selector("URL"));
      respond_to_url_scheme_task(task, url, status, headers, body.data(),
                                 body.size());
    }
    objc::release(task);
  }

  // Streamed responses (see webview_begin_response). Every step runs on the
  // main queue, in the order it was called; a task WebKit stopped in between
  // is gone from m_pending_tasks, so what follows for it does nothing.
  noresult begin_response_impl(uint64_t request_id, int status,
                               http_headers_t headers) override {
    return dispatch_if_alive([this, request_id, status, headers] {
      forget_submitted_request(request_id);
      auto found = m_pending_tasks.find(request_id);
      // NOLINTNEXTLINE(readability-container-contains): contains() requires C++20
      if (found == m_pending_tasks.end() ||
          m_streaming_tasks.count(request_id) > 0) {
        return; // answered, stopped, or already begun
      }
      auto task = found->second;
      objc::autoreleasepool arp;
      auto request = objc::msg_send<id>(task, objc::selector("request"));
      auto url = objc::msg_send<id>(request, objc::selector("URL"));
      send_url_scheme_response_head(task, url, status, headers);
      m_streaming_tasks.insert(request_id);
    });
  }

  noresult write_response_chunk_impl(uint64_t request_id,
                                     std::string chunk) override {
    return dispatch_if_alive([this, request_id, chunk] {
      auto found = m_pending_tasks.find(request_id);
      // NOLINTNEXTLINE(readability-container-contains): contains() requires C++20
      if (found == m_pending_tasks.end() ||
          m_streaming_tasks.count(request_id) == 0) {
        return;
      }
      send_url_scheme_response_data(found->second, chunk.data(), chunk.size());
    });
  }

  noresult end_response_impl(uint64_t request_id) override {
    return dispatch_if_alive([this, request_id] {
      auto found = m_pending_tasks.find(request_id);
      if (found == m_pending_tasks.end()) {
        return; // already ended, or stopped by WebKit
      }
      // NOLINTNEXTLINE(readability-container-contains): contains() requires C++20
      if (m_streaming_tasks.count(request_id) == 0) {
        return; // never begun: webview_complete_request may still answer it
      }
      auto task = found->second;
      m_pending_tasks.erase(found);
      m_streaming_tasks.erase(request_id);
      {
        objc::autoreleasepool arp;
        objc::msg_send<void>(task, objc::selector("didFinish"));
      }
      objc::release(task);
    });
  }

  // External drops (see webview_set_allow_external_drop): the WKWebView
  // subclass checks m_allow_external_drop on every drag.
  noresult set_allow_external_drop_impl(bool allow) override {
    m_allow_external_drop.store(allow);
    return {};
  }

  // File drops (see webview_set_file_drop_handler): the WKWebView subclass
  // asks file_drop_handler() on every drag, so there is nothing to switch.
  noresult set_file_drop_handler_impl(bool /*enabled*/) override { return {}; }

  // A WKWebView that refuses external drags when allow_external_drop is false,
  // takes drags of files from Finder while a file drop handler is set, and
  // leaves every other drag to WKWebView.
  //
  // Each drag session (NSDraggingInfo.draggingSequenceNumber) is routed once,
  // in draggingEntered:, and every later message of that session follows the
  // recorded route, so WebKit only ever sees a complete entered / updated /
  // exited-or-performed sequence for the sessions it was given. Every method
  // body catches everything: no C++ exception may unwind into AppKit.
  static Class get_webview_class() {
    constexpr auto class_name = "SweetviewWKWebView";
    // Avoid crash due to registering same class twice
    auto cls = objc_lookUpClass(class_name);
    if (cls) {
      return cls;
    }
    auto wk_cls = objc::get_class("WKWebView");
    cls = objc_allocateClassPair(wk_cls, class_name, 0);
    auto add_drag_method = [&](const char *name, IMP imp,
                               const char *fallback_types) {
      auto sel = objc::selector(name);
      // Copy WKWebView's own encoding: BOOL is "c" on x86_64 but "B" on
      // arm64, so a hardcoded encoding is wrong on one of them.
      const char *types = nullptr;
      if (auto method = class_getInstanceMethod(wk_cls, sel)) {
        types = method_getTypeEncoding(method);
      }
      class_addMethod(cls, sel, imp, types ? types : fallback_types);
    };
#if defined(__arm64__) || defined(__aarch64__)
    constexpr auto bool_types = "B@:@";
#else
    constexpr auto bool_types = "c@:@";
#endif
    add_drag_method(
        "draggingEntered:",
        (IMP)(+[](id self, SEL sel, id sender) -> NSUInteger {
          try {
            auto w = get_associated_webview(self);
            if (!w) {
              return call_webview_super<NSUInteger>(self, sel, sender);
            }
            auto route = decide_drag_route(self, sender);
            w->m_drag_routes[drag_sequence(sender)] = route;
            return drag_operation_for(self, sel, sender, route);
          } catch (...) {
            return 0; // NSDragOperationNone
          }
        }),
        "Q@:@");
    add_drag_method(
        "draggingUpdated:",
        (IMP)(+[](id self, SEL sel, id sender) -> NSUInteger {
          try {
            auto w = get_associated_webview(self);
            if (!w) {
              return call_webview_super<NSUInteger>(self, sel, sender);
            }
            auto seq = drag_sequence(sender);
            auto found = w->m_drag_routes.find(seq);
            if (found == w->m_drag_routes.end()) {
              // Unseen session (entered before this engine was attached):
              // decide now and stick to it.
              found =
                  w->m_drag_routes.emplace(seq, decide_drag_route(self, sender))
                      .first;
            }
            switch (found->second) {
            case drag_route_t::rejected:
              return 0; // NSDragOperationNone
            case drag_route_t::file_drop:
              if (!file_drop_still_allowed(self, sender)) {
                // Handler cleared, external drops disabled, or copying no
                // longer allowed (modifier keys) mid-drag.
                found->second = drag_route_t::rejected;
                return 0;
              }
              return 1; // NSDragOperationCopy
            case drag_route_t::webkit:
              if (!w->m_allow_external_drop.load() &&
                  is_external_drag(sender)) {
                // External drops disabled mid-drag: let WebKit wind down the
                // drag it was following, then refuse the rest of it.
                found->second = drag_route_t::rejected;
                call_webview_super_exited(self, sender);
                return 0;
              }
              return call_webview_super<NSUInteger>(self, sel, sender);
            }
            return 0;
          } catch (...) {
            return 0; // NSDragOperationNone
          }
        }),
        "Q@:@");
    add_drag_method(
        "draggingExited:",
        (IMP)(+[](id self, SEL sel, id sender) {
          try {
            auto w = get_associated_webview(self);
            if (!w) {
              call_webview_super<void>(self, sel, sender);
              return;
            }
            auto route = take_drag_route(w, sender);
            // Only sessions WebKit was following get the exit; it never saw
            // rejected or file drop sessions enter.
            if (route == drag_route_t::webkit) {
              call_webview_super<void>(self, sel, sender);
            }
          } catch (...) {
          }
        }),
        "v@:@");
    add_drag_method(
        "prepareForDragOperation:",
        (IMP)(+[](id self, SEL sel, id sender) -> BOOL {
          try {
            auto w = get_associated_webview(self);
            if (!w) {
              return call_webview_super<BOOL>(self, sel, sender);
            }
            switch (peek_drag_route(self, w, sender)) {
            case drag_route_t::rejected:
              return NO;
            case drag_route_t::file_drop:
              if (!file_drop_still_allowed(self, sender)) {
                w->m_drag_routes[drag_sequence(sender)] =
                    drag_route_t::rejected;
                return NO;
              }
              return YES;
            case drag_route_t::webkit:
              if (!w->m_allow_external_drop.load() &&
                  is_external_drag(sender)) {
                // External drops disabled since the last draggingUpdated:
                // let WebKit wind down the drag it was following, then
                // refuse the drop.
                w->m_drag_routes[drag_sequence(sender)] =
                    drag_route_t::rejected;
                call_webview_super_exited(self, sender);
                return NO;
              }
              return call_webview_super<BOOL>(self, sel, sender);
            }
            return NO;
          } catch (...) {
            return NO;
          }
        }),
        bool_types);
    add_drag_method(
        "performDragOperation:",
        (IMP)(+[](id self, SEL sel, id sender) -> BOOL {
          try {
            auto w = get_associated_webview(self);
            if (!w) {
              return call_webview_super<BOOL>(self, sel, sender);
            }
            switch (peek_drag_route(self, w, sender)) {
            case drag_route_t::rejected:
              // Kept (as rejected) until draggingEnded:, so the end of a
              // session WebKit never followed is not forwarded to it.
              return NO;
            case drag_route_t::file_drop: {
              w->m_drag_routes[drag_sequence(sender)] = drag_route_t::rejected;
              if (!file_drop_still_allowed(self, sender)) {
                return NO;
              }
              // The pasteboard is only valid during this call, so the paths
              // are read now; the handler runs after AppKit's drag ends.
              auto paths = dragged_file_paths(sender);
              if (paths.empty()) {
                return NO;
              }
              // NO when the delivery timer could not be made: the drop
              // would otherwise be accepted and then never delivered.
              return w->schedule_file_drop(std::move(paths)) ? YES : NO;
            }
            case drag_route_t::webkit:
              if (!w->m_allow_external_drop.load() &&
                  is_external_drag(sender)) {
                // External drops disabled since prepareForDragOperation:.
                // WebKit gets the exit now, so not the end later.
                w->m_drag_routes[drag_sequence(sender)] =
                    drag_route_t::rejected;
                call_webview_super_exited(self, sender);
                return NO;
              }
              // Forgotten here, so draggingEnded: (finding no route) still
              // reaches WebKit, which followed the whole session.
              take_drag_route(w, sender);
              return call_webview_super<BOOL>(self, sel, sender);
            }
            return NO;
          } catch (...) {
            return NO;
          }
        }),
        bool_types);
    add_drag_method(
        "draggingEnded:",
        (IMP)(+[](id self, SEL sel, id sender) {
          try {
            auto w = get_associated_webview(self);
            if (!w) {
              call_webview_super_if_implemented(self, sel, sender);
              return;
            }
            // Forgets the session however it ended (a refused
            // prepareForDragOperation: is followed by no exit or perform).
            // Only sessions WebKit was following get the end.
            if (take_drag_route(w, sender) == drag_route_t::webkit) {
              call_webview_super_if_implemented(self, sel, sender);
            }
          } catch (...) {
          }
        }),
        "v@:@");
    // WebKit's own updateLayer resets the layer's backgroundColor, so a
    // custom background color set once would be lost on the next layer
    // update; it is re-applied after WebKit's pass every time.
    add_drag_method(
        "updateLayer", (IMP)(+[](id self, SEL sel) {
          struct objc_super super_info {
            self, objc::get_class("WKWebView")
          };
          objc::invoke<void>(objc_msgSendSuper, &super_info, sel);
          try {
            if (auto w = get_associated_webview(self)) {
              w->apply_layer_background_color();
            }
          } catch (...) {
          }
        }),
        "v@:");
    objc_registerClassPair(cls);
    return cls;
  }

  // Runs the file drop handler from a one-shot timer on the main run loop,
  // not from the main dispatch queue: the handler may call webview_destroy,
  // whose teardown waits for a block queued on the main queue, and a block
  // already running on that serial queue would never let it run. Nothing is
  // called once the engine is being destroyed.
  // False when the timer could not be created; nothing is scheduled then.
  bool schedule_file_drop(std::vector<std::string> paths) {
    struct pending_drop {
      cocoa_wkwebview_engine *w;
      std::shared_ptr<std::atomic<bool>> alive;
      std::vector<std::string> paths;
    };
    auto drop = new pending_drop{this, alive_token(), std::move(paths)};
    CFRunLoopTimerContext context{};
    context.info = drop;
    // Called when the timer goes, whether or not it fired.
    context.release = +[](const void *info) {
      delete static_cast<const pending_drop *>(info);
    };
    auto timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(), 0, 0, 0,
        +[](CFRunLoopTimerRef, void *info) {
          auto pending = static_cast<pending_drop *>(info);
          if (!pending->alive->load()) {
            return;
          }
          try {
            pending->w->deliver_file_drop(pending->paths);
          } catch (...) {
            // Must not unwind into the run loop.
          }
        },
        &context);
    if (!timer) {
      delete drop;
      return false;
    }
    auto main_loop = CFRunLoopGetMain();
    CFRunLoopAddTimer(main_loop, timer, kCFRunLoopCommonModes);
    CFRelease(timer); // the run loop holds it until it fires
    CFRunLoopWakeUp(main_loop);
    return true;
  }

  static NSInteger drag_sequence(id sender) {
    if (!sender) {
      return 0;
    }
    return objc::msg_send<NSInteger>(sender,
                                     objc::selector("draggingSequenceNumber"));
  }

  // Decides the route of a drag session from its first message.
  static drag_route_t decide_drag_route(id self, id sender) {
    auto w = get_associated_webview(self);
    if (!w) {
      return drag_route_t::webkit;
    }
    if (!w->m_allow_external_drop.load() && is_external_drag(sender)) {
      return drag_route_t::rejected;
    }
    if (takes_file_drag(self, sender)) {
      return drag_route_t::file_drop;
    }
    return drag_route_t::webkit;
  }

  // The recorded route of the session, deciding (and recording) it when the
  // session was never seen.
  static drag_route_t peek_drag_route(id self, cocoa_wkwebview_engine *w,
                                      id sender) {
    auto seq = drag_sequence(sender);
    auto found = w->m_drag_routes.find(seq);
    if (found != w->m_drag_routes.end()) {
      return found->second;
    }
    auto route = decide_drag_route(self, sender);
    w->m_drag_routes[seq] = route;
    return route;
  }

  // Forgets the session and returns the route it had (webkit when unseen).
  static drag_route_t take_drag_route(cocoa_wkwebview_engine *w, id sender) {
    auto found = w->m_drag_routes.find(drag_sequence(sender));
    if (found == w->m_drag_routes.end()) {
      return drag_route_t::webkit;
    }
    auto route = found->second;
    w->m_drag_routes.erase(found);
    return route;
  }

  static NSUInteger drag_operation_for(id self, SEL sel, id sender,
                                       drag_route_t route) {
    switch (route) {
    case drag_route_t::rejected:
      return 0; // NSDragOperationNone
    case drag_route_t::file_drop:
      return 1; // NSDragOperationCopy
    case drag_route_t::webkit:
      return call_webview_super<NSUInteger>(self, sel, sender);
    }
    return 0;
  }

  // True while a file drop session may still complete: a handler is set,
  // external drops have not been disabled since the session started, and
  // the source still allows copying (modifier keys can change that).
  static bool file_drop_still_allowed(id self, id sender) {
    auto w = get_associated_webview(self);
    if (!w || !w->file_drop_handler() || !sender) {
      return false;
    }
    if (!w->m_allow_external_drop.load() && is_external_drag(sender)) {
      return false;
    }
    return copy_allowed(sender);
  }

  // True when the drag's source allows NSDragOperationCopy.
  static bool copy_allowed(id sender) {
    auto source_mask = objc::msg_send<NSUInteger>(
        sender, objc::selector("draggingSourceOperationMask"));
    return (source_mask & 1) != 0; // NSDragOperationCopy
  }

  // Calls WKWebView's own implementation of a drag method.
  template <typename Result>
  static Result call_webview_super(id self, SEL sel, id sender) {
    struct objc_super super_info {
      self, objc::get_class("WKWebView")
    };
    return objc::invoke<Result>(objc_msgSendSuper, &super_info, sel, sender);
  }

  static void call_webview_super_exited(id self, id sender) {
    call_webview_super<void>(self, objc::selector("draggingExited:"), sender);
  }

  // For optional NSDraggingDestination methods WKWebView may not implement.
  static void call_webview_super_if_implemented(id self, SEL sel, id sender) {
    if (class_respondsToSelector(objc::get_class("WKWebView"), sel)) {
      call_webview_super<void>(self, sel, sender);
    }
  }

  static id file_url_type() {
    return NSString_stringWithUTF8String("public.file-url");
  }

  // True when the drag originated outside this application. AppKit sets
  // NSDraggingInfo.draggingSource to nil for drags from other apps (Finder,
  // etc.) and non-nil for in-app / in-page drags.
  static bool is_external_drag(id sender) {
    if (!sender) {
      return false;
    }
    objc::autoreleasepool arp;
    return objc::msg_send<id>(sender, objc::selector("draggingSource")) ==
           nullptr;
  }

  // True when the engine behind the WKWebView has a file drop handler, the
  // drag comes from outside this application and carries file URLs, and its
  // source allows copying. Drags inside the app stay with WebKit even when
  // they carry file URLs.
  static bool takes_file_drag(id self, id sender) {
    auto w = get_associated_webview(self);
    if (!w || !w->file_drop_handler() || !sender) {
      return false;
    }
    if (!is_external_drag(sender)) {
      return false;
    }
    objc::autoreleasepool arp;
    if (!copy_allowed(sender)) {
      return false;
    }
    auto pasteboard =
        objc::msg_send<id>(sender, objc::selector("draggingPasteboard"));
    if (!pasteboard) {
      return false;
    }
    auto types = objc::msg_send<id>(pasteboard, objc::selector("types"));
    return types && objc::msg_send<BOOL>(types,
                                         objc::selector("containsObject:"),
                                         file_url_type());
  }

  // The filesystem path of every file URL on the drag's pasteboard.
  static std::vector<std::string> dragged_file_paths(id sender) {
    std::vector<std::string> paths;
    objc::autoreleasepool arp;
    auto pasteboard =
        objc::msg_send<id>(sender, objc::selector("draggingPasteboard"));
    if (!pasteboard) {
      return paths;
    }
    auto items =
        objc::msg_send<id>(pasteboard, objc::selector("pasteboardItems"));
    if (!items) {
      return paths;
    }
    auto count = objc::msg_send<NSUInteger>(items, objc::selector("count"));
    for (NSUInteger i = 0; i < count; ++i) {
      auto item =
          objc::msg_send<id>(items, objc::selector("objectAtIndex:"), i);
      auto string = objc::msg_send<id>(item, objc::selector("stringForType:"),
                                       file_url_type());
      if (!string) {
        continue;
      }
      // A file reference URL (file:///.file/id=...) becomes a path URL.
      auto url = objc::msg_send<id>(objc::get_class("NSURL"),
                                    objc::selector("URLWithString:"), string);
      auto file_url =
          url ? objc::msg_send<id>(url, objc::selector("filePathURL"))
              : nullptr;
      auto path = nsstring_to_string(
          file_url ? objc::msg_send<id>(file_url, objc::selector("path"))
                   : nullptr);
      if (!path.empty()) {
        paths.push_back(path);
      }
    }
    return paths;
  }

  id create_script_message_handler() {
    objc::autoreleasepool arp;
    constexpr auto class_name = "WebviewWKScriptMessageHandler";
    // Avoid crash due to registering same class twice
    auto cls = objc_lookUpClass(class_name);
    if (!cls) {
      cls =
          objc_allocateClassPair(objc::get_class("NSResponder"), class_name, 0);
      class_addProtocol(cls, objc_getProtocol("WKScriptMessageHandler"));
      class_addMethod(
          cls, objc::selector("userContentController:didReceiveScriptMessage:"),
          (IMP)(+[](id self, SEL, id, id msg) {
            try {
              // Bindings belong to the top-level document only: drop
              // messages posted from any iframe so a cross-origin frame
              // cannot call host bindings through
              // window.webkit.messageHandlers.
              if (!msg || !WKScriptMessage_is_from_main_frame(msg)) {
                return;
              }
              auto w = get_associated_webview(self);
              if (!w) {
                return; // engine gone; the handler outlived it
              }
              auto body = WKScriptMessage_get_body(msg);
              // The bridge only posts strings; anything else (a page calling
              // postMessage with an object) is not a binding call.
              if (!body ||
                  !objc::msg_send<BOOL>(body, objc::selector("isKindOfClass:"),
                                        objc::get_class("NSString"))) {
                return;
              }
              auto utf8 = NSString_get_UTF8String(body);
              if (!utf8) {
                return;
              }
              w->on_message(utf8);
            } catch (...) {
              // Must not unwind into WebKit.
            }
          }),
          "v@:@@");
      objc_registerClassPair(cls);
    }
    auto instance{objc::Class_new(cls)};
    set_associated_webview(instance, this);
    return instance;
  }

  // Unregisters the "__webview__" script message handler, so the user content
  // controller drops its strong reference to it and no message posted while
  // the WKWebView winds down reaches this engine. Safe to call more than once,
  // and safe when the host already unregistered it (a host that calls
  // removeAllScriptMessageHandlers before webview_destroy): this engine holds
  // its own reference to the handler until here.
  void remove_script_message_handler() {
    if (!m_script_message_handler) {
      return;
    }
    objc::autoreleasepool arp;
    set_associated_webview(m_script_message_handler, nullptr);
    if (m_manager) {
      WKUserContentController_removeScriptMessageHandlerForName(
          m_manager, NSString_stringWithUTF8String("__webview__"));
    }
    objc::release(m_script_message_handler);
    m_script_message_handler = nullptr;
  }

  // Cuts the URL scheme handler off from this engine and releases every task
  // still open: from windowWillClose: and from the destructor. Tasks are only
  // released: WebKit stops them itself when the WKWebView goes, and answering
  // one it has stopped would throw. Idempotent.
  void release_url_scheme_state() {
    if (m_url_scheme_handler) {
      // WebKit may still start or stop tasks on the handler while the
      // WKWebView winds down; they must not reach this engine.
      set_associated_webview(m_url_scheme_handler, nullptr);
      objc::release(m_url_scheme_handler);
      m_url_scheme_handler = nullptr;
    }
    for (auto &entry : m_pending_tasks) {
      objc::release(entry.second);
    }
    m_pending_tasks.clear();
    m_streaming_tasks.clear();
  }

  // Cuts every view and controller this engine created off from it and
  // releases them: from windowWillClose: (the window is going, the engine
  // stays until webview_destroy) and from the destructor. Afterwards no drag,
  // script message or UI delegate call can reach this engine through them.
  // Idempotent: each member is nulled as it is released.
  void detach_and_release_views() {
    objc::autoreleasepool arp;
    remove_script_message_handler();
    if (m_manager) {
      WKUserContentController_removeScriptMessageHandlerForName(
          m_manager, NSString_stringWithUTF8String("__webview__"));
      objc::release(m_manager);
      m_manager = nullptr;
    }
    m_drag_routes.clear();
    if (m_ui_delegate) {
      // The host may have replaced the UI delegate; that one belongs to the
      // host and is neither cleared nor released here. Only the engine's own
      // delegate (held +1 in m_ui_delegate) is released.
      if (m_webview && WKWebView_get_UIDelegate(m_webview) == m_ui_delegate) {
        WKWebView_set_UIDelegate(m_webview, nullptr);
      }
      set_associated_webview(m_ui_delegate, nullptr);
      objc::release(m_ui_delegate);
      m_ui_delegate = nullptr;
    }
    if (m_webview) {
      set_associated_webview(m_webview, nullptr);
      objc::release(m_webview);
      m_webview = nullptr;
    }
    if (m_widget) {
      objc::release(m_widget);
      m_widget = nullptr;
    }
  }
  static id create_webkit_ui_delegate() {
    objc::autoreleasepool arp;
    constexpr auto class_name = "WebviewWKUIDelegate";
    // Avoid crash due to registering same class twice
    auto cls = objc_lookUpClass(class_name);
    if (!cls) {
      cls = objc_allocateClassPair(objc::get_class("NSObject"), class_name, 0);
      class_addProtocol(cls, objc_getProtocol("WKUIDelegate"));
      class_addMethod(
          cls,
          objc::selector("webView:runOpenPanelWithParameters:initiatedByFrame:"
                         "completionHandler:"),
          (IMP)(+[](id, SEL, id, id parameters, id, id completion_handler) {
            auto allows_multiple_selection{
                WKOpenPanelParameters_get_allowsMultipleSelection(parameters)};
            auto allows_directories{
                WKOpenPanelParameters_get_allowsDirectories(parameters)};

            // Show a panel for selecting files.
            auto panel{NSOpenPanel_openPanel()};
            NSOpenPanel_set_canChooseFiles(panel, true);
            NSOpenPanel_set_canChooseDirectories(panel, allows_directories);
            NSOpenPanel_set_allowsMultipleSelection(panel,
                                                    allows_multiple_selection);
            auto modal_response{NSSavePanel_runModal(panel)};

            // Get the URLs for the selected files. If the modal was canceled
            // then we pass null to the completion handler to signify
            // cancellation.
            id urls{modal_response == NSModalResponseOK
                        ? NSOpenPanel_get_URLs(panel)
                        : nullptr};

            // Invoke the completion handler block.
            auto sig{NSMethodSignature_signatureWithObjCTypes("v@?@")};
            auto invocation{NSInvocation_invocationWithMethodSignature(sig)};
            NSInvocation_set_target(invocation, completion_handler);
            NSInvocation_setArgument(invocation, &urls, 1);
            NSInvocation_invoke(invocation);
          }),
          "v@:@@@@");
      objc_registerClassPair(cls);
    }
    return objc::Class_new(cls);
  }
  static id create_window_delegate() {
    objc::autoreleasepool arp;
    constexpr auto class_name = "WebviewNSWindowDelegate";
    // Avoid crash due to registering same class twice
    auto cls = objc_lookUpClass(class_name);
    if (!cls) {
      cls = objc_allocateClassPair(objc::get_class("NSObject"), class_name, 0);
      class_addProtocol(cls, objc_getProtocol("NSWindowDelegate"));
      class_addMethod(cls, objc::selector("windowWillClose:"),
                      (IMP)(+[](id self, SEL, id notification) {
                        auto window{NSNotification_get_object(notification)};
                        auto w = get_associated_webview(self);
                        w->on_window_will_close(self, window);
                      }),
                      "v@:@");
      objc_registerClassPair(cls);
    }
    return objc::Class_new(cls);
  }
  static cocoa_wkwebview_engine *get_associated_webview(id object) {
    objc::autoreleasepool arp;
    if (id assoc_obj{objc_getAssociatedObject(object, "webview")}) {
      cocoa_wkwebview_engine *w{};
      NSValue_getValue(assoc_obj, &w, sizeof(w));
      return w;
    }
    return nullptr;
  }
  static void set_associated_webview(id object, cocoa_wkwebview_engine *w) {
    objc::autoreleasepool arp;
    objc_setAssociatedObject(object, "webview", NSValue_valueWithPointer(w),
                             OBJC_ASSOCIATION_RETAIN);
  }
  static bool is_app_bundled() noexcept {
    auto bundle = NSBundle_get_mainBundle();
    if (!bundle) {
      return false;
    }
    auto bundle_path = NSBundle_get_bundlePath(bundle);
    auto bundled =
        NSString_hasSuffix(bundle_path, NSString_stringWithUTF8String(".app"));
    return !!bundled;
  }
  void on_application_did_finish_launching(id /*delegate*/, id app) {
    // See comments related to application lifecycle in create_app_delegate().
    if (owns_window()) {
      // Stop the main run loop so that we can return
      // from the constructor.
      stop_run_loop();
    }

    // Activate the app if it is not bundled.
    // Bundled apps launched from Finder are activated automatically but
    // otherwise not. Activating the app even when it has been launched from
    // Finder does not seem to be harmful but calling this function is rarely
    // needed as proper activation is normally taken care of for us.
    // Bundled apps have a default activation policy of
    // NSApplicationActivationPolicyRegular while non-bundled apps have a
    // default activation policy of NSApplicationActivationPolicyProhibited.
    if (!is_app_bundled()) {
      // "setActivationPolicy:" must be invoked before
      // "activateIgnoringOtherApps:" for activation to work.
      NSApplication_setActivationPolicy(app,
                                        NSApplicationActivationPolicyRegular);
      // Activate the app regardless of other active apps.
      // This can be obtrusive so we only do it when necessary.
      NSApplication_activateIgnoringOtherApps(app, true);
    }

    window_init_proceed();
  }
  void on_window_will_close(id /*delegate*/, id /*window*/) {
    // The window keeps the views alive until it goes; this engine lets go of
    // them now so nothing left behind still points at it. From here on
    // queued answers, replies and file drops do nothing, and no URL scheme
    // task can reach this engine.
    mark_destroying();
    release_url_scheme_state();
    detach_and_release_views();
    m_window = nullptr;
    dispatch([this] { on_window_destroyed(); });
  }
  void window_settings(bool debug) {
    objc::autoreleasepool arp;

    auto config{objc::autorelease(WKWebViewConfiguration_new())};

    // Retained so teardown can still unregister the script message handler
    // after the window (and with it the WKWebView) has gone.
    m_manager = objc::retain(
        WKWebViewConfiguration_get_userContentController(config));

    auto url_scheme_handler = objc::autorelease(create_url_scheme_handler());
    m_url_scheme_handler = objc::retain(url_scheme_handler);
    for (const char *scheme : {"webview", "webview-frames"}) {
      objc::msg_send<void>(
          config, objc::selector("setURLSchemeHandler:forURLScheme:"),
          url_scheme_handler, NSString_stringWithUTF8String(scheme));
    }

    auto preferences = WKWebViewConfiguration_get_preferences(config);
    auto yes_value = NSNumber_numberWithBool(true);

    if (debug) {
      NSObject_setValue_forKey(
          preferences, yes_value,
          NSString_stringWithUTF8String("developerExtrasEnabled"));
    }

    NSObject_setValue_forKey(
        preferences, yes_value,
        NSString_stringWithUTF8String("fullScreenEnabled"));

#if defined(__has_builtin)
#if __has_builtin(__builtin_available)
    if (__builtin_available(macOS 10.13, *)) {
      NSObject_setValue_forKey(
          preferences, yes_value,
          NSString_stringWithUTF8String("javaScriptCanAccessClipboard"));
      NSObject_setValue_forKey(
          preferences, yes_value,
          NSString_stringWithUTF8String("DOMPasteAllowed"));
    }
#else
#error __builtin_available not supported by compiler
#endif
#else
#error __has_builtin not supported by compiler
#endif

    // Held +1 by the engine (WKWebView's UIDelegate is weak) and released in
    // detach_and_release_views().
    m_ui_delegate = create_webkit_ui_delegate();
    // A WKWebView subclass, so files dropped from Finder can be taken
    // before the page sees them (see webview_set_file_drop_handler).
    m_webview = objc::retain(objc::autorelease(WKWebView_initWithFrame(
        objc::msg_send<id>(get_webview_class(), objc::selector("alloc")),
        CGRectMake(0, 0, 0, 0), config)));
    set_associated_webview(m_webview, this);
    // Autoresizing mask is needed to prevent the Web Inspector pane from
    // pushing the main web view out of bounds
    auto autoresizing_mask{static_cast<NSAutoresizingMaskOptions>(
        NSViewWidthSizable | NSViewMaxXMargin | NSViewHeightSizable |
        NSViewMaxYMargin)};
    NSView_set_autoresizingMask(m_webview, autoresizing_mask);
    set_associated_webview(m_ui_delegate, this);
    WKWebView_set_UIDelegate(m_webview, m_ui_delegate);

    if (debug) {
      // Explicitly make WKWebView inspectable via Safari on OS versions that
      // disable the feature by default (macOS 13.3 and later) and support
      // enabling it. According to Apple, the behavior on older OS versions is
      // for content to always be inspectable in "debug builds".
      // Testing shows that this is true for macOS 12.6 but somehow not 10.15.
      // https://webkit.org/blog/13936/enabling-the-inspection-of-web-content-in-apps/
      WKWebView_set_inspectable(m_webview, true);
    }

    // Held +1 by the engine (and by the user content controller while
    // registered) until remove_script_message_handler() releases it.
    m_script_message_handler = create_script_message_handler();
    WKUserContentController_addScriptMessageHandler(
        m_manager, m_script_message_handler,
        NSString_stringWithUTF8String("__webview__"));

    add_init_script("function(message) {\n\
  return window.webkit.messageHandlers.__webview__.postMessage(message);\n\
}");
    set_up_widget();
    NSWindow_set_contentView(m_window, m_widget);
    if (owns_window()) {
      NSWindow_makeKeyAndOrderFront(m_window);
    }
  }
  void set_up_widget() {
    objc::autoreleasepool arp;
    // Create a new view that can contain both the web view and the Web Inspector pane
    m_widget = objc::retain(NSView_withFrame(NSRectMake(0, 0, 0, 0)));
    // Autoresizing is needed because the Web Inspector pane is a sibling of the web view
    NSView_set_autoresizesSubviews(m_widget, true);
    NSView_addSubview(m_widget, m_webview);
    NSView_set_frame(m_webview, NSView_get_bounds(m_widget));
  }
  void stop_run_loop() {
    objc::autoreleasepool arp;
    // Request the run loop to stop. This doesn't immediately stop the loop.
    NSApplication_stop(m_app);
    // The run loop will stop after processing an NSEvent.
    auto event{NSEvent_otherEventWithType(
        NSEventTypeApplicationDefined, NSPointMake(0, 0),
        NSEventModifierFlags{}, 0, 0, nullptr, 0, 0, 0)};
    NSApplication_postEvent(m_app, event, true);
  }
  static bool get_and_set_is_first_instance() noexcept {
    static std::atomic_bool first{true};
    bool temp = first;
    if (temp) {
      first = false;
    }
    return temp;
  }
  void window_init(void *window) {
    objc::autoreleasepool arp;

    m_window = static_cast<id>(window);
    if (!owns_window()) {
      return;
    }

    // Skip application setup if this isn't the first instance of this class
    // because the launch event is only sent once. The same holds when the
    // host has already launched the app (its run loop is running): the
    // launch event will never come again, so a nested run would hang.
    if (!get_and_set_is_first_instance() || NSApplication_isRunning(m_app)) {
      window_init_proceed();
      return;
    }

    m_app_delegate = create_app_delegate();
    set_associated_webview(m_app_delegate, this);
    NSApplication_set_delegate(m_app, m_app_delegate);

    // Start the main run loop so that the app delegate gets the
    // NSApplicationDidFinishLaunchingNotification notification after the run
    // loop has started in order to perform further initialization.
    // We need to return from this constructor so this run loop is only
    // temporary.
    NSApplication_run(m_app);
  }
  void window_init_proceed() {
    objc::autoreleasepool arp;

    m_window = objc::retain(NSWindow_withContentRect(
        NSRectMake(0, 0, 0, 0), NSWindowStyleMaskTitled, NSBackingStoreBuffered,
        false));
    m_window_delegate = create_window_delegate();
    set_associated_webview(m_window_delegate, this);
    NSWindow_set_delegate(m_window, m_window_delegate);
    on_window_created();
  }

  noresult window_show() {
    objc::autoreleasepool arp;
    if (m_is_window_shown) {
      return {};
    }
    m_is_window_shown = true;
    return {};
  }

  void run_event_loop_while(std::function<bool()> fn) override {
    objc::autoreleasepool arp;
    while (fn()) {
      objc::autoreleasepool arp2;
      if (auto event{NSApplication_nextEventMatchingMask(
              m_app, NSEventMaskAny, nullptr,
              NSRunLoopMode::NSDefaultRunLoopMode(), true)}) {
        NSApplication_sendEvent(m_app, event);
      }
    }
  }

  id m_app{};
  id m_app_delegate{};
  id m_window_delegate{};
  id m_window{};
  id m_widget{};
  id m_webview{};
  id m_manager{};
  bool m_is_window_shown{};
  std::atomic<bool> m_allow_external_drop{true};
  // Route of every drag session currently over the WKWebView, by
  // NSDraggingInfo.draggingSequenceNumber; main thread only.
  std::map<NSInteger, drag_route_t> m_drag_routes;
  // The last color given to set_background_color, re-applied to the
  // WKWebView's layer after each of WebKit's updateLayer passes; main
  // thread only.
  bool m_has_background_color{false};
  int m_bg_r{0};
  int m_bg_g{0};
  int m_bg_b{0};
  int m_bg_a{255};
  // The "__webview__" script message handler; retained by the engine.
  id m_script_message_handler{};
  // The engine's own WKUIDelegate; retained by the engine. Never confused
  // with a delegate the host set on the WKWebView later.
  id m_ui_delegate{};
  id m_url_scheme_handler{};
  // Tasks handed to the request handler and not yet answered or stopped;
  // each is retained.
  std::map<uint64_t, id> m_pending_tasks;
  // Of those, the ones whose streamed answer has sent its head
  // (webview_begin_response) and not yet finished.
  std::set<uint64_t> m_streaming_tasks;
};

} // namespace cocoa_webkit
} // namespace detail

using browser_engine = detail::cocoa_webkit::cocoa_wkwebview_engine;

} // namespace webview

#endif // defined(WEBVIEW_PLATFORM_DARWIN) && defined(WEBVIEW_COCOA)
#endif // defined(__cplusplus) && !defined(WEBVIEW_HEADER)
#endif // WEBVIEW_BACKENDS_COCOA_WEBKIT_HH
