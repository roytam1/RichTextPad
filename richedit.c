/*
 * rtfedit.c
 *
 * Minimal Win32 C89 Rich Text Editor.
 *
 * Features:
 *   - Native Rich Edit control (Msftedit 4.1+ with RichEdit 3.0 fallback)
 *   - Open .RTF / .MD (markdown via md<->rtf layer, tables supported)
 *   - Save .RTF / .MD
 *   - Save As
 *   - Basic Edit menu
 *   - Markdown source / rich view toggle
 *   - Clickable links (EN_LINK opens default browser)
 *
 * Build with something similar to:
 *
 *   cl rtfedit.c user32.lib gdi32.lib comdlg32.lib shell32.lib
 *
 * or MinGW:
 *
 *   gcc -std=c89 -mwindows rtfedit.c -o rtfedit.exe \
 *       -luser32 -lgdi32 -lcomdlg32 -lshell32
 */

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <tchar.h>

#ifndef IDC_HAND
#define IDC_HAND MAKEINTRESOURCE(32649)
#endif

#ifndef CFM_LINK
#define CFM_LINK 0x02000000
#endif
#ifndef CFE_LINK
#define CFE_LINK CFM_LINK
#endif

/* DWORD_PTR carries pointers (EDITSTREAM cookies). The fallback is
   32-bit only: on 64-bit the SDK type must be used, otherwise stream
   cookies get truncated to 32 bits. */
#ifndef _WIN64
#ifndef DWORD_PTR
#define DWORD_PTR DWORD
#endif
#ifndef UINT_PTR
#define UINT_PTR UINT
#endif
#ifndef INT_PTR
#define INT_PTR INT
#endif
#endif
#ifndef SCF_ALL
#define SCF_ALL 0x0004
#endif
#ifndef RICHEDIT_CLASS
#define RICHEDIT_CLASS "RichEdit20W"
#endif
#ifndef RICHEDIT_CLASSA
#define RICHEDIT_CLASSA "RichEdit20A"
#endif
#ifndef EM_FINDTEXTEXW
#define EM_FINDTEXTEXW (WM_USER + 124)
#endif
#ifndef EM_AUTOURLDETECT
#define EM_AUTOURLDETECT (WM_USER + 91)
#endif
#ifndef ENM_LINK
#define ENM_LINK 0x04000000
#endif
#ifndef EN_LINK
#define EN_LINK 0x070b
#endif
#ifndef SF_UNICODE
#define SF_UNICODE 0x0010
#endif
#ifndef SF_USECODEPAGE
#define SF_USECODEPAGE 0x0020
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

/* Plain-text streaming must carry SF_UNICODE on Unicode controls:
   without it the control transfers ANSI (UTF-16 bytes streamed in
   land as one character per byte, NULs showing as spaces; streamed
   out they come back ANSI, not UTF-16). */
#ifdef UNICODE
#define SF_TEXT_EX (SF_TEXT | SF_UNICODE)
#else
#define SF_TEXT_EX SF_TEXT
#endif

#if defined(_MSC_VER) && _MSC_VER < 1200
typedef struct _enlink {
  NMHDR     nmhdr;
  UINT      msg;
  WPARAM    wParam;
  LPARAM    lParam;
  CHARRANGE chrg;
} ENLINK;

typedef struct __charformat {
  UINT     cbSize;
  DWORD    dwMask;
  DWORD    dwEffects;
  LONG     yHeight;
  LONG     yOffset;
  COLORREF crTextColor;
  BYTE     bCharSet;
  BYTE     bPitchAndFamily;
  char     szFaceName[LF_FACESIZE];
} CHARFORMATA;

typedef struct _findtextexw {
  CHARRANGE chrg;
  LPCWSTR   lpstrText;
  CHARRANGE chrgText;
} FINDTEXTEXW;
#endif

#define WNDCLASS_NAME _T("RichTextPad")
#define WND_TITLE _T("RichTextPad")

#define IDM_OPEN        100
#define IDM_SAVE        101
#define IDM_SAVEAS      102
#define IDM_EXIT        103
#define IDM_UNDO        110
#define IDM_CUT         111
#define IDM_COPY        112
#define IDM_PASTE       113
#define IDM_SELECTALL   114
#define IDM_SHOW_SOURCE 115

#define IDC_EDITOR      200

static HINSTANCE g_hInst;
static HWND       g_hwndMain;
static HWND       g_hwndEdit;
static HMODULE     g_hRichEdit;
static LPCTSTR     g_editClass;
static TCHAR       g_filename[MAX_PATH];
static int        g_showSource;
static int        g_isRE10;

static void reset_source_format(void);
static void apply_link_effects(const char *md);
static char *stream_editor_text_out(HWND hwndEdit);
static int stream_editor_text_in(HWND hwndEdit, const char *text);

/*
 * ----------------------------------------------------------------------
 * Title bar
 * ----------------------------------------------------------------------
 */

static void
UpdateTitle(void)
{
    TCHAR title[MAX_PATH + 32];

    if (g_filename[0] == _T('\0'))
    {
        _tcscpy(title, WND_TITLE _T(" - Untitled"));
    }
    else
    {
        _stprintf(title, _T("%s - %s"), WND_TITLE, g_filename);
    }

    if (g_hwndMain != NULL)
    {
        SetWindowText(g_hwndMain, title);
    }
}

/*
 * ----------------------------------------------------------------------
 * Command line
 * ----------------------------------------------------------------------
 */

static int
GetCmdLineFile(LPTSTR lpCmdLine, LPTSTR out, int outSize)
{
    TCHAR *p;
    TCHAR *end;
    size_t len;

    if (lpCmdLine == NULL || out == NULL || outSize <= 1)
        return 0;

    p = lpCmdLine;

    while (*p == _T(' ') || *p == _T('\t'))
        p++;

    if (*p == _T('\0'))
        return 0;

    if (*p == _T('"'))
    {
        p++;
        end = _tcschr(p, _T('"'));
        if (end != NULL)
            len = (size_t)(end - p);
        else
            len = _tcslen(p);
    }
    else
    {
        /* Take whole remainder so paths with spaces work even unquoted. */
        end = p + _tcslen(p);
        while (end > p && (*(end - 1) == _T(' ') || *(end - 1) == _T('\t')))
            end--;
        len = (size_t)(end - p);
        /* Strip one pair of surrounding quotes, just in case. */
        if (len >= 2 && p[0] == _T('"') && p[len - 1] == _T('"'))
        {
            p++;
            len -= 2;
        }
    }

    while (len > 0 && (p[len - 1] == _T(' ') || p[len - 1] == _T('\t')))
        len--;

    if (len == 0)
        return 0;

    if (len > (size_t)(outSize - 1))
        len = (size_t)(outSize - 1);

    memcpy(out, p, len * sizeof(TCHAR));
    out[len] = _T('\0');

    return 1;
}

/*
 * ----------------------------------------------------------------------
 * Growable string buffer (C89)
 * ----------------------------------------------------------------------
 */

