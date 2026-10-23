/* HarisOS shell. Start SH.ELF from the File Explorer: it opens a console window and shows a prompt with the working folder.
 *
 *   built in:   cd [FOLDER]   pwd   echo ARGS...   help   exit [CODE]
 *   anything else is a program: "ls -l" runs LS.ELF, "./tool" or "/RAM Disk/x.elf" runs that file. The program is started
 *   with SYS_SPAWN, the shell waits for it (SYS_WAIT) and shows "[exit N]" when the code is not 0.
 *
 * A word with spaces goes in double quotes: cd "/RAM Disk". No pipes, redirection, variables or wildcards yet. */
#include "libc.h"

#define LINE_MAX 256
#define ARGS_MAX 32

/* Splits `line` in place into words; returns how many (at most ARGS_MAX). A quote starts / ends a run in which spaces
 * belong to the word; the quote characters themselves are removed. */
static int split(char *line, char **argv)
{
    int argc = 0;
    char *p = line;
    while (*p)
    {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (argc == ARGS_MAX)
            return -1;
        argv[argc++] = p;
        char *out = p;
        int quoted = 0;
        while (*p && (quoted || (*p != ' ' && *p != '\t')))
        {
            if (*p == '"')
                quoted = !quoted;
            else
                *out++ = *p;
            p++;
        }
        int more = *p != 0;
        *out = 0;
        if (more)
            p++;
    }
    argv[argc] = 0;
    return argc;
}

static int ends_with_elf(const char *s)
{
    usize n = strlen(s);
    return n >= 4 && s[n - 4] == '.' && tolower(s[n - 3]) == 'e' && tolower(s[n - 2]) == 'l' && tolower(s[n - 1]) == 'f';
}

static void run(char **argv)
{
    char path[160];
    if (strchr(argv[0], '/') || ends_with_elf(argv[0]))
        snprintf(path, sizeof(path), "%s", argv[0]);
    else
        snprintf(path, sizeof(path), "%s.ELF", argv[0]);
    long long pid = sys_spawn(path, (const char *const *)argv);
    if (pid < 0)
    {
        printf("sh: cannot run '%s' (not found, or not a program)\n", argv[0]);
        return;
    }
    int status = 0;
    if (sys_wait(pid, &status) < 0)
    {
        printf("sh: lost the program\n");
        return;
    }
    if (status == -1)
        printf("[crashed]\n");
    else if (status != 0)
        printf("[exit %d]\n", status);
}

static void help(void)
{
    puts("built in: cd [FOLDER]  pwd  echo ARGS  help  exit [CODE]");
    puts("programs: ls [-l] [FOLDER]  cat FILE  and every NAME.ELF on the drive");
    puts("quote words with spaces: cd \"/RAM Disk\"");
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
        char *argv[ARGS_MAX + 1];
        int argc = split(line, argv);
        if (argc < 0)
        {
            puts("sh: too many words");
            continue;
        }
        if (argc == 0)
            continue;

        if (strcmp(argv[0], "exit") == 0)
            sys_exit(argc > 1 ? atoi(argv[1]) : 0);
        else if (strcmp(argv[0], "cd") == 0)
        {
            if (argc > 2)
                puts("cd: too many arguments");
            else if (sys_chdir(argc == 2 ? argv[1] : "/") < 0)
                printf("cd: no such folder: %s\n", argc == 2 ? argv[1] : "/");
        }
        else if (strcmp(argv[0], "pwd") == 0)
            puts(cwd);
        else if (strcmp(argv[0], "echo") == 0)
        {
            for (int i = 1; i < argc; i++)
                printf(i > 1 ? " %s" : "%s", argv[i]);
            putchar('\n');
        }
        else if (strcmp(argv[0], "help") == 0)
            help();
        else
            run(argv);
    }
}
