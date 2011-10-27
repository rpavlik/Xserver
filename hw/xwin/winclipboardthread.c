/*
 *Copyright (C) 2003-2004 Harold L Hunt II All Rights Reserved.
 *Copyright (C) Colin Harrison 2005-2008
 *
 *Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 *"Software"), to deal in the Software without restriction, including
 *without limitation the rights to use, copy, modify, merge, publish,
 *distribute, sublicense, and/or sell copies of the Software, and to
 *permit persons to whom the Software is furnished to do so, subject to
 *the following conditions:
 *
 *The above copyright notice and this permission notice shall be
 *included in all copies or substantial portions of the Software.
 *
 *THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *NONINFRINGEMENT. IN NO EVENT SHALL HAROLD L HUNT II BE LIABLE FOR
 *ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 *WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *Except as contained in this notice, the name of the copyright holder(s)
 *and author(s) shall not be used in advertising or otherwise to promote
 *the sale, use or other dealings in this Software without prior written
 *authorization from the copyright holder(s) and author(s).
 *
 * Authors:	Harold L Hunt II
 *              Colin Harrison
 */

#ifdef HAVE_XWIN_CONFIG_H
#include <xwin-config.h>
#endif
#include <sys/types.h>
#include "winclipboard.h"
#ifdef __CYGWIN__
#include <errno.h>
#endif
#include "misc.h"
#include "winprefs.h"

extern void winSetAuthorization(void);

/*
 * References to external symbols
 */

extern Bool		g_fUnicodeClipboard;
extern unsigned long	serverGeneration;
extern Bool		g_fClipboardStarted;
extern Bool             g_fClipboardLaunched;
extern Bool             g_fClipboard;
extern HWND		g_hwndClipboard;
extern void		*g_pClipboardDisplay;
extern Window		g_iClipboardWindow;
extern WINPREFS		pref;


/*
 * Global variables
 */

static jmp_buf			g_jmpEntry;

static XIOErrorHandler g_winClipboardOldIOErrorHandler;
static pthread_t g_winClipboardProcThread;
static int clipboardRestarts = 0;


/*
 * Functions used by other threads
 */
void
winThreadExit(void *args);


/*
 * Local function prototypes
 */

static int
winClipboardErrorHandler (Display *pDisplay, XErrorEvent *pErr);

static int
winClipboardIOErrorHandler (Display *pDisplay);

static BOOL CALLBACK
winTerminateAppEnum(HWND hwnd, LPARAM lParam);


/*
 * Main thread function
 */

