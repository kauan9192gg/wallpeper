#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// ============================================================
//                      GDI+ (carregar jpg)
// ============================================================
typedef struct { UINT32 v; void *cb; BOOL bg; BOOL ext; } GpStart;
static HMODULE   gdipLib;
static ULONG_PTR gdipToken;
static int  (WINAPI *pGpStart)(ULONG_PTR*, GpStart*, void*);
static void (WINAPI *pGpStop)(ULONG_PTR);
static int  (WINAPI *pGpFromFile)(const WCHAR*, void**);
static int  (WINAPI *pGpHBmp)(void*, HBITMAP*, DWORD);
static int  (WINAPI *pGpDispose)(void*);

static int gdipInit(void) {
    gdipLib = LoadLibraryA("gdiplus.dll");
    if (!gdipLib) return 0;
    pGpStart   = (void*)GetProcAddress(gdipLib, "GdiplusStartup");
    pGpStop    = (void*)GetProcAddress(gdipLib, "GdiplusShutdown");
    pGpFromFile= (void*)GetProcAddress(gdipLib, "GdipCreateBitmapFromFile");
    pGpHBmp    = (void*)GetProcAddress(gdipLib, "GdipCreateHBITMAPFromBitmap");
    pGpDispose = (void*)GetProcAddress(gdipLib, "GdipDisposeImage");
    if (!pGpStart || !pGpFromFile) return 0;
    GpStart s = {1, NULL, FALSE, FALSE};
    return pGpStart(&gdipToken, &s, NULL) == 0;
}
static void gdipEnd(void) { if (pGpStop) pGpStop(gdipToken); if (gdipLib) FreeLibrary(gdipLib); }
static HBITMAP loadJpg(const char *p) {
    if (!pGpFromFile) return NULL;
    WCHAR wp[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, p, -1, wp, MAX_PATH);
    void *b = NULL;
    if (pGpFromFile(wp, &b) != 0) return NULL;
    HBITMAP hb = NULL;
    pGpHBmp(b, &hb, 0);
    pGpDispose(b);
    return hb;
}

// ============================================================
//                      paleta / medidas
// ============================================================
#define C_BG_TOP     RGB( 18, 18, 30)
#define C_BG_BOT     RGB( 28, 20, 48)
#define C_PANEL      RGB( 24, 24, 40)
#define C_CARD       RGB( 30, 30, 48)
#define C_CARD_HOV   RGB( 40, 40, 62)
#define C_BORDER     RGB( 55, 55, 80)
#define C_TEXT       RGB(235,235,245)
#define C_SUB        RGB(140,140,170)
#define C_SUB_DIM    RGB(100,100,130)
#define C_CYAN       RGB(  0,229,255)
#define C_PURPLE     RGB(139, 92,246)
#define C_RED        RGB(239, 68, 68)
#define C_TRACK      RGB( 55, 55, 80)

#define CFG_W 960
#define CFG_H 620
#define HEADER_H 56
#define TABS_H 40
#define PAD 18
#define GAP 10
#define COLS 3
#define THUMB_W 150
#define THUMB_H 96
#define CARD_H (THUMB_H + 8)
#define PREVIEW_W 300
#define RIGHT_X (CFG_W - PREVIEW_W - PAD)

// ============================================================
//                      estado
// ============================================================
#define WM_TRAY (WM_USER + 1)
#define WM_THUMB_DONE (WM_USER + 2)
#define WM_SCAN_DONE  (WM_USER + 3)

typedef struct {
    char    path[MAX_PATH];
    HBITMAP orig;
    HBITMAP thumb;
    HBITMAP big;
    int     loading;
    int     fav;
    int     cat;
} Wall;

static HWND  hw, hcfg;
static int   sw, sh, paused;
static char  file[MAX_PATH], cfg[MAX_PATH], dir[MAX_PATH];
static PROCESS_INFORMATION pi;
static HANDLE job;
static NOTIFYICONDATA nid;

static HFONT fLogo, fBody, fSmall, fBtn, fSec;
static HBRUSH bBg;

static Wall  *walls = NULL;
static int    wallCount = 0, wallCap = 0;
static int    sel = -1;
static int    hoverCard = -1;
static int    hoverBtn = -1;
static int    dragging = 0;
static float  speed = 1.0f;
static HANDLE pipeH = INVALID_HANDLE_VALUE;
static int    scrollY = 0, maxScroll = 0;

static int    tab = 0;

// config
static int    cfgPauseFS  = 1;
static int    cfgPlaylist = 0;
static char   cfgFolder[MAX_PATH] = {0};
static int    cfgStartWin = 0;
static DWORD  playlistLast = 0;

// filtros
static int    filterFav = 0;
static int    filterCat = -1;

// glow
static HBITMAP glowBmp = NULL;
static int     glowFor = -1;

// loading overlay
static int    loadingCount = 0;
static int    scanning = 0;

// ============================================================
//                      util
// ============================================================
static unsigned int hashStr(const char *s) {
    unsigned int h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}
static int inR(RECT *r, int x, int y) {
    return x >= r->left && x <= r->right && y >= r->top && y <= r->bottom;
}

// ============================================================
//                      pipe mpv
// ============================================================
static void pipeOpen(void) {
    if (pipeH != INVALID_HANDLE_VALUE) return;
    pipeH = CreateFile("\\\\.\\pipe\\litewall", GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, 0, NULL);
}
static void pipeSend(const char *s) {
    if (pipeH == INVALID_HANDLE_VALUE) pipeOpen();
    if (pipeH == INVALID_HANDLE_VALUE) return;
    DWORD n;
    if (!WriteFile(pipeH, s, (DWORD)strlen(s), &n, NULL)) {
        CloseHandle(pipeH); pipeH = INVALID_HANDLE_VALUE;
    }
}
static void mpvPause(int p) {
    pipeSend(p ? "{\"command\":[\"set_property\",\"pause\",true]}\n"
               : "{\"command\":[\"set_property\",\"pause\",false]}\n");
}
static void mpvSpeed(float v) {
    char b[80];
    int n = snprintf(b, sizeof b, "{\"command\":[\"set_property\",\"speed\",%.3f]}\n", v);
    if (pipeH == INVALID_HANDLE_VALUE) pipeOpen();
    if (pipeH == INVALID_HANDLE_VALUE) return;
    DWORD w;
    if (!WriteFile(pipeH, b, (DWORD)n, &w, NULL)) {
        CloseHandle(pipeH); pipeH = INVALID_HANDLE_VALUE;
    }
}

// ============================================================
//                      worker window / busy
// ============================================================
static BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
    if (FindWindowEx(h, NULL, "SHELLDLL_DefView", NULL))
        *(HWND *)lp = FindWindowEx(NULL, h, "WorkerW", NULL);
    return TRUE;
}
static HWND getWorker(void) {
    HWND prog = FindWindow("Progman", NULL), w = NULL;
    SendMessageTimeout(prog, 0x052C, 0, 0, SMTO_NORMAL, 1000, NULL);
    EnumWindows(enumProc, (LPARAM)&w);
    if (!w) w = FindWindowEx(prog, NULL, "WorkerW", NULL);
    return w ? w : prog;
}
static int busy(void) {
    if (!cfgPauseFS) return 0;
    HWND f = GetForegroundWindow();
    if (!f || f == hw || f == hcfg || IsIconic(f)) return 0;
    char cls[32];
    GetClassName(f, cls, sizeof cls);
    if (!strcmp(cls,"Progman")||!strcmp(cls,"WorkerW")||!strcmp(cls,"Shell_TrayWnd")) return 0;
    if (IsZoomed(f)) return 1;
    RECT r; GetWindowRect(f, &r);
    return r.left<=0 && r.top<=0 && r.right>=sw && r.bottom>=sh;
}

