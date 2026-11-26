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

#ifndef WEBVIEW_DETAIL_ENGINE_BASE_HH
#define WEBVIEW_DETAIL_ENGINE_BASE_HH

#if defined(__cplusplus) && !defined(WEBVIEW_HEADER)

#include "../errors.hh"
#include "../types.h"
#include "../types.hh"
#include "http_request.hh"
#include "json.hh"
#include "user_script.hh"

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

namespace webview {
namespace detail {

class engine_base {
public:
  engine_base(bool owns_window) : m_owns_window{owns_window} {}

  virtual ~engine_base() {
    std::lock_guard<std::mutex> lock{m_submitted_mutex};
    m_submitted_requests.clear();
  }

  noresult navigate(const std::string &url) {
    if (url.empty()) {
      return navigate_impl("about:blank");
    }
    return navigate_impl(url);
  }

  using binding_t = std::function<void(std::string, std::string, void *)>;
  class binding_ctx_t {
  public:
    binding_ctx_t(binding_t callback, void *arg)
        : m_callback(callback), m_arg(arg) {}
    void call(std::string id, std::string args) const {
      if (m_callback) {
        m_callback(id, args, m_arg);
      }
    }

  private:
    // This function is called upon execution of the bound JS function
    binding_t m_callback;
    // This user-supplied argument is passed to the callback
    void *m_arg;
  };

  using sync_binding_t = std::function<std::string(std::string)>;

  // Synchronous bind
  noresult bind(const std::string &name, sync_binding_t fn) {
    auto wrapper = [this, fn](const std::string &id, const std::string &req,
                              void * /*arg*/) { resolve(id, 0, fn(req)); };
    return bind(name, wrapper, nullptr);
  }

  // Asynchronous bind
  noresult bind(const std::string &name, binding_t fn, void *arg) {
    // NOLINTNEXTLINE(readability-container-contains): contains() requires C++20
    if (bindings.count(name) > 0) {
      return error_info{WEBVIEW_ERROR_DUPLICATE};
    }
    bindings.emplace(name, binding_ctx_t(fn, arg));
    replace_bind_script();
    // Notify that a binding was created if the init script has already
    // set things up.
    eval("if (window.__webview__) {\n\
window.__webview__.onBind(" +
         json_escape(name) + ")\n\
}");
    return {};
  }

  noresult unbind(const std::string &name) {
    auto found = bindings.find(name);
    if (found == bindings.end()) {
      return error_info{WEBVIEW_ERROR_NOT_FOUND};
    }
    bindings.erase(found);
    replace_bind_script();
    // Notify that a binding was created if the init script has already
    // set things up.
    eval("if (window.__webview__) {\n\
window.__webview__.onUnbind(" +
         json_escape(name) + ")\n\
}");
    return {};
  }

  noresult resolve(const std::string &id, int status,
                   const std::string &result) {
    // NOLINTNEXTLINE(modernize-avoid-bind): Lambda with move requires C++14
    return dispatch(std::bind(
        [id, status, this](std::string escaped_result) {
          std::string js = "window.__webview__.onReply(" + json_escape(id) +
                           ", " + std::to_string(status) + ", " +
                           escaped_result + ")";
          eval(js);
        },
        result.empty() ? "undefined" : json_escape(result)));
  }

  result<void *> window() { return window_impl(); }
  result<void *> widget() { return widget_impl(); }
  result<void *> browser_controller() { return browser_controller_impl(); }
  noresult run() { return run_impl(); }
  noresult terminate() { return terminate_impl(); }
  noresult dispatch(std::function<void()> f) { return dispatch_impl(f); }
  noresult set_title(const std::string &title) { return set_title_impl(title); }

  noresult set_size(int width, int height, webview_hint_t hints) {
    auto res = set_size_impl(width, height, hints);
    m_is_size_set = true;
    return res;
  }

  noresult set_html(const std::string &html) { return set_html_impl(html); }

  noresult set_background_color(int r, int g, int b, int a) {
    return set_background_color_impl(r, g, b, a);
  }

  // Bytes served at a path of the webview's own origin (see webview_set_asset).
  struct asset_t {
    std::string mime;
    std::string data;
  };

  noresult set_asset(const std::string &path, const std::string &mime,
                     const void *data, size_t len) {
    m_assets[normalize_asset_path(path)] =
        asset_t{mime, data && len > 0
                          ? std::string(static_cast<const char *>(data), len)
                          : std::string{}};
    return {};
  }

  // The asset for a request path ("/a/b.js?x" matches "a/b.js"), or nullptr.
  const asset_t *find_asset(const std::string &request_path) const {
    auto it = m_assets.find(normalize_asset_path(request_path));
    return it == m_assets.end() ? nullptr : &it->second;
  }

