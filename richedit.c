/*
 * rtfedit.c
 *
 * Minimal Win32 C89 Rich Text Editor.
 *
 * Features:
 *   - Native Rich Edit control
 *   - Open .RTF
 *   - Save .RTF
 *   - Save As
 *   - Basic Edit menu
 *
 * Build with something similar to:
 *
 *   cl rtfedit.c user32.lib gdi32.lib comdlg32.lib
 *
 * or MinGW:
 *
 *   gcc -std=c89 -mwindows rtfedit.c -o rtfedit.exe \
 *       -luser32 -lgdi32 -lcomdlg32
 */

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <stdio.h>
#include <string.h>

#ifndef DWORD_PTR
#define DWORD_PTR DWORD
#endif

#define WNDCLASS_NAME "RichTextPad"
#define WND_TITLE "RichTextPad"

#define IDM_OPEN        100
#define IDM_SAVE        101
#define IDM_SAVEAS      102
#define IDM_EXIT        103
#define IDM_UNDO        110
#define IDM_CUT         111
#define IDM_COPY        112
#define IDM_PASTE       113
#define IDM_SELECTALL   114

#define IDC_EDITOR      200

static HINSTANCE g_hInst;
static HWND       g_hwndEdit;
static HMODULE     g_hRichEdit;
static char       g_filename[MAX_PATH];

/*
 * ----------------------------------------------------------------------
 * RTF streaming callbacks
 * ----------------------------------------------------------------------
 */

static DWORD CALLBACK
StreamInCallback(DWORD_PTR dwCookie, LPBYTE pbBuff,
                 LONG cb, LONG *pcb)
{
    FILE *fp;

    fp = (FILE *)dwCookie;

    *pcb = (LONG)fread(pbBuff, 1, (size_t)cb, fp);

    if (ferror(fp))
        return 1;

    return 0;
}

static DWORD CALLBACK
StreamOutCallback(DWORD_PTR dwCookie, LPBYTE pbBuff,
                  LONG cb, LONG *pcb)
{
    FILE *fp;

    fp = (FILE *)dwCookie;

    *pcb = (LONG)fwrite(pbBuff, 1, (size_t)cb, fp);

    if (ferror(fp))
        return 1;

    return 0;
}

/*
 * ----------------------------------------------------------------------
 * File operations
 * ----------------------------------------------------------------------
 */

static int
LoadRTF(HWND hwndEdit, const char *filename)
{
    FILE *fp;
    EDITSTREAM es;

    fp = fopen(filename, "rb");

    if (fp == NULL)
    {
        MessageBox(hwndEdit,
                   "Unable to open the RTF file.",
                   "Open",
                   MB_OK | MB_ICONERROR);
        return 0;
    }

    memset(&es, 0, sizeof(es));

    es.dwCookie = (DWORD_PTR)fp;
    es.pfnCallback = StreamInCallback;

    SendMessage(hwndEdit,
                EM_STREAMIN,
                (WPARAM)SF_RTF,
                (LPARAM)&es);

    fclose(fp);

    if (es.dwError != 0)
    {
        MessageBox(hwndEdit,
                   "Unable to load the RTF file.",
                   "Open",
                   MB_OK | MB_ICONERROR);
        return 0;
    }

    strncpy(g_filename, filename, MAX_PATH - 1);
    g_filename[MAX_PATH - 1] = '\0';

    return 1;
}

static int
SaveRTF(HWND hwndEdit, const char *filename)
{
    FILE *fp;
    EDITSTREAM es;

    fp = fopen(filename, "wb");

    if (fp == NULL)
    {
        MessageBox(hwndEdit,
                   "Unable to create the RTF file.",
                   "Save",
                   MB_OK | MB_ICONERROR);
        return 0;
    }

    memset(&es, 0, sizeof(es));

    es.dwCookie = (DWORD_PTR)fp;
    es.pfnCallback = StreamOutCallback;

    SendMessage(hwndEdit,
                EM_STREAMOUT,
                (WPARAM)SF_RTF,
                (LPARAM)&es);

    fclose(fp);

    if (es.dwError != 0)
    {
        MessageBox(hwndEdit,
                   "Unable to save the RTF file.",
                   "Save",
                   MB_OK | MB_ICONERROR);
        return 0;
    }

    strncpy(g_filename, filename, MAX_PATH - 1);
    g_filename[MAX_PATH - 1] = '\0';

    return 1;
}

/*
 * ----------------------------------------------------------------------
 * File dialogs
 * ----------------------------------------------------------------------
 */

static int
Open_File(HWND hwnd)
{
    OPENFILENAME ofn;
    char filename[MAX_PATH];

    memset(&ofn, 0, sizeof(ofn));
    memset(filename, 0, sizeof(filename));

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter =
        "Rich Text Format (*.rtf)\0*.rtf\0"
        "All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST |
                OFN_HIDEREADONLY |
                OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = "rtf";

    if (!GetOpenFileName(&ofn))
        return 0;

    return LoadRTF(g_hwndEdit, filename);
}

static int
SaveAsFile(HWND hwnd)
{
    OPENFILENAME ofn;
    char filename[MAX_PATH];

    memset(&ofn, 0, sizeof(ofn));
    memset(filename, 0, sizeof(filename));

    if (g_filename[0] != '\0')
    {
        strncpy(filename, g_filename, MAX_PATH - 1);
        filename[MAX_PATH - 1] = '\0';
    }
    else
    {
        strcpy(filename, "document.rtf");
    }

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter =
        "Rich Text Format (*.rtf)\0*.rtf\0"
        "All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT |
                OFN_HIDEREADONLY |
                OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = "rtf";

    if (!GetSaveFileName(&ofn))
        return 0;

    return SaveRTF(g_hwndEdit, filename);
}

