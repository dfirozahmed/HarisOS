/* HarisOS shell. Start SH.ELF from the File Explorer (or use the Terminal app): it opens a console window and shows a prompt
 * with the working folder.
 *
 *   built in:   cd [FOLDER]   pwd   echo ARGS...   help   exit [CODE]
 *   a | b | c    the output of each command is the input of the next (up to 4 commands; a built in only as the first)
 *   anything else is a program: "ls -l" runs LS.ELF, "./tool" or "/RAM Disk/x.elf" runs that file. The program is started
 *   with SYS_SPAWN, the shell waits for it (SYS_WAIT) and shows "[exit N]" when the code is not 0.
 *
 *   redirection (on built in commands too):   cmd > file    the output goes to the file (made, or emptied first)
 *                                             cmd >> file   ... added to the end of the file
 *                                             cmd < file    the program reads the file instead of the keyboard
 *                                             cmd 2> file   the error messages go to the file
 *                                             cmd 2>&1      ... to wherever the output goes
 *
 * A word with spaces goes in double quotes: cd "/RAM Disk". No variables or wildcards yet. */
#include "libc.h"

#define LINE_MAX 256
#define ARGS_MAX 32
#define STORE_MAX (LINE_MAX + ARGS_MAX * 2 + 8) /* the words of a line, each with its NUL: see split() */

enum { OP_NONE = 0, OP_IN, OP_OUT, OP_APPEND, OP_ERR, OP_ERR_APPEND, OP_ERR_TO_OUT, OP_PIPE };

typedef struct
{
    char *text;
    int op; /* OP_NONE = a word; else the redirection operator (the file name is the next token) */
} Token;

static int is_blank(char c) { return c == ' ' || c == '\t'; }

/* Splits `line` into tokens, copying the words into `store` (STORE_MAX bytes: each token costs at most its own length + 1);
 * returns how many tokens (at most ARGS_MAX), or -1 if there are more. A double quote starts / ends a run in which spaces and
 * < > | belong to the word; the quote characters themselves are dropped. An unquoted < > ends the word before it and is a token
 * of its own: "<", ">", ">>", "|", and "2>", "2>>", "2>&1" when the 2 stands alone right in front of the >. */
static int split(const char *line, char *store, Token *tok)
{
    int n = 0;
    char *w = store;
    const char *p = line;
    while (*p)
    {
        while (is_blank(*p))
            p++;
        if (!*p)
            break;
        if (n == ARGS_MAX)
            return -1;
        if (*p == '<' || *p == '>' || *p == '|') /* an operator with no word in front */
        {
            tok[n].text = w;
            *w++ = 0;
            if (*p == '|')
            {
                tok[n].op = OP_PIPE;
                p++;
            }
            else if (*p == '<')
            {
                tok[n].op = OP_IN;
                p++;
            }
            else if (p[1] == '>')
            {
                tok[n].op = OP_APPEND;
                p += 2;
            }
            else
            {
                tok[n].op = OP_OUT;
                p++;
            }
            n++;
            continue;
        }
        char *start = w;
        int quoted = 0;
        while (*p && (quoted || !(is_blank(*p) || *p == '<' || *p == '>' || *p == '|')))
        {
            if (*p == '"')
                quoted = !quoted;
            else
                *w++ = *p;
            p++;
        }
        *w++ = 0;
        tok[n].text = start;
        tok[n].op = OP_NONE;
        if (start[0] == '2' && start[1] == 0 && p[-1] == '2' && *p == '>') /* "2>", "2>>", "2>&1" */
        {
            if (p[1] == '&' && p[2] == '1')
            {
                tok[n].op = OP_ERR_TO_OUT;
                p += 3;
            }
            else if (p[1] == '>')
            {
                tok[n].op = OP_ERR_APPEND;
                p += 2;
            }
            else
            {
                tok[n].op = OP_ERR;
                p++;
            }
            start[0] = 0;
        }
        n++;
    }
    return n;
}

