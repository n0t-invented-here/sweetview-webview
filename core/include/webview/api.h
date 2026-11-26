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

#ifndef WEBVIEW_API_H
#define WEBVIEW_API_H

#include "errors.h"
#include "macros.h"
#include "types.h"

#include <stddef.h>

/*
 * Linux: Sweetview keeps the upstream webview/webview C ABI, so every
 * function below exists on every platform and a program built against it
 * links everywhere. Sweetview's fork additions (assets, the request handler
 * and streaming, background color, external drop control, the file drop
 * handler, input hooks and permissions) are implemented for Windows
 * (WebView2) and macOS (WKWebView) only. On Linux (GTK/WebKitGTK) they are
 * accepted and do nothing, or fail with WEBVIEW_ERROR_UNSPECIFIED where a
 * silent no-op would mislead (the input hooks); each function's Remarks say
 * which.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates a new webview instance.
 *
 * @param debug Enable developer tools if supported by the backend.
 * @param window Optional native window handle, i.e. @c GtkWindow pointer
 *        @c NSWindow pointer (Cocoa) or @c HWND (Win32). If non-null,
 *        the webview widget is embedded into the given window, and the
 *        caller is expected to assume responsibility for the window as
 *        well as application lifecycle. If the window handle is null,
 *        a new window is created and both the window and application
 *        lifecycle are managed by the webview instance.
 * @remark Win32: The function also accepts a pointer to @c HWND (Win32) in the
 *         window parameter for backward compatibility.
 * @remark Win32/WebView2: @c CoInitializeEx should be called with
 *         @c COINIT_APARTMENTTHREADED before attempting to call this function
 *         with an existing window. Omitting this step may cause WebView2
 *         initialization to fail.
 * @return @c NULL on failure. Creation can fail for various reasons such
 *         as when required runtime dependencies are missing or when window
 *         creation fails.
 * @retval WEBVIEW_ERROR_MISSING_DEPENDENCY
 *         May be returned if WebView2 is unavailable on Windows.
 */
WEBVIEW_API webview_t webview_create(int debug, void *window);

/**
 * Destroys a webview instance and closes the native window.
 *
 * @param w The webview instance.
 */
WEBVIEW_API webview_error_t webview_destroy(webview_t w);

/**
 * Runs the main loop until it's terminated.
 *
 * @param w The webview instance.
 */
WEBVIEW_API webview_error_t webview_run(webview_t w);

/**
 * Stops the main loop. It is safe to call this function from another other
 * background thread.
 *
 * @param w The webview instance.
 */
WEBVIEW_API webview_error_t webview_terminate(webview_t w);

/**
 * Schedules a function to be invoked on the thread with the run/event loop.
 *
 * Since library functions generally do not have thread safety guarantees,
 * this function can be used to schedule code to execute on the main/GUI
 * thread and thereby make that execution safe in multi-threaded applications.
 *
 * @param w The webview instance.
 * @param fn The function to be invoked.
 * @param arg An optional argument passed along to the callback function.
 */
WEBVIEW_API webview_error_t webview_dispatch(webview_t w,
                                             void (*fn)(webview_t w, void *arg),
                                             void *arg);

/**
 * Returns the native handle of the window associated with the webview instance.
 * The handle can be a @c GtkWindow pointer (GTK), @c NSWindow pointer (Cocoa)
 * or @c HWND (Win32).
 *
 * @param w The webview instance.
 * @return The handle of the native window.
 */
WEBVIEW_API void *webview_get_window(webview_t w);

/**
 * Get a native handle of choice.
 *
 * @param w The webview instance.
 * @param kind The kind of handle to retrieve.
 * @return The native handle or @c NULL.
 * @since 0.11
 */
WEBVIEW_API void *webview_get_native_handle(webview_t w,
                                            webview_native_handle_kind_t kind);

/**
 * Updates the title of the native window.
 *
 * @param w The webview instance.
 * @param title The new title.
 */
WEBVIEW_API webview_error_t webview_set_title(webview_t w, const char *title);

