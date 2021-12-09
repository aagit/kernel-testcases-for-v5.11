// SPDX-License-Identifier: GPL-3.0-or-later
/*
 *  Attempted reproducer based on io_uring_swap.
 *
 *  Copyright (C) 2021  Red Hat, Inc.
 *
 *  Add "iommu_intel=on" in guest /proc/cmdline and "-machine
 *  q35,kernel-irqchip=split -device intel-iommu,intremap=on" on host qemu.
 *
 *  lspci -n -s 00:01.0
 *  echo 0000:00:01.0 >/sys/bus/pci/devices/0000\:00\:01.0/driver/unbind
 *  modprobe vfio-pci
 *  echo 1234 1111 >/sys/bus/pci/drivers/vfio-pci/new_id
 *
 *  gcc -O2 -o vfio_swap vfio_swap.c -lpthread
 *  ./vfio_swap 0000:00:01.0
 *
 *  Run concurrently with kprobes introduced via bpftrace:
 *
 * bpftrace -e 'kprobe:vfio_pin_pages_remote { @vfio_start[tid] = arg1; } kretprobe:vfio_pin_pages_remote { @vfio_pinned[tid] = retval; } kprobe:wp_page_copy /@vfio_pinned[tid] > 0/ { $x = (struct vm_fault *)arg0; $addr = $x->address; if ($addr >= @vfio_start[tid] && $addr < @vfio_start[tid] + @vfio_pinned[tid] * 4096) { printf("bug\n"); } } kprobe:vfio_unmap_unpin /@vfio_pinned[tid]/ { delete(@vfio_pinned[tid]); }'
 *
 * Fixed in https://github.com/aagit/aa/tree/mapcount_unshare
 *
 * To verify: bpftrace -e 'kprobe:vfio_pin_pages_remote { @vfio_start[tid] = arg1; } kretprobe:vfio_pin_pages_remote { @vfio_pinned[tid] = retval; } kprobe:__wp_page_copy /@vfio_pinned[tid] > 0/ { $x = (struct vm_fault *)arg0; $addr = $x->address; if ($addr >= @vfio_start[tid] && $addr < @vfio_start[tid] + @vfio_pinned[tid] * 4096) { printf("bug\n"); } } kprobe:vfio_unmap_unpin /@vfio_pinned[tid]/ { delete(@vfio_pinned[tid]); }'
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

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <linux/vfio.h>

#define PAGE_SIZE (1UL<<12)
/*
 * NOTE: an arch with a PAGE_SIZE > 4k will reproduce the silent mm
 * corruption with an HARDBLKSIZE of 4k or more.
 */
#define HARDBLKSIZE 512

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

static int get_container(void)
{
	int container = open("/dev/vfio/vfio", O_RDWR);

	if (container < 0)
		fprintf(stderr, "Failed to open /dev/vfio/vfio, %d (%s)\n",
			container, strerror(errno));

	return container;
}

static int get_group(char *name)
{
	int seg, bus, slot, func;
	int ret, group, groupid;
	char path[50], iommu_group_path[50], *group_name;
	struct stat st;
	ssize_t len;
	struct vfio_group_status group_status = {
		.argsz = sizeof(group_status)
	};

	ret = sscanf(name, "%04x:%02x:%02x.%d", &seg, &bus, &slot, &func);
	if (ret != 4) {
		fprintf(stderr, "Invalid device\n");
		return -EINVAL;
	}

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/",
		 seg, bus, slot, func);

	ret = stat(path, &st);
	if (ret < 0) {
		fprintf(stderr, "No such device\n");
		return ret;
	}

	strncat(path, "iommu_group", sizeof(path) - strlen(path) - 1);

	len = readlink(path, iommu_group_path, sizeof(iommu_group_path));
	if (len <= 0) {
		fprintf(stderr, "No iommu_group for device\n");
		return -EINVAL;
	}

	iommu_group_path[len] = 0;
	group_name = basename(iommu_group_path);

	if (sscanf(group_name, "%d", &groupid) != 1) {
		fprintf(stderr, "Unknown group\n");
		return -EINVAL;
	}

	snprintf(path, sizeof(path), "/dev/vfio/%d", groupid);
	group = open(path, O_RDWR);
	if (group < 0) {
		fprintf(stderr, "Failed to open %s, %d (%s)\n",
			path, group, strerror(errno));
		return group;
	}

	ret = ioctl(group, VFIO_GROUP_GET_STATUS, &group_status);
	if (ret) {
		fprintf(stderr, "ioctl(VFIO_GROUP_GET_STATUS) failed\n");
		return ret;
	}

	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		fprintf(stderr,
			"Group not viable, all devices attached to vfio?\n");
		return -1;
	}

	return group;
}

