# Background color

```c
webview_error_t webview_set_background_color(webview_t w, int r, int g, int b, int a);
```

Sets the color the browser paints behind the page. With `a` of 0 and a page whose own background is
transparent, the native window shows through, which is what an overlay or a shaped window wants.

| Platform | Behavior                                                                                                                                                                                                                                                                                                                               |
| -------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Windows  | Sets `ICoreWebView2Controller2::DefaultBackgroundColor`. WebView2 takes only `a` of 0 (fully transparent) or 255 (opaque); any other alpha is refused by WebView2 and returns `WEBVIEW_ERROR_UNSPECIFIED`, as does a Runtime without `ICoreWebView2Controller2`. For true transparency the host window also needs DWM composition that honors alpha, for example `DwmEnableBlurBehindWindow` with an empty region |
| macOS    | Any alpha 0-255. Turns the `WKWebView`'s own background off (`drawsBackground`, KVC, `NO`) and paints the color, in sRGB, on the `WKWebView`'s backing layer (`wantsLayer`, `layer.backgroundColor`) behind the page. With `a` below 255 the window is made non-opaque with a clear background, owned or adopted alike, so the translucent layer shows what is behind it. With `a` of 255 only a window the library created is made opaque again (with this color as its background); an adopted window's opacity and background stay as the host set them. `webview_set_size` also leaves an adopted window alone |
| Linux    | Accepted; does nothing ([why](README.md#linux))                                                                                                                                                                                                                                                                                       |

- Channels are 0 to 255; `a` of 0 is fully transparent. A channel outside 0-255 returns
  `WEBVIEW_ERROR_INVALID_ARGUMENT` and changes nothing.
- `WEBVIEW_ERROR_INVALID_STATE` when the browser is not created yet or already gone.