typedef struct
{
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static void
sb_init(StrBuf *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static void
sb_free(StrBuf *sb)
{
    if (sb->data != NULL)
        free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static int
sb_reserve(StrBuf *sb, size_t extra)
{
    size_t need;
    size_t newcap;
    char *nd;

    need = sb->len + extra + 1;
    if (need <= sb->cap)
        return 1;

    newcap = sb->cap != 0 ? sb->cap : 256;
    while (newcap < need)
        newcap *= 2;

    nd = (char *)realloc(sb->data, newcap);
    if (nd == NULL)
        return 0;

    sb->data = nd;
    sb->cap = newcap;
    if (sb->len == 0)
        sb->data[0] = '\0';
    return 1;
}

static int
sb_append_n(StrBuf *sb, const char *s, size_t n)
{
    if (n == 0)
    {
        if (sb->data == NULL)
        {
            if (!sb_reserve(sb, 1))
                return 0;
        }
        return 1;
    }
    if (!sb_reserve(sb, n))
        return 0;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 1;
}

static int
sb_append_str(StrBuf *sb, const char *s)
{
    return sb_append_n(sb, s, strlen(s));
}

static int
sb_append_char(StrBuf *sb, char c)
{
    return sb_append_n(sb, &c, 1);
}

/*
 * UTF-8 decode (returns 1 on success).
 */
static int
utf8_decode(const unsigned char *s, size_t avail,
            unsigned long *cp, int *nbytes)
{
    unsigned char c;

    if (avail == 0)
        return 0;
    c = s[0];
    if (c < 0x80)
    {
        *cp = c;
        *nbytes = 1;
        return 1;
    }
    else if ((c & 0xE0) == 0xC0)
    {
        if (avail < 2)
            return 0;
        if ((s[1] & 0xC0) != 0x80)
            return 0;
        *cp = ((unsigned long)(c & 0x1F) << 6) |
              (unsigned long)(s[1] & 0x3F);
        if (*cp < 0x80)
            return 0;
        *nbytes = 2;
        return 1;
    }
    else if ((c & 0xF0) == 0xE0)
    {
        if (avail < 3)
            return 0;
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80)
            return 0;
        *cp = ((unsigned long)(c & 0x0F) << 12) |
              ((unsigned long)(s[1] & 0x3F) << 6) |
              (unsigned long)(s[2] & 0x3F);
        if (*cp < 0x800)
            return 0;
        *nbytes = 3;
        return 1;
    }
    else if ((c & 0xF8) == 0xF0)
    {
        if (avail < 4)
            return 0;
        if ((s[1] & 0xC0) != 0x80 ||
            (s[2] & 0xC0) != 0x80 ||
            (s[3] & 0xC0) != 0x80)
            return 0;
        *cp = ((unsigned long)(c & 0x07) << 18) |
              ((unsigned long)(s[1] & 0x3F) << 12) |
              ((unsigned long)(s[2] & 0x3F) << 6) |
              (unsigned long)(s[3] & 0x3F);
        if (*cp < 0x10000 || *cp > 0x10FFFF)
            return 0;
        *nbytes = 4;
        return 1;
    }
    return 0;
}

static int
utf8_encode(unsigned long cp, char *out)
{
    if (cp < 0x80)
    {
        out[0] = (char)cp;
        return 1;
    }
    else if (cp < 0x800)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    else if (cp < 0x10000)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    else
    {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

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
 * Markdown -> RTF (MVP + tables, C89)
 * ----------------------------------------------------------------------
 */

#define MD_MAX_COLS 16
#define MD_MAX_ROWS 128

static int
rtf_append_codepoint(StrBuf *out, unsigned long cp)
{
    char tmp[48];
    long v;
    long hi;
    long lo;

    if (cp == 0)
        return 1;
    if (cp < 0x20)
    {
        if (cp == 9)
            return sb_append_str(out, "\\tab ");
        return sb_append_char(out, ' ');
    }
    if (cp <= 0xFFFF)
    {
        if (cp >= 0xD800 && cp <= 0xDFFF)
            cp = '?';
        v = (long)cp;
        if (v > 32767)
            v -= 65536;
        sprintf(tmp, "\\u%ld?", v);
        return sb_append_str(out, tmp);
    }
    cp -= 0x10000;
    hi = 0xD800 + (long)(cp >> 10);
    lo = 0xDC00 + (long)(cp & 0x3FF);
    if (hi > 32767)
        hi -= 65536;
    if (lo > 32767)
        lo -= 65536;
    sprintf(tmp, "\\u%ld?\\u%ld?", hi, lo);
    return sb_append_str(out, tmp);
}

static int
rtf_append_text(StrBuf *out, const char *s, size_t n)
{
    size_t i;

    i = 0;
    while (i < n)
    {
        unsigned char c;
        unsigned long cp;
        int nb;

        c = (unsigned char)s[i];
        if (c == '{' || c == '}' || c == '\\')
        {
            if (!sb_append_char(out, '\\'))
                return 0;
            if (!sb_append_char(out, (char)c))
                return 0;
            i++;
        }
        else if (c < 0x80)
        {
            if (c == '\t')
            {
                if (!sb_append_str(out, "\\tab "))
                    return 0;
            }
            else if (c < 0x20)
            {
                if (!sb_append_char(out, ' '))
                    return 0;
            }
            else
            {
                if (!sb_append_char(out, (char)c))
                    return 0;
            }
            i++;
        }
        else
        {
            if (utf8_decode((const unsigned char *)s + i, n - i, &cp, &nb))
            {
                if (!rtf_append_codepoint(out, cp))
                    return 0;
                i += (size_t)nb;
            }
            else
            {
                if (!sb_append_char(out, '?'))
                    return 0;
                i++;
            }
        }
    }
    return 1;
}

static int
rtf_append_field_url(StrBuf *out, const char *s, size_t n)
{
    size_t i;

    i = 0;
    while (i < n)
    {
        char c;

        c = s[i];
        if (c == '"')
        {
            if (!sb_append_str(out, "\\'22"))
                return 0;
        }
        else if (c == '\\' || c == '{' || c == '}')
        {
            if (!sb_append_char(out, '\\'))
                return 0;
            if (!sb_append_char(out, c))
                return 0;
        }
        else
        {
            if (!sb_append_char(out, c))
                return 0;
        }
        i++;
    }
    return 1;
}

static int
md_is_blank(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r')
        s++;
    return *s == '\0';
}

static int
md_heading(const char *s, const char **content)
{
    int level;

    level = 0;
    while (level < 6 && s[level] == '#')
        level++;
    if (level == 0 || level > 6)
        return 0;
    if (s[level] != ' ' && s[level] != '\t' && s[level] != '\0')
        return 0;
    s += level;
    if (*s == ' ' || *s == '\t')
        s++;
    *content = s;
    return level;
}

static int
md_is_hrule(const char *s)
{
    char c;
    int n;

    while (*s == ' ' || *s == '\t')
        s++;
    c = *s;
    if (c != '-' && c != '*' && c != '_')
        return 0;
    n = 0;
    while (*s != '\0')
    {
        if (*s == c)
            n++;
        else if (*s != ' ' && *s != '\t')
            return 0;
        s++;
    }
    return n >= 3;
}

static int
md_parse_ul(const char *s, int *indent, const char **content)
{
    int sp;

    sp = 0;
    while (s[sp] == ' ')
        sp++;
    if (s[sp] == '-' || s[sp] == '*' || s[sp] == '+')
    {
        if (s[sp + 1] == ' ' || s[sp + 1] == '\t')
        {
            *indent = sp / 2;
            s += sp + 1;
            while (*s == ' ' || *s == '\t')
                s++;
            *content = s;
            return 1;
        }
    }
    return 0;
}

static int
md_parse_ol(const char *s, int *indent, const char **content,
            const char **numstart, size_t *numlen)
{
    int sp;
    int d;

    sp = 0;
    while (s[sp] == ' ')
        sp++;
    d = sp;
    while (s[d] >= '0' && s[d] <= '9')
        d++;
    if (d > sp && s[d] == '.' &&
        (s[d + 1] == ' ' || s[d + 1] == '\t'))
    {
        *indent = sp / 2;
        *numstart = s + sp;
        *numlen = (size_t)(d - sp);
        s += d + 1;
        while (*s == ' ' || *s == '\t')
            s++;
        *content = s;
        return 1;
    }
    return 0;
}

static int
md_parse_quote(const char *s, const char **content)
{
    while (*s == ' ')
        s++;
    if (*s != '>')
        return 0;
    s++;
    if (*s == ' ' || *s == '\t')
        s++;
    *content = s;
    return 1;
}

static int
md_is_fence(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s[0] == '`' && s[1] == '`' && s[2] == '`';
}

static int
md_is_table_sep(const char *s)
{
    int dash;

    dash = 0;
    while (*s != '\0')
    {
        if (*s == '-')
            dash++;
        else if (*s != '|' && *s != ':' &&
                 *s != ' ' && *s != '\t')
            return 0;
        s++;
    }
    return dash > 0;
}

static int
md_cell_align(const char *cell)
{
    const char *p;
    const char *e;
    int left;
    int right;

    p = cell;
    while (*p == ' ' || *p == '\t')
        p++;
    e = p + strlen(p);
    while (e > p && (*(e - 1) == ' ' || *(e - 1) == '\t'))
        e--;
    if (e - p < 3)
        return 0;
    left = (*p == ':');
    right = (*(e - 1) == ':');
    if (left && right)
        return 1;
    if (right)
        return 2;
    return 0;
}

/* Split "| a | b |" into NUL-terminated copies. Returns ncol. */
static int
md_split_row(const char *line, char cells[MD_MAX_COLS][1024])
{
    const char *p;
    const char *e;
    int ncol;

    ncol = 0;
    p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    e = p + strlen(p);
    while (e > p && (*(e - 1) == ' ' || *(e - 1) == '\t' ||
                     *(e - 1) == '\r'))
        e--;
    if (e > p && *p == '|')
        p++;
    if (e > p && *(e - 1) == '|')
        e--;
    while (p < e && ncol < MD_MAX_COLS)
    {
        const char *q;
        const char *qe;
        size_t len;

        q = p;
        while (p < e && *p != '|')
            p++;
        qe = p;
        while (q < qe && (*q == ' ' || *q == '\t'))
            q++;
        while (qe > q && (*(qe - 1) == ' ' || *(qe - 1) == '\t'))
            qe--;
        len = (size_t)(qe - q);
        if (len > 1023)
            len = 1023;
        memcpy(cells[ncol], q, len);
        cells[ncol][len] = '\0';
        ncol++;
        if (p < e && *p == '|')
            p++;
        while (p < e && (*p == ' ' || *p == '\t'))
        {
            /* keep single pass, trim happens per cell */
            break;
        }
    }
    return ncol;
}

static int
md_inline_to_rtf(StrBuf *out, const char *s)
{
    int bold;
    int italic;
    int mono;
    size_t i;
    size_t n;

    bold = 0;
    italic = 0;
    mono = 0;
    n = strlen(s);
    i = 0;
    while (i < n)
    {
        char c;
        unsigned char uc;

        c = s[i];
        uc = (unsigned char)c;
        if (mono)
        {
            if (c == '`')
            {
                mono = 0;
                if (!sb_append_str(out, "\\f0 "))
                    return 0;
                i++;
            }
            /* Inside code spans backslashes are literal (no escaping).
               Emit everything raw so "`a\\b`" stays "`a\\b`". */
            else if (uc < 0x80)
            {
                if (!rtf_append_text(out, s + i, 1))
                    return 0;
                i++;
            }
            else
            {
                unsigned long cp;
                int nb;

                if (utf8_decode((const unsigned char *)s + i, n - i,
                                &cp, &nb))
                {
                    if (!rtf_append_codepoint(out, cp))
                        return 0;
                    i += (size_t)nb;
                }
                else
                {
                    if (!sb_append_char(out, '?'))
                        return 0;
                    i++;
                }
            }
        }
        else if (c == '\\' && i + 1 < n &&
                 strchr("*_`[]\\#.!|>", s[i + 1]) != NULL)
        {
            if (!rtf_append_text(out, s + i + 1, 1))
                return 0;
            i += 2;
        }
        else if (c == '`')
        {
            mono = 1;
            if (!sb_append_str(out, "\\f1 "))
                return 0;
            i++;
        }
        else if (c == '*' || c == '_')
        {
            if (i + 1 < n && s[i + 1] == c)
            {
                bold = !bold;
                if (!sb_append_str(out, bold ? "\\b " : "\\b0 "))
                    return 0;
                i += 2;
            }
            else
            {
                if (c == '_' && i > 0 && i + 1 < n &&
                    isalnum((unsigned char)s[i - 1]) &&
                    isalnum((unsigned char)s[i + 1]))
                {
                    if (!rtf_append_text(out, "_", 1))
                        return 0;
                    i++;
                }
                else
                {
                    italic = !italic;
                    if (!sb_append_str(out, italic ? "\\i " : "\\i0 "))
                        return 0;
                    i++;
                }
            }
        }
        else if (c == '!' && i + 1 < n && s[i + 1] == '[')
        {
            const char *t;
            const char *u;
            const char *ve;
            size_t tlen;
            size_t ulen;

            t = s + i + 2;
            u = strchr(t, ']');
            if (u != NULL && *(u + 1) == '(')
            {
                ve = strchr(u + 2, ')');
                if (ve != NULL)
                {
                    tlen = (size_t)(u - t);
                    ulen = (size_t)(ve - (u + 2));
                    if (!sb_append_str(out,
                        "{\\field{\\*\\fldinst HYPERLINK \""))
                        return 0;
                    if (!rtf_append_field_url(out, u + 2, ulen))
                        return 0;
                    if (!sb_append_str(out,
                        "\"}{\\fldrslt \\ul\\cf1 "))
                        return 0;
                    if (!rtf_append_text(out, t, tlen))
                        return 0;
                    if (!sb_append_str(out, "}}"))
                        return 0;
                    i = (size_t)(ve - s) + 1;
                    continue;
                }
            }
            if (!rtf_append_text(out, "!", 1))
                return 0;
            i++;
        }
        else if (c == '[')
        {
            const char *u;
            const char *ve;
            size_t tlen;
            size_t ulen;

            u = strchr(s + i, ']');
            if (u != NULL && *(u + 1) == '(')
            {
                ve = strchr(u + 2, ')');
                if (ve != NULL)
                {
                    tlen = (size_t)(u - (s + i + 1));
                    ulen = (size_t)(ve - (u + 2));
                    if (!sb_append_str(out,
                        "{\\field{\\*\\fldinst HYPERLINK \""))
                        return 0;
                    if (!rtf_append_field_url(out, u + 2, ulen))
                        return 0;
                    if (!sb_append_str(out,
                        "\"}{\\fldrslt \\ul\\cf1 "))
                        return 0;
                    if (!rtf_append_text(out, s + i + 1, tlen))
                        return 0;
                    if (!sb_append_str(out, "}}"))
                        return 0;
                    i = (size_t)(ve - s) + 1;
                    continue;
                }
            }
            if (!rtf_append_text(out, "[", 1))
                return 0;
            i++;
        }
        else if (uc < 0x80)
        {
            if (!rtf_append_text(out, s + i, 1))
                return 0;
            i++;
        }
        else
        {
            unsigned long cp;
            int nb;

            if (utf8_decode((const unsigned char *)s + i, n - i, &cp, &nb))
            {
                if (!rtf_append_codepoint(out, cp))
                    return 0;
                i += (size_t)nb;
            }
            else
            {
                if (!sb_append_char(out, '?'))
                    return 0;
                i++;
            }
        }
    }
    if (mono)
    {
        if (!sb_append_str(out, "\\f0 "))
            return 0;
    }
    if (bold)
    {
        if (!sb_append_str(out, "\\b0 "))
            return 0;
    }
    if (italic)
    {
        if (!sb_append_str(out, "\\i0 "))
            return 0;
    }
    return 1;
}

static int
md_emit_table(StrBuf *out, char rows[MD_MAX_ROWS][2048],
              int nrows, int aligns[MD_MAX_COLS], int ncols)
{
    int r;
    int c;
    int cellx;

    r = 0;
    while (r < nrows)
    {
        char cells[MD_MAX_COLS][1024];
        int nc;
        int k;

        nc = md_split_row(rows[r], cells);
        if (nc > ncols)
            nc = ncols;
        if (!sb_append_str(out, "\\pard\\trowd\\trgaph60\\trleft0"))
            return 0;
        k = 0;
        while (k < ncols)
        {
            char tmp[64];

            cellx = (k + 1) * 9000 / ncols;
            sprintf(tmp, "\\cellx%d", cellx);
            if (!sb_append_str(out, tmp))
                return 0;
            k++;
        }
        if (!sb_append_str(out, " "))
            return 0;
        c = 0;
        while (c < ncols)
        {
            if (aligns[c] == 1)
            {
                if (!sb_append_str(out, "\\qc"))
                    return 0;
            }
            else if (aligns[c] == 2)
            {
                if (!sb_append_str(out, "\\qr"))
                    return 0;
            }
            else
            {
                if (!sb_append_str(out, "\\ql"))
                    return 0;
            }
            if (!sb_append_str(out, "\\intbl "))
                return 0;
            if (r == 0)
            {
                if (!sb_append_str(out, "\\b "))
                    return 0;
            }
            if (c < nc)
            {
                /* Header is already bold via outer \b; strip redundant
                   surrounding ** to avoid toggle cancel + flip-flop. */
                if (r == 0)
                {
                    size_t cl;

                    cl = strlen(cells[c]);
                    if (cl >= 4 &&
                        cells[c][0] == '*' && cells[c][1] == '*' &&
                        cells[c][cl - 1] == '*' &&
                        cells[c][cl - 2] == '*')
                    {
                        cells[c][cl - 2] = '\0';
                        if (!md_inline_to_rtf(out, cells[c] + 2))
                            return 0;
                    }
                    else
                    {
                        if (!md_inline_to_rtf(out, cells[c]))
                            return 0;
                    }
                }
                else
                {
                    if (!md_inline_to_rtf(out, cells[c]))
                        return 0;
                }
            }
            if (r == 0)
            {
                if (!sb_append_str(out, "\\b0 "))
                    return 0;
            }
            if (!sb_append_str(out, "\\cell "))
                return 0;
            c++;
        }
        if (!sb_append_str(out, "\\row "))
            return 0;
        r++;
    }
    if (!sb_append_str(out, "\\pard "))
        return 0;
    return 1;
}

static char *
md_to_rtf(const char *md)
{
    StrBuf out;
    size_t mdlen;
    size_t pos;
    int in_fence;
    static const int hsize[7] = { 0, 48, 36, 28, 24, 22, 20 };

    sb_init(&out);
    if (!sb_append_str(&out,
        "{\\rtf1\\ansi\\deff0"
        "{\\fonttbl{\\f0 Arial;}{\\f1 Courier New;}}"
        "{\\colortbl ;\\red0\\green0\\blue255;}"
        "\\pard\\fs20 "))
        return NULL;

    if (md == NULL)
        md = "";
    mdlen = strlen(md);
    pos = 0;
    in_fence = 0;

    while (pos < mdlen)
    {
        size_t eol;
        size_t len;
        char *line;
        const char *content;
        int level;
        int indent;
        int ok;

        eol = pos;
        while (eol < mdlen && md[eol] != '\n' && md[eol] != '\r')
            eol++;
        len = eol - pos;
        line = (char *)malloc(len + 1);
        if (line == NULL)
        {
            sb_free(&out);
            return NULL;
        }
        if (len > 0)
            memcpy(line, md + pos, len);
        line[len] = '\0';
        if (eol < mdlen && md[eol] == '\r' &&
            eol + 1 < mdlen && md[eol + 1] == '\n')
            pos = eol + 2;
        else if (eol < mdlen)
            pos = eol + 1;
        else
            pos = eol;

        if (md_is_fence(line))
        {
            in_fence = !in_fence;
            free(line);
            continue;
        }
        if (in_fence)
        {
            ok = sb_append_str(&out, "\\pard\\li200\\f1\\fs20 ") &&
                 rtf_append_text(&out, line, strlen(line)) &&
                 sb_append_str(&out, "\\f0\\par ");
            free(line);
            if (!ok)
            {
                sb_free(&out);
                return NULL;
            }
            continue;
        }
        if (md_is_blank(line))
        {
            free(line);
            if (!sb_append_str(&out, "\\par "))
            {
                sb_free(&out);
                return NULL;
            }
            continue;
        }
        /* Table? line has '|' and next line is separator. */
        if (strchr(line, '|') != NULL)
        {
            size_t p2;
            size_t q2;
            char *sepline;

            p2 = pos;
            q2 = p2;
            while (q2 < mdlen && md[q2] != '\n' && md[q2] != '\r')
                q2++;
            if (q2 > p2)
            {
                sepline = (char *)malloc(q2 - p2 + 1);
                if (sepline != NULL)
                {
                    char trows[MD_MAX_ROWS][2048];
                    char seps[MD_MAX_COLS][1024];
                    int aligns[MD_MAX_COLS];
                    int ncols;
                    int nrows;
                    int k;

                    memcpy(sepline, md + p2, q2 - p2);
                    sepline[q2 - p2] = '\0';
                    if (md_is_table_sep(sepline))
                    {
                        ncols = md_split_row(line, seps);
                        if (ncols < 1)
                            ncols = 1;
                        if (ncols > MD_MAX_COLS)
                            ncols = MD_MAX_COLS;
                        k = 0;
                        while (k < ncols)
                        {
                            aligns[k] = md_cell_align(seps[k]);
                            k++;
                        }
                        nrows = 0;
                        if (strlen(line) < 2048)
                        {
                            strcpy(trows[0], line);
                            nrows = 1;
                        }
                        pos = q2;
                        if (pos < mdlen && md[pos] == '\r' &&
                            pos + 1 < mdlen && md[pos + 1] == '\n')
                            pos += 2;
                        else if (pos < mdlen &&
                                 (md[pos] == '\n' || md[pos] == '\r'))
                            pos++;
                        while (pos < mdlen && nrows < MD_MAX_ROWS)
                        {
                            size_t r2;
                            size_t rq;

                            r2 = pos;
                            rq = r2;
                            while (rq < mdlen &&
                                   md[rq] != '\n' && md[rq] != '\r')
                                rq++;
                            if (rq == r2)
                                break;
                            {
                                char *rl;

                                rl = (char *)malloc(rq - r2 + 1);
                                if (rl == NULL)
                                    break;
                                memcpy(rl, md + r2, rq - r2);
                                rl[rq - r2] = '\0';
                                if (md_is_blank(rl) ||
                                    md_is_fence(rl) ||
                                    strchr(rl, '|') == NULL)
                                {
                                    free(rl);
                                    break;
                                }
                                if (strlen(rl) >= 2048)
                                {
                                    free(rl);
                                    break;
                                }
                                strcpy(trows[nrows], rl);
                                nrows++;
                                free(rl);
                                pos = rq;
                                if (pos < mdlen && md[pos] == '\r' &&
                                    pos + 1 < mdlen &&
                                    md[pos + 1] == '\n')
                                    pos += 2;
                                else if (pos < mdlen &&
                                         (md[pos] == '\n' ||
                                          md[pos] == '\r'))
                                    pos++;
                            }
                        }
                        free(line);
                        free(sepline);
                        if (nrows >= 1)
                        {
                            if (!md_emit_table(&out, trows, nrows,
                                               aligns, ncols))
                            {
                                sb_free(&out);
                                return NULL;
                            }
                            continue;
                        }
                        /* fall through as normal paragraph */
                        /* rebuild line handling below is skipped;
                           re-emit header as paragraph */
                        if (!sb_append_str(&out, "\\pard ") ||
                            !md_inline_to_rtf(&out, trows[0]) ||
                            !sb_append_str(&out, "\\par "))
                        {
                            sb_free(&out);
                            return NULL;
                        }
                        continue;
                    }
                    free(sepline);
                }
            }
        }
        level = md_heading(line, &content);
        if (level > 0)
        {
            char tmp[64];

            sprintf(tmp, "\\pard\\sb120\\sa60\\b\\fs%d ",
                    hsize[level]);
            ok = sb_append_str(&out, tmp) &&
                 md_inline_to_rtf(&out, content) &&
                 sb_append_str(&out, "\\b0\\fs20\\par ");
            free(line);
            if (!ok)
            {
                sb_free(&out);
                return NULL;
            }
            continue;
        }
        if (md_is_hrule(line))
        {
            free(line);
            if (!sb_append_str(&out,
                "\\pard\\qc\\emdash\\emdash\\emdash\\par\\pard "))
            {
                sb_free(&out);
                return NULL;
            }
            continue;
        }
        if (md_parse_quote(line, &content))
        {
            ok = sb_append_str(&out, "\\pard\\li720 ") &&
                 sb_append_str(&out, "> ") &&
                 md_inline_to_rtf(&out, content) &&
                 sb_append_str(&out, "\\par ");
            free(line);
            if (!ok)
            {
                sb_free(&out);
                return NULL;
            }
            continue;
        }
        {
            const char *c2;

            if (md_parse_ul(line, &indent, &c2))
            {
                char tmp[64];
                int li;

                if (indent > 4)
                    indent = 4;
                li = 360 + indent * 360;
                sprintf(tmp, "\\pard\\li%d\\fi-360 ", li);
                /* \'95 (Win1252 bullet), not \u8226?: RichEdit 1.0
                   has no \uN support; rtf_to_md maps \'95 back to
                   U+2022, so lists survive on every version. */
                ok = sb_append_str(&out, tmp) &&
                     sb_append_str(&out, "\\'95 ") &&
                     md_inline_to_rtf(&out, c2) &&
                     sb_append_str(&out, "\\par ");
                free(line);
                if (!ok)
                {
                    sb_free(&out);
                    return NULL;
                }
                continue;
            }
        }
        {
            const char *c2;
            const char *ns;
            size_t nl;
            char numbuf[32];
            size_t nn;

            if (md_parse_ol(line, &indent, &c2, &ns, &nl))
            {
                char tmp[64];
                int li;

                if (indent > 4)
                    indent = 4;
                li = 360 + indent * 360;
                nn = nl < 30 ? nl : 30;
                memcpy(numbuf, ns, nn);
                numbuf[nn] = '\0';
                sprintf(tmp, "\\pard\\li%d\\fi-360 ", li);
                ok = sb_append_str(&out, tmp) &&
                     rtf_append_text(&out, numbuf, nn) &&
                     sb_append_str(&out, ". ") &&
                     md_inline_to_rtf(&out, c2) &&
                     sb_append_str(&out, "\\par ");
                free(line);
                if (!ok)
                {
                    sb_free(&out);
                    return NULL;
                }
                continue;
            }
        }
        if ((line[0] == ' ' && line[1] == ' ' &&
             line[2] == ' ' && line[3] == ' ') || line[0] == '\t')
        {
            const char *c2;

            c2 = line[0] == '\t' ? line + 1 : line + 4;
            ok = sb_append_str(&out, "\\pard\\li200\\f1\\fs20 ") &&
                 rtf_append_text(&out, c2, strlen(c2)) &&
                 sb_append_str(&out, "\\f0\\par ");
            free(line);
            if (!ok)
            {
                sb_free(&out);
                return NULL;
            }
            continue;
        }
        ok = sb_append_str(&out, "\\pard ") &&
             md_inline_to_rtf(&out, line) &&
             sb_append_str(&out, "\\par ");
        free(line);
        if (!ok)
        {
            sb_free(&out);
            return NULL;
        }
    }

    if (!sb_append_str(&out, "}"))
    {
        sb_free(&out);
        return NULL;
    }
    if (out.data == NULL)
    {
        out.data = (char *)malloc(1);
        if (out.data == NULL)
            return NULL;
        out.data[0] = '\0';
    }
    return out.data;
}

/*
 * ----------------------------------------------------------------------
 * RTF -> Markdown (restricted subset, C89)
 * ----------------------------------------------------------------------
 */

#define RTF_MAX_DEPTH 32
#define RTF_MAX_CELLS 16

typedef struct
{
    int ignore;
    int is_field;
    int star;
} RtfGroup;

#define RTF_MAX_FONTS 64

typedef struct
{
    StrBuf md;
    StrBuf para;
    int p_bold;
    int p_italic;
    int p_mono;
    int cur_bold;
    int cur_italic;
    int cur_mono;
    int cur_fs;
    int para_first_fs;
    int para_first_mono;
    int para_has_nonmono;
    int para_has_text;
    int prev_block;
    int in_row;
    int table_first;
    StrBuf cells[RTF_MAX_CELLS];
    int cell_idx;
    int field_depth;
    int in_field;
    int in_fldinst;
    int in_fldrslt;
    int fldinst_depth;
    int fldrslt_depth;
    StrBuf fld_url;
    StrBuf fld_result;
    RtfGroup stack[RTF_MAX_DEPTH];
    int depth;
    /* \'xx codepage (from \ansicpgN, default 1252) + \ucN fallback count */
    int ansi_cp;
    int uc;
    /* fonttbl -> monospace mapping (controls renumber fonts) */
    int fonttbl_depth;
    int font_entry;
    char font_name[64];
    int font_namelen;
    int fonts_mono[RTF_MAX_FONTS];
    int fonts_tbl;
} RtfParse;

static unsigned long
win1252_to_unicode(unsigned char b)
{
    static const unsigned long tab[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
    };

    if (b < 0x80)
        return (unsigned long)b;
    if (b < 0xA0)
        return tab[b - 0x80];
    return (unsigned long)b;
}

static int
md_needs_escape(char c)
{
    /* Minimal set, kept in sync with the md_inline_to_rtf unescape
       list for '(' / ')' / '\\' handling:
       - '`', '*', '_', '[', ']', '|' are escaped on output and
         unescaped on input (stable pairs; '[' ']' '|' must be
         escaped to avoid link/table formation).
       - '(', ')' and '\\' are emitted RAW on purpose. Our inline
         parser leaves "\(" / "\)" alone (parens not in its escape
         list) and passes single "\" through, so escaping them here
         would add a slash every cycle ("(" -> "\(" -> "\\\(" ...).
         Raw parens are harmless mid-text ("[", "]" stay escaped, so
         no accidental links); raw backslashes normalize once
         ("\\" -> "\") then stay stable.
       - '.', '#', '-', '>', '+', '!' are also raw for the same
         reason (keeps list/quote/heading detection working). */
    if (c == '`' || c == '*' || c == '_' ||
        c == '[' || c == ']' || c == '|')
        return 1;
    return 0;
}

static int
md_append_codepoint(StrBuf *out, unsigned long cp)
{
    char tmp[5];
    int nb;

    if (cp == 0)
        return 1;
    if (cp < 0x80)
    {
        char c;

        c = (char)cp;
        if (md_needs_escape(c))
        {
            if (!sb_append_char(out, '\\'))
                return 0;
        }
        return sb_append_char(out, c);
    }
    nb = utf8_encode(cp, tmp);
    tmp[nb] = '\0';
    return sb_append_n(out, tmp, (size_t)nb);
}

static int
md_append_raw(StrBuf *out, unsigned long cp)
{
    char tmp[5];
    int nb;

    if (cp == 0)
        return 1;
    if (cp < 0x80)
    {
        char c;

        c = (char)cp;
        if (c == '`')
        {
            /* Avoid closing the code span; input side treats
               backslashes in code as literal, so "\`" survives. */
            if (!sb_append_char(out, '\\'))
                return 0;
        }
        return sb_append_char(out, c);
    }
    nb = utf8_encode(cp, tmp);
    tmp[nb] = '\0';
    return sb_append_n(out, tmp, (size_t)nb);
}

/* ASCII word char, mirroring the md_inline intra-word '_' rule
   (which uses byte isalnum in the C locale). */
static int
is_md_wordchar(unsigned char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

static StrBuf *
rtf_target(RtfParse *st)
{
    if (st->in_field && st->in_fldrslt)
        return &st->fld_result;
    if (st->in_row)
    {
        if (st->cell_idx < 0)
            st->cell_idx = 0;
        if (st->cell_idx >= RTF_MAX_CELLS)
            st->cell_idx = RTF_MAX_CELLS - 1;
        return &st->cells[st->cell_idx];
    }
    return &st->para;
}

static int
rtf_toggle_bold(RtfParse *st, int on)
{
    StrBuf *t;

    if (st->in_field && st->in_fldrslt)
        return 1;
    if (st->depth > 0 && st->stack[st->depth - 1].ignore)
        return 1;
    st->cur_bold = on;
    t = rtf_target(st);
    if (t == &st->para || t != &st->fld_result)
    {
        if (on && !st->p_bold)
        {
            st->p_bold = 1;
            if (!sb_append_str(t, "**"))
                return 0;
        }
        else if (!on && st->p_bold)
        {
            st->p_bold = 0;
            if (!sb_append_str(t, "**"))
                return 0;
        }
    }
    return 1;
}

static int
rtf_toggle_italic(RtfParse *st, int on)
{
    StrBuf *t;

    if (st->in_field && st->in_fldrslt)
        return 1;
    if (st->depth > 0 && st->stack[st->depth - 1].ignore)
        return 1;
    st->cur_italic = on;
    t = rtf_target(st);
    if (on && !st->p_italic)
    {
        st->p_italic = 1;
        if (!sb_append_str(t, "*"))
            return 0;
    }
    else if (!on && st->p_italic)
    {
        st->p_italic = 0;
        if (!sb_append_str(t, "*"))
            return 0;
    }
    return 1;
}

static int
is_mono_font_name(const char *name)
{
    /* case-insensitive substring match */
    const char *subs[] = {
        "courier", "consolas", "lucida console",
        "fixedsys", "terminal", NULL
    };
    int k;
    const char *p;

    if (name == NULL)
        return 0;
    k = 0;
    while (subs[k] != NULL)
    {
        p = name;
        while (*p != '\0')
        {
            const char *a;
            const char *b;
            a = p;
            b = subs[k];
            while (*b != '\0')
            {
                char ca;
                char cb;
                ca = *a;
                cb = *b;
                if (ca >= 'A' && ca <= 'Z')
                    ca = (char)(ca + 32);
                if (ca != cb)
                    break;
                a++;
                b++;
            }
            if (*b == '\0')
                return 1;
            p++;
        }
        k++;
    }
    return 0;
}

static int
is_mono_font(RtfParse *st, int idx)
{
    if (idx < 0 || idx >= RTF_MAX_FONTS)
        return 0;
    if (!st->fonts_tbl)
        return 0;
    return st->fonts_mono[idx];
}

static int
rtf_toggle_mono(RtfParse *st, int on)
{
    StrBuf *t;

    if (st->in_field && st->in_fldrslt)
        return 1;
    if (st->depth > 0 && st->stack[st->depth - 1].ignore)
        return 1;
    st->cur_mono = on;
    t = rtf_target(st);
    if (on && !st->p_mono)
    {
        st->p_mono = 1;
        if (!sb_append_str(t, "`"))
            return 0;
    }
    else if (!on && st->p_mono)
    {
        st->p_mono = 0;
        if (!sb_append_str(t, "`"))
            return 0;
    }
    return 1;
}

static int
rtf_append_plain(RtfParse *st, const char *s, size_t n)
{
    StrBuf *t;
    size_t i;

    if (st->depth > 0 && st->stack[st->depth - 1].ignore)
        return 1;
    if (st->in_field && st->in_fldinst)
    {
        /* URL capture happens in caller for fldinst text. */
        return 1;
    }
    t = rtf_target(st);
    i = 0;
    while (i < n)
    {
        unsigned char c;
        unsigned long cp;
        int nb;

        c = (unsigned char)s[i];
        if (c < 0x80)
        {
            cp = c;
            nb = 1;
        }
        else
        {
            if (!utf8_decode((const unsigned char *)s + i, n - i,
                             &cp, &nb))
            {
                cp = '?';
                nb = 1;
            }
        }
        if (t == &st->para && !st->para_has_text)
        {
            if (cp != ' ' && cp != '\t')
            {
                st->para_has_text = 1;
                st->para_first_fs = st->cur_fs;
                st->para_first_mono = st->cur_mono;
                st->para_has_nonmono = !st->cur_mono;
            }
        }
        else if (t == &st->para && st->para_has_text && !st->cur_mono)
        {
            st->para_has_nonmono = 1;
        }
        /* Inside `code` backslashes are literal on both sides;
           escaping them here would double them every cycle. */
        if (st->cur_mono && t != &st->fld_result)
        {
            if (!md_append_raw(t, cp))
                return 0;
        }
        else if (cp == '_' && t != &st->fld_result)
        {
            /* Mirror md_inline: '_' between word chars is literal
               (identifiers like test_avif2pnm need no slash);
               elsewhere it toggles italics, so it must be escaped. */
            unsigned char pc;
            unsigned char nc;
            int prev_a;
            int next_a;

            if (i > 0)
                pc = (unsigned char)s[i - 1];
            else if (t->len > 0)
                pc = (unsigned char)t->data[t->len - 1];
            else
                pc = 0;
            if (i + (size_t)nb < n)
                nc = (unsigned char)s[i + nb];
            else
                nc = 0;
            prev_a = is_md_wordchar(pc);
            next_a = is_md_wordchar(nc);
            if (prev_a && next_a)
            {
                if (!sb_append_char(t, '_'))
                    return 0;
            }
            else
            {
                if (!md_append_codepoint(t, cp))
                    return 0;
            }
        }
        else
        {
            if (!md_append_codepoint(t, cp))
                return 0;
        }
        i += (size_t)nb;
    }
    return 1;
}

static void
rtf_close_markers(StrBuf *t, RtfParse *st)
{
    if (st->p_mono)
    {
        sb_append_str(t, "`");
        st->p_mono = 0;
    }
    if (st->p_bold)
    {
        sb_append_str(t, "**");
        st->p_bold = 0;
    }
    if (st->p_italic)
    {
        sb_append_str(t, "*");
        st->p_italic = 0;
    }
}

static int
rtf_is_blank_buf(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    return *s == '\0';
}

static void
rtf_trim_inplace(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    /* caller trims leading by moving pointer; here trim trailing */
    e = s + strlen(s);
    while (e > s && (*(e - 1) == ' ' || *(e - 1) == '\t' ||
                     *(e - 1) == '\r' || *(e - 1) == '\n'))
        e--;
    *e = '\0';
}

static int
rtf_flush_para(RtfParse *st)
{
    char *txt;
    size_t len;

    if (st->in_row)
    {
        /* \par inside a table cell: treat as space. */
        if (st->cell_idx >= 0 && st->cell_idx < RTF_MAX_CELLS)
            sb_append_char(&st->cells[st->cell_idx], ' ');
        return 1;
    }
    rtf_close_markers(&st->para, st);
    st->cur_bold = 0;
    st->cur_italic = 0;
    st->cur_mono = 0;
    /* Empty RTF paras collapse: block flushes already terminate with
       blank lines, so emitting another "\n" here would grow blanks
       every cycle. Exception: list/code/quote/table blocks end with a
       single "\n", so one empty para after them completes the blank
       line (further empties collapse via prev==7). */
    if (st->para.data == NULL || st->para.len == 0)
    {
        st->para_has_text = 0;
        if (st->prev_block == 2 || st->prev_block == 3 ||
            st->prev_block == 5 || st->prev_block == 6)
            sb_append_char(&st->md, '\n');
        st->prev_block = 7;
        return 1;
    }
    txt = st->para.data;
    /* trim trailing spaces/newlines for detection, keep leading */
    len = strlen(txt);
    while (len > 0 && (txt[len - 1] == ' ' || txt[len - 1] == '\t'))
    {
        txt[len - 1] = '\0';
        len--;
    }
    if (rtf_is_blank_buf(txt))
    {
        st->para.len = 0;
        if (st->para.data != NULL)
            st->para.data[0] = '\0';
        st->para_has_text = 0;
        if (st->prev_block == 2 || st->prev_block == 3 ||
            st->prev_block == 5 || st->prev_block == 6)
            sb_append_char(&st->md, '\n');
        st->prev_block = 7;
        return 1;
    }
    /* code block? all mono: "`...`" */
    if (st->para_has_text && st->para_first_mono && !st->para_has_nonmono)
    {
        char *body;

        body = txt;
        if (body[0] == '`')
            body++;
        len = strlen(body);
        while (len > 0 && (body[len - 1] == '`' || body[len - 1] == ' ' ||
                           body[len - 1] == '\t'))
        {
            body[len - 1] = '\0';
            len--;
        }
        while (*body == ' ' || *body == '\t')
            body++;
        if (st->prev_block != 3 && st->prev_block != 0 && st->prev_block != 7)
            sb_append_char(&st->md, '\n');
        sb_append_str(&st->md, "    ");
        sb_append_str(&st->md, body);
        sb_append_char(&st->md, '\n');
        st->prev_block = 3;
    }
    else if (txt[0] == '>' && (txt[1] == ' ' || txt[1] == '\0'))
    {
        if (st->prev_block == 3 || st->prev_block == 6)
            sb_append_char(&st->md, '\n');
        sb_append_str(&st->md, txt);
        sb_append_char(&st->md, '\n');
        st->prev_block = 5;
    }
    else if ((txt[0] == '-' && txt[1] == ' ') ||
             (txt[0] == '+' && txt[1] == ' ') ||
             ((unsigned char)txt[0] == 0xE2 &&
              (unsigned char)txt[1] == 0x80 &&
              (unsigned char)txt[2] == 0xA2 && txt[3] == ' '))
    {
        const char *body;

        if ((unsigned char)txt[0] == 0xE2)
            body = txt + 4;
        else
            body = txt + 2;
        if (st->prev_block != 2 && st->prev_block != 0 &&
            st->prev_block != 7)
            sb_append_char(&st->md, '\n');
        sb_append_str(&st->md, "- ");
        sb_append_str(&st->md, body);
        sb_append_char(&st->md, '\n');
        st->prev_block = 2;
    }
    else if ((txt[0] >= '0' && txt[0] <= '9'))
    {
        const char *p;
        int is_ol;

        p = txt;
        while (*p >= '0' && *p <= '9')
            p++;
        is_ol = (*p == '.' && (*(p + 1) == ' ' || *(p + 1) == '\0'));
        if (is_ol)
        {
            if (st->prev_block != 2 && st->prev_block != 0 &&
                st->prev_block != 7)
                sb_append_char(&st->md, '\n');
            sb_append_str(&st->md, txt);
            sb_append_char(&st->md, '\n');
            st->prev_block = 2;
        }
        else
        {
            if (st->prev_block == 2 || st->prev_block == 3 ||
                st->prev_block == 6)
                sb_append_char(&st->md, '\n');
            sb_append_str(&st->md, txt);
            sb_append_str(&st->md, "\n\n");
            st->prev_block = 1;
        }
    }
    else if (st->para_first_fs >= 40)
    {
        char *body;

        body = txt;
        if (body[0] == '*' && body[1] == '*' &&
            strlen(body) > 4 &&
            body[strlen(body) - 1] == '*' &&
            body[strlen(body) - 2] == '*')
        {
            body[strlen(body) - 2] = '\0';
            body += 2;
        }
        if (st->prev_block != 0 && st->prev_block != 7)
            sb_append_char(&st->md, '\n');
        sb_append_str(&st->md, "# ");
        sb_append_str(&st->md, body);
        sb_append_str(&st->md, "\n\n");
        st->prev_block = 4;
    }
    else if (st->para_first_fs >= 30)
    {
        char *body;

        body = txt;
        if (body[0] == '*' && body[1] == '*' &&
            strlen(body) > 4 &&
            body[strlen(body) - 1] == '*' &&
            body[strlen(body) - 2] == '*')
        {
            body[strlen(body) - 2] = '\0';
            body += 2;
        }
        if (st->prev_block != 0 && st->prev_block != 7)
            sb_append_char(&st->md, '\n');
        if (st->para_first_fs >= 36)
            sb_append_str(&st->md, "## ");
        else
            sb_append_str(&st->md, "### ");
        sb_append_str(&st->md, body);
        sb_append_str(&st->md, "\n\n");
        st->prev_block = 4;
    }
    else if (st->para_first_fs >= 23)
    {
        char *body;

        body = txt;
        if (body[0] == '*' && body[1] == '*' &&
            strlen(body) > 4 &&
            body[strlen(body) - 1] == '*' &&
            body[strlen(body) - 2] == '*')
        {
            body[strlen(body) - 2] = '\0';
            body += 2;
        }
        if (st->prev_block != 0 && st->prev_block != 7)
            sb_append_char(&st->md, '\n');
        sb_append_str(&st->md, "#### ");
        sb_append_str(&st->md, body);
        sb_append_str(&st->md, "\n\n");
        st->prev_block = 4;
    }
    else
    {
        /* hr? em dashes */
        if ((strcmp(txt, "\\-\\-\\-") == 0) ||
            (strcmp(txt, "---") == 0))
        {
            sb_append_str(&st->md, "---\n\n");
            st->prev_block = 8;
        }
        else
        {
            /* Detect 3 em dashes (each 3 bytes in UTF-8). */
            if (strlen(txt) == 9 &&
                (unsigned char)txt[0] == 0xE2 &&
                (unsigned char)txt[3] == 0xE2 &&
                (unsigned char)txt[6] == 0xE2)
            {
                sb_append_str(&st->md, "---\n\n");
                st->prev_block = 8;
            }
            else
            {
                if (st->prev_block == 2 || st->prev_block == 3 ||
                    st->prev_block == 6)
                    sb_append_char(&st->md, '\n');
                sb_append_str(&st->md, txt);
                sb_append_str(&st->md, "\n\n");
                st->prev_block = 1;
            }
        }
    }
    st->para.len = 0;
    if (st->para.data != NULL)
        st->para.data[0] = '\0';
    st->para_has_text = 0;
    st->para_first_fs = 20;
    st->para_first_mono = 0;
    st->para_has_nonmono = 0;
    return 1;
}

static int
rtf_flush_row(RtfParse *st)
{
    int i;
    int last;
    int c;

    if (!st->in_row)
        return 1;
    if (st->cell_idx >= 0 && st->cell_idx < RTF_MAX_CELLS)
        rtf_close_markers(&st->cells[st->cell_idx], st);
    st->cur_bold = 0;
    st->cur_italic = 0;
    st->cur_mono = 0;
    last = st->cell_idx;
    if (last >= RTF_MAX_CELLS)
        last = RTF_MAX_CELLS - 1;
    if (last < 0)
        last = 0;
    /* trim trailing empty cells from our fixed array: find last non-blank */
    while (last > 0)
    {
        const char *s;

        s = st->cells[last].data != NULL ? st->cells[last].data : "";
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == '\0')
            last--;
        else
            break;
    }
    if (st->prev_block != 6 && st->prev_block != 0 && st->prev_block != 7)
        sb_append_char(&st->md, '\n');
    sb_append_char(&st->md, '|');
    i = 0;
    while (i <= last)
    {
        const char *s;

        s = st->cells[i].data != NULL ? st->cells[i].data : "";
        while (*s == ' ' || *s == '\t')
            s++;
        {
            char *e;

            e = (char *)s + strlen(s);
            while (e > s && (*(e - 1) == ' ' || *(e - 1) == '\t'))
                e--;
            /* emit trimmed */
            sb_append_char(&st->md, ' ');
            if ((size_t)(e - s) == 0)
                sb_append_char(&st->md, ' ');
            else
                sb_append_n(&st->md, s, (size_t)(e - s));
            sb_append_str(&st->md, " |");
        }
        i++;
    }
    sb_append_char(&st->md, '\n');
    if (st->table_first)
    {
        sb_append_char(&st->md, '|');
        c = 0;
        while (c <= last)
        {
            sb_append_str(&st->md, " --- |");
            c++;
        }
        sb_append_char(&st->md, '\n');
        st->table_first = 0;
    }
    st->prev_block = 6;
    c = 0;
    while (c < RTF_MAX_CELLS)
    {
        st->cells[c].len = 0;
        if (st->cells[c].data != NULL)
            st->cells[c].data[0] = '\0';
        c++;
    }
    st->cell_idx = 0;
    st->in_row = 0;
    return 1;
}

static int
rtf_is_hex(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int
rtf_hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return c - 'A' + 10;
}

/*
 * Codepage-aware \'xx handling (DBCS fix).
 *
 * RTF \'xx bytes are in the document codepage (\ansicpgN, e.g. 950
 * for Big5 where 0xA1 0xF7 is U+2192 RIGHTWARDS ARROW). Decoding
 * them always as Win1252 turns that arrow into U+00A1 U+00F7
 * ("¡÷"). Keep the Win1252 fast path for the default 1252 case;
 * otherwise convert whole runs via the declared codepage to UTF-8.
 * \uN fallback bytes are skipped (not decoded) so \u8594\'a1\'f7
 * yields a single arrow instead of "arrow + mojibake".
 */

static int
rtf_is_dbcs_lead(int cp, unsigned char b)
{
    if (cp == 1252 || cp == 65001 || cp <= 0)
        return 0;
    if (b < 0x81 || b == 0xFF)
        return 0;
    return IsDBCSLeadByteEx((UINT)cp, (BYTE)b) != 0;
}

static int
rtf_append_ansi_bytes(RtfParse *st, const unsigned char *bytes, int n)
{
    WCHAR *w;
    int wn;

    if (n <= 0)
        return 1;
    if (st->depth > 0 && st->stack[st->depth - 1].ignore)
        return 1;
    if (st->in_field && st->in_fldinst)
        return 1;
    if (st->ansi_cp == 1252)
    {
        int k;
        unsigned long cp;
        char ub[5];
        int nb;

        k = 0;
        while (k < n)
        {
            cp = win1252_to_unicode(bytes[k]);
            nb = utf8_encode(cp, ub);
            if (st->in_field && st->in_fldrslt)
            {
                if (!sb_append_n(&st->fld_result, ub, (size_t)nb))
                    return 0;
            }
            else
            {
                if (!rtf_append_plain(st, ub, (size_t)nb))
                    return 0;
            }
            k++;
        }
        return 1;
    }
    if (st->ansi_cp == 65001)
    {
        /* UTF-8 bytes: no OS codepage needed (retro-safe). */
        if (st->in_field && st->in_fldrslt)
        {
            int k;
            unsigned long cp;
            char ub[5];
            int nb;
            int used;

            k = 0;
            while (k < n)
            {
                if (utf8_decode(bytes + k, (size_t)(n - k), &cp, &nb))
                    used = nb;
                else
                {
                    cp = '?';
                    used = 1;
                }
                nb = utf8_encode(cp, ub);
                if (!sb_append_n(&st->fld_result, ub, (size_t)nb))
                    return 0;
                k += used;
            }
            return 1;
        }
        return rtf_append_plain(st, (const char *)bytes, (size_t)n);
    }
    wn = MultiByteToWideChar((UINT)st->ansi_cp, 0,
                             (LPCSTR)bytes, n, NULL, 0);
    if (wn <= 0)
    {
        int k;
        unsigned long cp;
        char ub[5];
        int nb;

        /* e.g. lone 0x95 bullet on a DBCS system: fall back to 1252 */
        k = 0;
        while (k < n)
        {
            cp = win1252_to_unicode(bytes[k]);
            nb = utf8_encode(cp, ub);
            if (st->in_field && st->in_fldrslt)
            {
                if (!sb_append_n(&st->fld_result, ub, (size_t)nb))
                    return 0;
            }
            else
            {
                if (!rtf_append_plain(st, ub, (size_t)nb))
                    return 0;
            }
            k++;
        }
        return 1;
    }
    w = (WCHAR *)malloc((size_t)(wn + 1) * sizeof(WCHAR));
    if (w == NULL)
        return 0;
    if (MultiByteToWideChar((UINT)st->ansi_cp, 0,
                            (LPCSTR)bytes, n, w, wn) <= 0)
    {
        free(w);
        return 1;
    }
    /* Manual WCHAR -> UTF-8 (retro-safe; DBCS maps to BMP only). */
    {
        int k;
        unsigned long cp;
        char ub[5];
        int nb;

        k = 0;
        while (k < wn)
        {
            cp = (unsigned long)w[k];
            if (cp >= 0xD800 && cp <= 0xDFFF)
                cp = '?';
            nb = utf8_encode(cp, ub);
            if (st->in_field && st->in_fldrslt)
            {
                if (!sb_append_n(&st->fld_result, ub, (size_t)nb))
                {
                    free(w);
                    return 0;
                }
            }
            else
            {
                if (!rtf_append_plain(st, ub, (size_t)nb))
                {
                    free(w);
                    return 0;
                }
            }
            k++;
        }
    }
    free(w);
    return 1;
}

static void
rtf_skip_u_fallback(const char *rtf, size_t len, size_t *pi,
                    int uc, int ansi_cp)
{
    size_t i;
    int k;

    i = *pi;
    k = 0;
    while (k < uc && i < len)
    {
        if (i + 3 < len && rtf[i] == '\\' && rtf[i + 1] == '\'' &&
            rtf_is_hex(rtf[i + 2]) && rtf_is_hex(rtf[i + 3]))
        {
            unsigned char b;

            b = (unsigned char)(rtf_hex_val(rtf[i + 2]) * 16 +
                                rtf_hex_val(rtf[i + 3]));
            i += 4;
            if (ansi_cp == 65001)
            {
                int t;

                /* UTF-8 fallback: one char is up to 4 bytes */
                t = 0;
                while (t < 3 && i + 3 < len && rtf[i] == '\\' &&
                       rtf[i + 1] == '\'' &&
                       rtf_is_hex(rtf[i + 2]) &&
                       rtf_is_hex(rtf[i + 3]))
                {
                    /* stop unless continuation byte 0x80-0xBF */
                    b = (unsigned char)(rtf_hex_val(rtf[i + 2]) * 16 +
                                        rtf_hex_val(rtf[i + 3]));
                    if (b < 0x80 || b > 0xBF)
                        break;
                    i += 4;
                    t++;
                }
            }
            else if (rtf_is_dbcs_lead(ansi_cp, b))
            {
                if (i + 3 < len && rtf[i] == '\\' &&
                    rtf[i + 1] == '\'' &&
                    rtf_is_hex(rtf[i + 2]) &&
                    rtf_is_hex(rtf[i + 3]))
                    i += 4;
            }
            k++;
        }
        else if (rtf[i] != '\\' && rtf[i] != '{' && rtf[i] != '}')
        {
            i++;
            k++;
        }
        else
        {
            break;
        }
    }
    *pi = i;
}

static char *
rtf_to_md(const char *rtf)
{
    RtfParse st;
    size_t len;
    size_t i;
    int c;

    memset(&st, 0, sizeof(st));
    sb_init(&st.md);
    sb_init(&st.para);
    sb_init(&st.fld_url);
    sb_init(&st.fld_result);
    c = 0;
    while (c < RTF_MAX_CELLS)
    {
        sb_init(&st.cells[c]);
        c++;
    }
    st.cur_fs = 20;
    st.para_first_fs = 20;
    st.cell_idx = 0;
    st.field_depth = -1;
    st.depth = 0;
    st.ansi_cp = 1252;
    st.uc = 1;
    st.fonttbl_depth = -1;
    st.font_entry = -1;
    st.font_namelen = 0;

    if (rtf == NULL)
        rtf = "";
    len = strlen(rtf);
    i = 0;
    while (i < len)
    {
        char ch;

        ch = rtf[i];
        if (ch == '{')
        {
            int ign;

            ign = 0;
            if (st.depth > 0 && st.stack[st.depth - 1].ignore)
                ign = 1;
            if (st.depth < RTF_MAX_DEPTH)
            {
                st.stack[st.depth].ignore = ign;
                st.stack[st.depth].is_field = 0;
                st.stack[st.depth].star = 0;
                st.depth++;
            }
            i++;
        }
        else if (ch == '}')
        {
            if (st.depth > 0)
                st.depth--;
            if (st.fonttbl_depth >= 0 && st.depth < st.fonttbl_depth)
            {
                st.fonttbl_depth = -1;
                st.font_entry = -1;
                st.font_namelen = 0;
            }
            if (st.in_fldinst && st.depth < st.fldinst_depth)
                st.in_fldinst = 0;
            if (st.in_fldrslt && st.depth < st.fldrslt_depth)
                st.in_fldrslt = 0;
            if (st.in_field && st.depth < st.field_depth)
            {
                /* flush link */
                if (st.fld_url.len > 0 && st.fld_result.len > 0)
                {
                    StrBuf *t;

                    t = rtf_target(&st);
                    /* temporarily disable field result routing */
                    st.in_fldrslt = 0;
                    t = rtf_target(&st);
                    sb_append_char(t, '[');
                    sb_append_str(t, st.fld_result.data != NULL ?
                                       st.fld_result.data : "");
                    sb_append_str(t, "](");
                    sb_append_str(t, st.fld_url.data != NULL ?
                                       st.fld_url.data : "");
                    sb_append_char(t, ')');
                    st.in_fldrslt = 0;
                }
                else if (st.fld_result.len > 0)
                {
                    StrBuf *t;

                    st.in_fldrslt = 0;
                    t = rtf_target(&st);
                    sb_append_str(t, st.fld_result.data != NULL ?
                                       st.fld_result.data : "");
                }
                st.in_field = 0;
                st.in_fldinst = 0;
                st.in_fldrslt = 0;
                st.field_depth = -1;
                st.fld_url.len = 0;
                if (st.fld_url.data != NULL)
                    st.fld_url.data[0] = '\0';
                st.fld_result.len = 0;
                if (st.fld_result.data != NULL)
                    st.fld_result.data[0] = '\0';
            }
            i++;
        }
        else if (ch == '\\')
        {
            size_t j;
            char nc;

            if (i + 1 >= len)
            {
                i++;
                continue;
            }
            nc = rtf[i + 1];
            if (nc == '{' || nc == '}' || nc == '\\')
            {
                char tmp[2];

                tmp[0] = nc;
                tmp[1] = '\0';
                if (!(st.depth > 0 &&
                      st.stack[st.depth - 1].ignore))
                {
                    if (st.in_field && st.in_fldinst)
                    {
                        /* ignore */
                    }
                    else if (st.in_field && st.in_fldrslt)
                    {
                        sb_append_str(&st.fld_result, tmp);
                    }
                    else
                    {
                        rtf_append_plain(&st, tmp, 1);
                    }
                }
                i += 2;
                continue;
            }
            if (nc == '\'')
            {
                if (i + 3 < len && rtf_is_hex(rtf[i + 2]) &&
                    rtf_is_hex(rtf[i + 3]))
                {
                    if (st.ansi_cp == 1252)
                    {
                        unsigned char b;
                        unsigned long cp;
                        char ub[5];
                        int nb;

                        b = (unsigned char)(rtf_hex_val(rtf[i + 2]) * 16 +
                                            rtf_hex_val(rtf[i + 3]));
                        cp = win1252_to_unicode(b);
                        nb = utf8_encode(cp, ub);
                        if (!(st.depth > 0 &&
                              st.stack[st.depth - 1].ignore))
                        {
                            if (st.in_field && st.in_fldinst)
                            {
                            }
                            else if (st.in_field && st.in_fldrslt)
                            {
                                sb_append_n(&st.fld_result, ub, (size_t)nb);
                            }
                            else
                            {
                                rtf_append_plain(&st, ub, (size_t)nb);
                            }
                        }
                        i += 4;
                        continue;
                    }
                    else
                    {
                        size_t k;
                        int nbytes;
                        int t;
                        unsigned char *abuf;

                        nbytes = 0;
                        k = i;
                        while (k + 3 < len && rtf[k] == '\\' &&
                               rtf[k + 1] == '\'' &&
                               rtf_is_hex(rtf[k + 2]) &&
                               rtf_is_hex(rtf[k + 3]))
                        {
                            nbytes++;
                            k += 4;
                        }
                        abuf = (unsigned char *)malloc((size_t)nbytes);
                        if (abuf == NULL)
                        {
                            i += 4;
                            continue;
                        }
                        t = 0;
                        k = i;
                        while (t < nbytes)
                        {
                            abuf[t] = (unsigned char)
                                (rtf_hex_val(rtf[k + 2]) * 16 +
                                 rtf_hex_val(rtf[k + 3]));
                            t++;
                            k += 4;
                        }
                        rtf_append_ansi_bytes(&st, abuf, nbytes);
                        free(abuf);
                        i = k;
                        continue;
                    }
                }
                i += 2;
                continue;
            }
            /* Single-char controls: \*, \~, \-, \_, \<space>, etc.
               Consume both chars so '*' etc. don't leak as text. */
            if (!((nc >= 'a' && nc <= 'z') ||
                  (nc >= 'A' && nc <= 'Z')))
            {
                if (nc == '~')
                {
                    if (!(st.depth > 0 &&
                          st.stack[st.depth - 1].ignore) &&
                        !(st.in_field && st.in_fldinst))
                    {
                        if (st.in_field && st.in_fldrslt)
                            sb_append_char(&st.fld_result, ' ');
                        else
                            rtf_append_plain(&st, " ", 1);
                    }
                }
                else if (nc == '_')
                {
                    if (!(st.depth > 0 &&
                          st.stack[st.depth - 1].ignore) &&
                        !(st.in_field && st.in_fldinst))
                    {
                        if (st.in_field && st.in_fldrslt)
                            sb_append_char(&st.fld_result, '-');
                        else
                            rtf_append_plain(&st, "-", 1);
                    }
                }
                else if (nc == ' ' || nc == '\n' || nc == '\r' ||
                         nc == '\t')
                {
                    if (!(st.depth > 0 &&
                          st.stack[st.depth - 1].ignore) &&
                        !(st.in_field && st.in_fldinst))
                    {
                        if (st.in_field && st.in_fldrslt)
                            sb_append_char(&st.fld_result, ' ');
                        else
                            rtf_append_plain(&st, " ", 1);
                    }
                }
                /* \*, \-, \:, etc.: destination / formatting, ignore. */
                if (nc == '*' && st.depth > 0)
                    st.stack[st.depth - 1].star = 1;
                i += 2;
                continue;
            }
            /* control word */
            j = i + 1;
            while (j < len && ((rtf[j] >= 'a' && rtf[j] <= 'z') ||
                              (rtf[j] >= 'A' && rtf[j] <= 'Z')))
                j++;
            {
                char word[32];
                size_t wl;
                long param;
                int has_param;
                int neg;

                wl = j - (i + 1);
                if (wl > 31)
                    wl = 31;
                memcpy(word, rtf + i + 1, wl);
                word[wl] = '\0';
                has_param = 0;
                param = 0;
                neg = 0;
                if (j < len && (rtf[j] == '-' ||
                               (rtf[j] >= '0' && rtf[j] <= '9')))
                {
                    has_param = 1;
                    if (rtf[j] == '-')
                    {
                        neg = 1;
                        j++;
                    }
                    param = 0;
                    while (j < len && rtf[j] >= '0' && rtf[j] <= '9')
                    {
                        param = param * 10 + (rtf[j] - '0');
                        j++;
                    }
                    if (neg)
                        param = -param;
                }
                if (j < len && rtf[j] == ' ')
                    j++;
                i = j;

                /* {\*destination ...}: ignore unless field-related. */
                if (st.depth > 0 && st.stack[st.depth - 1].star)
                {
                    st.stack[st.depth - 1].star = 0;
                    if (!(strcmp(word, "field") == 0 ||
                          strcmp(word, "fldinst") == 0 ||
                          strcmp(word, "fldrslt") == 0 ||
                          strcmp(word, "formfield") == 0 ||
                          strcmp(word, "datafield") == 0 ||
                          strcmp(word, "ffname") == 0 ||
                          strcmp(word, "ffdeftext") == 0))
                    {
                        st.stack[st.depth - 1].ignore = 1;
                        continue;
                    }
                }

                if (strcmp(word, "par") == 0 ||
                    strcmp(word, "line") == 0)
                {
                    rtf_flush_para(&st);
                }
                else if (strcmp(word, "row") == 0)
                {
                    rtf_flush_row(&st);
                }
                else if (strcmp(word, "cell") == 0)
                {
                    if (st.in_row)
                    {
                        if (st.cell_idx >= 0 &&
                            st.cell_idx < RTF_MAX_CELLS)
                            rtf_close_markers(
                                &st.cells[st.cell_idx], &st);
                        st.cell_idx++;
                        if (st.cell_idx >= RTF_MAX_CELLS)
                            st.cell_idx = RTF_MAX_CELLS - 1;
                        st.cur_bold = 0;
                        st.cur_italic = 0;
                        st.cur_mono = 0;
                    }
                }
                else if (strcmp(word, "trowd") == 0)
                {
                    if (!st.in_row)
                    {
                        int k;

                        if (st.para.data != NULL && st.para.len > 0)
                            rtf_flush_para(&st);
                        st.in_row = 1;
                        st.cell_idx = 0;
                        k = 0;
                        while (k < RTF_MAX_CELLS)
                        {
                            st.cells[k].len = 0;
                            if (st.cells[k].data != NULL)
                                st.cells[k].data[0] = '\0';
                            k++;
                        }
                        if (st.prev_block != 6)
                            st.table_first = 1;
                        st.cur_bold = 0;
                        st.cur_italic = 0;
                        st.cur_mono = 0;
                        st.p_bold = 0;
                        st.p_italic = 0;
                        st.p_mono = 0;
                    }
                }
                else if (strcmp(word, "intbl") == 0)
                {
                    if (!st.in_row)
                    {
                        int k;

                        st.in_row = 1;
                        st.cell_idx = 0;
                        k = 0;
                        while (k < RTF_MAX_CELLS)
                        {
                            st.cells[k].len = 0;
                            if (st.cells[k].data != NULL)
                                st.cells[k].data[0] = '\0';
                            k++;
                        }
                        if (st.prev_block != 6)
                            st.table_first = 1;
                    }
                }
                else if (strcmp(word, "b") == 0)
                {
                    int on;

                    on = !has_param || param != 0;
                    rtf_toggle_bold(&st, on);
                }
                else if (strcmp(word, "i") == 0)
                {
                    int on;

                    on = !has_param || param != 0;
                    rtf_toggle_italic(&st, on);
                }
                else if (strcmp(word, "f") == 0)
                {
                    if (st.fonttbl_depth >= 0 &&
                        st.depth > st.fonttbl_depth)
                    {
                        /* fonttbl entry: name text follows */
                        if (has_param && param >= 0 &&
                            param < RTF_MAX_FONTS)
                            st.font_entry = (int)param;
                        else
                            st.font_entry = -1;
                        st.font_namelen = 0;
                    }
                    /* Mono by fonttbl name only: controls add their
                       own fonts (e.g. Cambria Math for missing
                       glyphs), so a bare \f1 proves nothing. */
                    else if (has_param &&
                             is_mono_font(&st, (int)param))
                        rtf_toggle_mono(&st, 1);
                    else
                        rtf_toggle_mono(&st, 0);
                }
                else if (strcmp(word, "fs") == 0)
                {
                    if (has_param)
                    {
                        st.cur_fs = (int)param;
                        if (!st.para_has_text && !st.in_row &&
                            !(st.in_field && st.in_fldrslt))
                            st.para_first_fs = (int)param;
                    }
                }
                else if (strcmp(word, "u") == 0)
                {
                    long v;
                    unsigned long cp;
                    char ub[5];
                    int nb;

                    v = has_param ? param : 0;
                    if (v < 0)
                        v += 65536;
                    cp = (unsigned long)v;
                    nb = utf8_encode(cp, ub);
                    if (!(st.depth > 0 &&
                          st.stack[st.depth - 1].ignore))
                    {
                        if (st.in_field && st.in_fldinst)
                        {
                        }
                        else if (st.in_field && st.in_fldrslt)
                        {
                            sb_append_n(&st.fld_result, ub, (size_t)nb);
                        }
                        else
                        {
                            rtf_append_plain(&st, ub, (size_t)nb);
                        }
                    }
                    /* skip \ucN fallback chars (DBCS-aware, see helper) */
                    rtf_skip_u_fallback(rtf, len, &i, st.uc, st.ansi_cp);
                }
                else if (strcmp(word, "tab") == 0)
                {
                    rtf_append_plain(&st, " ", 1);
                }
                else if (strcmp(word, "emdash") == 0)
                {
                    char ub[5];

                    ub[0] = (char)0xE2;
                    ub[1] = (char)0x80;
                    ub[2] = (char)0x94;
                    rtf_append_plain(&st, ub, 3);
                }
                else if (strcmp(word, "endash") == 0)
                {
                    char ub[5];

                    ub[0] = (char)0xE2;
                    ub[1] = (char)0x80;
                    ub[2] = (char)0x93;
                    rtf_append_plain(&st, ub, 3);
                }
                else if (strcmp(word, "bullet") == 0)
                {
                    char ub[5];

                    ub[0] = (char)0xE2;
                    ub[1] = (char)0x80;
                    ub[2] = (char)0xA2;
                    rtf_append_plain(&st, ub, 3);
                }
                else if (strcmp(word, "lquote") == 0 ||
                         strcmp(word, "rquote") == 0)
                {
                    char ub[5];

                    ub[0] = (char)0xE2;
                    ub[1] = (char)0x80;
                    ub[2] = (char)0x99;
                    rtf_append_plain(&st, ub, 3);
                }
                else if (strcmp(word, "ldblquote") == 0 ||
                         strcmp(word, "rdblquote") == 0)
                {
                    char ub[5];

                    ub[0] = (char)0xE2;
                    ub[1] = (char)0x80;
                    ub[2] = (char)0x9C;
                    rtf_append_plain(&st, ub, 3);
                }
                else if (strcmp(word, "field") == 0)
                {
                    if (st.depth > 0)
                    {
                        st.stack[st.depth - 1].is_field = 1;
                        st.field_depth = st.depth;
                        st.in_field = 1;
                        st.in_fldinst = 0;
                        st.in_fldrslt = 0;
                        st.fld_url.len = 0;
                        if (st.fld_url.data != NULL)
                            st.fld_url.data[0] = '\0';
                        st.fld_result.len = 0;
                        if (st.fld_result.data != NULL)
                            st.fld_result.data[0] = '\0';
                    }
                }
                else if (strcmp(word, "fldinst") == 0)
                {
                    st.in_fldinst = 1;
                    st.fldinst_depth = st.depth;
                    st.in_fldrslt = 0;
                }
                else if (strcmp(word, "fldrslt") == 0)
                {
                    st.in_fldrslt = 1;
                    st.fldrslt_depth = st.depth;
                    st.in_fldinst = 0;
                }
                else if (strcmp(word, "ansicpg") == 0)
                {
                    if (has_param && param > 0 && param < 65536)
                        st.ansi_cp = (int)param;
                }
                else if (strcmp(word, "uc") == 0)
                {
                    if (has_param && param >= 0 && param <= 8)
                        st.uc = (int)param;
                    else if (!has_param)
                        st.uc = 1;
                }
                else if (strcmp(word, "fonttbl") == 0)
                {
                    if (st.depth > 0)
                        st.stack[st.depth - 1].ignore = 1;
                    st.fonttbl_depth = st.depth;
                    st.font_entry = -1;
                    st.font_namelen = 0;
                    st.fonts_tbl = 1;
                }
                else if (strcmp(word, "colortbl") == 0 ||
                         strcmp(word, "stylesheet") == 0 ||
                         strcmp(word, "info") == 0 ||
                         strcmp(word, "title") == 0 ||
                         strcmp(word, "author") == 0 ||
                         strcmp(word, "subject") == 0 ||
                         strcmp(word, "keywords") == 0 ||
                         strcmp(word, "comment") == 0 ||
                         strcmp(word, "generator") == 0 ||
                         strcmp(word, "themedata") == 0 ||
                         strcmp(word, "colorschememapping") == 0 ||
                         strcmp(word, "datastore") == 0 ||
                         strcmp(word, "mmathPr") == 0)
                {
                    if (st.depth > 0)
                        st.stack[st.depth - 1].ignore = 1;
                }
                else if (strcmp(word, "pard") == 0 ||
                         strcmp(word, "plain") == 0)
                {
                    if (st.para.data != NULL && st.para.len > 0 &&
                        !st.in_row)
                    {
                        /* missing \par: flush to avoid mixing */
                    }
                    st.cur_bold = 0;
                    st.cur_italic = 0;
                    st.cur_mono = 0;
                    st.cur_fs = 20;
                    if (!st.para_has_text)
                        st.para_first_fs = 20;
                }
                else
                {
                    /* ignore other controls */
                }
            }
        }
        else if (ch == '\n' || ch == '\r')
        {
            i++;
        }
        else
        {
            size_t j;

            j = i;
            while (j < len && rtf[j] != '\\' &&
                   rtf[j] != '{' && rtf[j] != '}' &&
                   rtf[j] != '\n' && rtf[j] != '\r')
                j++;
            if (j > i)
            {
                if (st.fonttbl_depth >= 0 &&
                    st.depth > st.fonttbl_depth)
                {
                    /* fonttbl entry name text; ';' ends the entry */
                    size_t k;

                    k = i;
                    while (k < j)
                    {
                        if (rtf[k] == ';')
                        {
                            int ismono;

                            st.font_name[st.font_namelen] = '\0';
                            ismono = 0;
                            if (st.font_entry >= 0 &&
                                st.font_entry < RTF_MAX_FONTS)
                            {
                                ismono =
                                    is_mono_font_name(st.font_name);
                                st.fonts_mono[st.font_entry] = ismono;
                            }
                            st.font_entry = -1;
                            st.font_namelen = 0;
                        }
                        else if (st.font_namelen <
                                 (int)sizeof(st.font_name) - 1)
                        {
                            st.font_name[st.font_namelen] = rtf[k];
                            st.font_namelen++;
                        }
                        k++;
                    }
                }
                else if (!(st.depth > 0 &&
                      st.stack[st.depth - 1].ignore))
                {
                    if (st.in_field && st.in_fldinst)
                    {
                        const char *seg;
                        const char *hl;
                        size_t seglen;

                        seg = rtf + i;
                        seglen = j - i;
                        {
                            char *tmp;

                            tmp = (char *)malloc(seglen + 1);
                            if (tmp != NULL)
                            {
                                memcpy(tmp, seg, seglen);
                                tmp[seglen] = '\0';
                                hl = strstr(tmp, "HYPERLINK");
                                if (hl != NULL &&
                                    st.fld_url.len == 0)
                                {
                                    const char *q1;
                                    const char *q2;

                                    q1 = strchr(hl, '"');
                                    if (q1 != NULL)
                                    {
                                        q2 = strchr(q1 + 1, '"');
                                        if (q2 != NULL &&
                                            q2 > q1 + 1)
                                        {
                                            sb_append_n(&st.fld_url,
                                                q1 + 1,
                                                (size_t)(q2 - q1 - 1));
                                        }
                                    }
                                }
                                free(tmp);
                            }
                        }
                    }
                    else if (st.in_field && st.in_fldrslt)
                    {
                        size_t k;

                        k = i;
                        while (k < j)
                        {
                            unsigned char cc;

                            cc = (unsigned char)rtf[k];
                            if (cc < 0x80)
                            {
                                /* Link text is raw on the md->rtf side
                                   (rtf_append_text, no unescaping), so keep
                                   it raw here too; escaping backslashes
                                   would double them every cycle. */
                                sb_append_char(&st.fld_result,
                                               (char)cc);
                                k++;
                            }
                            else
                            {
                                unsigned long cp;
                                int nb;
                                char ub[5];
                                int wb;

                                if (utf8_decode(
                                        (const unsigned char *)rtf + k,
                                        j - k, &cp, &nb))
                                {
                                    wb = utf8_encode(cp, ub);
                                    sb_append_n(&st.fld_result,
                                                ub, (size_t)wb);
                                    k += (size_t)nb;
                                }
                                else
                                {
                                    sb_append_char(&st.fld_result, '?');
                                    k++;
                                }
                            }
                        }
                    }
                    else
                    {
                        rtf_append_plain(&st, rtf + i, j - i);
                    }
                }
                i = j;
            }
            else
            {
                i++;
            }
        }
    }
    if (st.in_row)
        rtf_flush_row(&st);
    else
        rtf_flush_para(&st);
    {
        char *res;

        if (st.md.data == NULL)
        {
            res = (char *)malloc(1);
            if (res != NULL)
                res[0] = '\0';
        }
        else
        {
            res = st.md.data;
            st.md.data = NULL;
        }
        sb_free(&st.md);
        sb_free(&st.para);
        sb_free(&st.fld_url);
        sb_free(&st.fld_result);
        c = 0;
        while (c < RTF_MAX_CELLS)
        {
            sb_free(&st.cells[c]);
            c++;
        }
        return res;
    }
}

/*
 * ----------------------------------------------------------------------
 * Memory streaming + markdown file dispatch
 * ----------------------------------------------------------------------
 */

typedef struct
{
    const char *buf;
    LONG len;
    LONG pos;
} MemIn;

typedef struct
{
    char *buf;
    LONG len;
    LONG cap;
    int failed;
} MemOut;

static DWORD CALLBACK
StreamInMemCallback(DWORD_PTR dwCookie, LPBYTE pbBuff,
                    LONG cb, LONG *pcb)
{
    MemIn *m;

    m = (MemIn *)dwCookie;
    if (m->pos >= m->len)
    {
        *pcb = 0;
        return 0;
    }
    if (cb > m->len - m->pos)
        cb = m->len - m->pos;
    memcpy(pbBuff, m->buf + m->pos, (size_t)cb);
    m->pos += cb;
    *pcb = cb;
    return 0;
}

static DWORD CALLBACK
StreamOutMemCallback(DWORD_PTR dwCookie, LPBYTE pbBuff,
                     LONG cb, LONG *pcb)
{
    MemOut *m;
    LONG need;

    m = (MemOut *)dwCookie;
    if (m->failed)
    {
        *pcb = 0;
        return 1;
    }
    /* +2: keep two spare NUL bytes so the buffer also terminates
       a WCHAR string for the Unicode text path. */
    need = m->len + cb + 2;
    if (need > m->cap)
    {
        LONG newcap;
        char *nb;

        newcap = m->cap > 0 ? m->cap : 8192;
        while (newcap < need)
            newcap *= 2;
        nb = (char *)realloc(m->buf, (size_t)newcap);
        if (nb == NULL)
        {
            m->failed = 1;
            *pcb = 0;
            return 1;
        }
        m->buf = nb;
        m->cap = newcap;
    }
    memcpy(m->buf + m->len, pbBuff, (size_t)cb);
    m->len += cb;
    m->buf[m->len] = '\0';
    m->buf[m->len + 1] = '\0';
    *pcb = cb;
    return 0;
}

static char *
read_entire_file(LPCTSTR path, size_t *out_len)
{
    FILE *fp;
    long sz;
    size_t len;
    char *buf;
    size_t got;

    fp = _tfopen(path, _T("rb"));
    if (fp == NULL)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return NULL;
    }
    sz = ftell(fp);
    if (sz < 0)
    {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    len = (size_t)sz;
    buf = (char *)malloc(len + 1);
    if (buf == NULL)
    {
        fclose(fp);
        return NULL;
    }
    got = fread(buf, 1, len, fp);
    fclose(fp);
    buf[got] = '\0';
    if (out_len != NULL)
        *out_len = got;
    return buf;
}

static int
write_entire_file(LPCTSTR path, const char *data)
{
    FILE *fp;
    size_t len;
    size_t wrote;

    fp = _tfopen(path, _T("wb"));
    if (fp == NULL)
        return 0;
    len = strlen(data);
    wrote = fwrite(data, 1, len, fp);
    fclose(fp);
    return wrote == len;
}

static int
has_ext_ci(LPCTSTR filename, LPCTSTR ext)
{
    size_t fl;
    size_t el;

    fl = _tcslen(filename);
    el = _tcslen(ext);
    if (el >= fl)
        return 0;
    filename += fl - el;
    while (*ext != _T('\0'))
    {
        TCHAR a;
        TCHAR b;

        a = *filename;
        b = *ext;
        if (a >= _T('A') && a <= _T('Z'))
            a = (TCHAR)(a + 32);
        if (b >= _T('A') && b <= _T('Z'))
            b = (TCHAR)(b + 32);
        if (a != b)
            return 0;
        filename++;
        ext++;
    }
    return 1;
}

static int
is_markdown_file(LPCTSTR filename)
{
    if (has_ext_ci(filename, _T(".md")))
        return 1;
    if (has_ext_ci(filename, _T(".markdown")))
        return 1;
    if (has_ext_ci(filename, _T(".mkd")))
        return 1;
    if (has_ext_ci(filename, _T(".mdown")))
        return 1;
    if (has_ext_ci(filename, _T(".txt")))
        return 1;
    return 0;
}

/*
 * ----------------------------------------------------------------------
 * File operations
 * ----------------------------------------------------------------------
 */

static int
LoadRTF(HWND hwndEdit, LPCTSTR filename)
{
    FILE *fp;
    EDITSTREAM es;

    if (g_showSource)
    {
        char *rtf;
        char *md;

        rtf = read_entire_file(filename, NULL);
        if (rtf == NULL)
        {
            MessageBox(hwndEdit,
                       _T("Unable to open the RTF file."),
                       _T("Open"),
                       MB_OK | MB_ICONERROR);
            return 0;
        }
        md = rtf_to_md(rtf);
        free(rtf);
        if (md == NULL)
        {
            MessageBox(hwndEdit,
                       _T("Out of memory."),
                       _T("Open"),
                       MB_OK | MB_ICONERROR);
            return 0;
        }
        if (!stream_editor_text_in(hwndEdit, md))
        {
            free(md);
            MessageBox(hwndEdit,
                       _T("Unable to load the RTF file."),
                       _T("Open"),
                       MB_OK | MB_ICONERROR);
            return 0;
        }
        free(md);
        _tcsncpy(g_filename, filename, MAX_PATH - 1);
        g_filename[MAX_PATH - 1] = _T('\0');
        UpdateTitle();
        reset_source_format();
        return 1;
    }

    fp = _tfopen(filename, _T("rb"));

    if (fp == NULL)
    {
        MessageBox(hwndEdit,
                   _T("Unable to open the RTF file."),
                   _T("Open"),
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
                   _T("Unable to load the RTF file."),
                   _T("Open"),
                   MB_OK | MB_ICONERROR);
        return 0;
    }

    _tcsncpy(g_filename, filename, MAX_PATH - 1);
    g_filename[MAX_PATH - 1] = _T('\0');

    UpdateTitle();

    return 1;
}

static int
SaveRTF(HWND hwndEdit, LPCTSTR filename)
{
    FILE *fp;
    EDITSTREAM es;

    if (g_showSource)
    {
        char *md;
        char *rtf;

        md = stream_editor_text_out(hwndEdit);
        if (md == NULL)
        {
            MessageBox(hwndEdit,
                       _T("Unable to save the RTF file."),
                       _T("Save"),
                       MB_OK | MB_ICONERROR);
            return 0;
        }
        rtf = md_to_rtf(md);
        free(md);
        if (rtf == NULL)
        {
            MessageBox(hwndEdit,
                       _T("Out of memory."),
                       _T("Save"),
                       MB_OK | MB_ICONERROR);
            return 0;
        }
        fp = _tfopen(filename, _T("wb"));
        if (fp == NULL)
        {
            free(rtf);
            MessageBox(hwndEdit,
                       _T("Unable to create the RTF file."),
                       _T("Save"),
                       MB_OK | MB_ICONERROR);
            return 0;
        }
        fwrite(rtf, 1, strlen(rtf), fp);
        fclose(fp);
        free(rtf);
        _tcsncpy(g_filename, filename, MAX_PATH - 1);
        g_filename[MAX_PATH - 1] = _T('\0');
        UpdateTitle();
        return 1;
    }

    fp = _tfopen(filename, _T("wb"));

    if (fp == NULL)
    {
        MessageBox(hwndEdit,
                   _T("Unable to create the RTF file."),
                   _T("Save"),
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
                   _T("Unable to save the RTF file."),
                   _T("Save"),
                   MB_OK | MB_ICONERROR);
        return 0;
    }

    _tcsncpy(g_filename, filename, MAX_PATH - 1);
    g_filename[MAX_PATH - 1] = _T('\0');

    UpdateTitle();

    return 1;
}

static int
LoadMarkdown(HWND hwndEdit, LPCTSTR filename)
{
    char *md;
    char *rtf;
    MemIn m;
    EDITSTREAM es;

    md = read_entire_file(filename, NULL);
    if (md == NULL)
    {
        MessageBox(hwndEdit,
                   _T("Unable to open the file."),
                   _T("Open"),
                   MB_OK | MB_ICONERROR);
        return 0;
    }
    if (g_showSource)
    {
        if (!stream_editor_text_in(hwndEdit, md))
        {
            free(md);
            MessageBox(hwndEdit,
                       _T("Unable to load the file."),
                       _T("Open"),
                       MB_OK | MB_ICONERROR);
            return 0;
        }
        free(md);
        _tcsncpy(g_filename, filename, MAX_PATH - 1);
        g_filename[MAX_PATH - 1] = _T('\0');
        UpdateTitle();
        reset_source_format();
        return 1;
    }
    rtf = md_to_rtf(md);
    if (rtf == NULL)
    {
        free(md);
        MessageBox(hwndEdit,
                   _T("Out of memory."),
                   _T("Open"),
                   MB_OK | MB_ICONERROR);
        return 0;
    }
    memset(&m, 0, sizeof(m));
    m.buf = rtf;
    m.len = (LONG)strlen(rtf);
    m.pos = 0;
    memset(&es, 0, sizeof(es));
    es.dwCookie = (DWORD_PTR)&m;
    es.pfnCallback = StreamInMemCallback;
    SendMessage(hwndEdit,
                EM_STREAMIN,
                (WPARAM)SF_RTF,
                (LPARAM)&es);
    free(rtf);
    if (es.dwError != 0)
    {
        free(md);
        MessageBox(hwndEdit,
                   _T("Unable to load the file."),
                   _T("Open"),
                   MB_OK | MB_ICONERROR);
        return 0;
    }
    _tcsncpy(g_filename, filename, MAX_PATH - 1);
    g_filename[MAX_PATH - 1] = _T('\0');
    UpdateTitle();
    apply_link_effects(md);
    free(md);
    return 1;
}

static int
SaveMarkdown(HWND hwndEdit, LPCTSTR filename)
{
    MemOut m;
    EDITSTREAM es;
    char *rtf;
    char *md;
    int ok;
    WPARAM tryfmt;

    if (g_showSource)
    {
        md = stream_editor_text_out(hwndEdit);
        if (md == NULL)
        {
            MessageBox(hwndEdit,
                       _T("Unable to save the file."),
                       _T("Save"),
                       MB_OK | MB_ICONERROR);
            return 0;
        }
        ok = write_entire_file(filename, md);
        free(md);
        if (!ok)
        {
            MessageBox(hwndEdit,
                       _T("Unable to create the file."),
                       _T("Save"),
                       MB_OK | MB_ICONERROR);
            return 0;
        }
        _tcsncpy(g_filename, filename, MAX_PATH - 1);
        g_filename[MAX_PATH - 1] = _T('\0');
        UpdateTitle();
        return 1;
    }
    memset(&m, 0, sizeof(m));
    memset(&es, 0, sizeof(es));
    es.dwCookie = (DWORD_PTR)&m;
    es.pfnCallback = StreamOutMemCallback;
    /* Prefer UTF-8 RTF on 3.0+ (codepage-independent); fall back
       to system codepage RTF on 2.0 or earlier. */
    tryfmt = (WPARAM)(((DWORD)CP_UTF8 << 16) |
                      (SF_RTF | SF_USECODEPAGE));
    SendMessage(hwndEdit,
                EM_STREAMOUT,
                tryfmt,
                (LPARAM)&es);
    if (es.dwError != 0 || m.failed)
    {
        if (m.buf != NULL)
        {
            free(m.buf);
            m.buf = NULL;
        }
        m.len = 0;
        m.cap = 0;
        m.failed = 0;
        memset(&es, 0, sizeof(es));
        es.dwCookie = (DWORD_PTR)&m;
        es.pfnCallback = StreamOutMemCallback;
        SendMessage(hwndEdit,
                    EM_STREAMOUT,
                    (WPARAM)SF_RTF,
                    (LPARAM)&es);
    }
    if (es.dwError != 0 || m.failed)
    {
        if (m.buf != NULL)
            free(m.buf);
        MessageBox(hwndEdit,
                   _T("Unable to save the file."),
                   _T("Save"),
                   MB_OK | MB_ICONERROR);
        return 0;
    }
    rtf = m.buf != NULL ? m.buf : NULL;
    if (rtf == NULL)
    {
        rtf = (char *)malloc(1);
        if (rtf == NULL)
        {
            MessageBox(hwndEdit,
                       _T("Out of memory."),
                       _T("Save"),
                       MB_OK | MB_ICONERROR);
            return 0;
        }
        rtf[0] = '\0';
    }
    md = rtf_to_md(rtf);
    free(rtf);
    if (md == NULL)
    {
        MessageBox(hwndEdit,
                   _T("Out of memory."),
                   _T("Save"),
                   MB_OK | MB_ICONERROR);
        return 0;
    }
    ok = write_entire_file(filename, md);
    free(md);
    if (!ok)
    {
        MessageBox(hwndEdit,
                   _T("Unable to create the file."),
                   _T("Save"),
                   MB_OK | MB_ICONERROR);
        return 0;
    }
    _tcsncpy(g_filename, filename, MAX_PATH - 1);
    g_filename[MAX_PATH - 1] = _T('\0');
    UpdateTitle();
    return 1;
}

static int
LoadAny(HWND hwndEdit, LPCTSTR filename)
{
    if (is_markdown_file(filename))
        return LoadMarkdown(hwndEdit, filename);
    return LoadRTF(hwndEdit, filename);
}

static int
SaveAny(HWND hwndEdit, LPCTSTR filename)
{
    if (is_markdown_file(filename))
        return SaveMarkdown(hwndEdit, filename);
    return SaveRTF(hwndEdit, filename);
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
    TCHAR filename[MAX_PATH];

    memset(&ofn, 0, sizeof(ofn));
    memset(filename, 0, sizeof(filename));

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter =
        _T("Markdown (*.md;*.markdown;*.mkd;*.mdown;*.txt)\0")
        _T("*.md;*.markdown;*.mkd;*.mdown;*.txt\0")
        _T("Rich Text Format (*.rtf)\0*.rtf\0")
        _T("All Files (*.*)\0*.*\0\0");
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST |
                OFN_HIDEREADONLY |
                OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = _T("md");

    if (!GetOpenFileName(&ofn))
        return 0;

    return LoadAny(g_hwndEdit, filename);
}

static int
SaveAsFile(HWND hwnd)
{
    OPENFILENAME ofn;
    TCHAR filename[MAX_PATH];

    memset(&ofn, 0, sizeof(ofn));
    memset(filename, 0, sizeof(filename));

    if (g_filename[0] != _T('\0'))
    {
        _tcsncpy(filename, g_filename, MAX_PATH - 1);
        filename[MAX_PATH - 1] = _T('\0');
    }
    else
    {
        _tcscpy(filename, _T("document.md"));
    }

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter =
        _T("Markdown (*.md;*.markdown;*.mkd;*.mdown;*.txt)\0")
        _T("*.md;*.markdown;*.mkd;*.mdown;*.txt\0")
        _T("Rich Text Format (*.rtf)\0*.rtf\0")
        _T("All Files (*.*)\0*.*\0\0");
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT |
                OFN_HIDEREADONLY |
                OFN_PATHMUSTEXIST;
    if (is_markdown_file(filename))
        ofn.lpstrDefExt = _T("md");
    else
        ofn.lpstrDefExt = _T("rtf");

    if (!GetSaveFileName(&ofn))
        return 0;

    return SaveAny(g_hwndEdit, filename);
}

static int
SaveFile(HWND hwnd)
{
    if (g_filename[0] == _T('\0'))
        return SaveAsFile(hwnd);

    return SaveAny(g_hwndEdit, g_filename);
}

/*
 * ----------------------------------------------------------------------
 * Markdown source / rich view toggle
 * ----------------------------------------------------------------------
 */

static char *
stream_editor_out(HWND hwndEdit, WPARAM fmt)
{
    MemOut m;
    EDITSTREAM es;
    WPARAM tryfmt;

    memset(&m, 0, sizeof(m));
    memset(&es, 0, sizeof(es));
    es.dwCookie = (DWORD_PTR)&m;
    es.pfnCallback = StreamOutMemCallback;
    if (fmt == SF_RTF)
    {
        /* RichEdit 3.0+: request UTF-8 RTF (\ansicpg65001) so \'xx
           bytes are codepage-independent; 2.0 or earlier ignores
           SF_USECODEPAGE and fails, then we fall back to plain
           SF_RTF (system codepage, handled via \ansicpg). */
        tryfmt = (WPARAM)(((DWORD)CP_UTF8 << 16) |
                          (SF_RTF | SF_USECODEPAGE));
        SendMessage(hwndEdit, EM_STREAMOUT, tryfmt, (LPARAM)&es);
        if (es.dwError == 0 && !m.failed)
        {
            if (m.buf == NULL)
            {
                m.buf = (char *)malloc(2);
                if (m.buf == NULL)
                    return NULL;
                m.buf[0] = '\0';
                m.buf[1] = '\0';
            }
            return m.buf;
        }
        if (m.buf != NULL)
        {
            free(m.buf);
            m.buf = NULL;
        }
        m.len = 0;
        m.cap = 0;
        m.failed = 0;
        memset(&es, 0, sizeof(es));
        es.dwCookie = (DWORD_PTR)&m;
        es.pfnCallback = StreamOutMemCallback;
    }
    SendMessage(hwndEdit, EM_STREAMOUT, fmt, (LPARAM)&es);
    if (es.dwError != 0 || m.failed)
    {
        if (m.buf != NULL)
            free(m.buf);
        return NULL;
    }
    if (m.buf == NULL)
    {
        /* Two NULs: also terminates an empty WCHAR string for the
           Unicode text path (single byte would over-read heap). */
        m.buf = (char *)malloc(2);
        if (m.buf == NULL)
            return NULL;
        m.buf[0] = '\0';
        m.buf[1] = '\0';
    }
    return m.buf;
}

static int
stream_editor_in(HWND hwndEdit, WPARAM fmt, const char *text)
{
    MemIn m;
    EDITSTREAM es;

    if (text == NULL)
        text = "";
    memset(&m, 0, sizeof(m));
    m.buf = text;
    m.len = (LONG)strlen(text);
    m.pos = 0;
    memset(&es, 0, sizeof(es));
    es.dwCookie = (DWORD_PTR)&m;
    es.pfnCallback = StreamInMemCallback;
    SendMessage(hwndEdit, EM_STREAMIN, fmt, (LPARAM)&es);
    return es.dwError == 0;
}

/*
 * ----------------------------------------------------------------------
 * UTF-8 <-> UTF-16 boundary (both flavors, manual for retro Windows)
 * ----------------------------------------------------------------------
 *
 * Converters and files stay byte-based (UTF-8 + RTF); text exchanged
 * with the control needs conversion. Manual codecs avoid CP_UTF8,
 * which is missing on some retro Windows. Both helpers return
 * malloc'd buffers; caller frees (NULL on failure).
 */

static WCHAR *
utf8_to_wide(const char *s)
{
    size_t len;
    size_t i;
    size_t need;
    WCHAR *w;
    size_t k;

    /* Manual UTF-8 -> UTF-16 (no CP_UTF8 dependency for retro Windows).
       Fails (NULL) on invalid sequences, like the OS API. */
    if (s == NULL)
        s = "";
    len = strlen(s);
    need = 1;
    i = 0;
    while (i < len)
    {
        unsigned long cp;
        int nb;

        if (!utf8_decode((const unsigned char *)s + i, len - i, &cp, &nb))
            return NULL;
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return NULL;
        if (cp > 0x10FFFF)
            return NULL;
        if (cp < 0x10000)
            need += 1;
        else
            need += 2;
        i += (size_t)nb;
    }
    w = (WCHAR *)malloc(need * sizeof(WCHAR));
    if (w == NULL)
        return NULL;
    i = 0;
    k = 0;
    while (i < len)
    {
        unsigned long cp;
        int nb;

        utf8_decode((const unsigned char *)s + i, len - i, &cp, &nb);
        if (cp < 0x10000)
        {
            w[k++] = (WCHAR)cp;
        }
        else
        {
            cp -= 0x10000;
            w[k++] = (WCHAR)(0xD800 + (cp >> 10));
            w[k++] = (WCHAR)(0xDC00 + (cp & 0x3FF));
        }
        i += (size_t)nb;
    }
    w[k] = 0;
    return w;
}

static char *
wide_to_utf8(const WCHAR *w, int wlen)
{
    size_t len;
    size_t i;
    size_t need;
    char *s;
    size_t k;
    char tmp[5];

    /* Manual UTF-16 -> UTF-8 (no CP_UTF8 dependency for retro Windows).
       Fails (NULL) on unpaired surrogates, like the OS API. */
    if (w == NULL)
        return NULL;
    if (wlen < 0)
        len = wcslen(w);
    else if (wlen == 0)
        return NULL;
    else
        len = (size_t)wlen;
    need = 0;
    i = 0;
    while (i < len)
    {
        unsigned long cp;
        WCHAR hi;
        WCHAR lo;

        hi = w[i];
        if (hi >= 0xD800 && hi <= 0xDBFF)
        {
            if (i + 1 >= len)
                return NULL;
            lo = w[i + 1];
            if (lo < 0xDC00 || lo > 0xDFFF)
                return NULL;
            need += 4;
            i += 2;
        }
        else if (hi >= 0xDC00 && hi <= 0xDFFF)
        {
            return NULL;
        }
        else
        {
            cp = (unsigned long)hi;
            if (cp < 0x80)
                need += 1;
            else if (cp < 0x800)
                need += 2;
            else
                need += 3;
            i += 1;
        }
    }
    s = (char *)malloc(need + 1);
    if (s == NULL)
        return NULL;
    i = 0;
    k = 0;
    while (i < len)
    {
        unsigned long cp;
        WCHAR hi;
        WCHAR lo;
        int nb;

        hi = w[i];
        if (hi >= 0xD800 && hi <= 0xDBFF)
        {
            lo = w[i + 1];
            cp = 0x10000UL +
                 (((unsigned long)(hi - 0xD800) << 10) |
                  (unsigned long)(lo - 0xDC00));
            nb = utf8_encode(cp, tmp);
            memcpy(s + k, tmp, (size_t)nb);
            k += (size_t)nb;
            i += 2;
        }
        else
        {
            cp = (unsigned long)hi;
            nb = utf8_encode(cp, tmp);
            memcpy(s + k, tmp, (size_t)nb);
            k += (size_t)nb;
            i += 1;
        }
    }
    s[k] = '\0';
    return s;
}

/* UTF-8 -> UTF-16 into a fixed buffer (manual, retro-safe).
   Returns WCHAR count including NUL, like MultiByteToWideChar,
   or 0 on invalid input or too-small buffer. */
static int
utf8_to_wide_buf(const char *s, WCHAR *out, int outcap)
{
    size_t len;
    size_t i;
    int k;

    if (s == NULL)
        s = "";
    if (out == NULL || outcap <= 1)
        return 0;
    len = strlen(s);
    i = 0;
    k = 0;
    while (i < len)
    {
        unsigned long cp;
        int nb;

        if (!utf8_decode((const unsigned char *)s + i, len - i, &cp, &nb))
            return 0;
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return 0;
        if (cp > 0x10FFFF)
            return 0;
        if (cp < 0x10000)
        {
            if (k + 1 >= outcap)
                return 0;
            out[k++] = (WCHAR)cp;
        }
        else
        {
            if (k + 2 >= outcap)
                return 0;
            cp -= 0x10000;
            out[k++] = (WCHAR)(0xD800 + (cp >> 10));
            out[k++] = (WCHAR)(0xDC00 + (cp & 0x3FF));
        }
        i += (size_t)nb;
    }
    out[k++] = 0;
    return k;
}

#ifndef UNICODE
static char *
ansi_to_utf8(const char *s)
{
    WCHAR *w;
    char *u;
    int wn;

    if (s == NULL)
        s = "";
    wn = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (wn <= 0)
        return NULL;
    w = (WCHAR *)malloc((size_t)wn * sizeof(WCHAR));
    if (w == NULL)
        return NULL;
    if (MultiByteToWideChar(CP_ACP, 0, s, -1, w, wn) <= 0)
    {
        free(w);
        return NULL;
    }
    u = wide_to_utf8(w, -1);
    free(w);
    return u;
}

static char *
utf8_to_ansi(const char *s)
{
    WCHAR *w;
    char *a;
    int an;

    if (s == NULL)
        s = "";
    w = utf8_to_wide(s);
    if (w == NULL)
        return NULL;
    an = WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
    if (an <= 0)
    {
        free(w);
        return NULL;
    }
    a = (char *)malloc((size_t)an);
    if (a == NULL)
    {
        free(w);
        return NULL;
    }
    if (WideCharToMultiByte(CP_ACP, 0, w, -1, a, an, NULL, NULL) <= 0)
    {
        free(w);
        free(a);
        return NULL;
    }
    free(w);
    return a;
}
#endif

/*
 * Plain-text exchange with the control, always UTF-8 on the caller
 * side (ANSI build prefers UTF-8 transfer on 3.0+, falls back to
 * ANSI with conversion; Unicode build uses UTF-16).
 */
static char *
stream_editor_text_out(HWND hwndEdit)
{
#ifdef UNICODE
    char *raw;
    char *utf8;

    raw = stream_editor_out(hwndEdit, SF_TEXT_EX);
    if (raw == NULL)
        return NULL;
    utf8 = wide_to_utf8((const WCHAR *)raw, -1);
    free(raw);
    return utf8;
#else
    MemOut m;
    EDITSTREAM es;
    WPARAM tryfmt;
    char *raw;
    char *utf8;

    memset(&m, 0, sizeof(m));
    memset(&es, 0, sizeof(es));
    es.dwCookie = (DWORD_PTR)&m;
    es.pfnCallback = StreamOutMemCallback;
    tryfmt = (WPARAM)(((DWORD)CP_UTF8 << 16) |
                      (SF_TEXT | SF_USECODEPAGE));
    SendMessage(hwndEdit, EM_STREAMOUT, tryfmt, (LPARAM)&es);
    if (es.dwError == 0 && !m.failed)
    {
        if (m.buf == NULL)
        {
            m.buf = (char *)malloc(1);
            if (m.buf == NULL)
                return NULL;
            m.buf[0] = '\0';
        }
        return m.buf;
    }
    if (m.buf != NULL)
        free(m.buf);
    raw = stream_editor_out(hwndEdit, SF_TEXT_EX);
    if (raw == NULL)
        return NULL;
    utf8 = ansi_to_utf8(raw);
    free(raw);
    return utf8;
#endif
}

static int
stream_editor_text_in(HWND hwndEdit, const char *text)
{
#ifdef UNICODE
    WCHAR *w;
    int ok;

    w = utf8_to_wide(text);
    if (w == NULL)
        return 0;
    {
        MemIn m;
        EDITSTREAM es;

        memset(&m, 0, sizeof(m));
        m.buf = (const char *)w;
        m.len = (LONG)(wcslen(w) * sizeof(WCHAR));
        m.pos = 0;
        memset(&es, 0, sizeof(es));
        es.dwCookie = (DWORD_PTR)&m;
        es.pfnCallback = StreamInMemCallback;
        SendMessage(hwndEdit, EM_STREAMIN, (WPARAM)SF_TEXT_EX, (LPARAM)&es);
        ok = (es.dwError == 0);
    }
    free(w);
    return ok;
#else
    MemIn m;
    EDITSTREAM es;
    WPARAM tryfmt;
    char *ansi;
    int ok;

    if (text == NULL)
        text = "";
    memset(&m, 0, sizeof(m));
    m.buf = text;
    m.len = (LONG)strlen(text);
    m.pos = 0;
    memset(&es, 0, sizeof(es));
    es.dwCookie = (DWORD_PTR)&m;
    es.pfnCallback = StreamInMemCallback;
    tryfmt = (WPARAM)(((DWORD)CP_UTF8 << 16) |
                      (SF_TEXT | SF_USECODEPAGE));
    SendMessage(hwndEdit, EM_STREAMIN, tryfmt, (LPARAM)&es);
    if (es.dwError == 0)
        return 1;
    ansi = utf8_to_ansi(text);
    if (ansi == NULL)
        return 0;
    ok = stream_editor_in(hwndEdit, SF_TEXT_EX, ansi);
    free(ansi);
    return ok;
#endif
}

static void
reset_source_format(void)
{
    CHARFORMAT cf;
    PARAFORMAT pf;

    if (g_hwndEdit == NULL)
        return;
    SendMessage(g_hwndEdit, EM_SETSEL, 0, -1);
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT |
                CFM_SIZE | CFM_FACE | CFM_COLOR | CFM_CHARSET;
    cf.dwEffects = 0;
    cf.yHeight = 200;
    cf.bCharSet = DEFAULT_CHARSET;
    _tcscpy(cf.szFaceName, _T("Courier New"));
    SendMessage(g_hwndEdit, EM_SETCHARFORMAT,
                (WPARAM)SCF_ALL, (LPARAM)&cf);
    memset(&pf, 0, sizeof(pf));
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_ALIGNMENT | PFM_STARTINDENT | PFM_RIGHTINDENT |
                PFM_OFFSET;
    pf.wAlignment = PFA_LEFT;
    pf.dxStartIndent = 0;
    pf.dxRightIndent = 0;
    pf.dxOffset = 0;
    SendMessage(g_hwndEdit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    SendMessage(g_hwndEdit, EM_SETSEL, 0, 0);
}

static void
UpdateSourceCheck(void)
{
    HMENU menu;

    if (g_hwndMain == NULL)
        return;
    menu = GetMenu(g_hwndMain);
    if (menu == NULL)
        return;
    CheckMenuItem(menu, IDM_SHOW_SOURCE,
                  MF_BYCOMMAND |
                  (g_showSource ? MF_CHECKED : MF_UNCHECKED));
}

static void
SetSourceMode(int on)
{
    char *out;
    char *conv;

    if (g_hwndEdit == NULL)
    {
        g_showSource = on ? 1 : 0;
        UpdateSourceCheck();
        return;
    }
    if ((on ? 1 : 0) == g_showSource)
    {
        UpdateSourceCheck();
        return;
    }
    if (on)
    {
        out = stream_editor_out(g_hwndEdit, SF_RTF);
        if (out == NULL)
        {
            MessageBox(g_hwndMain,
                       _T("Unable to read editor content."),
                       WND_TITLE,
                       MB_OK | MB_ICONERROR);
            return;
        }
        conv = rtf_to_md(out);
        free(out);
        if (conv == NULL)
        {
            MessageBox(g_hwndMain,
                       _T("Out of memory."),
                       WND_TITLE,
                       MB_OK | MB_ICONERROR);
            return;
        }
        if (!stream_editor_text_in(g_hwndEdit, conv))
        {
            free(conv);
            MessageBox(g_hwndMain,
                       _T("Unable to show markdown source."),
                       WND_TITLE,
                       MB_OK | MB_ICONERROR);
            return;
        }
        free(conv);
        reset_source_format();
        g_showSource = 1;
    }
    else
    {
        out = stream_editor_text_out(g_hwndEdit);
        if (out == NULL)
        {
            MessageBox(g_hwndMain,
                       _T("Unable to read editor content."),
                       WND_TITLE,
                       MB_OK | MB_ICONERROR);
            return;
        }
        conv = md_to_rtf(out);
        if (conv == NULL)
        {
            free(out);
            MessageBox(g_hwndMain,
                       _T("Out of memory."),
                       WND_TITLE,
                       MB_OK | MB_ICONERROR);
            return;
        }
        if (!stream_editor_in(g_hwndEdit, SF_RTF, conv))
        {
            free(conv);
            free(out);
            MessageBox(g_hwndMain,
                       _T("Unable to show rich text."),
                       WND_TITLE,
                       MB_OK | MB_ICONERROR);
            return;
        }
        free(conv);
        apply_link_effects(out);
        free(out);
        g_showSource = 0;
    }
    UpdateSourceCheck();
}

/*
 * ----------------------------------------------------------------------
 * Markdown links -> CFE_LINK effects (clickable on every RichEdit)
 * ----------------------------------------------------------------------
 *
 * HYPERLINK fields are only honored by newer controls (Msftedit):
 * RichEdit20 2.0/3.x expands them to "display <url>" plain text, so the
 * display part never fires EN_LINK there. Applying CFE_LINK to each
 * link display range after load makes display clicks notify on every
 * version; the URL is recovered at click time (field RTF on new
 * controls, nearby "<url>"/bare URL text on old ones).
 */

/* Markdown files and converter buffers are UTF-8 in both flavors;
   link display text uses utf8_to_wide_buf (manual, retro-safe). */

#define MD_MAX_LINKS 256
#define MD_LINK_TEXT_MAX 256
#define MD_LINK_URL_MAX 1024

typedef struct
{
    char text[MD_LINK_TEXT_MAX];
    char url[MD_LINK_URL_MAX];
} MdLink;

/* Collect [text](url) pairs in document order, skipping fenced code,
   indented code lines and `inline code` (mirrors md_to_rtf). */
static int
md_collect_links(const char *md, MdLink *out, int cap)
{
    size_t mdlen;
    size_t pos;
    int in_fence;
    int n;

    if (md == NULL || out == NULL || cap <= 0)
        return 0;
    mdlen = strlen(md);
    pos = 0;
    in_fence = 0;
    n = 0;
    while (pos < mdlen)
    {
        size_t eol;
        size_t len;
        char *line;
        int i;
        int in_code;

        eol = pos;
        while (eol < mdlen && md[eol] != '\n' && md[eol] != '\r')
            eol++;
        len = eol - pos;
        line = (char *)malloc(len + 1);
        if (line == NULL)
            return n;
        if (len > 0)
            memcpy(line, md + pos, len);
        line[len] = '\0';
        if (eol < mdlen && md[eol] == '\r' &&
            eol + 1 < mdlen && md[eol + 1] == '\n')
            pos = eol + 2;
        else if (eol < mdlen)
            pos = eol + 1;
        else
            pos = eol;

        if (md_is_fence(line))
        {
            in_fence = !in_fence;
            free(line);
            continue;
        }
        if (in_fence)
        {
            free(line);
            continue;
        }
        if (line[0] == '\t' ||
            (line[0] == ' ' && line[1] == ' ' &&
             line[2] == ' ' && line[3] == ' '))
        {
            free(line);
            continue;
        }
        in_code = 0;
        i = 0;
        while (line[i] != '\0')
        {
            if (line[i] == '`')
            {
                in_code = !in_code;
                i++;
            }
            else if (!in_code && line[i] == '[' &&
                     !(i > 0 && line[i - 1] == '\\'))
            {
                const char *t;
                const char *u;
                const char *ve;
                size_t tlen;
                size_t ulen;

                t = line + i + 1;
                u = strchr(t, ']');
                if (u != NULL && *(u + 1) == '(')
                {
                    ve = strchr(u + 2, ')');
                    if (ve != NULL)
                    {
                        tlen = (size_t)(u - t);
                        ulen = (size_t)(ve - (u + 2));
                        if (tlen > 0 && tlen < MD_LINK_TEXT_MAX &&
                            ulen > 0 && ulen < MD_LINK_URL_MAX &&
                            n < cap)
                        {
                            memcpy(out[n].text, t, tlen);
                            out[n].text[tlen] = '\0';
                            memcpy(out[n].url, u + 2, ulen);
                            out[n].url[ulen] = '\0';
                            n++;
                            if (n >= cap)
                            {
                                free(line);
                                return n;
                            }
                        }
                        i = (int)(ve - line) + 1;
                        continue;
                    }
                }
                i++;
            }
            else
            {
                i++;
            }
        }
        free(line);
    }
    return n;
}

static int
range_has_link(LONG s, LONG e)
{
    CHARRANGE cr;
    CHARRANGE old;
    CHARFORMAT cf;

    if (s < 0 || e <= s)
        return 0;
    cr.cpMin = s;
    cr.cpMax = e;
    SendMessage(g_hwndEdit, EM_EXGETSEL, 0, (LPARAM)&old);
    SendMessage(g_hwndEdit, EM_EXSETSEL, 0, (LPARAM)&cr);
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    SendMessage(g_hwndEdit, EM_GETCHARFORMAT,
                (WPARAM)SCF_SELECTION, (LPARAM)&cf);
    SendMessage(g_hwndEdit, EM_EXSETSEL, 0, (LPARAM)&old);
    return (cf.dwMask & CFM_LINK) && (cf.dwEffects & CFE_LINK);
}

static void
apply_link_effects(const char *md)
{
    MdLink *links;
    int n;
    int i;
    LONG pos;
    CHARRANGE old;
    CHARRANGE cr;
    CHARFORMAT cf;
    WCHAR wtext[MD_LINK_TEXT_MAX];
    char needle[1024];
    WCHAR wneedle[1024];
    FINDTEXTEXW ftw;
    int wlen;
    int guard;
    LONG search;
    LONG found;

    if (g_hwndEdit == NULL || md == NULL)
        return;
    links = (MdLink *)malloc(sizeof(MdLink) * MD_MAX_LINKS);
    if (links == NULL)
        return;
    n = md_collect_links(md, links, MD_MAX_LINKS);
    if (n <= 0)
    {
        free(links);
        return;
    }
    SendMessage(g_hwndEdit, EM_EXGETSEL, 0, (LPARAM)&old);
    pos = 0;
    i = 0;
    while (i < n)
    {
        found = 0;
        /* Phase 1: native field link (Msftedit marks results CFE_LINK).
           Plain lookalikes are skipped, never linked: no phantoms. */
        wlen = utf8_to_wide_buf(links[i].text, wtext, MD_LINK_TEXT_MAX);
        if (wlen > 1)
        {
            search = pos;
            guard = 0;
            while (guard++ < 128)
            {
                ftw.chrg.cpMin = search;
                ftw.chrg.cpMax = -1;
                ftw.lpstrText = wtext;
                if (SendMessageW(g_hwndEdit, EM_FINDTEXTEXW,
                                 (WPARAM)FR_DOWN, (LPARAM)&ftw) < 0)
                    break;
                cr.cpMin = ftw.chrgText.cpMin;
                cr.cpMax = ftw.chrgText.cpMax;
                if (cr.cpMax <= cr.cpMin || cr.cpMin < search)
                    break;
                if (range_has_link(cr.cpMin, cr.cpMax))
                {
                    pos = cr.cpMax;
                    found = 1;
                    break;
                }
                if (cr.cpMax <= search)
                    break;
                search = cr.cpMax;
            }
        }
        if (!found)
        {
            /* Phase 2: old controls expand fields to "display <url>".
               Match that exact pattern, link the display part only. */
            size_t dl;
            size_t ul;
            size_t un;

            dl = strlen(links[i].text);
            ul = strlen(links[i].url);
            un = ul;
            while (dl + 3 + un > 800 && un > 16)
                un--;
            if (dl > 0 && dl < (size_t)(sizeof(needle) - 4) &&
                ul > 0 && wlen > 1)
            {
                memcpy(needle, links[i].text, dl);
                needle[dl] = ' ';
                needle[dl + 1] = '<';
                memcpy(needle + dl + 2, links[i].url, un);
                needle[dl + 2 + un] = '\0';
                if (utf8_to_wide_buf(needle, wneedle, 1024) > 1)
                {
                    ftw.chrg.cpMin = pos;
                    ftw.chrg.cpMax = -1;
                    ftw.lpstrText = wneedle;
                    if (SendMessageW(g_hwndEdit, EM_FINDTEXTEXW,
                                     (WPARAM)FR_DOWN,
                                     (LPARAM)&ftw) >= 0)
                    {
                        cr.cpMin = ftw.chrgText.cpMin;
                        cr.cpMax = cr.cpMin + (wlen - 1);
                        SendMessage(g_hwndEdit, EM_EXSETSEL, 0,
                                    (LPARAM)&cr);
                        memset(&cf, 0, sizeof(cf));
                        cf.cbSize = sizeof(cf);
                        cf.dwMask = CFM_LINK;
                        cf.dwEffects = CFE_LINK;
                        SendMessage(g_hwndEdit, EM_SETCHARFORMAT,
                                    (WPARAM)SCF_SELECTION, (LPARAM)&cf);
                        pos = ftw.chrgText.cpMax;
                        found = 1;
                    }
                }
            }
        }
        i++;
    }
    SendMessage(g_hwndEdit, EM_EXSETSEL, 0, (LPARAM)&old);
    free(links);
}

/*
 * ----------------------------------------------------------------------
 * Menu
 * ----------------------------------------------------------------------
 */

/* Unescape our HYPERLINK field encoding (\'22 -> ", \\ -> \, \{ \}). */
static char *
extract_hyperlink_url(const char *rtf)
{
    const char *p;
    const char *q1;
    const char *q2;
    char *url;
    size_t n;
    size_t i;
    size_t j;

    if (rtf == NULL)
        return NULL;
    p = strstr(rtf, "HYPERLINK");
    if (p == NULL)
        return NULL;
    q1 = strchr(p, '"');
    if (q1 == NULL)
        return NULL;
    q2 = strchr(q1 + 1, '"');
    if (q2 == NULL || q2 == q1 + 1)
        return NULL;
    n = (size_t)(q2 - (q1 + 1));
    url = (char *)malloc(n + 1);
    if (url == NULL)
        return NULL;
    i = 0;
    j = 0;
    while (i < n)
    {
        if (q1[1 + i] == '\\' && i + 1 < n)
        {
            if (q1[1 + i + 1] == '\\' ||
                q1[1 + i + 1] == '{' ||
                q1[1 + i + 1] == '}')
            {
                url[j++] = q1[1 + i + 1];
                i += 2;
            }
            else if (q1[1 + i + 1] == '\'' && i + 3 < n &&
                     rtf_is_hex(q1[1 + i + 2]) &&
                     rtf_is_hex(q1[1 + i + 3]))
            {
                url[j++] = (char)(rtf_hex_val(q1[1 + i + 2]) * 16 +
                                  rtf_hex_val(q1[1 + i + 3]));
                i += 4;
            }
            else
            {
                url[j++] = q1[1 + i];
                i++;
            }
        }
        else
        {
            url[j++] = q1[1 + i];
            i++;
        }
    }
    url[j] = '\0';
    return url;
}

static int
looks_like_web_url(const char *s)
{
    while (*s == ' ' || *s == '\t' ||
           *s == '\r' || *s == '\n')
        s++;
    if (strncmp(s, "http://", 7) == 0 ||
        strncmp(s, "https://", 8) == 0 ||
        strncmp(s, "ftp://", 6) == 0 ||
        strncmp(s, "mailto:", 7) == 0 ||
        strncmp(s, "www.", 4) == 0)
        return 1;
    return 0;
}

/* First URL-like token in s (for "display <url>" expansions and
   source-mode "[text](url)" display clicks). Stops at whitespace or
   <>" chars; a trailing ")" / "]" is stripped only when unbalanced
   (keeps Wikipedia-style balanced parens intact). */
static int
find_url_in_text(const char *s, char *out, int outsz)
{
    const char *p;
    const char *e;
    size_t n;

    if (s == NULL || out == NULL || outsz <= 1)
        return 0;
    p = NULL;
    if ((e = strstr(s, "https://")) != NULL) p = e;
    if ((e = strstr(s, "http://")) != NULL &&
        (p == NULL || e < p)) p = e;
    if ((e = strstr(s, "ftp://")) != NULL &&
        (p == NULL || e < p)) p = e;
    if ((e = strstr(s, "mailto:")) != NULL &&
        (p == NULL || e < p)) p = e;
    if ((e = strstr(s, "www.")) != NULL &&
        (p == NULL || e < p)) p = e;
    if (p == NULL)
        return 0;
    e = p;
    while (*e != '\0' && *e != ' ' && *e != '\t' &&
           *e != '\r' && *e != '\n' && *e != '<' &&
           *e != '>' && *e != '"')
        e++;
    while (e > p && (*(e - 1) == '.' || *(e - 1) == ',' ||
                     *(e - 1) == ';' || *(e - 1) == ':' ||
                     *(e - 1) == '!' || *(e - 1) == '?' ||
                     *(e - 1) == '\'' || *(e - 1) == '"'))
        e--;
    /* balance-aware trailing paren/bracket strip */
    for (;;)
    {
        int no;
        int nc;
        const char *k;

        if (e <= p || (*(e - 1) != ')' && *(e - 1) != ']'))
            break;
        no = 0;
        nc = 0;
        k = p;
        while (k < e)
        {
            if (*k == '(' || *k == '[')
                no++;
            else if (*k == ')' || *k == ']')
                nc++;
            k++;
        }
        if (nc <= no)
            break;
        e--;
    }
    n = (size_t)(e - p);
    if (n == 0 || n > (size_t)(outsz - 1))
        return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

/* Visible text of the clicked paragraph (line), always UTF-8.
   Caller frees. */
static char *
get_line_text(LONG chpos)
{
    LONG ln;
    LONG ls;
    LONG llen;
    LONG cap;
    TCHAR *buf;
    WORD n16;
#ifdef UNICODE
    char *utf8;
#endif

    ln = (LONG)SendMessage(g_hwndEdit, EM_EXLINEFROMCHAR, 0,
                           (LPARAM)chpos);
    ls = (LONG)SendMessage(g_hwndEdit, EM_LINEINDEX, (WPARAM)ln, 0);
    if (ls < 0)
        return NULL;
    llen = (LONG)SendMessage(g_hwndEdit, EM_LINELENGTH, (WPARAM)ls, 0);
    if (llen < 0)
        return NULL;
    if (llen > 2048)
        llen = 2048;
    cap = llen + 1;
    buf = (TCHAR *)malloc(((size_t)cap + 1) * sizeof(TCHAR));
    if (buf == NULL)
        return NULL;
    n16 = (WORD)cap;
    memcpy(buf, &n16, sizeof(n16));
    llen = (LONG)SendMessage(g_hwndEdit, EM_GETLINE, (WPARAM)ln,
                             (LPARAM)buf);
    if (llen < 0)
        llen = 0;
    if (llen > cap)
        llen = cap;
    buf[llen] = _T('\0');
#ifdef UNICODE
    utf8 = wide_to_utf8(buf, (int)llen);
    free(buf);
    return utf8;
#else
    return buf;
#endif
}

/* Visible text of the current selection (no hidden field codes,
   dynamically sized; used for auto-detected URLs). */
static char *
get_selection_text(void)
{
    MemOut m;
    EDITSTREAM es;

    memset(&m, 0, sizeof(m));
    memset(&es, 0, sizeof(es));
    es.dwCookie = (DWORD_PTR)&m;
    es.pfnCallback = StreamOutMemCallback;
    SendMessage(g_hwndEdit,
                EM_STREAMOUT,
                (WPARAM)(SFF_SELECTION | SF_TEXT_EX),
                (LPARAM)&es);
    if (es.dwError != 0 || m.failed || m.buf == NULL)
    {
        if (m.buf != NULL)
            free(m.buf);
        return NULL;
    }
#ifdef UNICODE
    {
        char *utf8;

        utf8 = wide_to_utf8((const WCHAR *)m.buf,
                            (int)(m.len / sizeof(WCHAR)));
        free(m.buf);
        return utf8;
    }
#else
    return m.buf;
#endif
}

/* RTF of the current selection (carries HYPERLINK "url" for fields). */
static char *
get_selection_rtf(void)
{
    MemOut m;
    EDITSTREAM es;
    char *res;

    memset(&m, 0, sizeof(m));
    memset(&es, 0, sizeof(es));
    es.dwCookie = (DWORD_PTR)&m;
    es.pfnCallback = StreamOutMemCallback;
    SendMessage(g_hwndEdit,
                EM_STREAMOUT,
                (WPARAM)(SFF_SELECTION | SF_RTF),
                (LPARAM)&es);
    if (es.dwError != 0 || m.failed || m.buf == NULL)
    {
        if (m.buf != NULL)
            free(m.buf);
        return NULL;
    }
    res = m.buf;
    return res;
}

static void
OpenLinkAtRange(CHARRANGE *cr)
{
    CHARRANGE old;
    char *selrtf;
    char *seltext;
    char *url;
    char *line;
    char urlbuf[2080];
    char *p;

    if (g_hwndEdit == NULL || cr == NULL)
        return;
    url = NULL;
    SendMessage(g_hwndEdit, EM_EXGETSEL, 0, (LPARAM)&old);
    SendMessage(g_hwndEdit, EM_EXSETSEL, 0, (LPARAM)cr);
    selrtf = get_selection_rtf();
    if (selrtf != NULL)
    {
        url = extract_hyperlink_url(selrtf);
        free(selrtf);
    }
    if (url == NULL)
    {
        seltext = get_selection_text();
        SendMessage(g_hwndEdit, EM_EXSETSEL, 0, (LPARAM)&old);
        if (seltext != NULL)
        {
            if (looks_like_web_url(seltext))
            {
                p = seltext;
                while (*p == ' ' || *p == '\t' ||
                       *p == '\r' || *p == '\n')
                    p++;
                if (strncmp(p, "www.", 4) == 0)
                {
                    url = (char *)malloc(strlen(p) + 8);
                    if (url != NULL)
                        sprintf(url, "http://%s", p);
                }
                else
                {
                    url = (char *)malloc(strlen(p) + 1);
                    if (url != NULL)
                        strcpy(url, p);
                }
            }
            free(seltext);
        }
    }
    else
    {
        SendMessage(g_hwndEdit, EM_EXSETSEL, 0, (LPARAM)&old);
    }
    if (url == NULL)
    {
        /* Old controls expand fields to "display <url>" with no link
           effect on the display part; source-mode "[text](url)" display
           clicks land here too. Search the clicked line. */
        line = get_line_text(cr->cpMin);
        if (line != NULL)
        {
            if (find_url_in_text(line, urlbuf, sizeof(urlbuf)))
            {
                if (strncmp(urlbuf, "www.", 4) == 0)
                {
                    url = (char *)malloc(strlen(urlbuf) + 8);
                    if (url != NULL)
                        sprintf(url, "http://%s", urlbuf);
                }
                else
                {
                    url = (char *)malloc(strlen(urlbuf) + 1);
                    if (url != NULL)
                        strcpy(url, urlbuf);
                }
            }
            free(line);
        }
    }
    if (url == NULL)
        return;
    p = url + strlen(url);
    while (p > url && (*(p - 1) == ' ' || *(p - 1) == '\t' ||
                       *(p - 1) == '\r' || *(p - 1) == '\n'))
        *(--p) = '\0';
    if (*url == '\0')
    {
        free(url);
        return;
    }
#ifdef UNICODE
    {
        WCHAR *wurl;

        wurl = utf8_to_wide(url);
        if (wurl == NULL)
        {
            free(url);
            return;
        }
        if ((INT_PTR)ShellExecute(NULL, _T("open"), wurl,
                                  NULL, NULL, SW_SHOWNORMAL) <= 32)
        {
            MessageBox(g_hwndMain,
                       _T("Unable to open link."),
                       WND_TITLE,
                       MB_OK | MB_ICONERROR);
        }
        free(wurl);
    }
#else
    if ((INT_PTR)ShellExecuteA(NULL, "open", url,
                               NULL, NULL, SW_SHOWNORMAL) <= 32)
    {
        MessageBox(g_hwndMain,
                   _T("Unable to open link."),
                   WND_TITLE,
                   MB_OK | MB_ICONERROR);
    }
#endif
    free(url);
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

    AppendMenu(fileMenu, MF_STRING, IDM_OPEN,   _T("&Open..."));
    AppendMenu(fileMenu, MF_STRING, IDM_SAVE,   _T("&Save"));
    AppendMenu(fileMenu, MF_STRING, IDM_SAVEAS, _T("Save &As..."));
    AppendMenu(fileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(fileMenu, MF_STRING, IDM_EXIT,   _T("E&xit"));

    AppendMenu(menu, MF_POPUP,
               (UINT_PTR)fileMenu, _T("&File"));

    editMenu = CreatePopupMenu();

    AppendMenu(editMenu, MF_STRING, IDM_UNDO,      _T("&Undo"));
    AppendMenu(editMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(editMenu, MF_STRING, IDM_CUT,       _T("Cu&t"));
    AppendMenu(editMenu, MF_STRING, IDM_COPY,      _T("&Copy"));
    AppendMenu(editMenu, MF_STRING, IDM_PASTE,     _T("&Paste"));
    AppendMenu(editMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(editMenu, MF_STRING, IDM_SELECTALL, _T("Select &All"));
    AppendMenu(editMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(editMenu, MF_STRING | MF_UNCHECKED,
               IDM_SHOW_SOURCE, _T("Show &Markdown Source"));

    AppendMenu(menu, MF_POPUP,
               (UINT_PTR)editMenu, _T("&Edit"));

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
            DWORD evmask;

            g_hwndEdit = CreateWindowEx(
                WS_EX_CLIENTEDGE,
                g_editClass != NULL ? g_editClass : 
#ifdef UNICODE
                RICHEDIT_CLASS
#else
                RICHEDIT_CLASSA
#endif
                ,
                _T(""),
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

            /*
             * RichEdit 1.0 ignores EM_EXLIMITTEXT: also send the
             * original EM_LIMITTEXT (byte count on 1.0).
             * Harmless on newer controls.
             */
            SendMessage(g_hwndEdit,
                        EM_LIMITTEXT,
                        (WPARAM)64000,
                        0);

            /*
             * EN_LINK notifications make HYPERLINK fields (markdown
             * links) and auto-detected URLs clickable (see WM_NOTIFY).
             * Both need RichEdit 2.0+, so skip them on 1.0.
             */
            if (!g_isRE10)
            {
                evmask = (DWORD)SendMessage(g_hwndEdit,
                                            EM_GETEVENTMASK, 0, 0);
                SendMessage(g_hwndEdit,
                            EM_SETEVENTMASK, 0,
                            (LPARAM)(evmask | ENM_LINK));
                SendMessage(g_hwndEdit,
                            EM_AUTOURLDETECT,
                            (WPARAM)TRUE, 0);
            }

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

        case IDM_SHOW_SOURCE:
            SetSourceMode(!g_showSource);
            return 0;
        }

        break;

    case WM_NOTIFY:
        {
            LPNMHDR nm;

            nm = (LPNMHDR)lParam;
            if (nm != NULL && nm->hwndFrom == g_hwndEdit &&
                nm->code == EN_LINK)
            {
                ENLINK *lk;

                lk = (ENLINK *)lParam;
                if (lk->msg == WM_LBUTTONUP)
                {
                    OpenLinkAtRange(&lk->chrg);
                    return 0;
                }
                else if (lk->msg == WM_SETCURSOR)
                {
                    SetCursor(LoadCursor(NULL, IDC_HAND));
                    return 0;
                }
            }
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

/* UNICODE selects the entry point: wWinMain for the Unicode
   flavor, WinMain for ANSI. LPTSTR adapts with it. */
#ifdef UNICODE
#define APP_ENTRY wWinMain
#else
#define APP_ENTRY WinMain
#endif

int WINAPI
APP_ENTRY(HINSTANCE hInstance,
        HINSTANCE hPrevInstance,
        LPTSTR lpCmdLine,
        int nCmdShow)
{
    WNDCLASS wc;
    HWND hwnd;
    MSG msg;
    HMENU menu;
    UINT oldErrMode;
    TCHAR cmdFile[MAX_PATH];

    (void)hPrevInstance;

    g_hInst = hInstance;
    g_hwndMain = NULL;
    g_hwndEdit = NULL;
    g_hRichEdit = NULL;
    g_editClass = NULL;
    g_filename[0] = _T('\0');
    g_showSource = 0;
    g_isRE10 = 0;

    /* Disable Error Dialog when DLL is not found */
    oldErrMode = SetErrorMode(SEM_NOOPENFILEERRORBOX);
    /*
     * Prefer Msftedit (RichEdit 4.1+, best table support) and fall back
     * to RICHED20 (2.0 on old systems, 3.0+ on XP and later, tables OK).
     * RICHED32 (1.0) is a degraded last resort (sets g_isRE10):
     * no links, no tables, no Unicode, 64K text cap.
     *
     * Msftedit only provides the Unicode RICHEDIT50W class, but byte
     * based SF_RTF streaming still works from this ANSI app.
     */
    g_hRichEdit = LoadLibrary(_T("Msftedit.dll"));

    if (g_hRichEdit != NULL)
    {
        /* NOTE: use _T literal, not MSFTEDIT_CLASS macro which is
           wide (L"RICHEDIT50W") in newer SDKs. */
        g_editClass = _T("RICHEDIT50W");
    }
    else
    {
        g_hRichEdit = LoadLibrary(_T("RICHED20.DLL"));
#ifdef UNICODE
        g_editClass = RICHEDIT_CLASS;
#else
        g_editClass = RICHEDIT_CLASSA;
#endif
#ifndef UNICODE
        /* RichEdit 1.0 is ANSI-only: no Unicode build fallback. */
        if (g_hRichEdit == NULL) {
            g_hRichEdit = LoadLibrary(_T("RICHED32.DLL"));
            g_editClass = _T("RICHEDIT");
            g_isRE10 = 1;
        }
#endif
    }

    /* Restore old Error Mode */
    SetErrorMode(oldErrMode);
    if (g_hRichEdit == NULL)
    {
        MessageBox(NULL,
                   _T("Unable to load Msftedit.dll or RICHED20.DLL."),
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

    g_hwndMain = hwnd;
    UpdateTitle();
    UpdateSourceCheck();

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    if (GetCmdLineFile(lpCmdLine, cmdFile, MAX_PATH))
    {
        LoadAny(g_hwndEdit, cmdFile);
    }

    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    FreeLibrary(g_hRichEdit);

    return (int)msg.wParam;
}
