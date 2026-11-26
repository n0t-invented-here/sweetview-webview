# External drop

```c
webview_error_t webview_set_allow_external_drop(webview_t w, int allow);
```

Sets whether files and other content dragged from outside the app can be dropped on the page.
Allowed by default.

| Platform | How                                           |
| -------- | --------------------------------------------- |
| Windows  | `ICoreWebView2Controller4::AllowExternalDrop` |
| macOS    | Accepted; does nothing                        |
| Linux    | Accepted; does nothing                        |

- Windows: needs WebView2 SDK 1.0.1185.39 or later at build time (the build fetches it); a WebView2
  Runtime without `ICoreWebView2Controller4` returns `WEBVIEW_ERROR_UNSPECIFIED`.

## File drop paths

```c
webview_error_t webview_set_file_drop_handler(webview_t w, webview_file_drop_handler_t fn, void *arg);
```

While a handler is set, files dropped from the operating system go to `fn` as a JSON array of real
filesystem paths instead of to the page.

- Windows: drops are captured only on the trusted top-level document, never inside an `<iframe>`
  (cross-origin or not). The top-level document's drop event hands the dropped `File` objects to the
  host with `chrome.webview.postMessageWithAdditionalObjects`, and WebView2's `ICoreWebView2File`
  attachment gives their paths; a drop on an iframe stays with the iframe. `fn` runs from the message
  loop after WebView2's event has returned, so it may set or clear the handler, open dialogs, or
  destroy the webview. Needs external drop allowed and WebView2 Runtime 113 or later.
- macOS: the `WKWebView` takes drags whose pasteboard holds file URLs.
- Linux: accepted; does nothing.
