# Assets

```c
webview_error_t webview_set_asset(webview_t w, const char *path, const char *mime,
                                  const void *data, size_t len);
```

Serves bytes at a path of the webview's own origin.

The page is then loaded with `webview_navigate` instead of `webview_set_html` (which Windows caps at
2 MB), and fetches scripts, styles, images and models by relative URL.

- Nothing reaches the network.
- The bytes are copied; calling again with the same path replaces them.

```c
webview_set_asset(w, "index.html", "text/html; charset=utf-8", html, strlen(html));
webview_set_asset(w, "main.js", "text/javascript", js, js_len);
webview_navigate(w, "http://webview.localhost/index.html");
```

The origin is fixed per platform:

| Platform | Origin                     | How                                                                                                              |
| -------- | -------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| Windows  | `http://webview.localhost` | WebView2's `WebResourceRequested` event; requests to any other origin are untouched                              |
| macOS    | `webview://localhost`      | A `WKURLSchemeHandler` for the scheme `webview`, registered when the `WKWebView` is created. Answers with an `NSHTTPURLResponse`, so WebKit parses a `Content-Type` with a charset and loads module scripts |
| Linux    |                            | Accepted and stored; not served ([why](README.md#linux))                                                         |

- `path`: the URL path without the leading slash; must not need percent-encoding. A request's query
  string and fragment are ignored when matching.
- Matching is the same on both platforms: the request URL's raw path (not percent-decoded; a
  trailing slash kept), query string and fragment removed, leading slashes ignored.
- On macOS the authority must be exactly `localhost` (any case): `webview://other-host/index.html`,
  a port or userinfo is answered 404 and never matches an asset.
- `mime`: the value of the `Content-Type` header.

Validation, all before anything is stored (the asset table is left as it was on failure):

- `path` and `mime` must not be `NULL`; `data` may be `NULL` only when `len` is 0.
- `mime` must be a valid HTTP header value: no CR, LF, NUL or other control character but tab, so it
  can never split the response or add a header.
- `len` must be at most 4294967295 (`UINT32_MAX`).
- Anything else returns `WEBVIEW_ERROR_INVALID_ARGUMENT`. The checks run both in the C API (before
  the instance is touched) and in the engine, for C++ callers.
