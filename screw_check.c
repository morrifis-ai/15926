#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <shellapi.h>

#define MAX_PARTS       20000
#define MAX_COUNT_V     512
#define DEF_GAP         8
#define DEF_STABLE      3
#define DEF_IMGSZ       640
#define DEF_STRIDE      1

typedef struct { long frame; int cnt; } Rec;

typedef struct {
    long first_frame;
    long last_frame;
    long i_start;
    long i_end;
    int  stable;
    long stable_frames;
} Part;

static Rec  *g_rec = NULL;
static long  g_rec_n = 0;
static long  g_rec_cap = 0;

static wchar_t g_exe_dir[2048];
static wchar_t g_video[2048];
static wchar_t g_model[2048];
static wchar_t g_csv[2048];
static wchar_t g_report[2048];
static wchar_t g_script[2048];
static wchar_t g_cls[128] = L"auto";

static int    g_expected = 0;
static int    g_gap = DEF_GAP;
static int    g_stable_min = DEF_STABLE;
static int    g_imgsz = DEF_IMGSZ;
static int    g_stride = DEF_STRIDE;
static double g_conf = 0.2;
static int    g_reuse = 0;
static int    g_no_pause = 0;

static char *w2u8(const wchar_t *w)
{
    static char buf[4096];
    if (!w) { buf[0] = 0; return buf; }
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)(sizeof(buf) - 1), NULL, NULL) <= 0)
        buf[0] = 0;
    return buf;
}

static void u2w(const char *u, wchar_t *out, size_t cap)
{
    if (!u) { out[0] = 0; return; }
    if (MultiByteToWideChar(CP_UTF8, 0, u, -1, out, (int)cap) <= 0) out[cap - 1] = 0;
}

static int exists_w(const wchar_t *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static void join_w(wchar_t *out, size_t cap, const wchar_t *base, const wchar_t *sub)
{
    size_t l;
    wcsncpy(out, base, cap - 1);
    out[cap - 1] = 0;
    l = wcslen(out);
    if (l && (out[l - 1] == L'\\' || out[l - 1] == L'/')) out[l - 1] = 0;
    wcsncat(out, L"\\", cap - wcslen(out) - 1);
    wcsncat(out, sub, cap - wcslen(out) - 1);
}

static void get_exe_dir(wchar_t *out, size_t cap)
{
    wchar_t buf[2048];
    size_t i;
    GetModuleFileNameW(NULL, buf, 2048);
    wcsncpy(out, buf, cap - 1);
    out[cap - 1] = 0;
    for (i = wcslen(out); i > 0; i--) {
        if (out[i - 1] == L'\\' || out[i - 1] == L'/') { out[i - 1] = 0; break; }
    }
}

static void append_quoted(wchar_t *cmd, size_t cap, const wchar_t *arg)
{
    size_t len = wcslen(cmd), i, k, nbs = 0;
    if (len + wcslen(arg) * 2 + 8 >= cap) return;
    if (len) cmd[len++] = L' ';
    cmd[len++] = L'"';
    for (i = 0; arg[i]; i++) {
        if (arg[i] == L'\\') { nbs++; continue; }
        if (arg[i] == L'"') { for (k = 0; k < 2 * nbs + 1; k++) cmd[len++] = L'\\'; }
        else { for (k = 0; k < nbs; k++) cmd[len++] = L'\\'; }
        nbs = 0;
        cmd[len++] = arg[i];
    }
    for (k = 0; k < 2 * nbs; k++) cmd[len++] = L'\\';
    cmd[len++] = L'"';
    cmd[len] = 0;
}

static void append_text(wchar_t *cmd, size_t cap, const wchar_t *text)
{
    size_t len = wcslen(cmd);
    if (len + wcslen(text) + 2 >= cap) return;
    if (len) cmd[len++] = L' ';
    wcsncpy(cmd + len, text, cap - len - 1);
    cmd[cap - 1] = 0;
}

static int find_python(wchar_t *out, size_t cap)
{
    wchar_t p[2048];
    const wchar_t *env = _wgetenv(L"SCREW_PYTHON");
    if (env && *env && exists_w(env)) { wcsncpy(out, env, cap - 1); out[cap - 1] = 0; return 1; }
    join_w(p, 2048, g_exe_dir, L"..\\venv\\Scripts\\python.exe");
    if (exists_w(p)) { wcsncpy(out, p, cap - 1); out[cap - 1] = 0; return 1; }
    join_w(p, 2048, g_exe_dir, L"python.exe");
    if (exists_w(p)) { wcsncpy(out, p, cap - 1); out[cap - 1] = 0; return 1; }
    if (SearchPathW(NULL, L"python.exe", NULL, (DWORD)cap, out, NULL) > 0) return 1;
    if (SearchPathW(NULL, L"py.exe", NULL, (DWORD)cap, out, NULL) > 0) return 1;
    return 0;
}

static void push_rec(long frame, int cnt)
{
    if (g_rec_n >= g_rec_cap) {
        long cap = g_rec_cap ? g_rec_cap * 2 : 8192;
        Rec *p = (Rec *)realloc(g_rec, (size_t)cap * sizeof(Rec));
        if (!p) { printf("内存不足\n"); exit(1); }
        g_rec = p;
        g_rec_cap = cap;
    }
    g_rec[g_rec_n].frame = frame;
    g_rec[g_rec_n].cnt = cnt;
    g_rec_n++;
}

static long read_csv(const wchar_t *path)
{
    FILE *fp = _wfopen(path, L"rb");
    char line[512];
    long ok = 0;
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line, *endp = NULL;
        long fr, c;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == 0) continue;
        fr = strtol(p, &endp, 10);
        if (endp == p) continue;
        p = endp;
        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t') p++;
        c = strtol(p, &endp, 10);
        if (endp == p) continue;
        push_rec(fr, (int)c);
        ok++;
    }
    fclose(fp);
    return ok;
}

