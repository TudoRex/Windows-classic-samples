#include <winsock2.h>
#include <mswsock.h>
#include <Ws2tcpip.h>
#include <strsafe.h>

#include "IocpServer.h"
#include "util.h"



PPER_SOCKET_CONTEXT g_pCtxtListenSocket = NULL;
PPER_SOCKET_CONTEXT g_pCtxtList = NULL;		// linked list of context info structures
// maintained to allow the the cleanup 
// handler to cleanly close all sockets and 
// free resources.

//
// Worker thread that handles all I/O requests on any socket handle added to the IOCP.
//
DWORD WINAPI WorkerThreadNative(LPVOID WorkThreadContext)	{

	HANDLE hIOCP = (HANDLE)WorkThreadContext;
	BOOL bSuccess = FALSE;
	int nRet = 0;
	LPWSAOVERLAPPED lpOverlapped = NULL;
	PPER_SOCKET_CONTEXT lpPerSocketContext = NULL;
	PPER_SOCKET_CONTEXT lpAcceptSocketContext = NULL;
	PPER_IO_CONTEXT lpIOContext = NULL;
	WSABUF buffRecv;
	WSABUF buffSend;
	DWORD dwRecvNumBytes = 0;
	DWORD dwSendNumBytes = 0;
	DWORD dwFlags = 0;
	DWORD dwIoSize = 0;
	HRESULT hRet;

	while (TRUE) {

		//
		// continually loop to service io completion packets
		//
		bSuccess = GetQueuedCompletionStatus(
			hIOCP,
			&dwIoSize,
			(PDWORD_PTR)&lpPerSocketContext,
			(LPOVERLAPPED *)&lpOverlapped,
			INFINITE
			);
		if (!bSuccess)
			myprintf("GetQueuedCompletionStatus() failed: %d\n", GetLastError());

		if (lpPerSocketContext == NULL) {

			//
			// CTRL-C handler used PostQueuedCompletionStatus to post an I/O packet with
			// a NULL CompletionKey (or if we get one for any reason).  It is time to exit.
			//
			return(0);
		}

		if (g_bEndServer) {

			//
			// main thread will do all cleanup needed - see finally block
			//
			return(0);
		}

		lpIOContext = (PPER_IO_CONTEXT)lpOverlapped;

		//
		//We should never skip the loop and not post another AcceptEx if the current
		//completion packet is for previous AcceptEx
		//
		if (lpIOContext->IOOperation != ClientIoAccept) {
			if (!bSuccess || (bSuccess && (0 == dwIoSize))) {

				//
				// client connection dropped, continue to service remaining (and possibly 
				// new) client connections
				//
				CloseClient(lpPerSocketContext, FALSE);
				continue;
			}
		}

		//
		// determine what type of IO packet has completed by checking the PER_IO_CONTEXT 
		// associated with this socket.  This will determine what action to take.
		//
		switch (lpIOContext->IOOperation) {
		case ClientIoAccept:

			//
			// When the AcceptEx function returns, the socket sAcceptSocket is 
			// in the default state for a connected socket. The socket sAcceptSocket 
			// does not inherit the properties of the socket associated with 
			// sListenSocket parameter until SO_UPDATE_ACCEPT_CONTEXT is set on 
			// the socket. Use the setsockopt function to set the SO_UPDATE_ACCEPT_CONTEXT 
			// option, specifying sAcceptSocket as the socket handle and sListenSocket 
			// as the option value. 
			//
			nRet = setsockopt(
				lpPerSocketContext->pIOContext->SocketAccept,
				SOL_SOCKET,
				SO_UPDATE_ACCEPT_CONTEXT,
				(char *)&g_sdListen,
				sizeof(g_sdListen)
				);

			if (nRet == SOCKET_ERROR) {

				//
				//just warn user here.
				//
				myprintf("setsockopt(SO_UPDATE_ACCEPT_CONTEXT) failed to update accept socket\n");
				WSASetEvent(g_hCleanupEvent[0]);
				return(0);
			}

			lpAcceptSocketContext = user_UpdateCompletionPort(
				lpPerSocketContext->pIOContext->SocketAccept,
				ClientIoAccept, TRUE);

			if (lpAcceptSocketContext == NULL) {

				//
				//just warn user here.
				//
				myprintf("failed to update accept socket to IOCP\n");
				WSASetEvent(g_hCleanupEvent[0]);
				return(0);
			}

			if (dwIoSize) {
				lpAcceptSocketContext->pIOContext->IOOperation = ClientIoWrite;
				lpAcceptSocketContext->pIOContext->nTotalBytes = dwIoSize;
				lpAcceptSocketContext->pIOContext->nSentBytes = 0;
				lpAcceptSocketContext->pIOContext->wsabuf.len = dwIoSize;
				hRet = StringCbCopyN(lpAcceptSocketContext->pIOContext->Buffer,
					MAX_BUFF_SIZE,
					lpPerSocketContext->pIOContext->Buffer,
					sizeof(lpPerSocketContext->pIOContext->Buffer)
					);
				lpAcceptSocketContext->pIOContext->wsabuf.buf = lpAcceptSocketContext->pIOContext->Buffer;

				nRet = WSASend(
					lpPerSocketContext->pIOContext->SocketAccept,
					&lpAcceptSocketContext->pIOContext->wsabuf, 1,
					&dwSendNumBytes,
					0,
					&(lpAcceptSocketContext->pIOContext->Overlapped), NULL);

				if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError())) {
					myprintf("WSASend() failed: %d\n", WSAGetLastError());
					CloseClient(lpAcceptSocketContext, FALSE);
				}
				else if (g_bVerbose) {
					myprintf("WorkerThread %d: Socket(%d) AcceptEx completed (%d bytes), Send posted\n",
						GetCurrentThreadId(), lpPerSocketContext->Socket, dwIoSize);
				}
			}
			else {

				//
				// AcceptEx completes but doesn't read any data so we need to post
				// an outstanding overlapped read.
				//
				lpAcceptSocketContext->pIOContext->IOOperation = ClientIoRead;
				dwRecvNumBytes = 0;
				dwFlags = 0;
				buffRecv.buf = lpAcceptSocketContext->pIOContext->Buffer,
					buffRecv.len = MAX_BUFF_SIZE;
				nRet = WSARecv(
					lpAcceptSocketContext->Socket,
					&buffRecv, 1,
					&dwRecvNumBytes,
					&dwFlags,
					&lpAcceptSocketContext->pIOContext->Overlapped, NULL);
				if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError())) {
					myprintf("WSARecv() failed: %d\n", WSAGetLastError());
					CloseClient(lpAcceptSocketContext, FALSE);
				}
			}

			//
			//Time to post another outstanding AcceptEx
			//
			if (!user_CreateAcceptSocket(FALSE)) {
				myprintf("Please shut down and reboot the server.\n");
				WSASetEvent(g_hCleanupEvent[0]);
				return(0);
			}
			break;


		case ClientIoRead:

			//
			// a read operation has completed, post a write operation to echo the
			// data back to the client using the same data buffer.
			//
			lpIOContext->IOOperation = ClientIoWrite;
			lpIOContext->nTotalBytes = dwIoSize;
			lpIOContext->nSentBytes = 0;
			lpIOContext->wsabuf.len = dwIoSize;
			dwFlags = 0;
			nRet = WSASend(
				lpPerSocketContext->Socket,
				&lpIOContext->wsabuf, 1, &dwSendNumBytes,
				dwFlags,
				&(lpIOContext->Overlapped), NULL);
			if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError())) {
				myprintf("WSASend() failed: %d\n", WSAGetLastError());
				CloseClient(lpPerSocketContext, FALSE);
			}
			else if (g_bVerbose) {
				myprintf("WorkerThread %d: Socket(%d) Recv completed (%d bytes), Send posted\n",
					GetCurrentThreadId(), lpPerSocketContext->Socket, dwIoSize);
			}
			break;

		case ClientIoWrite:

			//
			// a write operation has completed, determine if all the data intended to be
			// sent actually was sent.
			//
			lpIOContext->IOOperation = ClientIoWrite;
			lpIOContext->nSentBytes += dwIoSize;
			dwFlags = 0;
			if (lpIOContext->nSentBytes < lpIOContext->nTotalBytes) {

				//
				// the previous write operation didn't send all the data,
				// post another send to complete the operation
				//
				buffSend.buf = lpIOContext->Buffer + lpIOContext->nSentBytes;
				buffSend.len = lpIOContext->nTotalBytes - lpIOContext->nSentBytes;
				nRet = WSASend(
					lpPerSocketContext->Socket,
					&buffSend, 1, &dwSendNumBytes,
					dwFlags,
					&(lpIOContext->Overlapped), NULL);
				if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError())) {
					myprintf("WSASend() failed: %d\n", WSAGetLastError());
					CloseClient(lpPerSocketContext, FALSE);
				}
				else if (g_bVerbose) {
					myprintf("WorkerThread %d: Socket(%d) Send partially completed (%d bytes), Recv posted\n",
						GetCurrentThreadId(), lpPerSocketContext->Socket, dwIoSize);
				}
			}
			else {

				//
				// previous write operation completed for this socket, post another recv
				//
				lpIOContext->IOOperation = ClientIoRead;
				dwRecvNumBytes = 0;
				dwFlags = 0;
				buffRecv.buf = lpIOContext->Buffer,
					buffRecv.len = MAX_BUFF_SIZE;
				nRet = WSARecv(
					lpPerSocketContext->Socket,
					&buffRecv, 1, &dwRecvNumBytes,
					&dwFlags,
					&lpIOContext->Overlapped, NULL);
				if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError())) {
					myprintf("WSARecv() failed: %d\n", WSAGetLastError());
					CloseClient(lpPerSocketContext, FALSE);
				}
				else if (g_bVerbose) {
					myprintf("WorkerThread %d: Socket(%d) Send completed (%d bytes), Recv posted\n",
						GetCurrentThreadId(), lpPerSocketContext->Socket, dwIoSize);
				}
			}
			break;

		} //switch
	} //while
	return(0);
}


