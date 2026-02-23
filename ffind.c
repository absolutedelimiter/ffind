#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif

// -------------------- small helpers --------------------

static void print_winerr(const wchar_t *what) {
    DWORD e = GetLastError();
    wchar_t *msg = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, e, 0, (LPWSTR)&msg, 0, NULL);
    fwprintf(stderr, L"%ls failed (err=%lu): %ls\n", what, (unsigned long)e, msg ? msg : L"(no message)");
    if (msg) LocalFree(msg);
}

static int wcontains_i(const wchar_t *hay, const wchar_t *needle) {
    if (!needle || !*needle) return 1;
    size_t nlen = wcslen(needle);
    for (const wchar_t *p = hay; *p; ++p) {
        if (_wcsnicmp(p, needle, nlen) == 0) return 1;
    }
    return 0;
}

static const wchar_t* basename_ptr(const wchar_t *path) {
    const wchar_t *last = path;
    for (const wchar_t *p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') last = p + 1;
    }
    return last;
}

// extcsv like "c,h,cpp" (no dots required). empty => allow all
static int ext_allowed(const wchar_t *filename, const wchar_t *extcsv) {
    if (!extcsv || !*extcsv) return 1;

    const wchar_t *dot = wcsrchr(filename, L'.');
    if (!dot || !dot[1]) return 0;
    const wchar_t *ext = dot + 1;

    const wchar_t *p = extcsv;
    while (*p) {
        while (*p == L',' || *p == L' ' || *p == L'\t') p++;
        const wchar_t *start = p;
        while (*p && *p != L',') p++;
        size_t len = (size_t)(p - start);
        if (len > 0) {
            if (wcslen(ext) == len && _wcsnicmp(ext, start, len) == 0) return 1;
        }
    }
    return 0;
}

static wchar_t* wcsdup_heap(const wchar_t *s) {
    size_t n = wcslen(s);
    wchar_t *p = (wchar_t*)malloc((n + 1) * sizeof(wchar_t));
    if (!p) return NULL;
    memcpy(p, s, (n + 1) * sizeof(wchar_t));
    return p;
}

static int is_dot_or_dotdot(const wchar_t *s) {
    return (s[0] == L'.' && s[1] == 0) || (s[0] == L'.' && s[1] == L'.' && s[2] == 0);
}

// Join: dir + '\' + name into out (caller provides buffer)
static int join_path(wchar_t *out, size_t cap, const wchar_t *dir, const wchar_t *name) {
    size_t dlen = wcslen(dir);
    size_t nlen = wcslen(name);
    int needs_slash = (dlen > 0 && dir[dlen-1] != L'\\' && dir[dlen-1] != L'/');
    size_t total = dlen + (needs_slash ? 1 : 0) + nlen + 1;
    if (total > cap) return 0;
    wcscpy_s(out, cap, dir);
    if (needs_slash) wcscat_s(out, cap, L"\\");
    wcscat_s(out, cap, name);
    return 1;
}

// Make: dir + "\*" pattern
static int make_glob(wchar_t *out, size_t cap, const wchar_t *dir) {
    size_t dlen = wcslen(dir);
    int needs_slash = (dlen > 0 && dir[dlen-1] != L'\\' && dir[dlen-1] != L'/');
    size_t total = dlen + (needs_slash ? 1 : 0) + 1 + 1;
    if (total > cap) return 0;
    wcscpy_s(out, cap, dir);
    if (needs_slash) wcscat_s(out, cap, L"\\");
    wcscat_s(out, cap, L"*");
    return 1;
}

// -------------------- work queue --------------------

typedef struct Node {
    struct Node *next;
    wchar_t *dir; // owned heap string
} Node;

typedef struct {
    Node *head, *tail;
    LONG active_workers;     // workers currently processing a dir
    LONG stop;              // set when done
    CRITICAL_SECTION mu;
    CONDITION_VARIABLE cv;
} WorkQ;