/**
 * Updates the size of the native window.
 *
 * Remarks:
 * - Using WEBVIEW_HINT_MAX for setting the maximum window size is not
 *   supported with GTK 4 because X11-specific functions such as
 *   gtk_window_set_geometry_hints were removed. This option has no effect
 *   when using GTK 4.
 * - GTK 4 can set a default/initial window size if done early enough;
 *   otherwise, this function has no effect. GTK 4 (unlike 3) can't resize
 *   a window after it has been set up.
 *
 * @param w The webview instance.
 * @param width New width.
 * @param height New height.
 * @param hints Size hints.
 */
WEBVIEW_API webview_error_t webview_set_size(webview_t w, int width, int height,
                                             webview_hint_t hints);

/**
 * Sets the color the browser paints behind the page. With alpha 0 and a page
 * whose own background is transparent, the native window shows through.
 *
 * Every channel must be 0-255; anything outside that returns
 * WEBVIEW_ERROR_INVALID_ARGUMENT and changes nothing.
 *
 * Remarks:
 * - Windows: sets ICoreWebView2Controller2::DefaultBackgroundColor. WebView2
 *   takes only alpha 0 (fully transparent) or 255 (opaque); any other alpha
 *   is refused by WebView2 and returns WEBVIEW_ERROR_UNSPECIFIED. A Runtime
 *   without ICoreWebView2Controller2 also returns WEBVIEW_ERROR_UNSPECIFIED.
 *   The host window still needs DWM composition that honors alpha (for
 *   example DwmEnableBlurBehindWindow with an empty region) for true
 *   transparency.
 * - macOS: sets the WKWebView's drawsBackground (KVC) to NO when @p a is 0
 *   and YES otherwise; sets this color as the window's background and makes
 *   the window non-opaque unless @p a is 255.
 * - Linux: accepted; does nothing (see "Linux" in the file overview).
 *
 * @param w The webview instance.
 * @param r Red, 0-255.
 * @param g Green, 0-255.
 * @param b Blue, 0-255.
 * @param a Alpha, 0-255 (0 is fully transparent).
 * @return WEBVIEW_ERROR_INVALID_ARGUMENT for a channel outside 0-255;
 *         WEBVIEW_ERROR_INVALID_STATE when the browser is not created yet or
 *         already gone; WEBVIEW_ERROR_UNSPECIFIED when the platform refuses
 *         the color (see Remarks).
 */
WEBVIEW_API webview_error_t webview_set_background_color(webview_t w, int r,
                                                         int g, int b, int a);

/**
 * Serves bytes at a path of the webview's own origin, so the page can be
 * loaded with webview_navigate instead of webview_set_html (which Windows
 * caps at 2 MB) and can fetch scripts, styles, images and models by relative
 * URL. Nothing reaches the network. The bytes are copied; calling again with
 * the same path replaces them.
 *
 * The origin is fixed per platform:
 * - Windows: http://webview.localhost (WebView2's WebResourceRequested event;
 *   requests to any other origin are untouched).
 * - macOS: webview://localhost (a WKURLSchemeHandler for the scheme "webview",
 *   registered when the WKWebView is created).
 * - Linux: accepted and stored; not served (see "Linux" in the file overview).
 *
 * Validation, all before anything is stored: @p path and @p mime must not be
 * NULL, @p data may be NULL only when @p len is 0, @p mime must be a valid
 * HTTP header value (no CR, LF, NUL or other control character but tab, so it
 * cannot split or add a header), and @p len must be at most 4294967295
 * (UINT32_MAX). Anything else returns WEBVIEW_ERROR_INVALID_ARGUMENT and
 * leaves the asset table as it was.
 *
 * Example:
 * @code{.c}
 * webview_set_asset(w, "index.html", "text/html; charset=utf-8", html, strlen(html));
 * webview_set_asset(w, "main.js", "text/javascript", js, js_len);
 * webview_navigate(w, "http://webview.localhost/index.html");
 * @endcode
 *
 * @param w The webview instance.
 * @param path URL path without the leading slash. A request's query string
 *             and fragment are ignored when matching. Must not need
 *             percent-encoding.
 * @param mime Value for the Content-Type header.
 * @param data The bytes, or NULL when @p len is 0.
 * @param len Length of @p data in bytes, at most 4294967295 (UINT32_MAX).
 * @return WEBVIEW_ERROR_INVALID_ARGUMENT for a bad argument (see above).
 */
