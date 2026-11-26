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
