#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/io.h>
#include <fcntl.h>
#include <linux/types.h>
#include <time.h>
#include <x86intrin.h>

/*
 * width (& height) of puzzle in squares -- must be an odd number
 * (currently limited to a max of 15x15 because of data sizes)
 *
 * TODO:  make this an input to the program, instead of hard-coded
 */

#define PUZZLE_SIZE 15
#define PRINT_ALL_VALID_GRIDS 0
#define PRINT_ALL_VALID_GRIDS_GRAPHICAL 0

/*
 * This program works with "subgrids" that are 1/8 and 1/4 the size of the puzzle
 * so for a 15x15 grid, it will generate 7x4 and 7x8 "subgrids".
 *
 * The program works with 7x4 and 7x8 subgrids, even though with smaller puzzle sizes
 * the subgrids will have rows/columns of black at the edges
 *
 * Currently the puzzle size must be ODD because the same set of subgrids is used for
 * each quadrant of the puzzle
 *
 * The stuff below shouldn't need to change for different puzzle sizes
 */

#define SUBGRID_ROWS 7
#define SUBGRID_COLS_7x4 4
#define SUBGRID_COLS_7x8 8

#define SUBGRID_ROWS_ACTUAL ((PUZZLE_SIZE - 1) / 2)
#define SUBGRID_COLS_7x4_ACTUAL ((PUZZLE_SIZE + 1) / 4)
#define SUBGRID_COLS_7x8_ACTUAL ((PUZZLE_SIZE + 1) / 2)

#define MAX_SUBGRID_7x4 ((1UL << (SUBGRID_ROWS * SUBGRID_COLS_7x4 )) - 1)
#define ALL_BLACK_SUBGRID_7x4 MAX_SUBGRID_7x4
#define ALL_BLACK_SUBGRID_ROW_7x4 ((1UL << (SUBGRID_COLS_7x4)) -1)
#define ALL_BLACK_SUBGRID_ROW_7x8 ((1UL << (SUBGRID_COLS_7x8)) -1)
#define ALL_BLACK_GRID_ROW ((1UL << (SUBGRID_COLS_7x8 + SUBGRID_ROWS)) - 1)
#define ALL_BLACK_SUBGRID_COL ((1UL << (SUBGRID_ROWS)) -1)


/*
 * To avoid having to store and compare EVERY combination of grids, this
 * program works with subgrids that are roughly 1/4 the size of the grid
 * (they're 7x8 for a 15x15, and it just makes sure that the outer rows 
 * and columns are black for smaller sizes).
 *
 * To avoid having to store and compare every possible 7x8 subgrid, the
 * program just looks at and stores info based on the edge squares of
 * the possible subgrids.
 *
 * It generates keys that represent all the necessary information--which is:
 *
 * (1) How many white squares would be needed in a row or column in the
 * adjacent subgrid for the word lengths to all be 3 letters or more.
 * This information is referred to as the "key" of the subgrid.
 *
 * (2) It will look at regions of non-connecting white squares in the
 * subgrids, and store a "region key" that shows where the different
 * white regions in the subgrid connect to the edge squares that connect
 * to other subgrids, which we can use to make sure that all the white
 * squares in the final grid will be connected to each other.
 *
 * So this program uses the "key" of the subgrids to see if the big 7x8 subgrids
 * will fit together... the key is a number that indicates how many white
 * squares are on that edge of the subgrid, 2 bits per row or column...
 * the 2 bits per row (or column for a bottom key) indicate that row's edge
 * has 0, 1, 2, or 3+ white squares on the relevant edge.
 * So, for example, take the 7x8 subgrid below:
 *
 *      +===+===+===+===+===+===+===+===+
 *      [ # |   |   |   |   | # | # | # | 0
 *      +---+---+---+---+---+---+---+---+
 *      [ # |   |   |   |   | # |   |   | 2
 *      +---+---+---+---+---+---+---+---+
 *      [ # |   |   |   |   |   |   |   | 3+
 *      +---+---+---+---+---+---+---+---+
 *      [ # | # | # | # |   |   |   |   | 3+  right key = 00011111111000b = 0x07f8
 *      +---+---+---+---+---+---+---+---+
 *      [   |   |   | # |   |   |   |   | 3+
 *      +---+---+---+---+---+---+---+---+
 *      [   |   |   | # |   |   | # |   | 1
 *      +---+---+---+---+---+---+---+---+
 *      [   |   |   |   |   |   | # | # | 0
 *      +---+---+---+---+---+---+---+---+
 *        3+  3+  3+  1   3+  3+  0
 *
 *       bottom key = 00111101111111b = 0xf7f
 *
 * It is a valid subgrid because the top & left edges don't need any more
 * white squares to make sure all the words are 3 letters or more.
 * The top and left are the least significant bits in this program.
 * So the right key would be (0 << 12) + (1 << 10) + (3 << 8) + (3 << 6) +
 * (3 << 4) + (2 << 2) + 0, which is 0x03f8.
 *
 * By using keys, we don't have to store and consider every unique subgrid,
 * we can just keep track of the information that we need to count up how
 * how many valid arrangements of subgrids we can make, and this requires
 * far fewer calculations for larger grids than examining every single
 * possible grid (since there are hundreds of trillions of possible 15x15
 * grids...)
 */

/* 
 * keys are 2 bits for each of the rows... for 7x8 subgrids, keys are 14
 * bits, and max key is 16384
 */
#define NUM_KEYS (1 << (2 * SUBGRID_ROWS))
#define MAX_KEY (NUM_KEYS - 1)

/*
 * these masks show which bits of a key are the part of the key from 
 * the highest (or 2nd highest, or 3rd highest) row... the key has two bits
 * for each row, so for a subgrid that's acutally 7x8, the keys will be
 * 14 bits each (2 bits for each of the 7 rows, for the right key, and
 * 2 bits for each of the first 7 columns, for the bottom key...
 * the bottom key doesn't cover the right-most column, because that
 * column doesn't connect to another subgrid, but rather the center square--
 * at least in a grid with an odd number width and height, which the code
 * currently assumes
 */
#define KEY_ROW_MASK_HIGHEST (0x3 << (2 * (SUBGRID_ROWS - 1)))
#define KEY_ROW_MASK_2NDHIGHEST (0x3 << (2 * (SUBGRID_ROWS - 2)))
#define KEY_ROW_MASK_3RDHIGHEST (0x3 << (2 * (SUBGRID_ROWS - 3)))

/*
 * "left" 7x4 sugrids are the top 1/8 of the puzzle... 1/4 the width and 1/2 the height of the puzzle (7x4 for a 15x15)
 * so there will be fewer possible of these, since both the left and top edges of the subgrid are at the edge of the puzzle
 */
int valid_left_7x4_subgrid_count;
int *valid_left_7x4_subgrid;

/*
 * "right" 7x4 subgrids will go immediately to the right of the "left" subgrids--so only the top edge is against the
 * edge of the puzzle
 */
int valid_right_7x4_subgrid_count;
int *valid_right_7x4_subgrid;

int valid_key_count_array[NUM_KEYS];  // each element is count of valid 7x4 subgrids with that key
int valid_key_count;                  // total number of valid keys (should be 2015, but calculated rather than hard-coded)
int *valid_key;                       // (dynamically-allocated) array of only the valid 14-bit keys
int valid_key_index[NUM_KEYS];        // reverse-lookup table--put the key in and get the index of that key into the "valid_key" array

int *valid_7x8_subgrid_count_rk_bk;  // 2D array by right key index and then bottom key index, containing the number of valid 7x8 subgrids containing that key combo
                                     // so valid_7x8_subgrid_count_rk_bk[5][100] would have the number of valid 7x8 subgrids that have right key index of 5 and a
				     // bottom key index of 100 (use the valid_key array to look up the actual key from the index)

/*
 * On a 7x8 subgrid, there can be, at most, 3 unconnected regions of white squares.
 * All white regions in a 7x8 must connect to an edge where they will connect 
 * to the other subgrids--if they don't, then you'll have an isolated region of white squares.
 *
 * Since there are 14 edge squares on a 7x8 subgrid, and words have to be at least 3 letters long plus
 * one black to divide it from the next word, you can have 14/4 = 3 separate white regions that touch the edge... or
 * (edge squares)/4 in general.
 *
 * With any given edge key (rk/bk combination), that given edge could have (from the last paragraph) up to 3 different
 * regions touching the edge (though they may connect internally, we don't know what's going on inside)... so that given
 * rk/bk edge key combo could have up to 5 different region key bitmasks:  XXX XXY XYX YXX XYZ... in fact only seeing up to
 * 4 different region keys for a given rk/bk in a 7x8 subgrid, presumably because there's just not
 * enough space in a 7x8 subgrid to have 5 regions that all have at least 3-square-long words and all of which connect
 * to the edge.
 *
 * Going to hard code that there can be a maximum of 4-regions in a 7x8 subgrid for now--it isn't worth the trouble to make the program
 * figure out the max and then dynamically alloce.  We can hard code a bigger number if we want to support
 * a bigger grid, and the program will throw an error and abort if we don't have enough, so no big deal.
 *
 * Also going to hard code that we won't have more than 5 different possible region keys for any given RK/BK combo (again, we can
 * tweak if we want to support bigger grids, and we'll get an error if we don't have enough).
 */