  static std::string normalize_asset_path(const std::string &path) {
    auto end = path.find_first_of("?#");
    auto start = path.find_first_not_of('/');
    if (start == std::string::npos || start >= end) {
      return {};
    }
    return path.substr(start, end == std::string::npos ? end : end - start);
  }

  // Requests the asset table cannot answer go to this handler (see
  // webview_set_request_handler). Read on the UI thread by the backends.
  noresult set_request_handler(webview_request_handler_t fn, void *arg) {
    m_request_handler = fn;
    m_request_handler_arg = fn ? arg : nullptr;
    return {};
  }

  // Validates and copies the answer, then hands it to the UI thread, so it
  // may be called from any thread and the caller's buffers need not outlive
  // the call. Invalid headers are refused here, before anything is queued.
  // The queued completion holds the alive token rather than trusting `this`:
  // if the engine is destroyed before it runs, it does nothing.
  noresult complete_request(uint64_t request_id, int status,
                            const char *headers_json, const void *body,
                            size_t body_len) {
    if (!is_alive()) {
      return error_info{WEBVIEW_ERROR_INVALID_STATE,
                        "the webview is being destroyed"};
    }
    if (request_id == 0 || status < 100 || status > 599 ||
        (!body && body_len > 0)) {
      return error_info{WEBVIEW_ERROR_INVALID_ARGUMENT};
    }
    // The backends hand bodies to APIs that take 32-bit lengths.
    if (body_len >
        static_cast<size_t>((std::numeric_limits<std::uint32_t>::max)())) {
      return error_info{WEBVIEW_ERROR_INVALID_ARGUMENT,
                        "body_len is larger than 4 GiB - 1"};
    }
    http_headers_t headers;
    if (headers_json && !http_headers_from_json(headers_json, headers)) {
      return error_info{
          WEBVIEW_ERROR_INVALID_ARGUMENT,
          "headers_json is not valid strict JSON (an object or "
          "array of pairs) of valid HTTP header names and values"};
    }
    std::string bytes;
    if (body && body_len > 0) {
      bytes.assign(static_cast<const char *>(body), body_len);
    }
    auto alive = m_alive;
    // Recorded before the answer is queued, so a handler that answers and
    // then throws does not have its answer replaced by the fallback 500.
    // Only the call that recorded the id may undo it on failure, so a failed
    // duplicate completion never erases an earlier successful submission.
    const bool inserted = [&] {
      std::lock_guard<std::mutex> lock{m_submitted_mutex};
      return m_submitted_requests.insert(request_id).second;
    }();
    noresult res;
    try {
      res = dispatch([this, alive, request_id, status, headers, bytes] {
        if (!alive->load()) {
          return; // destroyed after this was queued
        }
        forget_submitted_request(request_id);
        complete_request_impl(request_id, status, headers, bytes);
      });
    } catch (...) {
      if (inserted) {
        forget_submitted_request(request_id);
      }
      throw;
    }
    if (!res.ok() && inserted) {
      forget_submitted_request(request_id);
    }
    return res;
  }

  // True while an answer from webview_complete_request for request_id is
  // queued and has not run yet.
  bool is_request_submitted(uint64_t request_id) const {
    std::lock_guard<std::mutex> lock{m_submitted_mutex};
    // NOLINTNEXTLINE(readability-container-contains): contains() requires C++20
    return m_submitted_requests.count(request_id) > 0;
  }

  // Marks the engine as going away. From here on complete_request refuses
  // new answers and answers already queued do nothing when they run. Called
  // by webview_destroy before the delete, and by each backend's destructor.
  void mark_destroying() noexcept { m_alive->store(false); }

  bool is_alive() const noexcept { return m_alive->load(); }

  // Shared with queued work that must not touch the engine once it is gone.
  std::shared_ptr<std::atomic<bool>> alive_token() const noexcept {
    return m_alive;
  }

  noresult set_allow_external_drop(bool allow) {
    return set_allow_external_drop_impl(allow);
  }

  noresult init(const std::string &js) {
    add_user_script(js);
    return {};
  }

