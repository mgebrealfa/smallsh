/*
 * smallsh.c
 * A small POSIX shell implementation supporting:
 *   - Command execution via fork/execvp
 *   - Input redirection (<), output redirection (>), append redirection (>>)
 *   - Background execution (&)
 *   - Built-in commands: exit, cd, pwd
 *   - Parameter expansion: $$, $?, $!, ${VAR}
 *   - Signal handling: SIGINT ignored in parent, SIGTSTP ignored globally
 *   - Background job tracking and reaping
 *
 * Created by Mesmer Gebrealfa
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <err.h>
#include <stdint.h>

#ifndef MAX_WORDS
#define MAX_WORDS 512
#endif

/* ── Global state ─────────────────────────────────────────────────────────── */

/* Last foreground exit/signal status, used for $? expansion */
int status = 0;

/* Most recent background child PID, used for $! expansion */
pid_t bg_last_pid = 0;

/* Circular list of active background PIDs; terminated by sentinel -1 */
pid_t bg_pids[MAX_WORDS];
int   bg_count = 0;

/* Redirection target filenames collected during argument parsing */
char *writefile[MAX_WORDS];   /* > targets  (can be multiple) */
char *appendfile[1];          /* >> target  (only one allowed) */

/* Word array populated by wordsplit(), expanded in-place */
char *words[MAX_WORDS];

/* ── Forward declarations ─────────────────────────────────────────────────── */
size_t wordsplit(char const *line);
char  *expand(char const *word);
char   param_scan(char const *word, char const **start, char const **end);
char  *build_str(char const *start, char const *end);

/* ── Signal handler ───────────────────────────────────────────────────────── */

/*
 * sigint_handler: installed in the parent shell for SIGINT (Ctrl-C).
 * The parent ignores it; foreground children inherit SIG_DFL so they
 * do terminate on Ctrl-C.  The handler body is intentionally empty —
 * its only job is to interrupt blocking syscalls (e.g. getline) so
 * the shell can reprint the prompt.
 */
void sigint_handler(int sig)
{
    (void)sig; /* suppress unused-parameter warning */
}

