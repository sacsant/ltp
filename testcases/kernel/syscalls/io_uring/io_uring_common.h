// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 IBM
 * Author: Sachin Sant <sachinp@linux.ibm.com>
 *
 * Common definitions and helper functions for io_uring tests
 */

#ifndef IO_URING_COMMON_H
#define IO_URING_COMMON_H

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include "config.h"
#include "tst_test.h"
#include "lapi/io_uring.h"

/* Common structures for io_uring ring management */
struct io_sq_ring {
	unsigned int *head;
	unsigned int *tail;
	unsigned int *ring_mask;
	unsigned int *ring_entries;
	unsigned int *flags;
	unsigned int *array;
};

struct io_cq_ring {
	unsigned int *head;
	unsigned int *tail;
	unsigned int *ring_mask;
	unsigned int *ring_entries;
	struct io_uring_cqe *cqes;
};

struct io_uring_submit {
	int ring_fd;
	struct io_sq_ring sq_ring;
	struct io_uring_sqe *sqes;
	struct io_cq_ring cq_ring;
	void *sq_ptr;
	size_t sq_ptr_size;
	void *cq_ptr;
	size_t cq_ptr_size;
};

/*
 * Setup io_uring instance with specified queue depth
 * Returns 0 on success, -1 on failure
 */
static inline int io_uring_setup_queue(struct io_uring_submit *s,
				       unsigned int queue_depth)
{
	struct io_sq_ring *sring = &s->sq_ring;
	struct io_cq_ring *cring = &s->cq_ring;
	struct io_uring_params p;

	memset(&p, 0, sizeof(p));
	s->ring_fd = io_uring_setup(queue_depth, &p);
	if (s->ring_fd < 0) {
		tst_brk(TBROK | TERRNO, "io_uring_setup() failed");
		return -1;
	}

	s->sq_ptr_size = p.sq_off.array + p.sq_entries * sizeof(unsigned int);

	/* Map submission queue ring buffer */
	s->sq_ptr = SAFE_MMAP(0, s->sq_ptr_size, PROT_READ | PROT_WRITE,
			      MAP_SHARED | MAP_POPULATE, s->ring_fd,
			      IORING_OFF_SQ_RING);

	/* Save submission queue pointers */
	sring->head = s->sq_ptr + p.sq_off.head;
	sring->tail = s->sq_ptr + p.sq_off.tail;
	sring->ring_mask = s->sq_ptr + p.sq_off.ring_mask;
	sring->ring_entries = s->sq_ptr + p.sq_off.ring_entries;
	sring->flags = s->sq_ptr + p.sq_off.flags;
	sring->array = s->sq_ptr + p.sq_off.array;

	/* Map submission queue entries */
	s->sqes = SAFE_MMAP(0, p.sq_entries * sizeof(struct io_uring_sqe),
			    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
			    s->ring_fd, IORING_OFF_SQES);

	s->cq_ptr_size = p.cq_off.cqes +
			 p.cq_entries * sizeof(struct io_uring_cqe);

	s->cq_ptr = SAFE_MMAP(0, s->cq_ptr_size, PROT_READ | PROT_WRITE,
			      MAP_SHARED | MAP_POPULATE, s->ring_fd,
			      IORING_OFF_CQ_RING);

	/* Save completion queue pointers */
	cring->head = s->cq_ptr + p.cq_off.head;
	cring->tail = s->cq_ptr + p.cq_off.tail;
	cring->ring_mask = s->cq_ptr + p.cq_off.ring_mask;
	cring->ring_entries = s->cq_ptr + p.cq_off.ring_entries;
	cring->cqes = s->cq_ptr + p.cq_off.cqes;

	return 0;
}

/*
 * Cleanup io_uring instance and unmap all memory regions
 */
static inline void io_uring_cleanup_queue(struct io_uring_submit *s,
					  unsigned int queue_depth)
{
	if (s->sqes)
		SAFE_MUNMAP(s->sqes, queue_depth * sizeof(struct io_uring_sqe));
	if (s->cq_ptr)
		SAFE_MUNMAP(s->cq_ptr, s->cq_ptr_size);
	if (s->sq_ptr)
		SAFE_MUNMAP(s->sq_ptr, s->sq_ptr_size);
	if (s->ring_fd > 0)
		SAFE_CLOSE(s->ring_fd);
}

/*
 * Internal helper to submit a single SQE to the submission queue
 * Used by both vectored and non-vectored I/O operations
 */
