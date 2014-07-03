/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This file is derived from nsHTTPCompressConv.cpp
#include <algorithm>
#include "HTTPXzConv.h"
#include "mozilla/Preferences.h"
#include "nsAlgorithm.h"
#include "nsComponentManagerUtils.h"
#include "nsCRT.h"
#include "nsMemory.h"
#include "nsStreamUtils.h"
#include "nsStringStream.h"
#include "nsThreadUtils.h"

namespace mozilla {
namespace net {

// nsISupports implementation
NS_IMPL_ISUPPORTS(HTTPXzConv,
                  nsIStreamConverter,
                  nsIStreamListener,
                  nsIRequestObserver)

// static variables
static class HTTPXzConvPrefObserver* sPrefObserver = nullptr;
bool HTTPXzConv::sInitialisedXzLib = false;

// A preference observer
static const char kEnabledPrefName[] = "converter.xz.enabled";
static const char kMemLimitPrefName[] = "converter.xz.memory_limit_mb";
static const char kAcceptEncodingPrefName[] = "network.http.accept-encoding";

class HTTPXzConvPrefObserver : public nsIObserver
{
public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIOBSERVER

  // To disable use of xz encoding.  Modifying this pref also triggers an
  // adjustment to network.http.accept-encoding to include/exclude xz in the
  // Accept-Encoding request header.
  bool mXzEnabled;

  // A limit on the memory used for each LZMA decoding stream.
  // xz(1) manual page says: "...decompressing a file created with xz -9
  // currently requires 65 MiB of memory.  Still, it is possible to have .xz
  // files that require several gigabytes of memory to decompress."
  uint32_t mXzMemoryLimitMB;

