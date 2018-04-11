// cus.cpp : Defines the entry point for the console application.
//

// TODO
// - switch to unicode consistently
// - single thread using WaitForMultipleObjects on the console objects/pipe events?

#include "stdafx.h"

DWORD WINAPI io_runner_ConToPipe(__in LPVOID lpParameter);
DWORD WINAPI io_runner_PipeToCon(__in LPVOID lpParameter);

struct io_handles {
	HANDLE read_handle;
	HANDLE write_handle;
};

VOID ErrorExit(const char *);
void restore_terminal(void);

bool stdinModesaved = false;
DWORD fdwStdinSavedmode;
bool stdoutModesaved = false;
DWORD fdwStdoutSavedmode;
HANDLE hStdin, hStdout, hPipe, hStderr;
HANDLE waiters[2];

int wmain(int argc, wchar_t *argv[], wchar_t *envp[])
{
	DWORD flags;

	hStderr = GetStdHandle(STD_ERROR_HANDLE);
	if (hStderr == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "failed to get stderr handle\n");
		return (1);
	}

	hStdin = GetStdHandle(STD_INPUT_HANDLE);
	if (hStdin == INVALID_HANDLE_VALUE) {
		ErrorExit("GetStdHandle(stdin)");
	}
	if (!GetConsoleMode(hStdin, &fdwStdinSavedmode)) {
		ErrorExit("GetConsoleMode");
	}
	stdinModesaved = true;
	flags = fdwStdinSavedmode;
	flags |= ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS;
	flags &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
	if (!SetConsoleMode(hStdin, flags)) {
		ErrorExit("SetConsoleMode(stdin)");
	}

	hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hStdout == INVALID_HANDLE_VALUE) {
		ErrorExit("GetStdHandle(stdout)");
	}
	if (!GetConsoleMode(hStdout, &fdwStdoutSavedmode)) {
		ErrorExit("GetConsoleMode(stdout");
	}
	stdoutModesaved = true;
	if (!SetConsoleMode(hStdout, fdwStdoutSavedmode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | 
		DISABLE_NEWLINE_AUTO_RETURN | 
		ENABLE_WRAP_AT_EOL_OUTPUT)) {
		ErrorExit("SetConsoleMode(stdout)");
	}

	if (argc != 2) {
		fprintf(stderr, "%ls [pipe]\n", argv[0]);
		return (1);
	}

	hPipe = CreateFile(argv[1], GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (hPipe == INVALID_HANDLE_VALUE) {
		ErrorExit("cus");
	}
	HANDLE hPipeToStd, hStdToPipe;
	struct io_handles ioPipeToStd, ioStdToPipe;
	DWORD idx, nCount;

	nCount = sizeof(waiters) / sizeof(waiters[0]);
	ioPipeToStd.read_handle = hPipe;
	ioPipeToStd.write_handle = hStdout;
	ioStdToPipe.read_handle = hStdin;
	ioStdToPipe.write_handle = hPipe;
	hPipeToStd = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)io_runner_PipeToCon, &ioPipeToStd, 0, NULL);
	hStdToPipe = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)io_runner_ConToPine, &ioStdToPipe, 0, NULL);
	waiters[0] = hPipeToStd;
	waiters[1] = hStdToPipe;

	for (;;) {
		idx = WaitForMultipleObjects(nCount, waiters, TRUE, INFINITE);
		if (idx == WAIT_TIMEOUT) {
			printf("WAIT timeout\n");
			break;
		}
		else if (idx == WAIT_FAILED) {
			ErrorExit("Wait");
		}
		else if (idx >= WAIT_OBJECT_0 && idx < WAIT_OBJECT_0 + nCount) {
			// Our threads must have exited
			CloseHandle(hPipeToStd);
			CloseHandle(hStdToPipe);
			break;
		}
		else if (idx >= WAIT_ABANDONED_0 && idx < WAIT_ABANDONED_0 + nCount) {
			printf("abandoned?(%d)", idx - WAIT_ABANDONED_0);
		}
		else
			printf("WAIT(%d)\n", idx);
	}
	restore_terminal();
	return (0);
}