// ============================================================
//                      scale bitmap
// ============================================================
static HBITMAP scaleBitmap(HBITMAP src, int dstW, int dstH) {
    if (!src) return NULL;
    BITMAP bm;
    GetObject(src, sizeof bm, &bm);
    HDC screen = GetDC(NULL);
    HDC sdc = CreateCompatibleDC(screen);
    HDC ddc = CreateCompatibleDC(screen);
    HBITMAP dst = CreateCompatibleBitmap(screen, dstW, dstH);
    HGDIOBJ os = SelectObject(sdc, src);
    HGDIOBJ od = SelectObject(ddc, dst);
    SetStretchBltMode(ddc, HALFTONE);
    SetBrushOrgEx(ddc, 0, 0, NULL);
    float sa = (float)bm.bmWidth / bm.bmHeight;
    float da = (float)dstW / dstH;
    int sw2, sh2, sx, sy;
    if (sa > da) { sh2 = bm.bmHeight; sw2 = (int)(bm.bmHeight * da); sx = (bm.bmWidth - sw2)/2; sy = 0; }
    else         { sw2 = bm.bmWidth; sh2 = (int)(bm.bmWidth / da); sx = 0; sy = (bm.bmHeight - sh2)/2; }
    StretchBlt(ddc, 0, 0, dstW, dstH, sdc, sx, sy, sw2, sh2, SRCCOPY);
    SelectObject(sdc, os); SelectObject(ddc, od);
    DeleteDC(sdc); DeleteDC(ddc);
    ReleaseDC(NULL, screen);
    return dst;
}

// ============================================================
//                      thumb thread
// ============================================================
typedef struct { char video[MAX_PATH]; char out[MAX_PATH]; HWND notify; } ThumbJob;

static DWORD WINAPI thumbThread(LPVOID p) {
    ThumbJob *j = (ThumbJob*)p;

    char tdir[MAX_PATH], tmp[MAX_PATH];
    snprintf(tdir, sizeof tdir, "%s\\thumbs", dir);
    CreateDirectory(tdir, NULL);
    snprintf(tmp, sizeof tmp, "%s\\thumbs\\t%08x", dir, hashStr(j->video));
    CreateDirectory(tmp, NULL);

    char cmd[2048];
    snprintf(cmd, sizeof cmd,
        "\"%s\\mpv.exe\" --no-config --vo=image --vo-image-format=jpg "
        "--vo-image-jpeg-quality=75 --vo-image-outdir=\"%s\" "
        "--frames=1 --start=1 --no-audio --really-quiet \"%s\"",
        dir, tmp, j->video);

    STARTUPINFO si = {0}; si.cb = sizeof si;
    PROCESS_INFORMATION pr = {0};
    if (CreateProcess(NULL, cmd, NULL, NULL, FALSE,
                      CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                      NULL, NULL, &si, &pr)) {
        WaitForSingleObject(pr.hProcess, 15000);
        CloseHandle(pr.hProcess); CloseHandle(pr.hThread);
    }

    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*.jpg", tmp);
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        char src[MAX_PATH];
        snprintf(src, sizeof src, "%s\\%s", tmp, fd.cFileName);
        MoveFileEx(src, j->out, MOVEFILE_REPLACE_EXISTING);
        FindClose(f);
    }
    RemoveDirectory(tmp);

    PostMessage(j->notify, WM_THUMB_DONE, 0, (LPARAM)j);
    return 0;
}
static void thumbStart(const char *video, HWND notify) {
    unsigned int hh = hashStr(video);
    char tdir[MAX_PATH], out[MAX_PATH];
    snprintf(tdir, sizeof tdir, "%s\\thumbs", dir);
    CreateDirectory(tdir, NULL);
    snprintf(out, sizeof out, "%s\\%08x.jpg", tdir, hh);

    ThumbJob *j = malloc(sizeof *j);
    strncpy(j->video, video, MAX_PATH-1); j->video[MAX_PATH-1]=0;
    strncpy(j->out, out, MAX_PATH-1);     j->out[MAX_PATH-1]=0;
    j->notify = notify;

    if (GetFileAttributes(out) != INVALID_FILE_ATTRIBUTES) {
        // thumb ja existe em cache: notifica imediatamente
        PostMessage(notify, WM_THUMB_DONE, 0, (LPARAM)j);
        return;
    }

    HANDLE t = CreateThread(NULL, 0, thumbThread, j, 0, NULL);
    if (t) CloseHandle(t);
    else { PostMessage(notify, WM_THUMB_DONE, 0, (LPARAM)j); }
}

static void buildThumbs(Wall *w) {
    if (!w->orig) {
        unsigned int hh = hashStr(w->path);
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\thumbs\\%08x.jpg", dir, hh);
        w->orig = loadJpg(p);
    }
    if (!w->orig) return;
    if (!w->thumb) w->thumb = scaleBitmap(w->orig, THUMB_W - 8, THUMB_H);
    if (!w->big)   w->big   = scaleBitmap(w->orig, 240, 200);
}

// ============================================================
//                      scan de pasta (thread)
// ============================================================
typedef struct { char folder[MAX_PATH]; HWND notify; } ScanJob;

static int isVideoExt(const char *p) {
    const char *e = strrchr(p, '.');
    if (!e) return 0;
    e++;
    return !_stricmp(e,"mp4") || !_stricmp(e,"mkv") || !_stricmp(e,"webm") ||
           !_stricmp(e,"avi") || !_stricmp(e,"mov") || !_stricmp(e,"gif") ||
           !_stricmp(e,"wmv") || !_stricmp(e,"flv") || !_stricmp(e,"m4v");
}

static DWORD WINAPI scanThread(LPVOID p) {
    ScanJob *j = (ScanJob*)p;
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", j->folder);
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) {
        PostMessage(j->notify, WM_SCAN_DONE, 0, (LPARAM)j);
        return 0;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!isVideoExt(fd.cFileName)) continue;
        char *cp = malloc(MAX_PATH);
        snprintf(cp, MAX_PATH, "%s\\%s", j->folder, fd.cFileName);
        cp[MAX_PATH-1] = 0;
        PostMessage(j->notify, WM_SCAN_DONE, 1, (LPARAM)cp);
    } while (FindNextFile(f, &fd));
    FindClose(f);
    PostMessage(j->notify, WM_SCAN_DONE, 0, (LPARAM)j);
    return 0;
}

// ============================================================
//                      walls list
// ============================================================
static int wallsHas(const char *p) {
    for (int i = 0; i < wallCount; i++)
        if (!_stricmp(walls[i].path, p)) return 1;
    return 0;
}
static void wallsAdd(const char *p) {
    if (wallsHas(p)) return;
    if (wallCount >= wallCap) {
        wallCap = wallCap ? wallCap * 2 : 8;
        walls = realloc(walls, wallCap * sizeof(Wall));
    }
    memset(&walls[wallCount], 0, sizeof(Wall));
    strncpy(walls[wallCount].path, p, MAX_PATH-1);
    walls[wallCount].path[MAX_PATH-1] = 0;
    walls[wallCount].loading = 1;
    walls[wallCount].cat = 0;
    wallCount++;
    loadingCount++;
    thumbStart(p, hcfg);
}
static void wallsRemove(int idx) {
    if (idx < 0 || idx >= wallCount) return;
    Wall *w = &walls[idx];
    if (w->orig)  DeleteObject(w->orig);
    if (w->thumb) DeleteObject(w->thumb);
    if (w->big)   DeleteObject(w->big);
    if (w->loading && loadingCount > 0) loadingCount--;
    for (int i = idx; i < wallCount - 1; i++) walls[i] = walls[i+1];
    wallCount--;
    if (sel >= wallCount) sel = wallCount - 1;
}
static void wallsFree(void) {
    for (int i = 0; i < wallCount; i++) {
        if (walls[i].orig)  DeleteObject(walls[i].orig);
        if (walls[i].thumb) DeleteObject(walls[i].thumb);
        if (walls[i].big)   DeleteObject(walls[i].big);
    }
    free(walls); walls = NULL; wallCount = wallCap = 0;
    loadingCount = 0;
}

