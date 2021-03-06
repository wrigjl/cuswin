// cus.cpp : cu for windows named pipes.
//
// Copyright (c) 2018 Jason L. Wright (jason@thought.net)
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// Helpful documentation:
// https://docs.microsoft.com/en-us/windows/console/
//
// There are many oddities of the Windows Console. One of the most
// annoying is the fact that you can't use WaitForMultipleObjects()
// and ReadFile() on the console (ReadFile() will block. Instead,
// you have to use WaitForMutipleObjects() and ReadConsoleInput(),
// which also means you must interpret the key codes somewhat
// manually.

// TODO
// - switch to unicode consistently
//		fprintf, etc.
// - allocate buffers based on pipe dimensions

#include "stdafx.h"
#include <queue>

VOID ErrorExit(LPCWSTR msg);
void restore_terminal(void);

bool stdinModesaved = false;
DWORD fdwStdinSavedmode;
bool stdoutModesaved = false;
DWORD fdwStdoutSavedmode;
HANDLE hStdin, hStdout;
HANDLE hPipe, hLog;

#define ABUFFER_SIZE 64

struct abuffer {
private:
	DWORD len;
	__int8 buf[ABUFFER_SIZE];
	__int8 *ptr;
public:
	abuffer();
	void advance(DWORD);
	void add(__int8);
	bool full() { return len == ABUFFER_SIZE; }
	bool empty() { return len == 0; }
	DWORD size() { return len; }
	void size(DWORD n) { len = n; }
	__int8 *getptr() { return ptr; }
	__int8 at(DWORD i) { return buf[i]; }
};

abuffer::abuffer() {
	len = 0;
	ptr = &buf[0];
}

void
abuffer::advance(DWORD n) {
	len -= n;
	ptr += n;
}

void
abuffer::add(__int8 c) {
	buf[len++] = c;
}

typedef std::queue<abuffer *> bufferqueue;

DWORD pipe_input_helper(HANDLE hOutput, HANDLE hLog, OVERLAPPED *outlap, bufferqueue *outq, abuffer *abuf);
DWORD start_pipe_input(HANDLE hInput, OVERLAPPED *inlap, bufferqueue *inq, HANDLE hOutput, HANDLE hLog, OVERLAPPED *outlap, bufferqueue *outq);
DWORD handle_pipe_input(HANDLE hInput, OVERLAPPED *inlap, bufferqueue *inq, HANDLE hOutput, HANDLE hLog, OVERLAPPED *outlap, bufferqueue *outq);
DWORD start_async_out(HANDLE h, OVERLAPPED *olap, bufferqueue *bufq, abuffer *);
DWORD handle_async_out(HANDLE h, OVERLAPPED *olap, bufferqueue *bufq);
DWORD handle_stdin(HANDLE hInput, HANDLE hOutput, OVERLAPPED *olap, bufferqueue *bufq);
void add_offset(DWORD off, OVERLAPPED *olap);
void drain_log(HANDLE h, OVERLAPPED *olap, bufferqueue *bufq);
void usage(const wchar_t *name);
int pipeinfo(LPCTSTR);
void get_acctName(LPCTSTR name, PSID pSid);
void show_acl(LPCTSTR name, PACL acl);
void show_mask(DWORD mask);

#define WAITER_SUCCESS 0
#define WAITER_IO_ERROR 1
#define WAITER_EXIT_NORMAL 2