static int stable_of_range(long start, long end, long *stable_frames_out)
{
    static int score[MAX_COUNT_V + 1];
    static int total[MAX_COUNT_V + 1];
    long i, v, best_total = -1;
    int best = -1, best_score = -1;

    memset(score, 0, sizeof(score));
    memset(total, 0, sizeof(total));
    *stable_frames_out = 0;
    if (start < 0 || end < start) return 0;

    for (i = start; i <= end; i++) {
        int c = g_rec[i].cnt;
        if (c > 0 && c <= MAX_COUNT_V) total[c]++;
    }

    i = start;
    while (i <= end) {
        long j = i;
        int c = g_rec[i].cnt;
        while (j + 1 <= end && g_rec[j + 1].cnt == c) j++;
        if (c > 0 && c <= MAX_COUNT_V && (j - i + 1) >= g_stable_min) score[c] += (int)(j - i + 1);
        i = j + 1;
    }

    for (v = 1; v <= MAX_COUNT_V; v++) {
        if (score[v] <= 0) continue;
        if (score[v] > best_score ||
            (score[v] == best_score && total[v] > best_total) ||
            (score[v] == best_score && total[v] == best_total && (best < 0 || v < best))) {
            best = (int)v;
            best_score = score[v];
            best_total = total[v];
        }
    }

    if (best > 0) {
        *stable_frames_out = best_score;
        return best;
    }

    {
        long n = end - start + 1, acc = 0, half = (n + 1) / 2;
        for (v = 1; v <= MAX_COUNT_V; v++) {
            acc += total[v];
            if (acc >= half) { *stable_frames_out = total[v]; return (int)v; }
        }
    }
    return 0;
}

static void emit(FILE *rep, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    if (rep) fprintf(rep, "%s\r\n", buf);
}

