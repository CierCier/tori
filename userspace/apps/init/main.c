#include <unistd.h>

#include <unistd.h>

void print(const char *s) {
  size_t len = 0;
  while (s[len])
    len++;
  write(1, s, len);
}

void print_hex(unsigned long n) {
  char buf[17];
  buf[16] = '\0';
  for (int i = 15; i >= 0; i--) {
    int d = n & 0xF;
    buf[i] = d < 10 ? '0' + d : 'a' + d - 10;
    n >>= 4;
  }
  print("0x");
  print(buf);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  print("Tori init started\n");

  print("PID: ");
  print_hex(getpid());
  print("\n");
  print("PPID: ");
  print_hex(getppid());
  print("\n");

  print("Forking...\n");
  pid_t pid = fork();

  if (pid < 0) {
    print("Fork failed\n");
  } else if (pid == 0) {
    print("Child: PID=");
    print_hex(getpid());
    print(", PPID=");
    print_hex(getppid());
    print("\n");
    print("Child: exiting\n");
    _Exit(42);
  } else {
    print("Parent: forked child ");
    print_hex(pid);
    print("\n");
    int status = 0;
    print("Parent: waiting for child...\n");
    pid_t waited = waitpid(pid, &status, 0);
    print("Parent: waitpid returned ");
    print_hex(waited);
    print(", status=");
    print_hex(status);
    print("\n");

    if (waited == pid && status == 42) {
      print("Fork/Waitpid test PASSED\n");
    } else {
      print("Fork/Waitpid test FAILED\n");
    }
  }

  print("Init: entering infinite loop\n");
  for (;;) {
    // We could yield or just loop
  }
}
