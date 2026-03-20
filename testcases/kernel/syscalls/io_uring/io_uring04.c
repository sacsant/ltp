// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 IBM
 * Author: Sachin Sant <sachinp@linux.ibm.com>
 */
/*
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

static char write_bufs[NUM_VECS][VEC_SIZE];
static char read_bufs[NUM_VECS][VEC_SIZE];
static struct iovec write_iovs[NUM_VECS];
static struct iovec read_iovs[NUM_VECS];
static struct io_uring_submit s;
static sigset_t sig;

static void prepare_write_buffers(void)
{
	size_t i, j;

	for (i = 0; i < NUM_VECS; i++) {
		for (j = 0; j < VEC_SIZE; j++) {
			/* Each vector has a different pattern */
			write_bufs[i][j] = 'A' + i + (j % 26);
		}
		write_iovs[i].iov_base = write_bufs[i];
		write_iovs[i].iov_len = VEC_SIZE;
	}
}

static void prepare_read_buffers(void)
{
	size_t i;

	for (i = 0; i < NUM_VECS; i++) {
		memset(read_bufs[i], 0, VEC_SIZE);
		read_iovs[i].iov_base = read_bufs[i];
		read_iovs[i].iov_len = VEC_SIZE;
	}
}

static void verify_vector_data(char write_bufs[][VEC_SIZE],
			       char read_bufs[][VEC_SIZE],
			       size_t num_vecs, const char *test_name)
{
	size_t i, j;

	for (i = 0; i < num_vecs; i++) {
		if (memcmp(write_bufs[i], read_bufs[i], VEC_SIZE) != 0) {
			tst_res(TFAIL, "%s: data mismatch in vector %zu",
				test_name, i);
			for (j = 0; j < VEC_SIZE && j < 64; j++) {
				if (write_bufs[i][j] != read_bufs[i][j]) {
					tst_res(TINFO, "Vector %zu: first mismatch at "
						"offset %zu: wrote 0x%02x, read 0x%02x",
						i, j, write_bufs[i][j], read_bufs[i][j]);
					break;
				}
			}
			return;
		}
	}

	tst_res(TPASS, "%s: data integrity verified across %zu vectors",
		test_name, num_vecs);
}

static void test_writev_readv(void)
{
	int fd;
	int total_size = NUM_VECS * VEC_SIZE;

	tst_res(TINFO, "Testing IORING_OP_WRITEV and IORING_OP_READV");

	prepare_write_buffers();
	prepare_read_buffers();

	fd = SAFE_OPEN(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);

	tst_res(TINFO, "Writing %d bytes using %d vectors", total_size, NUM_VECS);
	io_uring_do_vec_io_op(&s, fd, IORING_OP_WRITEV, write_iovs, NUM_VECS,
			      0, total_size, &sig,
			      "IORING_OP_WRITEV completed successfully");

	SAFE_FSYNC(fd);

	tst_res(TINFO, "Reading %d bytes using %d vectors", total_size, NUM_VECS);
	io_uring_do_vec_io_op(&s, fd, IORING_OP_READV, read_iovs, NUM_VECS,
			      0, total_size, &sig,
			      "IORING_OP_READV completed successfully");

	verify_vector_data(write_bufs, read_bufs, NUM_VECS, "Basic vectored I/O");

	SAFE_CLOSE(fd);
}

static void test_partial_vectors(void)
{
	int fd;
	struct iovec partial_write[2];
	struct iovec partial_read[2];
	int expected_size;

	tst_res(TINFO, "Testing partial vector operations");

	prepare_write_buffers();
	prepare_read_buffers();

	fd = SAFE_OPEN(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);

	/* Write using only 2 vectors */
	partial_write[0] = write_iovs[0];
	partial_write[1] = write_iovs[1];
	expected_size = 2 * VEC_SIZE;

	io_uring_do_vec_io_op(&s, fd, IORING_OP_WRITEV, partial_write, 2, 0,
			      expected_size, &sig,
			      "Partial IORING_OP_WRITEV (2 vectors) succeeded");

	SAFE_FSYNC(fd);

	/* Read back using 2 vectors */
	partial_read[0] = read_iovs[0];
	partial_read[1] = read_iovs[1];

	io_uring_do_vec_io_op(&s, fd, IORING_OP_READV, partial_read, 2, 0,
			      expected_size, &sig,
			      "Partial IORING_OP_READV (2 vectors) succeeded");

	verify_vector_data(write_bufs, read_bufs, 2, "Partial vector I/O");

	SAFE_CLOSE(fd);
}

static void test_varying_sizes(void)
{
	int fd;
	struct iovec var_write[3];
	struct iovec var_read[3];
	char buf1[512], buf2[1024], buf3[256];
	char rbuf1[512], rbuf2[1024], rbuf3[256];
	int expected_size = 512 + 1024 + 256;

	tst_res(TINFO, "Testing vectors with varying sizes");

	io_uring_init_buffer_pattern(buf1, 512, 'X');
	io_uring_init_buffer_pattern(buf2, 1024, 'Y');
	io_uring_init_buffer_pattern(buf3, 256, 'Z');

	var_write[0].iov_base = buf1;
	var_write[0].iov_len = 512;
	var_write[1].iov_base = buf2;
	var_write[1].iov_len = 1024;
	var_write[2].iov_base = buf3;
	var_write[2].iov_len = 256;

	memset(rbuf1, 0, 512);
	memset(rbuf2, 0, 1024);
	memset(rbuf3, 0, 256);

	var_read[0].iov_base = rbuf1;
	var_read[0].iov_len = 512;
	var_read[1].iov_base = rbuf2;
	var_read[1].iov_len = 1024;
	var_read[2].iov_base = rbuf3;
	var_read[2].iov_len = 256;

	fd = SAFE_OPEN(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);

	io_uring_do_vec_io_op(&s, fd, IORING_OP_WRITEV, var_write, 3, 0,
			      expected_size, &sig,
			      "IORING_OP_WRITEV with varying sizes succeeded");

	SAFE_FSYNC(fd);

	io_uring_do_vec_io_op(&s, fd, IORING_OP_READV, var_read, 3, 0,
			      expected_size, &sig,
			      "IORING_OP_READV with varying sizes succeeded");

	/* Verify each buffer */
	if (memcmp(buf1, rbuf1, 512) == 0 &&
	    memcmp(buf2, rbuf2, 1024) == 0 &&
	    memcmp(buf3, rbuf3, 256) == 0) {
		tst_res(TPASS, "Varying size vector data integrity verified");
	} else {
		tst_res(TFAIL, "Varying size vector data mismatch");
	}

	SAFE_CLOSE(fd);
}

static void run(void)
{
	io_uring_setup_queue(&s, QUEUE_DEPTH);
	test_writev_readv();
	test_partial_vectors();
	test_varying_sizes();
	io_uring_cleanup_queue(&s, QUEUE_DEPTH);
}

static void setup(void)
{
	io_uring_setup_supported_by_kernel();
	sigemptyset(&sig);
	memset(&s, 0, sizeof(s));
}

static struct tst_test test = {
	.test_all = run,
	.setup = setup,
	.needs_tmpdir = 1,
	.save_restore = (const struct tst_path_val[]) {
		{"/proc/sys/kernel/io_uring_disabled", "0",
			TST_SR_SKIP_MISSING | TST_SR_TCONF_RO},
		{}
	}
};