#define MAX_REGKEYS_PER_RK_BK 5
#define MAX_REGIONS_IN_A_7x8_SUBGRID 4

typedef unsigned short regkey_bitmask;  // this type has to have as many bits as are in the right+bottom edges of a 7x8 subgrid... for actual 7x8, that's 14
struct single_regkey {
	char num_regions;  // really this is redundant, as unused bitmasks will be 0
	regkey_bitmask bitmask_for_region[MAX_REGIONS_IN_A_7x8_SUBGRID];
};

/*
 * change REGKEYS_SAME if the size of the bitmask_for_region array is more than 64 bits!!
 */
#define REGKEYS_SAME(key1,key2) (*(unsigned long long *)((key1).bitmask_for_region) == *(unsigned long long *)((key2).bitmask_for_region)) 
#define ZERO_REGKEY(key) {key->num_regions = 0; *(unsigned long long *)(key->bitmask_for_region) = 0;}

// TODO: put in check to make sure these functions work for the data sizes used!

struct sg_regkeys *regkeys_by_rk_bk;
struct sg_regkeys {
	int num_regkeys;
	struct single_regkey regkey[MAX_REGKEYS_PER_RK_BK];
	unsigned long long num_sgs_with_regkey[MAX_REGKEYS_PER_RK_BK];  // unsigned long long may be overkill here
};


#if PRINT_ALL_VALID_GRIDS==1
struct valid_7x8_subgrid_type {
	int lsg;
	int rsg;
	int rki;
	int bki;
	struct single_regkey regkey;
	int centersquare;
};
struct valid_7x8_subgrid_type *valid_7x8_subgrid;
int *valid_7x8_subgrid_index_by_rk_bk;

int valid7x8compare(const void *elem1, const void *elem2) {
	struct valid_7x8_subgrid_type f = *(struct valid_7x8_subgrid_type *)elem1;
	struct valid_7x8_subgrid_type s = *(struct valid_7x8_subgrid_type *)elem2;
	if (f.rki > s.rki) return 1;
	if (f.rki < s.rki) return -1;
	if (f.bki > s.bki) return 1;
	if (f.bki < s.bki) return -1;
	return 0;	
}

#define MAX_GOOD_GRIDS 25000000
unsigned long long good_grids[MAX_GOOD_GRIDS];
int good_grids_count;

#endif


int num_subgrids_with_given_regcount[8];
int num_rk_bk_with_given_num_regkeys[MAX_REGKEYS_PER_RK_BK];

/*
 * line1x10_ok
 *
 * if bit X is set, then X is a valid line (i.e., it doesn't have one or two white squares surrounded by black squares)
 * pre-calculated for speed
 */

char line1x10_ok[128]; // 1024-bit bitmap

static __always_inline void set_bit(int n, char *addr) {
	addr[n >> 3] |= 1 << (n & 7);
}

static __always_inline int test_bit(int n, char *addr) {
	return addr[n >> 3] & (1 << (n & 7));
}


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

/*
 * these arrays give the "key" of a row/column from the three bits closest to the edge of that row/column
 */
int rightkey[8] = {3, 2, 1, 1, 0, 0, 0, 0};
//int leftkey[8] = {3, 0, 1, 0, 2, 0, 1, 0};


/*
 * subgrid_row / subgrid_col
 *
 * return just the row or column requested from a subgrid
 */
int subgrid_row_7x4(int subgrid, int r) {
	return ( subgrid >> (r * SUBGRID_COLS_7x4) ) & ALL_BLACK_SUBGRID_ROW_7x4;
}

int subgrid_row_7x8(int lsg, int rsg, int r) {
	return subgrid_row_7x4(lsg, r) + (subgrid_row_7x4(rsg, r) << SUBGRID_COLS_7x4);
}

int subgrid_row_7x8_whole(unsigned long long subgrid_7x8, int r) {
	return ( subgrid_7x8 >> (r * SUBGRID_COLS_7x8) ) & ALL_BLACK_SUBGRID_ROW_7x8;
}

int subgrid_col_7x4(int subgrid, int c) {
	int out;

#if SUBGRID_ROWS <3
	fixme i am broken;
#endif
	out = 
		  ((subgrid & (1 << (SUBGRID_COLS_7x4 * 0 + c))) >> (SUBGRID_COLS_7x4 * 0 + c - 0))
		+ ((subgrid & (1 << (SUBGRID_COLS_7x4 * 1 + c))) >> (SUBGRID_COLS_7x4 * 1 + c - 1))
		+ ((subgrid & (1 << (SUBGRID_COLS_7x4 * 2 + c))) >> (SUBGRID_COLS_7x4 * 2 + c - 2))
#if SUBGRID_ROWS >=4
		+ ((subgrid & (1 << (SUBGRID_COLS_7x4 * 3 + c))) >> (SUBGRID_COLS_7x4 * 3 + c - 3))
#if SUBGRID_ROWS >=5
		+ ((subgrid & (1 << (SUBGRID_COLS_7x4 * 4 + c))) >> (SUBGRID_COLS_7x4 * 4 + c - 4))
#if SUBGRID_ROWS >=6
		+ ((subgrid & (1 << (SUBGRID_COLS_7x4 * 5 + c))) >> (SUBGRID_COLS_7x4 * 5 + c - 5))
#if SUBGRID_ROWS >= 7
		+ ((subgrid & (1 << (SUBGRID_COLS_7x4 * 6 + c))) >> (SUBGRID_COLS_7x4 * 6 + c - 6))
#if SUBGRID_ROWS >= 8
		+ ((subgrid & (1 << (SUBGRID_COLS_7x4 * 7 + c))) >> (SUBGRID_COLS_7x4 * 7 + c - 7))
#if SUBGRID_ROWS >= 9
		+ ((subgrid & (1 << (SUBGRID_COLS_7x4 * 8 + c))) >> (SUBGRID_COLS_7x4 * 8 + c - 8))
#if SUBGRID_ROWS >= 10
		fixme
#endif
#endif
#endif
#endif
#endif
#endif
#endif
		;
	return out;
}


int rightkey_7x4(int sg) {
	int r, key;
	
	key = 0;
	for (r = 0; r < SUBGRID_ROWS; r++)
		/*
		 * use the 3 highest (right-most on the grid) columns to generate the key
		 */
		key += rightkey[(subgrid_row_7x4(sg, r) >> (SUBGRID_COLS_7x4 - 3)) & 7] << r*2;
	return key;
}

int bottomkey_7x8(int lsg, int rsg) {
#if (SUBGRID_COLS_7x8 == 8) && (SUBGRID_ROWS == 7)
	return   (rightkey[(subgrid_col_7x4(lsg, 0) & 0x70) >> 4] << 0)
	       + (rightkey[(subgrid_col_7x4(lsg, 1) & 0x70) >> 4] << 2)
	       + (rightkey[(subgrid_col_7x4(lsg, 2) & 0x70) >> 4] << 4)
	       + (rightkey[(subgrid_col_7x4(lsg, 3) & 0x70) >> 4] << 6)
	       + (rightkey[(subgrid_col_7x4(rsg, 0) & 0x70) >> 4] << 8)
	       + (rightkey[(subgrid_col_7x4(rsg, 1) & 0x70) >> 4] << 10)
	       + (rightkey[(subgrid_col_7x4(rsg, 2) & 0x70) >> 4] << 12);
#endif
}


int keysfit(int key1, int key2) {
	/*
	 * This first `if` will just speed up any case where the keys
	 * fit, as long as there aren't exactly 2 white squares in the
	 * same row on both keys (or 0 on both)... this case isn't
	 * _needed_, because all cases will  be covered by the second `if`
	 * (it just might speed things up a bit)
	 */
	if ((key1 & key2) == 0x3FFF)
		return 1;
	//if ((key1 | key2) == 0x0000)
	//	return 1;
	if (   ((unsigned int) ((key1 & 0x0003) + (key2 & 0x0003) - 1) >= (0x0003 - 1))
	    && ((unsigned int) ((key1 & 0x000c) + (key2 & 0x000c) - 1) >= (0x000c - 1))
	    && ((unsigned int) ((key1 & 0x0030) + (key2 & 0x0030) - 1) >= (0x0030 - 1))
#if SUBGRID_ROWS >=4
	    && ((unsigned int) ((key1 & 0x00c0) + (key2 & 0x00c0) - 1) >= (0x00c0 - 1))
#if SUBGRID_ROWS >=5
	    && ((unsigned int) ((key1 & 0x0300) + (key2 & 0x0300) - 1) >= (0x0300 - 1))
#if SUBGRID_ROWS >=6
	    && ((unsigned int) ((key1 & 0x0c00) + (key2 & 0x0c00) - 1) >= (0x0c00 - 1))
#if SUBGRID_ROWS >=7
	    && ((unsigned int) ((key1 & 0x3000) + (key2 & 0x3000) - 1) >= (0x3000 - 1))
#if SUBGRID_ROWS >=8
		fixmeiambroken;;
#endif
#endif
#endif
#endif
#endif
	   )
		return 1;
	return 0;
}

