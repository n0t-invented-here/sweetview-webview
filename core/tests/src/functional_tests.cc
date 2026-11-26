#include "webview/test_driver.hh"

#define WEBVIEW_VERSION_MAJOR 1
#define WEBVIEW_VERSION_MINOR 2
#define WEBVIEW_VERSION_PATCH 3
#define WEBVIEW_VERSION_PRE_RELEASE "-test"
#define WEBVIEW_VERSION_BUILD_METADATA "+gaabbccd"

#include "webview/webview.h"

#include <cassert>
#include <cstdint>

// This test should only run on Windows to enable us to perform a controlled
// "warm-up" of MS WebView2 in order to avoid the initial test from
// occationally timing out in CI.
#ifdef WEBVIEW_PLATFORM_WINDOWS
#include <iostream>

TEST_CASE("# Warm-up") {
  // Signal to the test runner that this may be a slow test.
  std::cerr << "[[slow]]" << std::endl; // NOLINT(performance-avoid-endl)
  webview::webview w(false, nullptr);
  w.dispatch([&]() { w.terminate(); });
  w.run();
}
#endif

TEST_CASE("Start app loop and terminate it") {
  webview::webview w(false, nullptr);
  w.dispatch([&]() { w.terminate(); });
  w.run();
}

void cb_assert_arg(webview_t w, void *arg) {
  REQUIRE(w != nullptr);
  REQUIRE(memcmp(arg, "arg", 3) == 0);
}

void cb_terminate(webview_t w, void *arg) {
  REQUIRE(arg == nullptr);
  webview_terminate(w);
}

TEST_CASE("Use C API to create a window, run app and terminate it") {
  webview_t w;
  w = webview_create(false, nullptr);
  webview_set_size(w, 480, 320, WEBVIEW_HINT_NONE);
  webview_set_title(w, "Test");
  webview_set_html(w, "set_html ok");
  webview_navigate(w, "data:text/plain,navigate%20ok");
  webview_dispatch(w, cb_assert_arg, (void *)"arg");
  webview_dispatch(w, cb_terminate, nullptr);
  webview_run(w);
  webview_destroy(w);
}

TEST_CASE("Use C API to test binding and unbinding") {
  struct context_t {
    webview_t w;
    unsigned int number;
  } context{};
  auto test = +[](const char *seq, const char *req, void *arg) {
    auto increment = +[](const char *seq, const char * /*req*/, void *arg) {
      auto *context = static_cast<context_t *>(arg);
      ++context->number;
      webview_return(context->w, seq, 0, "");
    };
    auto context = static_cast<context_t *>(arg);
    std::string req_(req);
    // Bind and increment number.
    if (req_ == "[0]") {
      REQUIRE(context->number == 0);
      webview_bind(context->w, "increment", increment, context);
      webview_eval(context->w,
                   "try{window.increment().then(r => window.test(1))"
                   ".catch(() => window.test(1,1))}"
                   "catch{window.test(1,1)}");
      webview_return(context->w, seq, 0, "");
      return;
    }
    // Unbind and make sure that we cannot increment even if we try.
    if (req_ == "[1]") {
      REQUIRE(context->number == 1);
      webview_unbind(context->w, "increment");
      webview_eval(context->w,
                   "try{window.increment().then(r => window.test(2))"
                   ".catch(() => window.test(2,1))}"
                   "catch{window.test(2,1)}");
      webview_return(context->w, seq, 0, "");
      return;
    }
    // Number should not have changed but we can bind again and change the number.
    if (req_ == "[2,1]") {
      REQUIRE(context->number == 1);
      webview_bind(context->w, "increment", increment, context);
      webview_eval(context->w,
                   "try{window.increment().then(r => window.test(3))"
                   ".catch(() => window.test(3,1))}"
                   "catch{window.test(3,1)}");
      webview_return(context->w, seq, 0, "");
      return;
    }
    // Finish test.
    if (req_ == "[3]") {
      REQUIRE(context->number == 2);
      webview_terminate(context->w);
      return;
    }
    REQUIRE(!"Should not reach here");
  };
  auto html = "<script>\n"
              "  window.test(0);\n"
              "</script>";
  auto w = webview_create(1, nullptr);
  context.w = w;
  // Attempting to remove non-existing binding is OK
  webview_unbind(w, "test");
  webview_bind(w, "test", test, &context);
  // Attempting to bind multiple times only binds once
  webview_bind(w, "test", test, &context);
  webview_set_html(w, html);
  webview_run(w);
}