static inline void io_uring_submit_sqe_internal(struct io_uring_submit *s,
						int fd, int opcode,
						unsigned long addr,
						unsigned int len,
						off_t offset)
{
	struct io_sq_ring *sring = &s->sq_ring;
	unsigned int tail, index;
	struct io_uring_sqe *sqe;

	tail = *sring->tail;
	index = tail & *sring->ring_mask;
	sqe = &s->sqes[index];

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = opcode;
	sqe->fd = fd;
	sqe->addr = addr;
	sqe->len = len;
	sqe->off = offset;
	sqe->user_data = opcode;

	sring->array[index] = index;
	tail++;

	*sring->tail = tail;
}

/*
 * Submit a single SQE to the submission queue
 * For basic read/write operations (non-vectored)
 */
static inline void io_uring_submit_sqe(struct io_uring_submit *s, int fd,
				       int opcode, void *buf, size_t len,
				       off_t offset)
{
	io_uring_submit_sqe_internal(s, fd, opcode, (unsigned long)buf,
				     len, offset);
}

/*
 * Submit a vectored SQE to the submission queue
 * For readv/writev operations
 */
static inline void io_uring_submit_sqe_vec(struct io_uring_submit *s, int fd,
					   int opcode, struct iovec *iovs,
					   int nr_vecs, off_t offset)
{
	io_uring_submit_sqe_internal(s, fd, opcode, (unsigned long)iovs,
				     nr_vecs, offset);
}

/*
 * Wait for and validate a completion queue entry
 * Returns 0 on success, -1 on failure
 */
static inline int io_uring_wait_cqe(struct io_uring_submit *s,
				    int expected_res, int expected_opcode,
				    sigset_t *sig)
{
	struct io_cq_ring *cring = &s->cq_ring;
	struct io_uring_cqe *cqe;
	unsigned int head;
	int ret;

	ret = io_uring_enter(s->ring_fd, 1, 1, IORING_ENTER_GETEVENTS, sig);
	if (ret < 0) {
		tst_res(TFAIL | TERRNO, "io_uring_enter() failed");
		return -1;
	}

	head = *cring->head;
	if (head == *cring->tail) {
		tst_res(TFAIL, "No completion event received");
		return -1;
	}

	cqe = &cring->cqes[head & *cring->ring_mask];

	if (cqe->user_data != (uint64_t)expected_opcode) {
		tst_res(TFAIL, "Unexpected user_data: got %llu, expected %d",
			(unsigned long long)cqe->user_data, expected_opcode);
		*cring->head = head + 1;
		return -1;
	}

	if (cqe->res != expected_res) {
		tst_res(TFAIL, "Operation failed: res=%d, expected=%d",
			cqe->res, expected_res);
		*cring->head = head + 1;
		return -1;
	}

	*cring->head = head + 1;
	return 0;
}

/*
 * Initialize buffer with a repeating character pattern
 * Useful for creating test data with predictable patterns
 */
static inline void io_uring_init_buffer_pattern(char *buf, size_t size,
						char pattern)
{
	size_t i;

	for (i = 0; i < size; i++)
		buf[i] = pattern;
}

/*
 * Submit and wait for a non-vectored I/O operation
 * Combines io_uring_submit_sqe() and io_uring_wait_cqe() with result reporting
 */
static inline void io_uring_do_io_op(struct io_uring_submit *s, int fd,
				     int op, void *buf, size_t len,
				     off_t offset, sigset_t *sig,
				     const char *msg)
{
	io_uring_submit_sqe(s, fd, op, buf, len, offset);

	if (io_uring_wait_cqe(s, len, op, sig) == 0)
		tst_res(TPASS, "%s", msg);
}

/*
 * Submit and wait for a vectored I/O operation
 * Combines io_uring_submit_sqe_vec() and io_uring_wait_cqe() with
 * result reporting
 */
static inline void io_uring_do_vec_io_op(struct io_uring_submit *s, int fd,
					 int op, struct iovec *iovs,
					 int nvecs, off_t offset,
					 int expected_size, sigset_t *sig,
					 const char *msg)
{
	io_uring_submit_sqe_vec(s, fd, op, iovs, nvecs, offset);

	if (io_uring_wait_cqe(s, expected_size, op, sig) == 0)
		tst_res(TPASS, "%s", msg);
}

#endif /* IO_URING_COMMON_H */
