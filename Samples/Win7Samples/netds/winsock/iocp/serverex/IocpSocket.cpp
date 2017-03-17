#include "IocpServer.h"
#include "util.h"
#include <winsock2.h>
#include <iostream>

SOCKET g_sdListen = INVALID_SOCKET;
//
// Create a socket with all the socket options we need, namely disable buffering
// and set linger.
//


static void SetSocketSendBuf(SOCKET hSock, int nBufSize)
{

}

SOCKET user_CreateWSASocket(void) {
	int nRet = 0;
	int nZero = 0;
	SOCKET sdSocket = INVALID_SOCKET;

	sdSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_IP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (sdSocket == INVALID_SOCKET) {
		myprintf("WSASocket(sdSocket) failed: %d\n", WSAGetLastError());
		return(sdSocket);
	}

	//
	// Disable send buffering on the socket.  Setting SO_SNDBUF
	// to 0 causes winsock to stop buffering sends and perform
	// sends directly from our buffers, thereby save one memory copy.
	//
	// However, this does prevent the socket from ever filling the
	// send pipeline. This can lead to packets being sent that are
	// not full (i.e. the overhead of the IP and TCP headers is 
	// great compared to the amount of data being carried).
	//
	// Disabling the send buffer has less serious repercussions 
	// than disabling the receive buffer.
	//
	nZero = 0;
	nRet = setsockopt(sdSocket, SOL_SOCKET, SO_SNDBUF, (char *)&nZero, sizeof(nZero));
	if (nRet == SOCKET_ERROR) {
		myprintf("setsockopt(SNDBUF) failed: %d\n", WSAGetLastError());
		return(sdSocket);
	}

	//
	// Don't disable receive buffering. This will cause poor network
	// performance since if no receive is posted and no receive buffers,
	// the TCP stack will set the window size to zero and the peer will
	// no longer be allowed to send data.
	//

	// 
	// Do not set a linger value...especially don't set it to an abortive
	// close. If you set abortive close and there happens to be a bit of
	// data remaining to be transfered (or data that has not been 
	// acknowledged by the peer), the connection will be forcefully reset
	// and will lead to a loss of data (i.e. the peer won't get the last
	// bit of data). This is BAD. If you are worried about malicious
	// clients connecting and then not sending or receiving, the server
	// should maintain a timer on each connection. If after some point,
	// the server deems a connection is "stale" it can then set linger
	// to be abortive and close the connection.
	//

	/*
	LINGER lingerStruct;

	lingerStruct.l_onoff = 1;
	lingerStruct.l_linger = 0;
	nRet = setsockopt(sdSocket, SOL_SOCKET, SO_LINGER,
	(char *)&lingerStruct, sizeof(lingerStruct));
	if( nRet == SOCKET_ERROR ) {
	myprintf("setsockopt(SO_LINGER) failed: %d\n", WSAGetLastError());
	return(sdSocket);
	}
	*/

	return(sdSocket);
}

//
//  Create a listening socket, bind, and set up its listening backlog.
//
BOOL user_CreateListenSocket(void) {

	int nRet = 0;
	LINGER lingerStruct;
	struct addrinfo hints = { 0 };
	struct addrinfo *addrlocal = NULL;

	lingerStruct.l_onoff = 1;
	lingerStruct.l_linger = 0;

	//
	// Resolve the interface
	//
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_IP;

	if (getaddrinfo(NULL, g_Port, &hints, &addrlocal) != 0) {
		myprintf("getaddrinfo() failed with error %d\n", WSAGetLastError());
		return(FALSE);
	}

	if (addrlocal == NULL) {
		myprintf("getaddrinfo() failed to resolve/convert the interface\n");
		return(FALSE);
	}
	else
	{
		struct addrinfo* ptr = addrlocal;
		for (; ptr; ptr = ptr->ai_next)
		{
			struct sockaddr_in* pSockAddr = (sockaddr_in*)ptr->ai_addr;
			myprintf("listen on {%s}addr[%s]port[%d]\n", ptr->ai_canonname, inet_ntoa(pSockAddr->sin_addr), ntohs(pSockAddr->sin_port));
		}
	}

	g_sdListen = user_CreateWSASocket();
	if (g_sdListen == INVALID_SOCKET) {
		freeaddrinfo(addrlocal);
		return(FALSE);
	}

	nRet = bind(g_sdListen, addrlocal->ai_addr, (int)addrlocal->ai_addrlen);
	if (nRet == SOCKET_ERROR) {
		myprintf("bind() failed: %d\n", WSAGetLastError());
		freeaddrinfo(addrlocal);
		return(FALSE);
	}
	myprintf("bind() ok.\n");

	nRet = listen(g_sdListen, SOMAXCONN);
	if (nRet == SOCKET_ERROR) {
		myprintf("listen() failed: %d\n", WSAGetLastError());
		freeaddrinfo(addrlocal);
		return(FALSE);
	}
	myprintf("listen() ok.\n");

	freeaddrinfo(addrlocal);

	return(TRUE);
}

