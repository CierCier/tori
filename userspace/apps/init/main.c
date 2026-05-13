#include <unistd.h>

int main(int argc, char** argv, char** envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    const char msg[] = "Hello from init!\n";
    write(1, msg, sizeof(msg) - 1);
    _Exit(0);
}
