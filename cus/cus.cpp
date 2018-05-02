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
// - handle partial write?
// - allocate buffers based on pipe dimensions
// - log file handling (and command line)

#include "stdafx.h"

VOID ErrorExit(LPCWSTR msg);
void restore_terminal(void);

bool stdinModesaved = false;
DWORD fdwStdinSavedmode;
bool stdoutModesaved = false;
DWORD fdwStdoutSavedmode;
HANDLE hStdin, hStdout;

struct waiter {
	OVERLAPPED *pOverlapIn, *pOverlapOut;
	HANDLE hInput, hOutput, hEvent;
	DWORD (*pHandleFunc)(struct waiter *);
	bool *pbOutputPending;
	unsigned __int8 *pBuffer;
	DWORD dwBufferSize;
};

void add_waiter(DWORD *pnWaiters, HANDLE *hWaiters, struct waiter *waiter, struct waiter **waiters);
DWORD start_pipein(struct waiter *w);
DWORD handle_pipein(struct waiter *w);
DWORD handle_stdin(struct waiter *w);
DWORD handle_pipeout(struct waiter *w);
DWORD handle_logout(struct waiter *w);

#define WAITER_SUCCESS 0
#define WAITER_IO_ERROR 1
#define WAITER_EXIT_NORMAL 2