int reverse_bits(int n, int numbits) {
	int b;
	int out;

	out = 0;
	for (b = 0; b < numbits; b++)
		if (n & (1 << b))
			out += 1 << (numbits - b - 1);
	return out;
}

void print_regkey(struct single_regkey regkey) {
	printf("   %d: %x %x %x %x %x\n", regkey.num_regions,
		regkey.bitmask_for_region[0],
		regkey.bitmask_for_region[1],
		regkey.bitmask_for_region[2],
		regkey.bitmask_for_region[3],
		regkey.bitmask_for_region[4]);
}


void print_row(int row, int length) {
	int c;

	for (c = 0; c < length; c++)
		if (row & (1<<c))
			printf("#");
		else
			printf("_");
}

void print_subgrid_7x4(int subgrid) {
	int r;

	for (r = 0; r < SUBGRID_ROWS; r++) {
		print_row(subgrid_row_7x4(subgrid, r), SUBGRID_COLS_7x4);
		printf("\n");
	}
}

void print_subgrid_7x8(int lsg, int rsg) {
	int r,c;

	for (r = 0; r < SUBGRID_ROWS; r++) {
		print_row(subgrid_row_7x4(lsg, r), SUBGRID_COLS_7x4);
		print_row(subgrid_row_7x4(rsg, r), SUBGRID_COLS_7x4);
		printf("\n");
	}
}

void print_subgrid_7x8_whole(unsigned long long subgrid) {
	int r;

	for (r = 0; r < SUBGRID_ROWS; r++) {
		print_row(subgrid_row_7x8_whole(subgrid, r), SUBGRID_COLS_7x8);
		printf("\n");
	}
}

void print_wholegrid(int lsg_a, int rsg_a, int lsg_b, int rsg_b, int csq) {
	int r;
	for (r = 0; r < SUBGRID_ROWS; r++) {
		print_row(subgrid_row_7x4(lsg_a ,r), SUBGRID_COLS_7x4);
		print_row(subgrid_row_7x4(rsg_a, r), SUBGRID_COLS_7x4);
		if (r < SUBGRID_COLS_7x4)
			print_row(reverse_bits(subgrid_col_7x4(lsg_b, r), SUBGRID_ROWS), SUBGRID_ROWS);
		else
			print_row(reverse_bits(subgrid_col_7x4(rsg_b, r - SUBGRID_COLS_7x4), SUBGRID_ROWS), SUBGRID_ROWS);
		printf("\n");
	}
	print_row(subgrid_col_7x4(rsg_b, SUBGRID_COLS_7x4 - 1), SUBGRID_ROWS);
	printf(csq ? "#" : "_");
	print_row(reverse_bits(subgrid_col_7x4(rsg_b, SUBGRID_COLS_7x4 - 1), SUBGRID_ROWS), SUBGRID_ROWS);
	printf("\n");
	for (r = SUBGRID_ROWS - 1; r >= 0; r--) {
		if (r < SUBGRID_COLS_7x4)
			print_row(subgrid_col_7x4(lsg_b, r), SUBGRID_ROWS);
		else
			print_row(subgrid_col_7x4(rsg_b, r - SUBGRID_COLS_7x4), SUBGRID_ROWS);
		print_row(reverse_bits(subgrid_row_7x4(rsg_a, r), SUBGRID_COLS_7x4), SUBGRID_COLS_7x4);
		print_row(reverse_bits(subgrid_row_7x4(lsg_a, r), SUBGRID_COLS_7x4), SUBGRID_COLS_7x4);
		printf("\n");
	}
	printf("\n\n");
}


#if PRINT_ALL_VALID_GRIDS==1
/*
 * unsigned long used for the number, 64 bits is too small for anything over 11x11
 */
#if PUZZLE_SIZE < 12
unsigned long long store_wholegrid_asanumber_in_good_grids(int lsg_a, int rsg_a, int lsg_b, int rsg_b, int centersquare) {
	int r, rr;
	unsigned long long grid;
	unsigned long long row;

	if (good_grids_count >= MAX_GOOD_GRIDS) {
		printf("Too many valid grids found--good_grids array needs to be enlarged!\n");
		return 0;
	}

	grid = 0;
	/*
	 * start at first actual row, don't include all-black rows if full subgrid size isn't being used
	 */
	rr = 0;
	for (r = (SUBGRID_ROWS - SUBGRID_ROWS_ACTUAL); r < SUBGRID_ROWS; r++) {
		row  = subgrid_row_7x4(lsg_a, r);
		row += subgrid_row_7x4(rsg_a, r) << SUBGRID_COLS_7x4;
		if (r < SUBGRID_COLS_7x4)
			row += reverse_bits(subgrid_col_7x4(lsg_b, r), SUBGRID_ROWS) << (SUBGRID_COLS_7x4*2);
		else
			row += reverse_bits(subgrid_col_7x4(rsg_b, r - SUBGRID_COLS_7x4), SUBGRID_ROWS) << (SUBGRID_COLS_7x4*2);
		/*
		 * chop off edges if full subgrid size isn't being used
		 */
		row &= (ALL_BLACK_GRID_ROW >> (SUBGRID_ROWS - SUBGRID_ROWS_ACTUAL));
		row >>= (SUBGRID_ROWS - SUBGRID_ROWS_ACTUAL);

		grid += row << (rr * (SUBGRID_COLS_7x8_ACTUAL + SUBGRID_ROWS_ACTUAL));
		rr++;
	}

	row = subgrid_col_7x4(rsg_b, SUBGRID_COLS_7x4 - 1);
	row += centersquare << (SUBGRID_COLS_7x8 - 1);
	/*
	 * chop off edges if full subgrid size isn't being used
	 */
	row &= (ALL_BLACK_GRID_ROW >> (SUBGRID_ROWS - SUBGRID_ROWS_ACTUAL));
	row >>= (SUBGRID_ROWS - SUBGRID_ROWS_ACTUAL);

	grid += row << (rr * (SUBGRID_COLS_7x8_ACTUAL + SUBGRID_ROWS_ACTUAL));

	good_grids[good_grids_count++] = grid;
	//printf("%lld\n", grid);
	return grid;
}
#endif

int ullcomp (const void *elem1, const void *elem2) {
	unsigned long long f = *((unsigned long long *)elem1);
	unsigned long long s = *((unsigned long long *)elem2);
	if (f>s) return 1;
	if (f<s) return -1;
	return 0;
}

void print_good_grids() {
	int i;

	printf("there were %d valid grids found\n", good_grids_count);
	qsort(good_grids, good_grids_count, sizeof(*good_grids), ullcomp);
	for (i=0; i<good_grids_count; i++)
		printf("%llu\n", good_grids[i]);
}

/*
 * list_all_grids_with_specified_keys
 *
 * for debugging... it will list all the grids with the specified keys and center square values,
 * so they can all be saved/printed.  used to verify that the program works correctly (with 11x11 grids)
 * by comparing all the valid grids with those generated by a completely separate program.
 */

