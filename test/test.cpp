#include "config.hpp"

#include <iostream> // cout
#include <iomanip>  // for setprecision

#ifdef USE_MPI
#include "mpi.h"
#endif

#include <cmath>      // log
#include <chrono>     // timing
#include <algorithm>  // for std::min

#include "omp_patterns.hpp"

template <typename OT>
struct compute {
    OT operator()(size_t iter, size_t end) {
      OT tv = 0;
      for ( ; iter < end; ++iter) {
        tv += log2(iter + 1);
        //ofs << j << std::endl;
      }
      return tv;
    }
};


void printTiming(std::string tag, int rank, int nprocs, int nthreads,
                 const std::chrono::duration<double>& time_span, int iter,
                 double v)
{
  std::cout << tag << "\tMPI rank: " << rank << "/" << nprocs << "\tOMP "
            << nthreads << " threads\ttook " << std::fixed
            << std::setprecision(6) << time_span.count() / iter
            << "s,\tresult = " << v << std::endl;
}

int main(int argc, char* argv[])
{

  int rank = 0, nprocs = 1;
#ifdef USE_MPI

  // initialize MPI
  MPI_Init(&argc, &argv);

  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  if (rank == 0)
  std::cout << "USE_MPI is set" << std::endl;

#endif

#ifdef USE_OPENMP
  if (rank == 0)
  std::cout << "USE_OPENMP is set" << std::endl;
  omp_set_nested(1);
  omp_set_dynamic(0);
#endif


  int nthreads = 1;
  if (argc > 1)
    nthreads = atoi(argv[1]);

  size_t step = 128;
  if (argc > 2)
    step = atoi(argv[2]);

  size_t max = 1000000;
  if (argc > 3)
    max = atoi(argv[3]);

  int iter = 10;
  if (argc > 4)
    iter = atoi(argv[4]);

  RangeType r = RangeType::block_partition(nprocs, rank, 0, max, 0);
  compute<double> op;
  double v = 0.0;

  std::chrono::high_resolution_clock::time_point t1, t2;
  std::chrono::duration<double> time_span;

  /// Workers only, critical
#if defined(USE_MPI)
  MPI_Barrier(MPI_COMM_WORLD);
#endif
  t1 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iter; ++i)
    v = P2P(op, nthreads, r, step);
#if defined(USE_MPI)
  MPI_Barrier(MPI_COMM_WORLD);
#endif
  t2 = std::chrono::high_resolution_clock::now();
  time_span =
      std::chrono::duration_cast<std::chrono::duration<double>>(
          t2 - t1);
  if (rank == 0)
    printTiming("P2P critical:", rank, nprocs, nthreads, time_span, iter, v);



  /// Workers only, atomic  - close to time for parfor.
#if defined(USE_MPI)
  MPI_Barrier(MPI_COMM_WORLD);
#endif
  t1 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iter; ++i)
    v = P2P_atomic(op, nthreads, r, step);
#if defined(USE_MPI)
  MPI_Barrier(MPI_COMM_WORLD);
#endif
  t2 = std::chrono::high_resolution_clock::now();
  time_span =
      std::chrono::duration_cast<std::chrono::duration<double>>(
          t2 - t1);
  if (rank == 0)
    printTiming("P2P atomic:", rank, nprocs, nthreads, time_span, iter, v);


  /// master slave
#if defined(USE_MPI)
  MPI_Barrier(MPI_COMM_WORLD);
#endif
  t1 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iter; ++i)
    v= MasterSlave(op, nthreads, r, step);
#if defined(USE_MPI)
  MPI_Barrier(MPI_COMM_WORLD);
#endif
  t2 = std::chrono::high_resolution_clock::now();
  time_span =
      std::chrono::duration_cast<std::chrono::duration<double>>(
          t2 - t1);
  if (rank == 0)
    printTiming("MS Wait:", rank, nprocs, nthreads, time_span, iter, v);

  /// master slave No Wait
#if defined(USE_MPI)
  MPI_Barrier(MPI_COMM_WORLD);
#endif
  t1 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iter; ++i)
    v= MasterSlaveNoWait(op, nthreads, r, step);
#if defined(USE_MPI)
  MPI_Barrier(MPI_COMM_WORLD);
#endif
  t2 = std::chrono::high_resolution_clock::now();
  time_span =
      std::chrono::duration_cast<std::chrono::duration<double>>(
          t2 - t1);
  if (rank == 0)
    printTiming("MS NoWait:", rank, nprocs, nthreads, time_span, iter, v);

  /// parallel for
#if defined(USE_MPI)
  MPI_Barrier(MPI_COMM_WORLD);
#endif
  t1 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iter; ++i)
    v = ParFor(op, nthreads, r, step);
#if defined(USE_MPI)
  MPI_Barrier(MPI_COMM_WORLD);
#endif
  t2 = std::chrono::high_resolution_clock::now();
  time_span =
      std::chrono::duration_cast<std::chrono::duration<double>>(
          t2 - t1);
  if (rank == 0)
    printTiming("PARFOR:\t", rank, nprocs, nthreads, time_span, iter, v);

  //// serial for
#if defined(USE_MPI)
  MPI_Barrier(MPI_COMM_WORLD);
#endif
  t1 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iter; ++i)
    v = Sequential(op, nthreads, r, step);
#if defined(USE_MPI)
  MPI_Barrier(MPI_COMM_WORLD);
#endif
  t2 = std::chrono::high_resolution_clock::now();
  time_span =
      std::chrono::duration_cast<std::chrono::duration<double>>(
          t2 - t1);
  if (rank == 0)
    printTiming("SEQFOR:\t", rank, nprocs, nthreads, time_span, iter, v);

#ifdef USE_MPI
  MPI_Finalize();

#endif

  return 0;

}

