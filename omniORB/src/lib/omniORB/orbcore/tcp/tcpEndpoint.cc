// -*- Mode: C++; -*-
//                            Package   : omniORB
// tcpEndpoint.cc             Created on: 19 Mar 2001
//                            Author    : Sai Lai Lo (sll)
//
//    Copyright (C) 2002-2013 Apasphere Ltd
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
//	*** PROPRIETARY INTERFACE ***
//

#include <omniORB4/CORBA.h>
#include <omniORB4/giopEndpoint.h>
#include <omniORB4/omniURI.h>
#include <omniORB4/connectionInfo.h>
#include <SocketCollection.h>
#include <orbParameters.h>
#include <objectAdapter.h>
#include <libcWrapper.h>
#include <tcpSocket.h>
#include <tcp/tcpTransportImpl.h>
#include <tcp/tcpConnection.h>
#include <tcp/tcpAddress.h>
#include <tcp/tcpEndpoint.h>
#include <stdio.h>
#include <omniORB4/linkHacks.h>

OMNI_EXPORT_LINK_FORCE_SYMBOL(tcpEndpoint);

OMNI_NAMESPACE_BEGIN(omni)

/////////////////////////////////////////////////////////////////////////
tcpEndpoint::tcpEndpoint(const char* param) :
  SocketHolder(RC_INVALID_SOCKET), pd_address_param(param),
  pd_new_conn_socket(RC_INVALID_SOCKET), pd_callback_func(0),
  pd_callback_cookie(0), pd_poked(0)
{
}

/////////////////////////////////////////////////////////////////////////
tcpEndpoint::~tcpEndpoint() {
  if (pd_socket != RC_INVALID_SOCKET) {
    CLOSESOCKET(pd_socket);
    pd_socket = RC_INVALID_SOCKET;
  }
}

/////////////////////////////////////////////////////////////////////////
const char*
tcpEndpoint::type() const {
  return "giop:tcp";
}

/////////////////////////////////////////////////////////////////////////
const char*
tcpEndpoint::address() const {
  return pd_addresses[0];
}

/////////////////////////////////////////////////////////////////////////
const _CORBA_Unbounded_Sequence_String*
tcpEndpoint::addresses() const {
  return &pd_addresses;
}

