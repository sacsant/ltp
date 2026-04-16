// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 IBM
 * Author: Sachin Sant <sachinp@linux.ibm.com>
 */
/*\
 * Test IORING_OP_READV and IORING_OP_WRITEV operations.
 *
 * This test validates vectored read and write operations using io_uring.
 * It tests:
 * 1. IORING_OP_WRITEV - Writing data using multiple buffers (scatter)
 * 2. IORING_OP_READV - Reading data into multiple buffers (gather)
 * 3. Data integrity verification across multiple iovecs
 * 4. Edge cases with different iovec configurations
 */

#include "io_uring_common.h"

#define TEST_FILE "io_uring_test_file"
#define QUEUE_DEPTH 2
#define NUM_VECS 4
#define VEC_SIZE 1024
#define VAR_BUF1_SIZE 512
#define VAR_BUF2_SIZE 1024
#define VAR_BUF3_SIZE 256

static struct iovec *write_iovs, *read_iovs;
static struct iovec *var_write_iovs, *var_read_iovs;
static struct io_uring_submit s;
static sigset_t sig;

/*
 * Initialize iovec buffers with pattern
 * @iovs: array of iovec structures
 * @nvecs: number of iovecs
 * @base_char: base character for pattern
 * @use_rotating: if true, use rotating pattern; if false, use simple repeat
 */
static void init_iovec_buffers(struct iovec *iovs, int nvecs,
				char base_char, int use_rotating)
{
	int i;
	size_t j;
	char *buf;

	for (i = 0; i < nvecs; i++) {
		if (iovs[i].iov_len == 0)
			continue;

		buf = (char *)iovs[i].iov_base;
		if (use_rotating) {
			/* Each vector has a different rotating pattern */
			for (j = 0; j < iovs[i].iov_len; j++)
				buf[j] = base_char + i + (j % 26);
		} else {
			for (j = 0; j < iovs[i].iov_len; j++)
				buf[j] = base_char + i;
		}
	}
}

static void clear_iovec_buffers(struct iovec *iovs, int nvecs)
{
	int i;

	for (i = 0; i < nvecs; i++)
		memset(iovs[i].iov_base, 0, iovs[i].iov_len);
}

static void verify_iovec_data(struct iovec *write_iovs, struct iovec *read_iovs,
			      int nvecs, const char *test_name)
{
	int i;
	size_t j;

	for (i = 0; i < nvecs; i++) {
		if (write_iovs[i].iov_len != read_iovs[i].iov_len) {
			tst_res(TFAIL, "%s: iovec %d length mismatch: write=%zu read=%zu",
				test_name, i, write_iovs[i].iov_len, read_iovs[i].iov_len);
			return;
		}

		if (memcmp(write_iovs[i].iov_base, read_iovs[i].iov_base,
			   write_iovs[i].iov_len) != 0) {
			tst_res(TFAIL, "%s: data mismatch in vector %d", test_name, i);
			for (j = 0; j < write_iovs[i].iov_len && j < 64; j++) {
				char *wbuf = (char *)write_iovs[i].iov_base;
				char *rbuf = (char *)read_iovs[i].iov_base;
				if (wbuf[j] != rbuf[j]) {
					tst_res(TINFO, "Vector %d: first mismatch at "
						"offset %zu: wrote 0x%02x, read 0x%02x",
						i, j, wbuf[j], rbuf[j]);
					break;
				}
			}
			return;
		}
	}

	tst_res(TPASS, "%s: data integrity verified across %d vectors",
		test_name, nvecs);
}

static void test_writev_readv(void)
{
	int fd;
	int total_size = NUM_VECS * VEC_SIZE;

	tst_res(TINFO, "Testing IORING_OP_WRITEV and IORING_OP_READV");

	fd = SAFE_OPEN(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);

	tst_res(TINFO, "Writing %d bytes using %d vectors", total_size, NUM_VECS);
	io_uring_do_vec_io_op(&s, fd, IORING_OP_WRITEV, write_iovs, NUM_VECS,
			      0, total_size, &sig);

	SAFE_FSYNC(fd);

	tst_res(TINFO, "Reading %d bytes using %d vectors", total_size, NUM_VECS);
	io_uring_do_vec_io_op(&s, fd, IORING_OP_READV, read_iovs, NUM_VECS,
			      0, total_size, &sig);

	verify_iovec_data(write_iovs, read_iovs, NUM_VECS, "Basic vectored I/O");

	SAFE_CLOSE(fd);
}

