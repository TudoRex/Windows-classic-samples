// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (C) Microsoft Corporation.  All Rights Reserved.
//
// Module:
//      iocpserverex.cpp
//
// Abstract:
//      This program is a Winsock echo server program that demonstrates the usage
//      of AcceptEx with IOCP. The AcceptEx function accepts a new connection, 
//      returns the local and remote address, and receives the first block of data 
//      sent by the client application. The design of this program is based on that 
//      in the iocpserver.cpp. But it uses overlapped AcceptEx on the IOCP also. 
//      AcceptEx allows data to be "returned" from an accepted connection.
//
//      Another point worth noting is that the Win32 API CreateThread() does not 
//      initialize the C Runtime and therefore, C runtime functions such as 
//      printf() have been avoid or rewritten (see myprintf()) to use just Win32 APIs.
//
//
//  Usage:
//      Start the server and wait for connections on port 6001
//          iocpserverex -e:6001
//
//  Build:
//      Use the headers and libs from the April98 Platform SDK or later.
//      Link with ws2_32.lib and mswsock.lib
//      
//
//

#pragma warning (disable:4127)
                            
#ifdef _IA64_
	#pragma warning(disable:4267)
#endif 

#
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>

#include <mutex>

#include "iocpserver.h"
#include "util.h"

char *g_Port = DEFAULT_PORT;
BOOL g_bEndServer = FALSE;			// set to TRUE on CTRL-C
BOOL g_bRestart = TRUE;				// set to TRUE to CTRL-BRK
BOOL g_bVerbose = FALSE;
HANDLE g_hIOCP = INVALID_HANDLE_VALUE;
HANDLE g_ThreadHandles[MAX_WORKER_THREAD];
WSAEVENT g_hCleanupEvent[1];

std::mutex g_Mutex;
std::unique_lock<std::mutex> g_CtxtListLock(g_Mutex, std::defer_lock);

