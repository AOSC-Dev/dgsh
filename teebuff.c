/*
 * Copyright 2013 Diomidis Spinellis
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* #define DEBUG */

/*
 * Data that can't be written is stored in a sequential pool of buffers
 * each of buffer_size long.
 */

static int buffer_size = 1024 * 1024;

static char **buffers;

static off_t source_pos_read;

/* A buffer that can be used for reading */
struct buffer {
	void *p;	/* Memory pointer */
	size_t size;	/* Buffer size */
};

/* Scatter the output across the files, rather than copying it. */
static int opt_scatter;

/* Split scattered data on line boundaries */
static int opt_line;

/* Information regarding the files we write to */
struct sink_info {
	char *name;		/* Output file name */
	int fd;			/* Output file descriptor */
	off_t pos_written;	/* Position up to which written */
	off_t pos_to_write;	/* Position up to which to write */
	int active;		/* True if this sink is still active */
};


/*
 * Allocate memory for the specified pool
 */
static void
memory_allocate(int pool)
{
	static int pool_size;
	static int allocated_pool_end;
	int i;

	if (pool < allocated_pool_end)
		return;

	/* Resize bank, if needed. One iteration should suffice. */
	while (pool >= pool_size) {
		if (pool_size == 0)
			pool_size = 1;
		else
			pool_size *= 2;
		if ((buffers = realloc(buffers, pool_size * sizeof(char *))) == NULL)
			err(1, "Unable to reallocate buffer pool bank");
	}

	/* Allocate buffer memory [allocated_pool_end, pool]. */
	for (i = allocated_pool_end; i <= pool; i++) {
		if ((buffers[i] = malloc(buffer_size)) == NULL)
			err(1, "Unable to allocate %d bytes for buffer %d", buffer_size, i);
		#ifdef DEBUG
		fprintf(stderr, "Allocated buffer %d to %p\n", i, buffers[i]);
		#endif
	}
	allocated_pool_end = pool + 1;
}

/*
 * Ensure that pool buffers from [0,pos) are free.
 */
static void
memory_free(off_t pos)
{
	static int pool_begin = 0;
	int pool_end = pos / buffer_size;
	int i;

	for (i = pool_begin; i < pool_end; i++) {
		free(buffers[i]);
		#ifdef DEBUG
		buffers[i] = NULL;
		fprintf(stderr, "Freed buffer %d (pos = %ld, begin=%d end=%d)\n",
			i, (long)pos, pool_begin, pool_end);
		#endif
	}
	pool_begin = pool_end;
}

/*
 * Return the buffer to write to for reading from a file from
 * position onward, ensuring that sufficient memory is allocated.
 */
static struct buffer
source_buffer(off_t pos)
{
	struct buffer b;
	int pool = pos / buffer_size;
	size_t pool_offset = pos % buffer_size;

	memory_allocate(pool);
	b.p = buffers[pool] + pool_offset;
	b.size = buffer_size - pool_offset;
	#ifdef DEBUG
	fprintf(stderr, "Source buffer(%ld) returns pool %d(%p) o=%ld l=%ld a=%p\n",
		(long)pos, pool, buffers[pool], (long)pool_offset, (long)b.size, b.p);
	#endif
	return b;
}

/*
 * Return a buffer to read from for writing to a file from a position onward
 */
static struct buffer
sink_buffer(struct sink_info *si)
{
	struct buffer b;
	int pool = si->pos_written / buffer_size;
	size_t pool_offset = si->pos_written % buffer_size;
	size_t source_bytes = si->pos_to_write - si->pos_written;

	b.p = buffers[pool] + pool_offset;
	b.size = MIN(buffer_size - pool_offset, source_bytes);
	#ifdef DEBUG
	fprintf(stderr, "Sink buffer(%ld) returns pool %d(%p) o=%ld l=%ld a=%p\n",
		(long)si->pos_written, pool, buffers[pool], (long)pool_offset, (long)b.size, b.p);
	#endif
	return b;
}

/*
 * Return a pointer to read from for writing to a file from a position onward
 */
static char *
sink_pointer(off_t pos_written)
{
	int pool = pos_written / buffer_size;
	size_t pool_offset = pos_written % buffer_size;

	return buffers[pool] + pool_offset;
}

