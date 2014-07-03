/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined (__HTTPXzConv__h__)
#define __HTTPXzConv__h__ 1

#include "nsAutoPtr.h"
#include "nsCOMPtr.h"
#include "nsIObserver.h"
#include "nsIStreamConverter.h"
#include "nsIStringStream.h"

#define XZ_USE_CRC64
#include "xz.h"

#define NS_HTTPXZCONVERTER_CID                    \
{                                                 \
  /* 7b5f604a-84a4-48dd-ab53-8b8252275695 */      \
  0x7b5f604a,                                     \
  0x84a4,                                         \
  0x48dd,                                         \
  {0xab, 0x53, 0x8b, 0x82, 0x52, 0x27, 0x56, 0x95}\
}


#define HTTP_XZ_TYPE "xz"

namespace mozilla {
namespace net {

class HTTPXzConv : public nsIStreamConverter {
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIREQUESTOBSERVER
  NS_DECL_NSISTREAMLISTENER
  NS_DECL_NSISTREAMCONVERTER

  HTTPXzConv ();
  virtual ~HTTPXzConv ();

  // A pref observer needs to be initialised at app startup, so that we can
  // update the Accept-Encoding header when the converter.xz.enabled pref
  // is toggled.
  static already_AddRefed<nsIObserver> InitPrefObserver ();

private:
  nsresult do_OnDataAvailable (nsIRequest* request, nsISupports* aContext,
                               uint64_t aSourceOffset, const char* buffer,
                               uint32_t aCount);

  // In case there was some issue registering the preference observer with
  // the preference service, hold a strong reference to it here too to ensure
  // we can access it for the lifetime of this instance:
  nsCOMPtr<nsIObserver> mPrefObserver;

  nsCOMPtr<nsIStreamListener> mListener; // receives decompressed data
  nsCOMPtr<nsIStringInputStream> mStream; // supplies compressed data

  static bool sInitialisedXzLib;
  struct xz_dec* mXzDecoder;
  nsAutoArrayPtr<char> mOutBuffer;
  nsAutoArrayPtr<char> mInpBuffer;
  uint32_t mOutBufferLen;
  uint32_t mInpBufferLen;
  bool mStreamEnded;
  bool mStreamInitialized;
};

} // namespace mozilla::net
} // namespace mozilla

#endif