int list_all_grids_with_specified_keys(int A_rki, int A_bki, int B_rki, int B_bki,
			       struct single_regkey *A_regkey, struct single_regkey *B_regkey,
			       int csqs) {
	int i, a_sg_start_idx, b_sg_start_idx, a_sg_idx, b_sg_idx, c;
	struct single_regkey *a_regkey_ptr;
	int matching_grids_found;
	int flag;

	flag = 0;
	matching_grids_found = 0;
	a_sg_start_idx = valid_7x8_subgrid_index_by_rk_bk[valid_key_count * A_rki + A_bki];
	b_sg_start_idx = valid_7x8_subgrid_index_by_rk_bk[valid_key_count * B_rki + B_bki];
	
	/*
	 * loop through all of the A subgrids with the specified right/bottom key indexes
	 */
	a_sg_idx = a_sg_start_idx;
	while ((valid_7x8_subgrid[a_sg_idx].rki == A_rki) && (valid_7x8_subgrid[a_sg_idx].bki == A_bki)) { 
		/*
		 * make sure the region key of this subgrid matches what was specified
		 */
		if (REGKEYS_SAME((valid_7x8_subgrid[a_sg_idx].regkey), *A_regkey)) { 
			/*
			 * loop through all of the B subgrids with the specified right/bottom key indexes
			 */
			b_sg_idx = b_sg_start_idx;
			while ((valid_7x8_subgrid[b_sg_idx].rki == B_rki) && (valid_7x8_subgrid[b_sg_idx].bki == B_bki)) {
				if (REGKEYS_SAME(((valid_7x8_subgrid[b_sg_idx].regkey)), *B_regkey)) {
					/*
					 * loop through the possible center square values (C is a mask--bit 0=white square, bit 1=black square)
					 */
					for (c = 1; c < 4; c <<= 1)
						if (csqs & c) {
							unsigned long long wholegridnumber;
							matching_grids_found++;
#if PRINT_ALL_VALID_GRIDS_GRAPHICAL==1
							print_wholegrid(valid_7x8_subgrid[a_sg_idx].lsg,
									valid_7x8_subgrid[a_sg_idx].rsg,
									valid_7x8_subgrid[b_sg_idx].lsg,
									valid_7x8_subgrid[b_sg_idx].rsg, ((c == 1) ? 0 : 1) );
#endif
							wholegridnumber = store_wholegrid_asanumber_in_good_grids(
												valid_7x8_subgrid[a_sg_idx].lsg,
												valid_7x8_subgrid[a_sg_idx].rsg,
												valid_7x8_subgrid[b_sg_idx].lsg,
												valid_7x8_subgrid[b_sg_idx].rsg, ((c == 1) ? 0 : 1) );
#if 0
							if (flag) {
								printf("\n%lld was from A_rki/A_bki/B_rki/B_bki %x/%x/%x/%x\n", wholegridnumber, A_rki, A_bki, B_rki, B_bki);
								printf("    (A_rk/A_bk/B_rk/B_bk %x %x %x %x)\n",
									valid_key[A_rki],
									valid_key[A_bki],
									valid_key[B_rki],
									valid_key[B_bki]);
								printf("    (A_lsg/A_rsg/B_lsg/B_rsg %x %x %x %x center %d\n",
									valid_7x8_subgrid[a_sg_idx].lsg,
									valid_7x8_subgrid[a_sg_idx].rsg,
									valid_7x8_subgrid[b_sg_idx].lsg,
									valid_7x8_subgrid[b_sg_idx].rsg,
									((c == 1) ? 0 : 1));
								printf("    A_regkey:\n");
								print_regkey(*A_regkey);
								printf("    B_regkey:\n");
								print_regkey(*B_regkey);
							}
#endif
						}
				}
				b_sg_idx++;
			}
		}
		a_sg_idx++;
	}

	return matching_grids_found;
}
#endif


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

	for (l = 0; l < (1 << 10); l++)
		if (check_line_ok_slow(l, 10))
			set_bit(l, line1x10_ok);
}


int check_line_ok_quick(int l) {
	return test_bit(l, line1x10_ok);
}


/*
 * check_subgrid_ok
 * check no violations on horizontal words with two grids adjacent left & right
 * check that it is ok on the top edge of the puzzle
 * returns 0 if subgrid is not ok
 * returns 1 if is ok (may require white squares on the right side, the left side, and/or below it)
 * returns 2 if it is ok to be on the left edge of the puzzle
 */

int check_subgrid_ok_7x4(int subgrid) {
	int r, c;
	int ok_on_leftside;

	ok_on_leftside = 1;
	for (r = 0; r < SUBGRID_ROWS; r++) {
		int sgrow;
		sgrow = subgrid_row_7x4(subgrid, r);
		if (!check_line_ok_quick(sgrow))
			return 0;
		// stick a black square to the left of the line and check again
		if (!check_line_ok_quick(0x1 | (sgrow << 1)))
			ok_on_leftside = 0;
	}
	/*
	 * make sure subgrid is ok with black on top for 7x4 subgrids, since the top edge
	 * will always be against the edge of the puzzle
	 */
	for (c = 0; c < SUBGRID_COLS_7x4; c++)
		if (!check_line_ok_quick( (subgrid_col_7x4(subgrid, c) << 1) | 0x01))
			return 0;

	/*
	 * make sure top X rows and left X columns are black if the puzzle size is smaller
	 * than 15x15... we'll still work with 7x4 & 7x8 subgrids, just with black rows
	 * and columns at the top and left
	 */

	for (r = 0; r < (SUBGRID_ROWS - SUBGRID_ROWS_ACTUAL); r++)
		if (subgrid_row_7x4(subgrid, r) != ALL_BLACK_SUBGRID_ROW_7x4)
			return 0;
	for (c = 0; c < (SUBGRID_COLS_7x8 - SUBGRID_COLS_7x8_ACTUAL); c++)
		if (c < SUBGRID_COLS_7x4) {
			if (subgrid_col_7x4(subgrid, c) != ALL_BLACK_SUBGRID_COL) {
				ok_on_leftside = 0;
				break;
			}
		} else {
			if (subgrid_col_7x4(subgrid, c - SUBGRID_COLS_7x4))
				return 0;
		}

	return 1 + ok_on_leftside;
}

int check_subgrid_ok_7x8(int left_sg, int right_sg) {
	int r;
	/*
	 * we already know that the columns are ok b/c the 7x4 subgrids were checked
	 * we just need to know if the two subgrids fit together
	 * (TODO: check for isolated white squares?)
	 */
	for (r = 0; r < SUBGRID_ROWS; r++)
		if (!check_line_ok_quick(subgrid_row_7x4(left_sg, r) + (subgrid_row_7x4(right_sg, r) << SUBGRID_COLS_7x4)))
			return 0;
	return 1;
}



/*
 * find valid 7x4 subgrids
 *
 * INPUT:  nothing
 * OUTPUTS:  global variables as follows
 *   valid_left_7x4_subgrid_count
 *   valid_left_7x4_subgrid        - array of 7x4 subgrids that could be at top left
 *   valid_right_7x4_subgrid_count
 *   valid_right_7x4_subgrid       - array of 7x4 subgrids that could be to the right of the left 7x4 subgrids
 *
 * (looking at every possible 7x4 subgrid is fast enough)
 *
 * (they won't all be ok on their own--find the ones
 *  that _could_ be ok as part of a larger grid, not
 *  just as standalone 7x4 grids)
 *
 * (It isn't possible to have isolated white squares
 *  in a grid this size--all white squares have to
 *  touch the edge of the 7x4 subgrid)
 *
 *  Check that there are no 1 or 2 letter across or down
 *  words (surrounded by black squares)
 *
 *  Assume that the top of the 7x4 will always be at
 *  the edge of the puzzle
 *
 *  For "left" subgrids, the left side will also be at
 *  the edge of the puzzle
 */

void find_valid_7x4_subgrids(void) {
	int g;
	int res;
	int lcount, rcount;
	int i, k;

	/*
	 * count valid 7x4 subgrids
	 */	
	valid_left_7x4_subgrid_count = 0;
	valid_right_7x4_subgrid_count = 0;
	for (g = 0; g <= MAX_SUBGRID_7x4; g++) {
		res = check_subgrid_ok_7x4(g);
		if (res) {
			valid_key_count_array[rightkey_7x4(g)]++;
			valid_right_7x4_subgrid_count++;
			if (res == 2) {
				valid_left_7x4_subgrid_count++;
			}
		}
	}
	printf("%d valid left and %d valid right 7x4 subgrids out of %d possible\n", valid_left_7x4_subgrid_count, valid_right_7x4_subgrid_count, g);

	/*
	 * count and make array of valid keys
	 */
	for (k = 0; k <= MAX_KEY; k++)
		if (valid_key_count_array[k])
			valid_key_count++;
	printf("%d valid keys found\n", valid_key_count);
	valid_key = calloc(valid_key_count, sizeof(int));
	if (valid_key == NULL) {
		printf("failed to allocate %ld bytes for list of valid keys\n", valid_key_count * sizeof(int));
		exit(0);
	}
	i = 0;
	for (k = 0; k <= MAX_KEY; k++)
		if (valid_key_count_array[k]) {
			valid_key[i] = k;
			valid_key_index[k] = i;
			i++;
		}

	/*
	 * allocate memory for array of valid 7x4 subgrids
	 */
	valid_left_7x4_subgrid = malloc(valid_left_7x4_subgrid_count * sizeof(int));
	if (valid_left_7x4_subgrid == NULL) {
		printf("failed to allocate %ld bytes for list of valid subgrids\n", valid_left_7x4_subgrid_count * sizeof(int));
		exit(0);
	}
	valid_right_7x4_subgrid = malloc(valid_right_7x4_subgrid_count * sizeof(int));
	if (valid_right_7x4_subgrid == NULL) {
		printf("failed to allocate %ld bytes for list of valid subgrids\n", valid_right_7x4_subgrid_count * sizeof(int));
		free(valid_left_7x4_subgrid);
		exit(0);
	}

	/*
	 * populate array of valid 7x4 subgrids
	 */
	lcount = 0;
	rcount = 0;
	for (g = 0; g <= MAX_SUBGRID_7x4; g++) {
		res = check_subgrid_ok_7x4(g);
		if (res) {
			valid_right_7x4_subgrid[rcount++] = g;
			if (res == 2) {
				valid_left_7x4_subgrid[lcount++] = g;
			}
		}
	}
}


unsigned long long seen_7x8_sg, working_7x8_sg; // subgrids used for finding regions

/*
 * singlesquare_bitmask_7x8
 *
 * returns bitmask of the single square at r, c in a 7x8 subgrid
 * (as represented by a single unsigned long long, not as two 7x4 subgrids)
 */
