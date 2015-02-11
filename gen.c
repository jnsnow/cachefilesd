#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef unsigned atime_t;

enum method {
	RANDOM,
	SPARSE,
	ZERO,
	ASCEND,
	DESCEND,
	FLAT
};

void gen_atime(const char *name, size_t n, char method){
	unsigned X;
	FILE *fh = fopen(name, "w+");
	size_t i, j;
	size_t perpage = getpagesize() / sizeof(atime_t);
	size_t numpages = n / perpage;
  
	atime_t *buffer = malloc(getpagesize());
	if (fh == NULL) {
		fprintf(stderr,"oh god\n");
		exit(30);
	}

	fprintf(stderr, "num slots: %lu\n", n);
	fprintf(stderr, "numpages: %lu\n", numpages);
  
	/* initial values */
	if (method == ASCEND || method == ZERO) X = 0;
	else if (method == DESCEND) X = -1;
	else X = random();
  
	for (i = 0; i < numpages; ++i) {
		for (j = 0; j < perpage; ++j) {
			buffer[j] = X;
      
			if (method == ASCEND) X++;
			else if (method == DESCEND) X--;
			else if (method == RANDOM) X = random();
			else if (method == SPARSE) {
				X = random();
				X = (X & 0x01) ? X : 0;
			}
		}
    
		fwrite(buffer, sizeof(atime_t), perpage, fh);
	}
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

	fprintf(stderr, "gen [slots_exponent]\n");
	srand(time(NULL));

	if (argc > 1)
		slots_exponent = atoi(argv[1]);

	fprintf(stderr, "slots = 2^%u = %u\n", slots_exponent, 1ul << slots_exponent);
	for (i = 0; i < 6; ++i) {
		gen_atime(names[i], 1ul << slots_exponent, types[i]);
	}

	return EXIT_SUCCESS;
}