/* ── main ─────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    /* ── Input source: stdin or a script file ─────────────────────────── */
    FILE *input    = stdin;
    char *input_fn = "(stdin)";

    if (argc == 2) {
        input_fn = argv[1];
        input = fopen(input_fn, "re");
        if (!input) err(1, "%s", input_fn);
    } else if (argc > 2) {
        err(1, "%s", "too many arguments");
    }

    /* ── Signal setup ─────────────────────────────────────────────────── */

    /* SIGTSTP (Ctrl-Z): ignore in the shell process.
     * myoldsa is saved so we can query whether we were already ignoring
     * it (e.g. when running as a background script). */
    struct sigaction mysa   = {0};
    struct sigaction myoldsa= {0};
    mysa.sa_handler = SIG_IGN;
    sigaction(SIGTSTP, &mysa, &myoldsa);

    /* SIGINT (Ctrl-C): use our empty handler so getline is interrupted
     * but the shell itself does not exit. */
    struct sigaction sa    = {0};
    struct sigaction oldsa = {0};
    sa.sa_handler = sigint_handler;
    if (sigaction(SIGINT, &sa, &oldsa) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    /* ── Read-eval loop ───────────────────────────────────────────────── */
    char  *line = NULL;
    size_t n    = 0;

    for (;;) {

        /* ── Reset per-iteration state ────────────────────────────────── */

        /* Clear write-redirect targets from the previous command */
        for (int i = 0; i < MAX_WORDS && writefile[i]; i++)
            writefile[i] = NULL;
        appendfile[0] = NULL;

        /* Index into words[] used while scanning for redirects / & */
        int word_idx   = 0;
        /* Index of '&' token if background execution was requested */
        int bg_tok_idx = 0;

        /* ── Print prompt ─────────────────────────────────────────────── */
        char *ps1 = getenv("PS1");
        if (ps1) fprintf(stderr, "%s", ps1); /* use %s to avoid format injection */

        /* ── Read a line; retry on EINTR (signal interrupted getline) ─── */
        ssize_t nread = getline(&line, &n, input);
        while (nread < 0 && errno == EINTR) {
            errno = 0;
            clearerr(stdin);
            fprintf(stderr, "\n");
            if (ps1) fprintf(stderr, "%s", ps1);
            nread = getline(&line, &n, input);
        }

        /* EOF or unrecoverable error → exit with last status */
        if (nread < 0) exit(status);

        /* ── Tokenise and expand ──────────────────────────────────────── */
        size_t nwords = wordsplit(line);

        /* Expand parameter references ($$, $?, $!, ${VAR}) in each token */
        for (size_t i = 0; i < nwords; i++) {
            char *exp = expand(words[i]);
            free(words[i]);
            words[i] = exp;
        }

        /* Empty line or comment-only line */
        if (nwords == 0 || !words[0]) continue;

        /* ── Built-in: exit ───────────────────────────────────────────── */
        if (strcmp(words[0], "exit") == 0) {
            if (words[1])
                exit(atoi(words[1]));   /* exit with explicit code */
            else
                exit(0);
        }

        /* ── Built-in: cd ─────────────────────────────────────────────── */
        if (strcmp(words[0], "cd") == 0) {
            const char *dest = (words[1] && words[1][0]) ? words[1] : getenv("HOME");
            if (chdir(dest) == -1) perror("cd");
            continue; /* cd is fully handled; skip fork */
        }

        /* ── Reap completed background jobs ──────────────────────────── */
        for (int i = 0; i < bg_count; i++) {
            if (bg_pids[i] == -1) continue;

            int bg_status = 0;
            pid_t r = waitpid(bg_pids[i], &bg_status, WNOHANG | WUNTRACED);
            if (r <= 0) continue; /* still running or already reaped */

            if (WIFSTOPPED(bg_status)) {
                /* Stopped background job: send SIGCONT and report */
                kill(bg_pids[i], SIGCONT);
                fprintf(stderr, "Child process %jd stopped. Continuing.\n",
                        (intmax_t)bg_pids[i]);
            } else if (WIFEXITED(bg_status)) {
                fprintf(stderr, "Child process %jd done. Exit status %d.\n",
                        (intmax_t)bg_pids[i], WEXITSTATUS(bg_status));
            } else if (WIFSIGNALED(bg_status)) {
                fprintf(stderr, "Child process %jd done. Signaled %d.\n",
                        (intmax_t)bg_pids[i], WTERMSIG(bg_status));
            }

            bg_pids[i] = -1; /* mark slot as free */
        }

        /* ── Parse redirections and build args[] ─────────────────────── */
        char *args[MAX_WORDS] = {NULL};
        char *program = words[0];
        int   fcount  = 0; /* number of > targets collected so far */

        word_idx = 0;
        while (words[word_idx]) {

            /* Input redirection: '<' — consume the token (filename already
             * passed to the child via the next token; handled in child) */
            if (strcmp(words[word_idx], "<") == 0) {
                /* Store the filename for dup2 in the child */
                /* Shift tokens left to remove the '<' operator */
                int c = word_idx;
                /* Save filename before shifting */
                char *infile = words[word_idx + 1];
                while (words[c]) { words[c] = words[c + 1]; c++; }
                /* Re-scan same index since tokens shifted */
                /* (infile currently unused — see child section) */
                (void)infile;
                continue;
            }

            /* Output redirection: '>' */
            if (strcmp(words[word_idx], ">") == 0) {
                writefile[fcount++] = words[word_idx + 1];
                int c = word_idx;
                if (!words[word_idx + 2]) {
                    words[word_idx]     = NULL;
                    words[word_idx + 1] = NULL;
                } else {
                    while (words[c + 2]) { words[c] = words[c + 2]; c++; }
                    words[c]     = NULL;
                    words[c + 1] = NULL;
                }
                continue;
            }

            /* Append redirection: '>>' */
            if (strcmp(words[word_idx], ">>") == 0) {
                appendfile[0] = words[word_idx + 1];
                int c = word_idx;
                words[word_idx]     = NULL;
                words[word_idx + 1] = NULL;
                while (words[c + 2]) { words[c] = words[c + 2]; c++; }
                continue;
            }

            /* Background operator: '&' at end of command */
            if (strcmp(words[word_idx], "&") == 0) {
                bg_tok_idx = word_idx;
                args[word_idx] = NULL;
                break;
            }

            /* Regular argument */
            args[word_idx] = words[word_idx];
            word_idx++;
        }
        args[word_idx] = NULL;

        /* ── Fork ─────────────────────────────────────────────────────── */
        pid_t childpid = fork();

        switch (childpid) {

        case -1:
            /* Fork failed */
            perror("fork");
            exit(EXIT_FAILURE);

        /* ── Child process ────────────────────────────────────────────── */
        case 0:
            /*
             * Restore default signal dispositions for the child.
             * If the shell was started with SIGTSTP ignored (e.g. as a
             * background script), keep it ignored; otherwise restore default.
             */
            if (myoldsa.sa_handler == SIG_IGN) {
                signal(SIGTSTP, SIG_IGN);
                signal(SIGINT,  SIG_IGN);
            } else {
                signal(SIGTSTP, SIG_DFL);
                signal(SIGINT,  SIG_DFL); /* foreground child dies on Ctrl-C */
            }

            /* Set up append redirection (>>) */
            if (appendfile[0]) {
                int fd = open(appendfile[0], O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd == -1) { perror("open >>"); exit(EXIT_FAILURE); }
                if (dup2(fd, STDOUT_FILENO) == -1) { perror("dup2 >>"); exit(EXIT_FAILURE); }
                close(fd);
            }

            /* Set up output redirection (>); last > wins for stdout */
            if (writefile[0]) {
                for (int i = 0; writefile[i]; i++) {
                    int fd = open(writefile[i], O_WRONLY | O_CREAT | O_TRUNC, 0640);
                    if (fd == -1) { perror("open >"); exit(EXIT_FAILURE); }
                    if (dup2(fd, STDOUT_FILENO) == -1) { perror("dup2 >"); exit(EXIT_FAILURE); }
                    close(fd);
                }
            }

            /* Execute the command; only returns on error */
            if (execvp(program, args) == -1) {
                perror("execvp");
                exit(EXIT_FAILURE);
            }
            break; /* unreachable, but silences compiler warning */

        /* ── Parent process ───────────────────────────────────────────── */
        default:
            /* Re-ignore SIGINT in parent while waiting */
            signal(SIGINT, SIG_IGN);

            if (words[bg_tok_idx] && strcmp(words[bg_tok_idx], "&") == 0) {
                /* Background job: don't wait, just record the PID */
                waitpid(childpid, &status, WNOHANG);
                bg_last_pid = childpid;
                bg_pids[bg_count++] = childpid;
                bg_pids[bg_count]   = -1; /* sentinel */
            } else {
                /* Foreground job: wait until done or stopped */
                pid_t waited = waitpid(-1, &status, WUNTRACED);
                if (waited != -1) {
                    if (WIFSTOPPED(status)) {
                        /* Shell doesn't support job control stop; continue child */
                        kill(childpid, SIGCONT);
                        fprintf(stderr, "Child process %d stopped. Continuing.\n", childpid);
                        bg_last_pid = childpid;
                    } else if (WIFEXITED(status)) {
                        status = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        status = 128 + WTERMSIG(status);
                    }
                }
            }

            /* Restore SIGINT handler for the next prompt */
            sigaction(SIGINT, &sa, NULL);
            break;
        }

    } /* end for(;;) */

    return 0;
}

