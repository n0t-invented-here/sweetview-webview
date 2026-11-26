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

- Nothing reaches the network and no socket is opened.
- `fn == NULL` removes the handler. Requests already handed to it stay open until completed or until
  the webview is destroyed, which answers them 503.

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

## The handler

Runs on the UI thread, from inside the run loop, and must not block: it hands the request off and
returns.

- `request_id`: never 0; increases with each request.
- `headers_json`: the request headers as a JSON array of `[name, value]` string pairs.
- `body`: the request body, or `NULL`.
- Every pointer is valid only for the duration of the call.

## Completing

`webview_complete_request` answers a request, now or later, from any thread.

- The headers and body are copied before it returns; the work runs on the UI thread.
- `headers_json`: a JSON object (`{"Content-Type": "text/html"}`), or a JSON array of `[name, value]`
  pairs, which lets a name repeat (`[["Set-Cookie","a=1"],["Set-Cookie","b=2"]]`). CR and LF are
  removed from names and values, so no header can be injected. `NULL` or `""` for none.
- `status`: 100-599; the reason phrase is chosen from it.
- Completing a request twice, or one the browser has cancelled, does nothing.
- macOS: WebKit stops a task the page no longer wants (`webView:stopURLSchemeTask:`). The task is
  dropped then, so a later completion never touches it (WebKit throws if a stopped task is
  answered).

## Teardown

Destroying the webview answers every open request (Windows: 503, then the deferral and event args
are released; macOS: the retained tasks are released, and WebKit stops them).

Implementation: [`http_request.hh`](../core/include/webview/detail/http_request.hh) (headers to and
from JSON, reason phrases), the backends in
[`detail/backends/`](../core/include/webview/detail/backends).
