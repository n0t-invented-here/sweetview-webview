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

#ifndef WEBVIEW_BACKENDS_WIN32_EDGE_HH
#define WEBVIEW_BACKENDS_WIN32_EDGE_HH

#if defined(__cplusplus) && !defined(WEBVIEW_HEADER)

#include "../../macros.h"

#if defined(WEBVIEW_PLATFORM_WINDOWS) && defined(WEBVIEW_EDGE)

//
// ====================================================================
//
// This implementation uses Win32 API to create a native window. It
// uses Edge/Chromium webview2 backend as a browser engine.
//
// ====================================================================
//

#include "../../errors.hh"
#include "../../types.hh"
#include "../engine_base.hh"
#include "../http_request.hh"
#include "../native_library.hh"
#include "../platform/windows/com_init_wrapper.hh"
#include "../platform/windows/dpi.hh"
#include "../platform/windows/iid.hh"
#include "../platform/windows/reg_key.hh"
#include "../platform/windows/streaming_istream.hh"
#include "../platform/windows/theme.hh"
#include "../platform/windows/version.hh"
#include "../platform/windows/webview2/loader.hh"
#include "../user_script.hh"
#include "../utility/string.hh"

#include <atomic>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "version.lib")
#endif

namespace webview {
namespace detail {

using msg_cb_t = std::function<void(const std::string)>;

// Owns one reference to a COM interface and releases it when it goes, so an
// early return or an exception can never leak it.
template <typename T> class com_ptr {
public:
  com_ptr() noexcept = default;
  // Adopts a reference the caller already holds.
  explicit com_ptr(T *p) noexcept : m_p{p} {}
  com_ptr(const com_ptr &other) noexcept : m_p{other.m_p} {
    if (m_p) {
      m_p->AddRef();
    }
  }
  com_ptr(com_ptr &&other) noexcept : m_p{other.m_p} { other.m_p = nullptr; }
  com_ptr &operator=(com_ptr other) noexcept {
    std::swap(m_p, other.m_p);
    return *this;
  }
  ~com_ptr() { reset(); }

  // Takes a new reference of its own to p.
  static com_ptr add_ref(T *p) noexcept {
    if (p) {
      p->AddRef();
    }
    return com_ptr{p};
  }

  void reset() noexcept {
    if (m_p) {
      auto *p = m_p;
      m_p = nullptr;
      p->Release();
    }
  }
  T *get() const noexcept { return m_p; }
  T *operator->() const noexcept { return m_p; }
  explicit operator bool() const noexcept { return m_p != nullptr; }
  // For out-parameters: releases what is held and lends the slot.
  T **put() noexcept {
    reset();
    return &m_p;
  }
  void **put_void() noexcept { return reinterpret_cast<void **>(put()); }

private:
  T *m_p{};
};

// Owns a wide string WebView2 allocated with CoTaskMemAlloc.
class cotaskmem_wstr {
public:
  cotaskmem_wstr() noexcept = default;
  explicit cotaskmem_wstr(LPWSTR s) noexcept : m_s{s} {}
  cotaskmem_wstr(const cotaskmem_wstr &) = delete;
  cotaskmem_wstr &operator=(const cotaskmem_wstr &) = delete;
  ~cotaskmem_wstr() { CoTaskMemFree(m_s); }