// filtros
static int passFilter(int i) {
    if (filterFav && !walls[i].fav) return 0;
    if (filterCat >= 0 && walls[i].cat != filterCat) return 0;
    return 1;
}
static int visibleCount(void) {
    int n = 0;
    for (int i = 0; i < wallCount; i++) if (passFilter(i)) n++;
    return n;
}
static int visibleAt(int v) {
    int n = 0;
    for (int i = 0; i < wallCount; i++) {
        if (!passFilter(i)) continue;
        if (n == v) return i;
        n++;
    }
    return -1;
}

// ============================================================
//                      launch mpv
// ============================================================
static void launch(void) {
    if (pi.hProcess) {
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        pi.hProcess = NULL;
    }
    if (pipeH != INVALID_HANDLE_VALUE) { CloseHandle(pipeH); pipeH = INVALID_HANDLE_VALUE; }

    if (!file[0]) return;

    char cmd[3072];
    snprintf(cmd, sizeof cmd,
        "\"%s\\mpv.exe\" --no-config --wid=%lu --profile=fast --hwdec=auto-safe "
        "--vo=gpu --gpu-context=d3d11 --vd-lavc-fast --vd-lavc-skiploopfilter=all "
        "--vd-lavc-skipframe=nonref --framedrop=vo --video-sync=audio "
        "--no-audio --loop-file=inf --panscan=1.0 --no-osc --osd-level=0 "
        "--no-input-default-bindings --no-input-cursor --cache=no "
        "--demuxer-max-bytes=4MiB --demuxer-max-back-bytes=512KiB --really-quiet "
        "--speed=%.3f --input-ipc-server=\\\\.\\pipe\\litewall \"%s\"",
        dir, (unsigned long)(ULONG_PTR)hw, speed, file);
    STARTUPINFO si = {0}; si.cb = sizeof si;
    if (CreateProcess(NULL, cmd, NULL, NULL, FALSE,
                      CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                      NULL, NULL, &si, &pi)) {
        AssignProcessToJobObject(job, pi.hProcess);
        paused = 0;
    } else {
        pi.hProcess = NULL;
    }
}

static int pick(void) {
    OPENFILENAME o = {0};
    o.lStructSize = sizeof o;
    o.lpstrFile = file;
    o.nMaxFile = MAX_PATH;
    o.lpstrFilter = "Videos e GIF\0*.mp4;*.mkv;*.webm;*.avi;*.mov;*.gif\0Todos\0*.*\0";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileName(&o);
}

// ============================================================
//                      cfg
// ============================================================
static void saveCfg(void) {
    FILE *f = fopen(cfg, "w");
    if (!f) return;
    fprintf(f, "%s\n%.3f\n%d\n%d\n%d\n%s\n",
            file, speed, cfgPauseFS, cfgPlaylist, cfgStartWin, cfgFolder);
    fclose(f);
}
static void loadCfg(void) {
    FILE *f = fopen(cfg, "r");
    if (!f) return;
    if (fgets(file, MAX_PATH, f)) file[strcspn(file, "\r\n")] = 0;
    if (fscanf(f, "%f", &speed) != 1) speed = 1.0f;
    int a=1,b=0,c=0;
    fscanf(f, "%d %d %d", &a, &b, &c);
    cfgPauseFS  = a;
    cfgPlaylist = b;
    cfgStartWin = c;
    int ch; while ((ch = fgetc(f)) != '\n' && ch != EOF);
    if (fgets(cfgFolder, MAX_PATH, f)) cfgFolder[strcspn(cfgFolder, "\r\n")] = 0;
    fclose(f);
}
static void setStartWithWindows(int on) {
    HKEY k;
    if (RegOpenKeyEx(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        char exe[MAX_PATH];
        GetModuleFileName(NULL, exe, MAX_PATH);
        char val[MAX_PATH + 4];
        snprintf(val, sizeof val, "\"%s\"", exe);
        RegSetValueEx(k, "LiteWall", 0, REG_SZ, (BYTE*)val, (DWORD)strlen(val)+1);
    } else {
        RegDeleteValue(k, "LiteWall");
    }
    RegCloseKey(k);
}

// ============================================================
//                      GDI helpers
// ============================================================
static void fillC(HDC dc, RECT *r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c); FillRect(dc, r, b); DeleteObject(b);
}
static void roundC(HDC dc, RECT *r, int rad, COLORREF fill) {
    HBRUSH b = CreateSolidBrush(fill); HPEN p = CreatePen(PS_SOLID,1,fill);
    HGDIOBJ ob = SelectObject(dc,b), op = SelectObject(dc,p);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, rad, rad);
    SelectObject(dc,ob); SelectObject(dc,op); DeleteObject(b); DeleteObject(p);
}
static void roundB(HDC dc, RECT *r, int rad, COLORREF fill, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fill); HPEN p = CreatePen(PS_SOLID,1,border);
    HGDIOBJ ob = SelectObject(dc,b), op = SelectObject(dc,p);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, rad, rad);
    SelectObject(dc,ob); SelectObject(dc,op); DeleteObject(b); DeleteObject(p);
}
static void roundGrad(HDC dc, RECT *r, int rad, COLORREF c1, COLORREF c2, BOOL horiz) {
    HRGN rgn = CreateRoundRectRgn(r->left, r->top, r->right+1, r->bottom+1, rad, rad);
    SelectClipRgn(dc, rgn);
    TRIVERTEX v[2] = {
        { r->left, r->top, GetRValue(c1)<<8, GetGValue(c1)<<8, GetBValue(c1)<<8, 0 },
        { r->right, r->bottom, GetRValue(c2)<<8, GetGValue(c2)<<8, GetBValue(c2)<<8, 0 }
    };
    GRADIENT_RECT gr = {0, 1};
    GradientFill(dc, v, 2, &gr, 1, horiz ? GRADIENT_FILL_RECT_H : GRADIENT_FILL_RECT_V);
    SelectClipRgn(dc, NULL);
    DeleteObject(rgn);
}
static void textC(HDC dc, const char *s, RECT *r, HFONT f, COLORREF c, UINT fmt) {
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawText(dc, s, -1, r, fmt);
    SelectObject(dc, of);
}

