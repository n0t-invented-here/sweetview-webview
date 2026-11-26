# External drop

```c
webview_error_t webview_set_allow_external_drop(webview_t w, int allow);
```

Sets whether files and other content dragged from outside the app can be dropped on the page.
Allowed by default. Drags that start inside the page (in-page drag and drop) keep working either way.

| Platform | How                                                                                                                                                                                                                       |
| -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Windows  | `ICoreWebView2Controller4::AllowExternalDrop`: external drops allowed or refused for the whole WebView2 control                                                                                                           |
| macOS    | Enforced in the engine's `WKWebView` subclass, `SweetviewWKWebView`: while refused, a drag whose `NSDraggingInfo.draggingSource` is `nil` (it comes from another app, such as Finder) gets `NSDragOperationNone` and never reaches WebKit; drags from inside the app still go to WebKit |
| Linux    | Accepted; does nothing ([why](README.md#linux))                                                                                                                                                                           |

- Windows: needs WebView2 SDK 1.0.1185.39 or later at build time (the build fetches 1.0.2478.35); a
  WebView2 Runtime without `ICoreWebView2Controller4` returns `WEBVIEW_ERROR_UNSPECIFIED`. Set before
  the controller exists, the value is applied when it is created.
- macOS: each drag session (`draggingSequenceNumber`) is routed once, when it enters the view: to
  WebKit, to the file drop handler, or refused. Every later message of that session follows the same
  route, so WebKit only sees `draggingExited:` for sessions it saw enter. Refusing external drops
  mid-drag ends the session it affects: a session WebKit was following gets `draggingExited:` and is
  refused from then on.
- The file drop handler below receives drops only while external drops are allowed, on both
  platforms.

## File drop paths

```c
typedef void (*webview_file_drop_handler_t)(const char *paths_json, void *arg);

webview_error_t webview_set_file_drop_handler(webview_t w, webview_file_drop_handler_t fn, void *arg);
```

When the platform captures a file drop while a handler is set (see the table below for where and
when it does), the files dropped from the operating system go to `fn` as a JSON array of real
filesystem paths instead of to the page, and the browser does not navigate to the dropped file. Drags
inside the page, and drags that carry no files, are not affected. `fn == NULL` removes the handler and
gives drops back to the page. Call from the UI thread.

On both Windows and macOS:

- External drops must be allowed (`webview_set_allow_external_drop`).
- `fn` runs asynchronously, on the UI thread's message loop after the operating system's drop event
  has returned, so it may set or clear the handler, open dialogs, or destroy the webview.
- `fn` is never called with an empty array.
- Setting the handler is transactional: when the backend refuses the change (on Windows, the script
  that switches the page's drop capture fails), the previous handler and argument stay in place and
  the error is returned.

| Platform | How                                                                                                                                                                                                                                                                                                                                                                                           |
| -------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Windows  | Drops are captured only on the top-level document, never inside an `<iframe>` (cross-origin or not). The top-level document's drop event hands the dropped `File` objects to the host with `chrome.webview.postMessageWithAdditionalObjects`, and WebView2's `ICoreWebView2File` attachment gives their paths; a drop on an iframe stays with the iframe. Needs WebView2 Runtime 113 or later: on an older Runtime, which lacks `postMessageWithAdditionalObjects`, the drop script neither accepts the drag nor prevents the drop, so the drop is left to the page and the browser's default instead of being swallowed, and the handler is not called |
| macOS    | `SweetviewWKWebView` takes a drag session when a handler is set, its pasteboard holds file URLs (`public.file-url`) and its source allows copying (`draggingSourceOperationMask` includes `NSDragOperationCopy`); any other drag goes to WebKit. The paths are read from the pasteboard during `performDragOperation:`. Clearing the handler, or refusing external drops, mid-drag refuses the drop |
| Linux    | Accepted; does nothing ([why](README.md#linux))                                                                                                                                                                                                                                                                                                                                               |

Tests: this repository unit-tests the argument checks and the transactional handler swap. The
Windows drop script is exercised end to end by a synthetic drop in sweetview-webview-bun's live tests
(the page dispatches a `drop` event carrying a `File` it builds, which has no path, so the handler
itself is not called there); a real Explorer or Finder drag is not automated and is checked by hand.
