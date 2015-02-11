#include <stdio.h>
#include <stdlib.h>
#include "common/cull.h"
#include "common/debug.h"
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GENERATE_ONCE (0)
#define GENERATE_MANY (1)

void gen_cull(const char *name, size_t n) {

	size_t i;
	size_t entsize = 26;
	size_t pagesize = getpagesize();
	size_t perpage = pagesize / entsize;
	size_t numpages = (n + perpage - 1) / perpage;
	int rc;
	FILE *fh = fopen(name, "w");
	unsigned char *buffer = malloc(pagesize);

	memset(buffer, 0xFF, pagesize);

	for (i = 0; i < numpages; ++i) {
		rc = fwrite(buffer, pagesize, 1, fh);
		if (rc < 1) {
			debug(0, "rc = %d; i = %lu", rc, i);
			break;
		}
	}

	free(buffer);
	fclose(fh);
}


enum method {
	RANDOM,
	SPARSE,
	ZERO,
	ASCEND,
	DESCEND,
	FLAT
};

void gen_atime(const char *name, size_t n, char method)
{
	unsigned X;
	FILE *fh = fopen(name, "w+");
	size_t i;
	atime_t *buffer = malloc(sizeof(atime_t) * n);
	if (fh == NULL) {
		debug(2,"error");
		exit(30);
	}

	/* initial values */
	if (method == ASCEND || method == ZERO) X = 0;
	else if (method == DESCEND) X = -1;
	else X = random();


	for (i = 0; i < n; ++i) {
		buffer[i] = X;

		if (method == ASCEND) X++;
		else if (method == DESCEND) X--;
		else if (method == RANDOM) X = random();
		else if (method == SPARSE) {
			X = random();
			X = (X & 0x01) ? X : 0;
		}
	}

	fwrite(buffer, sizeof(atime_t), n, fh);
	free(buffer);
	fclose(fh);
}

const char *names[] = {
	"atimes",
	"atimes_sparse",
	"atimes_zero",
	"atimes_ascend",
	"atimes_descend",
	"atimes_flat" };
const char *resultnames[] = {
	"data",
	"data_sparse",
	"data_zero",
	"data_ascend",
	"data_descend",
	"data_flat" };
const char types[] = {
	RANDOM,
	SPARSE,
	ZERO,
	ASCEND,
	DESCEND,
	FLAT };

int main(int argc, char *argv[]) {

	struct queue *cullq;
	unsigned table_exponent = 12;
	unsigned slots_exponent = 24; /* 16 million slots */
	int i;
	struct timeval tv;
	long unsigned d1, d2;
	FILE *fh[4];
	struct cachefilesd_state state;

	/* Enable visible debugging */
	xnolog = 1;
	xdebug += 2;

	srand(time(NULL));

	if (argc > 1)
		table_exponent = atoi(argv[1]);
	if (argc > 2)
		slots_exponent = atoi(argv[2]);

	/* Initialize some stuff, there are inflexible testing values ... */
	state.indexfile = strdup("cull_index");
	state.ent_size = 26;
	state.pagesize = getpagesize();
	state.num_perpage = state.pagesize / state.ent_size;

	/* generate a solid block of 0xFF bytes to represent occupied slots */
	/*gen_cull("cull_index", 1 << slots_exponent);*/

/*	for (percent_evicted = 0; percent_evicted <= 100; ++percent_evicted) {*/
	for (percent_evicted = 100; percent_evicted != -1; --percent_evicted) {
		if (percent_evicted == 94) percent_evicted = 5;
		for (i = 0; i < 6; ++i) {

			state.atimefile = strdup(names[i]);

			if (GENERATE_MANY)
				gen_atime(names[i], 1 << slots_exponent, types[i]);
			fh[i] = fopen(resultnames[i], "a+");

			fprintf(fh[i], "%3lu\t", percent_evicted);
			fprintf(stdout, "%3lu\t", percent_evicted);
			fflush(stdout);

			cullq = new_queue(table_exponent, &state);

			timer_start(&tv);
			build_cull_queue(cullq, 1);
			d1 = timer_stop(&tv);

			fprintf(fh[i], "%8lu\t", d1);
			fprintf(stdout, "%8lu\t", d1);
			fflush(stdout);

			timer_start(&tv);
			queue_refresh(cullq);
			d2 = timer_stop(&tv);

			fprintf(fh[i], "%8lu\n", d2);
			fprintf(stdout, "%8lu\n", d2);
			fflush(stdout);

			delete_queue(cullq);
			fclose(fh[i]);
			free(state.atimefile);
		}
	}

	free(state.indexfile);
	return EXIT_SUCCESS;
}
