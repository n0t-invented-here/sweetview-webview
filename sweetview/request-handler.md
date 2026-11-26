# Request handler

```c
typedef void (*webview_request_handler_t)(uint64_t request_id, const char *method,
                                          const char *url, const char *headers_json,
                                          const void *body, size_t body_len, void *arg);

webview_error_t webview_set_request_handler(webview_t w, webview_request_handler_t fn, void *arg);
webview_error_t webview_complete_request(webview_t w, uint64_t request_id, int status,
                                         const char *headers_json, const void *body,
                                         size_t body_len);

webview_error_t webview_begin_response(webview_t w, uint64_t request_id, int status,
                                       const char *headers_json);
webview_error_t webview_write_response_chunk(webview_t w, uint64_t request_id,
                                             const void *chunk, size_t chunk_len);
webview_error_t webview_end_response(webview_t w, uint64_t request_id);
```

Answers requests to the webview's origins in code instead of from fixed bytes.

- Nothing reaches the network and no socket is opened. On Windows every request the filters match
  gets a response: when one cannot be built, a bare `500 Internal Server Error` goes out instead.
- `fn == NULL` removes the handler. Requests already handed to it stay open until completed or until
  the webview is destroyed (see [Teardown](#teardown)).

## Origins

Two origins go to the handler:

| Platform | Main origin ([assets](assets.md) first) | Frames origin (every request)     | How                                                                             |
| -------- | --------------------------------------- | --------------------------------- | ------------------------------------------------------------------------------- |
| Windows  | `http://webview.localhost`              | `http://frames.webview.localhost` | WebView2's `WebResourceRequested`, held open with `GetDeferral` until completed |
| macOS    | `webview://localhost`                   | `webview-frames://localhost`      | One `WKURLSchemeHandler` for the schemes `webview` and `webview-frames`         |
| Linux    |                                         |                                   | Accepted and stored; not served ([why](README.md#linux))                        |

- On the main origin, an asset set with `webview_set_asset` answers first, at no extra cost; only
  paths with no asset reach the handler.
- The frames origin is for content that must be cross-origin from the page, such as a sandboxed
  `<iframe>`: the browser keeps the page out of its document, and the two talk through `postMessage`.
- Without a handler, both answer 404 for anything with no asset.
- Only those exact origins reach the handler. On macOS a request to the `webview` or
  `webview-frames` scheme with any other authority (`webview://other-host/...`, a port, userinfo)
  is answered 404 before the asset table or the handler sees it; on Windows WebView2 routes only
  the two origins here.
- Windows: where the WebView2 Runtime has `ICoreWebView2_22` (Runtime 122.0.2365.46 or later; the
  interface shipped in SDK 1.0.2365.46, and this build is configured with SDK 1.0.2478.35), the filters
  are added with `AddWebResourceRequestedFilterWithRequestSourceKinds` and all source kinds, so they
  catch requests from service workers, shared workers and cross-origin iframes as well as documents.
  An older Runtime falls back to the legacy `AddWebResourceRequestedFilter`, which misses requests from
  service and shared workers and from cross-origin iframes.

## The handler

Runs on the UI thread, from inside the run loop, and must not block: it hands the request off and
returns.

- `request_id`: never 0; increases with each request.
- `headers_json`: the request headers as a JSON array of `[name, value]` string pairs.
- `body`: the request body, or `NULL`.
- Every pointer is valid only for the duration of the call.
- A request whose body stream fails partway never reaches the handler with a partial body: Windows
  answers it 500, macOS fails the task with the stream's `NSError`.
- A handler that throws (C++) never lets the exception reach the browser: a request it neither
  answered with `webview_complete_request` nor began with `webview_begin_response` is answered with a
  bare 500.

## Completing

`webview_complete_request` answers a request, now or later, from any thread.

- The headers are checked, and the headers and body copied, before it returns; the work runs on the
  UI thread.
- `headers_json`: a JSON object (`{"Content-Type": "text/html"}`), or a JSON array of `[name, value]`
  pairs, which lets a name repeat (`[["X-Multi","a"],["X-Multi","b"]]`). Values may be strings,
  numbers or booleans; `null` drops the header. `NULL` or `""` for none.
- `headers_json` is parsed as strict JSON and refused with `WEBVIEW_ERROR_INVALID_ARGUMENT`, before
  anything is queued, when it has trailing text (`{"X-A":trueish}`), raw control characters or lone
  surrogates in a string, invalid UTF-8, or U+0000 (raw or `\u0000`); when a name is not an RFC 9110
  token (empty, spaces, `:` and so on); or when a value holds CR, LF, NUL or another control
  character but tab. No header can be injected.
- `status`: 100-599; the reason phrase is chosen from it.
- `body_len`: at most 4294967295 (`UINT32_MAX`); larger returns `WEBVIEW_ERROR_INVALID_ARGUMENT`.
- Completing a request twice, or one the browser has cancelled, does nothing.
- macOS: WebKit stops a task the page no longer wants (`webView:stopURLSchemeTask:`). The task is
  dropped then, so a later completion never touches it (WebKit throws if a stopped task is
  answered).

### Repeated header names

| Platform | What goes out                                                                                                                                                                                                                         |
| -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Windows  | Each `[name, value]` pair as its own header line; several `Set-Cookie` headers work.                                                                                                                                                 |
| macOS    | `NSHTTPURLResponse` holds one value per name. Names match without regard to case and keep the casing first seen; repeats are joined with `", "`. `Set-Cookie` cannot be joined (RFC 9110 5.3), so only the first is kept: **at most one `Set-Cookie` per response on macOS**. |

## Streaming

`webview_begin_response` starts an answer with its status and headers; the body then follows in
pieces through `webview_write_response_chunk` and ends with `webview_end_response`. Use it instead of
`webview_complete_request`, never both for one request.

- All three may be called from any thread. `begin` checks `status` and `headers_json` exactly like
  `webview_complete_request`; `write` copies the chunk before it returns.
- `chunk_len`: at most 4294967295 (`UINT32_MAX`); `NULL` with a length is refused. A zero-length
  chunk does nothing once the handle and arguments pass (after `webview_destroy` has begun it still
  returns `WEBVIEW_ERROR_INVALID_STATE`).
- Beginning a request already answered or cancelled does nothing, and its chunks are dropped.
  Beginning one whose stream is already open returns `WEBVIEW_ERROR_DUPLICATE` on Windows; macOS
  ignores the second begin.
- Ending a response twice, or one never begun, does nothing; a request never begun can still be
  answered with `webview_complete_request`.

| Platform | How                                                                                                                                                                                                                                                                                                                                                  |
| -------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Windows  | The response's content is an `IStream` whose `Read()` blocks on WebView2's background thread until a chunk or the end arrives; the UI thread never waits and only unread bytes are held. WebView2 reads the whole stream before it hands the response to the page (WebView2Feedback #3519), so the page receives the body at `webview_end_response`, not chunk by chunk |
| macOS    | `WKURLSchemeTask` `didReceiveResponse:`, then `didReceiveData:` per chunk and `didFinish`, each on the main queue in the order called, so Server-Sent Events and large media reach the page as they are written. A task WebKit stops is forgotten; later chunks and the end do nothing                                                                     |
| Linux    | Accepted; does nothing ([why](README.md#linux))                                                                                                                                                                                                                                                                                                      |

## Teardown

Requests still open when the webview is destroyed are closed and released without an answer, on
both platforms; none of them is ever passed on to the network:

- Windows: every open stream is aborted first (a `Read()` blocked on WebView2's thread fails and
  returns), then the WebView2 controller is closed, then each pending deferral and its event args are
  released without `Complete()`.
- macOS: the retained tasks, streamed or not, are released without an answer; WebKit stops them as
  the `WKWebView` goes.
- On both, answers and stream steps still queued for the UI thread do nothing when they run.

Completing against teardown:

- `webview_complete_request` and the streaming calls may race `webview_destroy` from any thread. The library keeps a
  registry of live webviews: `webview_destroy` first marks the webview as going and removes it, under
  a lock, before deleting it, and `webview_complete_request` validates, copies and queues under the
  same lock. After `webview_destroy` has begun it returns `WEBVIEW_ERROR_INVALID_STATE`.
- An answer queued before then carries the webview's alive token and does nothing if it runs after
  the webview is gone (on macOS it can still be on the main queue after the delete).
- A worker thread that answers requests should still be told to stop before `webview_destroy` and
  its in-flight `webview_complete_request` calls allowed to return; calls after that are refused
  cleanly rather than crashing.

Implementation: [`http_request.hh`](../core/include/webview/detail/http_request.hh) (headers to and
from JSON, reason phrases), the backends in
[`detail/backends/`](../core/include/webview/detail/backends).
