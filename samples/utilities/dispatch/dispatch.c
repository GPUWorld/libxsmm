/******************************************************************************
** Copyright (c) 2016-2019, Intel Corporation                                **
** All rights reserved.                                                      **
**                                                                           **
** Redistribution and use in source and binary forms, with or without        **
** modification, are permitted provided that the following conditions        **
** are met:                                                                  **
** 1. Redistributions of source code must retain the above copyright         **
**    notice, this list of conditions and the following disclaimer.          **
** 2. Redistributions in binary form must reproduce the above copyright      **
**    notice, this list of conditions and the following disclaimer in the    **
**    documentation and/or other materials provided with the distribution.   **
** 3. Neither the name of the copyright holder nor the names of its          **
**    contributors may be used to endorse or promote products derived        **
**    from this software without specific prior written permission.          **
**                                                                           **
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS       **
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT         **
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR     **
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT      **
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,    **
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED  **
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR    **
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    **
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING      **
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS        **
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.              **
******************************************************************************/
/* Hans Pabst (Intel Corp.)
******************************************************************************/
#include <libxsmm.h>

#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(push,target(LIBXSMM_OFFLOAD_TARGET))
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#if defined(_OPENMP)
# include <omp.h>
#endif
#if defined(__MKL)
# include <mkl.h>
#endif
#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(pop)
#endif

#if !defined(MKLJIT) && defined(mkl_jit_create_dgemm)
# define MKLJIT
#endif
#if !defined(INTEL_MKL_VERSION) || (20190003 <= INTEL_MKL_VERSION)
# define CHECK
#endif
#if !defined(MAXSIZE)
# define MAXSIZE LIBXSMM_MAX_M
#endif


typedef struct triplet { libxsmm_blasint m, n, k; } triplet;

LIBXSMM_INLINE void unique(triplet* mnk, int* size)
{
  if (NULL != mnk && NULL != size && 0 < *size) {
    triplet *const first = mnk, *last = mnk + ((size_t)*size - 1), *i;
    for (i = mnk + 1; mnk < last; ++mnk, i = mnk + 1) {
      while (i <= last) {
        if (i->m != mnk->m || i->n != mnk->n || i->k != mnk->k) {
          i++; /* skip */
        }
        else { /* copy */
          *i = *last--;
        }
      }
    }
    *size = (int)(last - first + 1);
  }
}


/**
 * This (micro-)benchmark optionally takes a number of dispatches to be performed.
 * The program measures the duration needed to figure out whether a requested matrix
 * multiplication is available or not. The measured duration excludes the time taken
 * to actually generate the code during the first dispatch.
 */
