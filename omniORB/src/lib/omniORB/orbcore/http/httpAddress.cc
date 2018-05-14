// -*- Mode: C++; -*-
//                            Package   : omniORB
// httpAddress.cc             Created on: 18 April 2018
//                            Author    : Duncan Grisby
//
//    Copyright (C) 2018      BMC Software
//    Copyright (C) 2003-2013 Apasphere Ltd
//    Copyright (C) 2001      AT&T Laboratories Cambridge
//
//    This file is part of the omniORB library
//
//    The omniORB library is free software; you can redistribute it and/or
//    modify it under the terms of the GNU Lesser General Public
//    License as published by the Free Software Foundation; either
//    version 2.1 of the License, or (at your option) any later version.
//
//    This library is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//    Lesser General Public License for more details.
//
//    You should have received a copy of the GNU Lesser General Public
//    License along with this library. If not, see http://www.gnu.org/licenses/
//
//
// Description:
//      *** PROPRIETARY INTERFACE ***
//

#include <omniORB4/CORBA.h>
#include <omniORB4/giopEndpoint.h>
#include <omniORB4/omniURI.h>
#include <omniORB4/httpContext.h>
#include <orbParameters.h>
#include <giopStrandFlags.h>
#include <tcpSocket.h>
#include <http/httpConnection.h>
#include <http/httpAddress.h>
#include <stdio.h>
#include <omniORB4/linkHacks.h>

#if defined(__vxWorks__)
#  include "selectLib.h"
#endif

OMNI_EXPORT_LINK_FORCE_SYMBOL(httpAddress);

OMNI_NAMESPACE_BEGIN(omni)

/////////////////////////////////////////////////////////////////////////
httpAddress::httpAddress(const char* url, httpContext* ctx)
: pd_url(url), pd_ctx(ctx)
{
  CORBA::String_var scheme;

  CORBA::Boolean ok = omniURI::extractURL(url,
                                          scheme.out(), pd_address.host.out(),
                                          pd_address.port, pd_path.out());

  // Caller should have checked the URL already
  OMNIORB_ASSERT(ok);

  if (!strcmp(scheme, "https")) {
    pd_secure = 1;
    if (!pd_address.port)
      pd_address.port = 443;
  }
  else {
    pd_secure = 0;
    if (!pd_address.port)
      pd_address.port = 80;
  }

  setAddrString();
}

/////////////////////////////////////////////////////////////////////////
httpAddress::httpAddress(const char*          url,
                         CORBA::Boolean       secure,
                         const IIOP::Address& address,
                         const char*          path,
                         httpContext*         ctx)
  : pd_url(url), pd_secure(secure), pd_address(address),
    pd_path(path), pd_ctx(ctx)
{
  setAddrString();
}

/////////////////////////////////////////////////////////////////////////
void
httpAddress::setAddrString()
{
  const char* prefix = pd_secure ? "giop:http:https://" : "giop:http:http://";

  pd_address_string = omniURI::buildURI(prefix, pd_address.host,
                                        pd_address.port, pd_path);
}

/////////////////////////////////////////////////////////////////////////
const char*
httpAddress::type() const {
  return "giop:http";
}

/////////////////////////////////////////////////////////////////////////
const char*
httpAddress::address() const {
  return pd_address_string;
}

/////////////////////////////////////////////////////////////////////////
const char*
httpAddress::host() const {
  return pd_address.host;
}

/////////////////////////////////////////////////////////////////////////
giopAddress*
httpAddress::duplicate() const {
  return new httpAddress(pd_url, pd_secure, pd_address, pd_path, pd_ctx);
}

/////////////////////////////////////////////////////////////////////////
giopAddress*
httpAddress::duplicate(const char* host) const {
  IIOP::Address addr;
  addr.host = host;
  addr.port = pd_address.port;

  return new httpAddress(pd_url, pd_secure, addr, pd_path, pd_ctx);
}