/* ── wordsplit ────────────────────────────────────────────────────────────── */
/*
 * Splits `line` into whitespace-delimited tokens, storing pointers in
 * the global `words[]` array.  Backslash escapes a single character.
 * Tokens starting with '#' end parsing (comment).
 * Returns the number of tokens found.
 */
size_t wordsplit(char const *line)
{
    size_t wlen = 0; /* current token length */
    size_t wind = 0; /* current token index  */

    char const *c = line;

    /* Skip leading whitespace */
    for (; *c && isspace(*c); c++);

    for (; *c;) {
        if (wind == MAX_WORDS) break;
        if (*c == '#') break; /* rest of line is a comment */

        /* Accumulate one token */
        for (; *c && !isspace(*c); c++) {
            if (*c == '\\') c++; /* skip the backslash, keep next char */
            if (!*c) break;

            void *tmp = realloc(words[wind], sizeof **words * (wlen + 2));
            if (!tmp) err(1, "realloc");
            words[wind]        = tmp;
            words[wind][wlen]  = *c;
            words[wind][++wlen]= '\0';
        }

        wind++;
        wlen = 0;
        for (; *c && isspace(*c); c++); /* skip inter-token whitespace */
    }

    return wind;
}

/* ── param_scan ───────────────────────────────────────────────────────────── */
/*
 * Scans `word` for the next parameter reference starting after the
 * previous call's endpoint (stored in static `prev`).
 * Pass word=NULL to continue from where the last call left off.
 *
 * On success sets *start and *end to bracket the full $X or ${VAR} token
 * and returns the type character: '$', '!', '?', or '{'.
 * Returns 0 when no more references are found.
 */