unsigned long long singlesquare_bitmask_7x8(int r, int c) {
	return ((unsigned long long)1 << ((r * SUBGRID_COLS_7x8) + c));
}	

/*
 * edge_bitmask_of_seen_sg
 *
 * return the squares of the "seen" 7x8 subgrid that are along the bottom & right edges
 * (the edges that have to connect to other subgrids)
 *
 * for a 7x8 subgrid that is 14 bits
 * 
 * This is used to generate the region key for the subgrid
 *
 * A region key actually consists of a number of different bitmasks, each of which
 * shows where one of the unconnected regions in the subgrid touch the edge squares.
 *
 * For most subgrids there will only be one region (i.e., all the white squares in that
 * subgrid connect to each other), so there will only be one bitmask.  If there are two
 * regions in the subgrid, that subgrid will have two bitmasks in its region key, and so
 * on.
 *
 */

/*
 * regkey bitmask stuff
 *
 * regkey bitmasks represent the bottom & right edges of a single 7x8 subgrid, used in region keys
 *
 */
#if (SUBGRID_ROWS==7)
const int edge_bitpos_in_7x8sg[14] =    {7, 15, 23, 31, 39, 47, 55,  54, 53, 52, 51, 50, 49, 48};
const int edge_bitpos_in_bitmask[14] =  {0,  1,  2,  3,  4,  5,  6,  14, 13, 12, 11, 10,  9,  8};
const regkey_bitmask regkey_bitmask_next_to_centersquare = 0x40; // regkey bitmask that has a "1" adjacent to the center square
#endif

#if PUZZLE_SIZE<16
/*
 * whole grid edge bitmask stuff
 *
 * the wholegrid_edge_bitmask must be twice the size of the single regkey bitmask
 * it represents all four of the subgrid edges where the subgrids connect (not just the two that touch any single subgrid)
 * these are used to ensure that all of the white squares at the edges connect to each other (i.e., no isolated regions of white squares)
 *
 * the layout (will need to be changed if PUZZLE_SIZE is more than 15) is 32 bits (since a single regkey bitmask is 16 bits):
 * bits 0-7:   top center edge, corresponding to top-left A subgrid's right mask and top-right B subgrid's bottom mask
 * bits 8-15:  left center edge, corresponding to top-left A subgrid's bottom mask and bottom-left B subgrid's right mask
 * bits 16-23: bottom center edge, corresponding to bottom-left B subgrid's right mask and bottom-right A subgrids right mask
 * bits 24-31: right center edge, corresponding to bottom-right A subgrid's bottom mask and top-right B subgrid's right mask
 */

typedef unsigned int wholegrid_edge_bitmask;

/*
 * TL/BL/BR/TR will transform a 7x8 subgrid regkey bitmask into a whole grid edge bitmask, depending on which position the subgrid is in
 */

#define TL(rkbitmask) ((wholegrid_edge_bitmask)rkbitmask)
#define BL(rkbitmask) (((wholegrid_edge_bitmask)rkbitmask) << 8)
#define BR(rkbitmask) (((wholegrid_edge_bitmask)rkbitmask) << 16)
#define TR(rkbitmask) ((((wholegrid_edge_bitmask)rkbitmask) >> 8) | ((wholegrid_edge_bitmask)rkbitmask) << 24)
#endif

/*
 * edges_next_to_centersquare
 *
 * wholegrid edge bitmask with bits set for all 4 squares that are adjacent to the center square
 * set in init_early_stuff... it was throwing an error if i tried to set it here
 * should be = (TL(regkey_bitmask_next_to_centersquare) | BL(regkey_bitmask_next_to_centersquare) | BR(regkey_bitmask_next_to_centersquare) | TR(regkey_bitmask_next_to_centersquare));
 */

wholegrid_edge_bitmask edges_next_to_centersquare; // set in init_early_stuff... can't get it to work here


regkey_bitmask edge_bitmask_of_seen_sg() {
	int i;
	unsigned long long regkey;

	regkey = 0;
	for (i = 0; i < (SUBGRID_ROWS + SUBGRID_COLS_7x8 - 1); i++)
		regkey |= ((seen_7x8_sg & (1ll << edge_bitpos_in_7x8sg[i])) >> edge_bitpos_in_7x8sg[i]) << edge_bitpos_in_bitmask[i];
	return regkey;
}

int whitesquares_in_working_7x8_sg () {
	return (SUBGRID_ROWS * SUBGRID_COLS_7x8) - __builtin_popcountl(working_7x8_sg);
}
int blacksquares_in_seen_7x8_sg() {
	return __builtin_popcountl(seen_7x8_sg);
}

int find_region(int r, int c) {
	int count;

	count = 0;
	// skip if this is a black square (not part of a white region)
	if (working_7x8_sg & singlesquare_bitmask_7x8(r, c)) {
		return 0;
	}
	// skip if we've already put this square in a region
	if (seen_7x8_sg & singlesquare_bitmask_7x8(r, c)) {
		return 0;
	}
	// set bit showing we've put this square in a region
	seen_7x8_sg |= singlesquare_bitmask_7x8(r, c);
	count++;

	// find the remainder of the region
	if (r < (SUBGRID_ROWS - 1)) {
		count += find_region(r + 1, c);
	}
	if (c < (SUBGRID_COLS_7x8 - 1)) {
		count += find_region(r, c + 1);
	}
	if (r > 0) {
		count += find_region(r - 1, c);
	}
	if (c > 0) {
		count += find_region(r, c - 1);
	}

	//return how many white squares are in this region
	return count;
}


/*
 * pre-define the row/col of each edge square in the 7x8 subgrid (for speed)
 */
#if SUBGRID_ROWS==7
int sg_7x8_edge_row[SUBGRID_ROWS + SUBGRID_COLS_7x8 - 1] = {0, 1, 2, 3, 4, 5, 6,  6, 6, 6, 6, 6, 6, 6};
int sg_7x8_edge_col[SUBGRID_ROWS + SUBGRID_COLS_7x8 - 1] = {7, 7, 7, 7, 7, 7, 7,  6, 5, 4, 3, 2, 1, 0};
#endif

/*
 * region_key_7x8
 *
 * generates region_key for a 7x8 subgrid
 *
 * it puts the key in *regkey
 *
 * returns the number of regions found in the subgrid
 * returns -1 if there are isolated white regions within the subgrid
 *
 */
int region_key_7x8(int lsg, int rsg, struct single_regkey *regkey) {
	int r, c, count, i;
	int regions_found;
	int region_size;
	regkey_bitmask seen_bitmask, previous_seen_bitmasks;

	/*
	 * copy lsg/rsg into one 7x8 subgrid & clear "seen" subgrid
	 */
	working_7x8_sg = 0;
	for (r = 0; r < SUBGRID_ROWS; r++) {
		working_7x8_sg += (unsigned long long)subgrid_row_7x8(lsg, rsg, r) << (r * SUBGRID_COLS_7x8);
	}
	seen_7x8_sg = 0;

	/*
	 *  can start loops at 2, because the subgrids we're looking at are known to be good WRT
	 *  word lengths, so there can't be a white square at the top or left edge that doesn't
	 *  also have a white square two squares to the right or below it.
	 */
	ZERO_REGKEY(regkey);
	previous_seen_bitmasks = 0;
	regions_found = 0;
	/*
	 * cycle through all the edges to find regions
	 */
	for (i = 0; i < SUBGRID_ROWS + SUBGRID_COLS_7x8 - 1; i++) {
		region_size = find_region(sg_7x8_edge_row[i], sg_7x8_edge_col[i]);
		seen_bitmask = edge_bitmask_of_seen_sg();
		if (region_size) {
			/*
			 * save the bitmask for this region and increment the number of regions we've found
			 */
			//regions_found++;
			//*regkey |= (raw_bitmask - previous_raw_bitmasks) * regions_found;
			if (regkey->num_regions >= MAX_REGIONS_IN_A_7x8_SUBGRID) {
				printf("Found at least %d regions in a 7x8 subgrid, change MAX_REGIONS_IN_A_7x8_SUBGRID\n", regkey->num_regions + 1);
				exit(0);
			}	
			regkey->bitmask_for_region[regkey->num_regions++] = seen_bitmask - previous_seen_bitmasks;
			previous_seen_bitmasks |= seen_bitmask;
			if (blacksquares_in_seen_7x8_sg() == whitesquares_in_working_7x8_sg()) {
				/*
				 *  found all the white regions, quit looking
				 */
				return regkey->num_regions;
			}
		}
	}
	if (blacksquares_in_seen_7x8_sg() == whitesquares_in_working_7x8_sg()) {
		// this should only be the all-black subgrid
		return regkey->num_regions;
	}
	/*
	 * there's a region of white squares that doesn't touch an edge... no can do
	 */
	regkey->num_regions = -1;
	return -1;
}

