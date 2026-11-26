/*
 * MIT License
 *
 * Copyright (c) 2026 Mrowr Purr
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef WEBVIEW_PLATFORM_WINDOWS_STREAMING_ISTREAM_HH
#define WEBVIEW_PLATFORM_WINDOWS_STREAMING_ISTREAM_HH

#if defined(__cplusplus) && !defined(WEBVIEW_HEADER)

#include "../../../macros.h"

#if defined(WEBVIEW_PLATFORM_WINDOWS)

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <objbase.h>
#include <objidl.h>

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#endif

namespace webview {
namespace detail {

// The body of a streamed response (see webview_begin_response): a
// read-only, forward-only IStream fed from any thread with push() and closed
// with finish() or abort(). WebView2 reads it on a background thread, where
// Read() blocks until bytes arrive, the stream ends, or it is aborted. Only
// bytes not yet read are held.
//
// Free-threaded: every member is safe from any thread, and the object
// aggregates the free-threaded marshaler (and reports IAgileObject) so COM
// never marshals Read() back to the UI thread, which would deadlock the
// blocking read against the thread that feeds it.
class streaming_istream final : public IStream {
public:
  // The new object holds one reference, owned by the caller.
  streaming_istream() noexcept {
    if (FAILED(CoCreateFreeThreadedMarshaler(static_cast<IStream *>(this),
                                             &m_ftm))) {
      m_ftm = nullptr;
    }
  }

  streaming_istream(const streaming_istream &) = delete;
  streaming_istream &operator=(const streaming_istream &) = delete;
  streaming_istream(streaming_istream &&) = delete;
  streaming_istream &operator=(streaming_istream &&) = delete;

  // Appends bytes for the reader. False, and nothing kept, once the stream
  // has ended or been aborted.
  bool push(const void *data, size_t len) {
    if (!data || len == 0) {
      return true;
    }
    {
      std::lock_guard<std::mutex> lock{m_mutex};
      if (m_eof || m_aborted) {
        return false;
      }
      compact();
      m_buffer.append(static_cast<const char *>(data), len);
    }
    m_cv.notify_all();
    return true;
  }

  // No more bytes: once what is buffered has been read, Read() reports the
  // end of the stream.
  void finish() noexcept {
    {
      std::lock_guard<std::mutex> lock{m_mutex};
      m_eof = true;
    }
    m_cv.notify_all();
  }

  // Ends the stream at once: what is buffered is dropped and every Read(),
  // blocked or not, fails with E_ABORT.
  void abort() noexcept {
    {
      std::lock_guard<std::mutex> lock{m_mutex};
      m_aborted = true;
      m_buffer.clear();
      m_read_pos = 0;
    }
    m_cv.notify_all();
  }

  bool is_closed() const noexcept {
    std::lock_guard<std::mutex> lock{m_mutex};
    return m_eof || m_aborted;
  }

  // Bytes pushed and not yet read.
  size_t buffered() const noexcept {
    std::lock_guard<std::mutex> lock{m_mutex};
    return m_buffer.size() - m_read_pos;
  }

  // Set once the stream is handed to WebView2 in a response.
  void set_handed_off() noexcept { m_handed_off.store(true); }

  // True when the stream was handed to WebView2 and WebView2 has let go of
  // it (the page abandoned the response), so only the caller's reference
  // is left and nobody will read what is pushed.
  bool is_abandoned() const noexcept {
    return m_handed_off.load() && m_ref_count.load() <= 1;
  }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) {
      return E_POINTER;
    }
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ISequentialStream) ||
        IsEqualIID(riid, IID_IStream)) {
      *ppv = static_cast<IStream *>(this);
      AddRef();
      return S_OK;
    }
    if (IsEqualIID(riid, IID_IAgileObject)) {
      *ppv = static_cast<IStream *>(this);
      AddRef();
      return S_OK;
    }
    if (IsEqualIID(riid, IID_IMarshal) && m_ftm) {
      return m_ftm->QueryInterface(riid, ppv);
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref_count; }

  ULONG STDMETHODCALLTYPE Release() override {
    auto count = --m_ref_count;
    if (count == 0) {
      delete this;
    }
    return count;
  }

  // ISequentialStream
  HRESULT STDMETHODCALLTYPE Read(void *pv, ULONG cb, ULONG *pcbRead) override {
    if (pcbRead) {
      *pcbRead = 0;
    }
    if (!pv && cb > 0) {
      return STG_E_INVALIDPOINTER;
    }
    std::unique_lock<std::mutex> lock{m_mutex};
    m_cv.wait(lock, [this] {
      return m_aborted || m_eof || m_read_pos < m_buffer.size();
    });
    if (m_aborted) {
      return E_ABORT;
    }
    auto available = m_buffer.size() - m_read_pos;
    if (available == 0) {
      return S_FALSE; // the end, and everything before it has been read
    }
    if (cb == 0) {
      return S_OK;
    }
    auto n = available < static_cast<size_t>(cb) ? available
                                                 : static_cast<size_t>(cb);
    std::memcpy(pv, m_buffer.data() + m_read_pos, n);
    m_read_pos += n;
    if (m_read_pos == m_buffer.size()) {
      m_buffer.clear();
      m_read_pos = 0;
    }
    if (pcbRead) {
      *pcbRead = static_cast<ULONG>(n);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *) override {
    return E_NOTIMPL;
  }

  // IStream: forward-only and read-only, so everything else is refused.
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER, DWORD,
                                 ULARGE_INTEGER *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CopyTo(IStream *, ULARGE_INTEGER, ULARGE_INTEGER *,
                                   ULARGE_INTEGER *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
                                       DWORD) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
                                         DWORD) override {
    return E_NOTIMPL;
  }
  // The size is not known while the body is still being produced.
  HRESULT STDMETHODCALLTYPE Stat(STATSTG *pstatstg, DWORD) override {
    if (!pstatstg) {
      return STG_E_INVALIDPOINTER;
    }
    std::memset(pstatstg, 0, sizeof(*pstatstg));
    pstatstg->type = STGTY_STREAM;
    pstatstg->cbSize.QuadPart = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(IStream **ppstm) override {
    if (ppstm) {
      *ppstm = nullptr;
    }
    return E_NOTIMPL;
  }

private:
  ~streaming_istream() {
    if (m_ftm) {
      m_ftm->Release();
    }
  }

  // Drops bytes already read once they are most of the buffer. Under m_mutex.
  void compact() {
    if (m_read_pos > 0 && m_read_pos >= m_buffer.size() / 2) {
      m_buffer.erase(0, m_read_pos);
      m_read_pos = 0;
    }
  }

  std::atomic<ULONG> m_ref_count{1};
  std::atomic<bool> m_handed_off{false};
  IUnknown *m_ftm{};
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::string m_buffer;
  size_t m_read_pos{};
  bool m_eof{};
  bool m_aborted{};
};

} // namespace detail
} // namespace webview

#endif // defined(WEBVIEW_PLATFORM_WINDOWS)
#endif // defined(__cplusplus) && !defined(WEBVIEW_HEADER)
#endif // WEBVIEW_PLATFORM_WINDOWS_STREAMING_ISTREAM_HH