TEST_CASE("Test synchronous binding and unbinding") {
  auto make_call_js = [](unsigned int result) {
    std::string js;
    js += "try{window.increment().then(r => window.test(";
    js += std::to_string(result);
    js += "))";
    js += ".catch(() => window.test(";
    js += std::to_string(result);
    js += ",1))}catch{window.test(";
    js += std::to_string(result);
    js += ",1)}";
    return js;
  };
  unsigned int number = 0;
  webview::webview w(false, nullptr);
  auto test = [&](const std::string &req) -> std::string {
    auto increment = [&](const std::string & /*req*/) -> std::string {
      ++number;
      return "";
    };
    // Bind and increment number.
    if (req == "[0]") {
      REQUIRE(number == 0);
      w.bind("increment", increment);
      w.eval(make_call_js(1));
      return "";
    }
    // Unbind and make sure that we cannot increment even if we try.
    if (req == "[1]") {
      REQUIRE(number == 1);
      w.unbind("increment");
      w.eval(make_call_js(2));
      return "";
    }
    // We should have gotten an error on the JS side.
    // Number should not have changed but we can bind again and change the number.
    if (req == "[2,1]") {
      REQUIRE(number == 1);
      w.bind("increment", increment);
      w.eval(make_call_js(3));
      return "";
    }
    // Finish test.
    if (req == "[3]") {
      REQUIRE(number == 2);
      w.terminate();
      return "";
    }
    REQUIRE(!"Should not reach here");
    return "";
  };
  auto html = "<script>\n"
              "  window.test(0);\n"
              "</script>";
  // Attempting to remove non-existing binding is OK
  w.unbind("test");
  w.bind("test", test);
  // Attempting to bind multiple times only binds once
  w.bind("test", test);
  w.set_html(html);
  w.run();
}

TEST_CASE("The string returned from a binding call must be JSON") {
  constexpr auto html =
      R"html(<script>
  try {
    window.loadData()
      .then(() => window.endTest(0))
      .catch(() => window.endTest(1));
  } catch {
    window.endTest(2);
  }
</script>)html";

  webview::webview w(true, nullptr);

  w.bind("loadData", [](const std::string & /*req*/) -> std::string {
    return "\"hello\"";
  });

  w.bind("endTest", [&](const std::string &req) -> std::string {
    REQUIRE(req != "[2]");
    REQUIRE(req != "[1]");
    REQUIRE(req == "[0]");
    w.terminate();
    return "";
  });

  w.set_html(html);
  w.run();
}

TEST_CASE("The string returned of a binding call must not be JS") {
  constexpr const auto html =
      R"html(<script>
  try {
    window.loadData()
      .then(() => window.endTest(0))
      .catch(() => window.endTest(1));
  } catch {
    window.endTest(2);
  }
</script>)html";

  webview::webview w(true, nullptr);

  w.bind("loadData", [](const std::string & /*req*/) -> std::string {
    // Try to load malicious JS code
    return "(()=>{document.body.innerHTML='gotcha';return 'hello';})()";
  });

  w.bind("endTest", [&](const std::string &req) -> std::string {
    REQUIRE(req != "[0]");
    REQUIRE(req != "[2]");
    REQUIRE(req == "[1]");
    w.terminate();
    return "";
  });

  w.set_html(html);
  w.run();
}

TEST_CASE("webview_version()") {
  auto vi = webview_version();
  REQUIRE(vi);
  REQUIRE(vi->version.major == 1);
  REQUIRE(vi->version.minor == 2);
  REQUIRE(vi->version.patch == 3);
  REQUIRE(std::string(vi->version_number) == "1.2.3");
  REQUIRE(std::string(vi->pre_release) == "-test");
  REQUIRE(std::string(vi->build_metadata) == "+gaabbccd");
  // The function should return the same pointer when called again.
  REQUIRE(webview_version() == vi);
}

struct test_webview : webview::browser_engine {
  using cb_t = std::function<void(test_webview *, int, const std::string &)>;
  test_webview(cb_t cb) : webview::browser_engine(true, nullptr), m_cb(cb) {}
  void on_message(const std::string &msg) override { m_cb(this, i++, msg); }
  int i = 0;
  cb_t m_cb;
};

TEST_CASE("Ensure that JS code can call native code and vice versa") {
  test_webview browser([](test_webview *w, int i, const std::string &msg) {
    switch (i) {
    case 0:
      REQUIRE(msg == "loaded");
      w->eval("window.__webview__.post('exiting ' + window.x)");
      break;
    case 1:
      REQUIRE(msg == "exiting 42");
      w->terminate();
      break;
    default:
      REQUIRE(0);
    }
  });
  browser.init(R"(
    window.x = 42;
    window.onload = () => {
      window.__webview__.post('loaded');
    };
  )");
  browser.navigate("data:text/html,%3Chtml%3Ehello%3C%2Fhtml%3E");
  browser.run();
}

#define ASSERT_WEBVIEW_FAILED(expr) REQUIRE(WEBVIEW_FAILED(expr))