static void usage(void)
{
    printf("用法: screw_check.exe [-n 单体螺丝数] [-v 视频路径] [选项]\n");
    printf("  -n N     单体螺丝数（每个零件应有的螺丝颗数）\n");
    printf("  -v PATH  视频文件路径\n");
    printf("  -m PATH  模型文件（默认与本程序同目录的 screw_best.pt）\n");
    printf("  -c VALUE 置信度阈值（默认 0.2）\n");
    printf("  -g N     连续多少帧未识别到螺丝即判为零件分界（默认 8）\n");
    printf("  -s N     稳定状态所需的最短连续帧数（默认 3）\n");
    printf("  -t N     抽帧步长，1 表示逐帧处理（默认 1）\n");
    printf("  -k CLS   统计哪些类别：auto 自动只算螺丝类（默认），或类别 id/名称，如 0 或 bolt\n");
    printf("  -i N     推理尺寸（默认 640）\n");
    printf("  -f CSV   直接读取已有的检测结果 csv，跳过推理\n");
    printf("  -q       结束后不等待回车（便于脚本调用）\n");
    printf("  -h       显示本帮助\n");
}

static int prompt_int(const char *title, int minv, int maxv)
{
    char line[256];
    for (;;) {
        int v = 0;
        printf("%s", title);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) return -1;
        if (sscanf(line, "%d", &v) == 1 && v >= minv && v <= maxv) return v;
        printf("输入无效，请输入 %d 到 %d 之间的整数。\n", minv, maxv);
    }
}

static int prompt_path(wchar_t *out, size_t cap)
{
    char line[2048];
    for (;;) {
        char *s;
        size_t l;
        printf("请输入视频文件路径（可把视频文件直接拖进本窗口）：");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) return 0;
        s = line;
        l = strlen(s);
        while (l && (s[l - 1] == '\r' || s[l - 1] == '\n' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
        while (*s == ' ' || *s == '\t') { s++; l--; }
        if (l >= 2 && s[0] == '"' && s[l - 1] == '"') { s[l - 1] = 0; s++; }
        if (!*s) continue;
        u2w(s, out, cap);
        if (!exists_w(out)) {
            printf("文件不存在：%s\n", s);
            continue;
        }
        return 1;
    }
}

static int run_python(void)
{
    wchar_t py[2048], cmd[8192], num[64];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    if (!find_python(py, 2048)) {
        printf("未找到 Python 解释器，可用环境变量 SCREW_PYTHON 指定 python.exe 的完整路径。\n");
        return 0;
    }

    cmd[0] = 0;
    append_quoted(cmd, 8192, py);
    append_quoted(cmd, 8192, g_script);
    append_text(cmd, 8192, L"--model");
    append_quoted(cmd, 8192, g_model);
    append_text(cmd, 8192, L"--video");
    append_quoted(cmd, 8192, g_video);
    append_text(cmd, 8192, L"--out");
    append_quoted(cmd, 8192, g_csv);
    swprintf(num, 64, L"%.2f", g_conf);
    append_text(cmd, 8192, L"--conf");
    append_text(cmd, 8192, num);
    swprintf(num, 64, L"%d", g_imgsz);
    append_text(cmd, 8192, L"--imgsz");
    append_text(cmd, 8192, num);
    swprintf(num, 64, L"%d", g_stride);
    append_text(cmd, 8192, L"--stride");
    append_text(cmd, 8192, num);
    append_text(cmd, 8192, L"--cls");
    append_quoted(cmd, 8192, g_cls);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (_wgetenv(L"SCREW_DEBUG")) {
        char dbg[9000];
        WideCharToMultiByte(CP_UTF8, 0, cmd, -1, dbg, 8999, NULL, NULL);
        fprintf(stderr, "CMD=%s\n", dbg);
        fflush(stderr);
    }

    printf("正在调用 YOLOv8 推理，视频较长时需要等待……\n");
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        printf("启动推理进程失败，错误码 %lu\n", (unsigned long)GetLastError());
        return 0;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        printf("推理进程异常结束，退出码 %lu。\n", (unsigned long)code);
        return 0;
    }
    return 1;
}

