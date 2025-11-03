#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>

__attribute__((noinline)) void _serverLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[512];

    va_start(ap, fmt);
    // Escape to vsnprintf! all pointers passed into the vararg part of _serverLog must be escaped!
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    write(1, msg, strlen(msg));
}

int main(void) {

  int *x = malloc(sizeof(int));
  _serverLog(4, "Test log: %d %p\n", 42, x);
  return 0;
}