  noresult eval(const std::string &js) { return eval_impl(js); }

protected:
  virtual noresult navigate_impl(const std::string &url) = 0;
  virtual result<void *> window_impl() = 0;
  virtual result<void *> widget_impl() = 0;
  virtual result<void *> browser_controller_impl() = 0;
  virtual noresult run_impl() = 0;
  virtual noresult terminate_impl() = 0;
  virtual noresult dispatch_impl(std::function<void()> f) = 0;
  virtual noresult set_title_impl(const std::string &title) = 0;
  virtual noresult set_size_impl(int width, int height,
                                 webview_hint_t hints) = 0;
  virtual noresult set_html_impl(const std::string &html) = 0;
  virtual noresult eval_impl(const std::string &js) = 0;
  // Backends without a transparent-background story accept and ignore it.
  virtual noresult set_background_color_impl(int /*r*/, int /*g*/, int /*b*/,
                                             int /*a*/) {
    return {};
  }
  // Runs on the UI thread. Backends that serve no requests have none open.
  virtual void complete_request_impl(uint64_t /*request_id*/, int /*status*/,
                                     const http_headers_t & /*headers*/,
                                     const std::string & /*body*/) {}
  // Backends without external drag-and-drop control accept and ignore it.
  virtual noresult set_allow_external_drop_impl(bool /*allow*/) { return {}; }

  webview_request_handler_t request_handler() const {
    return m_request_handler;
  }
  void *request_handler_arg() const { return m_request_handler_arg; }

  // Request ids handed to the request handler: never 0 and never reused
  // across the whole process. A process-wide counter (rather than one per
  // instance) guarantees that a late webview_complete_request() carrying an
  // id from a destroyed webview can never match a request of a newer webview
  // that happens to reuse the same heap address.
  static uint64_t next_request_id() {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
  }

  virtual user_script *add_user_script(const std::string &js) {
    return std::addressof(*m_user_scripts.emplace(m_user_scripts.end(),
                                                  add_user_script_impl(js)));
  }

  virtual user_script add_user_script_impl(const std::string &js) = 0;

  virtual void
  remove_all_user_scripts_impl(const std::list<user_script> &scripts) = 0;

  virtual bool are_user_scripts_equal_impl(const user_script &first,
                                           const user_script &second) = 0;

  virtual user_script *replace_user_script(const user_script &old_script,
                                           const std::string &new_script_code) {
    remove_all_user_scripts_impl(m_user_scripts);
    user_script *old_script_ptr{};
    for (auto &script : m_user_scripts) {
      auto is_old_script = are_user_scripts_equal_impl(script, old_script);
      script = add_user_script_impl(is_old_script ? new_script_code
                                                  : script.get_code());
      if (is_old_script) {
        old_script_ptr = std::addressof(script);
      }
    }
    return old_script_ptr;
  }

  void replace_bind_script() {
    if (m_bind_script) {
      m_bind_script = replace_user_script(*m_bind_script, create_bind_script());
    } else {
      m_bind_script = add_user_script(create_bind_script());
    }
  }

  void add_init_script(const std::string &post_fn) {
    add_user_script(create_init_script(post_fn));
    m_is_init_script_added = true;
  }

  std::string create_init_script(const std::string &post_fn) {
    // Top-level document only. WebView2 runs scripts added with
    // AddScriptToExecuteOnDocumentCreated in every frame, so without this
    // guard a cross-origin iframe would get window.__webview__ and every
    // bound function. (WebKit already injects these into the main frame
    // only.)
    auto js = std::string{} + "(function() {\n\
  'use strict';\n\
  if (window !== window.top) {\n\
    return;\n\
  }\n\
  function generateId() {\n\
    var crypto = window.crypto || window.msCrypto;\n\
    var bytes = new Uint8Array(16);\n\
    crypto.getRandomValues(bytes);\n\
    return Array.prototype.slice.call(bytes).map(function(n) {\n\
      var s = n.toString(16);\n\
      return ((s.length % 2) == 1 ? '0' : '') + s;\n\
    }).join('');\n\
  }\n\
  var Webview = (function() {\n\
    var _promises = {};\n\
    function Webview_() {}\n\
    Webview_.prototype.post = function(message) {\n\
      return (" +
              post_fn + ")(message);\n\
    };\n\
    Webview_.prototype.call = function(method) {\n\
      var _id = generateId();\n\
      var _params = Array.prototype.slice.call(arguments, 1);\n\
      var promise = new Promise(function(resolve, reject) {\n\
        _promises[_id] = { resolve, reject };\n\
      });\n\
      this.post(JSON.stringify({\n\
        id: _id,\n\
        method: method,\n\
        params: _params\n\
      }));\n\
      return promise;\n\
    };\n\
    Webview_.prototype.onReply = function(id, status, result) {\n\
      var promise = _promises[id];\n\
      if (result !== undefined) {\n\
        try {\n\
          result = JSON.parse(result);\n\
        } catch (e) {\n\
          promise.reject(new Error(\"Failed to parse binding result as JSON\"));\n\
          return;\n\
        }\n\
      }\n\
      if (status === 0) {\n\
        promise.resolve(result);\n\
      } else {\n\
        promise.reject(result);\n\
      }\n\
    };\n\
    Webview_.prototype.onBind = function(name) {\n\
      if (window.hasOwnProperty(name)) {\n\
        throw new Error('Property \"' + name + '\" already exists');\n\
      }\n\
      window[name] = (function() {\n\
        var params = [name].concat(Array.prototype.slice.call(arguments));\n\
        return Webview_.prototype.call.apply(this, params);\n\
      }).bind(this);\n\
    };\n\
    Webview_.prototype.onUnbind = function(name) {\n\
      if (!window.hasOwnProperty(name)) {\n\
        throw new Error('Property \"' + name + '\" does not exist');\n\
      }\n\
      delete window[name];\n\
    };\n\
    return Webview_;\n\
  })();\n\
  window.__webview__ = new Webview();\n\
})()";
    return js;
  }