WEBVIEW_API webview_error_t webview_set_asset(webview_t w, const char *path,
                                              const char *mime,
                                              const void *data, size_t len);

/**
 * Answers requests to the webview's origins in code instead of from fixed
 * bytes. Nothing reaches the network and no socket is opened.
 *
 * Two origins go to the handler:
 * - The main origin of webview_set_asset (Windows: http://webview.localhost,
 *   macOS: webview://localhost), for any path with no asset; an asset still
 *   answers first.
 * - A second origin for content that must be cross-origin from the page, for
 *   example a sandboxed iframe (Windows: http://frames.webview.localhost,
 *   macOS: webview-frames://localhost). Every request to it goes to the
 *   handler.
 *
 * Without a handler both answer 404 for anything with no asset. Passing
 * @p fn NULL removes the handler; requests already handed to it stay open
 * until completed or until the webview is destroyed (see Remarks for what
 * destroying does to them).
 *
 * @p fn runs on the UI thread. It must not block: it hands the request off and
 * returns, and the answer goes back with webview_complete_request.
 *
 * A request whose body cannot be read in full never reaches @p fn: Windows
 * answers it 500, macOS fails the task with an NSError.
 *
 * If @p fn throws (C++ callers), the exception goes no further: a request it
 * neither answered with webview_complete_request nor began with
 * webview_begin_response is answered with a bare 500.
 *
 * Remarks:
 * - Windows: WebView2's WebResourceRequested event, deferred
 *   (ICoreWebView2Deferral) until the request is completed. Where the
 *   WebView2 Runtime has ICoreWebView2_22 (Runtime 122.0.2365.46 or later,
 *   SDK 1.0.2365.46; this build is configured with SDK 1.0.2478.35) the
 *   filter is added with source kinds "all", so requests from service
 *   workers, shared workers and cross-origin iframes are caught too. An
 *   older Runtime falls back to the legacy filter, which misses requests
 *   from service and shared workers and from cross-origin iframes. When a
 *   response cannot be built (WebView2 refuses the headers, no memory for
 *   the body) the request is answered with a bare 500 instead, so it never
 *   goes on to the network. Destroying the webview aborts every stream
 *   still open, closes the WebView2 controller, and releases the deferrals
 *   and event arguments of requests still pending without answering them;
 *   answers still queued do nothing when they run.
 * - macOS: a WKURLSchemeHandler for the schemes "webview" and
 *   "webview-frames". A task WebKit stops before it is completed is dropped,
 *   and completing it later does nothing. Destroying the webview releases
 *   the tasks still open (retained by the engine) without answering them;
 *   WebKit stops them as the WKWebView goes. Answers still queued do nothing
 *   when they run.
 * - Linux: accepted and stored; not served (see "Linux" in the file
 *   overview).
 *
 * @param w The webview instance.
 * @param fn The handler, or NULL to remove it.
 * @param arg Passed to @p fn as is.
 */
WEBVIEW_API webview_error_t webview_set_request_handler(
    webview_t w, webview_request_handler_t fn, void *arg);