TEST_CASE("Bad C API usage without crash") {
  webview_t w{};
  REQUIRE(webview_get_window(w) == nullptr);
  REQUIRE(webview_get_native_handle(w, WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW) ==
          nullptr);
  ASSERT_WEBVIEW_FAILED(webview_set_size(w, 0, 0, WEBVIEW_HINT_NONE));
  ASSERT_WEBVIEW_FAILED(webview_navigate(w, nullptr));
  ASSERT_WEBVIEW_FAILED(webview_set_title(w, nullptr));
  ASSERT_WEBVIEW_FAILED(webview_set_html(w, nullptr));
  ASSERT_WEBVIEW_FAILED(webview_init(w, nullptr));
  ASSERT_WEBVIEW_FAILED(webview_eval(w, nullptr));
  ASSERT_WEBVIEW_FAILED(webview_bind(w, nullptr, nullptr, nullptr));
  ASSERT_WEBVIEW_FAILED(webview_unbind(w, nullptr));
  ASSERT_WEBVIEW_FAILED(webview_return(w, nullptr, 0, nullptr));
  ASSERT_WEBVIEW_FAILED(webview_dispatch(w, nullptr, nullptr));
  ASSERT_WEBVIEW_FAILED(webview_terminate(w));
  ASSERT_WEBVIEW_FAILED(webview_run(w));
  ASSERT_WEBVIEW_FAILED(webview_destroy(w));
}

TEST_CASE("webview_destroy refuses a handle already destroyed") {
  REQUIRE(webview_destroy(nullptr) == WEBVIEW_ERROR_INVALID_ARGUMENT);
  auto w = webview_create(false, nullptr);
  REQUIRE(w != nullptr);
  REQUIRE(webview_destroy(w) == WEBVIEW_ERROR_OK);
  // The handle is gone from the live registry: refused without touching it.
  REQUIRE(webview_destroy(w) == WEBVIEW_ERROR_INVALID_STATE);
  REQUIRE(webview_dispatch(w, cb_terminate, nullptr) ==
          WEBVIEW_ERROR_INVALID_STATE);
}

#if defined(__APPLE__)
// macOS: drag routing and file drops in the SweetviewWKWebView subclass, the
// background color's updateLayer reapply, and teardown after a host touched
// the WKWebView. Only the public C API and the ObjC runtime are used. A real
// drag cannot be synthesized here, so the view's NSDraggingDestination
// methods are called directly with a fake NSDraggingInfo, and WKWebView's own
// implementations are replaced by counting stubs for the duration of a test:
// the subclass reaches them through objc_msgSendSuper(WKWebView), so the
// stubs see exactly what WebKit would be given, and no WebKit code ever runs
// against the fake object.

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <objc/NSObjCRuntime.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <cmath>
#include <cstdlib>
#include <map>
#include <string>

namespace sv_mac_test {

// objc_msgSend cast to the exact function type of each call.
template <typename R, typename... Args>
R send(id self, const char *sel, Args... args) {
  return reinterpret_cast<R (*)(id, SEL, Args...)>(objc_msgSend)(
      self, sel_registerName(sel), args...);
}

inline id cls(const char *name) {
  return reinterpret_cast<id>(objc_getClass(name));
}

inline id nsstring(const char *utf8) {
  return send<id>(cls("NSString"), "stringWithUTF8String:", utf8);
}

inline id new_object(Class c) {
  return send<id>(send<id>(reinterpret_cast<id>(c), "alloc"), "init");
}

struct autorelease_pool {
  autorelease_pool() : pool{new_object(objc_getClass("NSAutoreleasePool"))} {}
  ~autorelease_pool() { send<void>(pool, "drain"); }
  autorelease_pool(const autorelease_pool &) = delete;
  autorelease_pool &operator=(const autorelease_pool &) = delete;
  id pool;
};

// Runs the AppKit event loop (events, timers, the main queue) for a while.
inline void spin(double seconds) {
  auto app = send<id>(cls("NSApplication"), "sharedApplication");
  auto mode = reinterpret_cast<id>(
      const_cast<void *>(static_cast<const void *>(kCFRunLoopDefaultMode)));
  auto end = CFAbsoluteTimeGetCurrent() + seconds;
  while (CFAbsoluteTimeGetCurrent() < end) {
    autorelease_pool pool;
    auto until = send<id>(cls("NSDate"), "dateWithTimeIntervalSinceNow:", 0.01);
    auto event =
        send<id>(app, "nextEventMatchingMask:untilDate:inMode:dequeue:",
                 static_cast<unsigned long long>(~0ULL), until, mode,
                 static_cast<BOOL>(YES));
    if (event) {
      send<void>(app, "sendEvent:", event);
    }
  }
}

// Stops [NSApp run] the way the library's own terminate does, for a test
// whose webview is already destroyed.
inline void stop_app() {
  auto app = send<id>(cls("NSApplication"), "sharedApplication");
  send<void>(app, "stop:", static_cast<id>(nullptr));
  auto event = send<id>(
      cls("NSEvent"),
      "otherEventWithType:location:modifierFlags:timestamp:windowNumber:"
      "context:subtype:data1:data2:",
      static_cast<NSUInteger>(15) /* NSEventTypeApplicationDefined */,
      CGPointMake(0, 0), static_cast<NSUInteger>(0), 0.0,
      static_cast<NSInteger>(0), static_cast<id>(nullptr),
      static_cast<short>(0), static_cast<NSInteger>(0),
      static_cast<NSInteger>(0));
  send<void>(app, "postEvent:atStart:", event, static_cast<BOOL>(YES));
}

// Terminates the webview's loop if a test's expected callback never comes,
// so a failure shows up as a failed REQUIRE rather than a ctest timeout.
struct watchdog {
  watchdog(webview_t w_, double seconds) : w{w_} {
    CFRunLoopTimerContext context{};
    context.info = this;
    timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + seconds, 0, 0, 0,
        +[](CFRunLoopTimerRef, void *info) {
          auto self = static_cast<watchdog *>(info);
          self->fired = true;
          webview_terminate(self->w);
        },
        &context);
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
  }
  ~watchdog() { cancel(); }
  watchdog(const watchdog &) = delete;
  watchdog &operator=(const watchdog &) = delete;
  void cancel() {
    if (timer) {
      CFRunLoopTimerInvalidate(timer);
      CFRelease(timer);
      timer = nullptr;
    }
  }
  webview_t w;
  CFRunLoopTimerRef timer{};
  bool fired{};
};