// ============================================================
//                      icones
// ============================================================
static void icBolt(HDC dc, int cx, int cy, int sz, COLORREF c1, COLORREF c2) {
    POINT top[4] = {
        {cx + sz/3, cy - sz}, {cx - sz/2, cy + sz/6},
        {cx - sz/10, cy + sz/6}, {cx + sz/10, cy - sz/6}
    };
    POINT bot[4] = {
        {cx - sz/3, cy + sz}, {cx + sz/2, cy - sz/6},
        {cx + sz/10, cy - sz/6}, {cx - sz/10, cy + sz/6}
    };
    HBRUSH b1 = CreateSolidBrush(c1), b2 = CreateSolidBrush(c2);
    HGDIOBJ ob = SelectObject(dc, b1);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, top, 4);
    SelectObject(dc, b2);
    Polygon(dc, bot, 4);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b1); DeleteObject(b2);
}
static void icCheck(HDC dc, int cx, int cy, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 2, c);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, cx-5, cy, NULL);
    LineTo(dc, cx-1, cy+4);
    LineTo(dc, cx+6, cy-4);
    SelectObject(dc, op); DeleteObject(p);
}
static void icStarFilled(HDC dc, int cx, int cy, int s, COLORREF c) {
    POINT pts[10];
    for (int i = 0; i < 10; i++) {
        double a = -3.14159/2 + i * 3.14159 / 5;
        double rr = (i % 2) ? s/2.5 : s;
        pts[i].x = cx + (int)(cos(a)*rr);
        pts[i].y = cy + (int)(sin(a)*rr);
    }
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, pts, 10);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b);
}
static void icStarOutline(HDC dc, int cx, int cy, int s, COLORREF c) {
    POINT pts[10];
    for (int i = 0; i < 10; i++) {
        double a = -3.14159/2 + i * 3.14159 / 5;
        double rr = (i % 2) ? s/2.5 : s;
        pts[i].x = cx + (int)(cos(a)*rr);
        pts[i].y = cy + (int)(sin(a)*rr);
    }
    HPEN p = CreatePen(PS_SOLID, 2, c);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Polygon(dc, pts, 10);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(p);
}
static void icTrash(HDC dc, int x, int y, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 2, c);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, x, y+4, x+14, y+18);
    MoveToEx(dc, x-2, y+4, NULL); LineTo(dc, x+16, y+4);
    MoveToEx(dc, x+4, y, NULL); LineTo(dc, x+10, y);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(p);
}
static void icPlus(HDC dc, int cx, int cy, int s, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 2, c);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, cx-s, cy, NULL); LineTo(dc, cx+s+1, cy);
    MoveToEx(dc, cx, cy-s, NULL); LineTo(dc, cx, cy+s+1);
    SelectObject(dc, op); DeleteObject(p);
}

// ============================================================
//                      layout
// ============================================================
static RECT gridArea(void) {
    RECT r = {PAD, HEADER_H + TABS_H + PAD, RIGHT_X - GAP, CFG_H - 80};
    return r;
}
static RECT previewPanel(void) {
    RECT r = {RIGHT_X, HEADER_H + TABS_H + PAD, CFG_W - PAD, CFG_H - 24};
    return r;
}
static RECT cardRect(int v) {
    RECT g = gridArea();
    int col = v % COLS, row = v / COLS;
    int x = g.left + col * (THUMB_W + GAP);
    int y = g.top + row * (CARD_H + GAP) - scrollY;
    RECT r = {x, y, x + THUMB_W, y + CARD_H};
    return r;
}
static RECT applyBtn(void) {
    RECT p = previewPanel();
    RECT r = {p.left + 12, p.top + 260, p.right - 12, p.top + 260 + 48};
    return r;
}
static RECT favBtn(void) {
    RECT a = applyBtn();
    RECT r = {a.left, a.bottom + 8, a.right, a.bottom + 8 + 40};
    return r;
}
static RECT delBtn(void) {
    RECT f = favBtn();
    RECT r = {f.left, f.bottom + 8, f.right, f.bottom + 8 + 40};
    return r;
}
static RECT sliderRect(void) {
    RECT r = {PAD + 110, CFG_H - 56, PAD + 110 + 200, CFG_H - 36};
    return r;
}
static RECT addBtn(void) {
    RECT r = {RIGHT_X - 130, HEADER_H + 6, RIGHT_X - 12, HEADER_H + 34};
    return r;
}
static RECT tabRect(int i) {
    int w = 130;
    RECT r = {PAD + i * (w + 4), HEADER_H + 4, PAD + i * (w + 4) + w, HEADER_H + TABS_H - 4};
    return r;
}

static int hitAdd(int x, int y)    { RECT r = addBtn();    return inR(&r, x, y); }
static int hitApply(int x, int y)  { RECT r = applyBtn();  return inR(&r, x, y); }
static int hitFav(int x, int y)    { RECT r = favBtn();    return inR(&r, x, y); }
static int hitDel(int x, int y)    { RECT r = delBtn();    return inR(&r, x, y); }
static int hitSlider(int x, int y) {
    RECT r = sliderRect();
    return inR(&r, x, y) || (y >= r.top-8 && y <= r.bottom+8 && x >= r.left-6 && x <= r.right+6);
}

// ============================================================
//                      glow
// ============================================================
static void buildGlow(void) {
    if (glowBmp) { DeleteObject(glowBmp); glowBmp = NULL; }
    if (sel < 0) return;
    int gw = THUMB_W + 20, gh = CARD_H + 20;
    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    glowBmp = CreateCompatibleBitmap(screen, gw, gh);
    HGDIOBJ ob = SelectObject(dc, glowBmp);
    for (int i = 8; i > 0; i--) {
        int t = 100 - i * 11;
        if (t < 18) t = 18;
        COLORREF c = RGB(
            GetRValue(C_CYAN) * t / 100,
            GetGValue(C_CYAN) * t / 100,
            GetBValue(C_CYAN) * t / 100);
        HBRUSH b = CreateSolidBrush(c);
        HPEN p = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ ob2 = SelectObject(dc, b), op2 = SelectObject(dc, p);
        RoundRect(dc, 10 - i, 10 - i, gw - 10 + i, gh - 10 + i, 10 + i, 10 + i);
        SelectObject(dc, ob2); SelectObject(dc, op2);
        DeleteObject(b); DeleteObject(p);
    }
    SelectObject(dc, ob);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
}