/**
 * Answers a request handed to the request handler (see
 * webview_set_request_handler). Safe to call from any thread; the response is
 * delivered on the UI thread. The headers are checked and the headers and
 * body are copied before this returns. Completing a request twice, or one the
 * browser has since cancelled, does nothing.
 *
 * Safe to call at any time against webview_destroy on another thread: once
 * webview_destroy has begun, this returns WEBVIEW_ERROR_INVALID_STATE, and an
 * answer queued before then does nothing when it runs. @p w must be a handle
 * from webview_create.
 *
 * Repeated header names are merged differently per platform. Windows sends
 * each [name, value] pair as its own header line. macOS (NSHTTPURLResponse)
 * holds one value per name: names match without regard to case, repeats are
 * joined with ", ", and Set-Cookie, which cannot be joined, keeps only the
 * first value, so macOS answers with at most one Set-Cookie per response.
 *
 * @param w The webview instance.
 * @param request_id The @c request_id the handler received.
 * @param status HTTP status code, 100-599.
 * @param headers_json Response headers, as a JSON object of name to value
 *                     ({"Content-Type": "text/html"}) or a JSON array of
 *                     [name, value] pairs, which allows a name to repeat
 *                     ([["X-Multi","a"],["X-Multi","b"]]). Values may be
 *                     strings, numbers or booleans; null drops the header.
 *                     Parsed as strict JSON: no trailing text, no raw
 *                     control characters or lone surrogates in strings,
 *                     valid UTF-8, and no U+0000. Each name must be an
 *                     RFC 9110 token and each value free of CR, LF, NUL and
 *                     other control characters but tab. Anything else
 *                     returns WEBVIEW_ERROR_INVALID_ARGUMENT and answers
 *                     nothing. NULL or "" for none.
 * @param body The response body, or NULL when @p body_len is 0.
 * @param body_len Length of @p body in bytes, at most 4294967295 (UINT32_MAX);
 *                 larger returns WEBVIEW_ERROR_INVALID_ARGUMENT.
 * @return WEBVIEW_ERROR_INVALID_STATE when @p w is destroyed or being
 *         destroyed; WEBVIEW_ERROR_INVALID_ARGUMENT for a bad argument or bad
 *         headers.
 */
WEBVIEW_API webview_error_t webview_complete_request(webview_t w,
                                                     uint64_t request_id,
                                                     int status,
                                                     const char *headers_json,
                                                     const void *body,
                                                     size_t body_len);

/**
 * Starts a streamed answer to a request handed to the request handler (see
 * webview_set_request_handler): sends the status and headers now, then the
 * body arrives piece by piece through webview_write_response_chunk and ends
 * with webview_end_response. Where the engine delivers partial bodies
 * (macOS), Server-Sent Events (text/event-stream) and large media reach the
 * page as they are produced; on Windows the page sees the body only once it
 * has ended (see Remarks). Use instead of webview_complete_request, never
 * both for one request.
 *
 * Safe to call from any thread, under the same rules as
 * webview_complete_request: @p status and @p headers_json are checked before
 * this returns, and once webview_destroy has begun this returns
 * WEBVIEW_ERROR_INVALID_STATE. Starting a request that was already answered
 * or cancelled does nothing; the chunks written for it are dropped. Starting
 * one whose streamed response has already begun returns
 * WEBVIEW_ERROR_DUPLICATE on Windows while that stream is open; macOS
 * ignores the second start.
 *
 * Remarks:
 * - Windows: the response's content is an IStream whose Read() blocks on
 *   WebView2's background thread until a chunk or the end arrives, so the
 *   UI thread never waits and only unread bytes are held. Chunks may be
 *   written before the response reaches WebView2. WebView2 itself reads the
 *   whole stream before it hands the response to the page (a documented
 *   WebView2 limitation: a WebResourceRequested response is never delivered
 *   in parts), so the page receives the body at webview_end_response, not
 *   chunk by chunk. Destroying the webview aborts every stream still open
 *   (WebView2's pending Read() fails) and drops queued steps.
 * - macOS: WKURLSchemeTask didReceiveResponse:, then didReceiveData: per
 *   chunk and didFinish, each on the main queue in the order called. A task
 *   WebKit stops is forgotten, and later chunks and the end do nothing.
 *   Destroying the webview releases open tasks without finishing them.
 * - Linux: accepted; does nothing (see "Linux" in the file overview).
 *
 * Calling webview_end_response for a request that was never begun does
 * nothing, so webview_complete_request may still answer it.
 *
 * @param w The webview instance.
 * @param request_id The @c request_id the handler received.
 * @param status HTTP status code, 100-599.
 * @param headers_json Response headers, exactly as for
 *                     webview_complete_request. NULL or "" for none.
 * @return WEBVIEW_ERROR_INVALID_STATE when @p w is destroyed or being
 *         destroyed; WEBVIEW_ERROR_INVALID_ARGUMENT for a bad argument or bad
 *         headers (request_id 0, status outside 100-599);
 *         WEBVIEW_ERROR_DUPLICATE (Windows) when a stream for @p request_id
 *         is already open.
 */