/*
 * regkeys_fit
 *
 * Check if all the regions in the "A" (regkeyA) and "B" (regkeyB) subgrids
 * connect together (i.e., the non-connected regions in "A" must be connected
 * together through "B", or else we'll still have regions of white squares that don't
 * connect to each other)
 *
 * Each region key contains bitmasks of the edge squares for each separate region
 * in the subgrid... so if there are two unconnected regions of white squares in the
 * subgrid, the key will have num_regions=2, and there will be two bitmasks:  the first
 * will contain a "1" in the position of the edge squares that contain a white square
 * that's part of the first region, and the second bitmask will contain a "1" in the
 * position of the edge squares that contain a white square that's part of the second
 * region.
 *
 * So if all the white squares in the subgrid connect to each other, there will only be one
 * bitmask in the region key, and all the white squares at the (bottom and right) edges of the
 * subgrid will be "1" in the bitmask.
 *
 * INPUT:  region keys for the A and B subgrids
 *
 * OUTPUT:
 *          (to be consistent with valid_center_square_values:)
 *          3 = all white squares connect, center square can be black or white
 *          1 = all white squares connect, but center square must be white white
 *          0 = all white squares do not connect, regardless of center square
 *
 *          (no "2", because there's no way that the regions will all connect only in the case
 *           of a center black square but not in the case of a center white square)
 *
 */
#if PUZZLE_SIZE<16
int regkeys_fit(struct single_regkey *regkeyA, struct single_regkey *regkeyB) {
	int r;
	//regkey_bitmask edges_connected_to_topleft_region0, all_regions, connected_regions_previous_pass;
	wholegrid_edge_bitmask edges_connected_to_topleft_region0,
			       edges_connected_to_topleft_region0_previousloop,
			       all_whitesquare_edges,
			       csq_connections,
			       x;
	int centersquareiswhite;

	/*
	 * OR together all the region bitmasks to find all of the edge squares touched
	 * by any regions... that will be the target to determine if we are able
	 * to reach all the edge squares when we start with one region's edge
	 * squares and see which edge squares can be reached from there by going through
	 * other white regions that connect to it
	 */
	for (r = 0; r < regkeyA->num_regions; r++) {
		all_whitesquare_edges |= TL(regkeyA->bitmask_for_region[r]);
		all_whitesquare_edges |= BR(regkeyA->bitmask_for_region[r]);
	}
	for (r = 0; r < regkeyB->num_regions; r++) {
		all_whitesquare_edges |= BL(regkeyB->bitmask_for_region[r]);
		all_whitesquare_edges |= TR(regkeyB->bitmask_for_region[r]);
	}

	/*
	 * start with the first region of the top left subgrid (arbitrary), and use the
	 * region bitmasks to connect it to everything to which it can connect,
	 * then see if that's the same as all_whitesquare_edges to see if they all connect
	 */
	edges_connected_to_topleft_region0 = regkeyA->bitmask_for_region[0];

	for (centersquareiswhite = 0; centersquareiswhite <= 1; centersquareiswhite++) {
		csq_connections = centersquareiswhite ? edges_next_to_centersquare : 0;
		/*
		 * loop as long as we keep connecting more regions 
		 */
		do {
			edges_connected_to_topleft_region0_previousloop = edges_connected_to_topleft_region0;
			/*
			 * connect in B subgrid's regions (TR and BL)
			 */
			for (r = 0; r < regkeyB->num_regions; r++) {
				x = TR(regkeyB->bitmask_for_region[r]);
				if (x & csq_connections) x |= csq_connections;
				if (edges_connected_to_topleft_region0 & x)
					edges_connected_to_topleft_region0 |= x;
				x = BL(regkeyB->bitmask_for_region[r]);
				if (x & csq_connections) x |= csq_connections;
				if (edges_connected_to_topleft_region0 & x)
					edges_connected_to_topleft_region0 |= x;
			}
			/*
			 * connect in A subgrid's regions (TL and BR)
			 */
			for (r = 0; r < regkeyA->num_regions; r++) {
				x = TL(regkeyA->bitmask_for_region[r]);
				if (x & csq_connections) x |= csq_connections;
				if (edges_connected_to_topleft_region0 & x)
					edges_connected_to_topleft_region0 |= x;
				x = BR(regkeyA->bitmask_for_region[r]);
				if (x & csq_connections) x |= csq_connections;
				if (edges_connected_to_topleft_region0 & x)
					edges_connected_to_topleft_region0 |= x;
			}
			/*
			 * check if all regions have connected up yet
			 */
			if (edges_connected_to_topleft_region0 == all_whitesquare_edges) {
				/*
				 * they all connect!  return value based on whether center square has to be white or if it can be white or black
				 * (3 = center can be black or white, 1 = center must be white for all regions to connect)
				 */
				return centersquareiswhite ? 1 : 3;
			}
		} while (edges_connected_to_topleft_region0_previousloop != edges_connected_to_topleft_region0);
	}

	return 0;
}
#endif

/*
 * find valid 7x8 subgrids (by combining the already-found valid 7x4 subgrids)
 *
 * INPUT:
 *   valid_left_7x4_subgrid        (global -- array of valid left 7x4 subgrids)
 *   valid_left_7x4_subgrid_count  (global -- size of this array)
 *   valid_right_7x4_subgrid       (global -- array of valid right 7x4 subgrids)
 *   valid_right_7x4_subgrid_count (global -- size of this array)
 *
 *   valid_key_index  (global -- reverse of above, so you can look up the index of a key given the key value
 *
 * OUTPUT:
 *   valid_7x8_subgrid_count_rk_bk (global -- count of valid 7x8 subgrids with a given right and bottom key)
 *   regkeys_by_rk_bk              (2D array of pointers to the structs sg_regkeys for each rk/bk, which contains
 *                                  a list of the possible region keys for this rk/bk, along with a count of how
 *                                  many 7x8 subgrids have each of those region keys) 
 *
 * again nothing fancy is needed--it is fast enough to just stick every possible
 * combination of valid 7x4 grids next to each other, and see which ones still
 * meet the requirement that all across and down words are 3+ letters
 */