void __cdecl main (int argc, char *argv[])	{

    SYSTEM_INFO systemInfo;
	WSADATA wsaData;
	DWORD dwThreadCount = 0;
	int nRet = 0;

    g_ThreadHandles[0] = (HANDLE)WSA_INVALID_EVENT;

	for( int i = 0; i < MAX_WORKER_THREAD; i++ ) {
		g_ThreadHandles[i] = INVALID_HANDLE_VALUE;
	}

	if( !user_ValidOptions(argc, argv) )
		return;
	
	if( !SetConsoleCtrlHandler(user_CtrlHandler, TRUE) ) {
		myprintf("SetConsoleCtrlHandler() failed to install console handler: %d\n", 
				GetLastError());
		return;
	}
	

	GetSystemInfo(&systemInfo);
	dwThreadCount = systemInfo.dwNumberOfProcessors * 2;

	if(WSA_INVALID_EVENT == (g_hCleanupEvent[0] = WSACreateEvent()))
	{
		myprintf("WSACreateEvent() failed: %d\n", WSAGetLastError());
		return;
	}

	if( (nRet = WSAStartup(0x202, &wsaData)) != 0 ) {
		myprintf("WSAStartup() failed: %d\n",nRet);
		SetConsoleCtrlHandler(user_CtrlHandler, FALSE);
		if(g_hCleanupEvent[0] != WSA_INVALID_EVENT) {
			WSACloseEvent(g_hCleanupEvent[0]);
			g_hCleanupEvent[0] = WSA_INVALID_EVENT;
		}
		return;
	}
	/* todo³õÊ¼»¯Ê§°ÜCriticalSection
    __try
    {
        InitializeCriticalSection(&g_CriticalSection);
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        myprintf("InitializeCriticalSection raised an exception.\n");
		SetConsoleCtrlHandler(CtrlHandler, FALSE);
		if(g_hCleanupEvent[0] != WSA_INVALID_EVENT) {
			WSACloseEvent(g_hCleanupEvent[0]);
			g_hCleanupEvent[0] = WSA_INVALID_EVENT;
		}
		return;
    }
	*/

	while( g_bRestart ) {
		g_bRestart = FALSE;
		g_bEndServer = FALSE;
		WSAResetEvent(g_hCleanupEvent[0]);

		__try	{

            //
			// notice that we will create more worker threads (dwThreadCount) than 
			// the thread concurrency limit on the IOCP.
			//
			g_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
            if( g_hIOCP == NULL ) {
				myprintf("CreateIoCompletionPort() failed to create I/O completion port: %d\n", 
						GetLastError());
				__leave;
			}

			for( DWORD dwCPU=0; dwCPU<dwThreadCount; dwCPU++ ) {

				//
				// Create worker threads to service the overlapped I/O requests.  The decision
				// to create 2 worker threads per CPU in the system is a heuristic.  Also,
				// note that thread handles are closed right away, because we will not need them
				// and the worker threads will continue to execute.
				//
				HANDLE  hThread;
				DWORD   dwThreadId;

				hThread = CreateThread(NULL, 0, WorkerThreadNative, g_hIOCP, 0, &dwThreadId);
				if( hThread == NULL ) {
					myprintf("CreateThread() failed to create worker thread: %d\n", 
						   GetLastError());
					__leave;
				}
				g_ThreadHandles[dwCPU] = hThread;
				hThread = INVALID_HANDLE_VALUE;
			}

			if( !user_CreateListenSocket() )
				__leave;

			if( !user_UpdateIOCPWithAllocatedAcceptSocket(TRUE) )
				__leave;

			WSAWaitForMultipleEvents(1, g_hCleanupEvent, TRUE, WSA_INFINITE, FALSE);
		}

		__finally	{

			g_bEndServer = TRUE;

			//
			// Cause worker threads to exit
			//
			if( g_hIOCP ) {
				for( DWORD i = 0; i < dwThreadCount; i++ )
					PostQueuedCompletionStatus(g_hIOCP, 0, 0, NULL);
			}

            //
			// Make sure worker threads exits.
			// fix with std::future
			if( WAIT_OBJECT_0 != WaitForMultipleObjects(dwThreadCount,  g_ThreadHandles, TRUE, 1000) )
				myprintf("WaitForMultipleObjects() failed: %d\n", GetLastError());
			else
				for( DWORD i=0; i<dwThreadCount; i++ ) {
					if( g_ThreadHandles[i] != INVALID_HANDLE_VALUE )
						CloseHandle(g_ThreadHandles[i]);
					g_ThreadHandles[i] = INVALID_HANDLE_VALUE;
				}

			if( g_sdListen != INVALID_SOCKET ) {
				closesocket(g_sdListen);                                
				g_sdListen = INVALID_SOCKET;
			}

			if( g_pCtxtListenSocket ) {
				while( !HasOverlappedIoCompleted((LPOVERLAPPED)&g_pCtxtListenSocket->pIOContext->Overlapped) )
					Sleep(0);

				if( g_pCtxtListenSocket->pIOContext->SocketAccept != INVALID_SOCKET )
					closesocket(g_pCtxtListenSocket->pIOContext->SocketAccept);
				g_pCtxtListenSocket->pIOContext->SocketAccept = INVALID_SOCKET;

                //
				// We know there is only one overlapped I/O on the listening socket
				//
				if( g_pCtxtListenSocket->pIOContext )
					xfree(g_pCtxtListenSocket->pIOContext);

				if( g_pCtxtListenSocket )
					xfree(g_pCtxtListenSocket);
				g_pCtxtListenSocket = NULL;
			}

			CtxtListFree();

			if( g_hIOCP ) {
				CloseHandle(g_hIOCP);
				g_hIOCP = NULL;
			}
		} //finally

		if( g_bRestart ) {
			myprintf("\niocpserverex is restarting...\n");
		} else
			myprintf("\niocpserverex is exiting...\n");

	} //while (g_bRestart)

	//DeleteCriticalSection(&g_CriticalSection);
	if(g_hCleanupEvent[0] != WSA_INVALID_EVENT) {
		WSACloseEvent(g_hCleanupEvent[0]);
		g_hCleanupEvent[0] = WSA_INVALID_EVENT;
	}
	WSACleanup();
	SetConsoleCtrlHandler(user_CtrlHandler, FALSE);
} //main