/////////////////////////////////////////////////////////////////////////
static CORBA::Boolean
publish_one(const char*    	     publish_spec,
	    const char*    	     ep,
	    CORBA::Boolean 	     no_publish,
	    orbServer::EndpointList& published_eps)
{
  OMNIORB_ASSERT(!strncmp(ep, "giop:tcp:", 9));

  CORBA::String_var to_add;
  CORBA::String_var ep_host;
  CORBA::UShort     ep_port;

  ep_host = omniURI::extractHostPort(ep+9, ep_port);

  if (!strncmp(publish_spec, "giop:tcp:", 9)) {
    CORBA::UShort port;
    CORBA::String_var host = omniURI::extractHostPort(publish_spec+9, port, 0);
    if (!(char*)host) {
      if (omniORB::trace(1)) {
	omniORB::logger l;
	l << "Invalid endpoint '" << publish_spec
	  << "' in publish specification.\n";
      }
      OMNIORB_THROW(INITIALIZE,
		    INITIALIZE_EndpointPublishFailure,
		    CORBA::COMPLETED_NO);
    }
    if (strlen(host) == 0)
      host = ep_host;

    if (!port)
      port = ep_port;

    to_add = omniURI::buildURI("giop:tcp", host, port);
  }
  else if (no_publish) {
    // Suppress all the other options
    return 0;
  }
  else if (omni::strMatch(publish_spec, "addr")) {
    to_add = ep;
  }
  else if (omni::strMatch(publish_spec, "ipv6")) {
    if (!LibcWrapper::isip6addr(ep_host))
      return 0;
    to_add = ep;
  }
  else if (omni::strMatch(publish_spec, "ipv4")) {
    if (!LibcWrapper::isip4addr(ep_host))
      return 0;
    to_add = ep;
  }
  else if (omni::strMatch(publish_spec, "name")) {
    LibcWrapper::AddrInfo_var ai = LibcWrapper::getAddrInfo(ep_host, 0);
    if (!ai.in())
      return 0;

    CORBA::String_var name = ai->name();
    if (!(char*)name)
      return 0;

    to_add = omniURI::buildURI("giop:tcp", name, ep_port);
  }
  else if (omni::strMatch(publish_spec, "hostname")) {
    char self[OMNIORB_HOSTNAME_MAX];

    if (gethostname(&self[0],OMNIORB_HOSTNAME_MAX) == RC_SOCKET_ERROR)
      return 0;

    to_add = omniURI::buildURI("giop:tcp", self, ep_port);
  }
  else if (omni::strMatch(publish_spec, "fqdn")) {
    char self[OMNIORB_HOSTNAME_MAX];

    if (gethostname(&self[0],OMNIORB_HOSTNAME_MAX) == RC_SOCKET_ERROR)
      return 0;

    LibcWrapper::AddrInfo_var ai = LibcWrapper::getAddrInfo(self, 0);
    if (!ai.in())
      return 0;

    char* name = ai->name();
    if (name && !(omni::strMatch(name, "localhost") ||
		  omni::strMatch(name, "localhost.localdomain"))) {
      to_add = omniURI::buildURI("giop:tcp", name, ep_port);
    }
    else {
      to_add = omniURI::buildURI("giop:tcp", self, ep_port);
    }
  }
  else {
    // Don't understand the spec.
    return 0;
  }

  if (!omniObjAdapter::endpointInList(to_add, published_eps)) {
    if (omniORB::trace(20)) {
      omniORB::logger l;
      l << "Publish endpoint '" << to_add << "'\n";
    }
    giopEndpoint::addToIOR(to_add);
    published_eps.length(published_eps.length() + 1);
    published_eps[published_eps.length() - 1] = to_add._retn();
  }
  return 1;
}

CORBA::Boolean
tcpEndpoint::publish(const orbServer::PublishSpecs& publish_specs,
		     CORBA::Boolean 	      	    all_specs,
		     CORBA::Boolean 	      	    all_eps,
		     orbServer::EndpointList& 	    published_eps)
{
  CORBA::ULong i, j;
  CORBA::Boolean result = 0;

  if (publish_specs.length() == 1 &&
      omni::strMatch(publish_specs[0], "fail-if-multiple") &&
      pd_addresses.length() > 1) {

    omniORB::logs(1, "TCP endpoint has multiple addresses. "
		  "You must choose one to listen on.");
    OMNIORB_THROW(INITIALIZE, INITIALIZE_TransportError,
		  CORBA::COMPLETED_NO);
  }
  for (i=0; i < pd_addresses.length(); ++i) {

    CORBA::Boolean ok = 0;
    
    for (j=0; j < publish_specs.length(); ++j) {
      if (omniORB::trace(25)) {
	omniORB::logger l;
	l << "Try to publish '" << publish_specs[j]
	  << "' for endpoint " << pd_addresses[i] << "\n";
      }
      ok = publish_one(publish_specs[j], pd_addresses[i], no_publish(),
		       published_eps);
      result |= ok;

      if (ok && !all_specs)
	break;
    }
    if (result && !all_eps)
      break;
  }
  return result;
}