void find_valid_7x8_subgrids(void) {
	int lsgidx, rsgidx;
	unsigned long long count;
	int rightkey_index, bottomkey_index;
	int num_regions;
	struct single_regkey regkey;
	//unsigned long long regkey;
	struct sg_regkeys *regkeys_for_this_rk_bk;
	int i;

	/*
	 * allocate memory for 2D array that will hold count of 7x8 subgrids with given right- and bottom-keys
	 */
	valid_7x8_subgrid_count_rk_bk = calloc(valid_key_count, valid_key_count * sizeof(int));
	if (valid_key == NULL) {
		printf("failed to allocate %ld bytes for 2D array of subgrid counts\n", valid_key_count * valid_key_count * sizeof(int));
		exit(0);
	}
	/*
	 * allocate memory for 2D array that will hold list of region keys found for a given rk/bk
	 */
	regkeys_by_rk_bk = calloc(valid_key_count, valid_key_count * sizeof(struct sg_regkeys));
	if (valid_key == NULL) {
		printf("failed to allocate %ld bytes for 2D array for regkeys\n", valid_key_count * valid_key_count * sizeof(struct sg_regkeys));
		exit(0);
	}

	count = 0;
	for (lsgidx = 0; lsgidx < valid_left_7x4_subgrid_count; lsgidx++)
		for (rsgidx = 0; rsgidx < valid_right_7x4_subgrid_count; rsgidx++) {
			if (check_subgrid_ok_7x8(valid_left_7x4_subgrid[lsgidx], valid_right_7x4_subgrid[rsgidx])) {
				/*
				 * here we have a valid 7x8 subgrid as far as all words are 3+ letters... now
				 * get the region key (which also will reject 7x8 subgrid that has isolated white regions)
				 */
				num_regions = region_key_7x8(valid_left_7x4_subgrid[lsgidx], valid_right_7x4_subgrid[rsgidx], &regkey);
				/*
				 * num_regions will be -1 if the subgrid has an region of isolated white squares--exclude those
				 */
				if (num_regions >= 0) {
					//debug -- count how many 7x8 subgrids have a given number of regions...
					num_subgrids_with_given_regcount[num_regions]++;
					if (num_regions==0) printf("num_regions 0 for lsgidx rsgidx %x %x\n", lsgidx, rsgidx);

					// here we've found a valid 7x8 subgrid, with no isolated white squares
					count++;
					rightkey_index = valid_key_index[rightkey_7x4(valid_right_7x4_subgrid[rsgidx])];
					bottomkey_index = valid_key_index[bottomkey_7x8(valid_left_7x4_subgrid[lsgidx], valid_right_7x4_subgrid[rsgidx])];
					(*(valid_7x8_subgrid_count_rk_bk + (valid_key_count * rightkey_index + bottomkey_index)))++;

					/*
					 * add this region key to the struct sg_regkeys for this rk/bk if it hasn't already been found
					 * or increment the count of subgrids with this region key if it has already been found
					 */
					regkeys_for_this_rk_bk = regkeys_by_rk_bk + (valid_key_count * rightkey_index + bottomkey_index);
					for (i = 0; i < MAX_REGKEYS_PER_RK_BK; i++)
						if (REGKEYS_SAME(regkeys_for_this_rk_bk->regkey[i], regkey)) {
							/*
							 * already had this regkey on this rk/bk, just increment the count
							 */
							regkeys_for_this_rk_bk->num_sgs_with_regkey[i]++;
							break;
						} else if (regkeys_for_this_rk_bk->regkey[i].num_regions == 0) {
							/*
							 *  empty slot... put the new reg key here
							 */
							regkeys_for_this_rk_bk->regkey[i] = regkey;      // redundant if we already found this key, but this is cheaper than a comparea
							regkeys_for_this_rk_bk->num_sgs_with_regkey[i] = 1;
							regkeys_for_this_rk_bk->num_regkeys++;
							break;
						}

					if (i == MAX_REGKEYS_PER_RK_BK) {
						/*
						 * sanity check in case MAX_REGKEYS_PER_RK_BK is too small for this grid size
						 */
						printf("Too many regkeys for rk/bk %x/%x... increase MAX_REGKEYS_PER_RK_BK... regkeys found:\n",
							valid_key[rightkey_index], valid_key[bottomkey_index]);
						print_regkey(regkey);
						for (i = 0; i < MAX_REGKEYS_PER_RK_BK; i++)
							print_regkey(regkeys_for_this_rk_bk->regkey[i]);
						exit(0);
					}

				}
			} 
		}


#if PRINT_ALL_VALID_GRIDS==1 

	/*
	 * for debugging!
	 *
	 * store all the valid 7x8 subgrids along with their keys and regkeys (for debug... too much to store for a 15x15)
	 * (would be rather a lot of memory to store for PUZZLE_SIZE==15, and it isn't needed but for debugging)
	 */

	printf("storing, sorting, and indexing all 7x8 subgrids with their keys (for debugging)\n");
	valid_7x8_subgrid = calloc(count, count * sizeof(struct valid_7x8_subgrid_type));
	i = 0;
	for (lsgidx = 0; lsgidx < valid_left_7x4_subgrid_count; lsgidx++)
		for (rsgidx = 0; rsgidx < valid_right_7x4_subgrid_count; rsgidx++) {
			if (check_subgrid_ok_7x8(valid_left_7x4_subgrid[lsgidx], valid_right_7x4_subgrid[rsgidx])) {
				/*
				 * here we have a valid 7x8 subgrid as far as all words are 3+ letters... now
				 * get the region key (which also will reject 7x8 subgrid that has isolated white regions)
				 */
				num_regions = region_key_7x8(valid_left_7x4_subgrid[lsgidx], valid_right_7x4_subgrid[rsgidx], &regkey);
				/*
				 * num_regions will be 0 if the subgrid has an region of isolated white squares--exclude those
				 */
				if (num_regions >= 0) {
					//debug -- count how many 7x8 subgrids have a given number of regions...

					// here we've found a valid 7x8 subgrid, with no isolated white squares
					rightkey_index = valid_key_index[rightkey_7x4(valid_right_7x4_subgrid[rsgidx])];
					bottomkey_index = valid_key_index[bottomkey_7x8(valid_left_7x4_subgrid[lsgidx], valid_right_7x4_subgrid[rsgidx])];
					valid_7x8_subgrid[i].lsg = valid_left_7x4_subgrid[lsgidx];
					valid_7x8_subgrid[i].rsg = valid_right_7x4_subgrid[rsgidx];
					valid_7x8_subgrid[i].regkey = regkey;
					valid_7x8_subgrid[i].rki = rightkey_index;
					valid_7x8_subgrid[i].bki = bottomkey_index;
#if 1
					printf("setting valid_7x8_subgrid[%d] lsg/rsg %x/%x lsgi/rsgi %x/%x rk/bk %x/%x rki/bki %x/%x regkey %llx\n",
							i,
							valid_left_7x4_subgrid[lsgidx], valid_right_7x4_subgrid[rsgidx],
							lsgidx, rsgidx,
							rightkey_7x4(valid_right_7x4_subgrid[rsgidx]),
							bottomkey_7x8(valid_left_7x4_subgrid[lsgidx], valid_right_7x4_subgrid[rsgidx]),
							rightkey_index, bottomkey_index,
							*(unsigned long long *)(regkey.bitmask_for_region));
#endif
					i++;
				}
			} 
		}

	/*
	 * must sort the list of subgrids by right/bottom key indexes so it's easy
	 * to locate all valid subgrids by their keys
	 */
	qsort((void *)valid_7x8_subgrid, count, sizeof(struct valid_7x8_subgrid_type), valid7x8compare);

	/*
	 * generate 2D array that will tell us where in the valid_7x8_subgrid array
	 * we can find the start of the subgrids with a given rki/bki... since the list
	 * is sorted we just need to print them all starting there until the keys don't match
	 */
	{
		int subgrid_list_index;

		valid_7x8_subgrid_index_by_rk_bk = calloc(valid_key_count, valid_key_count * sizeof(int));
		rightkey_index = -1;
		bottomkey_index = 0;	
		for (i = 0; i < count; i++) {
			if (((valid_7x8_subgrid[i].rki != rightkey_index) || (valid_7x8_subgrid[i].bki != bottomkey_index))) {
				rightkey_index = valid_7x8_subgrid[i].rki;
				bottomkey_index = valid_7x8_subgrid[i].bki;
				subgrid_list_index = i;
			}
			(*(valid_7x8_subgrid_index_by_rk_bk + (valid_key_count * rightkey_index + bottomkey_index))) = subgrid_list_index;
		}
	}
#endif


	printf("found %lld valid 7x8 subgrids\n", count);
	for (i = 0; i < 6; i++) printf("7x8 subgrids with %d region(s): %d\n", i, num_subgrids_with_given_regcount[i]);

	for (rightkey_index = 0; rightkey_index < valid_key_count; rightkey_index++)
		for (bottomkey_index = 0; bottomkey_index < valid_key_count; bottomkey_index++) {
			if (*(valid_7x8_subgrid_count_rk_bk + (valid_key_count * rightkey_index + bottomkey_index)) > 0) {
				regkeys_for_this_rk_bk = regkeys_by_rk_bk + (valid_key_count * rightkey_index + bottomkey_index);
				num_rk_bk_with_given_num_regkeys[regkeys_for_this_rk_bk->num_regkeys]++;
			}
		}
	for (i = 0; i < MAX_REGKEYS_PER_RK_BK; i++) printf("rk_bks with %d num_regkeys: %d\n", i, num_rk_bk_with_given_num_regkeys[i]);

}

/*
 * missing_key
 *
 * INPUT: the RK (right key) of a subgrid
 *          (which gives the number of white squares at the end of each of the SUBGRID_ROWS rows)
 * OUTPUT: the BK (bottom key) for the rightmost column (just that one column) of the subgrid
 *
 * used to determine whether we can have white, black, both, or neither in the center square
 * that isn't covered by the four 7x8 subgrids
 */
int missing_key(int rk) {
	int bk, key_highest_row, key_2ndhighest_row, key_3rdhighest_row;

	key_highest_row =    rk & KEY_ROW_MASK_HIGHEST;
	key_2ndhighest_row = rk & KEY_ROW_MASK_2NDHIGHEST;
	key_3rdhighest_row = rk & KEY_ROW_MASK_3RDHIGHEST;

	bk = 0;
	if (key_highest_row) {
		bk = 1;
		if (key_2ndhighest_row) {
			bk = 2;
			if (key_3rdhighest_row) {
				bk = 3;
			}
		}
	}
	return bk;
}

/*
 * valid_center_square_values
 *
 * INPUT: the RKs (right side keys) for the A and B subgrids
 * OUTPUT: 0 = center square can be neither white nor black
 *         1 = center square can be white
 *         2 = center square can be black
 *         3 = center square can be either black or white
 *
 * to see if the center square can be white and/or black (or neither),
 * we have to figure out the "key" (the number of white squares: 0, 1, 2, or 3+)
 * for the squares above, below, right, and left of the center... but we don't 
 * store this as part of the key directly--the key only stores this info for the
 * other columns (the ones that have to fit with the other subgrid)... but we can
 * find this key by looking at how many white squares there are in the bottom
 * three rows of the right keys of the A and B subgrids
 */

int valid_center_square_values(int A_rightkey, int B_rightkey) {
	int white_squares_above_and_below_center, white_squares_right_and_left_of_center;
	int center_can_be_black, center_can_be_white;

	white_squares_above_and_below_center = missing_key(A_rightkey);
	white_squares_right_and_left_of_center = missing_key(B_rightkey);

	center_can_be_black = 2;
	if (   (white_squares_above_and_below_center == 1)
	    || (white_squares_above_and_below_center == 2)
	    || (white_squares_right_and_left_of_center == 1)
	    || (white_squares_right_and_left_of_center == 2))
		center_can_be_black = 0;

	center_can_be_white = 0;
	if (   (white_squares_above_and_below_center >= 1)
	    && (white_squares_right_and_left_of_center >= 1))
		center_can_be_white = 1;

	return center_can_be_black + center_can_be_white; 
}