static void wq_init(WorkQ *q) {
    q->head = q->tail = NULL;
    q->active_workers = 0;
    q->stop = 0;
    InitializeCriticalSection(&q->mu);
    InitializeConditionVariable(&q->cv);
}

static void wq_destroy(WorkQ *q) {
    EnterCriticalSection(&q->mu);
    Node *n = q->head;
    while (n) {
        Node *nx = n->next;
        free(n->dir);
        free(n);
        n = nx;
    }
    q->head = q->tail = NULL;
    LeaveCriticalSection(&q->mu);
    DeleteCriticalSection(&q->mu);
}

// push dir string (takes ownership)
static void wq_push_owned(WorkQ *q, wchar_t *dir_owned) {
    Node *n = (Node*)malloc(sizeof(Node));
    if (!n) {
        // out of memory: drop work item
        free(dir_owned);
        return;
    }
    n->dir = dir_owned;
    n->next = NULL;

    EnterCriticalSection(&q->mu);
    if (q->tail) q->tail->next = n;
    else q->head = n;
    q->tail = n;
    WakeConditionVariable(&q->cv);
    LeaveCriticalSection(&q->mu);
}

// pop dir string (caller owns returned dir) or NULL if should stop
static wchar_t* wq_pop(WorkQ *q) {
    EnterCriticalSection(&q->mu);
    for (;;) {
        if (q->stop) {
            LeaveCriticalSection(&q->mu);
            return NULL;
        }
        if (q->head) {
            Node *n = q->head;
            q->head = n->next;
            if (!q->head) q->tail = NULL;
            wchar_t *dir = n->dir;
            free(n);
            InterlockedIncrement(&q->active_workers);
            LeaveCriticalSection(&q->mu);
            return dir;
        }
        // no queued work: if no one active, we are done
        if (q->active_workers == 0) {
            q->stop = 1;
            WakeAllConditionVariable(&q->cv);
            LeaveCriticalSection(&q->mu);
            return NULL;
        }
        SleepConditionVariableCS(&q->cv, &q->mu, INFINITE);
    }
}

// mark worker finished a dir
static void wq_done_one(WorkQ *q) {
    EnterCriticalSection(&q->mu);
    InterlockedDecrement(&q->active_workers);
    WakeAllConditionVariable(&q->cv);
    LeaveCriticalSection(&q->mu);
}

// -------------------- shared settings/stats --------------------

typedef struct {
    const wchar_t *needle;
    const wchar_t *extcsv;
    int match_full_path;
    volatile LONG64 found;
    volatile LONG64 dirs_scanned;
    volatile LONG64 files_scanned;
    CRITICAL_SECTION out_mu; // serialize output
    WorkQ *q;
} Ctx;

// -------------------- worker --------------------

static DWORD WINAPI worker_thread(LPVOID p) {
    Ctx *ctx = (Ctx*)p;

    // stack-ish buffers to avoid heap churn
    wchar_t glob[MAX_PATH * 8];
    wchar_t full[MAX_PATH * 8];

    for (;;) {
        wchar_t *dir = wq_pop(ctx->q);
        if (!dir) break;

        InterlockedIncrement64(&ctx->dirs_scanned);

        if (!make_glob(glob, ARRAYSIZE(glob), dir)) {
            free(dir);
            wq_done_one(ctx->q);
            continue;
        }

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(glob, &fd);
        if (h == INVALID_HANDLE_VALUE) {
            free(dir);
            wq_done_one(ctx->q);
            continue;
        }

        do {
            const wchar_t *name = fd.cFileName;
            if (is_dot_or_dotdot(name)) continue;

            if (!join_path(full, ARRAYSIZE(full), dir, name)) {
                // path too long for our buffer: skip (upgrade later with dynamic buffers/\\?\)
                continue;
            }

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // avoid cycles via junctions/symlinks
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

                // enqueue subdir
                wchar_t *copy = wcsdup_heap(full);
                if (copy) wq_push_owned(ctx->q, copy);
            } else {
                InterlockedIncrement64(&ctx->files_scanned);

                if (!ext_allowed(name, ctx->extcsv)) continue;

                const wchar_t *target = ctx->match_full_path ? full : basename_ptr(full);
                if (wcontains_i(target, ctx->needle)) {
                    InterlockedIncrement64(&ctx->found);

                    EnterCriticalSection(&ctx->out_mu);
                    wprintf(L"%ls\n", full);
                    LeaveCriticalSection(&ctx->out_mu);
                }
            }

        } while (FindNextFileW(h, &fd));

        FindClose(h);
        free(dir);
        wq_done_one(ctx->q);
    }

    return 0;
}