WEBVIEW_API webview_error_t webview_begin_response(webview_t w,
                                                   uint64_t request_id,
                                                   int status,
                                                   const char *headers_json);

/**
 * Appends a piece of the body to a response started with
 * webview_begin_response. Safe to call from any thread; the bytes are copied
 * before this returns. A chunk for a response that has ended, was never
 * started, or that the page abandoned is dropped. @p chunk_len 0 does
 * nothing once the checks below pass (after webview_destroy has begun it
 * still returns WEBVIEW_ERROR_INVALID_STATE).
 *
 * @param w The webview instance.
 * @param request_id The @c request_id passed to webview_begin_response.
 * @param chunk The bytes, or NULL when @p chunk_len is 0.
 * @param chunk_len Length of @p chunk in bytes, at most 4294967295
 *                  (UINT32_MAX).
 * @return WEBVIEW_ERROR_INVALID_STATE when @p w is destroyed or being
 *         destroyed; WEBVIEW_ERROR_INVALID_ARGUMENT for a bad argument.
 */
WEBVIEW_API webview_error_t webview_write_response_chunk(webview_t w,
                                                         uint64_t request_id,
                                                         const void *chunk,
                                                         size_t chunk_len);

/**
 * Ends a response started with webview_begin_response: the page sees the end
 * of the body once it has read every chunk written before. Safe to call from
 * any thread. Ending a response twice, or one never started, does nothing.
 *
 * @param w The webview instance.
 * @param request_id The @c request_id passed to webview_begin_response.
 * @return WEBVIEW_ERROR_INVALID_STATE when @p w is destroyed or being
 *         destroyed; WEBVIEW_ERROR_INVALID_ARGUMENT for a bad argument.
 */
WEBVIEW_API webview_error_t webview_end_response(webview_t w,
                                                 uint64_t request_id);

/**
 * Sets whether files and other content dragged from outside the app can be
 * dropped on the page. Allowed by default. Drags that start inside the page
 * (in-page drag and drop) keep working either way. A file drop handler (see
 * webview_set_file_drop_handler) receives drops only while external drops
 * are allowed.
 *
 * Remarks:
 * - Windows: ICoreWebView2Controller4::AllowExternalDrop, which allows or
 *   refuses external drops for the whole WebView2 control. A Runtime without
 *   ICoreWebView2Controller4 returns WEBVIEW_ERROR_UNSPECIFIED.
 * - macOS: enforced by the engine's WKWebView subclass (SweetviewWKWebView):
 *   while refused, a drag whose NSDraggingInfo.draggingSource is nil (it
 *   comes from another app, such as Finder) gets NSDragOperationNone and
 *   never reaches WebKit, while drags from inside the app still go to
 *   WebKit. Each drag session is routed once when it enters; turning
 *   external drops off mid-drag ends the session it affects.
 * - Linux: accepted; does nothing (see "Linux" in the file overview).
 *
 * @param w The webview instance.
 * @param allow Non-zero to allow, 0 to refuse.
 */
WEBVIEW_API webview_error_t webview_set_allow_external_drop(webview_t w,
                                                            int allow);