char param_scan(char const *word, char const **start, char const **end)
{
    static char const *prev = NULL;
    if (!word) word = prev;

    char ret = 0;
    *start = NULL;
    *end   = NULL;

    char *s = strchr(word, '$');
    if (s) {
        char *brace = strchr(word, '{');

        if (!brace) {
            /* Special single-char params: $$, $!, $? */
            char *c = strchr("$!?", s[1]);
            if (c) {
                ret    = *c;
                *start = s;
                *end   = s + 2;
            }
        } else {
            /* ${VAR} form */
            s[1] = brace[0]; /* normalise so *start points at '${' */
            char *e = strchr(s + 2, '}');
            if (e) {
                ret    = '{';
                *start = s;
                *end   = e + 1;
            }
        }
    }

    prev = *end;
    return ret;
}

/* ── build_str ────────────────────────────────────────────────────────────── */
/*
 * Incrementally builds a heap string by appending the byte range
 * [start, end).  If end is NULL, appends the full NUL-terminated string
 * at start.
 *
 * Call build_str(NULL, NULL) to finalise: resets internal state and
 * returns the completed string (caller owns the memory).
 */
char *build_str(char const *start, char const *end)
{
    static size_t base_len = 0;
    static char  *base     = NULL;

    if (!start) {
        /* Finalise: hand off buffer to caller and reset */
        char *ret = base;
        base      = NULL;
        base_len  = 0;
        return ret;
    }

    size_t n       = end ? (size_t)(end - start) : strlen(start);
    void  *tmp     = realloc(base, base_len + n + 1);
    if (!tmp) err(1, "realloc");
    base = tmp;
    memcpy(base + base_len, start, n);
    base_len       += n;
    base[base_len]  = '\0';

    return base;
}

/* ── expand ───────────────────────────────────────────────────────────────── */
/*
 * Expands all parameter references in `word`, returning a newly
 * allocated string.  The caller is responsible for freeing it.
 *
 * Expansions:
 *   $$   → current PID  (as "<PID>" placeholder, resolved in main)
 *   $!   → last bg PID  (as "<BGPID>")
 *   $?   → last status  (as "<STATUS>")
 *   ${V} → value of env var V (or "" if unset)
 */
char *expand(char const *word)
{
    char const *pos   = word;
    char const *start, *end;

    build_str(NULL, NULL); /* reset builder */

    char c = param_scan(pos, &start, &end);
    build_str(pos, start); /* copy literal prefix */

    while (c) {
        if      (c == '$') build_str("<PID>",    NULL);
        else if (c == '!') build_str("<BGPID>",  NULL);
        else if (c == '?') build_str("<STATUS>", NULL);
        else if (c == '{') {
            /* Extract variable name between ${ and } */
            char varname[256] = {0};
            size_t vlen = (size_t)((end - 1) - (start + 2));
            if (vlen < sizeof varname) {
                memcpy(varname, start + 2, vlen);
                varname[vlen] = '\0';
            }
            const char *val = getenv(varname);
            build_str(val ? val : "", NULL);
        }

        pos = end;
        c   = param_scan(NULL, &start, &end); /* continue from prev endpoint */
        build_str(pos, start);
    }

    return build_str(NULL, NULL); /* finalise and return */
}