//
//  Add a client connection context structure to the global list of context structures.
//
VOID CtxtListAddTo(PPER_SOCKET_CONTEXT lpPerSocketContext)	{

	PPER_SOCKET_CONTEXT pTemp;

	__try
	{
		g_CtxtListLock.lock();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		myprintf("EnterCriticalSection raised an exception.\n");
		return;
	}

	if (g_pCtxtList == NULL) {

		//
		// add the first node to the linked list
		//
		lpPerSocketContext->pCtxtBack = NULL;
		lpPerSocketContext->pCtxtForward = NULL;
		g_pCtxtList = lpPerSocketContext;
	}
	else {

		//
		// add node to head of list
		//
		pTemp = g_pCtxtList;

		g_pCtxtList = lpPerSocketContext;
		lpPerSocketContext->pCtxtBack = pTemp;
		lpPerSocketContext->pCtxtForward = NULL;

		pTemp->pCtxtForward = lpPerSocketContext;
	}

	g_CtxtListLock.unlock();

	return;
}

//
//  Remove a client context structure from the global list of context structures.
//
VOID CtxtListDeleteFrom(PPER_SOCKET_CONTEXT lpPerSocketContext)	{

	PPER_SOCKET_CONTEXT pBack;
	PPER_SOCKET_CONTEXT pForward;
	PPER_IO_CONTEXT     pNextIO = NULL;
	PPER_IO_CONTEXT     pTempIO = NULL;

	__try
	{
		g_CtxtListLock.lock();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		myprintf("EnterCriticalSection raised an exception.\n");
		return;
	}

	if (lpPerSocketContext) {
		pBack = lpPerSocketContext->pCtxtBack;
		pForward = lpPerSocketContext->pCtxtForward;

		if (pBack == NULL && pForward == NULL) {

			//
			// This is the only node in the list to delete
			//
			g_pCtxtList = NULL;
		}
		else if (pBack == NULL && pForward != NULL) {

			//
			// This is the start node in the list to delete
			//
			pForward->pCtxtBack = NULL;
			g_pCtxtList = pForward;
		}
		else if (pBack != NULL && pForward == NULL) {

			//
			// This is the end node in the list to delete
			//
			pBack->pCtxtForward = NULL;
		}
		else if (pBack && pForward) {

			//
			// Neither start node nor end node in the list
			//
			pBack->pCtxtForward = pForward;
			pForward->pCtxtBack = pBack;
		}

		//
		// Free all i/o context structures per socket
		//
		pTempIO = (PPER_IO_CONTEXT)(lpPerSocketContext->pIOContext);
		do {
			pNextIO = (PPER_IO_CONTEXT)(pTempIO->pIOContextForward);
			if (pTempIO) {

				//
				//The overlapped structure is safe to free when only the posted i/o has
				//completed. Here we only need to test those posted but not yet received 
				//by PQCS in the shutdown process.
				//
				if (g_bEndServer)
					while (!HasOverlappedIoCompleted((LPOVERLAPPED)pTempIO)) Sleep(0);
				xfree(pTempIO);
				pTempIO = NULL;
			}
			pTempIO = pNextIO;
		} while (pNextIO);

		xfree(lpPerSocketContext);
		lpPerSocketContext = NULL;
	}
	else {
		myprintf("CtxtListDeleteFrom: lpPerSocketContext is NULL\n");
	}

	g_CtxtListLock.unlock();
	return;
}


