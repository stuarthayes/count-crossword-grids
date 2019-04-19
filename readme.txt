Two programs to count the number of possible crossword grids.

The rules are that all the words have to be at least 3 letters long,
all the white squares have to connect to each other, and the grids
must be the same if rotated 180 degrees.

t11x11noisolatedregions.c only works for an 11x11 crossword grid.
It works in a relatively unsophisticated way, though it isn't
totally brute force.  It is not fast enough to work on a 15x15
grid, but it is simple enough that its results are more likely
to be correct.

cwkey.c should work for any odd-sized grid that's 15x15 or less
(it would take some tweaking to get it to work for larger grids
because the size of some of the variables would need to be larger).
It actually works with 15x15 grids internally even if a smaller
size is specified, it just puts black squares around the perimeter
to make it effectively a smaller size grid.
It is much more sophisticated, generating valid "subgrids" that
are about 1/4 the puzzle size (7x8 squares), and information about
the subgrid's edges where they connect to the other subgrids is all
that is stored... this dramatically reduces the data that is stored
and the work that has to be done.  It will calculate the number of
possible 15x15 grids in under 10 minutes.  It could be optimized
further (to reduce repeatedly checking if the same "region keys"
fit together by caching which region keys have previously been checked,
since it is so time consuming to check this), but that would only be
worth the effort if it needed to be run on a larger grid size since
it already finishes in 10 minutes or less.

compile with

gcc -O3 -o cwkey cwkey.c

or

gcc -O3 -o t11x11noisolatedregions t11x11noisolatedregions.c