/*
 * Read from the source into the memory buffer
 * Return the number of bytes read, or 0 on end of file.
 */
static size_t
source_read(fd_set *source_fds)
{
	int n;
	struct buffer b;

	b = source_buffer(source_pos_read);
	if ((n = read(STDIN_FILENO, b.p, b.size)) == -1)
		err(3, "Read from standard input");
	source_pos_read += n;
	#ifdef DEBUG
	fprintf(stderr, "Read %d out of %d bytes\n", n, b.size);
	#endif
	return n;
}

/*
 * Allocate available read data to empty sinks that can be written to,
 * by adjusting their pos_written and pos_to_write pointers.
 */
static void
allocate_data_to_sinks(fd_set *sink_fds, struct sink_info *files, int nfiles)
{
	struct sink_info *si;
	int available_sinks = 0;
	off_t pos_assigned = 0;
	size_t available_data, data_per_sink;
	size_t data_to_assign = 0;

	/* Easy case: distribute to all files. */
	if (!opt_scatter) {
		for (si = files; si < files + nfiles; si++)
			si->pos_to_write = source_pos_read;
		return;
	}

	/*
	 * Difficult case: fair scattering
	 */

	/* Determine amount of fresh data to write and number of available sinks. */
	for (si = files; si < files + nfiles; si++) {
		pos_assigned = MAX(pos_assigned, si->pos_to_write);
		if (si->pos_written == si->pos_to_write && FD_ISSET(si->fd, sink_fds))
			available_sinks++;
	}

	if (available_sinks == 0)
		return;

	available_data = source_pos_read - pos_assigned;

	/* Assign data to sinks. */
	data_per_sink = available_data / available_sinks;
	#ifdef DEBUG
	fprintf(stderr, "available_data=%d data_per_sink=%d\n", available_data, data_per_sink);
	#endif
	for (si = files; si < files + nfiles; si++)
		if (si->pos_written == si->pos_to_write && FD_ISSET(si->fd, sink_fds)) {
			/* First file also gets the remainder bytes. */
			if (data_to_assign == 0)
				data_to_assign = data_per_sink + available_data % available_sinks;
			else
				data_to_assign = data_per_sink;
			/*
			 * Assign data_to_assign to *si (pos_written, pos_to_write),
			 * and advance pos_assigned.
			 */
			si->pos_written = pos_assigned;		/* Initially nothing has been written. */
			if (opt_line) {
				if (available_data > buffer_size / 2) {
					/*
					 * Efficient algorithm:
					 * Assume that multiple lines appear in data_per_sink.
					 * Go to a calculated boundary and move backward to find
					 * a new line.
					 */
					off_t data_end = pos_assigned + data_to_assign - 1;

					for (;;) {
						if (*sink_pointer(data_end) == '\n') {
							pos_assigned = data_end + 1;
							break;
						}
						data_end--;
						if (data_end + 1 == pos_assigned) {
							fprintf(stderr, "No newline found in a region of %d bytes. Increase buffer size.\n", data_to_assign - 1);
							exit(1);
						}
					}
				} else {
					/*
					 * Reliable algorithm:
					 * Scan forward for new lines until at least data_per_sink are covered,
					 * or we reach the end of available data.
					 * Keep a record of the last encountered newline.
					 * This is used when we scan past the end of the available data.
					 */
					off_t data_end, last_nl = -1;

					data_end = pos_assigned;
					for (;;) {
						if (data_end >= source_pos_read) {
							if (last_nl != -1) {
								pos_assigned = last_nl + 1;
								break;
							} else {
								/* No newline found in buffer; defer writing. */
								si->pos_to_write = pos_assigned;
								#ifdef DEBUG
								fprintf(stderr, "scatter to file[%d] no newline from %ld to %ld\n",
									si - files, (long)pos_assigned, (long)data_end);
								#endif
								return;
							}
						}

						if (*sink_pointer(data_end) == '\n') {
							last_nl = data_end;
							if (data_end - pos_assigned > data_per_sink) {
								pos_assigned = data_end + 1;
								break;
							}
						}
						data_end++;
					}
				}
			} else
				pos_assigned += data_to_assign;
			si->pos_to_write = pos_assigned;
			#ifdef DEBUG
			fprintf(stderr, "scatter to file[%d] pos_written=%ld pos_to_write=%ld\n",
				si - files, (long)si->pos_written, (long)si->pos_to_write);
			#endif
		}
}