/**
 * Sets the function that receives the real filesystem paths of files dropped
 * on the webview from the operating system (Explorer, Finder). While a
 * handler is set, a drop of files is taken by the webview instead of the page,
 * and the browser does not navigate to the dropped file. Drags inside the
 * page, and drags that carry no files, are not affected. Passing @p fn NULL
 * removes the handler and restores the page's own handling. Call from the UI
 * thread.
 *
 * On both Windows and macOS the handler needs external drops allowed (see
 * webview_set_allow_external_drop), and @p fn is called asynchronously, from
 * the UI thread's message loop after the operating system's drop event has
 * returned, so it may set or clear the handler, open dialogs, or destroy the
 * webview. @p paths_json is a JSON array of strings; @p fn is never called
 * with an empty array.
 *
 * Setting the handler is transactional: when the backend refuses the change
 * (for example the Windows drop script cannot be installed), the previous
 * handler and argument stay in place and the error is returned.
 *
 * Remarks:
 * - Windows: drops are captured only on the trusted top-level document,
 *   never inside an iframe (cross-origin or not). The top-level document's
 *   drop event hands the dropped File objects to the host with
 *   chrome.webview.postMessageWithAdditionalObjects, and WebView2's
 *   ICoreWebView2File attachment gives their paths; a drop on an iframe is
 *   left to the iframe and never reaches @p fn. Needs WebView2 Runtime 113
 *   or later.
 * - macOS: the engine's WKWebView subclass (SweetviewWKWebView) takes a
 *   drag session when a handler is set, its pasteboard holds file URLs
 *   (public.file-url) and its source allows copying; any other drag goes to
 *   WebKit. The paths are read from the pasteboard during
 *   performDragOperation: and @p fn runs on the main queue afterwards.
 *   Clearing the handler mid-drag refuses the drop.
 * - Linux: accepted; does nothing (see "Linux" in the file overview).
 *
 * @param w The webview instance.
 * @param fn The handler, or NULL to remove it.
 * @param arg Passed to @p fn as is.
 */
WEBVIEW_API webview_error_t webview_set_file_drop_handler(
    webview_t w, webview_file_drop_handler_t fn, void *arg);

/**
 * Installs a system-wide mouse hook: @p fn sees every mouse event on the
 * system before any window does, and decides per event whether it goes on or
 * is swallowed. This is how a click-through overlay window gets some of the
 * buttons over what it draws without ever taking the others: pass every
 * event through except, for example, right and middle presses over something
 * it draws.
 * One hook per process; it belongs to no webview instance.
 *
 * Call it on the UI thread before @c webview_run: @p fn runs on that thread
 * from inside the run loop, and must return quickly (the OS removes a hook
 * that stalls).
 *
 * Every event carries the modifier keys held (@c webview_modifier_t bits),
 * read inside the hook for that event.
 *
 * Remarks:
 * - Windows: a low-level mouse hook (WH_MOUSE_LL) on the calling thread,
 *   which must pump messages; webview_run does.
 * - macOS: a CGEventTap at the session level on the main run loop, shared
 *   with the keyboard hook. The process needs Device Control and Data
 *   Access (System Settings > Privacy & Security; see
 *   @c webview_feature_permissions); without it this fails with
 *   @c WEBVIEW_ERROR_MISSING_DEPENDENCY, after asking the system for it
 *   once unless @c webview_permission_set_auto_request turned that off, and
 *   the caller falls back to whatever else it has (for example hit
 *   regions). A tap the system disables for being slow is re-enabled by the
 *   library.
 * - Linux: not supported; fails with @c WEBVIEW_ERROR_UNSPECIFIED.
 *
 * @param fn The callback.
 * @param arg Passed to @p fn as its last argument.
 * @retval WEBVIEW_ERROR_INVALID_ARGUMENT @p fn is NULL.
 * @retval WEBVIEW_ERROR_DUPLICATE A mouse hook is already installed.
 * @retval WEBVIEW_ERROR_MISSING_DEPENDENCY macOS: the event tap could not be
 *         created, which is reported as the permission missing.
 * @retval WEBVIEW_ERROR_UNSPECIFIED Windows: SetWindowsHookEx failed; Linux:
 *         not supported.
 */
WEBVIEW_API webview_error_t webview_mouse_hook_install(webview_mouse_hook_fn_t fn,
                                                       void *arg);

/**
 * Removes the mouse hook installed by @c webview_mouse_hook_install. The
 * keyboard hook, if installed, keeps running.
 *
 * @retval WEBVIEW_ERROR_NOT_FOUND No mouse hook is installed.
 */
WEBVIEW_API webview_error_t webview_mouse_hook_remove(void);

