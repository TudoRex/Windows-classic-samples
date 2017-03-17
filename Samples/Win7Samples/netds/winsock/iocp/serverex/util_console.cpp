#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <strsafe.h>
#include <windows.h>

#include <mutex>
/*
int myprintf(const char *lpFormat, ...) {

	int nLen = 0;
	int nRet = 0;
	char cBuffer[512];
	va_list arglist;
	HANDLE hOut = NULL;
	HRESULT hRet;

	ZeroMemory(cBuffer, sizeof(cBuffer));

	va_start(arglist, lpFormat);


	nLen = lstrlen(lpFormat);
	hRet = StringCchVPrintf(cBuffer, 512, lpFormat, arglist);

	if (nRet >= nLen || GetLastError() == 0) {
		hOut = GetStdHandle(STD_ERROR_HANDLE);
		if (hOut != INVALID_HANDLE_VALUE)
			WriteConsole(hOut, cBuffer, lstrlen(cBuffer), (LPDWORD)&nLen, NULL);
	}

	return nLen;
}
*/

int myprintf(const char *lpFormat, ...)
{
	std::unique_lock<std::mutex> func_lock;
	va_list arglist;
	va_start(arglist, lpFormat);
	int nRet = vprintf(lpFormat, arglist);
	va_end(arglist);
	return nRet;

}