int wmain(int argc, wchar_t *argv[], wchar_t *envp[])
{
	DWORD flags;
	int c;
	const wchar_t *logName = NULL, *pipeName = NULL, *progname;
	bool iFlag = false;

	progname = argv[0];

	hStdin = GetStdHandle(STD_INPUT_HANDLE);
	if (hStdin == INVALID_HANDLE_VALUE)
		ErrorExit(TEXT("GetStdHandle(stdin)"));

	hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hStdout == INVALID_HANDLE_VALUE)
		ErrorExit(TEXT("GetStdHandle(stdout)"));

	while ((c = getopt(argc, argv, L"il:")) != -1) {
		switch (c) {
		case 'i':
			if (iFlag) {
				usage(progname);
				return (1);
			}
			iFlag = true;
			break;
		case 'l':
			if (logName != NULL) {
				usage(progname);
				return (1);
			}
			logName = optarg;
			break;
		default:
			usage(progname);
			return (1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 1 || (logName && iFlag)) {
		usage(progname);
		return (1);
	}
	pipeName = argv[0];

	if (iFlag)
		return pipeinfo(pipeName);

	hPipe = CreateFile(pipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL,
		OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (hPipe == INVALID_HANDLE_VALUE)
		ErrorExit(pipeName);

	hLog = NULL;
	if (logName != NULL) {
		hLog = CreateFile(logName, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE, NULL,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
		if (hLog == INVALID_HANDLE_VALUE)
			ErrorExit(logName);
	}

	if (!GetConsoleMode(hStdin, &fdwStdinSavedmode))
		ErrorExit(TEXT("GetConsoleMode"));
	stdinModesaved = true;
	flags = fdwStdinSavedmode;
	flags |= ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS;
	flags &= ~(ENABLE_ECHO_INPUT |
		ENABLE_LINE_INPUT |
		ENABLE_MOUSE_INPUT |
		ENABLE_WINDOW_INPUT);
	if (!SetConsoleMode(hStdin, flags))
		ErrorExit(TEXT("SetConsoleMode(stdin)"));

	if (!GetConsoleMode(hStdout, &fdwStdoutSavedmode))
		ErrorExit(TEXT("GetConsoleMode(stdout"));
	stdoutModesaved = true;
	flags = fdwStdoutSavedmode;
	flags |= ENABLE_VIRTUAL_TERMINAL_PROCESSING |
		DISABLE_NEWLINE_AUTO_RETURN |
		ENABLE_WRAP_AT_EOL_OUTPUT;
	if (!SetConsoleMode(hStdout, flags))
		ErrorExit(TEXT("SetConsoleMode(stdout)"));

	OVERLAPPED pipeInOverlap;
	ZeroMemory(&pipeInOverlap, sizeof(pipeInOverlap));
	pipeInOverlap.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (pipeInOverlap.hEvent == NULL)
		ErrorExit(TEXT("CreateEvent(pipein)"));

	OVERLAPPED pipeOutOverlap;
	ZeroMemory(&pipeOutOverlap, sizeof(pipeOutOverlap));
	pipeOutOverlap.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (pipeOutOverlap.hEvent == NULL)
		ErrorExit(TEXT("CreateEvent(pipeout)"));

	OVERLAPPED logOutOverlap;
	ZeroMemory(&logOutOverlap, sizeof(logOutOverlap));
	logOutOverlap.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (logOutOverlap.hEvent == NULL)
		ErrorExit(TEXT("CreateEvent(logout)"));

	auto logOutQueue = new bufferqueue();
	auto pipeInQueue = new bufferqueue();
	auto pipeOutQueue = new bufferqueue();
	pipeInQueue->push(new abuffer());
	start_pipe_input(hPipe, &pipeInOverlap, pipeInQueue, hStdout, hLog, &logOutOverlap, logOutQueue);

	for (;;) {
		HANDLE hWaiters[4];
		DWORD wait;

		hWaiters[0] = hStdin;					// stdin
		hWaiters[1] = pipeInOverlap.hEvent;		// pipe input
		hWaiters[2] = pipeOutOverlap.hEvent;	// pipe output
		hWaiters[3] = logOutOverlap.hEvent;		// log output

		wait = WaitForMultipleObjects(sizeof(hWaiters)/sizeof(hWaiters[0]), hWaiters, FALSE, INFINITE);
		switch (wait) {
		case WAIT_OBJECT_0 + 0:
			// stdin
			wait = handle_stdin(hStdin, hPipe, &pipeOutOverlap, pipeOutQueue);
			break;
		case WAIT_OBJECT_0 + 1:
			// pipe input
			wait = handle_pipe_input(hPipe, &pipeInOverlap, pipeInQueue, hStdout, hLog, &logOutOverlap, logOutQueue);
			break;
		case WAIT_OBJECT_0 + 2:
			// pipe output
			wait = handle_async_out(hPipe, &pipeOutOverlap, pipeOutQueue);
			break;
		case WAIT_OBJECT_0 + 3:
			// log output
			wait = handle_async_out(hLog, &logOutOverlap, logOutQueue);
			break;
		case WAIT_TIMEOUT:
			ErrorExit(TEXT("wait timeout\n"));
			break;
		case WAIT_FAILED:
			// *shrug*
			ErrorExit(TEXT("wait failed"));
			break;
		default:
			ErrorExit(TEXT("wait dunno"));
			break;
		}

		if (wait == WAITER_IO_ERROR)
			ErrorExit(TEXT("IOWAITER error"));
		if (wait == WAITER_EXIT_NORMAL)
			break;
	}

	drain_log(hLog, &logOutOverlap, logOutQueue);
	restore_terminal();
	return (0);
}

void
usage(const wchar_t *name) {
	fwprintf(stderr, L"%s [-l log] pipe\n%s -i pipe\n", name, name);
}

BOOL
WriteConsoleAll(_In_ HANDLE hConsoleOutput,
	_In_ const VOID *lpBuffer,
	_In_ DWORD nNumberOfCharsToWrite,
	_Out_ LPDWORD lpNumberOfCharsWritten) {
	DWORD dwWritten, dwRemaining, dwWrittenTotal = 0;
	const __int8 *buf = (const __int8 *)lpBuffer;

	lpNumberOfCharsWritten = 0;
	for (dwRemaining = nNumberOfCharsToWrite; dwRemaining != 0;) {
		if (!WriteConsole(hConsoleOutput, buf, dwRemaining, &dwWritten, NULL))
			return FALSE;
		lpNumberOfCharsWritten += dwWritten;
		dwRemaining -= dwWritten;
		buf += dwWritten;
	}
	return TRUE;
}

BOOL
WriteFileAll(_In_ HANDLE hConsoleOutput,
	_In_ const VOID *lpBuffer,
	_In_ DWORD nNumberOfCharsToWrite,
	_Out_ LPDWORD lpNumberOfCharsWritten) {
	DWORD dwWritten, dwRemaining, dwWrittenTotal = 0;
	const __int8 *buf = (const __int8 *)lpBuffer;

	lpNumberOfCharsWritten = 0;
	for (dwRemaining = nNumberOfCharsToWrite; dwRemaining != 0;) {
		if (!WriteFile(hConsoleOutput, buf, dwRemaining, &dwWritten, NULL))
			return FALSE;
		lpNumberOfCharsWritten += dwWritten;
		dwRemaining -= dwWritten;
		buf += dwWritten;
	}
	return TRUE;
}

VOID
ErrorExit(LPCWSTR lpszMessage)
{
	HANDLE hStderr;
	LPTSTR lpMsgBuf;
	DWORD written;

	restore_terminal();

	hStderr = GetStdHandle(STD_ERROR_HANDLE);
	if (hStderr == NULL || hStderr == INVALID_HANDLE_VALUE)
		ExitProcess(1);

	if (lpszMessage != NULL) {
		WriteConsoleAll(hStderr, lpszMessage, lstrlen(lpszMessage), &written);
		WriteConsoleAll(hStderr, TEXT(": "), lstrlen(TEXT(": ")), &written);
	}
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
	    FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, GetLastError(),
		0, (LPTSTR)&lpMsgBuf, 0, NULL);
	WriteConsoleAll(hStderr, lpMsgBuf, lstrlen(lpMsgBuf), &written);
	LocalFree(lpMsgBuf);
	ExitProcess(1);
}

void
restore_terminal(void) {
	if (stdinModesaved)
		SetConsoleMode(hStdin, fdwStdinSavedmode);
	if (stdoutModesaved)
		SetConsoleMode(hStdout, fdwStdoutSavedmode);
}

enum TermState {
	STATE_INIT,
	STATE_NEWLINE,
	STATE_TILDE,
	STATE_DOT
};

DWORD handle_stdin(HANDLE hInput, HANDLE hOutput, OVERLAPPED *olap, bufferqueue *bufq) {
	static TermState state = STATE_NEWLINE;
	INPUT_RECORD inrecs[ABUFFER_SIZE];
	DWORD nlen, rlen;

	if (!ReadConsoleInput(hInput, inrecs, ABUFFER_SIZE, &nlen))
		return WAITER_IO_ERROR;

	auto abuf = new abuffer();
	rlen = 0;
	for (DWORD i = 0; i < nlen; i++) {
		switch (inrecs[i].EventType) {
		case KEY_EVENT:
			if (inrecs[i].Event.KeyEvent.bKeyDown && inrecs[i].Event.KeyEvent.uChar.AsciiChar != 0)
				abuf->add(inrecs[i].Event.KeyEvent.uChar.AsciiChar);
			break;
		case MOUSE_EVENT:
		case WINDOW_BUFFER_SIZE_EVENT:
		case FOCUS_EVENT:
		case MENU_EVENT:
			break;
		default:
			fwprintf(stderr, L"Unknown console event 0x%x\n", inrecs[i].EventType);
		}
	}

	if (abuf->empty()) {
		delete abuf;
		return WAITER_SUCCESS;
	}

	for (DWORD i = 0; i < abuf->size(); i++) {
		switch (state) {
		case STATE_INIT:
			if (abuf->at(i) == '\r')
				state = STATE_NEWLINE;
			break;
		case STATE_NEWLINE:
			if (abuf->at(i) == '~')
				state = STATE_TILDE;
			else if (abuf->at(i) == '\r')
				state = STATE_NEWLINE;
			else
				state = STATE_INIT;
			break;
		case STATE_TILDE:
			if (abuf->at(i) == '.') {
				delete abuf;
				return WAITER_EXIT_NORMAL;
			}
			if (abuf->at(i) == '\r')
				state = STATE_NEWLINE;
			else
				state = STATE_INIT;
		}
	}

	return start_async_out(hOutput, olap, bufq, abuf);
}

DWORD start_pipe_input(HANDLE hInput, OVERLAPPED *inlap, bufferqueue *inq, HANDLE hOutput, HANDLE hLog, OVERLAPPED *outlap, bufferqueue *outq) {
	for (;;) {
		DWORD dwLen;

		if (inq->empty() == 0)
			inq->push(new abuffer());

		auto abuf = inq->front();
		if (!ReadFile(hInput, abuf->getptr(), ABUFFER_SIZE, &dwLen, inlap)) {
			DWORD error = GetLastError();
			if (error == ERROR_IO_PENDING)
				return WAITER_SUCCESS;
			return WAITER_IO_ERROR;
		}

		inq->pop();
		abuf->size(dwLen);

		dwLen = pipe_input_helper(hOutput, hLog, outlap, outq, abuf);
		if (dwLen != WAITER_SUCCESS)
			return dwLen;
	}
}

DWORD pipe_input_helper(HANDLE hOutput, HANDLE hLog, OVERLAPPED *outlap, bufferqueue *outq, abuffer *abuf) {
	DWORD nlen;

	if (abuf->empty()) {
		delete abuf;
		return WAITER_SUCCESS;
	}

	// Synchronous write to stdout
	if (!WriteFileAll(hOutput, abuf->getptr(), abuf->size(), &nlen))
		return WAITER_IO_ERROR;

	return start_async_out(hLog, outlap, outq, abuf);
}

// handle pipe input:
//   finish read
//   sync write to stdout
//   (optional) async write to log
DWORD handle_pipe_input(HANDLE hInput, OVERLAPPED *inlap, bufferqueue *inq, HANDLE hOutput, HANDLE hLog, OVERLAPPED *outlap, bufferqueue *outq) {
	DWORD nlen;
	abuffer *abuf;

	if (!GetOverlappedResult(hInput, inlap, &nlen, FALSE))
		return WAITER_IO_ERROR;

	abuf = inq->front();
	inq->pop();
	abuf->size(nlen);

	nlen = pipe_input_helper(hOutput, hLog, outlap, outq, abuf);
	if (nlen != WAITER_SUCCESS)
		return nlen;
	return start_pipe_input(hInput, inlap, inq, hOutput, hLog, outlap, outq);
}

DWORD start_async_out(HANDLE h, OVERLAPPED *olap, bufferqueue *bufq, abuffer *abuf) {
	DWORD nlen;

	if (h == NULL) {
		delete abuf;
		return WAITER_SUCCESS;
	}

	if (abuf != NULL && !bufq->empty()) {
		// already chewing on something, queue and go.
		bufq->push(abuf);
		return WAITER_SUCCESS;
	}

	if (abuf != NULL)
		bufq->push(abuf);
	while (!bufq->empty()) {
		abuf = bufq->front();
		
		if (!WriteFile(h, abuf->getptr(), abuf->size(), &nlen, olap)) {
			DWORD error = GetLastError();
			if (error == ERROR_IO_PENDING)
				return WAITER_SUCCESS;
			return WAITER_IO_ERROR;
		}

		// Write completed synchronously
		add_offset(nlen, olap);
		abuf->advance(nlen);
		if (abuf->empty()) {
			bufq->pop();
			delete abuf;
		}
	}

	// If we're here, all our writes are done, relax
	if (!ResetEvent(olap->hEvent))
		return WAITER_IO_ERROR;
	return WAITER_SUCCESS;
}

DWORD handle_async_out(HANDLE h, OVERLAPPED *olap, bufferqueue *bufq) {
	DWORD len;

	if (bufq->empty()) {
		if (!ResetEvent(olap->hEvent))
			return WAITER_IO_ERROR;
		return WAITER_SUCCESS;
	}

	if (!GetOverlappedResult(h, olap, &len, FALSE))
		return WAITER_IO_ERROR;
	add_offset(len, olap);

	auto abuf = bufq->front();
	abuf->advance(len);
	if (abuf->empty()) {
		bufq->pop();
		delete abuf;
	}

	return start_async_out(h, olap, bufq, NULL);
}

void
add_offset(DWORD off, OVERLAPPED *olap) {
	ULARGE_INTEGER pos;

	pos.LowPart = olap->Offset;
	pos.HighPart = olap->OffsetHigh;
	pos.QuadPart += off;
	olap->Offset = pos.LowPart;
	olap->OffsetHigh = pos.HighPart;
}

void
drain_log(HANDLE h, OVERLAPPED *olap, bufferqueue *bufq) {
	DWORD len;

	if (!bufq->empty()) {
		auto abuf = bufq->front();

		// There's an I/O pending, so compelete it.
		if (!GetOverlappedResult(h, olap, &len, TRUE))
			return;
		add_offset(len, olap);
		abuf->advance(len);
		if (abuf->empty()) {
			bufq->pop();
			delete abuf;
		}
	}

	// Now, drain the remaining buffers
	while (!bufq->empty()) {
		auto abuf = bufq->front();

		if (!WriteFile(h, abuf->getptr(), abuf->size(), &len, olap)) {
			if (GetLastError() == ERROR_IO_PENDING) {
				if (!GetOverlappedResult(h, olap, &len, TRUE))
					return;
			}
		}
		add_offset(len, olap);
		abuf->advance(len);
		if (abuf->empty()) {
			bufq->pop();
			delete abuf;
		}
	}
}

int
pipeinfo(LPCTSTR pipename) {
	DWORD dwRet, dwAcctName = 0, dwDomainName = 0;
	PSID pSidOwner = NULL, pSidGroup = NULL;
	PSECURITY_DESCRIPTOR pSD = NULL;
	LPTSTR pstrAcctName = NULL, pstrDomainName = NULL;
	SID_NAME_USE eUse = SidTypeUnknown;
	PACL pDacl = NULL, pSacl = NULL;

	HANDLE hPipe = CreateFile(pipename, FILE_READ_ATTRIBUTES | FILE_READ_EA | STANDARD_RIGHTS_READ | READ_CONTROL, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hPipe == INVALID_HANDLE_VALUE)
		ErrorExit(L"CreateFile");
	dwRet = GetSecurityInfo(hPipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION |
		GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &pSidOwner, &pSidGroup, &pDacl, &pSacl, &pSD);
	if (dwRet != ERROR_SUCCESS) {
		SetLastError(dwRet);
		ErrorExit(L"GetSecurityInfo");
	}

	get_acctName(TEXT("owner"), pSidOwner);
	get_acctName(TEXT("group"), pSidGroup);
	show_acl(TEXT("dacl"), pDacl);
	show_acl(TEXT("sacl"), pSacl);

	if (pSD)
		LocalFree(pSD);
	if (hPipe != INVALID_HANDLE_VALUE)
		CloseHandle(hPipe);
	return 0;
}

void
get_acctName(LPCTSTR name, PSID pSid) {
	DWORD dwAcctName = 0, dwDomainName = 0;
	PSECURITY_DESCRIPTOR pSD = NULL;
	LPTSTR pstrAcctName = NULL, pstrDomainName = NULL;
	SID_NAME_USE eUse = SidTypeUnknown;

	if (!LookupAccountSid(NULL, pSid, pstrAcctName, &dwAcctName, pstrDomainName, &dwDomainName, &eUse)) {
		if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
			ErrorExit(L"LookupAccountSid");
	}

	pstrAcctName = (LPWSTR)GlobalAlloc(GMEM_FIXED, dwAcctName * sizeof(TCHAR));
	if (pstrAcctName == NULL)
		ErrorExit(L"GlobalAlloc");

	pstrDomainName = (LPWSTR)GlobalAlloc(GMEM_FIXED, dwDomainName * sizeof(TCHAR));
	if (pstrDomainName == NULL)
		ErrorExit(L"GlobalAlloc");

	if (!LookupAccountSid(NULL, pSid, pstrAcctName, &dwAcctName, pstrDomainName, &dwDomainName, &eUse))
		ErrorExit(L"LookupAccountSid");

	_tprintf(TEXT("%s: %s\\%s\n"), name, pstrDomainName, pstrAcctName);

	if (pstrDomainName != NULL)
		GlobalFree(pstrDomainName);
	if (pstrAcctName != NULL)
		GlobalFree(pstrAcctName);
}

void
show_acl(LPCTSTR name, PACL acl) {
	ACL_REVISION_INFORMATION revinfo;
	ACL_SIZE_INFORMATION sizeinfo;

	if (acl == NULL)
		return;
	if (!GetAclInformation(acl, &revinfo, sizeof(revinfo), AclRevisionInformation))
		ErrorExit(TEXT("acl-revision"));
	if (!GetAclInformation(acl, &sizeinfo, sizeof(sizeinfo), AclSizeInformation))
		ErrorExit(TEXT("acl-size"));

	_tprintf(TEXT("%s: revision %u count %u bytes-used %u bytes-free %u\n"), name,
		revinfo.AclRevision, sizeinfo.AceCount, sizeinfo.AclBytesInUse, sizeinfo.AclBytesFree);

	for (DWORD i = 0; i < sizeinfo.AceCount; i++) {
		void *pAce;
		PACE_HEADER hdr;

		if (!GetAce(acl, i, &pAce))
			ErrorExit(TEXT("GetAce"));
		hdr = (PACE_HEADER)pAce;
		printf("%u: type %u flags=%x ", i, hdr->AceType, hdr->AceFlags);
		switch (hdr->AceType) {
		case ACCESS_ALLOWED_ACE_TYPE:
			PACCESS_ALLOWED_ACE accallowed = (PACCESS_ALLOWED_ACE)pAce;
			printf("access-allowed mask=%x", accallowed->Mask);
			show_mask(accallowed->Mask);
			get_acctName(TEXT(" sid"), (PSID)&accallowed->SidStart);
			break;
		}
	}
}

void
show_mask(DWORD mask) {
	const struct mask_bits {
		LPCTSTR name;
		DWORD bit;
	} maskbits[] = {
		{ TEXT("read"), FILE_READ_DATA },
		{ TEXT("write"), FILE_WRITE_DATA },
		{ TEXT("createpipe"), FILE_CREATE_PIPE_INSTANCE },
		{ TEXT("readea"), FILE_READ_EA },
		{ TEXT("writeea"), FILE_WRITE_EA },
		{ TEXT("execute"), FILE_EXECUTE },
		{ TEXT("delete-child"), FILE_DELETE_CHILD },
		{ TEXT("readattr"), FILE_READ_ATTRIBUTES },
		{ TEXT("writeattr"), FILE_WRITE_ATTRIBUTES },
		{ TEXT("delete"), DELETE },
		{ TEXT("readcontrol"), READ_CONTROL },
		{ TEXT("writedac"), WRITE_DAC },
		{ TEXT("writeowner"), WRITE_OWNER },
		{ TEXT("sync"), SYNCHRONIZE }
	};
	TCHAR c = '(';

	for (auto i = 0; i < sizeof(maskbits) / sizeof(maskbits[0]); i++) {
		if (mask & maskbits[i].bit) {
			mask &= ~maskbits[i].bit;
			_tprintf(TEXT("%c%s"), c, maskbits[i].name);
			c = ',';
		}
	}
	if (mask)
		_tprintf(TEXT("%c0x%x"), c, mask);
	if (c == '(')
		_tprintf(TEXT("%c"), c);
	_tprintf(TEXT("%c"), ')');
}
