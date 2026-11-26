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

#ifndef WEBVIEW_C_API_IMPL_HH
#define WEBVIEW_C_API_IMPL_HH

#if defined(__cplusplus) && !defined(WEBVIEW_HEADER)

#include "backends.hh"
#include "detail/keyboard_hook.hh"
#include "detail/mouse_hook.hh"
#include "errors.h"
#include "errors.hh"
#include "json_deprecated.hh"
#include "macros.h"
#include "types.h"
#include "version.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace webview {
namespace detail {

// The library's version information.
constexpr const webview_version_info_t library_version_info{
    {WEBVIEW_VERSION_MAJOR, WEBVIEW_VERSION_MINOR, WEBVIEW_VERSION_PATCH},
    WEBVIEW_VERSION_NUMBER,
    WEBVIEW_VERSION_PRE_RELEASE,
    WEBVIEW_VERSION_BUILD_METADATA};

template <typename WorkFn, typename ResultFn>
webview_error_t api_filter(WorkFn &&do_work, ResultFn &&put_result) noexcept {
  try {
    auto result = do_work();
    if (result.ok()) {
      put_result(result.value());
      return WEBVIEW_ERROR_OK;
    }
    return result.error().code();
  } catch (const exception &e) {
    return e.error().code();
  } catch (...) {
    return WEBVIEW_ERROR_UNSPECIFIED;
  }
}

template <typename WorkFn>
webview_error_t api_filter(WorkFn &&do_work) noexcept {
  try {
    auto result = do_work();
    if (result.ok()) {
      return WEBVIEW_ERROR_OK;
    }
    return result.error().code();
  } catch (const exception &e) {
    return e.error().code();
  } catch (...) {
    return WEBVIEW_ERROR_UNSPECIFIED;
  }
}

inline webview *cast_to_webview(void *w) {
  if (!w) {
    throw exception{WEBVIEW_ERROR_INVALID_ARGUMENT,
                    "Cannot cast null pointer to webview instance"};
  }
  return static_cast<webview *>(w);
}

// Every webview made by webview_create and not yet passed to
// webview_destroy, with its alive token. webview_complete_request may run on
// any thread, so it looks the instance up here, under the lock, instead of
// trusting the handle: a handle already destroyed (or being destroyed) is
// refused, and webview_destroy cannot delete the instance while a completion
// is being queued. Allocated once and never freed, so it outlives every
// static destructor that might still destroy a webview.
struct live_instances_t {
  std::mutex mutex;
  std::unordered_map<webview_t, std::shared_ptr<std::atomic<bool>>> alive;
};

inline live_instances_t &live_instances() {
  static auto *instances = new live_instances_t{};
  return *instances;
}

// Runs fn on the instance behind w while holding the live-instance lock, so
// webview_destroy cannot free it in the middle; a handle destroyed or being
// destroyed gets WEBVIEW_ERROR_INVALID_STATE. For the calls that may come
// from any thread.
template <typename Fn>
webview_error_t with_live_instance(webview_t w, Fn &&fn) noexcept {
  if (!w) {
    return WEBVIEW_ERROR_INVALID_ARGUMENT;
  }
  return api_filter([&]() -> noresult {
    auto &instances = live_instances();
    std::lock_guard<std::mutex> lock{instances.mutex};
    auto found = instances.alive.find(w);
    if (found == instances.alive.end() || !found->second->load()) {
      return error_info{WEBVIEW_ERROR_INVALID_STATE,
                        "the webview is destroyed or being destroyed"};
    }
    return fn(cast_to_webview(w));
  });
}

} // namespace detail
} // namespace webview

WEBVIEW_API webview_t webview_create(int debug, void *wnd) {
  using namespace webview::detail;
  webview::webview *w{};
  auto err = api_filter(
      [=]() -> webview::result<webview::webview *> {
        return new webview::webview{static_cast<bool>(debug), wnd};
      },
      [&](webview::webview *w_) { w = w_; });
  if (err != WEBVIEW_ERROR_OK) {
    return nullptr;
  }
  try {
    auto &instances = live_instances();
    std::lock_guard<std::mutex> lock{instances.mutex};
    instances.alive[w] = w->alive_token();
  } catch (...) {
    delete w;
    return nullptr;
  }
  return w;
}

WEBVIEW_API webview_error_t webview_destroy(webview_t w) {
  using namespace webview::detail;
  return api_filter([=]() -> webview::noresult {
    auto *instance = cast_to_webview(w);
    {
      // First: no completion may be queued from here on, and any already
      // queued does nothing when it runs.
      auto &instances = live_instances();
      std::lock_guard<std::mutex> lock{instances.mutex};
      instance->mark_destroying();
      instances.alive.erase(w);
    }
    delete instance;
    return {};
  });
}

WEBVIEW_API webview_error_t webview_run(webview_t w) {
  using namespace webview::detail;
  return api_filter([=] { return cast_to_webview(w)->run(); });
}

