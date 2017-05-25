/*-
 * Copyright (c) 2017 Hans Petter Selasky. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdint.h>
#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <sysexits.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/queue.h>
#include <sys/param.h>
#include <unistd.h>
#include <err.h>
#include <time.h>

struct data;
typedef TAILQ_HEAD(, data) head_t;
struct data {
	TAILQ_ENTRY(data) entry;
	int	fd;
	int	bytes;
	uint8_t	data[0];
};

static head_t data_head = TAILQ_HEAD_INITIALIZER(data_head);

struct devinfo;
typedef TAILQ_HEAD(, devinfo) devhead_t;
struct devinfo {
	TAILQ_ENTRY(devinfo) entry;
	char	inputfn[MAXPATHLEN];
	char	outputfn[MAXPATHLEN];
	int	input_fd;
	int	output_fd;
};

static devhead_t devinfo_head = TAILQ_HEAD_INITIALIZER(devinfo_head);

static pthread_mutex_t atomic_mtx;
static pthread_cond_t atomic_cv;
static int default_blocksize = 4096;	/* bytes */
static const char *default_prefix = "recording";
static int pending;
static uint64_t total;

static void
atomic_lock(void)
{
	pthread_mutex_lock(&atomic_mtx);
}

static void
atomic_unlock(void)
{
	pthread_mutex_unlock(&atomic_mtx);
}

static void
atomic_wait(void)
{
	pthread_cond_wait(&atomic_cv, &atomic_mtx);
}

static void
atomic_signal(void)
{
	pthread_cond_signal(&atomic_cv);
}

static int
write_async(int fd, const void *data, int bytes)
{
	struct data *ptr;

	ptr = malloc(sizeof(*ptr) + bytes);
	if (ptr == NULL)
		errx(EX_SOFTWARE, "Out of memory");
	ptr->fd = fd;
	ptr->bytes = bytes;
	memcpy(ptr + 1, data, bytes);

	atomic_lock();
	pending += bytes;
	TAILQ_INSERT_TAIL(&data_head, ptr, entry);
	atomic_signal();
	atomic_unlock();

	return (bytes);
}

static void *
audio_thread(void *arg)
{
	struct devinfo *di = arg;
	uint8_t buffer[default_blocksize] __aligned(8);
	int error;
	int i;

	while (1) {
		error = read(di->input_fd, buffer, sizeof(buffer));
		if (error != sizeof(buffer))
			err(EX_SOFTWARE, "Could not read from audio file");

		error = write_async(di->output_fd, buffer, sizeof(buffer));
		if (error != sizeof(buffer))
			err(EX_SOFTWARE, "Could not write to audio file");
	}
	return (NULL);
}

static void *
status_thread(void *arg)
{
	time_t start = time(NULL);

	while (1) {
		uint64_t runtime;
		uint64_t tot;
		int pnd;

		atomic_lock();
		tot = total;
		pnd = pending;
		atomic_unlock();

		runtime = difftime(time(NULL), start);
		if (runtime == 0)
			runtime = 1;

		printf("Status: %09d / %09d / %012lld - %03d:%02d:%02d\r",
		    (int)pnd, (int)(tot / runtime), (long long)tot,
		    (int)(runtime / (60 * 60)),
		    (int)((runtime / 60) % 60),
		    (int)(runtime % 60));
		fflush(stdout);

		usleep(1000000);
	}
}


static void
usage(void)
{
	fprintf(stderr, "usage: audiocat [-o recording] [-b 4096] -i /dev/dsp.wav [-i /dev/dsp2.wav]\n");
	exit(0);
}

int
main(int argc, char **argv)
{
	struct devinfo *di;
	pthread_t dummy;
	int c;
	int n = 0;

	pthread_mutex_init(&atomic_mtx, NULL);
	pthread_cond_init(&atomic_cv, NULL);

	while ((c = getopt(argc, argv, "hb:i:o:")) != -1) {
		switch (c) {
		case 'b':
			default_blocksize = atoi(optarg);
			break;
		case 'o':
			default_prefix = optarg;
			break;
		case 'i':
			di = malloc(sizeof(*di));
			if (di == NULL)
				errx(EX_SOFTWARE, "Out of memory");
			memset(di, 0, sizeof(*di));
			snprintf(di->inputfn, sizeof(di->inputfn), "%s", optarg);
			snprintf(di->outputfn, sizeof(di->outputfn), "%s-%d.wav", default_prefix, n);
			TAILQ_INSERT_TAIL(&devinfo_head, di, entry);
			n++;
			break;
		default:
			usage();
			break;
		}
	}

	if (n == 0)
		usage();

	TAILQ_FOREACH(di, &devinfo_head, entry) {

		di->input_fd = open(di->inputfn, O_RDONLY);
		if (di->input_fd == -1)
			err(EX_SOFTWARE, "Couldn't open device '%s'", di->inputfn);

		di->output_fd = open(di->outputfn, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (di->output_fd == -1)
			err(EX_SOFTWARE, "Couldn't open file '%s'", di->outputfn);

		if (pthread_create(&dummy, NULL, &audio_thread, di))
			errx(EX_SOFTWARE, "Couldn't create thread");
	}

	printf("Press CTRL+C to complete recording\n");

	if (pthread_create(&dummy, NULL, &status_thread, NULL))
		errx(EX_SOFTWARE, "Couldn't create thread");

	atomic_lock();
	while (1) {
		struct data *ptr;

		ptr = TAILQ_FIRST(&data_head);
		if (ptr == NULL) {
			atomic_wait();
			continue;
		}
		TAILQ_REMOVE(&data_head, ptr, entry);
		total += ptr->bytes;
		pending -= ptr->bytes;
		atomic_unlock();

		if (write(ptr->fd, ptr->data, ptr->bytes) != ptr->bytes)
			err(EX_SOFTWARE, "Could not write data to file");

		free(ptr);

		atomic_lock();
	}
	atomic_unlock();
	return (0);
}