/*
 * Write out from the memory buffer to the sinks where write will not block.
 * Free memory no more needed even by the write pointer farthest behind.
 * Return the number of bytes written.
 */
static size_t
sink_write(fd_set *sink_fds, struct sink_info *files, int nfiles)
{
	struct sink_info *si;
	off_t min_pos = source_pos_read;
	size_t written = 0;

	allocate_data_to_sinks(sink_fds, files, nfiles);
	for (si = files; si < files + nfiles; si++) {
		if (FD_ISSET(si->fd, sink_fds)) {
			int n;
			struct buffer b;

			b = sink_buffer(si);
			n = write(si->fd, b.p, b.size);
			if (n < 0)
				switch (errno) {
				/* EPIPE is acceptable, for the sink's reader can terminate early. */
				case EPIPE:
					si->active = 0;
					#ifdef DEBUG
					fprintf(stderr, "EPIPE for %s\n", si->name);
					#endif
					break;
				default:
					err(2, "Error writing to %s", si->name);
				}
			else {
				si->pos_written += n;
				written += n;
			}
			#ifdef DEBUG
			fprintf(stderr, "Wrote %d out of %d bytes for file %s\n",
				n, b.size, si->name);
			#endif
		}
		if (si->active)
			min_pos = MIN(min_pos, si->pos_written);
	}
	memory_free(min_pos);
	return written;
}

static void
usage(const char *name)
{
	fprintf(stderr, "Usage %s [-b buffer_size] [-s] [-l]\n", name);
	exit(1);
}

int
main(int argc, char *argv[])
{
	int max_fd = 0;
	int i;
	struct sink_info *files;
	int reached_eof = 0;
	int ch;
	const char *progname = argv[0];

	while ((ch = getopt(argc, argv, "b:sl")) != -1) {
		switch (ch) {
		case 'b':
			buffer_size = atoi(optarg);
			break;
		case 's':
			opt_scatter = 1;
			break;
		case 'l':
			opt_line = 1;
			break;
		case '?':
		default:
			usage(progname);
		}
	}
	argc -= optind;
	argv += optind;

	if ((files = (struct sink_info *)malloc(argc * sizeof(struct sink_info))) == NULL)
		err(1, NULL);

	/* Open files */
	for (i = 0; i < argc; i++) {
		if ((files[i].fd = open(argv[i], O_WRONLY | O_CREAT | O_TRUNC, DEFFILEMODE)) < 0)
			err(2, "Error opening %s", argv[i]);
		max_fd = MAX(files[i].fd, max_fd);
		files[i].name = argv[i];
		files[i].active = 1;
		files[i].pos_written = files[i].pos_to_write = 0;
	}

	/* We will handle SIGPIPE explicitly when calling write(2). */
	signal(SIGPIPE, SIG_IGN);

	/* Copy source to sink without allowing any single file to block us. */
	for (;;) {
		struct sink_info *si;
		fd_set source_fds;
		fd_set sink_fds;
		int active_fds;

		/* Set the fd's we're interested to read/write. */
		FD_ZERO(&source_fds);
		if (!reached_eof)
			FD_SET(STDIN_FILENO, &source_fds);

		active_fds = 0;
		FD_ZERO(&sink_fds);
		for (si = files; si < files + argc; si++)
			if (si->pos_written < source_pos_read && si->active) {
				FD_SET(si->fd, &sink_fds);
				active_fds++;
			}


		/* If no read possible, and no writes pending, terminate. */
		if (reached_eof && active_fds == 0)
			return 0;

		/* Block until we can read or write. */
		if (select(max_fd + 1, &source_fds, &sink_fds, NULL, NULL) < 0)
			err(3, "select");

		/* Write to all file descriptors that accept writes. */
		if (sink_write(&sink_fds, files, argc) > 0)
			/*
			 * If we wrote something, we made progress on the
			 * downstream end.  Loop without reading to avoid
			 * allocating excessive buffer memory.
			 */
			continue;

		/* Read, if possible. */
		if (FD_ISSET(STDIN_FILENO, &source_fds))
			if (source_read(&source_fds) == 0)
				reached_eof = 1;
	}
	return 0;
}