static int ends_with_elf(const char *s)
{
    usize n = strlen(s);
    return n >= 4 && s[n - 4] == '.' && tolower(s[n - 3]) == 'e' && tolower(s[n - 2]) == 'l' && tolower(s[n - 1]) == 'f';
}

/* What one command of a line asks for: the words, and where fd 0, 1, 2 should go. */
typedef struct
{
    char *argv[ARGS_MAX + 1];
    int argc;
    const char *in, *out, *err;
    int out_append, err_append, err_to_out;
} Command;

#define STAGES_MAX 4 /* commands joined by | */

/* The tokens tok[0..n) (no | among them) into a Command. Returns 0, or -1 after printing why not. */
static int build(const Token *tok, int n, Command *c)
{
    c->argc = 0;
    c->in = c->out = c->err = 0;
    c->out_append = c->err_append = c->err_to_out = 0;
    for (int i = 0; i < n; i++)
    {
        if (tok[i].op == OP_NONE)
        {
            c->argv[c->argc++] = tok[i].text;
            continue;
        }
        if (tok[i].op == OP_ERR_TO_OUT)
        {
            c->err_to_out = 1;
            continue;
        }
        if (i + 1 >= n || tok[i + 1].op != OP_NONE)
        {
            puts("sh: a file name must follow < > >> 2>");
            return -1;
        }
        const char *file = tok[++i].text;
        switch (tok[i - 1].op)
        {
        case OP_IN:
            c->in = file;
            break;
        case OP_OUT:
            c->out = file;
            c->out_append = 0;
            break;
        case OP_APPEND:
            c->out = file;
            c->out_append = 1;
            break;
        case OP_ERR:
            c->err = file;
            c->err_append = 0;
            break;
        default:
            c->err = file;
            c->err_append = 1;
            break;
        }
    }
    c->argv[c->argc] = 0;
    if (c->argc == 0)
    {
        puts("sh: a command is missing (before a redirection or around a |)");
        return -1;
    }
    return 0;
}

/* A whole line: up to STAGES_MAX commands. Checks where redirections may be (< only on the first, > only on the last). */
static int build_line(const Token *tok, int n, Command *cmds)
{
    int stages = 0, from = 0;
    for (int i = 0; i <= n; i++)
    {
        if (i < n && tok[i].op != OP_PIPE)
            continue;
        if (stages == STAGES_MAX)
        {
            puts("sh: too many commands joined by |");
            return -1;
        }
        if (build(tok + from, i - from, &cmds[stages]) < 0)
            return -1;
        stages++;
        from = i + 1;
    }
    for (int s = 0; s < stages; s++)
    {
        if (s > 0 && cmds[s].in)
        {
            puts("sh: only the first command of a pipeline can read from a file");
            return -1;
        }
        if (s < stages - 1 && cmds[s].out)
        {
            puts("sh: only the last command of a pipeline can write to a file");
            return -1;
        }
    }
    return stages;
}

/* The files of a command's redirections, opened. fd[k] is a descriptor for the shell's own fd k (0 in, 1 out, 2 err), or -1 =
 * not redirected (the console, or the pipe). Returns 0, or -1 after printing why not (what was opened is closed again). */
static int open_redirects(const Command *c, int *fd)
{
    fd[0] = fd[1] = fd[2] = -1;
    if (c->in && (fd[0] = (int)sys_open(c->in)) < 0)
    {
        printf("sh: cannot read '%s'\n", c->in);
        return -1;
    }
    if (c->out && (fd[1] = (int)sys_open_flags(c->out, SYS_O_WRITE | SYS_O_CREATE | (c->out_append ? SYS_O_APPEND : SYS_O_TRUNC))) < 0)
    {
        printf("sh: cannot write '%s' (is the drive locked? try: lock -u .)\n", c->out);
        if (fd[0] >= 0)
            sys_close(fd[0]);
        fd[0] = -1;
        return -1;
    }
    if (c->err && (fd[2] = (int)sys_open_flags(c->err, SYS_O_WRITE | SYS_O_CREATE | (c->err_append ? SYS_O_APPEND : SYS_O_TRUNC))) < 0)
    {
        printf("sh: cannot write '%s' (is the drive locked? try: lock -u .)\n", c->err);
        for (int k = 0; k < 2; k++)
            if (fd[k] >= 0)
                sys_close(fd[k]);
        fd[0] = fd[1] = -1;
        return -1;
    }
    return 0;
}

