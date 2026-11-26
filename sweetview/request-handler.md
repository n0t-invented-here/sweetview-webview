# Request handler

```c
typedef void (*webview_request_handler_t)(uint64_t request_id, const char *method,
                                          const char *url, const char *headers_json,
                                          const void *body, size_t body_len, void *arg);

webview_error_t webview_set_request_handler(webview_t w, webview_request_handler_t fn, void *arg);
webview_error_t webview_complete_request(webview_t w, uint64_t request_id, int status,
                                         const char *headers_json, const void *body,
                                         size_t body_len);
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
| Linux    |                                         |                                   | Accepted and stored; not served                                                 |

- On the main origin, an asset set with `webview_set_asset` answers first, at no extra cost; only
  paths with no asset reach the handler.
- The frames origin is for content that must be cross-origin from the page, such as a sandboxed
  `<iframe>`: the browser keeps the page out of its document, and the two talk through `postMessage`.
- Without a handler, both answer 404 for anything with no asset.
- Windows: the filters catch requests from service workers and shared workers as well as documents
  (`ICoreWebView2_22::AddWebResourceRequestedFilterWithRequestSourceKinds`, all source kinds) when the
  WebView2 Runtime has it (Runtime 125 or later; the build needs WebView2 SDK 1.0.2478.35 or later).
  An older Runtime falls back to `AddWebResourceRequestedFilter`, which sees only document requests.

## The handler

Runs on the UI thread, from inside the run loop, and must not block: it hands the request off and
returns.

- `request_id`: never 0; increases with each request.
- `headers_json`: the request headers as a JSON array of `[name, value]` string pairs.
- `body`: the request body, or `NULL`.
- Every pointer is valid only for the duration of the call.
- A request whose body stream fails partway never reaches the handler with a partial body: Windows
  answers it 500, macOS fails the task with the stream's `NSError`.

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

## Teardown

What happens to requests still open when the webview is destroyed:

- Windows: each is answered `503 Service Unavailable`, then its deferral is completed and the
  deferral and event args are released.
- macOS: the retained tasks are released without an answer; WebKit stops them as the `WKWebView`
  goes.

Completing against teardown:

- `webview_complete_request` may race `webview_destroy` from any thread. The library keeps a
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