static void test_partial_vectors(void)
{
	int fd;
	int half_size = 2 * VEC_SIZE;

	tst_res(TINFO, "Testing partial vector operations");

	fd = SAFE_OPEN(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);

	/* Write first half using first 2 vectors at offset 0 */
	io_uring_do_vec_io_op(&s, fd, IORING_OP_WRITEV, write_iovs, 2, 0,
			      half_size, &sig);

	/* Write second half using next 2 vectors at offset half_size */
	io_uring_do_vec_io_op(&s, fd, IORING_OP_WRITEV, &write_iovs[2], 2,
			      half_size, half_size, &sig);

	SAFE_FSYNC(fd);

	/* Read back entire file using all 4 vectors */
	io_uring_do_vec_io_op(&s, fd, IORING_OP_READV, read_iovs, NUM_VECS, 0,
			      NUM_VECS * VEC_SIZE, &sig);

	verify_iovec_data(write_iovs, read_iovs, NUM_VECS, "Partial vector I/O");

	SAFE_CLOSE(fd);
}

static void test_varying_sizes(void)
{
	int fd;
	int expected_size = VAR_BUF1_SIZE + VAR_BUF2_SIZE + VAR_BUF3_SIZE;

	tst_res(TINFO, "Testing vectors with varying sizes including zero-length buffer");

	init_iovec_buffers(var_write_iovs, 4, 'X', 0);

	clear_iovec_buffers(var_read_iovs, 4);

	fd = SAFE_OPEN(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);

	io_uring_do_vec_io_op(&s, fd, IORING_OP_WRITEV, var_write_iovs, 4, 0,
			      expected_size, &sig);

	SAFE_FSYNC(fd);

	io_uring_do_vec_io_op(&s, fd, IORING_OP_READV, var_read_iovs, 4, 0,
			      expected_size, &sig);

	verify_iovec_data(var_write_iovs, var_read_iovs, 4, "Varying size vector I/O with zero-length buffer");

	SAFE_CLOSE(fd);
}

static void run(void)
{
	clear_iovec_buffers(read_iovs, NUM_VECS);

	test_writev_readv();
	test_partial_vectors();
	test_varying_sizes();
}

static void setup(void)
{
	io_uring_setup_supported_by_kernel();
	sigemptyset(&sig);
	memset(&s, 0, sizeof(s));
	io_uring_setup_queue(&s, QUEUE_DEPTH, 0);
	init_iovec_buffers(write_iovs, NUM_VECS, 'A', 1);
	clear_iovec_buffers(read_iovs, NUM_VECS);
}

static void cleanup(void)
{
	io_uring_cleanup_queue(&s, QUEUE_DEPTH);
}

static struct tst_test test = {
	.test_all = run,
	.setup = setup,
	.cleanup = cleanup,
	.needs_tmpdir = 1,
	.bufs = (struct tst_buffers []) {
		{&write_iovs, .iov_sizes = (int[]){VEC_SIZE, VEC_SIZE, VEC_SIZE, VEC_SIZE, -1}},
		{&read_iovs, .iov_sizes = (int[]){VEC_SIZE, VEC_SIZE, VEC_SIZE, VEC_SIZE, -1}},
		{&var_write_iovs, .iov_sizes = (int[]){VAR_BUF1_SIZE, 0, VAR_BUF2_SIZE, VAR_BUF3_SIZE, -1}},
		{&var_read_iovs, .iov_sizes = (int[]){VAR_BUF1_SIZE, 0, VAR_BUF2_SIZE, VAR_BUF3_SIZE, -1}},
		{}
	},
	.save_restore = (const struct tst_path_val[]) {
		{"/proc/sys/kernel/io_uring_disabled", "0",
			TST_SR_SKIP_MISSING | TST_SR_TCONF_RO},
		{}
	}
};
