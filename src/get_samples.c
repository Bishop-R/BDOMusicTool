/*
 * get_samples.c - Standalone CLI tool to extract BDO music instrument samples
 *
 * Uses shared extraction code from sample_extract.c.
 * Build: linked against sample_extract.c, ice.c, wem2ogg.c — no SDL dependency.
 * Cross-platform: Linux (gcc) and Windows (mingw).
 */

#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sample_extract.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#define PATH_SEP_STR "\\"
#else
#include <dirent.h>
#include <sys/types.h>
#define PATH_SEP_STR "/"
#endif

/* ---- Process check ---- */

static int check_bdo_running(void)
{
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "BlackDesert64.exe") == 0) {
                CloseHandle(snap);
                return 1;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return 0;
#else
    DIR *proc = opendir("/proc");
    if (!proc) return 0;
    struct dirent *ent;
    while ((ent = readdir(proc)) != NULL) {
        char *end;
        long pid_val = strtol(ent->d_name, &end, 10);
        if (*end != '\0' || pid_val <= 0) continue;

        char cmdpath[256];
        snprintf(cmdpath, sizeof(cmdpath), "/proc/%s/comm", ent->d_name);
        FILE *f = fopen(cmdpath, "r");
        if (!f) continue;
        char comm[256];
        if (fgets(comm, sizeof(comm), f)) {
            char *nl = strchr(comm, '\n');
            if (nl) *nl = '\0';
            if (strcmp(comm, "BlackDesert64.e") == 0 ||
                strcmp(comm, "BlackDesert64.exe") == 0) {
                fclose(f);
                closedir(proc);
                return 1;
            }
        }
        fclose(f);
    }
    closedir(proc);
    return 0;
#endif
}

/* ---- Progress callback ---- */

static void cli_progress(int current, int total, const char *name, void *ctx)
{
    (void)ctx;
    printf("[%2d/%d] Extracting %s.bnk ...\n", current, total, name);
    fflush(stdout);
}

/* ---- Main ---- */

static void wait_exit(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
    printf("\nPress Enter to exit...");
    fflush(stdout);
    getchar();
}

int main(int argc, char **argv)
{
    atexit(wait_exit);

#ifdef _WIN32
    {
        char path[4096];
        DWORD len = GetModuleFileNameA(NULL, path, sizeof(path));
        if (len > 0) {
            char *last = strrchr(path, '\\');
            if (last) { *last = '\0'; SetCurrentDirectoryA(path); }
        }
    }
#endif

    printf("BDO Instrument Sample Extractor\n");
    printf("================================\n\n");

    if (check_bdo_running()) {
        fprintf(stderr, "Error: Black Desert Online is currently running.\n");
        fprintf(stderr, "Please close the game before extracting samples.\n");
        return 1;
    }

    /* Find PAZ directory */
    const char *paz_dir = NULL;
    static char user_path[4096];

    if (argc >= 2) {
        paz_dir = argv[1];
    } else {
        paz_dir = se_find_paz_dir();
    }

    if (!paz_dir || !se_dir_exists(paz_dir)) {
        if (paz_dir)
            printf("Tried auto-detect: %s (not found)\n\n", paz_dir);
        printf("Could not find BDO automatically.\n");
        printf("Please enter the path to your BDO 'Paz' folder.\n");
        printf("Examples:\n");
#ifdef _WIN32
        printf("  C:\\Program Files (x86)\\Steam\\steamapps\\common\\Black Desert Online\\Paz\n");
        printf("  C:\\Pearl Abyss\\BlackDesert\\Paz\n");
#else
        printf("  ~/.local/share/Steam/steamapps/common/Black Desert Online/Paz\n");
        printf("  /path/to/BlackDesertOnline/Paz\n");
#endif
        printf("\nPath: ");
        fflush(stdout);
        if (!fgets(user_path, sizeof(user_path), stdin)) return 1;
        size_t pl = strlen(user_path);
        while (pl > 0 && (user_path[pl-1] == '\n' || user_path[pl-1] == '\r' || user_path[pl-1] == ' '))
            user_path[--pl] = '\0';
        if (pl > 2 && user_path[0] == '"' && user_path[pl-1] == '"') {
            memmove(user_path, user_path + 1, pl - 2);
            user_path[pl - 2] = '\0';
        }
        paz_dir = user_path;
        if (!se_dir_exists(paz_dir)) {
            fprintf(stderr, "Error: directory not found: %s\n", paz_dir);
            return 1;
        }
    }

    printf("PAZ directory: %s\n\n", paz_dir);

    const char *out_base = "Data" PATH_SEP_STR "samples";

    int total = extract_all_samples(paz_dir, out_base, cli_progress, NULL);

    if (total < 0) {
        fprintf(stderr, "\nExtraction failed.\n");
        return 1;
    }

    printf("\nDone: %d samples extracted into %s\n", total, out_base);
    return 0;
}