  std::string create_bind_script() {
    std::string js_names = "[";
    bool first = true;
    for (const auto &binding : bindings) {
      if (first) {
        first = false;
      } else {
        js_names += ",";
      }
      js_names += json_escape(binding.first);
    }
    js_names += "]";

    // Top-level document only, like the init script.
    auto js = std::string{} + "(function() {\n\
  'use strict';\n\
  if (window !== window.top || !window.__webview__) {\n\
    return;\n\
  }\n\
  var methods = " +
              js_names + ";\n\
  methods.forEach(function(name) {\n\
    window.__webview__.onBind(name);\n\
  });\n\
})()";
    return js;
  }

  virtual void on_message(const std::string &msg) {
    auto id = json_parse(msg, "id", 0);
    auto name = json_parse(msg, "method", 0);
    auto args = json_parse(msg, "params", 0);
    auto found = bindings.find(name);
    if (found == bindings.end()) {
      return;
    }
    const auto &context = found->second;
    dispatch([=] { context.call(id, args); });
  }

  virtual void on_window_created() { inc_window_count(); }

  virtual void on_window_destroyed(bool skip_termination = false) {
    if (dec_window_count() <= 0) {
      if (!skip_termination) {
        terminate();
      }
    }
  }

  // Runs the event loop until the currently queued events have been processed.
  void deplete_run_loop_event_queue() {
    bool done{};
    if (!dispatch([&] { done = true; }).ok()) {
      return; // nothing could be queued, so there is nothing to wait for
    }
    run_event_loop_while([&] { return !done; });
  }

  // Runs the event loop while the passed-in function returns true.
  virtual void run_event_loop_while(std::function<bool()> fn) = 0;

  void dispatch_size_default() {
    if (!owns_window() || !m_is_init_script_added) {
      return;
    };
    dispatch([this]() {
      if (!m_is_size_set) {
        set_size(m_initial_width, m_initial_height, WEBVIEW_HINT_NONE);
      }
    });
  }

  void set_default_size_guard(bool guarded) { m_is_size_set = guarded; }

  bool owns_window() const { return m_owns_window; }

private:
  void forget_submitted_request(uint64_t request_id) noexcept {
    std::lock_guard<std::mutex> lock{m_submitted_mutex};
    m_submitted_requests.erase(request_id);
  }

  static std::atomic_uint &window_ref_count() {
    static std::atomic_uint ref_count{0};
    return ref_count;
  }

  static unsigned int inc_window_count() { return ++window_ref_count(); }

  static unsigned int dec_window_count() {
    auto &count = window_ref_count();
    if (count > 0) {
      return --count;
    }
    return 0;
  }

  std::map<std::string, binding_ctx_t> bindings;
  std::map<std::string, asset_t> m_assets;
  webview_request_handler_t m_request_handler{};
  void *m_request_handler_arg{};
  // False once the engine is being destroyed; see mark_destroying.
  std::shared_ptr<std::atomic<bool>> m_alive{
      std::make_shared<std::atomic<bool>>(true)};
  // Request ids answered by complete_request whose answer is still queued;
  // see is_request_submitted.
  mutable std::mutex m_submitted_mutex;
  std::unordered_set<uint64_t> m_submitted_requests;
  user_script *m_bind_script{};
  std::list<user_script> m_user_scripts;

  bool m_is_init_script_added{};
  bool m_is_size_set{};
  bool m_owns_window{};
  static const int m_initial_width = 640;
  static const int m_initial_height = 480;
};

} // namespace detail
} // namespace webview

#endif // defined(__cplusplus) && !defined(WEBVIEW_HEADER)
#endif // WEBVIEW_DETAIL_ENGINE_BASE_HH
