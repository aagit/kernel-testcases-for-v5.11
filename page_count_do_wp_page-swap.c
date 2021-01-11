// SPDX-License-Identifier: GPL-3.0-or-later
/*
 *  reproducer for v5.11 (still works on v5.15-rc3) memory corruption
 *  with page_count instead of mapcount in do_wp_page with only
 *  O_DIRECT read and swap.
 *
 *  Copyright (C) 2021  Red Hat, Inc.
 *
 *  gcc -O2 -o page_count_do_wp_page-swap page_count_do_wp_page-swap.c -lpthread
 *  ./page_count_do_wp_page-swap ./whateverfile
 *
 *  NOTE: swap must be enabled.
 *
 *  This is caused by the VM design flaw introduced in commit
 *  09854ba94c6aad7886996bfbee2530b3d8a7f4f4.
 *
 *  Fixed in https://github.com/aagit/aa/tree/mapcount_unshare
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

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

	/*
	 * This is not specific to O_DIRECT. Even if O_DIRECT was
	 * forced to use PAGE_SIZE minimum granularity for reads
	 * (which would break userland programs in a noticable way
	 * especially for archs with PAGE_SIZE much bigger than 4k), a
	 * recvmsg would create the same issue since it also use
	 * iov_iter_get_pages internally to create transient GUP pins
	 * on anon memory.
	 */
	int fd = open(argv[1], O_DIRECT|O_CREAT|O_RDWR|O_TRUNC, 0600);
	if (fd < 0)
		perror("open"), exit(1);
	if (write(fd, mem, PAGE_SIZE) != PAGE_SIZE)
		perror("write"), exit(1);

	FILE *file = fopen("/proc/meminfo", "r");
	if (!file)
		perror("fopen meminfo"), exit(1);

	char *line = NULL;
	size_t len = 0;
	unsigned long mem_free = 0, swap_total = 0, swap_free = 0;
	int match = 0;
	while (getline(&line, &len, file) > 0) {
		if (sscanf(line, "MemFree: %lu kB", &mem_free))
			match++;
		if (sscanf(line, "SwapTotal: %lu kB", &swap_total))
			match++;
		if (sscanf(line, "SwapFree: %lu kB", &swap_free)) {
			match++;
			break;
		}
	}
	if (match != 3 || swap_free > swap_total || !mem_free)
		fprintf(stderr, "/proc/meminfo error\n"), exit(1);
	if (!swap_total || !swap_free)
		fprintf(stderr, "not enough swap\n"), exit(1);
	unsigned long size = (swap_free * 3 / 4 + mem_free) * 1024;
	printf("Will allocate %lu MiB in order to swap\n", size / 1024);

	pthread_t pageout;
	if (pthread_create(&pageout, NULL, background_pageout, mem))
		perror("pthread_create pageout"), exit(1);

	pthread_t swap;
	if (pthread_create(&swap, NULL, background_swap, (void *)size))
		perror("pthread_create swap"), exit(1);

	pthread_t thread;
	if (pthread_create(&thread, NULL, writer, mem))
		perror("pthread_create writer"), exit(1);

	bool skip_memset = true;
	while (1) {
		if (pread(fd, mem, HARDBLKSIZE, 0) != HARDBLKSIZE)
			perror("read"), exit(1);
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
			} else
				printf("memory corruption detected\n");
		}
		skip_memset = !skip_memset;
		if (!skip_memset)
			memset(mem, 0xff, HARDBLKSIZE);
	}

	return 0;
}