//
//  Just validate the command line options.
//
BOOL user_ValidOptions(int argc, char *argv[]) {
	BOOL bRet = TRUE;

	for( int i=1; i<argc; i++ ) {
		if( (argv[i][0] =='-') || (argv[i][0] == '/') ) {
			switch( tolower(argv[i][1]) ) {
			case 'e':
				if( strlen(argv[i]) > 3 )
					g_Port = &argv[i][3];
				break;

			case 'v':
				g_bVerbose = TRUE;
				break;

			case '?':
				myprintf("Usage:\n  iocpserver [-p:port] [-v] [-?]\n");
				myprintf("  -e:port\tSpecify echoing port number\n");        
				myprintf("  -v\t\tVerbose\n");        
				myprintf("  -?\t\tDisplay this help\n");
				bRet = FALSE;
				break;

			default:
				myprintf("Unknown options flag %s\n", argv[i]);
				bRet = FALSE;
				break;
			}
		}
	}   

	return(bRet);
}

//
//  Intercept CTRL-C or CTRL-BRK events and cause the server to initiate shutdown.
//  CTRL-BRK resets the restart flag, and after cleanup the server restarts.
//
BOOL WINAPI user_CtrlHandler (DWORD dwEvent)	{

	switch( dwEvent ) {
	case CTRL_BREAK_EVENT: 
		g_bRestart = TRUE;
	case CTRL_C_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
	case CTRL_CLOSE_EVENT:
		if( g_bVerbose )
			myprintf("CtrlHandler: closing listening socket\n");

		g_bEndServer = TRUE;

		WSASetEvent(g_hCleanupEvent[0]);
		break;

	default:
		//
		// unknown type--better pass it on.
		//

		return(FALSE);
	}
	return(TRUE);
}











//
//  Close down a connection with a client.  This involves closing the socket (when 
//  initiated as a result of a CTRL-C the socket closure is not graceful).  Additionally, 
//  any context data associated with that socket is free'd.
//
VOID CloseClient (PPER_SOCKET_CONTEXT lpPerSocketContext, BOOL bGraceful)	{

	__try
    {
		g_CtxtListLock.lock();
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        myprintf("EnterCriticalSection raised an exception.\n");
        return;
    }

	if( lpPerSocketContext ) {
		if( g_bVerbose )
			myprintf("CloseClient: Socket(%d) connection closing (graceful=%s)\n",
				   lpPerSocketContext->Socket, (bGraceful?"TRUE":"FALSE"));
		if( !bGraceful ) {

			//
			// force the subsequent closesocket to be abortative.
			//
			LINGER  lingerStruct;

			lingerStruct.l_onoff = 1;
			lingerStruct.l_linger = 0;
			setsockopt(lpPerSocketContext->Socket, SOL_SOCKET, SO_LINGER,
					   (char *)&lingerStruct, sizeof(lingerStruct) );
		}
		if( lpPerSocketContext->pIOContext->SocketAccept != INVALID_SOCKET ) {
			closesocket(lpPerSocketContext->pIOContext->SocketAccept);
			lpPerSocketContext->pIOContext->SocketAccept = INVALID_SOCKET;
		};

		closesocket(lpPerSocketContext->Socket);
		lpPerSocketContext->Socket = INVALID_SOCKET;
		CtxtListDeleteFrom(lpPerSocketContext);
		lpPerSocketContext = NULL;
	} else {
		myprintf("CloseClient: lpPerSocketContext is NULL\n");
	}

	g_CtxtListLock.unlock();

	return;    
} 






