// cus.cpp : Defines the entry point for the console application.
//

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
// - make abuffer a real class
// - close down async properly (GetOverlappedResult(... bWait=TRUE);

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
	DWORD len;
	__int8 buf[ABUFFER_SIZE];
	__int8 *ptr;
	abuffer();
	void advance(DWORD);
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

typedef std::queue<abuffer *> bufferqueue;

DWORD pipe_input_helper(HANDLE hOutput, HANDLE hLog, OVERLAPPED *outlap, bufferqueue *outq, abuffer *abuf);
DWORD start_pipe_input(HANDLE hInput, OVERLAPPED *inlap, bufferqueue *inq, HANDLE hOutput, HANDLE hLog, OVERLAPPED *outlap, bufferqueue *outq);
DWORD handle_pipe_input(HANDLE hInput, OVERLAPPED *inlap, bufferqueue *inq, HANDLE hOutput, HANDLE hLog, OVERLAPPED *outlap, bufferqueue *outq);
DWORD start_async_out(HANDLE h, OVERLAPPED *olap, bufferqueue *bufq, abuffer *);
DWORD handle_async_out(HANDLE h, OVERLAPPED *olap, bufferqueue *bufq);
DWORD handle_stdin(HANDLE hInput, HANDLE hOutput, OVERLAPPED *olap, bufferqueue *bufq);
void add_offset(DWORD off, OVERLAPPED *olap);

#define WAITER_SUCCESS 0
#define WAITER_IO_ERROR 1
#define WAITER_EXIT_NORMAL 2

int wmain(int argc, wchar_t *argv[], wchar_t *envp[])
{
	DWORD flags;
	int c;
	const wchar_t *logName = NULL, *pipeName = NULL, *progname;

	progname = argv[0];

	hStdin = GetStdHandle(STD_INPUT_HANDLE);
	if (hStdin == INVALID_HANDLE_VALUE)
		ErrorExit(TEXT("GetStdHandle(stdin)"));
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

	hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hStdout == INVALID_HANDLE_VALUE)
		ErrorExit(TEXT("GetStdHandle(stdout)"));
	if (!GetConsoleMode(hStdout, &fdwStdoutSavedmode))
		ErrorExit(TEXT("GetConsoleMode(stdout"));
	stdoutModesaved = true;
	flags = fdwStdoutSavedmode;
	flags |= ENABLE_VIRTUAL_TERMINAL_PROCESSING |
		DISABLE_NEWLINE_AUTO_RETURN |
		ENABLE_WRAP_AT_EOL_OUTPUT;
	if (!SetConsoleMode(hStdout, flags))
		ErrorExit(TEXT("SetConsoleMode(stdout)"));

	while ((c = getopt(argc, argv, L"l:")) != -1) {
		switch (c) {
		case 'l':
			logName = optarg;
			break;
		default:
			fprintf(stderr, "%ls [-l logname] pipe\n", progname);
			return (1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 1) {
		fprintf(stderr, "%ls [-l logname] pipe\n", progname);
		return (1);
	}
	pipeName = argv[0];

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

	restore_terminal();
	return (0);
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
			if (inrecs[i].Event.KeyEvent.bKeyDown && inrecs[i].Event.KeyEvent.uChar.AsciiChar != 0) {
				abuf->buf[abuf->len++] = inrecs[i].Event.KeyEvent.uChar.AsciiChar;
			}
			break;
		case MOUSE_EVENT:
		case WINDOW_BUFFER_SIZE_EVENT:
		case FOCUS_EVENT:
		case MENU_EVENT:
			break;
		default:
			printf("Unknown Console EVent 0x%x\n", inrecs[i].EventType);
		}
	}

	if (abuf->len == 0) {
		delete abuf;
		return WAITER_SUCCESS;
	}

	for (DWORD i = 0; i < abuf->len; i++) {
		switch (state) {
		case STATE_INIT:
			if (abuf->buf[i] == '\r')
				state = STATE_NEWLINE;
			break;
		case STATE_NEWLINE:
			if (abuf->buf[i] == '~')
				state = STATE_TILDE;
			else if (abuf->buf[i] == '\r')
				state = STATE_NEWLINE;
			else
				state = STATE_INIT;
			break;
		case STATE_TILDE:
			if (abuf->buf[i] == '.') {
				delete abuf;
				return WAITER_EXIT_NORMAL;
			}
			if (abuf->buf[i] == '\r')
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
		if (!ReadFile(hInput, abuf->ptr, ABUFFER_SIZE, &dwLen, inlap)) {
			DWORD error = GetLastError();
			if (error == ERROR_IO_PENDING)
				return WAITER_SUCCESS;
			return WAITER_IO_ERROR;
		}

		inq->pop();
		abuf->len = dwLen;

		dwLen = pipe_input_helper(hOutput, hLog, outlap, outq, abuf);
		if (dwLen != WAITER_SUCCESS)
			return dwLen;
	}
}

DWORD pipe_input_helper(HANDLE hOutput, HANDLE hLog, OVERLAPPED *outlap, bufferqueue *outq, abuffer *abuf) {
	DWORD nlen;

	if (abuf->len == 0) {
		delete abuf;
		return WAITER_SUCCESS;
	}

	// Synchronous write to stdout
	if (!WriteFileAll(hOutput, abuf->ptr, abuf->len, &nlen))
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

	if (!GetOverlappedResult(hInput, inlap, &nlen, TRUE))
		return WAITER_IO_ERROR;

	abuf = inq->front();
	inq->pop();
	abuf->len = nlen;

	nlen = pipe_input_helper(hOutput, hLog, outlap, outq, abuf);
	if (nlen != WAITER_SUCCESS)
		return nlen;
	return start_pipe_input(hInput, inlap, inq, hOutput, hLog, outlap, outq);
}

DWORD start_async_out(HANDLE h, OVERLAPPED *olap, bufferqueue *bufq, abuffer *abuf) {
	DWORD nlen;

	if (h == NULL) {
		delete abuf;
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
		
		if (!WriteFile(h, abuf->ptr, abuf->len, &nlen, olap)) {
			DWORD error = GetLastError();
			if (error == ERROR_IO_PENDING)
				return WAITER_SUCCESS;
			return WAITER_IO_ERROR;
		}

		// Write completed synchronously
		add_offset(nlen, olap);
		abuf->advance(nlen);
		if (abuf->len == 0) {
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
	if (abuf->len == 0) {
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