/**
 * Installs a system-wide keyboard hook: @p fn sees every key event on the
 * system (down, up, and the OS's auto-repeat) before the focused window
 * does, and decides per event whether it goes on or is swallowed. A
 * swallowed key reaches no window at all, so a callback takes only the keys
 * it owns, by its own state (for example while something of its own is
 * selected), and passes the rest. One hook per process; it belongs to no
 * webview instance, and is independent of the mouse hook: install both in
 * either order, remove either, and the other keeps running.
 *
 * Each event names the physical key as the DOM's KeyboardEvent.code does
 * ("KeyZ", "Delete", "ArrowLeft"...), gives the platform's own code for it,
 * and carries the modifier keys held (@c webview_modifier_t bits).
 *
 * Call it on the UI thread before @c webview_run: @p fn runs on that thread
 * from inside the run loop, and must return quickly (the OS removes a hook
 * that stalls).
 *
 * Remarks:
 * - Windows: a low-level keyboard hook (WH_KEYBOARD_LL) on the calling
 *   thread, which must pump messages; webview_run does.
 * - macOS: key-down, key-up and flags-changed events on the CGEventTap the
 *   mouse hook uses (one tap, one permission, one run-loop source; what
 *   the mouse hook says about the permission applies). A modifier key's press and release arrive as flags-changed and
 *   are reported as its down and up. Caps Lock reports a down when it turns
 *   the lock on and an up when it turns it off, which is how the key's
 *   events arrive.
 * - Linux: not supported; fails with @c WEBVIEW_ERROR_UNSPECIFIED.
 *
 * @param fn The callback.
 * @param arg Passed to @p fn as its last argument.
 * @retval WEBVIEW_ERROR_INVALID_ARGUMENT @p fn is NULL.
 * @retval WEBVIEW_ERROR_DUPLICATE A keyboard hook is already installed.
 * @retval WEBVIEW_ERROR_MISSING_DEPENDENCY macOS: the event tap could not be
 *         created, which is reported as the permission missing.
 * @retval WEBVIEW_ERROR_UNSPECIFIED Windows: SetWindowsHookEx failed; Linux:
 *         not supported.
 */
WEBVIEW_API webview_error_t
webview_keyboard_hook_install(webview_keyboard_hook_fn_t fn, void *arg);

/**
 * Removes the keyboard hook installed by @c webview_keyboard_hook_install.
 * The mouse hook, if installed, keeps running.
 *
 * @retval WEBVIEW_ERROR_NOT_FOUND No keyboard hook is installed.
 */
WEBVIEW_API webview_error_t webview_keyboard_hook_remove(void);

/**
 * The system permissions a feature needs on this platform, as
 * @c webview_permission_t bits; 0 when it needs none. On macOS both hooks
 * need @c WEBVIEW_PERMISSION_DEVICE_CONTROL; on Windows and Linux nothing
 * needs anything. Ask this first, then @c webview_permission_status for
 * each bit, and the app knows what it needs before it installs.
 *
 * @param feature A @c webview_feature_t.
 */
WEBVIEW_API unsigned webview_feature_permissions(int feature);

/**
 * Whether the process holds a permission right now. Nothing is shown to the
 * user.
 *
 * @param permission One @c webview_permission_t.
 * @retval WEBVIEW_ERROR_OK Granted.
 * @retval WEBVIEW_ERROR_MISSING_DEPENDENCY Not granted (macOS).
 * @retval WEBVIEW_ERROR_UNSPECIFIED This platform has no such permission.
 * @retval WEBVIEW_ERROR_INVALID_ARGUMENT Not a @c webview_permission_t.
 */
WEBVIEW_API webview_error_t webview_permission_status(unsigned permission);

/**
 * Asks the system for a permission. On macOS it adds the app to the row in
 * System Settings > Privacy & Security and shows its dialog each time this
 * is called while the permission is missing; the user grants it there, and
 * an install after that succeeds. On a platform without the permission
 * nothing is shown.
 *
 * @param permission One @c webview_permission_t.
 * @retval WEBVIEW_ERROR_OK Granted (already, or earlier by the user).
 * @retval WEBVIEW_ERROR_MISSING_DEPENDENCY Still missing; the dialog was
 *         shown.
 * @retval WEBVIEW_ERROR_UNSPECIFIED This platform has no such permission.
 * @retval WEBVIEW_ERROR_INVALID_ARGUMENT Not a @c webview_permission_t.
 */
