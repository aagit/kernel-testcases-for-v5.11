// SPDX-License-Identifier: GPL-3.0-or-later
/*
 *  reproducer for v5.11 (still works on v5.15-rc3) memory corruption
 *  with page_count instead of mapcount in do_wp_page with only
 *  io uring fixed read and swap.
 *
 *  Copyright (C) 2021  Red Hat, Inc.
 *
 *  gcc -O2 -o io_uring_swap io_uring_swap.c -lpthread -luring
 *  ./io_uring_swap ./whateverfile
 *
 *  NOTE: swap must be enabled. The smaller the total memory in the system
 *  the easier it is to reproduce. Inside a 2 GiB VM it triggers fairly
 *  reliably within minutes.
 *
 *  This is caused by the VM design flaw introduced in commit
 *  09854ba94c6aad7886996bfbee2530b3d8a7f4f4.
 *
 *  Fixed in https://gitlab.com/aarcange/aa/-/tree/mapcount_unshare
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/errno.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include "liburing.h"

#define PAGE_SIZE (1UL<<12)
/*
 * NOTE: an arch with a PAGE_SIZE > 4k will reproduce the silent mm
 * corruption with an HARDBLKSIZE of 4k or more.
 */
#define HARDBLKSIZE 512

static void* writer(void *_mem)
{
	volatile char *mem = (char *)_mem;
	char x;
	for(;;) {
		usleep(random() % 1000);
		x = mem[PAGE_SIZE-1];
		mem[PAGE_SIZE-1] = x;
	}
	return NULL;
}

static void* background_pageout(void *_mem)
{
	char *mem = (char *)_mem;
	for(;;) {
		usleep(random() % 1000);
		madvise(mem, PAGE_SIZE, MADV_PAGEOUT);
	}
	return NULL;
}

static void* background_swap(void *_size)
{
	unsigned long size = (unsigned long) _size;
	for (;;) {
		volatile char *p = malloc(size);
		if (!p)
			perror("malloc"), exit(1);
		for (unsigned long i = 0; i < size; i += PAGE_SIZE) {
			p[i] = 0;
		}
		free((void *)p);
	}
	return NULL;
}

static int io_uring_read_fixed(struct io_uring *ring, int fd, void *buf,
			       size_t size)
{
	static unsigned long count;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct iovec iov;
	int ret, res;

	printf("Reading attempt #%lu\n", ++count);

	iov.iov_base = buf;
	iov.iov_len = size;

	/*
	 * Map the buffer, this will FOLL_PIN | FOLL_LONGTERM the target page.
	 * If we happen to pin just after putting the page into the swap cache
	 * and before unmapping it, we can be in trouble.
	 */
	ret = io_uring_register_buffers(ring, &iov, 1);
	if (ret) {
		fprintf(stderr, "io_uring_register_buffers() failed: %d\n",
			ret);
		return ret;
	}

	/*
	 * Let's wait a bit until actually reading the content, such that any
	 * wrong COW will see stale data.
	 */
	usleep(random() % 1000);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "io_uring_get_sqe() failed: %d\n", ret);
		return ret;
	}
	io_uring_prep_read_fixed(sqe, fd, buf, size, 0, 0);

	ret = io_uring_submit(ring);
	if (ret < 0) {
		fprintf(stderr, "io_uring_submit() failed: %d\n", ret);
		return ret;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "io_uring_wait_cqe() failed: %d\n", ret);
		return ret;
	}

	res = cqe->res;
	io_uring_cqe_seen(ring, cqe);

	/*
	 * Unmap the buffer, this will unpin the target page. Unfortunately,
	 * this might take a long time.
	 */
	ret = io_uring_unregister_buffers(ring);
	if (ret) {
		fprintf(stderr, "io_uring_unregister_buffers()\n");
		return ret;
	}
	return res;
}