/////////////////////////////////////////////////////////////////////////
CORBA::Boolean
tcpEndpoint::Bind() {

  OMNIORB_ASSERT(pd_socket == RC_INVALID_SOCKET);

  CORBA::String_var host;
  CORBA::UShort     port_min;
  CORBA::UShort     port_max;

  host = omniURI::extractHostPortRange(pd_address_param, port_min, port_max);
  OMNIORB_ASSERT((const char*)host);

  char*         bound_host;
  CORBA::UShort bound_port;

  pd_socket = tcpSocket::Bind(host,
			      port_min,
			      port_max,
			      type(),
			      bound_host,
			      bound_port,
			      pd_addresses);

  if (pd_socket == RC_INVALID_SOCKET)
    return 0;

  pd_address.host = bound_host;
  pd_address.port = bound_port;
  
  // Never block in accept
  tcpSocket::setNonBlocking(pd_socket);

  // Add the socket to our SocketCollection.
  addSocket(this);

  return 1;
}

/////////////////////////////////////////////////////////////////////////
void
tcpEndpoint::Poke() {

  tcpAddress* target = new tcpAddress(pd_address);

  pd_poked = 1;
  if (!target->Poke()) {
    if (omniORB::trace(5)) {
      omniORB::logger log;
      log << "Warning: fail to connect to myself ("
	  << (const char*) pd_addresses[0] << ") via tcp.\n";
    }
  }
  // Wake up the SocketCollection in case the connect did not work and
  // it is idle and blocked with no timeout.
  wakeUp();

  delete target;
}

/////////////////////////////////////////////////////////////////////////
void
tcpEndpoint::Shutdown() {
  SHUTDOWNSOCKET(pd_socket);
  removeSocket(this);
  decrRefCount();
  omniORB::logs(20, "TCP endpoint shut down.");
}

/////////////////////////////////////////////////////////////////////////
giopConnection*
tcpEndpoint::AcceptAndMonitor(giopConnection::notifyReadable_t func,
			      void* cookie) {

  OMNIORB_ASSERT(pd_socket != RC_INVALID_SOCKET);

  pd_callback_func = func;
  pd_callback_cookie = cookie;
  setSelectable(1,0);

  while (1) {
    pd_new_conn_socket = RC_INVALID_SOCKET;
    if (!Select()) break;
    if (pd_new_conn_socket != RC_INVALID_SOCKET) {
      tcpConnection* nc = new tcpConnection(pd_new_conn_socket, this);

      ConnectionInfo::set(ConnectionInfo::ACCEPTED_CONNECTION, 0,
                          nc->peeraddress());
      return nc;
    }
    if (pd_poked)
      return 0;
  }
  return 0;
}

/////////////////////////////////////////////////////////////////////////
CORBA::Boolean
tcpEndpoint::notifyReadable(SocketHolder* sh) {

  if (sh == (SocketHolder*)this) {
    // New connection
    SocketHandle_t sock;
again:
    sock = ::accept(pd_socket,0,0);
    if (sock == RC_SOCKET_ERROR) {
      if (ERRNO == RC_EBADF) {
        omniORB::logs(20, "accept() returned EBADF, unable to continue");
        return 0;
      }
      else if (ERRNO == RC_EINTR) {
        omniORB::logs(20, "accept() returned EINTR, trying again");
        goto again;
      }
#ifdef UnixArchitecture
      else if (ERRNO == RC_EAGAIN) {
        omniORB::logs(20, "accept() returned EAGAIN, will try later");
      }
#endif
      if (omniORB::trace(20)) {
        omniORB::logger log;
        log << "accept() failed with unknown error " << ERRNO << "\n";
      }
    }
    else {
#if defined(__vxWorks__)
      // vxWorks "forgets" socket options
      static const int valtrue = 1;
      if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                     (char*)&valtrue, sizeof(valtrue)) == ERROR) {
	return 0;
      }
#endif
      // On some platforms, the new socket inherits the non-blocking
      // setting from the listening socket, so we set it blocking here
      // just to be sure.
      tcpSocket::setBlocking(sock);

      pd_new_conn_socket = sock;
    }
    setSelectable(1,0);
    return 1;
  }
  else {
    // Existing connection
    pd_callback_func(pd_callback_cookie,(tcpConnection*)sh);
    return 1;
  }
}

OMNI_NAMESPACE_END(omni)