// ---- Fake NSDraggingInfo ----

struct fake_drag_values {
  NSInteger sequence;
  id source;
  NSUInteger source_mask;
  id pasteboard;
};

inline std::map<id, fake_drag_values> &fake_drag_table() {
  static std::map<id, fake_drag_values> table;
  return table;
}

inline const fake_drag_values &fake_values_of(id self) {
  return fake_drag_table().at(self);
}

inline Class fake_drag_class() {
  static Class c = [] {
    auto result = objc_allocateClassPair(objc_getClass("NSObject"),
                                         "SvTestFakeDraggingInfo", 0);
    class_addMethod(result, sel_registerName("draggingSequenceNumber"),
                    (IMP)(+[](id self, SEL) -> NSInteger {
                      return fake_values_of(self).sequence;
                    }),
                    "q@:");
    class_addMethod(
        result, sel_registerName("draggingSource"),
        (IMP)(+[](id self, SEL) -> id { return fake_values_of(self).source; }),
        "@@:");
    class_addMethod(result, sel_registerName("draggingSourceOperationMask"),
                    (IMP)(+[](id self, SEL) -> NSUInteger {
                      return fake_values_of(self).source_mask;
                    }),
                    "Q@:");
    class_addMethod(result, sel_registerName("draggingPasteboard"),
                    (IMP)(+[](id self, SEL) -> id {
                      return fake_values_of(self).pasteboard;
                    }),
                    "@@:");
    objc_registerClassPair(result);
    return result;
  }();
  return c;
}

struct fake_drag {
  fake_drag(NSInteger sequence, id source, NSUInteger source_mask,
            id pasteboard)
      : obj{new_object(fake_drag_class())} {
    fake_drag_table()[obj] =
        fake_drag_values{sequence, source, source_mask, pasteboard};
  }
  ~fake_drag() {
    fake_drag_table().erase(obj);
    send<void>(obj, "release");
  }
  fake_drag(const fake_drag &) = delete;
  fake_drag &operator=(const fake_drag &) = delete;
  id obj;
};

// A unique pasteboard holding one public.file-url item, the given URL
// string. The URL is written as is rather than built with
// [NSURL fileURLWithPath:], which decomposes non-ASCII characters (NFD), so
// the expected path is known byte for byte.
struct file_pasteboard {
  explicit file_pasteboard(const char *file_url) {
    autorelease_pool pool;
    pb = send<id>(send<id>(cls("NSPasteboard"), "pasteboardWithUniqueName"),
                  "retain");
    send<NSInteger>(pb, "clearContents");
    written = send<BOOL>(pb, "setString:forType:", nsstring(file_url),
                         nsstring("public.file-url"));
  }
  ~file_pasteboard() {
    send<void>(pb, "releaseGlobally");
    send<void>(pb, "release");
  }
  file_pasteboard(const file_pasteboard &) = delete;
  file_pasteboard &operator=(const file_pasteboard &) = delete;
  id pb{};
  bool written{};
};

// ---- Counting stubs in place of WKWebView's own drag methods ----