int main(int argc, char* argv[])
{
#if defined(_OPENMP)
  const int max_nthreads = omp_get_max_threads();
#else
  const int max_nthreads = 1;
#endif
  const int default_minsize = 4;
#if !defined(INTEL_MKL_VERSION) || (20190003 <= INTEL_MKL_VERSION)
  const int default_maxsize = MAXSIZE;
#else
  const int default_maxsize = 16;
#endif
  int size_total = LIBXSMM_MAX(1 < argc ? atoi(argv[1]) : 10000/*default*/, 2);
  const int size_local = LIBXSMM_CLMP(2 < argc ? atoi(argv[2]) : 1/*default*/, 1, size_total - 1);
  const int nthreads = LIBXSMM_CLMP(3 < argc ? atoi(argv[3]) : 1/*default*/, 1, max_nthreads);
  const libxsmm_blasint maxsize = LIBXSMM_CLMP(4 < argc ? atoi(argv[4]) : default_maxsize, 1, MAXSIZE);
  const libxsmm_blasint minsize = LIBXSMM_CLMP(5 < argc ? atoi(argv[5]) : default_minsize, 1, maxsize);
  const libxsmm_blasint range = maxsize - minsize;
  double a[LIBXSMM_MAX_M*LIBXSMM_MAX_M];
  double b[LIBXSMM_MAX_M*LIBXSMM_MAX_M];
  double c[LIBXSMM_MAX_M*LIBXSMM_MAX_M];
  double tcall, tdsp0, tdsp1, tcgen;
  libxsmm_timer_tickint start;
  int result = EXIT_SUCCESS;

#if 0 != LIBXSMM_JIT
  if (LIBXSMM_X86_SSE3 > libxsmm_get_target_archid()) {
    fprintf(stderr, "\n\tWarning: JIT support is not available at runtime!\n");
  }
#else
  fprintf(stderr, "\n\tWarning: JIT support has been disabled at build time!\n");
#endif
  LIBXSMM_MATINIT(double, 0, a, maxsize, maxsize, maxsize, 1.0);
  LIBXSMM_MATINIT(double, 0, b, maxsize, maxsize, maxsize, 1.0);
  LIBXSMM_MATINIT(double, 0, c, maxsize, maxsize, maxsize, 1.0);
#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload target(LIBXSMM_OFFLOAD_TARGET)
#endif
  {
    triplet *const rnd = (triplet*)malloc(sizeof(triplet) * size_total);
    const size_t shuffle = libxsmm_shuffle(size_total);
    const double alpha = 1, beta = 1;
    int i;

#if defined(MKLJIT)
    void* *const jitter = malloc(size_total * sizeof(void*));
    if (NULL == jitter) exit(EXIT_FAILURE);
#else
    const int prefetch = LIBXSMM_GEMM_PREFETCH_NONE;
    const int flags = LIBXSMM_GEMM_FLAG_NONE;
#endif
    if (NULL == rnd) exit(EXIT_FAILURE);

    /* generate a set of random numbers outside of any parallel region */
    for (i = 0; i < size_total; ++i) {
      const int r1 = rand(), r2 = rand(), r3 = rand();
      rnd[i].m = (1 < range ? (LIBXSMM_MOD(r1, range) + minsize) : minsize);
      rnd[i].n = (1 < range ? (LIBXSMM_MOD(r2, range) + minsize) : minsize);
      rnd[i].k = (1 < range ? (LIBXSMM_MOD(r3, range) + minsize) : minsize);
#if defined(MKLJIT)
      jitter[i] = NULL;
#endif
    }
    unique(rnd, &size_total);

    printf("Dispatching %i calls %s internal synchronization using %i thread%s...", size_total,
#if (0 != LIBXSMM_SYNC)
      "with",
#else
      "without",
#endif
      1 >= nthreads ? 1 : nthreads,
      1 >= nthreads ? "" : "s");

    /* first invocation may initialize some internals */
    libxsmm_init(); /* subsequent calls are not doing any work */
    start = libxsmm_timer_tick();
    for (i = 0; i < size_total; ++i) {
      /* measure call overhead of an "empty" function (not inlined) */
      libxsmm_init();
    }
    tcall = libxsmm_timer_duration(start, libxsmm_timer_tick());

    /* trigger code generation to subsequently measure only dispatch time */
    for (i = 0; i < size_local; ++i) {
#if defined(MKLJIT)
      mkl_cblas_jit_create_dgemm(jitter,
        MKL_COL_MAJOR, MKL_NOTRANS/*transa*/, MKL_NOTRANS/*transb*/,
        rnd[i].m, rnd[i].n, rnd[i].k, alpha, rnd[i].m, rnd[i].k, beta, rnd[i].m);
      mkl_jit_get_dgemm_ptr(jitter[i]); /* to measure "cached" lookup time (below) */
#else
      libxsmm_dmmdispatch(rnd[i].m, rnd[i].n, rnd[i].k, &rnd[i].m, &rnd[i].k, &rnd[i].m, &alpha, &beta, &flags, &prefetch);
#endif
    }

    /* measure duration for dispatching (cached) kernel */
#if defined(_OPENMP)
#   pragma omp parallel num_threads(nthreads) private(i)
    {
#     pragma omp single
      start = libxsmm_timer_tick();
#     pragma omp for
#else
    {
      start = libxsmm_timer_tick();
#endif
      for (i = 0; i < size_total; ++i) {
        const int j = LIBXSMM_MOD(i, size_local);
#if defined(MKLJIT)
        mkl_jit_get_dgemm_ptr(jitter[j]); /* no "dispatch" just unwrapping the jitter */
#else
        libxsmm_dmmdispatch(rnd[j].m, rnd[j].n, rnd[j].k, &rnd[j].m, &rnd[j].k, &rnd[j].m, &alpha, &beta, &flags, &prefetch);
#endif
      }
    }
    tdsp1 = libxsmm_timer_duration(start, libxsmm_timer_tick());

    /* measure duration for code-generation */
#if defined(_OPENMP)
#   pragma omp parallel num_threads(nthreads) private(i)
    {
#     pragma omp single
      start = libxsmm_timer_tick();
#     pragma omp for
#else
    {
      start = libxsmm_timer_tick();
#endif
      for (i = size_local; i < size_total; ++i) {
#if defined(MKLJIT)
        mkl_cblas_jit_create_dgemm(jitter + i,
          MKL_COL_MAJOR, MKL_NOTRANS/*transa*/, MKL_NOTRANS/*transb*/,
          rnd[i].m, rnd[i].n, rnd[i].k, alpha, rnd[i].m, rnd[i].k, beta, rnd[i].m);
        mkl_jit_get_dgemm_ptr(jitter[i]);
#else
        libxsmm_dmmdispatch(rnd[i].m, rnd[i].n, rnd[i].k, &rnd[i].m, &rnd[i].k, &rnd[i].m, &alpha, &beta, &flags, &prefetch);
#endif
      }
    }
    tcgen = libxsmm_timer_duration(start, libxsmm_timer_tick());

    /* measure dispatching previously generated kernel (likely non-cached) */
#if defined(_OPENMP)
#   pragma omp parallel num_threads(nthreads) private(i)
    {
#     pragma omp single
      start = libxsmm_timer_tick();
#     pragma omp for
#else
    {
      start = libxsmm_timer_tick();
#endif
      for (i = 0; i < size_total; ++i) {
        const int j = (int)LIBXSMM_MOD(shuffle * i, size_total);
#if defined(MKLJIT)
        mkl_jit_get_dgemm_ptr(jitter[j]);
#else
        libxsmm_dmmdispatch(rnd[j].m, rnd[j].n, rnd[j].k, &rnd[j].m, &rnd[j].k, &rnd[j].m, &alpha, &beta, &flags, &prefetch);
#endif
      }
    }
    tdsp0 = libxsmm_timer_duration(start, libxsmm_timer_tick());
#if defined(CHECK)
    { /* calculate l1-norm for manual validation */
      libxsmm_matdiff_info check;
      libxsmm_matdiff_clear(&check);
      for (i = 0; i < size_total; ++i) {
        const int j = (int)LIBXSMM_MOD(shuffle * i, size_total);
        libxsmm_matdiff_info diff;
# if defined(MKLJIT)
        const dgemm_jit_kernel_t kernel = mkl_jit_get_dgemm_ptr(jitter[j]);
# else
        const libxsmm_dmmfunction kernel = libxsmm_dmmdispatch(rnd[j].m, rnd[j].n, rnd[j].k,
          &rnd[j].m, &rnd[j].k, &rnd[j].m, &alpha, &beta, &flags, &prefetch);
# endif
        if (NULL != kernel) {
# if defined(MKLJIT)
          kernel(jitter[j], a, b, c);
# else
          if (LIBXSMM_GEMM_PREFETCH_NONE == prefetch) kernel(a, b, c); else kernel(a, b, c, a, b, c);
# endif
          result = libxsmm_matdiff(&diff, LIBXSMM_DATATYPE(double), rnd[j].m, rnd[j].n, NULL, c, &rnd[j].m, &rnd[j].m);
        }
        else {
          result = EXIT_FAILURE;
        }
        if (EXIT_SUCCESS == result) {
          libxsmm_matdiff_reduce(&check, &diff);
        }
        else {
          printf(" m=%u n=%u k=%u", (unsigned int)rnd[j].m, (unsigned int)rnd[j].n, (unsigned int)rnd[j].k);
          i = size_total + 1; /* break */
        }
      }
      if (i <= size_total) {
        printf(" check=%f\n", check.l1_tst);
      }
      else {
        printf(" <- ERROR!\n");
      }
    }
#endif /*defined(CHECK)*/
    free(rnd); /* release random numbers */
#if defined(MKLJIT) /* release dispatched code */
    for (i = 0; i < size_total; ++i) mkl_jit_destroy(jitter[i]);
    free(jitter); /* release array used to store dispatched code */
#endif
  }

  if (1 < size_total) {
    const int size1 = size_total - size_local;
    tcall /= size_total; tdsp0 /= size_total; tdsp1 /= size_total; tcgen /= size1;
    if (0 < tcall && 0 < tdsp0 && 0 < tdsp1 && 0 < tcgen) {
      printf("\tfunction-call (empty): %.0f ns (%.0f MHz)\n", 1E9 * tcall, 1E-6 / tcall);
      printf("\tdispatch (ro/cached): %.0f ns (%.0f MHz)\n", 1E9 * tdsp1, 1E-6 / tdsp1);
      printf("\tdispatch (ro): %.0f ns (%.0f MHz)\n", 1E9 * tdsp0, 1E-6 / tdsp0);
      if (1E-6 <= tcgen) {
        printf("\tcode-gen (rw): %.0f us (%.0f kHz)\n", 1E6 * tcgen, 1E-3 / tcgen);
      }
      else {
        printf("\tcode-gen (rw): %.0f ns (%.0f MHz)\n", 1E9 * tcgen, 1E-6 / tcgen);
      }
    }
  }
  printf("Finished\n");

  return result;
}

