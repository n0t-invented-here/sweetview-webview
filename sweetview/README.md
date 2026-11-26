# sweetview-webview

A fork of [webview](https://github.com/webview/webview) for the sweetview projects.

What the fork adds is documented here, one page per addition.

| Addition                                                                                                       | C API                                                                                                                           |
| -------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| [Background color](background-color.md): a color, or transparency, behind the page                             | `webview_set_background_color`                                                                                                  |
| [Assets](assets.md): serve bytes at the webview's own origin                                                   | `webview_set_asset`                                                                                                             |
| [Request handler](request-handler.md): answer requests in code; a cross-origin frames origin                   | `webview_set_request_handler`, `webview_complete_request`                                                                       |
| [Streaming responses](request-handler.md#streaming): status and headers now, the body in chunks                | `webview_begin_response`, `webview_write_response_chunk`, `webview_end_response`                                                |
| [External drop](external-drop.md): refuse drops from outside the app                                           | `webview_set_allow_external_drop`                                                                                               |
| [File drop paths](external-drop.md#file-drop-paths): real filesystem paths of files dropped from the OS        | `webview_set_file_drop_handler`                                                                                                 |
| [Mouse hook](mouse-hook.md): every mouse event on the system, swallow any                                      | `webview_mouse_hook_install`, `webview_mouse_hook_remove`                                                                       |
| [Keyboard hook](keyboard-hook.md): every key event on the system, swallow any                                  | `webview_keyboard_hook_install`, `webview_keyboard_hook_remove`                                                                 |
| [Permissions](permissions.md): what the hooks need from the OS, and who asks                                   | `webview_feature_permissions`, `webview_permission_status`, `webview_permission_request`, `webview_permission_set_auto_request` |

## Linux

The fork keeps the upstream webview/webview C ABI, so every function above exists on every platform
and a program built against it links everywhere. The additions are implemented for Windows (WebView2)
and macOS (WKWebView) only. On Linux (GTK/WebKitGTK) they are accepted and do nothing, or fail with
`WEBVIEW_ERROR_UNSPECIFIED` where a silent no-op would mislead (the input hooks, the permissions);
each page says which.

## Where things are

- Declarations and their full documentation: [`api.h`](../core/include/webview/api.h),
  [`types.h`](../core/include/webview/types.h).
- New implementation files, next to upstream's in [`detail/`](../core/include/webview/detail):
  [`http_request.hh`](../core/include/webview/detail/http_request.hh),
  [`input_hooks.hh`](../core/include/webview/detail/input_hooks.hh),
  [`mouse_hook.hh`](../core/include/webview/detail/mouse_hook.hh),
  [`keyboard_hook.hh`](../core/include/webview/detail/keyboard_hook.hh),
  [`key_codes.hh`](../core/include/webview/detail/key_codes.hh),
  [`streaming_istream.hh`](../core/include/webview/detail/platform/windows/streaming_istream.hh).
- Upstream files the fork changes:
  - [`api.h`](../core/include/webview/api.h), [`types.h`](../core/include/webview/types.h) and
    [`c_api_impl.hh`](../core/include/webview/c_api_impl.hh): the new C API.
  - [`engine_base.hh`](../core/include/webview/detail/engine_base.hh): assets, the request handler,
    streaming and file drop state shared by the backends.
  - The backends [`win32_edge.hh`](../core/include/webview/detail/backends/win32_edge.hh) and
    [`cocoa_webkit.hh`](../core/include/webview/detail/backends/cocoa_webkit.hh), plus the platform
    wrappers they use (`platform/darwin/webkit/WKScriptMessage.hh`,
    `platform/darwin/webkit/WKUserContentController.hh`, `platform/windows/webview2/loader.hh`).
  - [`unit_tests.cc`](../core/tests/src/unit_tests.cc): tests for the additions.
  - Build: [`cmake/webview.cmake`](../cmake/webview.cmake) (the WebView2 SDK version, 1.0.2478.35,
    and macOS's ApplicationServices framework for the input hooks) and `.github/workflows/check.yaml`.
  - The root [README](../README.md): a pointer to this folder at the top.
- Bindings: [sweetview-webview-bun](https://github.com/n0t-invented-here/sweetview-webview-bun) builds
  and ships this library and wraps all of these additions in TypeScript.