static void close_fd(int *fd)
{
    if (*fd >= 0)
        sys_close(*fd);
    *fd = -1;
}

static int is_builtin(const char *name)
{
    return strcmp(name, "exit") == 0 || strcmp(name, "cd") == 0 || strcmp(name, "pwd") == 0 || strcmp(name, "echo") == 0 ||
           strcmp(name, "help") == 0;
}

static void help(int out)
{
    fdprintf(out, "built in: cd [FOLDER]  pwd  echo ARGS  help  exit [CODE]\n");
    fdprintf(out, "drives:   start locked. lock -u .  unlocks the drive you are in;  lock  lists them\n");
    fdprintf(out, "files:    ls [-l] [FOLDER]  cat FILE  cp [-r] A B  mv A B  rm [-r] [-f] PATH  mkdir [-p] DIR  touch FILE\n");
    fdprintf(out, "filters:  upper   (try: cat notes.txt | upper)\n");
    fdprintf(out, "programs: every NAME.ELF on the drive.  Join with  a | b | c   Redirect with  > file  >> file  < file  2> file  2>&1\n");
    fdprintf(out, "quote words with spaces: cd \"/RAM Disk\"\n");
}

/* Runs a built in command; its output goes to descriptor `out`. */
static void builtin(const Command *c, int out, const char *cwd)
{
    char *const *argv = c->argv;
    int argc = c->argc;
    if (strcmp(argv[0], "exit") == 0)
        sys_exit(argc > 1 ? atoi(argv[1]) : 0);
    else if (strcmp(argv[0], "cd") == 0)
    {
        if (argc > 2)
            fdprintf(out, "cd: too many arguments\n");
        else if (sys_chdir(argc == 2 ? argv[1] : "/") < 0)
            fdprintf(out, "cd: no such folder: %s\n", argc == 2 ? argv[1] : "/");
    }
    else if (strcmp(argv[0], "pwd") == 0)
        fdprintf(out, "%s\n", cwd);
    else if (strcmp(argv[0], "echo") == 0)
    {
        for (int i = 1; i < argc; i++)
            fdprintf(out, i > 1 ? " %s" : "%s", argv[i]);
        fdprintf(out, "\n");
    }
    else
        help(out);
}

/* Starts a program with the given descriptors for its fd 0, 1, 2 (-1 = leave alone). Returns the pid or -1 after saying why. */
static long long start(const Command *c, const int *io)
{
    char path[160];
    if (strchr(c->argv[0], '/') || ends_with_elf(c->argv[0]))
        snprintf(path, sizeof(path), "%s", c->argv[0]);
    else
        snprintf(path, sizeof(path), "%s.ELF", c->argv[0]);
    long long pid = sys_spawn_io(path, (const char *const *)c->argv, io);
    if (pid < 0)
        printf("sh: cannot run '%s' (not found, not a program, or too many programs running)\n", c->argv[0]);
    return pid;
}

static void report(long long pid, int is_last)
{
    int status = 0;
    if (sys_wait(pid, &status) < 0)
    {
        printf("sh: lost a program\n");
        return;
    }
    if (status == -1)
        printf("[crashed]\n");
    else if (status != 0 && is_last)
        printf("[exit %d]\n", status);
}