WEBVIEW_API webview_error_t webview_terminate(webview_t w) {
  using namespace webview::detail;
  return api_filter([=] { return cast_to_webview(w)->terminate(); });
}

WEBVIEW_API webview_error_t webview_dispatch(webview_t w,
                                             void (*fn)(webview_t, void *),
                                             void *arg) {
  using namespace webview::detail;
  if (!fn) {
    return WEBVIEW_ERROR_INVALID_ARGUMENT;
  }
  return api_filter(
      [=] { return cast_to_webview(w)->dispatch([=]() { fn(w, arg); }); });
}

WEBVIEW_API void *webview_get_window(webview_t w) {
  using namespace webview::detail;
  void *window = nullptr;
  auto err = api_filter([=] { return cast_to_webview(w)->window(); },
                        [&](void *value) { window = value; });
  if (err == WEBVIEW_ERROR_OK) {
    return window;
  }
  return nullptr;
}

WEBVIEW_API void *webview_get_native_handle(webview_t w,
                                            webview_native_handle_kind_t kind) {
  using namespace webview::detail;
  void *handle{};
  auto err = api_filter(
      [=]() -> webview::result<void *> {
        auto *w_ = cast_to_webview(w);
        switch (kind) {
        case WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW:
          return w_->window();
        case WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET:
          return w_->widget();
        case WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER:
          return w_->browser_controller();
        default:
          return webview::error_info{WEBVIEW_ERROR_INVALID_ARGUMENT};
        }
      },
      [&](void *handle_) { handle = handle_; });
  if (err == WEBVIEW_ERROR_OK) {
    return handle;
  }
  return nullptr;
}

WEBVIEW_API webview_error_t webview_set_title(webview_t w, const char *title) {
  using namespace webview::detail;
  if (!title) {
    return WEBVIEW_ERROR_INVALID_ARGUMENT;
  }
  return api_filter([=] { return cast_to_webview(w)->set_title(title); });
}

WEBVIEW_API webview_error_t webview_set_size(webview_t w, int width, int height,
                                             webview_hint_t hints) {
  using namespace webview::detail;
  return api_filter(
      [=] { return cast_to_webview(w)->set_size(width, height, hints); });
}

WEBVIEW_API webview_error_t webview_set_background_color(webview_t w, int r,
                                                         int g, int b, int a) {
  using namespace webview::detail;
  for (int c : {r, g, b, a}) {
    if (c < 0 || c > 255) {
      return WEBVIEW_ERROR_INVALID_ARGUMENT;
    }
  }
  return api_filter(
      [=] { return cast_to_webview(w)->set_background_color(r, g, b, a); });
}

WEBVIEW_API webview_error_t webview_set_asset(webview_t w, const char *path,
                                              const char *mime,
                                              const void *data, size_t len) {
  using namespace webview::detail;
  if (!path || !mime || (!data && len > 0)) {
    return WEBVIEW_ERROR_INVALID_ARGUMENT;
  }
  // Checked again by the engine; refused here too so a bad MIME type or
  // size never reaches the instance.
  if (!http_is_valid_header_value(mime) ||
      len > static_cast<size_t>((std::numeric_limits<std::uint32_t>::max)())) {
    return WEBVIEW_ERROR_INVALID_ARGUMENT;
  }
  return api_filter(
      [=] { return cast_to_webview(w)->set_asset(path, mime, data, len); });
}

WEBVIEW_API webview_error_t webview_set_request_handler(
    webview_t w, webview_request_handler_t fn, void *arg) {
  using namespace webview::detail;
  return api_filter(
      [=] { return cast_to_webview(w)->set_request_handler(fn, arg); });
}

WEBVIEW_API webview_error_t webview_complete_request(webview_t w,
                                                     uint64_t request_id,
                                                     int status,
                                                     const char *headers_json,
                                                     const void *body,
                                                     size_t body_len) {
  using namespace webview::detail;
  // The lock is held while the answer is validated, copied and queued.
  return with_live_instance(w, [&](webview::webview *instance) {
    return instance->complete_request(request_id, status, headers_json, body,
                                      body_len);
  });
}

WEBVIEW_API webview_error_t webview_begin_response(webview_t w,
                                                   uint64_t request_id,
                                                   int status,
                                                   const char *headers_json) {
  using namespace webview::detail;
  return with_live_instance(w, [&](webview::webview *instance) {
    return instance->begin_response(request_id, status, headers_json);
  });
}

WEBVIEW_API webview_error_t webview_write_response_chunk(webview_t w,
                                                         uint64_t request_id,
                                                         const void *chunk,
                                                         size_t chunk_len) {
  using namespace webview::detail;
  return with_live_instance(w, [&](webview::webview *instance) {
    return instance->write_response_chunk(request_id, chunk, chunk_len);
  });
}

WEBVIEW_API webview_error_t webview_end_response(webview_t w,
                                                 uint64_t request_id) {
  using namespace webview::detail;
  return with_live_instance(w, [&](webview::webview *instance) {
    return instance->end_response(request_id);
  });
}