//
// Allocate a socket context for the new connection.  
//
PPER_SOCKET_CONTEXT CtxtAllocate(SOCKET sd, IO_OPERATION ClientIO)	{

	PPER_SOCKET_CONTEXT lpPerSocketContext;

	__try
	{
		g_CtxtListLock.lock();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		myprintf("EnterCriticalSection raised an exception.\n");
		return NULL;
	}

	lpPerSocketContext = (PPER_SOCKET_CONTEXT)xmalloc(sizeof(PER_SOCKET_CONTEXT));
	if (lpPerSocketContext) {
		lpPerSocketContext->pIOContext = (PPER_IO_CONTEXT)xmalloc(sizeof(PER_IO_CONTEXT));
		if (lpPerSocketContext->pIOContext) {
			lpPerSocketContext->Socket = sd;
			lpPerSocketContext->pCtxtBack = NULL;
			lpPerSocketContext->pCtxtForward = NULL;

			lpPerSocketContext->pIOContext->Overlapped.Internal = 0;
			lpPerSocketContext->pIOContext->Overlapped.InternalHigh = 0;
			lpPerSocketContext->pIOContext->Overlapped.Offset = 0;
			lpPerSocketContext->pIOContext->Overlapped.OffsetHigh = 0;
			lpPerSocketContext->pIOContext->Overlapped.hEvent = NULL;
			lpPerSocketContext->pIOContext->IOOperation = ClientIO;
			lpPerSocketContext->pIOContext->pIOContextForward = NULL;
			lpPerSocketContext->pIOContext->nTotalBytes = 0;
			lpPerSocketContext->pIOContext->nSentBytes = 0;
			lpPerSocketContext->pIOContext->wsabuf.buf = lpPerSocketContext->pIOContext->Buffer;
			lpPerSocketContext->pIOContext->wsabuf.len = sizeof(lpPerSocketContext->pIOContext->Buffer);
			lpPerSocketContext->pIOContext->SocketAccept = INVALID_SOCKET;

			ZeroMemory(lpPerSocketContext->pIOContext->wsabuf.buf, lpPerSocketContext->pIOContext->wsabuf.len);
		}
		else {
			xfree(lpPerSocketContext);
			myprintf("HeapAlloc() PER_IO_CONTEXT failed: %d\n", GetLastError());
		}

	}
	else {
		myprintf("HeapAlloc() PER_SOCKET_CONTEXT failed: %d\n", GetLastError());
		return(NULL);
	}

	g_CtxtListLock.unlock();

	return(lpPerSocketContext);
}


//
//  Free all context structure in the global list of context structures.
//
VOID CtxtListFree() {
	PPER_SOCKET_CONTEXT pTemp1, pTemp2;


	g_CtxtListLock.lock();
	//     __except(EXCEPTION_EXECUTE_HANDLER)
	//     {
	//         myprintf("EnterCriticalSection raised an exception.\n");
	//         return;
	//     }

	pTemp1 = g_pCtxtList;
	while (pTemp1) {
		pTemp2 = pTemp1->pCtxtBack;
		CloseClient(pTemp1, FALSE);
		pTemp1 = pTemp2;
	}

	g_CtxtListLock.unlock();

	return;
}