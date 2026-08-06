/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* The smallest MPI program that still exercises a modex.
 *
 * MPI_Init drives one modex fence and MPI_Finalize drives a barrier, so this
 * is the shortest path through the collectives PRRTE actually has to carry
 * for an MPI job - with none of an application's own communication on top to
 * confuse the timing.  Prints the wall time each of the two spent, per rank,
 * so a run can be compared across grpcomm_fence_movement settings.
 *
 * Build with the mpicc of the Open MPI under test; run it under that Open
 * MPI's prterun (or PRRTE's, which is the same thing when Open MPI was
 * configured --with-prrte=external).
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* NOT MPI_Wtime: it may not be called before MPI_Init, and the whole point
 * here is to time MPI_Init itself.  Every container shares one host kernel
 * clock, so these are directly comparable across ranks the same way
 * scaletest's are. */
static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1.0e6;
}

int main(int argc, char **argv)
{
    double t0, t1, t2;
    int rank, size;
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        if (0 == strcmp(argv[i], "--verbose")) {
            verbose = 1;
        }
    }

    t0 = now_ms();
    MPI_Init(&argc, &argv);
    t1 = now_ms();

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (verbose) {
        printf("MPINOOP rank %d/%d init %.3f ms\n", rank, size, t1 - t0);
        fflush(stdout);
    }

    MPI_Finalize();
    t2 = now_ms();

    if (0 == rank) {
        printf("MPINOOP %d ranks init %.3f ms finalize %.3f ms\n",
               size, t1 - t0, t2 - t1);
        fflush(stdout);
    }
    return 0;
}