WEBVIEW_API webview_error_t webview_set_file_drop_handler(
    webview_t w, webview_file_drop_handler_t fn, void *arg) {
  using namespace webview::detail;
  return api_filter(
      [=] { return cast_to_webview(w)->set_file_drop_handler(fn, arg); });
}

WEBVIEW_API webview_error_t webview_set_allow_external_drop(webview_t w,
                                                            int allow) {
  using namespace webview::detail;
  return api_filter(
      [=] { return cast_to_webview(w)->set_allow_external_drop(allow != 0); });
}

WEBVIEW_API webview_error_t webview_mouse_hook_install(webview_mouse_hook_fn_t fn,
                                                       void *arg) {
  using namespace webview::detail;
  if (!fn) {
    return WEBVIEW_ERROR_INVALID_ARGUMENT;
  }
  return api_filter([=] { return mouse_hook::install(fn, arg); });
}

WEBVIEW_API webview_error_t webview_mouse_hook_remove(void) {
  using namespace webview::detail;
  return api_filter([] { return mouse_hook::remove(); });
}

WEBVIEW_API webview_error_t
webview_keyboard_hook_install(webview_keyboard_hook_fn_t fn, void *arg) {
  using namespace webview::detail;
  if (!fn) {
    return WEBVIEW_ERROR_INVALID_ARGUMENT;
  }
  return api_filter([=] { return keyboard_hook::install(fn, arg); });
}

WEBVIEW_API webview_error_t webview_keyboard_hook_remove(void) {
  using namespace webview::detail;
  return api_filter([] { return keyboard_hook::remove(); });
}

WEBVIEW_API unsigned webview_feature_permissions(int feature) {
  using namespace webview::detail;
  return input_hooks::feature_permissions(feature);
}

WEBVIEW_API webview_error_t webview_permission_status(unsigned permission) {
  using namespace webview::detail;
  return api_filter([=] { return input_hooks::permission_status(permission); });
}

WEBVIEW_API webview_error_t webview_permission_request(unsigned permission) {
  using namespace webview::detail;
  return api_filter(
      [=] { return input_hooks::permission_request(permission); });
}

WEBVIEW_API webview_error_t webview_permission_set_auto_request(int on) {
  using namespace webview::detail;
  return api_filter([=] { return input_hooks::set_auto_request(on != 0); });
}

WEBVIEW_API webview_error_t webview_navigate(webview_t w, const char *url) {
  using namespace webview::detail;
  if (!url) {
    return WEBVIEW_ERROR_INVALID_ARGUMENT;
  }
  return api_filter([=] { return cast_to_webview(w)->navigate(url); });
}

WEBVIEW_API webview_error_t webview_set_html(webview_t w, const char *html) {
  using namespace webview::detail;
  if (!html) {
    return WEBVIEW_ERROR_INVALID_ARGUMENT;
  }
  return api_filter([=] { return cast_to_webview(w)->set_html(html); });
}

WEBVIEW_API webview_error_t webview_init(webview_t w, const char *js) {
  using namespace webview::detail;
  if (!js) {
    return WEBVIEW_ERROR_INVALID_ARGUMENT;
  }
  return api_filter([=] { return cast_to_webview(w)->init(js); });
}

WEBVIEW_API webview_error_t webview_eval(webview_t w, const char *js) {
  using namespace webview::detail;
  if (!js) {
    return WEBVIEW_ERROR_INVALID_ARGUMENT;
  }
  return api_filter([=] { return cast_to_webview(w)->eval(js); });
}

WEBVIEW_API webview_error_t webview_bind(webview_t w, const char *name,
                                         void (*fn)(const char *id,
                                                    const char *req, void *arg),
                                         void *arg) {
  using namespace webview::detail;
  if (!name || !fn) {
    return WEBVIEW_ERROR_INVALID_ARGUMENT;
  }
  return api_filter([=] {
    return cast_to_webview(w)->bind(
        name,
        [=](const std::string &seq, const std::string &req, void *arg_) {
          fn(seq.c_str(), req.c_str(), arg_);
        },
        arg);
  });
}

WEBVIEW_API webview_error_t webview_unbind(webview_t w, const char *name) {
  using namespace webview::detail;
  if (!name) {
    return WEBVIEW_ERROR_INVALID_ARGUMENT;
  }
  return api_filter([=] { return cast_to_webview(w)->unbind(name); });
}

WEBVIEW_API webview_error_t webview_return(webview_t w, const char *id,
                                           int status, const char *result) {
  using namespace webview::detail;
  if (!id || !result) {
    return WEBVIEW_ERROR_INVALID_ARGUMENT;
  }
  return api_filter(
      [=] { return cast_to_webview(w)->resolve(id, status, result); });
}

WEBVIEW_API const webview_version_info_t *webview_version(void) {
  return &webview::detail::library_version_info;
}

#endif // defined(__cplusplus) && !defined(WEBVIEW_HEADER)
#endif // WEBVIEW_C_API_IMPL_HH
