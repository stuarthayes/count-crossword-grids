#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/io.h>
#include <fcntl.h>
#include <linux/types.h>
#include <time.h>


int num_valid_singlerows;
int *singlerows;
int num_valid_doublerows;
int *doublerows;
int num_valid_quadruplerows;
int *quadruplerows;
int num_valid_fiverows;
int *fiverows;
int num_valid_grids;
int row[11];
int seen[11];
unsigned long long valid_grid[20000000];

int colmask[11] =  {0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400};
int colshift[11] = {  0,   1,   2,   3,    4,    5,    6,    7,     8,     9,    10};


int comp (const void *elem1, const void *elem2) {
	unsigned long long f = *((unsigned long long *)elem1);
	unsigned long long s = *((unsigned long long *)elem2);
	if (f>s) return 1;
	if (f<s) return -1;
	return 0;
}

void output_valid_grids() {
	int i;

	qsort(valid_grid, num_valid_grids, sizeof(*valid_grid), comp);

	for (i = 0; i < num_valid_grids; i++)
		printf("%lld\n",valid_grid[i]);

}

/*
 * line1x14_ok
 *
 * if bit X is set, then X is a valid line (i.e., it doesn't have one or two white squares surrounded by black squares)
 * pre-calculated for speed
 */
//DECLARE_BITMAP(line1x14_ok, 16384);
char line1x14_ok[2048]; //16384-bit bitmap

static __always_inline void set_bit(int n, char *addr) {
	addr[n >> 3] |= 1 << (n & 7);
}

static __always_inline int test_bit(int n, char *addr) {
	return addr[n >> 3] & (1 << (n & 7));
}


int numzeros[1<<11];

void print_bits(size_t const size, int number)
{
    unsigned char byte;
    int i;

    for (i=size-1;i>=0;i--)
    {
            byte = (number >> i) & 1;
            printf("%u", byte);
    }
    //puts("");
}

/*
 * progress bar
 */
#define PBSTR "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||"
#define PBWIDTH 100
void print_progress(double p) {
	int val = (int)(p * 100);
	int lpad = (int) (p * PBWIDTH);
	int rpad = PBWIDTH - lpad;
	printf("\r%3d%% [%.*s%*s]", val, lpad, PBSTR, rpad, "");
	fflush(stdout);
}

void print_progress_2(int p, int t) {
	printf("\r%d / %d", p, t);
	fflush(stdout);
}

int reverse_bits_11(int n) {
	return  ((n & (1<<10)) >> 10)
	      | ((n & (1<<9)) >> 8)
	      | ((n & (1<<8)) >> 6)
	      | ((n & (1<<7)) >> 4)
	      | ((n & (1<<6)) >> 2)
	      | ((n & (1<<5)) >> 0)
	      | ((n & (1<<4)) << 2)
	      | ((n & (1<<3)) << 4)
	      | ((n & (1<<2)) << 6)
	      | ((n & (1<<1)) << 8)
	      | ((n & (1<<0)) << 10);
}

int reverse_bits_3(int n) {
	return  ((n & (1<<2)) >> 2)
	      | ((n & (1<<1)) >> 0)
	      | ((n & (1<<0)) << 2);
}

void print_row(int row, int length) {
	int c;

	for (c = 0; c < length; c++)
		if (row & (1<<c))
			printf("#");
		else
			printf("_");
}

void print_rows(int numrows) {
	int i;

	for (i=0; i<numrows; i++) {
		print_row(row[i], 11);
		printf("\n");
	}
}


/*
 * check_line_ok -- just make sure there aren't 1 or 2 letter words between black squares
 */
int check_line_ok_slow(int l, int length) {
	int i;
	int whites_in_a_row, black_seen;

	whites_in_a_row = 0; black_seen = 0;
	for (i = 0; i < length; i++)
		if (l & (1 << i)) {
			/* black square */
			if (whites_in_a_row > 0 && whites_in_a_row < 3) {
				return 0;
			}
			black_seen = 1;
			whites_in_a_row = 0;
		} else {
			/* white square */
			whites_in_a_row += black_seen;
		}
	return 1;
}

void init_line_ok_array(void) {
	int l;

	for (l = 0; l < (1 << 14); l++)
		if (check_line_ok_slow(l, 14))
			set_bit(l, line1x14_ok);
}

