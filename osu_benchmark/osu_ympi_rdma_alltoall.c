#define BENCHMARK "OSU MPI%s All-to-All Personalized Exchange Latency Test"
/*
 * Copyright (C) 2002-2016 the Network-Based Computing Laboratory
 * (NBCL), The Ohio State University.
 *
 * Contact: Dr. D. K. Panda (panda@cse.ohio-state.edu)
 *
 * For detailed copyright and licensing information, please refer to the
 * copyright file COPYRIGHT in the top level OMB directory.
 */
#include "osu_coll.h"
#include "ympi.h"
#include <assert.h>

void* rbuf[1024];
uint32_t rkey[1024];

int
main (int argc, char *argv[])
{
    int i, numprocs, rank, size;
    double latency = 0.0, t_start = 0.0, t_stop = 0.0;
    double timer=0.0;
    double avg_time = 0.0, max_time = 0.0, min_time = 0.0;
    char * sendbuf = NULL, * recvbuf = NULL;
    YMPI_Rdma_buffer sendbuffer, recvbuffer;

    int po_ret;
    size_t bufsize;

    set_header(HEADER);
    set_benchmark_name("osu_alltoall");
    enable_accel_support();
    po_ret = process_options(argc, argv);

    if (po_okay == po_ret && none != options.accel) {
        if (init_accel()) {
            fprintf(stderr, "Error initializing device\n");
            exit(EXIT_FAILURE);
        }
    }

    MPI_Init(&argc, &argv);
    YMPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &numprocs);

    switch (po_ret) {
        case po_bad_usage:
            print_bad_usage_message(rank);
            MPI_Finalize();
            exit(EXIT_FAILURE);
        case po_help_message:
            print_help_message(rank);
            MPI_Finalize();
            exit(EXIT_SUCCESS);
        case po_version_message:
            print_version_message(rank);
            MPI_Finalize();
            exit(EXIT_SUCCESS);
        case po_okay:
            break;
    }

    if(numprocs < 2) {
        if (rank == 0) {
            fprintf(stderr, "This test requires at least two processes\n");
        }

        MPI_Finalize();
        exit(EXIT_FAILURE);
    }

    if ((options.max_message_size * numprocs) > options.max_mem_limit) {
        options.max_message_size = options.max_mem_limit / numprocs;
    }

    bufsize = options.max_message_size * numprocs;

    {
        /*
            YMPI_Allocate(&sendbuffer, bufsize, YMPI_BUFFER_TYPE_REMOTE_RW);
            YMPI_Get_buffer(sendbuffer, &sendbuf);
            set_buffer(sendbuf, options.accel, 1, bufsize);

            YMPI_Allocate(&recvbuffer, bufsize, YMPI_BUFFER_TYPE_REMOTE_RW);
            YMPI_Get_buffer(recvbuffer, &recvbuf);
            set_buffer(recvbuf, options.accel, 0, bufsize);

            uint32_t my_rkey;
            YMPI_Get_rkey(recvbuffer, &my_rkey);
            
            MPI_Allgather(&my_rkey, 1, MPI_INT, rkey, 1, MPI_INT, MPI_COMM_WORLD);
            MPI_Allgather(&recvbuf, 1, MPI_UNSIGNED_LONG_LONG, rbuf, 1, MPI_UNSIGNED_LONG_LONG, MPI_COMM_WORLD);
        */

        sendbuf = malloc(bufsize);
        recvbuf = malloc(bufsize);
        set_buffer(sendbuf, options.accel, 1, bufsize);
        set_buffer(recvbuf, options.accel, 0, bufsize);
    }

    print_preamble(rank);

    for(size=options.min_message_size; size <= options.max_message_size; size *= 2) {
        if (size > LARGE_MESSAGE_SIZE) {
            options.skip = options.skip_large;
            options.iterations = options.iterations_large;
        }

        MPI_Barrier(MPI_COMM_WORLD);
        timer=0.0;

        for (i=0; i < options.iterations + options.skip ; i++) {
            t_start = MPI_Wtime();
            YMPI_Alltoall_write_ptr(sendbuf, size, recvbuf, size, MPI_COMM_WORLD);
            //YMPI_Alltoall_write(sendbuffer, size, recvbuffer, size, MPI_COMM_WORLD);
            //MPI_Alltoall(sendbuf, size, MPI_CHAR, recvbuf, size, MPI_CHAR, MPI_COMM_WORLD);
            t_stop = MPI_Wtime();

            if (i >= options.skip) {
                timer+=t_stop-t_start;
            }
            MPI_Barrier(MPI_COMM_WORLD);
        }
        latency = (double)(timer * 1e6) / options.iterations;

        MPI_Reduce(&latency, &min_time, 1, MPI_DOUBLE, MPI_MIN, 0,
                MPI_COMM_WORLD);
        MPI_Reduce(&latency, &max_time, 1, MPI_DOUBLE, MPI_MAX, 0,
                MPI_COMM_WORLD);
        MPI_Reduce(&latency, &avg_time, 1, MPI_DOUBLE, MPI_SUM, 0,
                MPI_COMM_WORLD);
        avg_time = avg_time/numprocs;

        print_stats(rank, size, avg_time, min_time, max_time);
        MPI_Barrier(MPI_COMM_WORLD);

        {
            int i;
            for(i=0; i<size; i++) {
                assert(sendbuf[i] == 1);
                if(recvbuf[i] != 1) {
                    printf("Expected recvbuf[i] == 1, but equal to %d\n", (int)recvbuf[i]);
                }
            }
        }
    }

    free_buffer(sendbuf, options.accel);
    free_buffer(recvbuf, options.accel);

    MPI_Finalize();

    if (none != options.accel) {
        if (cleanup_accel()) {
            fprintf(stderr, "Error cleaning up device\n");
            exit(EXIT_FAILURE);
        }
    }

    return EXIT_SUCCESS;
}

/* vi: set sw=4 sts=4 tw=80: */