// -------------------- timing --------------------

static double qpc_seconds(void) {
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

// -------------------- main --------------------

static void usage(void) {
    fwprintf(stderr,
        L"Usage:\n"
        L"  ffind <root> <needle> [-e ext1,ext2,...] [-f] [-t N]\n\n"
        L"Examples:\n"
        L"  ffind C:\\\\Users\\\\banis prime -e c,h,cpp\n"
        L"  ffind C:\\\\ source -f -t 8\n");
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) { usage(); return 2; }

    const wchar_t *root = argv[1];
    const wchar_t *needle = argv[2];
    const wchar_t *extcsv = L"";
    int match_full_path = 0;
    int threads = 0;

    for (int i = 3; i < argc; i++) {
        if (wcscmp(argv[i], L"-e") == 0 && i + 1 < argc) {
            extcsv = argv[++i];
        } else if (wcscmp(argv[i], L"-f") == 0) {
            match_full_path = 1;
        } else if (wcscmp(argv[i], L"-t") == 0 && i + 1 < argc) {
            threads = _wtoi(argv[++i]);
        } else {
            fwprintf(stderr, L"Unknown option: %ls\n", argv[i]);
            usage();
            return 2;
        }
    }

    if (threads <= 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        threads = (int)si.dwNumberOfProcessors;
        if (threads < 1) threads = 1;
    }

    WorkQ q;
    wq_init(&q);

    // seed root
    wchar_t *root_copy = wcsdup_heap(root);
    if (!root_copy) {
        fwprintf(stderr, L"Out of memory\n");
        wq_destroy(&q);
        return 1;
    }
    wq_push_owned(&q, root_copy);

    Ctx ctx;
    ctx.needle = needle;
    ctx.extcsv = extcsv;
    ctx.match_full_path = match_full_path;
    ctx.found = 0;
    ctx.dirs_scanned = 0;
    ctx.files_scanned = 0;
    InitializeCriticalSection(&ctx.out_mu);
    ctx.q = &q;

    HANDLE *hs = (HANDLE*)malloc((size_t)threads * sizeof(HANDLE));
    if (!hs) {
        fwprintf(stderr, L"Out of memory\n");
        DeleteCriticalSection(&ctx.out_mu);
        wq_destroy(&q);
        return 1;
    }

    double t0 = qpc_seconds();

    for (int i = 0; i < threads; i++) {
        hs[i] = CreateThread(NULL, 0, worker_thread, &ctx, 0, NULL);
        if (!hs[i]) {
            print_winerr(L"CreateThread");
            threads = i; // wait only created ones
            break;
        }
    }

    WaitForMultipleObjects((DWORD)threads, hs, TRUE, INFINITE);

    double t1 = qpc_seconds();

    for (int i = 0; i < threads; i++) CloseHandle(hs[i]);
    free(hs);

    DeleteCriticalSection(&ctx.out_mu);
    wq_destroy(&q);

    fwprintf(stderr,
        L"Found %lld match(es)\nScanned %lld dirs, %lld files\nThreads: %d\nTime: %.3f s\n",
        (long long)ctx.found,
        (long long)ctx.dirs_scanned,
        (long long)ctx.files_scanned,
        threads,
        (t1 - t0));

    return 0;
}