struct webkit_drag_counts {
  int entered, updated, exited, prepare, perform;
  int total() const { return entered + updated + exited + prepare + perform; }
};

inline webkit_drag_counts &webkit_counts() {
  static webkit_drag_counts counts{};
  return counts;
}

struct webkit_drag_stubs {
  webkit_drag_stubs() {
    webkit_counts() = webkit_drag_counts{};
    auto wk = objc_getClass("WKWebView");
    replace(wk, 0, "draggingEntered:", (IMP)(+[](id, SEL, id) -> NSUInteger {
              ++webkit_counts().entered;
              return 1; // NSDragOperationCopy
            }));
    replace(wk, 1, "draggingUpdated:", (IMP)(+[](id, SEL, id) -> NSUInteger {
              ++webkit_counts().updated;
              return 1;
            }));
    replace(wk, 2, "draggingExited:", (IMP)(+[](id, SEL, id) {
              ++webkit_counts().exited;
            }));
    replace(wk, 3, "prepareForDragOperation:", (IMP)(+[](id, SEL, id) -> BOOL {
              ++webkit_counts().prepare;
              return YES;
            }));
    replace(wk, 4, "performDragOperation:", (IMP)(+[](id, SEL, id) -> BOOL {
              ++webkit_counts().perform;
              return YES;
            }));
  }
  ~webkit_drag_stubs() {
    for (int i = 0; i < 5; ++i) {
      if (methods[i]) {
        method_setImplementation(methods[i], originals[i]);
      }
    }
  }
  webkit_drag_stubs(const webkit_drag_stubs &) = delete;
  webkit_drag_stubs &operator=(const webkit_drag_stubs &) = delete;

  // Every method must be WKWebView's own, or the stub would replace an
  // inherited (NSView) implementation instead.
  bool all_declared_by_webkit() const {
    for (int i = 0; i < 5; ++i) {
      if (!methods[i] || !own[i]) {
        return false;
      }
    }
    return true;
  }

private:
  void replace(Class wk, int i, const char *name, IMP stub) {
    auto sel = sel_registerName(name);
    unsigned int count = 0;
    auto list = class_copyMethodList(wk, &count);
    for (unsigned int k = 0; k < count; ++k) {
      if (method_getName(list[k]) == sel) {
        own[i] = true;
      }
    }
    free(list);
    methods[i] = own[i] ? class_getInstanceMethod(wk, sel) : nullptr;
    if (methods[i]) {
      originals[i] = method_setImplementation(methods[i], stub);
    }
  }
  Method methods[5]{};
  IMP originals[5]{};
  bool own[5]{};
};

inline id browser_of(webview_t w) {
  return static_cast<id>(const_cast<void *>(webview_get_native_handle(
      w, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER)));
}

inline id window_of(webview_t w) {
  return static_cast<id>(const_cast<void *>(
      webview_get_native_handle(w, WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW)));
}

inline NSUInteger entered(id view, id info) {
  return send<NSUInteger>(view, "draggingEntered:", info);
}
inline NSUInteger updated(id view, id info) {
  return send<NSUInteger>(view, "draggingUpdated:", info);
}
inline void exited(id view, id info) {
  send<void>(view, "draggingExited:", info);
}
inline BOOL prepare(id view, id info) {
  return send<BOOL>(view, "prepareForDragOperation:", info);
}
inline BOOL perform(id view, id info) {
  return send<BOOL>(view, "performDragOperation:", info);
}
inline void ended(id view, id info) {
  send<void>(view, "draggingEnded:", info);
}

// A path with a space and a non-ASCII character (U+00E9, precomposed), and
// its file URL.
inline std::string drop_path() {
  return "/tmp/sv-agent-native/drop test \xC3\xA9.txt";
}
inline const char *drop_url() {
  return "file:///tmp/sv-agent-native/drop%20test%20%C3%A9.txt";
}

struct drop_record {
  webview_t w{};
  watchdog *dog{};
  int calls{};
  std::string json;
  bool destroy_in_handler{};
  webview_error_t destroy_result{WEBVIEW_ERROR_UNSPECIFIED};
};

inline void record_drop(const char *paths_json, void *arg) {
  auto r = static_cast<drop_record *>(arg);
  ++r->calls;
  r->json = paths_json ? paths_json : "";
  if (r->destroy_in_handler) {
    r->dog->cancel();
    r->destroy_result = webview_destroy(r->w);
    r->w = nullptr;
    stop_app();
  } else {
    webview_terminate(r->w);
  }
}

