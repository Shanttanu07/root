// $Id$
// Author: Sergey Linev   8/01/2018

/*************************************************************************
 * Copyright (C) 1995-2013, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "THttpLongPollEngine.h"

#include "THttpCallArg.h"
#include "TSystem.h"
#include <ROOT/TLogger.hxx>

#include <cstring>
#include <cstdlib>

//////////////////////////////////////////////////////////////////////////
//                                                                      //
// THttpLongPollEngine                                                  //
//                                                                      //
// Emulation of websocket with long poll requests                       //
// Allows to send data from server to client without explicit request   //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

const char *THttpLongPollEngine::gLongPollNope = "<<nope>>";

THttpLongPollEngine::QueueItem::~QueueItem()
{
   if (fBuffer)
      free((void *)fBuffer);
}

//////////////////////////////////////////////////////////////////////////
/// returns ID of the engine, created from this pointer

UInt_t THttpLongPollEngine::GetId() const
{
   const void *ptr = (const void *)this;
   return TString::Hash((void *)&ptr, sizeof(void *));
}

//////////////////////////////////////////////////////////////////////////
/// clear request, waiting for next portion of data

void THttpLongPollEngine::ClearHandle()
{
   if (fPoll) {
      fPoll->Set404();
      fPoll->NotifyCondition();
      fPoll = nullptr;
   }
}

//////////////////////////////////////////////////////////////////////////
/// Create raw buffer which should be send as reply
/// For the raw mode all information must be send via binary response

void *THttpLongPollEngine::MakeBuffer(const void *buf, int &len, const char *hdr)
{
   if (!fRaw) {
      void *res = malloc(len);
      memcpy(res, buf, len);
      return res;
   }

   int hdrlen = hdr ? strlen(hdr) : 0;
   std::string hdrstr = "bin:";
   hdrstr.append(std::to_string(hdrlen));

   while ((hdrstr.length() + 1 + hdrlen) % 8 != 0)
      hdrstr.append(" ");
   hdrstr.append(":");
   if (hdrlen > 0)
      hdrstr.append(hdr);

   void *res = malloc(hdrstr.length() + len);
   memcpy(res, hdrstr.c_str(), hdrstr.length());
   memcpy((char *)res + hdrstr.length(), buf, len);
   len += hdrstr.length();
   return res;
}

//////////////////////////////////////////////////////////////////////////
/// Send binary data via connection - not supported

void THttpLongPollEngine::Send(const void *buf, int len)
{
   void *buf2 = MakeBuffer(buf, len);

   if (fPoll) {
      fPoll->SetContentType("application/x-binary");
      fPoll->SetBinData(buf2, len);
      fPoll->NotifyCondition();
      fPoll = nullptr;
   } else {
      fQueue.emplace_back(buf2, len);
      if (fQueue.size() > 100)
         R__ERROR_HERE("http") << "Too many send operations " << fQueue.size() << " in the queue, check algorithms";
   }
}

//////////////////////////////////////////////////////////////////////////
/// Send binary data with text header via connection - not supported

void THttpLongPollEngine::SendHeader(const char *hdr, const void *buf, int len)
{
   void *buf2 = MakeBuffer(buf, len, hdr);

   if (fPoll) {
      fPoll->SetContentType("application/x-binary");
      fPoll->SetBinData(buf2, len);
      if (!fRaw)
         fPoll->SetExtraHeader("LongpollHeader", hdr);
      fPoll->NotifyCondition();
      fPoll = nullptr;
   } else {
      fQueue.emplace_back(buf2, len, hdr);
      if (fQueue.size() > 100)
         R__ERROR_HERE("http") << "Too many send operations " << fQueue.size() << " in the queue, check algorithms";
   }
}

//////////////////////////////////////////////////////////////////////////
/// Send const char data
/// Either do it immediately or keep in internal buffer

void THttpLongPollEngine::SendCharStar(const char *buf)
{
   std::string sendbuf(fRaw ? "txt:" : "");
   sendbuf.append(buf);

   if (fPoll) {
      fPoll->SetContentType("text/plain");
      fPoll->SetContent(sendbuf.c_str());
      fPoll->NotifyCondition();
      fPoll = nullptr;
   } else {
      fQueue.emplace_back(sendbuf);
      if (fQueue.size() > 100)
         R__ERROR_HERE("http") << "Too many send operations " << fQueue.size() << " in the queue, check algorithms";
   }
}

//////////////////////////////////////////////////////////////////////////////
/// Preview data for given socket
/// function called in the user code before processing correspondent websocket data
/// returns kTRUE when user should ignore such http request - it is for internal use

Bool_t THttpLongPollEngine::PreviewData(THttpCallArg &arg)
{
   if (!strstr(arg.GetQuery(), "&dummy")) {
      // this is normal request, deliver and process it as any other
      // put dummy content, it can be overwritten in the future
      arg.SetContentType("text/plain");
      arg.SetContent(gLongPollNope);
      return kFALSE;
   }

   if (&arg == fPoll)
      R__FATAL_HERE("http") << "Same object once again";

   if (fPoll) {
      R__ERROR_HERE("http") << "Get next dummy request when previous not completed";
      // if there are pending request, reply it immediately
      fPoll->SetContentType("text/plain");
      fPoll->SetContent(gLongPollNope); // normally should never happen
      fPoll->NotifyCondition();         // inform http server that request is processed
      fPoll = nullptr;
   }

   if (fQueue.size() > 0) {
      QueueItem &item = fQueue.front();
      if (item.fBuffer) {
         arg.SetContentType("application/x-binary");
         arg.SetBinData((void *)item.fBuffer, item.fLength);
         item.fBuffer = nullptr; // forget memory
         if (!fRaw && !item.fMessage.empty())
            arg.SetExtraHeader("LongpollHeader", item.fMessage.c_str());
      } else {
         arg.SetContentType("text/plain");
         arg.SetContent(item.fMessage.c_str());
      }
      fQueue.erase(fQueue.begin());
   } else {
      arg.SetPostponed(); // mark http request as pending, http server should wait for notification
      fPoll = &arg;       // keep pointer
   }

   // if arguments has "&dummy" string, user should not process it
   return kTRUE;
}

//////////////////////////////////////////////////////////////////////////////
/// Normally requests from client does not replied directly
/// Therefore one can use it to send data with it

void THttpLongPollEngine::PostProcess(THttpCallArg &arg)
{
   if ((fQueue.size() > 0) && arg.IsContentType("text/plain") &&
       (arg.GetContentLength() == (Long_t)strlen(gLongPollNope)) &&
       (strcmp((const char *)arg.GetContent(), gLongPollNope) == 0)) {
      QueueItem &item = fQueue.front();
      if (item.fBuffer) {
         arg.SetContentType("application/x-binary");
         // provide binary buffer with ownership
         arg.SetBinData((void *)item.fBuffer, item.fLength);
         // forget memory
         item.fBuffer = nullptr;
         if (!fRaw && !item.fMessage.empty())
            arg.SetExtraHeader("LongpollHeader", item.fMessage.c_str());
      } else {
         arg.SetContentType("text/plain");
         arg.SetContent(item.fMessage.c_str());
      }
      fQueue.erase(fQueue.begin());
   }
}