/* One command or a pipeline. The commands run at the same time; a pipe between each two (SYS_PIPE), the first one reads from
 * the keyboard (or "< file"), the last writes to the window (or "> file"). The shell closes its own copies of the pipe ends as
 * soon as the children have theirs, or the reader would never see the end of input. A built in command can only be the first
 * one (it writes into the pipe itself, after the others have started: so that a reader is there to empty it). */
static void run_line(Command *cmds, int n, const char *cwd)
{
    for (int s = 1; s < n; s++)
        if (is_builtin(cmds[s].argv[0]))
        {
            printf("sh: '%s' cannot be in the middle or at the end of a pipeline\n", cmds[s].argv[0]);
            return;
        }
    long long pids[STAGES_MAX];
    int started = 0, failed = 0;
    int prev_read = -1;     /* the read end of the pipe from the previous command */
    int deferred_out = -1;  /* a first built in command in a pipeline: the write end it fills when the others are running */

    for (int s = 0; s < n && !failed; s++)
    {
        int rd[3];
        if (open_redirects(&cmds[s], rd) < 0)
        {
            failed = 1;
            break;
        }
        int io[3] = {rd[0], rd[1], rd[2]};
        int next[2] = {-1, -1};
        if (s < n - 1)
        {
            if (sys_pipe(next) < 0)
            {
                printf("sh: cannot make a pipe (too many files open, or all pipes are in use)\n");
                for (int k = 0; k < 3; k++)
                    close_fd(&rd[k]);
                failed = 1;
                break;
            }
            io[1] = next[1];
        }
        if (s > 0)
            io[0] = prev_read;
        if (cmds[s].err_to_out && !cmds[s].err)
            io[2] = io[1]; /* the same descriptor: the kernel shares one open file between fd 1 and 2 */

        if (is_builtin(cmds[s].argv[0])) /* only possible as the first command */
        {
            if (n == 1) /* alone: run it now, writing where its > says, or to the window */
            {
                builtin(&cmds[0], rd[1] >= 0 ? rd[1] : 1, cwd);
                for (int k = 0; k < 3; k++)
                    close_fd(&rd[k]);
            }
            else /* the reader has to be running before it writes more than a pipe holds: later */
            {
                deferred_out = next[1];
                for (int k = 0; k < 3; k++)
                    close_fd(&rd[k]);
            }
            prev_read = next[0];
            continue;
        }

        long long pid = start(&cmds[s], io);
        /* the child has its own copies of every descriptor it was given: ours are closed, or the reader of a pipe would never
         * see the end of input */
        for (int k = 0; k < 3; k++)
            close_fd(&rd[k]);
        close_fd(&prev_read);
        close_fd(&next[1]);
        if (pid < 0)
        {
            close_fd(&next[0]);
            failed = 1;
            break;
        }
        pids[started++] = pid;
        prev_read = next[0];
    }

    if (deferred_out >= 0)
    {
        if (!failed)
            builtin(&cmds[0], deferred_out, cwd);
        close_fd(&deferred_out);
    }
    close_fd(&prev_read);
    for (int i = 0; i < started; i++)
        report(pids[i], i == started - 1);
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    puts("HarisOS shell. 'help' lists the commands.");
    char line[LINE_MAX];
    char cwd[256];
    for (;;)
    {
        if (sys_getcwd(cwd, sizeof(cwd)) < 0)
            strcpy(cwd, "?");
        printf("%s> ", cwd);
        long long n = sys_read(0, line, sizeof(line) - 1);
        if (n <= 0) /* the window was closed: input is gone (init stops when it sees 130) */
            sys_exit(130);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            n--;
        line[n] = 0;
        char store[STORE_MAX];
        Token tok[ARGS_MAX + 1];
        int ntok = split(line, store, tok);
        if (ntok < 0)
        {
            puts("sh: too many words");
            continue;
        }
        if (ntok == 0)
            continue;
        Command cmds[STAGES_MAX];
        int stages = build_line(tok, ntok, cmds);
        if (stages > 0)
            run_line(cmds, stages, cwd);
    }
}
