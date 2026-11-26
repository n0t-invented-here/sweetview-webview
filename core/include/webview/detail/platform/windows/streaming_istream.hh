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
#include <functional>
#include <memory>
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

// The body of a streamed response (see webview_begin_response), split in two
// so neither side can outlive the memory the other touches:
//
// - stream_state is the queue: bytes fed from any thread with push() and
//   closed with finish() or abort(). It is shared (std::shared_ptr) between
//   the engine's table of open responses and the IStream handed to WebView2,
//   so whichever lets go last frees it. Only bytes not yet read are held.
// - streaming_istream is the read-only, forward-only IStream WebView2 reads
//   on a background thread. It is made on the UI thread when the response is
//   handed to WebView2 and is owned only by COM references, so it lives
//   exactly as long as WebView2 holds it. When the last reference goes, its
//   destructor tells the state (on_consumer_released): unless the stream had
//   already ended, nobody will read it again, so the state aborts itself at
//   once, dropping what is buffered, and calls on_abandoned so the owner can
//   forget it.
struct stream_state {
  stream_state() = default;
  stream_state(const stream_state &) = delete;
  stream_state &operator=(const stream_state &) = delete;
  stream_state(stream_state &&) = delete;
  stream_state &operator=(stream_state &&) = delete;

  // Called at most once, from the thread that drops the last reference to the
  // IStream before the stream ended. It runs with the state's lock held, so it
  // must not call back into the state; it should only queue the owner's
  // cleanup. finish() and abort() drop it, so once either returns it can never
  // run.
  void set_on_abandoned(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock{m_mutex};
    if (m_eof || m_aborted) {
      return;
    }
    m_on_abandoned = std::move(fn);
  }

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
      m_on_abandoned = nullptr;
    }
    m_cv.notify_all();
  }

  // Ends the stream at once: what is buffered is dropped and every Read(),
  // blocked or not, fails with E_ABORT.
  void abort() noexcept {
    {
      std::lock_guard<std::mutex> lock{m_mutex};
      abort_locked();
      m_on_abandoned = nullptr;
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

  // True once the IStream handed to WebView2 has been released for good, so
  // nobody will read what is pushed.
  bool is_abandoned() const noexcept {
    std::lock_guard<std::mutex> lock{m_mutex};
    return m_consumer_released;
  }

  // ISequentialStream::Read: fills pv with exactly cb bytes (S_OK), waiting
  // for them as needed, unless the stream ends first, in which case it
  // returns what was left (S_FALSE, possibly 0 bytes). E_ABORT, with nothing
  // read, once the stream is aborted. A zero-byte read never waits.
  HRESULT read(void *pv, ULONG cb, ULONG *pcbRead) {
    if (pcbRead) {
      *pcbRead = 0;
    }
    if (!pv && cb > 0) {
      return E_POINTER;
    }
    if (cb == 0) {
      return S_OK; // nothing asked for: never wait
    }
    auto *out = static_cast<char *>(pv);
    auto wanted = static_cast<size_t>(cb);
    size_t total_read = 0;
    std::unique_lock<std::mutex> lock{m_mutex};
    while (total_read < wanted && !m_aborted) {
      if (m_read_pos == m_buffer.size()) {
        if (m_eof) {
          break;
        }
        m_cv.wait(lock, [this] {
          return m_aborted || m_eof || m_read_pos < m_buffer.size();
        });
        continue;
      }
      auto available = m_buffer.size() - m_read_pos;
      auto n = (wanted - total_read) < available ? (wanted - total_read)
                                                 : available;
      std::memcpy(out + total_read, m_buffer.data() + m_read_pos, n);
      m_read_pos += n;
      total_read += n;
      if (m_read_pos == m_buffer.size()) {
        m_buffer.clear();
        m_read_pos = 0;
      }
    }
    if (m_aborted) {
      return E_ABORT; // *pcbRead is already 0
    }
    if (pcbRead) {
      *pcbRead = static_cast<ULONG>(total_read);
    }
    return total_read == wanted ? S_OK : S_FALSE;
  }

  // The IStream reading this state is gone for good (its last COM reference
  // was released). Unless the stream already ended, nobody will read it
  // again: drop what is buffered now and let the owner forget it (see
  // set_on_abandoned). Called once, from the IStream's destructor.
  void on_consumer_released() noexcept {
    {
      std::lock_guard<std::mutex> lock{m_mutex};
      m_consumer_released = true;
      if (m_eof || m_aborted) {
        m_on_abandoned = nullptr;
        return;
      }
      abort_locked();
      auto fn = std::move(m_on_abandoned);
      m_on_abandoned = nullptr;
      if (fn) {
        try {
          fn();
        } catch (...) {
          // Nothing may unwind out of Release().
        }
      }
    }
    m_cv.notify_all();
  }

private:
  // Under m_mutex.
  void abort_locked() noexcept {
    m_aborted = true;
    m_buffer.clear();
    m_buffer.shrink_to_fit();
    m_read_pos = 0;
  }

  // Drops bytes already read once they are most of the buffer. Under m_mutex.
  void compact() {
    if (m_read_pos > 0 && m_read_pos >= m_buffer.size() / 2) {
      m_buffer.erase(0, m_read_pos);
      m_read_pos = 0;
    }
  }

  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::string m_buffer;
  size_t m_read_pos{};
  bool m_eof{};
  bool m_aborted{};
  bool m_consumer_released{};
  std::function<void()> m_on_abandoned;
};

// The IStream over a stream_state (see above). Free-threaded: the object
// aggregates the free-threaded marshaler (and reports IAgileObject) so COM
// never marshals Read() back to the UI thread, which would deadlock the
// blocking read against the thread that feeds it.
class streaming_istream final : public IStream {
public:
  // The new object holds one reference, owned by the caller.
  explicit streaming_istream(std::shared_ptr<stream_state> state) noexcept
      : m_state{std::move(state)} {
    if (FAILED(CoCreateFreeThreadedMarshaler(static_cast<IStream *>(this),
                                             &m_ftm))) {
      m_ftm = nullptr;
    }
  }

  streaming_istream(const streaming_istream &) = delete;
  streaming_istream &operator=(const streaming_istream &) = delete;
  streaming_istream(streaming_istream &&) = delete;
  streaming_istream &operator=(streaming_istream &&) = delete;

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) {
      return E_POINTER;
    }
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ISequentialStream) ||
        IsEqualIID(riid, IID_IStream) || IsEqualIID(riid, IID_IAgileObject)) {
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

  // The Release() that brings the count to 0 destroys the object, and the
  // destructor tells the state its reader is gone. Nothing else is touched
  // after the decrement, so no other owner of the state can race it.
  ULONG STDMETHODCALLTYPE Release() override {
    auto count = --m_ref_count;
    if (count == 0) {
      delete this;
    }
    return count;
  }

  // ISequentialStream (see stream_state::read)
  HRESULT STDMETHODCALLTYPE Read(void *pv, ULONG cb, ULONG *pcbRead) override {
    return m_state->read(pv, cb, pcbRead);
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
    if (m_state) {
      m_state->on_consumer_released();
    }
    if (m_ftm) {
      m_ftm->Release();
    }
  }

  std::atomic<ULONG> m_ref_count{1};
  IUnknown *m_ftm{};
  std::shared_ptr<stream_state> m_state;
};

} // namespace detail
} // namespace webview

#endif // defined(WEBVIEW_PLATFORM_WINDOWS)
#endif // defined(__cplusplus) && !defined(WEBVIEW_HEADER)
#endif // WEBVIEW_PLATFORM_WINDOWS_STREAMING_ISTREAM_HH