int main(int argc, char *argv[])
{
	if (argc < 2)
		printf("%s <filename>\n", argv[0]), exit(1);

	char *mem;
	if (posix_memalign((void **)&mem, PAGE_SIZE, PAGE_SIZE*3))
		perror("posix_memalign"), exit(1);

	/* THP is not using page_count so it would not corrupt memory */
	if (madvise(mem, PAGE_SIZE, MADV_NOHUGEPAGE))
		perror("madvise"), exit(1);

	bzero(mem, PAGE_SIZE * 3);
	memset(mem + PAGE_SIZE * 2, 0xff, HARDBLKSIZE);

	int fd = open(argv[1], O_CREAT|O_RDWR|O_TRUNC, 0600);
	if (fd < 0)
		perror("open"), exit(1);
	if (write(fd, mem, PAGE_SIZE) != PAGE_SIZE)
		perror("write"), exit(1);

	FILE *file = fopen("/proc/meminfo", "r");
	if (!file)
		perror("fopen meminfo"), exit(1);

	char *line = NULL;
	size_t len = 0;
	unsigned long mem_total = 0, mem_avail = 0, swap_total = 0, swap_free = 0;
	int match = 0;
	while (getline(&line, &len, file) > 0) {
		if (sscanf(line, "MemTotal: %lu kB", &mem_total))
			match++;
		if (sscanf(line, "MemAvailable: %lu kB", &mem_avail))
			match++;
		if (sscanf(line, "SwapTotal: %lu kB", &swap_total))
			match++;
		if (sscanf(line, "SwapFree: %lu kB", &swap_free)) {
			match++;
			break;
		}
	}

	/* Consume an additional 1 GiB */
	unsigned long size_kb = mem_total + 1024*1024;

	if (match != 4 || swap_free > swap_total)
		fprintf(stderr, "/proc/meminfo error\n"), exit(1);
	if (!swap_total || !swap_free || swap_free < size_kb - mem_avail)
		fprintf(stderr, "not enough swap\n"), exit(1);

	unsigned long size = size_kb * 1024;
	printf("Will allocate %lu MiB in order to swap\n", size / 1024 / 1024);

	pthread_t pageout;
	if (pthread_create(&pageout, NULL, background_pageout, mem))
		perror("pthread_create pageout"), exit(1);

	pthread_t swap;
	if (pthread_create(&swap, NULL, background_swap, (void *)size))
		perror("pthread_create swap"), exit(1);

	pthread_t thread;
	if (pthread_create(&thread, NULL, writer, mem))
		perror("pthread_create writer"), exit(1);

	struct io_uring ring;
	int ret = io_uring_queue_init(1, &ring, 0);
	if (ret < 0) {
		perror("io_uring_queue_init");
		exit(ret);
	}

	bool skip_memset = true;
	while (1) {
		if (io_uring_read_fixed(&ring, fd, mem, HARDBLKSIZE) != HARDBLKSIZE) {
			fprintf(stderr, "io_uring_read_fixed() failed\n");
			exit(-1);
		}
		if (memcmp(mem, mem+PAGE_SIZE, HARDBLKSIZE)) {
			if (memcmp(mem, mem+PAGE_SIZE*2, PAGE_SIZE)) {
				if (skip_memset)
					printf("unexpected memory "
					       "corruption detected\n");
				else
					printf("memory corruption detected, "
					       "dumping page\n");
				int end = PAGE_SIZE;
				if (!memcmp(mem+HARDBLKSIZE, mem+PAGE_SIZE,
					    PAGE_SIZE-HARDBLKSIZE))
					end = HARDBLKSIZE;
				for (int i = 0; i < end; i++)
					printf("%x", mem[i]);
				printf("\n");
			} else {
				printf("memory corruption detected\n");
				exit(-1);
			}
		}
		skip_memset = !skip_memset;
		if (!skip_memset)
			memset(mem, 0xff, HARDBLKSIZE);
	}

	return 0;
}