static int group_set_container(int group, int container)
{
	int ret = ioctl(group, VFIO_GROUP_SET_CONTAINER, &container);

	if (ret)
		fprintf(stderr, "Failed to set group container\n");

	return ret;
}

static int container_set_iommu(int container)
{
	int ret = ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);

	if (ret)
		fprintf(stderr, "Failed to set IOMMU\n");

	return ret;
}

static int group_get_device(int group, char *name)
{
	int device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, name);

	if (device < 0)
		fprintf(stderr, "Failed to get device\n");

	return device;
}

static int dma_map(int container, void *map, int size, unsigned long iova)
{
	struct vfio_iommu_type1_dma_map dma_map = {
		.argsz = sizeof(dma_map),
		.size = size,
		.vaddr = (__u64)map,
		.iova = iova,
		.flags = VFIO_DMA_MAP_FLAG_READ
	};
	int ret;

	ret = ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map);
	if (ret)
		fprintf(stderr, "Failed to DMA map: %m\n");

	return ret;
}

static int dma_unmap(int container, int size, unsigned long iova)
{
	struct vfio_iommu_type1_dma_unmap dma_unmap = {
		.argsz = sizeof(dma_unmap),
		.iova = iova,
		.size = size,
	};
	int ret;

	ret = ioctl(container, VFIO_IOMMU_UNMAP_DMA, &dma_unmap);
	if (ret)
		fprintf(stderr, "Failed to DMA unmap: %m\n");

	return dma_unmap.size;
}

int main(int argc, char *argv[])
{
	if (argc < 2)
		printf("%s <PCI device (xxxx:xx:xx.x)>\n",
		       argv[0]), exit(1);

	char *mem;
	if (posix_memalign((void **)&mem, PAGE_SIZE, PAGE_SIZE*3))
		perror("posix_memalign"), exit(1);

	/* THP is not using page_count so it would not corrupt memory */
	if (madvise(mem, PAGE_SIZE, MADV_NOHUGEPAGE))
		perror("madvise"), exit(1);

	bzero(mem, PAGE_SIZE * 3);
	memset(mem + PAGE_SIZE * 2, 0xff, HARDBLKSIZE);

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

	int group = get_group(argv[1]);
	if (group < 0)
		perror("get_group"), exit(1);

	printf("%d\n", group);
	int container = get_container();
	if (container < 0)
		perror("get_container"), exit(1);

	if (group_set_container(group, container))
		perror("group_set_container"), exit(1);

	if (container_set_iommu(container))
		perror("container_set_iommu"), exit(1);

	int device = group_get_device(group, argv[1]);
	if (device < 0)
		perror("group_get_device"), exit(1);

	pthread_t pageout;
	if (pthread_create(&pageout, NULL, background_pageout, mem))
		perror("pthread_create pageout"), exit(1);

	pthread_t swap;
	if (pthread_create(&swap, NULL, background_swap, (void *)size))
		perror("pthread_create swap"), exit(1);

	static unsigned long count;

	char x;
	volatile char *mem2 = (char *)mem;
	printf("VFIO mapping loop"), fflush(stdout);
	while (1) {
		if (!(++count % 1000))
			printf("."), fflush(stdout);

		usleep(random() % 1000);
		x = mem2[PAGE_SIZE-1];
		if (dma_map(container, mem, PAGE_SIZE, 1<<20)) {
			fprintf(stderr, "dma_map() failed\n");
			exit(-1);
		}

		mem2[PAGE_SIZE-1] = x;

		if (dma_unmap(container, PAGE_SIZE, 1<<20) != PAGE_SIZE) {
			fprintf(stderr, "dma_unmap() failed\n");
			exit(-1);
		}
	}

	return 0;
}