// The external file drag of test cases 1 and 2, up to the deferred delivery.
inline void drive_external_file_drop(id view, id info, const drop_record &r) {
  REQUIRE(entered(view, info) == 1);
  REQUIRE(webkit_counts().entered == 0);
  REQUIRE(updated(view, info) == 1);
  REQUIRE(webkit_counts().updated == 0);
  REQUIRE(prepare(view, info) == YES);
  REQUIRE(webkit_counts().prepare == 0);
  REQUIRE(perform(view, info) == YES);
  REQUIRE(webkit_counts().perform == 0);
  // Delivery is deferred to a run-loop timer.
  REQUIRE(r.calls == 0);
  ended(view, info);
  REQUIRE(r.calls == 0);
  REQUIRE(webkit_counts().total() == 0);
}

} // namespace sv_mac_test

TEST_CASE("macOS: file drop is taken from WebKit and delivered later") {
  using namespace sv_mac_test;
  autorelease_pool pool;
  webkit_drag_stubs stubs;
  REQUIRE(stubs.all_declared_by_webkit());
  auto w = webview_create(false, nullptr);
  REQUIRE(w != nullptr);
  auto view = browser_of(w);
  REQUIRE(view != nullptr);
  // WebKit observes the view with KVO, so its runtime class is a KVO
  // subclass of SweetviewWKWebView.
  REQUIRE(send<BOOL>(view, "isKindOfClass:", cls("SweetviewWKWebView")) == YES);

  drop_record record;
  record.w = w;
  REQUIRE(webview_set_file_drop_handler(w, record_drop, &record) ==
          WEBVIEW_ERROR_OK);
  file_pasteboard pb{drop_url()};
  REQUIRE(pb.written);
  fake_drag info{7, nullptr, 1, pb.pb};
  drive_external_file_drop(view, info.obj, record);

  watchdog dog{w, 10};
  REQUIRE(webview_run(w) == WEBVIEW_ERROR_OK);
  REQUIRE_FALSE(dog.fired);
  REQUIRE(record.calls == 1);
  REQUIRE(record.json == "[\"" + drop_path() + "\"]");
  REQUIRE(webkit_counts().total() == 0);
  dog.cancel();
  REQUIRE(webview_destroy(w) == WEBVIEW_ERROR_OK);
}

// api.h: the handler "may set or clear the handler, open dialogs, or destroy
// the webview". webview_destroy closes the owned window and drains the event
// queue (a block on the main queue), which is why delivery runs from a
// CFRunLoopTimer rather than a main-queue block. Destroying does not stop
// [NSApp run] (only webview_terminate does, and the handle is gone), so the
// handler stops the application loop itself after the destroy returns.
TEST_CASE("macOS: file drop handler may destroy the webview") {
  using namespace sv_mac_test;
  autorelease_pool pool;
  webkit_drag_stubs stubs;
  REQUIRE(stubs.all_declared_by_webkit());
  auto w = webview_create(false, nullptr);
  REQUIRE(w != nullptr);
  auto view = browser_of(w);
  REQUIRE(view != nullptr);

  watchdog dog{w, 10};
  drop_record record;
  record.w = w;
  record.dog = &dog;
  record.destroy_in_handler = true;
  REQUIRE(webview_set_file_drop_handler(w, record_drop, &record) ==
          WEBVIEW_ERROR_OK);
  file_pasteboard pb{drop_url()};
  REQUIRE(pb.written);
  fake_drag info{8, nullptr, 1, pb.pb};
  drive_external_file_drop(view, info.obj, record);

  REQUIRE(webview_run(w) == WEBVIEW_ERROR_OK);
  REQUIRE_FALSE(dog.fired);
  REQUIRE(record.calls == 1);
  REQUIRE(record.json == "[\"" + drop_path() + "\"]");
  REQUIRE(record.destroy_result == WEBVIEW_ERROR_OK);
  REQUIRE(record.w == nullptr);
  // The loop keeps working after the destroy.
  spin(0.1);
  REQUIRE(webkit_counts().total() == 0);
}

TEST_CASE("macOS: refused external drags never reach WebKit") {
  using namespace sv_mac_test;
  autorelease_pool pool;
  webkit_drag_stubs stubs;
  REQUIRE(stubs.all_declared_by_webkit());
  auto w = webview_create(false, nullptr);
  REQUIRE(w != nullptr);
  auto view = browser_of(w);
  REQUIRE(view != nullptr);
  REQUIRE(webview_set_allow_external_drop(w, 0) == WEBVIEW_ERROR_OK);

  file_pasteboard pb{drop_url()};
  REQUIRE(pb.written);
  {
    fake_drag external{11, nullptr, 1, pb.pb};
    REQUIRE(entered(view, external.obj) == 0);
    REQUIRE(webkit_counts().entered == 0);
    REQUIRE(updated(view, external.obj) == 0);
    REQUIRE(webkit_counts().updated == 0);
    REQUIRE(prepare(view, external.obj) == NO);
    REQUIRE(webkit_counts().prepare == 0);
    REQUIRE(perform(view, external.obj) == NO);
    REQUIRE(webkit_counts().perform == 0);
    ended(view, external.obj);
    REQUIRE(webkit_counts().total() == 0);
  }
  {
    // A drag from inside the app (non-nil source) still goes to WebKit.
    fake_drag in_app{12, view, 1, pb.pb};
    REQUIRE(entered(view, in_app.obj) == 1);
    REQUIRE(webkit_counts().entered == 1);
    REQUIRE(updated(view, in_app.obj) == 1);
    REQUIRE(webkit_counts().updated == 1);
    exited(view, in_app.obj);
    REQUIRE(webkit_counts().exited == 1);
    REQUIRE(webkit_counts().prepare == 0);
    REQUIRE(webkit_counts().perform == 0);
  }
  REQUIRE(webview_destroy(w) == WEBVIEW_ERROR_OK);
}