int main(void)
{
    int argc = 0, i;
    wchar_t **argv;
    FILE *rep = NULL;
    long idx, n_parts = 0, n_missing = 0, seg_start, seg_end, zero_run;
    Part *parts = NULL;

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    get_exe_dir(g_exe_dir, 2048);
    join_w(g_model, 2048, g_exe_dir, L"screw_best.pt");
    join_w(g_script, 2048, g_exe_dir, L"screw_detect.py");
    join_w(g_csv, 2048, g_exe_dir, L"screw_detections.csv");
    join_w(g_report, 2048, g_exe_dir, L"screw_report.txt");

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (i = 1; i < argc; i++) {
            const wchar_t *a = argv[i];
            if (!wcscmp(a, L"-h") || !wcscmp(a, L"--help")) { usage(); LocalFree(argv); return 0; }
            else if (!wcscmp(a, L"-n") && i + 1 < argc) g_expected = _wtoi(argv[++i]);
            else if (!wcscmp(a, L"-v") && i + 1 < argc) { wcsncpy(g_video, argv[++i], 2047); g_video[2047] = 0; }
            else if (!wcscmp(a, L"-m") && i + 1 < argc) { wcsncpy(g_model, argv[++i], 2047); g_model[2047] = 0; }
            else if (!wcscmp(a, L"-f") && i + 1 < argc) { wcsncpy(g_csv, argv[++i], 2047); g_csv[2047] = 0; g_reuse = 1; }
            else if (!wcscmp(a, L"-c") && i + 1 < argc) g_conf = _wtof(argv[++i]);
            else if (!wcscmp(a, L"-g") && i + 1 < argc) g_gap = _wtoi(argv[++i]);
            else if (!wcscmp(a, L"-s") && i + 1 < argc) g_stable_min = _wtoi(argv[++i]);
            else if (!wcscmp(a, L"-t") && i + 1 < argc) g_stride = _wtoi(argv[++i]);
            else if (!wcscmp(a, L"-k") && i + 1 < argc) { wcsncpy(g_cls, argv[++i], 127); g_cls[127] = 0; }
            else if (!wcscmp(a, L"-q")) g_no_pause = 1;
            else if (!wcscmp(a, L"-i") && i + 1 < argc) g_imgsz = _wtoi(argv[++i]);
        }
        LocalFree(argv);
    }

    if (g_gap < 1) g_gap = 1;
    if (g_stable_min < 1) g_stable_min = 1;
    if (g_stride < 1) g_stride = 1;
    if (g_imgsz < 32) g_imgsz = DEF_IMGSZ;
    if (g_conf <= 0 || g_conf > 1) g_conf = 0.2;

    printf("----------------------------------------------------------------\n");
    printf("   螺丝缺失检测（YOLOv8 模型 + C 计数主程序）\n");
    printf("----------------------------------------------------------------\n");

    if (g_expected <= 0) {
        g_expected = prompt_int("请输入单体螺丝数（每个零件应有的螺丝颗数）：", 1, 200);
        if (g_expected <= 0) return 1;
    }

    if (!g_reuse) {
        if (!g_video[0]) {
            if (!prompt_path(g_video, 2048)) return 1;
        } else if (!exists_w(g_video)) {
            printf("视频文件不存在：%s\n", w2u8(g_video));
            return 1;
        }
    }

    printf("\n单体螺丝数 : %d 颗\n", g_expected);
    if (!g_reuse) printf("视频文件   : %s\n", w2u8(g_video));
    printf("置信度阈值 : %.2f\n", g_conf);
    printf("分界帧数   : 连续 %d 帧无识别即切分下一个零件\n", g_gap);
    printf("稳定判定   : 同一计数至少连续 %d 帧\n", g_stable_min);
    printf("统计类别   : %s\n", w2u8(g_cls));
    if (g_stride > 1) printf("抽帧步长   : %d（阈值按处理帧计）\n", g_stride);
    printf("----------------------------------------------------------------\n\n");

    if (!g_reuse) {
        if (!exists_w(g_model)) { printf("模型文件不存在：%s\n", w2u8(g_model)); return 1; }
        if (!exists_w(g_script)) { printf("推理脚本不存在：%s\n", w2u8(g_script)); return 1; }
        DeleteFileW(g_csv);
        if (!run_python()) return 1;
    }

    if (read_csv(g_csv) <= 0) {
        printf("未读取到检测结果：%s\n", w2u8(g_csv));
        printf("可能原因：视频中没有目标被识别，或推理未成功完成。\n");
        return 1;
    }

    printf("检测记录读取完成，共 %ld 条帧记录。\n\n", g_rec_n);

    parts = (Part *)calloc(MAX_PARTS, sizeof(Part));
    if (!parts) { printf("内存不足\n"); return 1; }

    seg_start = -1;
    seg_end = -1;
    zero_run = 0;
    for (idx = 0; idx < g_rec_n; idx++) {
        if (g_rec[idx].cnt > 0) {
            if (seg_start < 0) seg_start = idx;
            seg_end = idx;
            zero_run = 0;
        } else if (seg_start >= 0) {
            zero_run++;
            if (zero_run >= g_gap) {
                if (n_parts < MAX_PARTS) {
                    parts[n_parts].i_start = seg_start;
                    parts[n_parts].i_end = seg_end;
                    parts[n_parts].first_frame = g_rec[seg_start].frame;
                    parts[n_parts].last_frame = g_rec[seg_end].frame;
                    n_parts++;
                }
                seg_start = -1;
                seg_end = -1;
                zero_run = 0;
            }
        }
    }
    if (seg_start >= 0 && n_parts < MAX_PARTS) {
        parts[n_parts].i_start = seg_start;
        parts[n_parts].i_end = seg_end;
        parts[n_parts].first_frame = g_rec[seg_start].frame;
        parts[n_parts].last_frame = g_rec[seg_end].frame;
        n_parts++;
    }

    rep = _wfopen(g_report, L"wb");
    if (rep) {
        fprintf(rep, "\xEF\xBB\xBF");
        fprintf(rep, "单体螺丝数=%d  置信度=%.2f  分界帧数=%d  稳定帧数=%d  类别=%s\r\n",
                g_expected, g_conf, g_gap, g_stable_min, w2u8(g_cls));
        if (!g_reuse) fprintf(rep, "视频=%s\r\n", w2u8(g_video));
        fprintf(rep, "\r\n");
    }

    for (idx = 0; idx < n_parts; idx++) {
        char state[128];
        parts[idx].stable = stable_of_range(parts[idx].i_start, parts[idx].i_end, &parts[idx].stable_frames);

        if (parts[idx].stable >= g_expected) {
            if (parts[idx].stable > g_expected)
                snprintf(state, sizeof(state), "多识别 %d 颗，疑似误检", parts[idx].stable - g_expected);
            else
                snprintf(state, sizeof(state), "正常");
        } else {
            snprintf(state, sizeof(state), "缺 %d 颗", g_expected - parts[idx].stable);
            n_missing++;
        }

        emit(rep, "零件 %02ld | 帧 %ld-%ld | 稳定识别 %d 颗 | 应为 %d 颗 | %s",
             idx + 1, parts[idx].first_frame, parts[idx].last_frame,
             parts[idx].stable, g_expected, state);
    }

    printf("----------------------------------------------------------------\n");
    if (rep) fprintf(rep, "----------------------------------------------------------------\r\n");

    if (n_parts == 0) {
        emit(rep, "统计：视频中未识别到任何螺丝，无法判定零件情况。");
    } else if (n_missing == 0) {
        emit(rep, "统计：共识别到 %ld 个零件，全部螺丝齐全，没有缺螺丝的零件。", n_parts);
    } else {
        emit(rep, "统计：共识别到 %ld 个零件，其中 %ld 个零件缺螺丝。", n_parts, n_missing);
        emit(rep, "缺螺丝明细：");
        for (idx = 0; idx < n_parts; idx++) {
            if (parts[idx].stable < g_expected)
                emit(rep, "  零件 %02ld：缺 %d 颗", idx + 1, g_expected - parts[idx].stable);
        }
    }

    if (rep) {
        fprintf(rep, "\r\n检测明细=%s\r\n", w2u8(g_csv));
        fclose(rep);
        printf("\n结果已保存：%s\n", w2u8(g_report));
    }

    if (!g_no_pause) {
        printf("\n按回车键退出……");
        fflush(stdout);
        fgetc(stdin);
    }

    free(parts);
    free(g_rec);
    return 0;
}