void init_early_stuff (void) {
#if 0	
	{
		time_t t;
		srand((unsigned) time(&t));
	}
#endif

	if (sizeof(regkey_bitmask)*8 < (SUBGRID_ROWS + SUBGRID_COLS_7x8 - 1)) {
		printf("Size of regkey_bitmask is only %ld bits, needs to be %d bits\n", sizeof(regkey_bitmask), (SUBGRID_ROWS + SUBGRID_COLS_7x8 - 1));
		exit(0);
	}

	edges_next_to_centersquare = (TL(regkey_bitmask_next_to_centersquare) | BL(regkey_bitmask_next_to_centersquare) | BR(regkey_bitmask_next_to_centersquare) | TR(regkey_bitmask_next_to_centersquare));
}

/*
 *     #   #    #     ###   #   #
 *     ## ##   # #     #    ##  #
 *     # # #  #   #    #    # # #
 *     #   #  #####    #    #  ##
 *     #   #  #   #   ###   #   #
 */

int main(int argc, const char *argv[]) {

	/*
	 * do some sanity checks on data sizes to make sure they fit
	 * initialize random number seed if we need it
	 * etc.
	 */
	init_early_stuff();

	/*
	 * pre-calculate which 1x10 lines are ok
	 */

	init_line_ok_array();

	printf("finding 7x4 subgrids...\n");
	find_valid_7x4_subgrids();
	printf("finding 7x8 subgrids...\n");
	find_valid_7x8_subgrids();

	/*
	 * find valid whole grids (by combining the already-found 7x8 subgrids)
	 *
	 * each 7x8 subgrid will fill a quadrant, which will fill everything but the center square
	 *   (whether black, white, or both is allowed in the center can be determined by looking at
	 *    the right key of subgrids "A" and "B")
	 * each 7x8 subgrid will be rotated 90 degrees when in the next quadrant, so the top and left
	 *   sides of each 7x8 subgrid will always be along the edge of the puzzle... this allowed
	 *   us to eliminate any 7x8 subgrids that didn't meet that requirement
	 * the bottom two subgrids must be the same as the top 2, to maintain 180-degree rotational symmetry
	 *
	 * so we have to cycle through every possible right key and bottom key for "A", and make sure those
	 *   both fit with the bottom key and right key (respectively) of "B"... make sure all the white
	 *   squares connect up (using the region keys), and consider whether the
	 *   center square can be white, black, or either
	 *
	 *
	 *   +------------------------------+
	 *   |m m m m m m m m { m m m m m } | 
	 *   |{ 0           } {         0 } | 
	 *   |{             R B           } | 
	 *   |{  subgrid    K K  subgrid  } | 
	 *   |{      "A"    } {    "B"    } | 
	 *   |{             } {           } | 
	 *   |w w w B-K w w w {           } | 
	 *   |{ m m R-K m } C { w w R-K w } | 
	 *   |{           } m m m m B-K m m | 
	 *   |{           } {             } | 
	 *   |{  subgrid  B R   subgrid   } | 
	 *   |{    "B"    K K     "A"     } | 
	 *   |{           } {             } | 
	 *   |{ 0         } {           0 } | 
	 *   |{ w w w w w } w w w w w w w w | 
	 *   +------------------------------+
	 */

	printf("calculating number of valid whole grids...\n");
	{
		int A_rightkeyidx, A_bottomkeyidx, B_rightkeyidx, B_bottomkeyidx;
		int A_regkeyidx, B_regkeyidx;
		struct sg_regkeys *A_regkeys, *B_regkeys;
		int csqs, regkeysfit;
		unsigned long long count;

		/*
		 * cycle through all the possible key combos for the "A" and "B" subgrids,
		 * check if the keys fit (A's rightkey with B's bottom key, and B's bottom
		 * key with A's right key), check if the white squares all connect,
		 * and finally add however many subgrids have those keys/regkeys to the total count of valid grids
		 *
		 * (also consider whether the center square can be black and/or white, since it isn't part of a subgrid)
		 */
		count = 0;
		/*
		 * start by cycling through all A-rightkeys and B-bottomkeys, looking for keys that fit together...
		 */
		for (A_rightkeyidx = 0; A_rightkeyidx < valid_key_count; A_rightkeyidx++) {
			//print_progress((double)A_rightkeyidx / (double)valid_key_count);
			for (B_bottomkeyidx = 0; B_bottomkeyidx < valid_key_count; B_bottomkeyidx++) {
				/*
				 * check that the A and B subgrids fit together at the A-right / B-bottom key interface
				 */
				if (keysfit(valid_key[A_rightkeyidx], valid_key[B_bottomkeyidx])) {
					/*
					 * here we've found an A-rightkey that fits with a B-bottomkey... now cycle through all of the possible B-rightkeys
					 */
					for (B_rightkeyidx = 0; B_rightkeyidx < valid_key_count; B_rightkeyidx++) {
						/*
						 * make sure there are subgrids with this RK/BK combo for the B subgrid...
						 */
						if (*(valid_7x8_subgrid_count_rk_bk + valid_key_count * B_rightkeyidx + B_bottomkeyidx) > 0) {
							/*
							 * make sure there is a possible value for the center square with this RK/BK combo for the B subgrid...
							 */
							csqs = valid_center_square_values(valid_key[A_rightkeyidx], valid_key[B_rightkeyidx]);
							if (csqs) {
								for (A_bottomkeyidx = 0; A_bottomkeyidx < valid_key_count; A_bottomkeyidx++) {
									/*
									 * make sure there are subgrids with this RK/BK combo for the A subgrid...
									 */
									if (*(valid_7x8_subgrid_count_rk_bk + valid_key_count * A_rightkeyidx + A_bottomkeyidx) > 0) {
										/*
										 * check that the A and B subgrids fit together at the A-bottom / B-right key interface
										 */
										if (keysfit(valid_key[A_bottomkeyidx], valid_key[B_rightkeyidx])) {
											/*
											 * Here we have values for A's and B's right and bottom keys,
											 * we know they fit together, and that they allow at least one
											 * color for the center square!
											 *
											 * Next cycle through the region keys (to make sure all the white squares connect)
											 */
											A_regkeys = regkeys_by_rk_bk + (valid_key_count * A_rightkeyidx + A_bottomkeyidx);
											B_regkeys = regkeys_by_rk_bk + (valid_key_count * B_rightkeyidx + B_bottomkeyidx);

											for (A_regkeyidx = 0; A_regkeyidx < A_regkeys->num_regkeys; A_regkeyidx++)
												for (B_regkeyidx = 0; B_regkeyidx < B_regkeys->num_regkeys; B_regkeyidx++) {
													regkeysfit = regkeys_fit(&A_regkeys->regkey[A_regkeyidx], &B_regkeys->regkey[B_regkeyidx]);
													// bits 0/1 of regkeysfit and csqs are whether center square can be black/white
													count +=   A_regkeys->num_sgs_with_regkey[A_regkeyidx]
														 * B_regkeys->num_sgs_with_regkey[B_regkeyidx]
														 * __builtin_popcount(csqs & regkeysfit);
													#if PRINT_ALL_VALID_GRIDS==1
													{
														int count_increased, printed;
#if 0
														if (   (A_rightkeyidx == 0) && (B_bottomkeyidx == 0)
														    && (B_rightkeyidx == 0) && (A_bottomkeyidx == 0)) {
															printf("<==gotvalid==> a_rki/a_bki/b_rki/b_bki %x/%x/%x/%x\n",
																	A_rightkeyidx, A_bottomkeyidx, B_rightkeyidx, B_bottomkeyidx);
															printf("<============> csqs %d regkeysfit %d\n", csqs, regkeysfit);
															printf("<============> A_regkey\n");
															print_regkey(A_regkeys->regkey[A_regkeyidx]);
															printf("<============> B_regkey\n");
															print_regkey(B_regkeys->regkey[B_regkeyidx]);
															printf("<=============================>\n");
														}
#endif
														count_increased =   A_regkeys->num_sgs_with_regkey[A_regkeyidx]
																  * B_regkeys->num_sgs_with_regkey[B_regkeyidx]
														 		  * __builtin_popcount(csqs & regkeysfit);
														if (count_increased > 0) {
															printed = list_all_grids_with_specified_keys(
																	  A_rightkeyidx, A_bottomkeyidx,
																	  B_rightkeyidx, B_bottomkeyidx,
																	  &A_regkeys->regkey[A_regkeyidx],
																	  &B_regkeys->regkey[B_regkeyidx],
																	  (csqs & regkeysfit));
														}
													}
													#endif
												}
										}
									}
								}
							}
						}
					}
				}
			}
		}
		printf("\n");

		printf("found %lld total valid grids\n", count);
	}

#if PRINT_ALL_VALID_GRIDS==1
	print_good_grids();
#endif
	return 0;
}

