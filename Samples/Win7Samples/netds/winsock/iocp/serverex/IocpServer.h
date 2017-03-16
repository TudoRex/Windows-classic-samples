// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (C) 1998 - 2000  Microsoft Corporation.  All Rights Reserved.
//
// Module:
//      iocpserver.h
//

#ifndef IOCPSERVER_H
#define IOCPSERVER_H


#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif


#include <winsock2.h>
#include <mswsock.h>
#include <Ws2tcpip.h>
#include <mswsock.h>

#define DEFAULT_PORT        "15001"
#define MAX_BUFF_SIZE       8192
#define MAX_WORKER_THREAD   16

typedef enum _IO_OPERATION {
    ClientIoAccept,
    ClientIoRead,
    ClientIoWrite
} IO_OPERATION, *PIO_OPERATION;

//
// data to be associated for every I/O operation on a socket
//
typedef struct _PER_IO_CONTEXT {
    WSAOVERLAPPED               Overlapped;
    char                        Buffer[MAX_BUFF_SIZE];
    WSABUF                      wsabuf;
    int                         nTotalBytes;
    int                         nSentBytes;
    IO_OPERATION                IOOperation;
    SOCKET                      SocketAccept; 

    struct _PER_IO_CONTEXT      *pIOContextForward;
} PER_IO_CONTEXT, *PPER_IO_CONTEXT;

//
// For AcceptEx, the IOCP key is the PER_SOCKET_CONTEXT for the listening socket,
// so we need to another field SocketAccept in PER_IO_CONTEXT. When the outstanding
// AcceptEx completes, this field is our connection socket handle.
//

//
// data to be associated with every socket added to the IOCP
//
typedef struct _PER_SOCKET_CONTEXT {
    SOCKET                      Socket;

    LPFN_ACCEPTEX               fnAcceptEx;

	//
    //linked list for all outstanding i/o on the socket
	//
    PPER_IO_CONTEXT             pIOContext;  
    struct _PER_SOCKET_CONTEXT  *pCtxtBack; 
    struct _PER_SOCKET_CONTEXT  *pCtxtForward;
} PER_SOCKET_CONTEXT, *PPER_SOCKET_CONTEXT;

BOOL user_ValidOptions(int argc, char *argv[]);

BOOL WINAPI user_CtrlHandler(
    DWORD dwEvent
    );

BOOL user_CreateListenSocket(void);

BOOL user_UpdateIOCPWithAllocatedAcceptSocket(
	SOCKET sd,
	HANDLE iocp,
    BOOL fUpdateIOCP
    );

DWORD WINAPI WorkerThreadNative (
    LPVOID WorkContext
    );

PPER_SOCKET_CONTEXT user_UpdateCompletionPort(
    SOCKET s,
	HANDLE iocp,
    IO_OPERATION ClientIo,
    BOOL bAddToList
    );
//
// bAddToList is FALSE for listening socket, and TRUE for connection sockets.
// As we maintain the context for listening socket in a global structure, we
// don't need to add it to the list.
//

VOID CloseClient (
    PPER_SOCKET_CONTEXT lpPerSocketContext,
    BOOL bGraceful
    );

PPER_SOCKET_CONTEXT CtxtAllocate(
    SOCKET s, 
    IO_OPERATION ClientIO
    );

VOID CtxtListFree(
    );

VOID CtxtListAddTo (
    PPER_SOCKET_CONTEXT lpPerSocketContext
    );

VOID CtxtListDeleteFrom(
    PPER_SOCKET_CONTEXT lpPerSocketContext
    );

// IocpSocket.cpp
SOCKET user_CreateWSASocket(void);
BOOL user_CreateListenSocket(void);
bool LoadExtensionRoutineAcceptEx(SOCKET sockListen, LPFN_ACCEPTEX* ppfn);

// global
extern LPFN_ACCEPTEX g_AcceptEx;
extern char *g_Port;
extern BOOL g_bEndServer;
extern HANDLE g_hIOCP;
extern SOCKET g_sdListen;
extern BOOL g_bRestart;
extern BOOL g_bVerbose;
extern HANDLE g_ThreadHandles[MAX_WORKER_THREAD];
extern WSAEVENT g_hCleanupEvent[1];

extern PPER_SOCKET_CONTEXT g_pCtxtListenSocket;
extern PPER_SOCKET_CONTEXT g_pCtxtList;

#include <mutex>
extern std::mutex g_Mutex;
extern std::unique_lock<std::mutex> g_CtxtListLock;



#endif