static int
SaveFile(HWND hwnd)
{
    if (g_filename[0] == '\0')
        return SaveAsFile(hwnd);

    return SaveRTF(g_hwndEdit, g_filename);
}

/*
 * ----------------------------------------------------------------------
 * Menu
 * ----------------------------------------------------------------------
 */

static HMENU
CreateMainMenu(void)
{
    HMENU menu;
    HMENU fileMenu;
    HMENU editMenu;

    menu = CreateMenu();

    fileMenu = CreatePopupMenu();

    AppendMenu(fileMenu, MF_STRING, IDM_OPEN,   "&Open...");
    AppendMenu(fileMenu, MF_STRING, IDM_SAVE,   "&Save");
    AppendMenu(fileMenu, MF_STRING, IDM_SAVEAS, "Save &As...");
    AppendMenu(fileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(fileMenu, MF_STRING, IDM_EXIT,   "E&xit");

    AppendMenu(menu, MF_POPUP,
               (UINT_PTR)fileMenu, "&File");

    editMenu = CreatePopupMenu();

    AppendMenu(editMenu, MF_STRING, IDM_UNDO,      "&Undo");
    AppendMenu(editMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(editMenu, MF_STRING, IDM_CUT,       "Cu&t");
    AppendMenu(editMenu, MF_STRING, IDM_COPY,      "&Copy");
    AppendMenu(editMenu, MF_STRING, IDM_PASTE,     "&Paste");
    AppendMenu(editMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(editMenu, MF_STRING, IDM_SELECTALL, "Select &All");

    AppendMenu(menu, MF_POPUP,
               (UINT_PTR)editMenu, "&Edit");

    return menu;
}

/*
 * ----------------------------------------------------------------------
 * Window procedure
 * ----------------------------------------------------------------------
 */

static LRESULT CALLBACK
WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        {
            g_hwndEdit = CreateWindowEx(
                WS_EX_CLIENTEDGE,
                RICHEDIT_CLASSA,
                "",
                WS_CHILD |
                WS_VISIBLE |
                WS_VSCROLL |
                WS_HSCROLL |
                ES_MULTILINE |
                ES_AUTOVSCROLL |
                ES_AUTOHSCROLL |
                ES_NOHIDESEL,
                0, 0, 0, 0,
                hwnd,
                (HMENU)IDC_EDITOR,
                g_hInst,
                NULL);

            if (g_hwndEdit == NULL)
                return -1;

            /*
             * Allow reasonably large RTF documents.
             */
            SendMessage(g_hwndEdit,
                        EM_EXLIMITTEXT,
                        0,
                        (LPARAM)0x7ffffffe);

            return 0;
        }

    case WM_SIZE:
        if (g_hwndEdit != NULL)
        {
            MoveWindow(g_hwndEdit,
                       0, 0,
                       LOWORD(lParam),
                       HIWORD(lParam),
                       TRUE);
        }
        return 0;

    case WM_SETFOCUS:
        if (g_hwndEdit != NULL)
            SetFocus(g_hwndEdit);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDM_OPEN:
            Open_File(hwnd);
            return 0;

        case IDM_SAVE:
            SaveFile(hwnd);
            return 0;

        case IDM_SAVEAS:
            SaveAsFile(hwnd);
            return 0;

        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;

        case IDM_UNDO:
            SendMessage(g_hwndEdit, WM_UNDO, 0, 0);
            return 0;

        case IDM_CUT:
            SendMessage(g_hwndEdit, WM_CUT, 0, 0);
            return 0;

        case IDM_COPY:
            SendMessage(g_hwndEdit, WM_COPY, 0, 0);
            return 0;

        case IDM_PASTE:
            SendMessage(g_hwndEdit, WM_PASTE, 0, 0);
            return 0;

        case IDM_SELECTALL:
            SendMessage(g_hwndEdit, EM_SETSEL, 0, -1);
            return 0;
        }

        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/*
 * ----------------------------------------------------------------------
 * WinMain
 * ----------------------------------------------------------------------
 */

int WINAPI
WinMain(HINSTANCE hInstance,
        HINSTANCE hPrevInstance,
        LPSTR lpCmdLine,
        int nCmdShow)
{
    WNDCLASS wc;
    HWND hwnd;
    MSG msg;
    HMENU menu;

    (void)hPrevInstance;
    (void)lpCmdLine;

    g_hInst = hInstance;
    g_filename[0] = '\0';

    /*
     * RICHEDIT_CLASSA is supplied by RichEdit 2.0 and later.
     *
     * Loading the DLL explicitly also makes the program work on
     * systems where the Rich Edit DLL has not yet been loaded.
     */
    g_hRichEdit = LoadLibrary("RICHED20.DLL");

    if (g_hRichEdit == NULL)
    {
        MessageBox(NULL,
                   "Unable to load RICHED20.DLL.",
                   WND_TITLE,
                   MB_OK | MB_ICONERROR);
        return 1;
    }

    memset(&wc, 0, sizeof(wc));

    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = WNDCLASS_NAME;

    if (!RegisterClass(&wc))
    {
        FreeLibrary(g_hRichEdit);
        return 1;
    }

    menu = CreateMainMenu();

    hwnd = CreateWindow(
        WNDCLASS_NAME,
        WND_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        800,
        600,
        NULL,
        menu,
        hInstance,
        NULL);

    if (hwnd == NULL)
    {
        FreeLibrary(g_hRichEdit);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    FreeLibrary(g_hRichEdit);

    return (int)msg.wParam;
}