void init_numzeros(void) {
	int l, b;
	for (l = 0; l < (1 << 11); l++) {
		for (b = 0; b < 11; b++)
			numzeros[l] += ((l & (1 << b)) == 0);
		//printf("%x has %d zeros\n", l, numzeros[l]);
	}
}

int check_line_ok_quick(int l) {
	return test_bit(l, line1x14_ok);
}



int check_columns(int n, int blackrowontop) {
	int i;
	int c;
	int col;

	//printf("row0: %x row1: %x\n", row[0], row[1]);
	for (c = 0; c < 11; c++) {
		col = 0;
		for (i = n - 1; i >= 0; i--) {
			col = (col << 1) + ( (row[i] & colmask[c]) >> colshift[c]);
		}
		if (blackrowontop)
			col = (col << 1) + 1;
		//printf("col%d: ",c); print_bits(n+1, col); printf("\n");
		if (!check_line_ok_quick(col)) {
			//printf(" .. not ok\n");
			return 0;
		}
	}
	return 1;
}


int rightkey[8] = {3, 2, 1, 1, 0, 0, 0, 0};

int a_rk() {
	return
		  (rightkey[(row[0] >> 3) & 0x7] << 0)
		+ (rightkey[(row[1] >> 3) & 0x7] << 2)
		+ (rightkey[(row[2] >> 3) & 0x7] << 4)
		+ (rightkey[(row[3] >> 3) & 0x7] << 6)
		+ (rightkey[(row[4] >> 3) & 0x7] << 8);
}

int a_bk() {
	return
		  (rightkey[((row[2] & 0x01) + ((row[3] & 0x01) << 1) + ((row[4] & 0x01) << 2)) >> 0] << 0)
		+ (rightkey[((row[2] & 0x02) + ((row[3] & 0x02) << 1) + ((row[4] & 0x02) << 2)) >> 1] << 2)
		+ (rightkey[((row[2] & 0x04) + ((row[3] & 0x04) << 1) + ((row[4] & 0x04) << 2)) >> 2] << 4)
		+ (rightkey[((row[2] & 0x08) + ((row[3] & 0x08) << 1) + ((row[4] & 0x08) << 2)) >> 3] << 6)
		+ (rightkey[((row[2] & 0x10) + ((row[3] & 0x10) << 1) + ((row[4] & 0x10) << 2)) >> 4] << 8);
}

int b_rk() {
	return
		  (rightkey[((row[3] & 0x400) + ((row[4] & 0x400) << 1) + ((row[5] & 0x400) << 2)) >> 10] << 0)
		+ (rightkey[((row[3] & 0x200) + ((row[4] & 0x200) << 1) + ((row[5] & 0x200) << 2)) >> 9] << 2)
		+ (rightkey[((row[3] & 0x100) + ((row[4] & 0x100) << 1) + ((row[5] & 0x100) << 2)) >> 8] << 4)
		+ (rightkey[((row[3] & 0x080) + ((row[4] & 0x080) << 1) + ((row[5] & 0x080) << 2)) >> 7] << 6)
		+ (rightkey[((row[3] & 0x040) + ((row[4] & 0x040) << 1) + ((row[5] & 0x040) << 2)) >> 6] << 8);
}

int b_bk() {
	return
		  (rightkey[reverse_bits_3((row[0] >> 6) & 0x7)] << 0)
		+ (rightkey[reverse_bits_3((row[1] >> 6) & 0x7)] << 2)
		+ (rightkey[reverse_bits_3((row[2] >> 6) & 0x7)] << 4)
		+ (rightkey[reverse_bits_3((row[3] >> 6) & 0x7)] << 6)
		+ (rightkey[reverse_bits_3((row[4] >> 6) & 0x7)] << 8);
}


int grid_whitesquares(void) {
	int r, count;

	count = 0;
	for (r = 0; r < 11; r++)
		count += numzeros[row[r]];
	return count;
}

int connected(int r, int c) {
	int count;
	
	count = 0;
	//printf("looking at (%d, %d): row is 0x%x, grid is %d, seen is %d\n",r,c, row[r],row[r]&colmask[c], seen[r]&colmask[c]);
	if (row[r] & colmask[c])
		return 0;
	if (seen[r] & colmask[c])
		return 0;
	seen[r] |= colmask[c];  // set bit showing we've looked at this square
	count++;
	if (r > 0)
		count += connected(r-1, c);
	if (r < 10)
		count += connected(r+1, c);
	if (c > 0)
		count += connected(r, c-1);
	if (c < 10)
		count += connected(r, c+1);
	return count;
}
		