WEBVIEW_API webview_error_t webview_permission_request(unsigned permission);

/**
 * Whether an install that fails for a missing permission asks the system
 * for it by itself (once per process, as @c webview_permission_request
 * would). On by default, so an app that never asks about permissions still
 * gets the system's dialog the first time it installs a hook. Turn it off
 * to ask only through webview_permission_request, for example after explaining why, or only once
 * per user rather than per launch.
 *
 * @param on Non-zero to ask on a failed install, zero not to.
 */
WEBVIEW_API webview_error_t webview_permission_set_auto_request(int on);

/**
 * Navigates webview to the given URL. URL may be a properly encoded data URI.
 *
 * Example:
 * @code{.c}
 * webview_navigate(w, "https://github.com/webview/webview");
 * webview_navigate(w, "data:text/html,%3Ch1%3EHello%3C%2Fh1%3E");
 * webview_navigate(w, "data:text/html;base64,PGgxPkhlbGxvPC9oMT4=");
 * @endcode
 *
 * @param w The webview instance.
 * @param url URL.
 */
WEBVIEW_API webview_error_t webview_navigate(webview_t w, const char *url);

/**
 * Load HTML content into the webview.
 *
 * Example:
 * @code{.c}
 * webview_set_html(w, "<h1>Hello</h1>");
 * @endcode
 *
 * @param w The webview instance.
 * @param html HTML content.
 */
WEBVIEW_API webview_error_t webview_set_html(webview_t w, const char *html);

/**
 * Injects JavaScript code to be executed immediately upon loading a page.
 * The code will be executed before @c window.onload.
 *
 * @param w The webview instance.
 * @param js JS content.
 */
WEBVIEW_API webview_error_t webview_init(webview_t w, const char *js);

/**
 * Evaluates arbitrary JavaScript code.
 *
 * Use bindings if you need to communicate the result of the evaluation.
 *
 * @param w The webview instance.
 * @param js JS content.
 */
WEBVIEW_API webview_error_t webview_eval(webview_t w, const char *js);

/**
 * Binds a function pointer to a new global JavaScript function.
 *
 * Internally, JS glue code is injected to create the JS function by the
 * given name. The callback function is passed a request identifier,
 * a request string and a user-provided argument. The request string is
 * a JSON array of the arguments passed to the JS function.
 *
 * @param w The webview instance.
 * @param name Name of the JS function.
 * @param fn Callback function.
 * @param arg User argument.
 * @retval WEBVIEW_ERROR_DUPLICATE
 *         A binding already exists with the specified name.
 */
WEBVIEW_API webview_error_t webview_bind(webview_t w, const char *name,
                                         void (*fn)(const char *id,
                                                    const char *req, void *arg),
                                         void *arg);

/**
 * Removes a binding created with webview_bind().
 *
 * @param w The webview instance.
 * @param name Name of the binding.
 * @retval WEBVIEW_ERROR_NOT_FOUND No binding exists with the specified name.
 */
WEBVIEW_API webview_error_t webview_unbind(webview_t w, const char *name);

/**
 * Responds to a binding call from the JS side.
 *
 * This function is safe to call from another thread.
 *
 * @param w The webview instance.
 * @param id The identifier of the binding call. Pass along the value received
 *           in the binding handler (see webview_bind()).
 * @param status A status of zero tells the JS side that the binding call was
 *               successful; any other value indicates an error.
 * @param result The result of the binding call to be returned to the JS side.
 *               This must either be a valid JSON value or an empty string for
 *               the primitive JS value @c undefined.
 */
WEBVIEW_API webview_error_t webview_return(webview_t w, const char *id,
                                           int status, const char *result);

/**
 * Get the library's version information.
 *
 * @since 0.10
 */
WEBVIEW_API const webview_version_info_t *webview_version(void);

#ifdef __cplusplus
}
#endif

#endif // WEBVIEW_API_H