//
// Create a socket and invoke AcceptEx.  Only the original call to to this
// function needs to be added to the IOCP.
//
// If the expected behaviour of connecting client applications is to NOT
// send data right away, then only posting one AcceptEx can cause connection
// attempts to be refused if a client connects without sending some initial
// data (notice that the associated iocpclient does not operate this way 
// but instead makes a connection and starts sending data write away).  
// This is because the IOCP packet does not get delivered without the initial
// data (as implemented in this sample) thus preventing the worker thread 
// from posting another AcceptEx and eventually the backlog value set in 
// listen() will be exceeded if clients continue to try to connect.
//
// One technique to address this situation is to simply cause AcceptEx
// to return right away upon accepting a connection without returning any
// data.  This can be done by setting dwReceiveDataLength=0 when calling AcceptEx.
//
// Another technique to address this situation is to post multiple calls 
// to AcceptEx.  Posting multiple calls to AcceptEx is similar in concept to 
// increasing the backlog value in listen(), though posting AcceptEx is 
// dynamic (i.e. during the course of running your application you can adjust 
// the number of AcceptEx calls you post).  It is important however to keep
// your backlog value in listen() high in your server to ensure that the 
// stack can accept connections even if your application does not get enough 
// CPU cycles to repost another AcceptEx under stress conditions.
// 
// This sample implements neither of these techniques and is therefore
// susceptible to the behaviour described above.
//
BOOL user_UpdateIOCPWithAllocatedAcceptSocket(SOCKET sockListen,HANDLE iocp, BOOL fUpdateIOCP) {

	int nRet = 0;
	DWORD dwRecvNumBytes = 0;
	
	//
	//The context for listening socket uses the SockAccept member to store the
	//socket for client connection. 
	//
	if (fUpdateIOCP) {
		g_pCtxtListenSocket = user_UpdateCompletionPort(sockListen, iocp, ClientIoAccept, FALSE);
		if (g_pCtxtListenSocket == NULL) {
			myprintf("failed to update listen socket to IOCP\n");
			return(FALSE);
		}
		myprintf("update listen socket to IOCP\n");
//#error "Do not load AcceptEx in each thread."
		g_pCtxtListenSocket->fnAcceptEx = g_AcceptEx;
		
	}

	SOCKET sockAccept = user_CreateWSASocket();
	if (sockAccept == INVALID_SOCKET) {
		myprintf("failed to create new accept socket\n");
		return(FALSE);
	}
	g_pCtxtListenSocket->pIOContext->SocketAccept = sockAccept;

	//
	// pay close attention to these parameters and buffer lengths
	//										
	nRet = g_pCtxtListenSocket->fnAcceptEx(sockListen, sockAccept,
		(LPVOID)(g_pCtxtListenSocket->pIOContext->Buffer),
		MAX_BUFF_SIZE - (2 * (sizeof(SOCKADDR_STORAGE) + 16)),
		sizeof(SOCKADDR_STORAGE) + 16, sizeof(SOCKADDR_STORAGE) + 16,
		&dwRecvNumBytes,
		(LPOVERLAPPED)&(g_pCtxtListenSocket->pIOContext->Overlapped));
	if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError())) {
		myprintf("AcceptEx() failed: %d\n", WSAGetLastError());
		return(FALSE);
	}

	return(TRUE);
}

bool LoadExtensionRoutineAcceptEx(SOCKET sockListen, LPFN_ACCEPTEX* ppfn)
{
	//
	// GUID to Microsoft specific extensions
	//
	GUID acceptex_guid = WSAID_ACCEPTEX;
	DWORD bytes = 0;

	// Load the AcceptEx extension function from the provider for this socket
	int nRet = WSAIoctl(
		sockListen,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&acceptex_guid,
		sizeof(acceptex_guid),
		ppfn,
		sizeof(*ppfn),
		&bytes,
		NULL,
		NULL
		);
	if (nRet == SOCKET_ERROR)
	{
		myprintf("failed to load AcceptEx: %d\n", WSAGetLastError());
		return (FALSE);
	}
	myprintf("LoadExtensionRoutineAcceptEx() @%p\n", *ppfn);
	return TRUE;
}

//
//  Allocate a context structures for the socket and add the socket to the IOCP.  
//  Additionally, add the context structure to the global list of context structures.
//
PPER_SOCKET_CONTEXT user_UpdateCompletionPort(SOCKET sd, HANDLE iocp, IO_OPERATION ClientIo,
	BOOL bAddToList)	{

	PPER_SOCKET_CONTEXT lpPerSocketContext;

	lpPerSocketContext = CtxtAllocate(sd, ClientIo);
	if (lpPerSocketContext == NULL)
		return(NULL);

	HANDLE _iocp = CreateIoCompletionPort((HANDLE)sd, iocp, (DWORD_PTR)lpPerSocketContext, 0);
	if (_iocp == NULL) {
		myprintf("CreateIoCompletionPort() failed: %d\n", GetLastError());
		if (lpPerSocketContext->pIOContext)
			xfree(lpPerSocketContext->pIOContext);
		xfree(lpPerSocketContext);
		return(NULL);
	}

	//
	//The listening socket context (bAddToList is FALSE) is not added to the list.
	//All other socket contexts are added to the list.
	//
	if (bAddToList) CtxtListAddTo(lpPerSocketContext);

	if (g_bVerbose)
		myprintf("UpdateCompletionPort: Socket(%p) added to IOCP\n", lpPerSocketContext->Socket);

	return(lpPerSocketContext);
}