#ifndef COMMON_H_
#define COMMON_H_

#ifndef X86
#include <athread.h>
#endif

#include <assert.h>

#define NZ(STATEMENT)   assert(STATEMENT != NULL)
#define ZERO(STATEMENT) assert(STATEMENT == 0)

#define LOGD(...) do {int rank;MPI_Comm_rank(MPI_COMM_WORLD, &rank); printf("[%4d]", rank); printf(__VA_ARGS__); }while(0)
#define LOGDS(...) do {int rank;MPI_Comm_rank(MPI_COMM_WORLD, &rank);if (rank==0) {printf("[%4d]", rank); printf(__VA_ARGS__);}}while(0)

#define LOGV(...) do {int rank;MPI_Comm_rank(MPI_COMM_WORLD, &rank); printf("[%4d]", rank); printf(__VA_ARGS__); }while(0)
#define LOGVS(...) do {int rank;MPI_Comm_rank(MPI_COMM_WORLD, &rank);if (rank==0) {printf("[%4d]", rank); printf(__VA_ARGS__);}}while(0)

// __FUNCTION__
#define LINE LOGD("<<<< %s: %s %d\n", __FILE__, __FUNCTION__, __LINE__)
#define LINES LOGDS("<<<< %s: %s %d\n", __FILE__, __FUNCTION__, __LINE__)

#endif
