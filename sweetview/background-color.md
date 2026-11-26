# Background color

```c
webview_error_t webview_set_background_color(webview_t w, int r, int g, int b, int a);
```

Sets the color the browser paints behind the page. With `a` of 0 and a page whose own background is
transparent, the native window shows through, which is what an overlay or a shaped window wants.

| Platform | Behavior                                                                                                                                                                                                                                                                                                                               |
| -------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Windows  | Sets `ICoreWebView2Controller2::DefaultBackgroundColor`. WebView2 takes only `a` of 0 (fully transparent) or 255 (opaque); any other alpha is refused by WebView2 and returns `WEBVIEW_ERROR_UNSPECIFIED`, as does a Runtime without `ICoreWebView2Controller2`. For true transparency the host window also needs DWM composition that honors alpha, for example `DwmEnableBlurBehindWindow` with an empty region |
| macOS    | Sets the `WKWebView`'s `drawsBackground` (KVC) to `NO` when `a` is 0 and `YES` otherwise; sets this color as the window's background and makes the window non-opaque unless `a` is 255. `webview_set_size` leaves an adopted window alone                                                                                             |
| Linux    | Accepted; does nothing ([why](README.md#linux))                                                                                                                                                                                                                                                                                       |

- Channels are 0 to 255; `a` of 0 is fully transparent. A channel outside 0-255 returns
  `WEBVIEW_ERROR_INVALID_ARGUMENT` and changes nothing.
- `WEBVIEW_ERROR_INVALID_STATE` when the browser is not created yet or already gone.