/////////////////////////////////////////////////////////////////////////
#ifdef HAS_Cplusplus_Namespace
namespace {
#endif
  struct httpConnHolder {
    inline httpConnHolder() : sock(RC_SOCKET_ERROR), conn(0) {}
    inline ~httpConnHolder()
    {
      if (conn)
        delete conn;

      if (sock != RC_SOCKET_ERROR)
        CLOSESOCKET(sock);
    }

    inline httpActiveConnection* retn()
    {
      httpActiveConnection* r = conn;
      
      sock = RC_SOCKET_ERROR;
      conn = 0;

      return r;
    }
    
    SocketHandle_t        sock;
    httpActiveConnection* conn;
  };
#ifdef HAS_Cplusplus_Namespace
};
#endif

giopActiveConnection*
httpAddress::Connect(const omni_time_t& deadline,
                     CORBA::ULong       strand_flags,
                     CORBA::Boolean&    timed_out) const {

  if (pd_address.port == 0) return 0;

  // Address to connect to
  const char*       host;
  CORBA::UShort     port;

  // Web proxy
  CORBA::String_var proxy_host, proxy_auth;
  CORBA::UShort     proxy_port;
  CORBA::Boolean    proxy_secure;

  if (pd_ctx->proxy_info(proxy_host.out(), proxy_port,
                         proxy_auth.out(), proxy_secure)) {
    host = proxy_host;
    port = proxy_port;

    if (omniORB::trace(25)) {
      omniORB::logger log;
      log << "Connect via "
          << (proxy_auth.in() ? "authenticated " : "")
          << "web proxy "
          << (proxy_secure ? "https://" : "http://")
          << proxy_host << ':' << proxy_port << "/\n";
    }
  }
  else {
    host = pd_address.host;
    port = pd_address.port;
  }

  httpConnHolder h;
  
  h.sock = tcpSocket::Connect(host, port, deadline, strand_flags, timed_out);
  if (h.sock == RC_SOCKET_ERROR)
    return 0;

  CORBA::Boolean via_proxy = (proxy_host.in() && !pd_secure) ? 1 : 0;
  
  h.conn = new httpActiveConnection(h.sock, pd_address.host, pd_path, pd_url,
                                    via_proxy, proxy_auth);
  
  if (proxy_host.in()) {

    // SSL connect to proxy
    if (proxy_secure)
      if (!h.conn->sslConnect(host, port, pd_ctx, deadline, timed_out))
        return 0;

    // Make an HTTP CONNECT request
    if (pd_secure)
      if (!h.conn->proxyConnect(pd_address.host, pd_address.port,
                                deadline, timed_out))
        return 0;
  }

  // SSL connect to target server
  if (pd_secure)
    if (!h.conn->sslConnect(pd_address.host, pd_address.port, pd_ctx,
                            deadline, timed_out))
      return 0;

  return h.retn();
}


/////////////////////////////////////////////////////////////////////////
CORBA::Boolean
httpAddress::Poke() const {

  SocketHandle_t sock;

  if (pd_address.port == 0) return 0;

  LibcWrapper::AddrInfo_var ai;
  ai = LibcWrapper::getAddrInfo(pd_address.host, pd_address.port);

  if ((LibcWrapper::AddrInfo*)ai == 0)
    return 0;

  if ((sock = socket(ai->addrFamily(),SOCK_STREAM,0)) == RC_INVALID_SOCKET)
    return 0;

#if defined(USE_NONBLOCKING_CONNECT)

  if (tcpSocket::setNonBlocking(sock) == RC_INVALID_SOCKET) {
    CLOSESOCKET(sock);
    return 0;
  }

#endif

  if (::connect(sock,ai->addr(),ai->addrSize()) == RC_SOCKET_ERROR) {

    if (ERRNO != RC_EINPROGRESS) {
      CLOSESOCKET(sock);
      return 0;
    }
  }

  // The connect has not necessarily completed by this stage, but
  // we've done enough to poke the endpoint.
  CLOSESOCKET(sock);
  return 1;
}

OMNI_NAMESPACE_END(omni)