// ============================================================
//                      draw preview
// ============================================================
static void drawPreview(HDC dc) {
    RECT p = previewPanel();
    roundB(dc, &p, 8, C_PANEL, C_BORDER);
    RECT lbl = {p.left + 12, p.top + 8, p.right, p.top + 28};
    textC(dc, "Preview", &lbl, fSmall, C_SUB, DT_LEFT | DT_SINGLELINE);

    RECT pv = {p.left + 12, p.top + 32, p.right - 12, p.top + 248};
    roundB(dc, &pv, 6, C_CARD, C_BORDER);

    int real = (sel >= 0 && sel < wallCount) ? sel : -1;
    if (real >= 0) {
        Wall *w = &walls[real];
        buildThumbs(w);
        if (w->loading) {
            RECT e = pv;
            textC(dc, "Carregando preview...", &e, fSmall, C_SUB_DIM,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (w->big) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ ob = SelectObject(mem, w->big);
            BitBlt(dc, pv.left + 30, pv.top + 24, 240, 200, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob); DeleteDC(mem);
        } else {
            RECT e = pv;
            textC(dc, "Sem preview", &e, fSmall, C_SUB_DIM,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        const char *base = strrchr(w->path, '\\');
        base = base ? base + 1 : w->path;
        RECT nm = {p.left + 12, p.top + 220, p.right - 12, p.top + 244};
        textC(dc, base, &nm, fSmall, C_SUB, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        RECT e = pv;
        textC(dc, "Selecione um wallpaper", &e, fSmall, C_SUB_DIM,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    int can = (real >= 0 && !walls[real].loading);

    // Apply
    RECT ab = applyBtn();
    if (can) {
        COLORREF c1 = (hoverBtn == 0) ? RGB(60,240,255) : C_CYAN;
        COLORREF c2 = (hoverBtn == 0) ? RGB(170,120,255) : C_PURPLE;
        roundGrad(dc, &ab, 6, c1, c2, TRUE);
        icCheck(dc, ab.left + 26, (ab.top+ab.bottom)/2, RGB(20,20,30));
        RECT t = {ab.left + 48, ab.top, ab.right, ab.bottom};
        textC(dc, "Apply", &t, fBody, RGB(20,20,30),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        roundB(dc, &ab, 6, C_CARD, C_BORDER);
        RECT t = ab;
        textC(dc, "Apply", &t, fBody, C_SUB_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // Favorite
    RECT fb = favBtn();
    roundB(dc, &fb, 6, (hoverBtn == 1) ? C_CARD_HOV : C_CARD,
           (hoverBtn == 1) ? C_PURPLE : C_BORDER);
    if (real >= 0 && walls[real].fav)
        icStarFilled(dc, fb.left + 26, (fb.top+fb.bottom)/2, 9, C_PURPLE);
    else
        icStarOutline(dc, fb.left + 26, (fb.top+fb.bottom)/2, 9, C_PURPLE);
    RECT ft = {fb.left + 48, fb.top, fb.right, fb.bottom};
    textC(dc, "Favorite", &ft, fBody, C_TEXT,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Delete
    RECT db = delBtn();
    roundB(dc, &db, 6, (hoverBtn == 2) ? C_CARD_HOV : C_CARD,
           (hoverBtn == 2) ? C_RED : C_BORDER);
    icTrash(dc, db.left + 20, (db.top+db.bottom)/2 - 9, C_RED);
    RECT dt = {db.left + 48, db.top, db.right, db.bottom};
    textC(dc, "Delete", &dt, fBody, C_TEXT,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// ============================================================
//                      draw card
// ============================================================
static void drawCard(HDC dc, int vi) {
    int i = visibleAt(vi);
    if (i < 0) return;
    RECT r = cardRect(vi);
    int selected = (i == sel);
    int hov = (vi == hoverCard);

    if (selected && glowBmp) {
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ ob = SelectObject(mem, glowBmp);
        BitBlt(dc, r.left - 10, r.top - 10,
               (r.right - r.left) + 20, (r.bottom - r.top) + 20,
               mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteDC(mem);
    }

    roundB(dc, &r, 8, hov ? C_CARD_HOV : C_CARD,
           selected ? C_CYAN : C_BORDER);

    Wall *w = &walls[i];
    buildThumbs(w);

    RECT im = {r.left + 4, r.top + 4, r.right - 4, r.top + 4 + THUMB_H};
    HRGN clip = CreateRoundRectRgn(im.left, im.top, im.right+1, im.bottom+1, 6, 6);
    SelectClipRgn(dc, clip);
    if (w->thumb) {
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ ob = SelectObject(mem, w->thumb);
        BitBlt(dc, im.left, im.top, THUMB_W - 8, THUMB_H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteDC(mem);
    } else {
        fillC(dc, &im, C_CARD_HOV);
        RECT e = im;
        textC(dc, w->loading ? "Carregando..." : "...", &e, fSmall, C_SUB_DIM,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);

    // bolinha
    int cx = r.left + 18, cy = r.bottom - 18;
    if (selected) {
        HBRUSH b = CreateSolidBrush(C_CYAN);
        HPEN p = CreatePen(PS_SOLID, 2, RGB(255,255,255));
        HGDIOBJ ob = SelectObject(dc,b), op = SelectObject(dc,p);
        Ellipse(dc, cx-10, cy-10, cx+10, cy+10);
        SelectObject(dc,ob); SelectObject(dc,op); DeleteObject(b); DeleteObject(p);
        icCheck(dc, cx, cy, RGB(20,20,30));
    } else {
        HBRUSH b = CreateSolidBrush(RGB(20,20,30));
        HPEN p = CreatePen(PS_SOLID, 2, RGB(200,200,220));
        HGDIOBJ ob = SelectObject(dc,b), op = SelectObject(dc,p);
        Ellipse(dc, cx-10, cy-10, cx+10, cy+10);
        SelectObject(dc,ob); SelectObject(dc,op); DeleteObject(b); DeleteObject(p);
    }

    if (w->fav) {
        icStarFilled(dc, r.right - 16, r.top + 16, 7, C_PURPLE);
    }
}

// ============================================================
//                      config
// ============================================================
static void drawCheck(HDC dc, int x, int y, int on) {
    RECT b = {x, y, x + 20, y + 20};
    if (on) {
        roundC(dc, &b, 4, C_CYAN);
        icCheck(dc, x + 10, y + 10, RGB(20,20,30));
    } else {
        roundB(dc, &b, 4, C_CARD, C_BORDER);
    }
}
static void drawConfig(HDC dc) {
    int y = HEADER_H + TABS_H + PAD;

    RECT r1 = {PAD, y, CFG_W - PAD, y + 44};
    roundB(dc, &r1, 6, C_CARD, C_BORDER);
    drawCheck(dc, r1.left + 16, r1.top + 12, cfgPauseFS);
    RECT t1 = {r1.left + 50, r1.top, r1.right - 20, r1.bottom};
    textC(dc, "Pausar quando uma janela estiver em tela cheia", &t1, fBody, C_TEXT,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += 54;

    RECT r2 = {PAD, y, CFG_W - PAD, y + 44};
    roundB(dc, &r2, 6, C_CARD, C_BORDER);
    drawCheck(dc, r2.left + 16, r2.top + 12, cfgStartWin);
    RECT t2 = {r2.left + 50, r2.top, r2.right - 20, r2.bottom};
    textC(dc, "Iniciar junto com o Windows", &t2, fBody, C_TEXT,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += 54;

    RECT r3 = {PAD, y, CFG_W - PAD, y + 44};
    roundB(dc, &r3, 6, C_CARD, C_BORDER);
    RECT t3 = {r3.left + 16, r3.top, r3.left + 260, r3.bottom};
    textC(dc, "Trocar wallpaper a cada:", &t3, fBody, C_TEXT,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    char buf[32];
    if (cfgPlaylist == 0) strcpy(buf, "desligado");
    else snprintf(buf, sizeof buf, "%d min", cfgPlaylist);
    RECT b3 = {r3.left + 270, r3.top, r3.right - 100, r3.bottom};
    textC(dc, buf, &b3, fBody, C_CYAN, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT minus = {r3.right - 90, r3.top + 8, r3.right - 62, r3.bottom - 8};
    RECT plus  = {r3.right - 56, r3.top + 8, r3.right - 28, r3.bottom - 8};
    roundB(dc, &minus, 4, C_BG_TOP, C_BORDER);
    roundB(dc, &plus,  4, C_BG_TOP, C_BORDER);
    HPEN p = CreatePen(PS_SOLID, 2, C_TEXT);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, minus.left + 8, (minus.top+minus.bottom)/2, NULL);
    LineTo(dc, minus.right - 8, (minus.top+minus.bottom)/2);
    MoveToEx(dc, plus.left + 8, (plus.top+plus.bottom)/2, NULL);
    LineTo(dc, plus.right - 8, (plus.top+plus.bottom)/2);
    MoveToEx(dc, (plus.left+plus.right)/2, plus.top + 8, NULL);
    LineTo(dc, (plus.left+plus.right)/2, plus.bottom - 8);
    SelectObject(dc, op); DeleteObject(p);
    y += 54;

    RECT r4 = {PAD, y, CFG_W - PAD, y + 60};
    roundB(dc, &r4, 6, C_CARD, C_BORDER);
    RECT t4 = {r4.left + 16, r4.top + 8, r4.right - 20, r4.top + 28};
    textC(dc, "Pasta padrao (adiciona todos os wallpapers da pasta)", &t4, fSmall, C_SUB,
          DT_LEFT | DT_SINGLELINE);
    RECT v4 = {r4.left + 16, r4.top + 30, r4.right - 130, r4.bottom - 8};
    textC(dc, cfgFolder[0] ? cfgFolder : "(nenhuma)", &v4, fSmall, C_TEXT,
          DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT b4 = {r4.right - 120, r4.top + 14, r4.right - 12, r4.top + 46};
    roundB(dc, &b4, 4, C_BG_TOP, C_BORDER);
    RECT b4t = b4;
    textC(dc, "Escolher...", &b4t, fSmall, C_CYAN,
          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    y += 70;

    RECT info = {PAD, y, CFG_W - PAD, y + 40};
    textC(dc, "Wallpapers: use o botao Adicionar. Botao direito no card para opcoes.",
          &info, fSmall, C_SUB_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// ============================================================
//                      paint
// ============================================================
static void paintCfg(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom;

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, W, H);
    HGDIOBJ obm = SelectObject(mem, bm);

    roundGrad(mem, &rc, 0, C_BG_TOP, C_BG_BOT, FALSE);

    icBolt(mem, 40, HEADER_H/2, 15, C_CYAN, C_PURPLE);
    RECT l1 = {64, 8, 240, HEADER_H - 8};
    textC(mem, "Lite", &l1, fLogo, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SIZE sz;
    HGDIOBJ of = SelectObject(mem, fLogo);
    GetTextExtentPoint32(mem, "Lite", 4, &sz);
    SelectObject(mem, of);
    RECT l2 = {64 + sz.cx, 8, 240, HEADER_H - 8};
    textC(mem, "Wall", &l2, fLogo, C_CYAN, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (tab == 0) {
        RECT ab = addBtn();
        roundB(mem, &ab, 6, C_CARD, C_BORDER);
        icPlus(mem, ab.left + 16, (ab.top+ab.bottom)/2, 5, C_CYAN);
        RECT at = {ab.left + 30, ab.top, ab.right, ab.bottom};
        textC(mem, "Adicionar", &at, fSmall, C_CYAN,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    const char *tabNames[2] = {"Biblioteca", "Configuracoes"};
    for (int i = 0; i < 2; i++) {
        RECT r = tabRect(i);
        int on = (tab == i);
        roundB(mem, &r, 4, on ? C_CARD_HOV : C_CARD, on ? C_CYAN : C_BORDER);
        textC(mem, tabNames[i], &r, fBody, on ? C_CYAN : C_SUB,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (tab == 1) {
        drawConfig(mem);
    } else {
        RECT ga = gridArea();
        HRGN clip = CreateRectRgn(ga.left - 20, ga.top - 20, ga.right + 20, ga.bottom);
        SelectClipRgn(mem, clip);
        int n = visibleCount();
        if (n == 0) {
            RECT e = {ga.left, ga.top + 60, ga.right, ga.top + 100};
            textC(mem, wallCount == 0 ? "Sua biblioteca esta vazia."
                                      : "Nenhum wallpaper com esse filtro.",
                  &e, fBody, C_SUB, DT_CENTER | DT_SINGLELINE);
            RECT e2 = {ga.left, ga.top + 130, ga.right, ga.top + 160};
            textC(mem, "Use Adicionar ou escolha uma pasta em Configuracoes.",
                  &e2, fSmall, C_SUB_DIM, DT_CENTER | DT_SINGLELINE);
        } else {
            for (int v = 0; v < n; v++) {
                RECT r = cardRect(v);
                if (r.bottom < ga.top - 10 || r.top > ga.bottom + 10) continue;
                drawCard(mem, v);
            }
        }
        SelectClipRgn(mem, NULL);
        DeleteObject(clip);

        drawPreview(mem);

        RECT sep = {PAD, CFG_H - 74, W - PAD, CFG_H - 73};
        fillC(mem, &sep, C_BORDER);
        RECT vl = {PAD + 10, CFG_H - 60, PAD + 100, CFG_H - 36};
        textC(mem, "Velocidade", &vl, fSmall, C_SUB,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT sl = sliderRect();
        int x0 = sl.left, x1 = sl.right;
        int y = (sl.top + sl.bottom) / 2;
        RECT tr = {x0, y-3, x1, y+3};
        fillC(mem, &tr, C_TRACK);
        float t = (speed - 0.25f) / (2.0f - 0.25f);
        if (t < 0) t = 0; if (t > 1) t = 1;
        int xf = x0 + (int)((x1-x0)*t);
        RECT fr = {x0, y-3, xf, y+3};
        fillC(mem, &fr, C_CYAN);
        HBRUSH bb = CreateSolidBrush(C_CYAN);
        HPEN pb = CreatePen(PS_SOLID, 2, RGB(255,255,255));
        HGDIOBJ ob = SelectObject(mem,bb), op = SelectObject(mem,pb);
        Ellipse(mem, xf-8, y-8, xf+8, y+8);
        SelectObject(mem,ob); SelectObject(mem,op);
        DeleteObject(bb); DeleteObject(pb);
        char buf[16];
        snprintf(buf, sizeof buf, "%.2fx", speed);
        RECT vr = {x1 + 14, sl.top, x1 + 80, sl.bottom};
        textC(mem, buf, &vr, fSmall, C_CYAN,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // overlay de loading
    if (loadingCount > 0 || scanning) {
        HDC tmp = CreateCompatibleDC(mem);
        HBITMAP tb = CreateCompatibleBitmap(mem, W, H);
        HGDIOBJ ot = SelectObject(tmp, tb);
        RECT fr2 = {0,0,W,H};
        fillC(tmp, &fr2, RGB(10,10,20));
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 160, 0};
        AlphaBlend(mem, 0, 0, W, H, tmp, 0, 0, W, H, bf);
        SelectObject(tmp, ot); DeleteObject(tb); DeleteDC(tmp);

        static int spin = 0;
        spin = (spin + 20) % 360;
        int cx = W/2, cy = H/2 - 20;
        for (int i = 0; i < 8; i++) {
            double a = (spin + i*45) * 3.14159 / 180.0;
            int px = cx + (int)(cos(a)*30);
            int py = cy + (int)(sin(a)*30);
            int alpha = 255 - i*28;
            if (alpha < 30) alpha = 30;
            COLORREF c = RGB(0, 229*alpha/255, 255*alpha/255);
            HBRUSH b = CreateSolidBrush(c);
            HGDIOBJ obb = SelectObject(mem, b);
            HGDIOBJ opp = SelectObject(mem, GetStockObject(NULL_PEN));
            Ellipse(mem, px-4, py-4, px+4, py+4);
            SelectObject(mem, obb); SelectObject(mem, opp);
            DeleteObject(b);
        }

        char msg[160];
        if (scanning && loadingCount == 0)
            snprintf(msg, sizeof msg, "Escaneando pasta...");
        else if (loadingCount > 0)
            snprintf(msg, sizeof msg, "Carregando wallpapers... (%d restantes)", loadingCount);
        else
            snprintf(msg, sizeof msg, "Carregando...");
        RECT trr = {0, cy + 50, W, cy + 80};
        textC(mem, msg, &trr, fBody, C_TEXT, DT_CENTER | DT_SINGLELINE);

        SetTimer(h, 99, 80, NULL);
    } else {
        KillTimer(h, 99);
    }

    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, obm);
    DeleteObject(bm);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

// ============================================================
//                      logica
// ============================================================
static void applyWall(const char *p) {
    if (!p || !p[0]) return;
    for (int i = 0; i < wallCount; i++) {
        if (!_stricmp(walls[i].path, p)) {
            if (walls[i].loading || !walls[i].thumb) return;
            break;
        }
    }
    strncpy(file, p, MAX_PATH - 1);
    file[MAX_PATH-1] = 0;
    saveCfg();
    launch();
    InvalidateRect(hcfg, NULL, TRUE);
}
static void setSpeedFromX(int x) {
    RECT sl = sliderRect();
    int x0 = sl.left, x1 = sl.right;
    float t = (float)(x - x0) / (float)(x1 - x0);
    if (t < 0) t = 0; if (t > 1) t = 1;
    speed = 0.25f + t * (2.0f - 0.25f);
    mpvSpeed(speed);
}
static void updateScroll(void) {
    RECT g = gridArea();
    int n = visibleCount();
    int rows = (n + COLS - 1) / COLS;
    int contentH = rows * (CARD_H + GAP);
    int viewH = g.bottom - g.top;
    maxScroll = contentH > viewH ? contentH - viewH + 8 : 0;
    if (scrollY > maxScroll) scrollY = maxScroll;
    if (scrollY < 0) scrollY = 0;
}

static void playlistTick(void) {
    if (cfgPlaylist <= 0) return;
    int n = visibleCount();
    if (n <= 1) return;
    DWORD now = GetTickCount();
    DWORD interval = (DWORD)cfgPlaylist * 60 * 1000;
    if (playlistLast == 0) { playlistLast = now; return; }
    if (now - playlistLast >= interval) {
        playlistLast = now;
        int cur = -1;
        for (int v = 0; v < n; v++) {
            int i = visibleAt(v);
            if (i >= 0 && !_stricmp(walls[i].path, file)) { cur = v; break; }
        }
        int nxt = (cur + 1) % n;
        int i = visibleAt(nxt);
        if (i >= 0) applyWall(walls[i].path);
    }
}

// ============================================================
//                      cfgProc
// ============================================================
static LRESULT CALLBACK cfgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    // bloqueia interacao durante carregamento / scan
    if ((loadingCount > 0 || scanning) &&
        (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_RBUTTONUP ||
         m == WM_MOUSEMOVE || m == WM_MOUSEWHEEL || m == WM_LBUTTONDBLCLK)) {
        if (m == WM_MOUSEMOVE) SetCursor(LoadCursor(NULL, IDC_WAIT));
        return 0;
    }

    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paintCfg(h); return 0;

    case WM_TIMER:
        if (wp == 99) {
            if (loadingCount > 0 || scanning)
                InvalidateRect(h, NULL, FALSE);
            else
                KillTimer(h, 99);
            return 0;
        }
        break;

    case WM_SCAN_DONE: {
        if (wp == 1) {
            char *path = (char*)lp;
            wallsAdd(path);
            free(path);
            updateScroll();
            InvalidateRect(h, NULL, TRUE);
            return 0;
        } else {
            ScanJob *j = (ScanJob*)lp;
            if (j) free(j);
            scanning = 0;
            updateScroll();
            InvalidateRect(h, NULL, TRUE);
            return 0;
        }
    }

    case WM_THUMB_DONE: {
        ThumbJob *j = (ThumbJob*)lp;
        if (j) {
            for (int i = 0; i < wallCount; i++) {
                if (!_stricmp(walls[i].path, j->video)) {
                    if (walls[i].orig)  { DeleteObject(walls[i].orig);  walls[i].orig = NULL; }
                    if (walls[i].thumb) { DeleteObject(walls[i].thumb); walls[i].thumb = NULL; }
                    if (walls[i].big)   { DeleteObject(walls[i].big);   walls[i].big = NULL; }
                    if (walls[i].loading) {
                        walls[i].loading = 0;
                        if (loadingCount > 0) loadingCount--;
                    }
                    buildThumbs(&walls[i]);
                    break;
                }
            }
            free(j);
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (dragging) { setSpeedFromX(x); InvalidateRect(h, NULL, FALSE); return 0; }

        int oldCard = hoverCard, oldBtn = hoverBtn;
        hoverCard = -1; hoverBtn = -1;

        if (tab == 0) {
            RECT ga = gridArea();
            if (inR(&ga, x, y)) {
                int n = visibleCount();
                for (int v = 0; v < n; v++) {
                    RECT r = cardRect(v);
                    if (inR(&r, x, y)) { hoverCard = v; break; }
                }
            } else {
                if (hitApply(x, y)) hoverBtn = 0;
                else if (hitFav(x, y)) hoverBtn = 1;
                else if (hitDel(x, y)) hoverBtn = 2;
            }
        }

        if (oldCard != hoverCard || oldBtn != hoverBtn)
            InvalidateRect(h, NULL, FALSE);

        if (hitSlider(x, y) || hoverCard >= 0 || hoverBtn >= 0 || hitAdd(x, y))
            SetCursor(LoadCursor(NULL, IDC_HAND));
        else
            SetCursor(LoadCursor(NULL, IDC_ARROW));
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        scrollY -= (d / WHEEL_DELTA) * 50;
        updateScroll();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);

        for (int i = 0; i < 2; i++) {
            RECT r = tabRect(i);
            if (inR(&r, x, y)) {
                tab = i;
                scrollY = 0;
                InvalidateRect(h, NULL, TRUE);
                return 0;
            }
        }

        if (tab == 1) {
            RECT r1 = {PAD, HEADER_H + TABS_H + PAD, CFG_W - PAD, HEADER_H + TABS_H + PAD + 44};
            if (inR(&r1, x, y)) { cfgPauseFS = !cfgPauseFS; saveCfg(); InvalidateRect(h, NULL, FALSE); return 0; }
            RECT r2 = {PAD, r1.bottom + 10, CFG_W - PAD, r1.bottom + 54};
            if (inR(&r2, x, y)) {
                cfgStartWin = !cfgStartWin;
                setStartWithWindows(cfgStartWin);
                saveCfg();
                InvalidateRect(h, NULL, FALSE);
                return 0;
            }
            RECT r3 = {PAD, r2.bottom + 10, CFG_W - PAD, r2.bottom + 54};
            RECT minus = {r3.right - 90, r3.top + 8, r3.right - 62, r3.bottom - 8};
            RECT plus  = {r3.right - 56, r3.top + 8, r3.right - 28, r3.bottom - 8};
            if (inR(&minus, x, y)) {
                cfgPlaylist -= 5; if (cfgPlaylist < 0) cfgPlaylist = 0;
                playlistLast = 0;
                saveCfg(); InvalidateRect(h, NULL, FALSE); return 0;
            }
            if (inR(&plus, x, y)) {
                cfgPlaylist += 5; if (cfgPlaylist > 240) cfgPlaylist = 240;
                playlistLast = 0;
                saveCfg(); InvalidateRect(h, NULL, FALSE); return 0;
            }
            RECT r4 = {PAD, r3.bottom + 10, CFG_W - PAD, r3.bottom + 70};
            RECT b4 = {r4.right - 120, r4.top + 14, r4.right - 12, r4.top + 46};
            if (inR(&b4, x, y)) {
                BROWSEINFO bi = {0};
                bi.hwndOwner = h;
                bi.lpszTitle = "Escolha a pasta com wallpapers";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                LPITEMIDLIST idl = SHBrowseForFolder(&bi);
                if (idl) {
                    SHGetPathFromIDList(idl, cfgFolder);
                    CoTaskMemFree(idl);
                    saveCfg();
                    if (!scanning) {
                        scanning = 1;
                        ScanJob *sj = malloc(sizeof *sj);
                        strncpy(sj->folder, cfgFolder, MAX_PATH-1);
                        sj->folder[MAX_PATH-1] = 0;
                        sj->notify = h;
                        HANDLE t = CreateThread(NULL, 0, scanThread, sj, 0, NULL);
                        if (t) CloseHandle(t);
                        else { scanning = 0; free(sj); }
                    }
                    InvalidateRect(h, NULL, TRUE);
                }
                return 0;
            }
            return 0;
        }

        if (hitAdd(x, y)) {
            char tmp[MAX_PATH];
            strncpy(tmp, file, MAX_PATH); tmp[MAX_PATH-1]=0;
            file[0] = 0;
            if (pick()) {
                wallsAdd(file);
                strncpy(file, tmp, MAX_PATH);
                saveCfg();
                updateScroll();
                InvalidateRect(h, NULL, TRUE);
            } else {
                strncpy(file, tmp, MAX_PATH);
            }
            return 0;
        }

        RECT ga = gridArea();
        if (inR(&ga, x, y)) {
            int n = visibleCount();
            for (int v = 0; v < n; v++) {
                RECT r = cardRect(v);
                if (inR(&r, x, y)) {
                    int i = visibleAt(v);
                    if (i >= 0) {
                        sel = i;
                        buildGlow();
                        InvalidateRect(h, NULL, TRUE);
                    }
                    return 0;
                }
            }
        }

        if (hitApply(x, y) && sel >= 0 && sel < wallCount) {
            applyWall(walls[sel].path);
            return 0;
        }
        if (hitFav(x, y) && sel >= 0 && sel < wallCount) {
            walls[sel].fav = !walls[sel].fav;
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (hitDel(x, y) && sel >= 0 && sel < wallCount) {
            wallsRemove(sel);
            buildGlow();
            updateScroll();
            InvalidateRect(h, NULL, TRUE);
            return 0;
        }

        if (hitSlider(x, y)) {
            dragging = 1;
            SetCapture(h);
            setSpeedFromX(x);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (tab != 0) return 0;
        RECT ga = gridArea();
        if (inR(&ga, x, y)) {
            int n = visibleCount();
            for (int v = 0; v < n; v++) {
                RECT r = cardRect(v);
                if (inR(&r, x, y)) {
                    int i = visibleAt(v);
                    if (i < 0) return 0;
                    POINT p = {x, y};
                    ClientToScreen(h, &p);
                    HMENU menu = CreatePopupMenu();
                    AppendMenu(menu, MF_STRING, 1000 + i, "Aplicar");
                    AppendMenu(menu, MF_STRING, 1100 + i, walls[i].fav ? "Desfavoritar" : "Favoritar");
                    AppendMenu(menu, MF_SEPARATOR, 0, NULL);
                    AppendMenu(menu, MF_STRING, 1200 + i, "Categoria: Anime");
                    AppendMenu(menu, MF_STRING, 1300 + i, "Categoria: Scenery");
                    AppendMenu(menu, MF_STRING, 1400 + i, "Categoria: Abstract");
                    AppendMenu(menu, MF_SEPARATOR, 0, NULL);
                    AppendMenu(menu, MF_STRING, 2000 + i, "Remover da biblioteca");
                    int c = TrackPopupMenu(menu, TPM_RETURNCMD, p.x, p.y, 0, h, NULL);
                    DestroyMenu(menu);
                    if (c >= 2000) {
                        wallsRemove(c - 2000);
                        buildGlow();
                        updateScroll();
                        InvalidateRect(h, NULL, TRUE);
                    } else if (c >= 1400) {
                        walls[c - 1400].cat = 2; InvalidateRect(h, NULL, FALSE);
                    } else if (c >= 1300) {
                        walls[c - 1300].cat = 1; InvalidateRect(h, NULL, FALSE);
                    } else if (c >= 1200) {
                        walls[c - 1200].cat = 0; InvalidateRect(h, NULL, FALSE);
                    } else if (c >= 1100) {
                        walls[c - 1100].fav = !walls[c - 1100].fav;
                        InvalidateRect(h, NULL, FALSE);
                    } else if (c >= 1000) {
                        applyWall(walls[c - 1000].path);
                    }
                    return 0;
                }
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (dragging) { dragging = 0; ReleaseCapture(); saveCfg(); }
        return 0;

    case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;
    case WM_DESTROY: return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

// ============================================================
//                      wallProc
// ============================================================
static LRESULT CALLBACK wallProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_TIMER: {
        static int t = 0;
        if (++t < 2) return 0;
        t = 0;
        int b = busy();
        if (b != paused) { paused = b; mpvPause(b); }
        playlistTick();
        return 0;
    }
    case WM_TRAY:
        if (lp == WM_LBUTTONDBLCLK) {
            ShowWindow(hcfg, SW_SHOW);
            SetForegroundWindow(hcfg);
            InvalidateRect(hcfg, NULL, TRUE);
        } else if (lp == WM_RBUTTONUP) {
            POINT p; GetCursorPos(&p);
            HMENU menu = CreatePopupMenu();
            AppendMenu(menu, MF_STRING, 1, "Abrir configuracao");
            AppendMenu(menu, MF_STRING, 2, "Sair");
            SetForegroundWindow(h);
            int c = TrackPopupMenu(menu, TPM_RETURNCMD, p.x, p.y, 0, h, NULL);
            DestroyMenu(menu);
            if (c == 1) {
                ShowWindow(hcfg, SW_SHOW);
                SetForegroundWindow(hcfg);
                InvalidateRect(hcfg, NULL, TRUE);
            }
            if (c == 2) DestroyWindow(h);
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIcon(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

// ============================================================
//                      main
// ============================================================
int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    (void)hp; (void)cmd; (void)show;
    SetProcessDPIAware();
    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);

    GetModuleFileName(NULL, dir, MAX_PATH);
    *strrchr(dir, '\\') = 0;
    snprintf(cfg, sizeof cfg, "%s\\litewall.cfg", dir);

    gdipInit();

    if (__argc > 1) strncpy(file, __argv[1], MAX_PATH - 1);
    else loadCfg();

    job = CreateJobObject(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);

    fLogo  = CreateFont(-24,0,0,0,FW_BOLD,    0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI");
    fBody  = CreateFont(-14,0,0,0,FW_NORMAL,  0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI");
    fSmall = CreateFont(-12,0,0,0,FW_NORMAL,  0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI");
    fSec   = CreateFont(-11,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI");
    fBtn   = fBody;
    bBg    = CreateSolidBrush(C_BG_TOP);

    WNDCLASS wc = {0};
    wc.lpfnWndProc = wallProc;
    wc.hInstance = hi;
    wc.lpszClassName = "LiteWall";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClass(&wc);
    hw = CreateWindowEx(0,"LiteWall","LiteWall",WS_CHILD|WS_VISIBLE,
                        0,0,sw,sh,getWorker(),NULL,hi,NULL);

    WNDCLASS wc2 = {0};
    wc2.lpfnWndProc = cfgProc;
    wc2.hInstance = hi;
    wc2.lpszClassName = "LiteWallCfg";
    wc2.hbrBackground = bBg;
    wc2.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc2);
    hcfg = CreateWindowEx(0,"LiteWallCfg","LiteWall",
                          WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
                          CW_USEDEFAULT,CW_USEDEFAULT,CFG_W,CFG_H,
                          NULL,NULL,hi,NULL);

    updateScroll();
    buildGlow();

    nid.cbSize = sizeof nid;
    nid.hWnd = hw;
    nid.uID = 1;
    nid.uFlags = NIF_ICON|NIF_MESSAGE|NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    strcpy(nid.szTip, "LiteWall");
    Shell_NotifyIcon(NIM_ADD, &nid);

    // so aplica se tiver file carregado do cfg
    if (file[0]) launch();
    SetTimer(hw, 1, 500, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (glowBmp) DeleteObject(glowBmp);
    wallsFree();
    DeleteObject(fLogo); DeleteObject(fBody);
    DeleteObject(fSmall); DeleteObject(fSec); DeleteObject(bBg);
    if (pipeH != INVALID_HANDLE_VALUE) CloseHandle(pipeH);
    gdipEnd();
    return 0;
}