  HTTPXzConvPrefObserver();
  virtual ~HTTPXzConvPrefObserver();
};
NS_IMPL_ISUPPORTS(HTTPXzConvPrefObserver, nsIObserver)

// HTTPXzConvPrefObserver methods

HTTPXzConvPrefObserver::HTTPXzConvPrefObserver()
  : mXzEnabled(false)
  , mXzMemoryLimitMB(32)
{
  MOZ_ASSERT(NS_IsMainThread()); // otherwise we may have a race condition

  nsresult rv = Preferences::AddStrongObserver(this, kEnabledPrefName);
  NS_ENSURE_SUCCESS_VOID(rv);
  rv = Preferences::AddStrongObserver(this, kMemLimitPrefName);
  NS_ENSURE_SUCCESS_VOID(rv);

  Observe(nullptr, NS_PREFBRANCH_PREFCHANGE_TOPIC_ID, nullptr);
}

HTTPXzConvPrefObserver::~HTTPXzConvPrefObserver()
{
  sPrefObserver = nullptr;
}

nsresult
HTTPXzConvPrefObserver::Observe(nsISupports* subject, const char* topic,
                                const char16_t* data)
{
  NS_ASSERTION(strcmp(topic, NS_PREFBRANCH_PREFCHANGE_TOPIC_ID) == 0,
               "Unexpected observe call topic.");

  mXzMemoryLimitMB = Preferences::GetUint(kMemLimitPrefName, mXzMemoryLimitMB);

  bool origXzEnabled = mXzEnabled;
  mXzEnabled = Preferences::GetBool(kEnabledPrefName, mXzEnabled);
  if (mXzEnabled == origXzEnabled) {
    // The xz enabled pref has not changed, so nothing further to be done
    return NS_OK;
  }

  // The xz converter is being enabled or disabled.  We need to adjust the
  // value we send for the Accept-Encoding: request header accordingly.
  // First get the existing value from "network.http.accept-encoding" pref:
  nsAutoCString acceptEncoding;
  nsresult rv = Preferences::GetCString(kAcceptEncodingPrefName,
                                        &acceptEncoding);
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoCString newAcceptEncoding;
  char* begin = acceptEncoding.BeginWriting();
  char* elem;

  if (!mXzEnabled) {
    // We need to cut out "xz" from the header.  Build the new header value
    // by copying comma-separated components from the old header, except for
    // any component for "xz".
    while ((elem = nsCRT::strtok(begin, ",", &begin))) {
      for (; *elem && strchr(" \t", *elem); ++elem); // skip leading whitespace
      // copy element to new header unless it is for "xz"
      if (!(elem[0]=='x' && elem[1]=='z' && strchr("; ", elem[2]))) {
        if (!newAcceptEncoding.IsEmpty()) {
          newAcceptEncoding.Append(", ");
        }
        newAcceptEncoding.Append(elem);
      }
    }
  } else {
    // We need to add "xz" to the header if it is not there already.  Check
    // this by going through comma-separated components of the existing header.
    newAcceptEncoding = acceptEncoding;
    while ((elem = nsCRT::strtok(begin, ",", &begin))) {
      for (; *elem && strchr(" \t", *elem); ++elem); // skip leading whitespace
      if (elem[0]=='x' && elem[1]=='z' && strchr("; ", elem[2])) {
        return NS_OK; // "xz" is already present, nothing to add
      }
    }
    // Add "xz" to the Accept-Encoding header
    if (!newAcceptEncoding.IsEmpty()) {
      newAcceptEncoding.Append(", ");
    }
    newAcceptEncoding.Append("xz");
  }

  return Preferences::SetCString(kAcceptEncodingPrefName, newAcceptEncoding);
}

// HTTPXzConv methods

HTTPXzConv::HTTPXzConv()
  : mOutBufferLen(0)
  , mInpBufferLen(0)
  , mStreamEnded(false)
  , mStreamInitialized(false)
{
  mPrefObserver = InitPrefObserver();

  if (!sInitialisedXzLib) {
    // The XZ Embedded library requires its CRC implementation to be
    // initialised before the library is used.
#if XZ_INTERNAL_CRC32
    xz_crc32_init();
#endif
#if XZ_INTERNAL_CRC64
    xz_crc64_init();
#endif
    sInitialisedXzLib = true;
  }
}

HTTPXzConv::~HTTPXzConv()
{
  if (mStreamInitialized && !mStreamEnded)
    xz_dec_end(mXzDecoder);
}

already_AddRefed<nsIObserver>
HTTPXzConv::InitPrefObserver()
{
  // This pref observer is constructed only once, and kept alive for the
  // lifetime of the application by a reference held by the preference
  // service.  It needs to be kept alive so that any changes to the "enabled"
  // pref will result in the "accept-encoding" pref being updated.
  if (!sPrefObserver) {
    sPrefObserver = new HTTPXzConvPrefObserver();
  }
  nsCOMPtr<nsIObserver> prefObserver = sPrefObserver; // this adds a ref
  return prefObserver.forget();
}

NS_IMETHODIMP
HTTPXzConv::AsyncConvertData(const char* aFromType,
                             const char* aToType,
                             nsIStreamListener* aListener,
                             nsISupports* aCtxt)
{
  MOZ_ASSERT(sPrefObserver);
  if (!sPrefObserver->mXzEnabled)
    return NS_ERROR_ABORT; // XZ decoding disabled by pref

  // hook ourself up with the receiving listener.
  mListener = aListener;

  return NS_OK;
}

NS_IMETHODIMP
HTTPXzConv::OnStartRequest(nsIRequest* request, nsISupports* aContext)
{
  return mListener->OnStartRequest(request, aContext);
}

NS_IMETHODIMP
HTTPXzConv::OnStopRequest(nsIRequest* request, nsISupports* aContext,
                          nsresult aStatus)
{
  return mListener->OnStopRequest(request, aContext, aStatus);
}

NS_IMETHODIMP
HTTPXzConv::OnDataAvailable(nsIRequest* request,
                            nsISupports* aContext,
                            nsIInputStream* iStr,
                            uint64_t aSourceOffset,
                            uint32_t aCount)
{
  nsresult rv;

  if (aCount == 0) {
    NS_ERROR("count of zero passed to OnDataAvailable");
    return NS_ERROR_UNEXPECTED;
  }

  if (mStreamEnded) {
    // Hmm... this may just indicate that the data stream is done and that
    // what's left is either metadata or padding of some sort.... throwing
    // it out is probably the safe thing to do.
    uint32_t n;
    return iStr->ReadSegments(NS_DiscardSegment, nullptr, aCount, &n);
  }

  if (!mInpBuffer) {
    mInpBufferLen = std::max(aCount, 8192U);
    mInpBuffer = static_cast<char*>(nsMemory::Alloc(mInpBufferLen));
  } else if (mInpBufferLen < aCount) {
    mInpBufferLen = aCount;
    mInpBuffer = static_cast<char*>(nsMemory::Realloc(mInpBuffer.forget(),
                                                      mInpBufferLen));
  }

  if (!mOutBuffer) {
    mOutBufferLen = mInpBufferLen * 3;
    mOutBuffer = static_cast<char*>(nsMemory::Alloc(mOutBufferLen));
  } else if (mOutBufferLen < aCount * 2) {
    mOutBufferLen = aCount * 3;
    mOutBuffer = static_cast<char*>(nsMemory::Realloc(mOutBuffer.forget(),
                                                      mOutBufferLen));
  }

  if (!mInpBuffer || !mOutBuffer) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  uint32_t bytesRead;
  iStr->Read(mInpBuffer.get(), aCount, &bytesRead);

  if (!mStreamInitialized) {
    uint64_t memlimit = (uint64_t)sPrefObserver->mXzMemoryLimitMB * 1048576;
    memlimit = std::min((uint64_t)0xffffffffU, memlimit); // cap to 32-bits
    if (!(mXzDecoder = xz_dec_init(XZ_DYNALLOC, (uint32_t)memlimit))) {
      return NS_ERROR_FAILURE;
    }
    mStreamInitialized = true;
  }

  struct xz_buf xzBuf;
  xzBuf.in = reinterpret_cast<uint8_t*>(mInpBuffer.get());
  xzBuf.in_pos = 0;
  xzBuf.in_size = bytesRead;

  while (xzBuf.in_pos < xzBuf.in_size) {
    xzBuf.out = reinterpret_cast<uint8_t*>(mOutBuffer.get());
    xzBuf.out_pos = 0;
    xzBuf.out_size = mOutBufferLen;

    xz_ret ret = xz_dec_run(mXzDecoder, &xzBuf);
    uint32_t bytesWritten = (uint32_t)xzBuf.out_pos;

    if (ret == XZ_STREAM_END) {
      if (xzBuf.out_pos) {
        rv = do_OnDataAvailable(request, aContext, aSourceOffset,
                                mOutBuffer.get(), bytesWritten);
        NS_ENSURE_SUCCESS(rv, rv);
      }

      xz_dec_end(mXzDecoder);
      mStreamEnded = true;
      break;
    } else if (ret == XZ_OK || ret == XZ_UNSUPPORTED_CHECK) {
      if (bytesWritten) {
        rv = do_OnDataAvailable(request, aContext, aSourceOffset,
                                mOutBuffer.get(), bytesWritten);
        NS_ENSURE_SUCCESS(rv, rv);
      }
    } else if (ret == XZ_MEM_ERROR) {
      return NS_ERROR_OUT_OF_MEMORY;
    } else {
      printf_stderr("SNORP: XZ error: %d\n", ret);
      return NS_ERROR_INVALID_CONTENT_ENCODING;
    }
  } /* for */

  return NS_OK;
} /* OnDataAvailable */

// nsIStreamConverter::Convert is not implemented (same as nsHTTPCompressConv)
NS_IMETHODIMP
HTTPXzConv::Convert(nsIInputStream* aFromStream,
                    const char* aFromType,
                    const char* aToType,
                    nsISupports* aCtxt,
                    nsIInputStream** _retval)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

nsresult
HTTPXzConv::do_OnDataAvailable(nsIRequest* request,
                               nsISupports* context, uint64_t offset,
                               const char* buffer, uint32_t count)
{
  if (!mStream) {
    mStream = do_CreateInstance(NS_STRINGINPUTSTREAM_CONTRACTID);
    NS_ENSURE_STATE(mStream);
  }

  mStream->ShareData(buffer, count);

  nsresult rv = mListener->OnDataAvailable(request, context, mStream,
                                           offset, count);

  // Make sure the stream no longer references |buffer| in case our listener
  // is crazy enough to try to read from |mStream| after ODA.
  mStream->ShareData("", 0);

  return rv;
}

} // namespace mozilla::net
} // namespace mozilla