  LPCWSTR get() const noexcept { return m_s; }
  LPWSTR *put() noexcept {
    CoTaskMemFree(m_s);
    m_s = nullptr;
    return &m_s;
  }
  std::string narrow() const {
    return m_s ? narrow_string(m_s) : std::string{};
  }

private:
  LPWSTR m_s{};
};

class webview2_com_handler
    : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
      public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
      public ICoreWebView2WebMessageReceivedEventHandler,
      public ICoreWebView2PermissionRequestedEventHandler,
      public ICoreWebView2WebResourceRequestedEventHandler,
      public ICoreWebView2ContentLoadingEventHandler {
  using webview2_com_handler_cb_t =
      std::function<void(ICoreWebView2Controller *, ICoreWebView2 *webview)>;
  using resource_requested_cb_t =
      std::function<void(ICoreWebView2WebResourceRequestedEventArgs *)>;
  using file_drop_message_cb_t =
      std::function<void(ICoreWebView2WebMessageReceivedEventArgs *)>;
  using content_loading_cb_t = std::function<void()>;

public:
  webview2_com_handler(HWND hwnd, msg_cb_t msgCb, webview2_com_handler_cb_t cb)
      : m_window(hwnd), m_msgCb(msgCb), m_cb(cb) {}

  virtual ~webview2_com_handler() {
    if (m_environment) {
      m_environment->Release();
    }
  }
  webview2_com_handler(const webview2_com_handler &other) = delete;
  webview2_com_handler &operator=(const webview2_com_handler &other) = delete;
  webview2_com_handler(webview2_com_handler &&other) = delete;
  webview2_com_handler &operator=(webview2_com_handler &&other) = delete;

  ULONG STDMETHODCALLTYPE AddRef() { return ++m_ref_count; }
  ULONG STDMETHODCALLTYPE Release() {
    if (m_ref_count > 1) {
      return --m_ref_count;
    }
    delete this;
    return 0;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID *ppv) {
    using namespace mswebview2::cast_info;

    if (!ppv) {
      return E_POINTER;
    }

    // All of the COM interfaces we implement should be added here regardless
    // of whether they are required.
    // This is just to be on the safe side in case the WebView2 Runtime ever
    // requests a pointer to an interface we implement.
    // The WebView2 Runtime must at the very least be able to get a pointer to
    // ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler when we use
    // our custom WebView2 loader implementation, and observations have shown
    // that it is the only interface requested in this case. None have been
    // observed to be requested when using the official WebView2 loader.

    if (cast_if_equal_iid(this, riid, controller_completed, ppv) ||
        cast_if_equal_iid(this, riid, environment_completed, ppv) ||
        cast_if_equal_iid(this, riid, message_received, ppv) ||
        cast_if_equal_iid(this, riid, permission_requested, ppv) ||
        cast_if_equal_iid(this, riid, web_resource_requested, ppv) ||
        cast_if_equal_iid(this, riid, content_loading, ppv)) {
      return S_OK;
    }

    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT res, ICoreWebView2Environment *env) {
    if (SUCCEEDED(res)) {
      if (m_environment) {
        m_environment->Release();
      }
      env->AddRef();
      m_environment = env;
      res = env->CreateCoreWebView2Controller(m_window, this);
      if (SUCCEEDED(res)) {
        return S_OK;
      }
    }
    try_create_environment();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT res,
                                   ICoreWebView2Controller *controller) {
    if (FAILED(res)) {
      // See try_create_environment() regarding
      // HRESULT_FROM_WIN32(ERROR_INVALID_STATE).
      // The result is E_ABORT if the parent window has been destroyed already.
      // Neither is worth retrying: signal the failure so embed()'s wait loop
      // returns an error instead of pumping messages forever.
      switch (res) {
      case HRESULT_FROM_WIN32(ERROR_INVALID_STATE):
      case E_ABORT:
        signal_failure();
        return S_OK;
      }
      // Retries; signals the failure itself once it gives up.
      try_create_environment();
      return S_OK;
    }

    com_ptr<ICoreWebView2> webview;
    if (!controller || FAILED(controller->get_CoreWebView2(webview.put())) ||
        !webview) {
      signal_failure();
      return S_OK;
    }
    ::EventRegistrationToken token;
    webview->add_WebMessageReceived(this, &token);
    webview->add_PermissionRequested(this, &token);
    webview->add_ContentLoading(this, &token);

    if (m_cb) {
      // The callback takes references of its own; ours goes with webview.
      m_cb(controller, webview.get());
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  Invoke(ICoreWebView2 * /*sender*/,
         ICoreWebView2WebMessageReceivedEventArgs *args) {
    // ICoreWebView2::WebMessageReceived fires only for the top-level
    // document; an iframe's chrome.webview.postMessage goes to
    // ICoreWebView2Frame2::WebMessageReceived, which is never subscribed.
    // So iframes cannot reach bindings through this path (the live iframe
    // test in sweetview-webview-bun checks it).
    if (!args) {
      return S_OK;
    }
    cotaskmem_wstr message;
    auto res = args->TryGetWebMessageAsString(message.put());
    if (FAILED(res) || !message.get()) {
      return S_OK;
    }
    // Posted by the file drop script with the dropped File objects attached;
    // never a binding call, so it never reaches m_msgCb.
    if (lstrcmpW(message.get(), file_drop_message()) == 0) {
      if (m_file_drop_cb) {
        try {
          m_file_drop_cb(args);
        } catch (...) {
          // Nothing may unwind into WebView2.
        }
      }
      return S_OK;
    }
    if (m_msgCb) {
      m_msgCb(message.narrow());
    }
    return S_OK;
  }

  // The web message the file drop script posts (see
  // webview_set_file_drop_handler).
  static const wchar_t *file_drop_message() noexcept {
    return L"__sweetview_file_drop__";
  }
  HRESULT STDMETHODCALLTYPE
  Invoke(ICoreWebView2 * /*sender*/,
         ICoreWebView2PermissionRequestedEventArgs *args) {
    COREWEBVIEW2_PERMISSION_KIND kind;
    args->get_PermissionKind(&kind);
    if (kind == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ) {
      args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  Invoke(ICoreWebView2 * /*sender*/,
         ICoreWebView2WebResourceRequestedEventArgs *args) {
    if (m_resource_requested_cb) {
      try {
        m_resource_requested_cb(args);
      } catch (...) {
        // The callback answers every failure itself; nothing may unwind
        // into WebView2.
      }
    }
    return S_OK;
  }

  // A new top-level document is starting to load (not raised for same-page
  // navigations).
  HRESULT STDMETHODCALLTYPE
  Invoke(ICoreWebView2 * /*sender*/,
         ICoreWebView2ContentLoadingEventArgs * /*args*/) {
    if (m_content_loading_cb) {
      try {
        m_content_loading_cb();
      } catch (...) {
        // Nothing may unwind into WebView2.
      }
    }
    return S_OK;
  }

  // Called for every request the WebResourceRequested filter lets through.
  void set_resource_requested_handler(resource_requested_cb_t cb) noexcept {
    m_resource_requested_cb = std::move(cb);
  }

  // Called for the web message the file drop script posts.
  void set_file_drop_message_handler(file_drop_message_cb_t cb) noexcept {
    m_file_drop_cb = std::move(cb);
  }

  // Called when a new top-level document starts loading.
  void set_content_loading_handler(content_loading_cb_t cb) noexcept {
    m_content_loading_cb = std::move(cb);
  }

  // The environment the controller was created from; null until then.
  ICoreWebView2Environment *environment() const noexcept {
    return m_environment;
  }

  // Set the function that will perform the initiating logic for creating
  // the WebView2 environment.
  void set_attempt_handler(std::function<HRESULT()> attempt_handler) noexcept {
    m_attempt_handler = attempt_handler;
  }

  // Drops the callbacks that point into the engine. WebView2 can hold this
  // handler past the engine (a creation still in flight, events queued
  // before the controller is released); after this they do nothing.
  void detach() noexcept {
    m_cb = nullptr;
    m_attempt_handler = nullptr;
    m_msgCb = nullptr;
    m_file_drop_cb = nullptr;
    m_content_loading_cb = nullptr;
  }

  // Retry creating a WebView2 environment.
  // The initiating logic for creating the environment is defined by the
  // caller of set_attempt_handler().
  void try_create_environment() noexcept {
    // WebView creation fails with HRESULT_FROM_WIN32(ERROR_INVALID_STATE) if
    // a running instance using the same user data folder exists, and the
    // Environment objects have different EnvironmentOptions.
    // Source: https://docs.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2environment?view=webview2-1.0.1150.38
    if (!m_attempt_handler) {
      return; // detached: the engine is gone
    }
    if (m_attempts < m_max_attempts) {
      ++m_attempts;
      auto res = m_attempt_handler();
      if (SUCCEEDED(res)) {
        return;
      }
      // Not entirely sure if this error code only applies to
      // CreateCoreWebView2Controller so we check here as well. Retrying
      // cannot help, so give up now rather than leave embed() waiting.
      if (res == HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) {
        signal_failure();
        return;
      }
      // Wait for m_sleep_ms before trying again.
      Sleep(m_sleep_ms);
      try_create_environment();
      return;
    }
    // Give up.
    signal_failure();
  }

private:
  // Tells the creation callback that no controller is coming, which ends
  // embed()'s wait loop with an error. Does nothing once detached.
  void signal_failure() noexcept {
    if (m_cb) {
      try {
        m_cb(nullptr, nullptr);
      } catch (...) {
      }
    }
  }

  HWND m_window;
  msg_cb_t m_msgCb;
  webview2_com_handler_cb_t m_cb;
  resource_requested_cb_t m_resource_requested_cb;
  file_drop_message_cb_t m_file_drop_cb;
  content_loading_cb_t m_content_loading_cb;
  ICoreWebView2Environment *m_environment = nullptr;
  std::atomic<ULONG> m_ref_count{1};
  std::function<HRESULT()> m_attempt_handler;
  unsigned int m_max_attempts = 60;
  unsigned int m_sleep_ms = 200;
  unsigned int m_attempts = 0;
};

class webview2_user_script_added_handler
    : public ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler {
public:
  using callback_fn = std::function<void(HRESULT errorCode, LPCWSTR id)>;

  webview2_user_script_added_handler(callback_fn cb) : m_cb{cb} {}

  virtual ~webview2_user_script_added_handler() = default;
  webview2_user_script_added_handler(
      const webview2_user_script_added_handler &other) = delete;
  webview2_user_script_added_handler &
  operator=(const webview2_user_script_added_handler &other) = delete;
  webview2_user_script_added_handler(
      webview2_user_script_added_handler &&other) = delete;
  webview2_user_script_added_handler &
  operator=(webview2_user_script_added_handler &&other) = delete;

  ULONG STDMETHODCALLTYPE AddRef() { return ++m_ref_count; }
  ULONG STDMETHODCALLTYPE Release() {
    if (m_ref_count > 1) {
      return --m_ref_count;
    }
    delete this;
    return 0;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID *ppv) {
    using namespace mswebview2::cast_info;

    if (!ppv) {
      return E_POINTER;
    }

    if (cast_if_equal_iid(this, riid,
                          add_script_to_execute_on_document_created_completed,
                          ppv)) {
      return S_OK;
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE Invoke(HRESULT res, LPCWSTR id) {
    m_cb(res, id);
    return S_OK;
  }

private:
  callback_fn m_cb;
  std::atomic<ULONG> m_ref_count{1};
};

class user_script::impl {
public:
  impl(const std::wstring &id, const std::wstring &code)
      : m_id{id}, m_code{code} {}

  impl(const impl &) = delete;
  impl &operator=(const impl &) = delete;
  impl(impl &&) = delete;
  impl &operator=(impl &&) = delete;

  const std::wstring &get_id() const { return m_id; }
  const std::wstring &get_code() const { return m_code; }

private:
  std::wstring m_id;
  std::wstring m_code;
};

class win32_edge_engine : public engine_base {
public:
  win32_edge_engine(bool debug, void *window) : engine_base{!window} {
    try {
      window_init(window);
      window_settings(debug);
      dispatch_size_default();
    } catch (...) {
      // A constructor that throws never reaches the destructor: tear down
      // whatever was built (windows, wndprocs that hold this, COM refs) so
      // nothing leaks and nothing calls back into a dead engine.
      deinit();
      throw;
    }
  }

  virtual ~win32_edge_engine() { deinit(); }

  win32_edge_engine(const win32_edge_engine &other) = delete;
  win32_edge_engine &operator=(const win32_edge_engine &other) = delete;
  win32_edge_engine(win32_edge_engine &&other) = delete;
  win32_edge_engine &operator=(win32_edge_engine &&other) = delete;

private:
  // Releases everything the constructor may have built. Safe on a partially
  // constructed engine: every step checks what exists first.
  void deinit() noexcept {
    // Answers queued by webview_complete_request do nothing from here on.
    mark_destroying();
    if (m_com_handler) {
      // No more requests reach this engine; the ones still open are
      // released by abort_pending_requests() below.
      m_com_handler->set_resource_requested_handler(nullptr);
      // WebView2 may still hold the handler; its creation callbacks capture
      // this engine (and embed()'s locals) and must never run again.
      m_com_handler->detach();
    }
    // Every streamed body still open ends now, before the controller closes:
    // WebView2's IO thread may be blocked in streaming_istream::Read(), and
    // it must not stay blocked while WebView2 shuts down.
    abort_active_streams();
    // Close the controller first so WebView2 tears down its navigation and
    // network stack before any deferral still open is released: a deferral
    // released unanswered can then never send its request to the network.
    if (m_controller) {
      m_controller->Close();
    }
    abort_pending_requests();
    if (m_com_handler) {
      m_com_handler->Release();
      m_com_handler = nullptr;
    }
    if (m_webview) {
      m_webview->Release();
      m_webview = nullptr;
    }
    if (m_controller) {
      m_controller->Release();
      m_controller = nullptr;
    }
    // Replace wndproc to avoid callbacks and other bad things during
    // destruction.
    auto wndproc = reinterpret_cast<LONG_PTR>(
        +[](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
          return DefWindowProcW(hwnd, msg, wp, lp);
        });
    if (m_widget) {
      SetWindowLongPtrW(m_widget, GWLP_WNDPROC, wndproc);
    }
    if (m_window && owns_window()) {
      SetWindowLongPtrW(m_window, GWLP_WNDPROC, wndproc);
    }
    if (m_widget) {
      DestroyWindow(m_widget);
      m_widget = nullptr;
    }
    if (m_window) {
      if (owns_window()) {
        DestroyWindow(m_window);
        if (m_window_counted) {
          m_window_counted = false;
          on_window_destroyed(true);
        }
      }
      m_window = nullptr;
    }
    if (owns_window()) {
      // Not strictly needed for windows to close immediately but aligns
      // behavior across backends.
      deplete_run_loop_event_queue();
    }
    // We need the message window in order to deplete the event queue.
    if (m_message_window) {
      SetWindowLongPtrW(m_message_window, GWLP_WNDPROC, wndproc);
      DestroyWindow(m_message_window);
    }
    // Work still queued never runs; it is destroyed here, not leaked.
    close_dispatch_queue();
  }

protected:
  noresult run_impl() override {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    return {};
  }
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
    if (m_controller) {
      return m_controller;
    }
    return error_info{WEBVIEW_ERROR_INVALID_STATE};
  }
  noresult terminate_impl() override {
    PostQuitMessage(0);
    return {};
  }
  // Queues f and posts one WM_APP per entry; the message window runs the
  // oldest entry for each WM_APP. The queue owns every entry, so nothing
  // leaks when a post fails or the window goes before the message arrives.
  noresult dispatch_impl(dispatch_fn_t f) override {
    std::lock_guard<std::mutex> lock{m_dispatch_mutex};
    if (!m_message_window) {
      return error_info{WEBVIEW_ERROR_INVALID_STATE,
                        "the message window is gone"};
    }
    m_dispatch_queue.push_back(std::move(f));
    if (!PostMessageW(m_message_window, WM_APP, 0, 0)) {
      // Still under the lock, so the entry just pushed is the last one.
      m_dispatch_queue.pop_back();
      return error_info{WEBVIEW_ERROR_UNSPECIFIED, "PostMessageW failed"};
    }
    return {};
  }

  // Runs the oldest queued entry, outside the lock so it may queue more.
  void run_next_dispatched() {
    dispatch_fn_t f;
    {
      std::lock_guard<std::mutex> lock{m_dispatch_mutex};
      if (m_dispatch_queue.empty()) {
        return;
      }
      f = std::move(m_dispatch_queue.front());
      m_dispatch_queue.pop_front();
    }
    if (f) {
      f();
    }
  }

  // The message window is going: refuse new work and destroy what is queued
  // (outside the lock, in case an entry's destructor dispatches).
  void close_dispatch_queue() {
    std::deque<dispatch_fn_t> dropped;
    {
      std::lock_guard<std::mutex> lock{m_dispatch_mutex};
      m_message_window = nullptr;
      dropped.swap(m_dispatch_queue);
    }
  }

  noresult set_title_impl(const std::string &title) override {
    SetWindowTextW(m_window, widen_string(title).c_str());
    return {};
  }

  noresult set_background_color_impl(int r, int g, int b, int a) override {
    if (!m_controller) {
      return error_info{WEBVIEW_ERROR_INVALID_STATE, "Controller is null"};
    }
    ICoreWebView2Controller2 *controller2 = nullptr;
    auto hr = m_controller->QueryInterface(
        __uuidof(ICoreWebView2Controller2),
        reinterpret_cast<void **>(&controller2));
    if (FAILED(hr) || !controller2) {
      return error_info{WEBVIEW_ERROR_UNSPECIFIED,
                        "ICoreWebView2Controller2 is not available"};
    }
    COREWEBVIEW2_COLOR color{};
    color.A = static_cast<BYTE>(a);
    color.R = static_cast<BYTE>(r);
    color.G = static_cast<BYTE>(g);
    color.B = static_cast<BYTE>(b);
    hr = controller2->put_DefaultBackgroundColor(color);
    controller2->Release();
    if (FAILED(hr)) {
      return error_info{WEBVIEW_ERROR_UNSPECIFIED,
                        "put_DefaultBackgroundColor failed"};
    }
    return {};
  }

  noresult set_allow_external_drop_impl(bool allow) override {
    m_allow_external_drop = allow ? 1 : 0;
    // Before the controller exists the value is applied by embed().
    return apply_allow_external_drop();
  }

  noresult apply_allow_external_drop() {
    if (m_allow_external_drop < 0 || !m_controller) {
      return {};
    }
    ICoreWebView2Controller4 *controller4 = nullptr;
    auto hr = m_controller->QueryInterface(
        __uuidof(ICoreWebView2Controller4),
        reinterpret_cast<void **>(&controller4));
    if (FAILED(hr) || !controller4) {
      return error_info{WEBVIEW_ERROR_UNSPECIFIED,
                        "ICoreWebView2Controller4 is not available"};
    }
    hr = controller4->put_AllowExternalDrop(m_allow_external_drop ? TRUE
                                                                  : FALSE);
    controller4->Release();
    if (FAILED(hr)) {
      return error_info{WEBVIEW_ERROR_UNSPECIFIED,
                        "put_AllowExternalDrop failed"};
    }
    return {};
  }

  // A request handed to the request handler and not yet answered.
  struct pending_request_t {
    com_ptr<ICoreWebView2Deferral> deferral;
    com_ptr<ICoreWebView2WebResourceRequestedEventArgs> args;
  };

  // Answer requests to http://webview.localhost/<path> from the asset table
  // (see webview_set_asset), and what it misses, plus every request to
  // http://frames.webview.localhost/<path>, from the request handler (see
  // webview_set_request_handler). Nothing reaches the network: every request
  // the filters match gets a response, a bare 500 when nothing better can be
  // built. Where the runtime has ICoreWebView2_22 the filters also catch
  // requests from service workers and shared workers; an older runtime
  // matches only requests from documents.
  noresult serve_assets() {
    m_com_handler->set_resource_requested_handler(
        [this](ICoreWebView2WebResourceRequestedEventArgs *args) {
          on_web_resource_requested(args);
        });
    const wchar_t *const filters[] = {L"http://webview.localhost/*",
                                      L"http://frames.webview.localhost/*"};
    com_ptr<ICoreWebView2_22> webview22;
    if (FAILED(m_webview->QueryInterface(__uuidof(ICoreWebView2_22),
                                         webview22.put_void()))) {
      webview22.reset();
    }
    for (const auto *filter : filters) {
      HRESULT hr;
      if (webview22) {
        hr = webview22->AddWebResourceRequestedFilterWithRequestSourceKinds(
            filter, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,
            COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
      } else {
        hr = m_webview->AddWebResourceRequestedFilter(
            filter, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
      }
      if (FAILED(hr)) {
        return error_info{WEBVIEW_ERROR_UNSPECIFIED,
                          "AddWebResourceRequestedFilter failed"};
      }
    }
    ::EventRegistrationToken token;
    if (FAILED(m_webview->add_WebResourceRequested(m_com_handler, &token))) {
      return error_info{WEBVIEW_ERROR_UNSPECIFIED,
                        "add_WebResourceRequested failed"};
    }
    return {};
  }

  // Narrows a string WebView2 allocated, and frees it.
  static std::string take_co_string(LPWSTR s) {
    cotaskmem_wstr owned{s};
    return owned.narrow();
  }

  // Reads the whole request body into out. False when the stream fails, so
  // the handler never sees a body cut short.
  static bool read_request_body(ICoreWebView2WebResourceRequest *req,
                                std::string &out) {
    com_ptr<IStream> content;
    if (FAILED(req->get_Content(content.put()))) {
      return false;
    }
    if (!content) {
      return true; // no body
    }
    LARGE_INTEGER zero{};
    auto hr = content->Seek(zero, STREAM_SEEK_SET, nullptr);
    // A stream that cannot seek is read from where it stands.
    if (FAILED(hr) && hr != E_NOTIMPL && hr != STG_E_INVALIDFUNCTION) {
      return false;
    }
    char buf[16384];
    for (;;) {
      ULONG read = 0;
      hr = content->Read(buf, static_cast<ULONG>(sizeof(buf)), &read);
      if (FAILED(hr)) {
        return false;
      }
      if (read > 0) {
        out.append(buf, read);
      }
      if (hr == S_FALSE || read == 0) {
        return true;
      }
    }
  }

  static std::string
  read_request_headers_json(ICoreWebView2WebResourceRequest *req) {
    http_headers_t headers;
    com_ptr<ICoreWebView2HttpRequestHeaders> collection;
    if (SUCCEEDED(req->get_Headers(collection.put())) && collection) {
      com_ptr<ICoreWebView2HttpHeadersCollectionIterator> it;
      if (SUCCEEDED(collection->GetIterator(it.put())) && it) {
        BOOL has = FALSE;
        if (FAILED(it->get_HasCurrentHeader(&has))) {
          has = FALSE;
        }
        while (has) {
          cotaskmem_wstr name;
          cotaskmem_wstr value;
          if (SUCCEEDED(it->GetCurrentHeader(name.put(), value.put()))) {
            headers.emplace_back(name.narrow(), value.narrow());
          }
          if (FAILED(it->MoveNext(&has))) {
            break;
          }
        }
      }
    }
    return http_headers_to_json(headers);
  }

  // A bare "500 Internal Server Error" with no body: the answer of last
  // resort, so a request never goes on to the network. When even that cannot
  // be built, a 500 with no reason phrase and no headers is tried. Returns
  // whether a response was actually set on args.
  //
  // Failing both means the WebView2 environment itself is unusable, which in
  // practice only happens while it is being torn down. A request left with no
  // response is passed through by WebView2, so callers holding a deferral
  // complete it only when this returns true (see complete_deferral).
  bool put_fail_closed_response(
      ICoreWebView2WebResourceRequestedEventArgs *args) noexcept {
    auto *env = m_com_handler ? m_com_handler->environment() : nullptr;
    if (!env || !args) {
      log_unanswered_request();
      return false;
    }
    com_ptr<ICoreWebView2WebResourceResponse> response;
    auto hr = env->CreateWebResourceResponse(
        nullptr, 500, L"Internal Server Error", L"Content-Type: text/plain",
        response.put());
    if (FAILED(hr) || !response) {
      response.reset();
      hr = env->CreateWebResourceResponse(nullptr, 500, L"", L"",
                                          response.put());
    }
    if (FAILED(hr) || !response ||
        FAILED(args->put_Response(response.get()))) {
      log_unanswered_request();
      return false;
    }
    return true;
  }

  static void log_unanswered_request() noexcept {
    OutputDebugStringW(
        L"webview: could not set even a bare 500 on a request; the WebView2 "
        L"environment is unusable (being torn down)\n");
  }

  // Completes a deferral. When no response could be set the request is left
  // deferred instead (the caller's reference is merely released), because
  // completing it would let WebView2 pass it on to the network unanswered.
  static void complete_deferral(ICoreWebView2Deferral *deferral,
                                bool answered) noexcept {
    if (deferral && answered) {
      deferral->Complete();
    }
  }

  HRESULT try_put_response(ICoreWebView2WebResourceRequestedEventArgs *args,
                           int status, const std::wstring &headers,
                           const std::string &body) {
    auto *env = m_com_handler ? m_com_handler->environment() : nullptr;
    if (!env || !args) {
      return E_POINTER;
    }
    com_ptr<IStream> stream;
    if (!body.empty()) {
      if (body.size() > static_cast<size_t>(UINT_MAX)) {
        return E_INVALIDARG; // SHCreateMemStream takes a UINT length
      }
      stream = com_ptr<IStream>{
          SHCreateMemStream(reinterpret_cast<const BYTE *>(body.data()),
                            static_cast<UINT>(body.size()))};
      if (!stream) {
        return E_OUTOFMEMORY;
      }
    }
    auto reason = widen_string(http_reason_phrase(status));
    com_ptr<ICoreWebView2WebResourceResponse> response;
    auto hr = env->CreateWebResourceResponse(
        stream.get(), status, reason.c_str(), headers.c_str(), response.put());
    if (FAILED(hr)) {
      return hr;
    }
    if (!response) {
      return E_FAIL;
    }
    return args->put_Response(response.get());
  }

  // Sets the response on the event args; an empty body sends no stream. When
  // the response cannot be built (headers WebView2 refuses, no memory for
  // the body stream) a bare 500 is set instead, never nothing. Returns
  // whether any response was set.
  bool put_response(ICoreWebView2WebResourceRequestedEventArgs *args,
                    int status, const std::wstring &headers,
                    const std::string &body) noexcept {
    HRESULT hr = E_FAIL;
    try {
      hr = try_put_response(args, status, headers, body);
    } catch (...) {
      hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) {
      return true;
    }
    return put_fail_closed_response(args);
  }

  void on_web_resource_requested(
      ICoreWebView2WebResourceRequestedEventArgs *args) noexcept {
    if (!args) {
      return;
    }
    try {
      handle_web_resource_request(args);
    } catch (...) {
      // Anything that went wrong before the request was handed off: answer
      // it here rather than let it reach the network.
      put_fail_closed_response(args);
    }
  }

  void handle_web_resource_request(
      ICoreWebView2WebResourceRequestedEventArgs *args) {
    com_ptr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(args->get_Request(request.put())) || !request) {
      put_fail_closed_response(args);
      return;
    }
    cotaskmem_wstr uri;
    if (FAILED(request->get_Uri(uri.put())) || !uri.get()) {
      put_fail_closed_response(args);
      return;
    }
    std::string url = uri.narrow();

    // The path is everything from the first '/' after "scheme://host".
    auto path_start = url.find("://");
    path_start = path_start == std::string::npos
                     ? std::string::npos
                     : url.find('/', path_start + 3);
    std::string origin =
        path_start == std::string::npos ? url : url.substr(0, path_start);
    for (auto &c : origin) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    bool is_frames_origin = origin == "http://frames.webview.localhost";

    // The asset table answers the main origin first, at no extra cost.
    if (!is_frames_origin && path_start != std::string::npos) {
      if (auto *asset = find_asset(url.substr(path_start))) {
        put_response(args, 200, widen_string("Content-Type: " + asset->mime),
                     asset->data);
        return;
      }
    }

    auto handler = request_handler();
    if (!handler) {
      put_response(args, 404, L"", std::string{});
      return;
    }

    std::string method = "GET";
    cotaskmem_wstr wmethod;
    if (SUCCEEDED(request->get_Method(wmethod.put())) && wmethod.get()) {
      method = wmethod.narrow();
    }
    auto headers_json = read_request_headers_json(request.get());
    std::string body;
    if (!read_request_body(request.get(), body)) {
      // A partial body must not reach the handler as if it were whole.
      put_response(args, 500, L"Content-Type: text/plain", std::string{});
      return;
    }
    request.reset();

    // Hold the request open until webview_complete_request answers it.
    com_ptr<ICoreWebView2Deferral> deferral;
    if (FAILED(args->GetDeferral(deferral.put())) || !deferral) {
      put_response(args, 500, L"", std::string{});
      return;
    }
    auto request_id = next_request_id();
    try {
      m_pending_requests.emplace(
          request_id,
          pending_request_t{
              deferral,
              com_ptr<ICoreWebView2WebResourceRequestedEventArgs>::add_ref(
                  args)});
    } catch (...) {
      complete_deferral(deferral.get(), put_fail_closed_response(args));
      return;
    }

    try {
      handler(request_id, method.c_str(), url.c_str(), headers_json.c_str(),
              body.empty() ? nullptr : body.data(), body.size(),
              request_handler_arg());
    } catch (...) {
      // A handler that throws must not strand the request: if it did not
      // answer before throwing, answer it here with the fallback 500. The
      // exception goes no further (WebView2 cannot take it).
      fail_pending_request(request_id);
    }
  }

  // Answers a still-open request with the fallback 500 and completes it.
  // A request the handler already answered through webview_complete_request
  // (queued, not yet run) is left alone so the fallback cannot replace that
  // answer. When not even the fallback can be set the request stays pending,
  // so abort_pending_requests() still releases it at teardown.
  void fail_pending_request(uint64_t request_id) noexcept {
    if (is_request_submitted(request_id)) {
      return; // answered; the queued completion delivers it
    }
    auto found = m_pending_requests.find(request_id);
    if (found == m_pending_requests.end()) {
      return; // already answered
    }
    if (!put_fail_closed_response(found->second.args.get())) {
      return; // left deferred: completing it would pass it to the network
    }
    complete_and_erase_pending_request(found);
  }

  // Completes the deferral of a request whose response has been set, and
  // forgets the request. Erased before Complete() so nothing reached from
  // Complete() can see it still pending.
  void complete_and_erase_pending_request(
      std::map<uint64_t, pending_request_t>::iterator found) noexcept {
    auto pending = std::move(found->second);
    m_pending_requests.erase(found);
    if (pending.deferral) {
      pending.deferral->Complete();
    }
  }

  // Sets the answer on the deferred request, falling back to a bare 500.
  // Returns whether any response was set; the deferral is left to the caller.
  bool answer_pending_request(pending_request_t &pending, int status,
                              const http_headers_t &headers,
                              const std::string &body) noexcept {
    std::wstring header_lines;
    return build_header_lines(headers, header_lines)
               ? put_response(pending.args.get(), status, header_lines, body)
               : put_fail_closed_response(pending.args.get());
  }

  // "Name: value\r\n" per header, as CreateWebResourceResponse takes them.
  // False when the headers cannot be converted.
  static bool build_header_lines(const http_headers_t &headers,
                                 std::wstring &out) noexcept {
    try {
      std::string lines;
      for (const auto &h : headers) {
        lines += h.first;
        lines += ": ";
        lines += h.second;
        lines += "\r\n";
      }
      out = widen_string(lines);
      return out.empty() == lines.empty();
    } catch (...) {
      return false;
    }
  }

  // When no response at all can be set the request stays pending (completing
  // it would pass it to the network); abort_pending_requests() releases it.
  void complete_request_impl(uint64_t request_id, int status,
                             const http_headers_t &headers,
                             const std::string &body) override {
    auto found = m_pending_requests.find(request_id);
    if (found == m_pending_requests.end()) {
      return; // already answered, or the webview is going away
    }
    if (!answer_pending_request(found->second, status, headers, body)) {
      return;
    }
    complete_and_erase_pending_request(found);
  }

  // Streamed responses (see webview_begin_response). The stream's state is
  // made and registered here, on the caller's thread, so chunks written right
  // after this returns are kept; the IStream over it is made and handed to
  // WebView2 on the UI thread, and from then on WebView2's COM references
  // alone own it. The table holds only the shared state, so ending,
  // aborting, or forgetting a response never touches the IStream's lifetime.
  noresult begin_response_impl(uint64_t request_id, int status,
                               http_headers_t headers) override {
    auto stream = std::make_shared<stream_state>();
    // When WebView2 lets go of the response early (the page cancelled it),
    // the state drops its buffer itself and this queues forgetting it.
    // Runs inside the IStream's destructor under the state's lock, so it only
    // queues: the UI thread takes m_streams_mutex later. deinit() marks the
    // engine destroyed before it aborts the streams (which waits out a
    // running callback and drops it), so this never reaches a dead engine.
    // It holds the state by address only, never by reference, so the state
    // does not keep itself alive.
    {
      auto alive = alive_token();
      auto key = reinterpret_cast<std::uintptr_t>(stream.get());
      stream->set_on_abandoned([this, alive, request_id, key] {
        if (!alive->load()) {
          return;
        }
        dispatch_if_alive([this, request_id, key] {
          forget_abandoned_stream(request_id, key);
        });
      });
    }
    {
      std::lock_guard<std::mutex> lock{m_streams_mutex};
      // NOLINTNEXTLINE(readability-container-contains): contains() requires C++20
      if (m_active_streams.count(request_id) > 0) {
        return error_info{WEBVIEW_ERROR_DUPLICATE,
                          "a response for this request has already begun"};
      }
      m_active_streams.emplace(request_id, stream);
    }
    noresult res;
    try {
      res = dispatch_if_alive([this, request_id, status, headers, stream] {
        start_streamed_response(request_id, status, headers, stream);
      });
    } catch (...) {
      drop_stream(request_id, stream);
      throw;
    }
    if (!res.ok()) {
      drop_stream(request_id, stream);
    }
    return res;
  }

  // Any thread: the stream is free-threaded, so no trip to the UI thread.
  noresult write_response_chunk_impl(uint64_t request_id,
                                     std::string chunk) override {
    std::shared_ptr<stream_state> stream;
    {
      std::lock_guard<std::mutex> lock{m_streams_mutex};
      auto found = m_active_streams.find(request_id);
      if (found == m_active_streams.end()) {
        return {}; // ended, never begun, or the webview is going away
      }
      if (found->second->is_abandoned()) {
        // WebView2 let go of the response (the page cancelled it): nobody
        // will read these bytes, so stop keeping them.
        found->second->abort();
        m_active_streams.erase(found);
        return {};
      }
      stream = found->second;
    }
    stream->push(chunk.data(), chunk.size());
    return {};
  }

  noresult end_response_impl(uint64_t request_id) override {
    std::lock_guard<std::mutex> lock{m_streams_mutex};
    auto found = m_active_streams.find(request_id);
    if (found == m_active_streams.end()) {
      return {}; // already ended, or never begun
    }
    // Finished while still registered, so there is never a moment when the
    // stream is open but out of reach of abort_active_streams().
    found->second->finish();
    m_active_streams.erase(found);
    return {};
  }

  // UI thread, queued by a stream's on_abandoned callback: forgets the
  // stream if it is still the one registered for request_id. It has
  // already aborted itself and dropped its buffer.
  void forget_abandoned_stream(uint64_t request_id,
                               std::uintptr_t key) noexcept {
    std::lock_guard<std::mutex> lock{m_streams_mutex};
    auto found = m_active_streams.find(request_id);
    if (found != m_active_streams.end() &&
        reinterpret_cast<std::uintptr_t>(found->second.get()) == key) {
      m_active_streams.erase(found);
    }
  }

  // UI thread. Sets the streamed response on the deferred request and
  // completes it; WebView2 then reads the body from the stream on its own
  // thread. When the request is gone (answered, cancelled) the stream is
  // dropped; when the response cannot be set the request gets the fallback
  // 500 and the stream is dropped.
  void start_streamed_response(uint64_t request_id, int status,
                               const http_headers_t &headers,
                               const std::shared_ptr<stream_state> &stream) {
    forget_submitted_request(request_id);
    auto found = m_pending_requests.find(request_id);
    if (found == m_pending_requests.end()) {
      drop_stream(request_id, stream);
      return;
    }
    std::wstring header_lines;
    HRESULT hr = E_FAIL;
    if (build_header_lines(headers, header_lines)) {
      try {
        // Ours only until CreateWebResourceResponse() takes its own
        // reference; released when this scope ends, leaving WebView2 as the
        // only owner. Dropped on failure, which aborts the state.
        com_ptr<streaming_istream> body{new streaming_istream{stream}};
        hr = try_put_stream_response(found->second.args.get(), status,
                                     header_lines, body.get());
      } catch (...) {
        hr = E_FAIL;
      }
    }
    if (FAILED(hr)) {
      drop_stream(request_id, stream);
      if (!put_fail_closed_response(found->second.args.get())) {
        return; // left deferred; abort_pending_requests() releases it
      }
    }
    complete_and_erase_pending_request(found);
  }

  HRESULT try_put_stream_response(
      ICoreWebView2WebResourceRequestedEventArgs *args, int status,
      const std::wstring &headers, IStream *stream) {
    auto *env = m_com_handler ? m_com_handler->environment() : nullptr;
    if (!env || !args || !stream) {
      return E_POINTER;
    }
    auto reason = widen_string(http_reason_phrase(status));
    com_ptr<ICoreWebView2WebResourceResponse> response;
    auto hr = env->CreateWebResourceResponse(stream, status, reason.c_str(),
                                             headers.c_str(), response.put());
    if (FAILED(hr)) {
      return hr;
    }
    if (!response) {
      return E_FAIL;
    }
    return args->put_Response(response.get());
  }

  // Aborts stream and forgets it, if it is still the one registered.
  void drop_stream(uint64_t request_id,
                   const std::shared_ptr<stream_state> &stream) noexcept {
    std::lock_guard<std::mutex> lock{m_streams_mutex};
    if (stream) {
      stream->abort();
    }
    auto found = m_active_streams.find(request_id);
    if (found != m_active_streams.end() &&
        found->second.get() == stream.get()) {
      m_active_streams.erase(found);
    }
  }

  // Ends every streamed body still open; a blocked Read() returns E_ABORT.
  void abort_active_streams() noexcept {
    std::map<uint64_t, std::shared_ptr<stream_state>> streams;
    {
      std::lock_guard<std::mutex> lock{m_streams_mutex};
      streams.swap(m_active_streams);
    }
    for (auto &entry : streams) {
      if (entry.second) {
        entry.second->abort();
      }
    }
  }

  // File drops (see webview_set_file_drop_handler). The drop script is
  // installed once by embed() and starts disabled in every document; this
  // flips the flag in the current document, and on_content_loading() sets it
  // again in each new one. Never blocks and never pumps messages (unlike
  // AddScriptToExecuteOnDocumentCreated), so it is safe to call from inside
  // a file drop handler or any other callback on the UI thread.
  noresult set_file_drop_handler_impl(bool enabled) override {
    if (enabled == m_file_drop_enabled) {
      return {};
    }
    m_file_drop_enabled = enabled;
    if (!m_webview) {
      return {}; // on_content_loading() applies it to the first document
    }
    return eval(file_drop_flag_script());
  }

  std::string file_drop_flag_script() const {
    return std::string{"if (window === window.top) {"
                       " window.__webview_file_drop_enabled__ = "} +
           (m_file_drop_enabled ? "true" : "false") + "; }";
  }

  // UI thread: a new top-level document is loading. Its drop script starts
  // disabled, so enable it there when a handler is set. ExecuteScript is
  // asynchronous; the drop script never overwrites a flag already set, so
  // the two may run in either order.
  void on_content_loading() {
    if (m_file_drop_enabled && m_webview) {
      eval(file_drop_flag_script());
    }
  }

  // Top-level document only, like the init script. Installed once and the
  // same in every document; the flag starts false unless the host already
  // set it (see on_content_loading). While enabled, a drag
  // that carries files is accepted (dragover) and its drop is kept from the
  // browser's default (opening the file) and posted to the host with the
  // File objects, whose paths WebView2 reveals only to the host. Page
  // listeners still see both events.
  static std::string create_file_drop_script() {
    return "(function() {\n\
  'use strict';\n\
  if (window !== window.top) {\n\
    return;\n\
  }\n\
  if (typeof window.__webview_file_drop_enabled__ !== 'boolean') {\n\
    window.__webview_file_drop_enabled__ = false;\n\
  }\n\
  function carriesFiles(e) {\n\
    return !!(window.__webview_file_drop_enabled__ && e.dataTransfer &&\n\
      e.dataTransfer.types &&\n\
      Array.from(e.dataTransfer.types).includes('Files'));\n\
  }\n\
  window.addEventListener('dragover', function(e) {\n\
    if (carriesFiles(e)) {\n\
      e.preventDefault();\n\
    }\n\
  }, true);\n\
  window.addEventListener('drop', function(e) {\n\
    if (!carriesFiles(e)) {\n\
      return;\n\
    }\n\
    e.preventDefault();\n\
    if (e.dataTransfer.files && e.dataTransfer.files.length &&\n\
        window.chrome && window.chrome.webview &&\n\
        window.chrome.webview.postMessageWithAdditionalObjects) {\n\
      window.chrome.webview.postMessageWithAdditionalObjects(\n\
        '__sweetview_file_drop__', Array.from(e.dataTransfer.files));\n\
    }\n\
  }, true);\n\
})()";
  }

  // UI thread: the drop script's message. Reads each attached File's path
  // (ICoreWebView2File) here, while the event args are valid, then queues
  // the handler call so WebMessageReceived returns first: the handler then
  // runs from the message loop, not inside a WebView2 event, and may set or
  // clear the drop handler, open dialogs, or destroy the webview.
  void on_file_drop_message(ICoreWebView2WebMessageReceivedEventArgs *args) {
    if (!file_drop_handler() || !args) {
      return;
    }
    com_ptr<ICoreWebView2WebMessageReceivedEventArgs2> args2;
    if (FAILED(args->QueryInterface(
            __uuidof(ICoreWebView2WebMessageReceivedEventArgs2),
            args2.put_void())) ||
        !args2) {
      return; // a WebView2 Runtime too old to attach objects
    }
    com_ptr<ICoreWebView2ObjectCollectionView> objects;
    if (FAILED(args2->get_AdditionalObjects(objects.put())) || !objects) {
      return;
    }
    UINT32 count = 0;
    if (FAILED(objects->get_Count(&count))) {
      return;
    }
    std::vector<std::string> paths;
    for (UINT32 i = 0; i < count; ++i) {
      com_ptr<IUnknown> item;
      if (FAILED(objects->GetValueAtIndex(i, item.put())) || !item) {
        continue;
      }
      com_ptr<ICoreWebView2File> file;
      if (FAILED(item->QueryInterface(__uuidof(ICoreWebView2File),
                                      file.put_void())) ||
          !file) {
        continue; // not a file from the operating system
      }
      cotaskmem_wstr wpath;
      if (FAILED(file->get_Path(wpath.put())) || !wpath.get() ||
          !*wpath.get()) {
        continue;
      }
      paths.push_back(wpath.narrow());
    }
    if (paths.empty()) {
      return;
    }
    // deliver_file_drop() reads the handler again when it runs, so a
    // handler cleared in between is never called.
    dispatch_if_alive([this, paths] { deliver_file_drop(paths); });
  }

  // Releases every request still open without answering it. Runs after the
  // controller is closed, so WebView2 has already abandoned these requests:
  // releasing a deferral without Complete() can never pass its request on
  // to the network.
  void abort_pending_requests() noexcept {
    std::map<uint64_t, pending_request_t> pending_requests;
    pending_requests.swap(m_pending_requests);
    // The map going out of scope releases every deferral and event args ref.
  }

  noresult set_size_impl(int width, int height, webview_hint_t hints) override {
    auto style = GetWindowLong(m_window, GWL_STYLE);
    if (hints == WEBVIEW_HINT_FIXED) {
      style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    } else {
      style |= (WS_THICKFRAME | WS_MAXIMIZEBOX);
    }
    SetWindowLong(m_window, GWL_STYLE, style);

    if (hints == WEBVIEW_HINT_MAX) {
      m_maxsz.x = width;
      m_maxsz.y = height;
    } else if (hints == WEBVIEW_HINT_MIN) {
      m_minsz.x = width;
      m_minsz.y = height;
    } else {
      auto dpi = get_window_dpi(m_window);
      m_dpi = dpi;
      auto scaled_size =
          scale_size(width, height, get_default_window_dpi(), dpi);
      auto frame_size =
          make_window_frame_size(m_window, scaled_size.cx, scaled_size.cy, dpi);
      SetWindowPos(m_window, nullptr, 0, 0, frame_size.cx, frame_size.cy,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE |
                       SWP_FRAMECHANGED);
    }
    return window_show();
  }

  noresult navigate_impl(const std::string &url) override {
    auto wurl = widen_string(url);
    m_webview->Navigate(wurl.c_str());
    return {};
  }

  noresult eval_impl(const std::string &js) override {
    // TODO: Skip if no content has begun loading yet. Can't check with
    //       ICoreWebView2::get_Source because it returns "about:blank".
    auto wjs = widen_string(js);
    m_webview->ExecuteScript(wjs.c_str(), nullptr);
    return {};
  }

  noresult set_html_impl(const std::string &html) override {
    m_webview->NavigateToString(widen_string(html).c_str());
    return {};
  }

  user_script add_user_script_impl(const std::string &js) override {
    auto wjs = widen_string(js);
    std::wstring script_id;
    bool done{};
    webview2_user_script_added_handler handler{[&](HRESULT res, LPCWSTR id) {
      if (SUCCEEDED(res)) {
        script_id = id;
      }
      done = true;
    }};
    auto res =
        m_webview->AddScriptToExecuteOnDocumentCreated(wjs.c_str(), &handler);
    if (SUCCEEDED(res)) {
      // We want to guard against executing the default `set_size` prematurely
      set_default_size_guard(true);
      // Sadly we need to pump the event loop in order to get the script ID.
      run_event_loop_while([&] { return !done; });
      // The user's `set_size` may have been executed from the depleted event queue,
      // and if so, guard against putting the default `set_size` back onto the queue.
      if (!m_is_window_shown) {
        set_default_size_guard(false);
        dispatch_size_default();
      }
    }
    // TODO: There's a non-zero chance that we didn't get the script ID.
    //       We need to convey the error somehow.
    return user_script{
        js, user_script::impl_ptr{new user_script::impl{script_id, wjs},
                                  [](user_script::impl *p) { delete p; }}};
  }

  void
  remove_all_user_scripts_impl(const std::list<user_script> &scripts) override {
    for (const auto &script : scripts) {
      const auto &id = script.get_impl().get_id();
      m_webview->RemoveScriptToExecuteOnDocumentCreated(id.c_str());
    }
  }

  bool are_user_scripts_equal_impl(const user_script &first,
                                   const user_script &second) override {
    const auto &first_id = first.get_impl().get_id();
    const auto &second_id = second.get_impl().get_id();
    return first_id == second_id;
  }

private:
  void window_init(void *window) {
    if (!is_webview2_available()) {
      throw exception{WEBVIEW_ERROR_MISSING_DEPENDENCY,
                      "WebView2 is unavailable"};
    }

    HINSTANCE hInstance = GetModuleHandle(nullptr);

    if (owns_window()) {
      m_com_init = {COINIT_APARTMENTTHREADED};
      enable_dpi_awareness();

      HICON icon = (HICON)LoadImage(
          hInstance, IDI_APPLICATION, IMAGE_ICON, GetSystemMetrics(SM_CXICON),
          GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);

      // Create a top-level window.
      WNDCLASSEXW wc;
      ZeroMemory(&wc, sizeof(WNDCLASSEX));
      wc.cbSize = sizeof(WNDCLASSEX);
      wc.hInstance = hInstance;
      wc.lpszClassName = L"webview";
      wc.hIcon = icon;
      wc.lpfnWndProc = (WNDPROC)(+[](HWND hwnd, UINT msg, WPARAM wp,
                                     LPARAM lp) -> LRESULT {
        win32_edge_engine *w{};

        if (msg == WM_NCCREATE) {
          auto *lpcs{reinterpret_cast<LPCREATESTRUCT>(lp)};
          w = static_cast<win32_edge_engine *>(lpcs->lpCreateParams);
          w->m_window = hwnd;
          SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
          enable_non_client_dpi_scaling_if_needed(hwnd);
          apply_window_theme(hwnd);
        } else {
          w = reinterpret_cast<win32_edge_engine *>(
              GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (!w) {
          return DefWindowProcW(hwnd, msg, wp, lp);
        }

        switch (msg) {
        case WM_SIZE:
          w->resize_widget();
          break;
        case WM_CLOSE:
          DestroyWindow(hwnd);
          break;
        case WM_DESTROY:
          w->m_window = nullptr;
          SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
          w->on_window_destroyed();
          break;
        case WM_GETMINMAXINFO: {
          auto lpmmi = (LPMINMAXINFO)lp;
          if (w->m_maxsz.x > 0 && w->m_maxsz.y > 0) {
            lpmmi->ptMaxSize = w->m_maxsz;
            lpmmi->ptMaxTrackSize = w->m_maxsz;
          }
          if (w->m_minsz.x > 0 && w->m_minsz.y > 0) {
            lpmmi->ptMinTrackSize = w->m_minsz;
          }
        } break;
        case 0x02E4 /*WM_GETDPISCALEDSIZE*/: {
          auto dpi = static_cast<int>(wp);
          auto *size{reinterpret_cast<SIZE *>(lp)};
          *size = w->get_scaled_size(w->m_dpi, dpi);
          return TRUE;
        }
        case 0x02E0 /*WM_DPICHANGED*/: {
          // Windows 10: The size we get here is exactly what we supplied to WM_GETDPISCALEDSIZE.
          // Windows 11: The size we get here is NOT what we supplied to WM_GETDPISCALEDSIZE.
          // Due to this difference, don't use the suggested bounds.
          auto dpi = static_cast<int>(HIWORD(wp));
          w->on_dpi_changed(dpi);
          break;
        }
        case WM_SETTINGCHANGE: {
          auto *area = reinterpret_cast<const wchar_t *>(lp);
          if (area) {
            w->on_system_setting_change(area);
          }
          break;
        }
        case WM_ACTIVATE:
          if (LOWORD(wp) != WA_INACTIVE) {
            w->focus_webview();
          }
          break;
        default:
          return DefWindowProcW(hwnd, msg, wp, lp);
        }
        return 0;
      });
      RegisterClassExW(&wc);

      CreateWindowW(L"webview", L"", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                    CW_USEDEFAULT, 0, 0, nullptr, nullptr, hInstance, this);
      if (!m_window) {
        throw exception{WEBVIEW_ERROR_INVALID_STATE, "Window is null"};
      }
      on_window_created();
      m_window_counted = true;

      m_dpi = get_window_dpi(m_window);
    } else {
      m_window = IsWindow(static_cast<HWND>(window))
                     ? static_cast<HWND>(window)
                     : *(static_cast<HWND *>(window));
      m_dpi = get_window_dpi(m_window);
    }
    // Create a window that WebView2 will be embedded into.
    WNDCLASSEXW widget_wc{};
    widget_wc.cbSize = sizeof(WNDCLASSEX);
    widget_wc.hInstance = hInstance;
    widget_wc.lpszClassName = L"webview_widget";
    widget_wc.lpfnWndProc = (WNDPROC)(+[](HWND hwnd, UINT msg, WPARAM wp,
                                          LPARAM lp) -> LRESULT {
      win32_edge_engine *w{};

      if (msg == WM_NCCREATE) {
        auto *lpcs{reinterpret_cast<LPCREATESTRUCT>(lp)};
        w = static_cast<win32_edge_engine *>(lpcs->lpCreateParams);
        w->m_widget = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
      } else {
        w = reinterpret_cast<win32_edge_engine *>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      }

      if (!w) {
        return DefWindowProcW(hwnd, msg, wp, lp);
      }

      switch (msg) {
      case WM_SIZE:
        w->resize_webview();
        break;
      case WM_DESTROY:
        w->m_widget = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
      default:
        return DefWindowProcW(hwnd, msg, wp, lp);
      }
      return 0;
    });
    RegisterClassExW(&widget_wc);
    CreateWindowExW(WS_EX_CONTROLPARENT, L"webview_widget", nullptr, WS_CHILD,
                    0, 0, 0, 0, m_window, nullptr, hInstance, this);
    if (!m_widget) {
      throw exception{WEBVIEW_ERROR_INVALID_STATE, "Widget window is null"};
    }

    // Create a message-only window for internal messaging.
    WNDCLASSEXW message_wc{};
    message_wc.cbSize = sizeof(WNDCLASSEX);
    message_wc.hInstance = hInstance;
    message_wc.lpszClassName = L"webview_message";
    message_wc.lpfnWndProc = (WNDPROC)(+[](HWND hwnd, UINT msg, WPARAM wp,
                                           LPARAM lp) -> LRESULT {
      win32_edge_engine *w{};

      if (msg == WM_NCCREATE) {
        auto *lpcs{reinterpret_cast<LPCREATESTRUCT>(lp)};
        w = static_cast<win32_edge_engine *>(lpcs->lpCreateParams);
        w->m_message_window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
      } else {
        w = reinterpret_cast<win32_edge_engine *>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      }

      if (!w) {
        return DefWindowProcW(hwnd, msg, wp, lp);
      }

      switch (msg) {
      case WM_APP:
        w->run_next_dispatched();
        break;
      case WM_DESTROY:
        w->close_dispatch_queue();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
      default:
        return DefWindowProcW(hwnd, msg, wp, lp);
      }
      return 0;
    });
    RegisterClassExW(&message_wc);
    CreateWindowExW(0, L"webview_message", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE,
                    nullptr, hInstance, this);
    if (!m_message_window) {
      throw exception{WEBVIEW_ERROR_INVALID_STATE, "Message window is null"};
    }
  }

  void window_settings(bool debug) {
    auto cb =
        std::bind(&win32_edge_engine::on_message, this, std::placeholders::_1);
    embed(m_widget, debug, cb).ensure_ok();
  }

  noresult window_show() {
    if (owns_window() && !m_is_window_shown) {
      ShowWindow(m_window, SW_SHOW);
      UpdateWindow(m_window);
      SetFocus(m_window);
      m_is_window_shown = true;
    }
    return {};
  }
  noresult embed(HWND wnd, bool debug, msg_cb_t cb) {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    flag.test_and_set();

    wchar_t currentExePath[MAX_PATH];
    GetModuleFileNameW(nullptr, currentExePath, MAX_PATH);
    wchar_t *currentExeName = PathFindFileNameW(currentExePath);

    wchar_t dataPath[MAX_PATH];
    if (!SUCCEEDED(
            SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, dataPath))) {
      return error_info{WEBVIEW_ERROR_UNSPECIFIED, "SHGetFolderPathW failed"};
    }
    wchar_t userDataFolder[MAX_PATH];
    PathCombineW(userDataFolder, dataPath, currentExeName);

    m_com_handler = new webview2_com_handler(
        wnd, cb,
        [&](ICoreWebView2Controller *controller, ICoreWebView2 *webview) {
          if (!controller || !webview) {
            flag.clear();
            return;
          }
          controller->AddRef();
          webview->AddRef();
          m_controller = controller;
          m_webview = webview;
          flag.clear();
        });

    m_com_handler->set_file_drop_message_handler(
        [this](ICoreWebView2WebMessageReceivedEventArgs *args) {
          on_file_drop_message(args);
        });
    m_com_handler->set_content_loading_handler(
        [this] { on_content_loading(); });

    m_com_handler->set_attempt_handler([&] {
      return m_webview2_loader.create_environment_with_options(
          nullptr, userDataFolder, nullptr, m_com_handler);
    });
    m_com_handler->try_create_environment();

    // Pump the message loop until WebView2 has finished initialization.
    bool got_quit_msg = false;
    MSG msg;
    while (flag.test_and_set() && GetMessageW(&msg, nullptr, 0, 0) >= 0) {
      if (msg.message == WM_QUIT) {
        got_quit_msg = true;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (got_quit_msg) {
      return error_info{WEBVIEW_ERROR_CANCELED};
    }
    if (!m_controller || !m_webview) {
      return error_info{WEBVIEW_ERROR_INVALID_STATE};
    }
    com_ptr<ICoreWebView2Settings> settings;
    auto res = m_webview->get_Settings(settings.put());
    if (res != S_OK || !settings) {
      return error_info{WEBVIEW_ERROR_UNSPECIFIED, "get_Settings failed"};
    }
    res = settings->put_AreDevToolsEnabled(debug ? TRUE : FALSE);
    if (res != S_OK) {
      return error_info{WEBVIEW_ERROR_UNSPECIFIED,
                        "put_AreDevToolsEnabled failed"};
    }
    res = settings->put_IsStatusBarEnabled(FALSE);
    if (res != S_OK) {
      return error_info{WEBVIEW_ERROR_UNSPECIFIED,
                        "put_IsStatusBarEnabled failed"};
    }
    add_init_script("function(message) {\n\
  return window.chrome.webview.postMessage(message);\n\
}");
    add_user_script(create_file_drop_script());
    auto served = serve_assets();
    if (!served.ok()) {
      return served;
    }
    apply_allow_external_drop();
    resize_webview();
    m_controller->put_IsVisible(TRUE);
    ShowWindow(m_widget, SW_SHOW);
    UpdateWindow(m_widget);
    if (owns_window()) {
      focus_webview();
    }
    return {};
  }

  void resize_widget() {
    if (m_widget) {
      RECT r{};
      if (GetClientRect(GetParent(m_widget), &r)) {
        MoveWindow(m_widget, r.left, r.top, r.right - r.left, r.bottom - r.top,
                   TRUE);
      }
    }
  }

  void resize_webview() {
    if (m_widget && m_controller) {
      RECT bounds{};
      if (GetClientRect(m_widget, &bounds)) {
        m_controller->put_Bounds(bounds);
      }
    }
  }

  void focus_webview() {
    if (m_controller) {
      m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
  }

  bool is_webview2_available() const noexcept {
    LPWSTR version_info = nullptr;
    auto res = m_webview2_loader.get_available_browser_version_string(
        nullptr, &version_info);
    // The result will be equal to HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
    // if the WebView2 runtime is not installed.
    auto ok = SUCCEEDED(res) && version_info;
    if (version_info) {
      CoTaskMemFree(version_info);
    }
    return ok;
  }

  void on_dpi_changed(int dpi) {
    auto scaled_size = get_scaled_size(m_dpi, dpi);
    auto frame_size =
        make_window_frame_size(m_window, scaled_size.cx, scaled_size.cy, dpi);
    SetWindowPos(m_window, nullptr, 0, 0, frame_size.cx, frame_size.cy,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_FRAMECHANGED);
    m_dpi = dpi;
  }

  SIZE get_size() const {
    RECT bounds;
    GetClientRect(m_window, &bounds);
    auto width = bounds.right - bounds.left;
    auto height = bounds.bottom - bounds.top;
    return {width, height};
  }

  SIZE get_scaled_size(int from_dpi, int to_dpi) const {
    auto size = get_size();
    return scale_size(size.cx, size.cy, from_dpi, to_dpi);
  }

  void on_system_setting_change(const wchar_t *area) {
    // Detect light/dark mode change in system.
    if (lstrcmpW(area, L"ImmersiveColorSet") == 0) {
      apply_window_theme(m_window);
    }
  }

  void run_event_loop_while(std::function<bool()> fn) override {
    MSG msg;
    while (fn() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  // The app is expected to call CoInitializeEx before
  // CreateCoreWebView2EnvironmentWithOptions.
  // Source: https://docs.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/webview2-idl#createcorewebview2environmentwithoptions
  com_init_wrapper m_com_init;
  HWND m_window = nullptr;
  HWND m_widget = nullptr;
  HWND m_message_window = nullptr;
  POINT m_minsz = POINT{0, 0};
  POINT m_maxsz = POINT{0, 0};
  DWORD m_main_thread = GetCurrentThreadId();
  ICoreWebView2 *m_webview = nullptr;
  ICoreWebView2Controller *m_controller = nullptr;
  webview2_com_handler *m_com_handler = nullptr;
  mswebview2::loader m_webview2_loader;
  int m_dpi{};
  bool m_is_window_shown{};
  // Requests handed to the request handler and not yet answered.
  std::map<uint64_t, pending_request_t> m_pending_requests;
  // Streamed bodies begun and not yet ended, by request id. Guarded by
  // m_streams_mutex: chunks arrive from any thread.
  std::mutex m_streams_mutex;
  std::map<uint64_t, std::shared_ptr<stream_state>> m_active_streams;
  // Whether the drop script (see create_file_drop_script) takes file drops;
  // UI thread only.
  bool m_file_drop_enabled{};
  // Work queued by dispatch_impl, run in order by the message window.
  std::mutex m_dispatch_mutex;
  std::deque<dispatch_fn_t> m_dispatch_queue;
  // -1 until webview_set_allow_external_drop, then 0 or 1.
  int m_allow_external_drop = -1;
  // True once on_window_created() counted the owned window, so teardown
  // uncounts it exactly once (and never for a window that was never made).
  bool m_window_counted = false;
};

} // namespace detail

using browser_engine = detail::win32_edge_engine;

} // namespace webview

#endif // defined(WEBVIEW_PLATFORM_WINDOWS) && defined(WEBVIEW_EDGE)
#endif // defined(__cplusplus) && !defined(WEBVIEW_HEADER)
#endif // WEBVIEW_BACKENDS_WIN32_EDGE_HH