TEST_CASE("macOS: disabling external drops mid-drag ends WebKit's session") {
  using namespace sv_mac_test;
  autorelease_pool pool;
  webkit_drag_stubs stubs;
  REQUIRE(stubs.all_declared_by_webkit());
  auto w = webview_create(false, nullptr);
  REQUIRE(w != nullptr);
  auto view = browser_of(w);
  REQUIRE(view != nullptr);

  file_pasteboard pb{drop_url()};
  REQUIRE(pb.written);
  fake_drag external{21, nullptr, 1, pb.pb};
  REQUIRE(entered(view, external.obj) == 1);
  REQUIRE(webkit_counts().entered == 1);
  REQUIRE(webview_set_allow_external_drop(w, 0) == WEBVIEW_ERROR_OK);
  REQUIRE(updated(view, external.obj) == 0);
  REQUIRE(webkit_counts().exited == 1);
  REQUIRE(webkit_counts().updated == 0);
  REQUIRE(updated(view, external.obj) == 0);
  REQUIRE(webkit_counts().exited == 1);
  REQUIRE(webkit_counts().updated == 0);
  exited(view, external.obj);
  REQUIRE(webkit_counts().exited == 1);
  ended(view, external.obj);
  REQUIRE(webkit_counts().entered == 1);
  REQUIRE(webkit_counts().exited == 1);
  REQUIRE(webkit_counts().prepare == 0);
  REQUIRE(webkit_counts().perform == 0);
  REQUIRE(webview_destroy(w) == WEBVIEW_ERROR_OK);
}

TEST_CASE("macOS: clearing the file drop handler mid-drag refuses the drop") {
  using namespace sv_mac_test;
  autorelease_pool pool;
  webkit_drag_stubs stubs;
  REQUIRE(stubs.all_declared_by_webkit());
  auto w = webview_create(false, nullptr);
  REQUIRE(w != nullptr);
  auto view = browser_of(w);
  REQUIRE(view != nullptr);

  drop_record record;
  record.w = w;
  REQUIRE(webview_set_file_drop_handler(w, record_drop, &record) ==
          WEBVIEW_ERROR_OK);
  file_pasteboard pb{drop_url()};
  REQUIRE(pb.written);
  fake_drag info{31, nullptr, 1, pb.pb};
  REQUIRE(entered(view, info.obj) == 1);
  REQUIRE(webkit_counts().entered == 0);
  REQUIRE(webview_set_file_drop_handler(w, nullptr, nullptr) ==
          WEBVIEW_ERROR_OK);
  REQUIRE(updated(view, info.obj) == 0);
  REQUIRE(perform(view, info.obj) == NO);
  ended(view, info.obj);
  spin(0.3);
  REQUIRE(record.calls == 0);
  REQUIRE(webkit_counts().total() == 0);
  REQUIRE(webview_destroy(w) == WEBVIEW_ERROR_OK);
}

namespace sv_mac_test {

inline bool layer_color_is(id view, double r, double g, double b, double a) {
  auto layer = send<id>(view, "layer");
  if (!layer) {
    return false;
  }
  auto color = send<CGColorRef>(layer, "backgroundColor");
  if (!color || CGColorGetNumberOfComponents(color) != 4) {
    return false;
  }
  auto c = CGColorGetComponents(color);
  return std::fabs(c[0] - r) < 0.01 && std::fabs(c[1] - g) < 0.01 &&
         std::fabs(c[2] - b) < 0.01 && std::fabs(c[3] - a) < 0.01;
}

} // namespace sv_mac_test