int wmain(int argc, wchar_t *argv[], wchar_t *envp[])
{
	HANDLE hPipe, hLog;
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

	while ((c = getopt(argc, argv, L"")) != -1) {
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

	struct waiter wStdinToPipe, wPipeToStdout, wPipeOut, wLogOut;
	OVERLAPPED logOutOverlap;
	unsigned __int8 bufStdinToPipe[128];
	unsigned __int8 bufPipeToStdout[128];

	bool outputPending = false;
	bool logOutputPending = false;

	memset(&logOutOverlap, 0, sizeof(logOutOverlap));
	if (logName != NULL) {
		hLog = CreateFile(logName, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE, NULL,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
		if (hLog == INVALID_HANDLE_VALUE)
			ErrorExit(logName);

		logOutOverlap.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
		if (logOutOverlap.hEvent == NULL)
			ErrorExit(TEXT("CreateEvent(logout)"));
		wLogOut.pOverlapIn = NULL;
		wLogOut.pOverlapOut = &logOutOverlap;
		wLogOut.hInput = INVALID_HANDLE_VALUE;
		wLogOut.hOutput = hLog;
		wLogOut.hEvent = logOutOverlap.hEvent;
		wLogOut.pBuffer = bufStdinToPipe; //XXX
		wLogOut.dwBufferSize = sizeof(bufStdinToPipe);//XXX
		wLogOut.pHandleFunc = handle_logout;
		wLogOut.pbOutputPending = &logOutputPending;
	}

	OVERLAPPED pipeInOverlap;
	memset(&pipeInOverlap, 0, sizeof(pipeInOverlap));
	pipeInOverlap.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (pipeInOverlap.hEvent == NULL)
		ErrorExit(TEXT("CreateEvent(pipein)"));

	OVERLAPPED pipeOutOverlap;
	memset(&pipeOutOverlap, 0, sizeof(pipeOutOverlap));
	pipeOutOverlap.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (pipeOutOverlap.hEvent == NULL)
		ErrorExit(TEXT("CreateEvent(pipeout)"));

	wStdinToPipe.pOverlapIn = NULL;
	wStdinToPipe.pOverlapOut = &pipeOutOverlap;
	wStdinToPipe.hInput = hStdin;
	wStdinToPipe.hOutput = hPipe;
	wStdinToPipe.hEvent = hStdin;
	wStdinToPipe.pBuffer = bufStdinToPipe;
	wStdinToPipe.dwBufferSize = sizeof(bufStdinToPipe);
	wStdinToPipe.pHandleFunc = handle_stdin;
	wStdinToPipe.pbOutputPending = &outputPending;

	wPipeToStdout.pOverlapIn = &pipeInOverlap;
	wPipeToStdout.pOverlapOut = NULL;
	wPipeToStdout.hInput = hPipe;
	wPipeToStdout.hOutput = hStdout;
	wPipeToStdout.hEvent = pipeInOverlap.hEvent;
	wPipeToStdout.dwBufferSize = sizeof(bufPipeToStdout);
	wPipeToStdout.pBuffer = bufPipeToStdout;
	wPipeToStdout.pHandleFunc = handle_pipein;
	wPipeToStdout.pbOutputPending = NULL;

	wPipeOut.pOverlapIn = NULL;
	wPipeOut.pOverlapOut = &pipeOutOverlap;
	wPipeOut.hInput = 0;
	wPipeOut.hOutput = hPipe;
	wPipeOut.hEvent = pipeOutOverlap.hEvent;
	wPipeOut.pBuffer = bufStdinToPipe;
	wPipeOut.dwBufferSize = sizeof(bufStdinToPipe);
	wPipeOut.pHandleFunc = handle_pipeout;
	wPipeOut.pbOutputPending = NULL;

	start_pipein(&wPipeToStdout);

	for (;;) {
		HANDLE hWaiters[4];
		DWORD nWaiters = 0;
		DWORD wait;
		struct waiter *waiters[4];

		// We always wait for console input
		add_waiter(&nWaiters, hWaiters, &wStdinToPipe, waiters);

		// We always wait for pipe input
		add_waiter(&nWaiters, hWaiters, &wPipeToStdout, waiters);

		// If there's output pending on the pipe, we wait for it
		if (outputPending)
			add_waiter(&nWaiters, hWaiters, &wPipeOut, waiters);

		if (logOutputPending)
			add_waiter(&nWaiters, hWaiters, &wLogOut, waiters);

		wait = WaitForMultipleObjects(nWaiters, hWaiters, FALSE, INFINITE);
		for (DWORD i = 0; i < nWaiters; i++) {
			if (wait == WAIT_OBJECT_0 + i) {
				DWORD w = waiters[i]->pHandleFunc(waiters[i]);
				switch (w) {
				case WAITER_SUCCESS:
					break;
				case WAITER_IO_ERROR:
					ErrorExit(TEXT("I/O"));
					break;
				case WAITER_EXIT_NORMAL:
					goto out;
				}
			}
		}
		if (wait == WAIT_TIMEOUT)
			printf("wait timeout\n");
		if (wait == WAIT_FAILED)
			ErrorExit(TEXT("wait failed"));
	}

out:
	restore_terminal();
	return (0);
}

void
add_waiter(DWORD *pnWaiters, HANDLE *hWaiters, struct waiter *waiter, struct waiter **waiters) {
	waiters[*pnWaiters] = waiter;
	hWaiters[*pnWaiters] = waiter->hEvent;
	*pnWaiters = (*pnWaiters)++;
}

BOOL
WriteConsoleAll(_In_ HANDLE hConsoleOutput,
	_In_ const VOID *lpBuffer,
	_In_ DWORD nNumberOfCharsToWrite,
	_Out_ LPDWORD lpNumberOfCharsWritten) {
	DWORD dwWritten, dwRemaining, dwWrittenTotal = 0;

	lpNumberOfCharsWritten = 0;
	for (dwRemaining = nNumberOfCharsToWrite; dwRemaining != 0;) {
		if (!WriteConsole(hConsoleOutput, lpBuffer, dwRemaining, &dwWritten, NULL))
			return FALSE;
		lpNumberOfCharsWritten += dwWritten;
		dwRemaining -= dwWritten;
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

//
// We just have to complete the write operation
// XXX how do we deal with incomplete write's?
//
DWORD
handle_pipeout(struct waiter *w) {
	DWORD len;

	if (!GetOverlappedResult(w->hOutput, w->pOverlapOut, &len, TRUE))
		return WAITER_IO_ERROR;
	if (w->pbOutputPending)
		*w->pbOutputPending = FALSE;
	return (0);
}

DWORD handle_logout(struct waiter *w) {
	DWORD len;

	if (!GetOverlappedResult(w->hOutput, w->pOverlapOut, &len, TRUE))
		return WAITER_IO_ERROR;
	if (w->pbOutputPending)
		*w->pbOutputPending = FALSE;
	return (0);
}

DWORD handle_stdin(struct waiter *w) {
	static TermState state = STATE_NEWLINE;
	INPUT_RECORD inrecs[10];
	DWORD nlen, rlen;

	// XXX we're assuming there are less inrecs than w->dwBufferSize
	if (!ReadConsoleInput(w->hInput, inrecs, sizeof(inrecs) / sizeof(inrecs[0]), &nlen))
		return WAITER_IO_ERROR;

	rlen = 0;
	for (DWORD i = 0; i < nlen; i++) {
		switch (inrecs[i].EventType) {
		case KEY_EVENT:
			if (inrecs[i].Event.KeyEvent.bKeyDown && inrecs[i].Event.KeyEvent.uChar.AsciiChar != 0) {
				w->pBuffer[rlen++] = inrecs[i].Event.KeyEvent.uChar.AsciiChar;
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

	for (DWORD i = 0; i < rlen; i++) {
		switch (state) {
		case STATE_INIT:
			if (w->pBuffer[i] == '\r')
				state = STATE_NEWLINE;
			break;
		case STATE_NEWLINE:
			if (w->pBuffer[i] == '~')
				state = STATE_TILDE;
			else if (w->pBuffer[i] == '\r')
				state = STATE_NEWLINE;
			else
				state = STATE_INIT;
			break;
		case STATE_TILDE:
			if (w->pBuffer[i] == '.')
				return WAITER_EXIT_NORMAL;
			if (w->pBuffer[i] == '\r')
				state = STATE_NEWLINE;
			else
				state = STATE_INIT;
		}
	}

	if (!WriteFile(w->hOutput, w->pBuffer, rlen, &nlen, w->pOverlapOut)) {
		DWORD error = GetLastError();
		if (error == ERROR_IO_PENDING) {
			if (w->pbOutputPending) {
				*w->pbOutputPending = true;
			}
		}
		else
			return WAITER_IO_ERROR;
	}

	return (WAITER_SUCCESS);
}

//
// start_pipein(waiter): read all the data we can and write
// it synchronously to stdout. As soon as we hit a read that
// doesn't compelete immediately, return (it will be signaled
// later).
//
DWORD start_pipein(struct waiter *w) {
	for (;;) {
		DWORD dwLen;

		if (!ReadFile(w->hInput, w->pBuffer, w->dwBufferSize, &dwLen, w->pOverlapIn)) {
			DWORD error = GetLastError();
			if (error == ERROR_IO_PENDING)
				return (WAITER_SUCCESS);
			return WAITER_IO_ERROR;
		}

		if (!WriteFile(w->hOutput, w->pBuffer, dwLen, &dwLen, w->pOverlapOut))
			return WAITER_IO_ERROR;
	}
}

//
// Handle the signal of pipe input
//
DWORD handle_pipein(struct waiter *w) {
	DWORD nlen;

	if (!GetOverlappedResult(w->hInput, w->pOverlapIn, &nlen, TRUE))
		return WAITER_IO_ERROR;
	if (!WriteFile(w->hOutput, w->pBuffer, nlen, &nlen, w->pOverlapOut))
		return WAITER_IO_ERROR;
	// XXX incomplete write?
	return start_pipein(w);
}