int first_white_region_size(void) {
	int r, c, count;

	for (r = 0; r < 11; r++)
		seen[r] = 0;
	for (r = 0; r < 11; r++)
		for (c = 0; c < 11; c++) {
			count = connected(r, c);
			if (count)
				return count;
		}
	return 0;
}

/*
 *     #   #    #     ###   #   #
 *     ## ##   # #     #    ##  #
 *     # # #  #   #    #    # # #
 *     #   #  #####    #    #  ##
 *     #   #  #   #   ###   #   #
 */

int main (int argc, const char *argv[]) {
	
	init_line_ok_array();
	init_numzeros();

	{
		time_t t;

		srand((unsigned) time(&t));
	}

	{
		int r, i;

		for (r = 0; r < (1 << 11); r++)
			if (check_line_ok_quick((1<<12) + (r << 1) + 1))
				num_valid_singlerows++;

		printf("%d valid rows in 11x11 \n", num_valid_singlerows);

		singlerows = malloc(num_valid_singlerows * sizeof(int));

		i = 0;
		for (r = 0; r < (1 << 11); r++)
			if (check_line_ok_quick((1<<12) + (r << 1) + 1)) {
				/*
				printf("%d:  ",i);
				print_bits(13, (1<<12) + (r<<1) + 1);
				printf("  (");
				print_bits(11, r);
				printf(")\n");
				*/
				singlerows[i] = r;
				i++;
				if ((r==0x780) || (r==0x400) || (r==0x403) || (r==0x0f0) || (r==0x070))
					printf("%x is valid line\n", r);
			}
	}

	{
		int r1, r2;
		int i;

		for (r1 = 0; r1 < num_valid_singlerows; r1++)
			for (r2 = 0; r2 < num_valid_singlerows; r2++) {
				row[0] = singlerows[r1];
				row[1] = singlerows[r2];
				if (check_columns(2, 0)) {
					num_valid_doublerows++;
					/*
					print_bits(11, row[0]);
					printf("\n");
					print_bits(11, row[1]);
					printf("\n\n");
					*/
				}
			}

		printf("found %d possible sets of two rows\n", num_valid_doublerows);
		doublerows = malloc(num_valid_doublerows * sizeof(int) * 2);

		i = 0;
		for (r1 = 0; r1 < num_valid_singlerows; r1++)
			for (r2 = 0; r2 < num_valid_singlerows; r2++) {
				row[0] = singlerows[r1];
				row[1] = singlerows[r2];
				if (check_columns(2, 0)) {
					doublerows[i*2+0] = r1;
					doublerows[i*2+1] = r2;
					i++;
				}
			}
	}

	{
		int r1, r2;
		int i;

		for (r1 = 0; r1 < num_valid_doublerows; r1++)
			for (r2 = 0; r2 < num_valid_doublerows; r2++) {
				row[0] = singlerows[doublerows[r1*2]];
				row[1] = singlerows[doublerows[r1*2+1]];
				row[2] = singlerows[doublerows[r2*2]];
				row[3] = singlerows[doublerows[r2*2+1]];
				if (check_columns(4, 1)) {
					num_valid_quadruplerows++;
					/*
					print_bits(11, row[0]);
					printf("\n");
					print_bits(11, row[1]);
					printf("\n\n");
					*/
				}
			}

		printf("found %d possible sets of first four rows\n", num_valid_quadruplerows);
		quadruplerows = malloc(num_valid_quadruplerows * sizeof(int) * 2);

		i = 0;
		for (r1 = 0; r1 < num_valid_doublerows; r1++)
			for (r2 = 0; r2 < num_valid_doublerows; r2++) {
				row[0] = singlerows[doublerows[r1*2+0]];
				row[1] = singlerows[doublerows[r1*2+1]];
				row[2] = singlerows[doublerows[r2*2+0]];
				row[3] = singlerows[doublerows[r2*2+1]];
				if (check_columns(4, 1)) {
					quadruplerows[i*2+0] = r1;
					quadruplerows[i*2+1] = r2;
					i++;
					//printf("doublerows %d / %d\n",r1, r2);
					//print_rows(11);
					//printf("\n");
				}
			}

	}

	{
		int r1, r2;
		int i;

		for (r1 = 0; r1 < num_valid_quadruplerows; r1++) {
			//print_progress_2(r1, num_valid_quadruplerows);
			for (r2 = 0; r2 < num_valid_singlerows; r2++) {
				row[0] = singlerows[doublerows[quadruplerows[r1*2+0]*2+0]];
				row[1] = singlerows[doublerows[quadruplerows[r1*2+0]*2+1]];
				row[2] = singlerows[doublerows[quadruplerows[r1*2+1]*2+0]];
				row[3] = singlerows[doublerows[quadruplerows[r1*2+1]*2+1]];
				row[4] = singlerows[r2];
				if (check_columns(5, 1)) {
					num_valid_fiverows++;
					/*
					print_bits(11, row[0]);
					printf("\n");
					print_bits(11, row[1]);
					printf("\n\n");
					*/
				}
			}
		}
		printf("\n");

		printf("found %d possible sets of first five rows\n", num_valid_fiverows);
		fiverows = malloc(num_valid_fiverows * sizeof(int) * 2);

		i = 0;
		for (r1 = 0; r1 < num_valid_quadruplerows; r1++)
			for (r2 = 0; r2 < num_valid_singlerows; r2++) {
				row[0] = singlerows[doublerows[quadruplerows[r1*2+0]*2+0]];
				row[1] = singlerows[doublerows[quadruplerows[r1*2+0]*2+1]];
				row[2] = singlerows[doublerows[quadruplerows[r1*2+1]*2+0]];
				row[3] = singlerows[doublerows[quadruplerows[r1*2+1]*2+1]];
				row[4] = singlerows[r2];
				if (check_columns(5, 1)) {
					fiverows[i*2+0] = r1;
					fiverows[i*2+1] = r2;
					i++;
				}
			}
	}

	{
		int r1, r2;
		int i;

		for (r1 = 0; r1 < num_valid_singlerows; r1++) {
			print_progress_2(r1, num_valid_singlerows);
			if (singlerows[r1] == reverse_bits_11(singlerows[r1])) {
				row[5] = singlerows[r1];
				for (r2 = 0; r2 < num_valid_fiverows; r2++) {
					row[0] = singlerows[doublerows[quadruplerows[fiverows[r2*2+0]*2+0]*2+0]];
					row[1] = singlerows[doublerows[quadruplerows[fiverows[r2*2+0]*2+0]*2+1]];
					row[2] = singlerows[doublerows[quadruplerows[fiverows[r2*2+0]*2+1]*2+0]];
					row[3] = singlerows[doublerows[quadruplerows[fiverows[r2*2+0]*2+1]*2+1]];
					row[4] = singlerows[fiverows[r2*2+1]];
					row[6] = reverse_bits_11(row[4]);
					row[7] = reverse_bits_11(row[3]);
					row[8] = reverse_bits_11(row[2]);
					row[9] = reverse_bits_11(row[1]);
					row[10] = reverse_bits_11(row[0]);
					if (check_columns(9, 1)) {
#if 0
						if ((rand() % 10000) < 2) {
							printf("grid_whitesquares=%d first_white_region_size=%d:\n",
									grid_whitesquares(), first_white_region_size());
							print_rows(11);
							printf("\n");
						}
#endif
						if (first_white_region_size() == grid_whitesquares())  {
							valid_grid[num_valid_grids] =
							         (unsigned long long)row[0]
							       + ((unsigned long long)row[1]<<11)
							       + ((unsigned long long)row[2]<<22)
							       + ((unsigned long long)row[3]<<33)
							       + ((unsigned long long)row[4]<<44)
							       + (((unsigned long long)row[5] & 0x3fll)<<55);

							//if (valid_grid[num_valid_grids]==1145149964959263) {
							//	printf("got grid 1145149964959263: r1=%d r2=%d rows %x %x %x %x %x %x \n", r1, r2, row[0], row[1], row[2], row[3], row[4], row[5]);
							//	printf("  singlerows[%d] = %x\n", r1, singlerows[r1]);
							//}
							num_valid_grids++;

							//if ((rand() % 10000) < 2) {
							//if ((a_rk()==0xff) && (a_bk()==0xf5) && (b_rk()==0x7e) && (b_bk()==0xfd)) {
							//	print_rows(11);
							//	printf("\n");
							//}
						}
					}
				}
			}
		}
		printf("\n");
		
		printf("found %d possible grids\n", num_valid_grids);

		output_valid_grids();

	}




	return 0;
}