void *
winClipboardProc (void *pvNotUsed)
{
  Atom			atomClipboard, atomClipboardManager;
  int			iReturn;
  HWND			hwnd = NULL;
  Display		*pDisplay = NULL;
  Window		iWindow = None;
  int			iRetries;
  char			szDisplay[512];
  Bool			OverDone = FALSE;

  ErrorF ("winClipboardProc - Hello\n");
  ++clipboardRestarts;

  /* Allow multiple threads to access Xlib */
  if (XInitThreads () == 0)
    {
      ErrorF ("winClipboardProc - XInitThreads failed.\n");
      /* disable the clipboard, which means the thread will die */
      g_fClipboard = FALSE;
      goto winClipboardProc_Done;
    }

  /* See if X supports the current locale */
  if (XSupportsLocale () == False)
    {
      ErrorF ("winClipboardProc - Warning: Locale not supported by X.\n");
    }

  /* Create Windows messaging window */
  hwnd = winClipboardCreateMessagingWindow ();

  /* Save copy of HWND in screen privates */
  g_hwndClipboard = hwnd;

  /* Set error handler */
  XSetErrorHandler (winClipboardErrorHandler);
  g_winClipboardProcThread = pthread_self();
  g_winClipboardOldIOErrorHandler = XSetIOErrorHandler (winClipboardIOErrorHandler);

  /* Set jump point for Error exits */
  iReturn = setjmp (g_jmpEntry);
  
  /* Check if we should continue operations */
  if (iReturn != WIN_JMP_ERROR_IO
      && iReturn != WIN_JMP_OKAY)
    {
      /* setjmp returned an unknown value, exit */
      ErrorF ("winClipboardProc - setjmp returned: %d.  Exiting.\n",
	      iReturn);
      /* disable the clipboard, which means the thread will die */
      g_fClipboard = FALSE;
      goto winClipboardProc_Done;
    }
  else if (iReturn == WIN_JMP_ERROR_IO)
    {
      /* TODO: Cleanup the Win32 window and free any allocated memory */
      ErrorF ("winClipboardProc - setjmp returned for IO Error Handler.\n");
      goto winClipboardProc_Done;
    }

  /* Use our generated cookie for authentication */
  winSetAuthorization();

  /* Initialize retry count */
  iRetries = 0;

  /* Setup the display connection string x */
  /*
   * NOTE: Always connect to screen 0 since we require that screen
   * numbers start at 0 and increase without gaps.  We only need
   * to connect to one screen on the display to get events
   * for all screens on the display.  That is why there is only
   * one clipboard client thread.
   */
  snprintf (szDisplay,
	    512,
	    "127.0.0.1:%s.0",
	    display);

  /* Print the display connection string */
  ErrorF ("winClipboardProc - DISPLAY=%s\n", szDisplay);

  /* Open the X display */
  do
    {
      pDisplay = XOpenDisplay (szDisplay);
      if (pDisplay == NULL)
	{
	  ErrorF ("winClipboardProc - Could not open display, "
		  "try: %d, sleeping: %d\n",
		  iRetries + 1, WIN_CONNECT_DELAY);
	  ++iRetries;
	  sleep (WIN_CONNECT_DELAY);
	  continue;
	}
      else
	break;
    }
  while (pDisplay == NULL && iRetries < WIN_CONNECT_RETRIES);

  /* Make sure that the display opened */
  if (pDisplay == NULL)
    {
      ErrorF ("winClipboardProc - Failed opening the display, giving up\n");
      goto winClipboardProc_Done;
    }

  /* Save the display in a global used by the wndproc */
  g_pClipboardDisplay = pDisplay;

  ErrorF ("winClipboardProc - XOpenDisplay () returned and "
	  "successfully opened the display.\n");

  /* Create atoms */
  atomClipboard = XInternAtom (pDisplay, "CLIPBOARD", False);
  atomClipboardManager = XInternAtom (pDisplay, "CLIPBOARD_MANAGER", False);

  /* Create a messaging window */
  iWindow = XCreateSimpleWindow (pDisplay,
				 DefaultRootWindow (pDisplay),
				 -1, -1,
				 1, 1,
				 0,
				 BlackPixel (pDisplay, 0),
				 BlackPixel (pDisplay, 0));
  if (iWindow == 0)
    {
      ErrorF ("winClipboardProc - Could not create an X window.\n");
      goto winClipboardProc_Done;
    }

  XStoreName(pDisplay, iWindow, "xwinclip");

  /* Select event types to watch */
  if (XSelectInput (pDisplay,
		    iWindow,
		    PropertyChangeMask) == BadWindow)
    ErrorF ("winClipboardProc - XSelectInput generated BadWindow "
	    "on messaging window\n");

  /* Save the window in the screen privates */
  g_iClipboardWindow = iWindow;

  /* Pre-flush X events */
  /* 
   * NOTE: Apparently you'll freeze if you don't do this,
   *	   because there may be events in local data structures
   *	   already.
   */
  winClipboardFlushXEvents (hwnd,
			    iWindow,
			    pDisplay,
			    g_fUnicodeClipboard,
			    0);

  /* Pre-flush Windows messages */
  if (!winClipboardFlushWindowsMessageQueue (hwnd))
    {
      ErrorF ("winClipboardProc - winClipboardFlushWindowsMessageQueue failed\n");
      goto winClipboardProc_Done;
    }

  XFlush (pDisplay);

  /* PRIMARY */
  iReturn = XSetSelectionOwner (pDisplay, XA_PRIMARY,
				iWindow, CurrentTime);
  if (iReturn == BadAtom || iReturn == BadWindow ||
      XGetSelectionOwner (pDisplay, XA_PRIMARY) != iWindow)
    {
      ErrorF ("winClipboardProc - Could not set PRIMARY owner\n");
      goto winClipboardProc_Done;
    }

  /* CLIPBOARD */
  iReturn = XSetSelectionOwner (pDisplay, atomClipboard,
				iWindow, CurrentTime);
  if (iReturn == BadAtom || iReturn == BadWindow ||
      XGetSelectionOwner (pDisplay, atomClipboard) != iWindow)
    {
      ErrorF ("winClipboardProc - Could not set CLIPBOARD owner\n");
      goto winClipboardProc_Done;
    }

  /* Signal that the clipboard client has started */
  g_fClipboardStarted = TRUE;

  /* Loop for X events */
  while (1)
    {
      /* Process Windows messages */
      if (!winClipboardFlushWindowsMessageQueue (hwnd))
	{
	  ErrorF ("winClipboardProc - "
		  "winClipboardFlushWindowsMessageQueue trapped "
		  "WM_QUIT message, exiting main loop.\n");
	  break;
	}
      iReturn = winClipboardFlushXEvents (hwnd,
					  iWindow,
					  pDisplay,
					  g_fUnicodeClipboard,
					  0);
      if (WIN_XEVENTS_SHUTDOWN == iReturn)
	{
	  ErrorF ("winClipboardProc - winClipboardFlushXEvents "
		  "trapped shutdown event, exiting main loop.\n");
	  break;
	}
      Sleep (100);
    }
  /* We get here when the server is about to regenerate or shutdown. */
  OverDone = TRUE;
  clipboardRestarts = 0;

winClipboardProc_Done:
  /* Close our Windows window */
  if (!OverDone && g_hwndClipboard)
    {
      /* Destroy the Window window (hwnd) */
      winDebug("winClipboardProc - Destroy Windows window\n");
      PostMessage(g_hwndClipboard, WM_DESTROY, 0, 0);
      winClipboardFlushWindowsMessageQueue(g_hwndClipboard);
    }

  /* Close our X window */
  if (pDisplay && iWindow)
    {
      iReturn = XDestroyWindow (pDisplay, iWindow);
      if (iReturn == BadWindow)
	ErrorF ("winClipboardProc - XDestroyWindow returned BadWindow.\n");
      else
	ErrorF ("winClipboardProc - XDestroyWindow succeeded.\n");
    }


#ifdef HAS_DEVWINDOWS
  /* Close our Win32 message handle */
  if (fdMessageQueue)
    close (fdMessageQueue);
#endif

#if 0
  /*
   * FIXME: XCloseDisplay hangs if we call it, as of 2004/03/26.  The
   * XSync and XSelectInput calls did not help.
   */

  /* Discard any remaining events */
  XSync (pDisplay, TRUE);

  /* Select event types to watch */
  XSelectInput (pDisplay,
		DefaultRootWindow (pDisplay),
		None);

  /* Close our X display */
  if (pDisplay)
    {
      XCloseDisplay (pDisplay);
    }
#endif

  /* global clipboard variable reset */
  g_fClipboardLaunched = FALSE;
  g_fClipboardStarted = FALSE;
  g_iClipboardWindow = None;
  g_pClipboardDisplay = NULL;
  g_hwndClipboard = NULL;

  if (OverDone) return NULL;

  /* checking if we need to restart */
  if (clipboardRestarts >= WIN_CLIPBOARD_RETRIES)
    {
      /* terminates clipboard thread but the main server still lives */
      ErrorF("winClipboardProc - the clipboard thread has restarted %d times and seems to be unstable, disabling clipboard integration\n",  clipboardRestarts);
      g_fClipboard = FALSE;
      return NULL;
    }

  if (g_fClipboard)
    {
      sleep(WIN_CLIPBOARD_DELAY);
      ErrorF("winClipboardProc - trying to restart clipboard thread \n");
      /* Create the clipboard client thread */
      if (!winInitClipboard ())
        {
          ErrorF ("winClipboardProc - winClipboardInit failed.\n");
          return NULL;
        }

      winDebug ("winClipboardProc - winInitClipboard returned.\n");
      /* Flag that clipboard client has been launched */
      g_fClipboardLaunched = TRUE;
    }
  else
    {
      ErrorF ("winClipboardProc - Clipboard disabled  - Exit from server \n");
      /* clipboard thread has exited, stop server as well */
      winThreadExit(NULL);
    }

  return NULL;
}


