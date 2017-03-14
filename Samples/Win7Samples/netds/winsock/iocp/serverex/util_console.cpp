#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <strsafe.h>
#include <windows.h>

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
		hOut = GetStdHandle(STD_OUTPUT_HANDLE);
		if (hOut != INVALID_HANDLE_VALUE)
			WriteConsole(hOut, cBuffer, lstrlen(cBuffer), (LPDWORD)&nLen, NULL);
	}

	return nLen;
}