TEST_CASE("macOS: background color survives WebKit's layer updates") {
  using namespace sv_mac_test;
  autorelease_pool pool;
  auto w = webview_create(false, nullptr);
  REQUIRE(w != nullptr);
  auto view = browser_of(w);
  auto window = window_of(w);
  REQUIRE(view != nullptr);
  REQUIRE(window != nullptr);

  REQUIRE(webview_set_background_color(w, 255, 0, 0, 128) == WEBVIEW_ERROR_OK);
  auto layer = send<id>(view, "layer");
  REQUIRE(layer != nullptr);
  REQUIRE(layer_color_is(view, 1, 0, 0, 128 / 255.0));
  REQUIRE(send<BOOL>(window, "isOpaque") == NO);

  // WebKit's own updateLayer resets the color; the subclass puts it back.
  send<void>(layer, "setBackgroundColor:", static_cast<CGColorRef>(nullptr));
  REQUIRE(send<CGColorRef>(layer, "backgroundColor") == nullptr);
  send<void>(view, "updateLayer");
  REQUIRE(layer_color_is(view, 1, 0, 0, 128 / 255.0));

  REQUIRE(webview_set_size(w, 640, 420, WEBVIEW_HINT_NONE) == WEBVIEW_ERROR_OK);
  spin(0.2);
  REQUIRE(layer_color_is(view, 1, 0, 0, 128 / 255.0));

  REQUIRE(webview_set_background_color(w, 0, 0, 255, 255) == WEBVIEW_ERROR_OK);
  REQUIRE(layer_color_is(view, 0, 0, 1, 1));
  REQUIRE(send<BOOL>(window, "isOpaque") == YES);

  // Out-of-range channels are refused and change nothing.
  REQUIRE(webview_set_background_color(w, -1, 0, 0, 255) ==
          WEBVIEW_ERROR_INVALID_ARGUMENT);
  REQUIRE(webview_set_background_color(w, 0, 256, 0, 255) ==
          WEBVIEW_ERROR_INVALID_ARGUMENT);
  REQUIRE(webview_set_background_color(w, 0, 0, 0, 256) ==
          WEBVIEW_ERROR_INVALID_ARGUMENT);
  REQUIRE(webview_set_background_color(w, 0, 0, -1, 0) ==
          WEBVIEW_ERROR_INVALID_ARGUMENT);
  REQUIRE(layer_color_is(view, 0, 0, 1, 1));
  REQUIRE(send<BOOL>(window, "isOpaque") == YES);
  REQUIRE(webview_destroy(w) == WEBVIEW_ERROR_OK);
}

TEST_CASE("macOS: destroy after the host removed all script handlers") {
  using namespace sv_mac_test;
  auto w = webview_create(false, nullptr);
  REQUIRE(w != nullptr);
  {
    autorelease_pool pool;
    auto view = browser_of(w);
    REQUIRE(view != nullptr);
    webview_set_html(w, "<p>teardown</p>");
    spin(0.2);
    auto controller =
        send<id>(send<id>(view, "configuration"), "userContentController");
    REQUIRE(controller != nullptr);
    send<void>(controller, "removeAllScriptMessageHandlers");
  }
  REQUIRE(webview_destroy(w) == WEBVIEW_ERROR_OK);
  spin(0.1);
}

namespace sv_mac_test {

inline bool &host_delegate_deallocated() {
  static bool flag = false;
  return flag;
}

inline Class host_delegate_class() {
  static Class c = [] {
    auto result = objc_allocateClassPair(objc_getClass("NSObject"),
                                         "SvTestHostUIDelegate", 0);
    class_addMethod(result, sel_registerName("dealloc"),
                    (IMP)(+[](id self, SEL sel) {
                      host_delegate_deallocated() = true;
                      objc_super super_info{self, objc_getClass("NSObject")};
                      reinterpret_cast<void (*)(objc_super *, SEL)>(
                          objc_msgSendSuper)(&super_info, sel);
                    }),
                    "v@:");
    objc_registerClassPair(result);
    return result;
  }();
  return c;
}

} // namespace sv_mac_test

TEST_CASE("macOS: destroy leaves a host-installed UIDelegate alone") {
  using namespace sv_mac_test;
  host_delegate_deallocated() = false;
  auto delegate = new_object(host_delegate_class());
  auto w = webview_create(false, nullptr);
  REQUIRE(w != nullptr);
  CFIndex retained_before = 0;
  {
    autorelease_pool pool;
    auto view = browser_of(w);
    REQUIRE(view != nullptr);
    send<void>(view, "setUIDelegate:", delegate);
    REQUIRE(send<id>(view, "UIDelegate") == delegate);
  }
  retained_before = CFGetRetainCount(static_cast<CFTypeRef>(delegate));
  REQUIRE(webview_destroy(w) == WEBVIEW_ERROR_OK);
  spin(0.1);
  REQUIRE_FALSE(host_delegate_deallocated());
  REQUIRE(CFGetRetainCount(static_cast<CFTypeRef>(delegate)) >=
          retained_before);
  send<void>(delegate, "release");
  REQUIRE(host_delegate_deallocated());
}
#endif