VOID
ErrorExit(const char *lpszMessage)
{
	LPVOID lpMsgBuf;
	DWORD written;

	restore_terminal();
	fprintf(stderr, "%s: ", lpszMessage);
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)&lpMsgBuf, 0, NULL);
	WriteConsole(hStderr, lpMsgBuf, lstrlen((LPTSTR)lpMsgBuf), &written, NULL);
	LocalFree(lpMsgBuf);
	ExitProcess(1);
}

void
restore_terminal(void) {
	if (stdinModesaved) {
		SetConsoleMode(hStdin, fdwStdinSavedmode);
	}
	if (stdoutModesaved) {
		SetConsoleMode(hStdout, fdwStdoutSavedmode);
	}
}

DWORD WINAPI io_runner_PipeToCon(__in LPVOID lpParameter) {
	OVERLAPPED roverlap;
	struct io_handles *io = (struct io_handles *)lpParameter;
	unsigned __int8 buf[128];
	DWORD nlen;
	HANDLE revent;

	revent = CreateEvent(NULL, TRUE, TRUE, NULL);
	memset(&roverlap, 0, sizeof(roverlap));
	roverlap.hEvent = revent;

	for (;;) {
		if (!ReadFile(io->read_handle, buf, sizeof(buf), &nlen, &roverlap)) {
			DWORD error = GetLastError();
			if (error == ERROR_IO_PENDING) {
				if (!GetOverlappedResult(io->read_handle, &roverlap, &nlen, TRUE)) {
					ErrorExit("GetOverlapResult");
				}
			}
			else
				ErrorExit("read");
		}
		if (!WriteFile(io->write_handle, buf, nlen, &nlen, NULL)) {
			ErrorExit("write");
		}
	}
	return (0);
}

#define STATE_INIT 0
#define STATE_NEWLINE 1
#define STATE_TILDE 2
#define STATE_DOT 3

DWORD WINAPI io_runner_ConToPipe(__in LPVOID lpParameter) {
	int state = STATE_NEWLINE;
	OVERLAPPED woverlap;
	struct io_handles *io = (struct io_handles *)lpParameter;
	unsigned __int8 buf[128];
	DWORD nlen, i;
	HANDLE wevent;

	wevent = CreateEvent(NULL, TRUE, TRUE, NULL);
	memset(&woverlap, 0, sizeof(woverlap));
	woverlap.hEvent = wevent;

	for (;;) {
		DWORD error = WaitForSingleObject(io->read_handle, INFINITE);
		switch (error) {
		case WAIT_ABANDONED:
			printf("wait(abandoned?\n");
			return (2);
		case WAIT_OBJECT_0:
			// This is the only success case, I think.
			break;
		case WAIT_TIMEOUT:
			printf("wait(timeout)\n");
			return (1);
		case WAIT_FAILED:
			ErrorExit("wait");
		default:
			printf("Wait(%u)\n", error);
			return (3);
		}
		if (!ReadFile(io->read_handle, buf, sizeof(buf), &nlen, NULL)) {
				ErrorExit("read");
		}
		for (i = 0; i < nlen; i++) {
			switch (state) {
			case STATE_INIT:
				if (buf[i] == '\r')
					state = STATE_NEWLINE;
				break;
			case STATE_NEWLINE:
				if (buf[i] == '~')
					state = STATE_TILDE;
				else if (buf[i] == '\r')
					state = STATE_NEWLINE;
				else
					state = STATE_INIT;
				break;
			case STATE_TILDE:
				if (buf[i] == '.')
					goto thatsit;
				if (buf[i] == '\r')
					state = STATE_NEWLINE;
				else
					state = STATE_INIT;
			}
		}
		if (!WriteFile(io->write_handle, buf, nlen, &nlen, &woverlap)) {
			DWORD error = GetLastError();
			if (error == ERROR_IO_PENDING) {
				if (!GetOverlappedResult(io->write_handle, &woverlap, &nlen, TRUE)) {
					ErrorExit("GetOverlapResultX");
				}
			} else
				ErrorExit("write");
		}
	}
	return (0);

thatsit:
	restore_terminal();
	printf("Bye bye!\n");
	ExitProcess(0);
	return (0);
}