/*
 * winClipboardErrorHandler - Our application specific error handler
 */

static int
winClipboardErrorHandler (Display *pDisplay, XErrorEvent *pErr)
{
  char pszErrorMsg[100];
  
  XGetErrorText (pDisplay,
		 pErr->error_code,
		 pszErrorMsg,
		 sizeof (pszErrorMsg));
  ErrorF ("winClipboardErrorHandler - ERROR: \n\t%s\n"
	  "\tError Code: %d, Serial: %lu, Resource ID: 0x%x\n\tRequest Code: %d, Minor Code: %d\n",
	  pszErrorMsg,
	  pErr->error_code,
	  pErr->serial,
	  (int)pErr->resourceid,
	  pErr->request_code,
	  pErr->minor_code);
  return 0;
}


/*
 * winClipboardIOErrorHandler - Our application specific IO error handler
 */

static int
winClipboardIOErrorHandler (Display *pDisplay)
{
  ErrorF ("winClipboardIOErrorHandler!\n");

  if (pthread_equal(pthread_self(),g_winClipboardProcThread))
    {
      /* Restart at the main entry point */
      longjmp (g_jmpEntry, WIN_JMP_ERROR_IO);
    }

  if (g_winClipboardOldIOErrorHandler)
    g_winClipboardOldIOErrorHandler(pDisplay);

  return 0;
}


/*
 * winTerminateAppEnum - Post WM_CLOSE to windows whose PID matches the process
 */

static BOOL CALLBACK
winTerminateAppEnum(HWND hwnd, LPARAM lParam)
{
  DWORD dwID;

  GetWindowThreadProcessId(hwnd, &dwID);

  if(dwID == (DWORD)lParam) PostMessage(hwnd, WM_CLOSE, 0, 0);

  return TRUE;
}


/*
 * winThreadExit - Thread exit handler
 */

void
winThreadExit(void *arg)
{
  /* Thread has exited, stop server as well */
  DWORD dwTimeout = 500; /* If process has not signalled in 500msec just kill it anyway */
  DWORD dwProcessId = GetCurrentProcessId();
  HANDLE hProcess = NULL;

  if (dwProcessId > 0) hProcess = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, dwProcessId);

  if (hProcess == NULL) return;

  pref.fForceExit = TRUE;

  EnumWindows((WNDENUMPROC)winTerminateAppEnum, (LPARAM)dwProcessId);

  if (WaitForSingleObject(hProcess, dwTimeout) != WAIT_OBJECT_0) TerminateProcess(hProcess, 1);

  CloseHandle(hProcess);
}
