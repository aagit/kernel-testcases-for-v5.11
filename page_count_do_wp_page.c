// SPDX-License-Identifier: GPL-3.0-or-later
/*
 *  reproducer for v5.11 (still works on v5.15-rc3) memory corruption
 *  with page_count instead of mapcount in do_wp_page with O_DIRECT
 *  read and clear_refs.
 *
 *  Copyright (C) 2021  Red Hat, Inc.
 *
 *  gcc -O2 -o page_count_do_wp_page page_count_do_wp_page.c -lpthread
 *  ./page_count_do_wp_page ./whateverfile
 *
 *  NOTE: CONFIG_SOFT_DIRTY=y is required in the kernel config.
 *
 *  This is caused by the VM design flaw introduced in commit
 *  09854ba94c6aad7886996bfbee2530b3d8a7f4f4.
 *
 *  The approach of skipping wrprotection on GUP pinned pages, is not
 *  applicable to mprotect() concurrent with an O_DIRECT write(),
 *  because such case has a deterministic result no matter if the
 *  write() is using O_DIRECT or buffered I/O. In addition there are
 *  false positives possible in the check if the page is GUP pinned.
 *
 *  Copying any GUP pinned page within mprotect was also suggested, but it
 *  would stll break coherency of readonly long term GUP pins.
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
	char *mem = (char *)_mem;
	for(;;) {
		usleep(random() % 1000);
		mem[PAGE_SIZE-1] = 0;
	}
	return NULL;
}

static void* background_soft_dirty(void *data)
{
	long fd = (long) data;
	for (;;)
		if (write(fd, "4", 1) != 1)
			perror("write soft dirty"), exit(1);
	return NULL;
}

int main(int argc, char *argv[])
{
	if (argc < 2)
		printf("%s <filename>\n", argv[0]), exit(1);

	char path[PAGE_SIZE];
	strcpy(path, "/proc/");
	sprintf(path + strlen(path), "%d", getpid());
	strcat(path, "/clear_refs");
	long soft_dirty_fd = open(path, O_WRONLY);
	if (soft_dirty_fd < 0)
		perror("open clear_refs"), exit(1);

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

	pthread_t soft_dirty;
	if (pthread_create(&soft_dirty, NULL,
			   background_soft_dirty, (void *)soft_dirty_fd))
		perror("pthread_create soft_dirty"), exit(1);

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
