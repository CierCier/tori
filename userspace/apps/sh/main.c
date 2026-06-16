#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

static void print(const char* s) {
    write(1, s, strlen(s));
}

static void print_dec(unsigned long v) {
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    else { while (v && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; } }
    print(buf + i);
}

static int run_command(const char* cmd) {
    pid_t pid = fork();
    if (pid < 0) {
        print("sh: fork failed\n");
        return -1;
    }
    if (pid == 0) {
        execv(cmd, NULL);
        print("sh: exec failed: ");
        print(cmd);
        print("\n");
        _Exit(1);
    }
    int status;
    pid_t ret = waitpid(pid, &status, 0);
    if (ret > 0) {
        return status;
    }
    return -1;
}

#define LINE_MAX 256

int main(int argc, char** argv, char** envp) {
    (void)argc; (void)argv; (void)envp;
    char line[LINE_MAX];

    print("Tori sh v0.1\n");

    for (;;) {
        print("$ ");

        ssize_t n = read(0, line, LINE_MAX - 1);
        if (n <= 0) continue;

        line[n] = '\0';

        // Remove trailing newline
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[n - 1] = '\0';
            --n;
        }

        // Skip empty lines
        if (n == 0) continue;

        if (strcmp(line, "exit") == 0) break;

        if (strcmp(line, "help") == 0) {
            print("Builtins: exit, help, ps\n");
            print("Other commands: /sys/<name>\n");
            continue;
        }

        run_command(line);
    }

    print("sh: exiting\n");
    _Exit(0);
}
