```c
/*
 * dockside-cli.c
 *
 * A simple fleet‑repo health checker.
 *
 * Compile with:
 *     gcc -Wall -Wextra -std=c11 -o dockside dockside-cli.c
 *
 * Supported commands:
 *   dockside check <repo-path>
 *   dockside scan  <org-path>
 *   dockside score <repo-path>
 *   dockside fix   <repo-path>
 *   dockside report <directory> [--format json|markdown]
 *
 * The program follows the specification given in the prompt.
 */

#define _XOPEN_SOURCE 700   /* for nftw */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fnmatch.h>
#include <errno.h>
#include <stdbool.h>
#include <ftw.h>

#define MAX_PATH 1024
#define README_MIN 100
#define README_EXCELLENT 500
#define FILE_EXCELLENT 500   /* size in bytes for other files */

/* -------------------------------------------------------------------------- */
/* Data structures                                                            */
/* -------------------------------------------------------------------------- */

typedef struct {
    int readme;       /* 0‑3 */
    int license;      /* 0‑3 */
    int tests;        /* 0‑3 */
    int ci;           /* 0‑3 */
    int charter;      /* 0‑3 */
    int abstraction;  /* 0‑3 */
    int state;        /* 0‑3 */
} SectionScores;

typedef struct {
    char path[MAX_PATH];
    SectionScores scores;
    int total;
    const char *grade;
} RepoResult;

/* -------------------------------------------------------------------------- */
/* Helper utilities                                                          */
/* -------------------------------------------------------------------------- */

static bool file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

static bool dir_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/* Return size of a regular file, or -1 on error */
static off_t file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
        return st.st_size;
    return -1;
}

/* Return true if any file matching pattern exists inside dir (non‑recursive) */
static bool any_match_in_dir(const char *dir, const char *pattern)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d)) != NULL) {
        if (fnmatch(pattern, e->d_name, 0) == 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/* -------------------------------------------------------------------------- */
/* Scoring functions                                                          */
/* -------------------------------------------------------------------------- */

static int score_readme(const char *repo)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/README.md", repo);
    if (!file_exists(path))
        return 0;

    off_t sz = file_size(path);
    if (sz < 0) return 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return 0; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    size_t len = strlen(buf);
    free(buf);

    if (len > README_EXCELLENT) return 3;
    if (len > README_MIN)       return 2;
    return 1;   /* file exists but too short */
}

static int score_generic_file(const char *repo, const char *filename)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", repo, filename);
    if (!file_exists(path))
        return 0;

    off_t sz = file_size(path);
    if (sz < 0) return 0;
    if (sz > FILE_EXCELLENT) return 3;
    if (sz > 0)               return 2;
    return 1;   /* empty file – minimal */
}

static int score_tests(const char *repo)
{
    char dirpath[MAX_PATH];
    snprintf(dirpath, sizeof(dirpath), "%s/test", repo);
    bool has_dir = dir_exists(dirpath);

    bool has_pattern = any_match_in_dir(repo, "*.test.*");

    if (has_dir && has_pattern) return 3;
    if (has_dir || has_pattern) return 2;
    return 0;
}

static int score_ci(const char *repo)
{
    char dirpath[MAX_PATH];
    snprintf(dirpath, sizeof(dirpath), "%s/.github/workflows", repo);
    if (!dir_exists(dirpath))
        return 0;

    /* any file inside the workflows directory gives a higher score */
    DIR *d = opendir(dirpath);
    if (!d) return 0;
    struct dirent *e;
    bool any = false;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        any = true;
        break;
    }
    closedir(d);
    return any ? 3 : 2;
}

/* -------------------------------------------------------------------------- */
/* Auto‑fix utilities                                                         */
/* -------------------------------------------------------------------------- */

static void create_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "  [error] cannot create %s: %s\n", path, strerror(errno));
        return;
    }
    fputs(content, f);
    fclose(f);
    printf("  [fixed] %s\n", path);
}

static void fix_repo(const char *repo)
{
    char path[MAX_PATH];

    /* README */
    snprintf(path, sizeof(path), "%s/README.md", repo);
    if (!file_exists(path))
        create_file(path, "# Repository\n\nA short description.\n");

    /* LICENSE */
    snprintf(path, sizeof(path), "%s/LICENSE", repo);
    if (!file_exists(path))
        create_file(path, "MIT License\n\nPermission is hereby granted ...\n");

    /* CHARTER.md */
    snprintf(path, sizeof(path), "%s/CHARTER.md", repo);
    if (!file_exists(path))
        create_file(path, "# Charter\n\nPurpose of the repository.\n");

    /* ABSTRACTION.md */
    snprintf(path, sizeof(path), "%s/ABSTRACTION.md", repo);
    if (!file_exists(path))
        create_file(path, "# Abstraction\n\nHigh‑level design notes.\n");

    /* STATE.md */
    snprintf(path, sizeof(path), "%s/STATE.md", repo);
    if (!file_exists(path))
        create_file(path, "# State\n\nCurrent status of the project.\n");

    /* test directory */
    snprintf(path, sizeof(path), "%s/test", repo);
    if (!dir_exists(path)) {
        if (mkdir(path, 0755) == 0)
            printf("  [fixed] %s (directory created)\n", path);
        else
            fprintf(stderr, "  [error] cannot create %s: %s\n", path, strerror(errno));
    }

    /* CI workflow */
    snprintf(path, sizeof(path), "%s/.github/workflows", repo);
    if (!dir_exists(path)) {
        if (mkdir(path, 0755) == 0) {
            printf("  [fixed] %s (directory created)\n", path);
            /* create a minimal workflow file */
            char wf[MAX_PATH];
            snprintf(wf, sizeof(wf), "%s/ci.yml", path);
            create_file(wf,
                "name: CI\non: [push, pull_request]\njobs:\n  build:\n    runs-on: ubuntu-latest\n    steps:\n      - uses: actions/checkout@v2\n");
        } else {
            fprintf(stderr, "  [error] cannot create %s: %s\n", path, strerror(errno));
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Evaluation of a single repository                                          */
/* -------------------------------------------------------------------------- */

static void evaluate_repo(const char *repo_path, RepoResult *out)
{
    strncpy(out->path, repo_path, sizeof(out->path) - 1);
    out->path[sizeof(out->path) - 1] = '\0';

    out->scores.readme       = score_readme(repo_path);
    out->scores.license      = score_generic_file(repo_path, "LICENSE");
    out->scores.tests        = score_tests(repo_path);
    out->scores.ci           = score_ci(repo_path);
    out->scores.charter      = score_generic_file(repo_path, "CHARTER.md");
    out->scores.abstraction  = score_generic_file(repo_path, "ABSTRACTION.md");
    out->scores.state        = score_generic_file(repo_path, "STATE.md");

    out->total = out->scores.readme + out->scores.license + out->scores.tests +
                 out->scores.ci + out->scores.charter + out->scores.abstraction +
                 out->scores.state;

    if (out->total >= 18)
        out->grade = "Seaworthy";
    else if (out->total >= 12)
        out->grade = "Conditional";
    else
        out->grade = "Failing";
}

/* -------------------------------------------------------------------------- */
/* Scan a directory for repositories (sub‑directories)                        */
/* -------------------------------------------------------------------------- */

static int scan_callback(const char *fpath, const struct stat *sb,
                         int typeflag, struct FTW *ftwbuf)
{
    (void)sb; (void)typeflag; (void)ftwbuf;
    /* Only consider immediate sub‑directories of the root argument */
    if (ftwbuf->level == 1 && S_ISDIR(sb->st_mode)) {
        RepoResult r;
        evaluate_repo(fpath, &r);
        printf("%s: %d/21 – %s\n", r.path, r.total, r.grade);
    }
    return 0;   /* continue */
}

/* -------------------------------------------------------------------------- */
/* Report generation                                                          */
/* -------------------------------------------------------------------------- */

static void emit_json(const RepoResult *repos, size_t count)
{
    printf("[\n");
    for (size_t i = 0; i < count; ++i) {
        const RepoResult *r = &repos[i];
        printf("  {\n"
               "    \"path\": \"%s\",\n"
               "    \"scores\": {\n"
               "      \"readme\": %d,\n"
               "      \"license\": %d,\n"
               "      \"tests\": %d,\n"
               "      \"ci\": %d,\n"
               "      \"charter\": %d,\n"
               "      \"abstraction\": %d,\n"
               "      \"state\": %d\n"
               "    },\n"
               "    \"total\": %d,\n"
               "    \"grade\": \"%s\"\n"
               "  }%s\n",
               r->path,
               r->scores.readme, r->scores.license, r->scores.tests,
               r->scores.ci, r->scores.charter, r->scores.abstraction,
               r->scores.state,
               r->total,
               r->grade,
               (i + 1 == count) ? "" : ",");
    }
    printf("]\n");
}

static void emit_markdown(const RepoResult *repos, size_t count)
{
    printf("| Repository | Score | Grade |\n");
    printf("|------------|-------|-------|\n");
    for (size_t i = 0; i < count; ++i) {
        const RepoResult *r = &repos[i];
        printf("| %s | %d/21 | %s |\n", r->path, r->total, r->grade);
    }
}

/* -------------------------------------------------------------------------- */
/* Main entry point                                                            */
/* -------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <command> [options]\n"
                "Commands:\n"
                "  check <repo-path>\n"
                "  scan  <org-path>\n"
                "  score <repo-path>\n"
                "  fix   <repo-path>\n"
                "  report <directory> [--format json|markdown]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    const char *cmd = argv[1];

    /* ---------------------------------------------------------------------- */
    if (strcmp(cmd, "check") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s check <repo-path>\n", argv[0]);
            return EXIT_FAILURE;
        }
        RepoResult r;
        evaluate_repo(argv[2], &r);
        printf("Repository: %s\n", r.path);
        printf("  README       : %d\n", r.scores.readme);
        printf("  LICENSE      : %d\n", r.scores.license);
        printf("  Tests        : %d\n", r.scores.tests);
        printf("  CI workflow  : %d\n", r.scores.ci);
        printf("  CHARTER.md   : %d\n", r.scores.charter);
        printf("  ABSTRACTION.md: %d\n", r.scores.abstraction);
        printf("  STATE.md     : %d\n", r.scores.state);
        printf("Total score: %d/21 – %s\n", r.total, r.grade);
        return EXIT_SUCCESS;
    }

    /* ---------------------------------------------------------------------- */
    if (strcmp(cmd, "score") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s score <repo-path>\n", argv[0]);
            return EXIT_FAILURE;
        }
        RepoResult r;
        evaluate_repo(argv[2], &r);
        printf("%d\n", r.total);
        return EXIT_SUCCESS;
    }

    /* ---------------------------------------------------------------------- */
    if (strcmp(cmd, "fix") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s fix <repo-path>\n", argv[0]);
            return EXIT_FAILURE;
        }
        printf("Auto‑fixing repository %s\n", argv[2]);
        fix_repo(argv[2]);
        return EXIT_SUCCESS;
    }

    /* ---------------------------------------------------------------------- */
    if (strcmp(cmd, "scan") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s scan <org-path>\n", argv[0]);
            return EXIT_FAILURE;
        }
        /* nftw will walk the directory tree; we only care about depth‑1 dirs */
        if (nftw(argv[2], scan_callback, 10, FTW_PHYS) == -1) {
            perror("nftw");
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    /* ---------------------------------------------------------------------- */
    if (strcmp(cmd, "report") == 0) {
        if (argc < 3 || argc > 5) {
            fprintf(stderr,
                    "Usage: %s report <directory> [--format json|markdown]\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
        const char *root = argv[2];
        const char *format = "markdown";   /* default */
        if (argc >= 5 && strcmp(argv[3], "--format") == 0) {
            format = argv[4];
            if (strcmp(format, "json") != 0 && strcmp(format, "markdown") != 0) {
                fprintf(stderr, "Invalid format: %s (use json or markdown)\n", format);
                return EXIT_FAILURE;
            }
        }

        /* Collect results */
        RepoResult *list = NULL;
        size_t count = 0, capacity = 0;

        DIR *d = opendir(root);
        if (!d) {
            perror("opendir");
            return EXIT_FAILURE;
        }
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char subpath[MAX_PATH];
            snprintf(subpath, sizeof(subpath), "%s/%s", root, e->d_name);
            if (!dir_exists(subpath)) continue;

            if (count == capacity) {
                capacity = capacity ? capacity * 2 : 8;
                list = realloc(list, capacity * sizeof(RepoResult));
                if (!list) {
                    perror("realloc");
                    closedir(d);
                    return EXIT_FAILURE;
                }
            }
            evaluate_repo(subpath, &list[count++]);
        }
        closedir(d);

        if (strcmp(format, "json") == 0)
            emit_json(list, count);
        else
            emit_markdown(list, count);

        free(list);
        return EXIT_SUCCESS;
    }

    /* ---------------------------------------------------------------------- */
    fprintf(stderr, "Unknown command: %s\n", cmd);
    return EXIT_FAILURE;
}
```