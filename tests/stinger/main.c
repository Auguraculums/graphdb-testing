/* -*- mode: C; mode: folding; fill-column: 70; -*- */
#define _XOPEN_SOURCE 600
#define _LARGEFILE64_SOURCE 1
#define _FILE_OFFSET_BITS 64

#include <sys/time.h>
#include <sys/resource.h>

#define R_A(X,...) fprintf(stdout, "RSLT: " X, __VA_ARGS__);
#define R(X) R_A(X,NULL)

#if defined(_OPENMP)
#include "omp.h"
#endif

#include "stinger-atomics.h"
#include "stinger-utils.h"
#include "stinger.h"
#include "timer.h"
#include "xmalloc.h"
#include "static_pagerank.h"
#include "static_components.h"

#define ACTI(k) (action[2*(k)])
#define ACTJ(k) (action[2*(k)+1])

static int64_t nv, ne, naction;
static int64_t * restrict off;
static int64_t * restrict from;
static int64_t * restrict ind;
static int64_t * restrict weight;
static int64_t * restrict action;

/* handles for I/O memory */
static int64_t * restrict graphmem;
static int64_t * restrict actionmem;

static char * initial_graph_name = INITIAL_GRAPH_NAME_DEFAULT;
static char * action_stream_name = ACTION_STREAM_NAME_DEFAULT;

static long batch_size = BATCH_SIZE_DEFAULT;
static long nbatch = 1;

static struct stinger * S;

static double * update_time_trace;

void
bfs(stinger_t * S, int64_t nv, int64_t source, int64_t * distances) {
  int64_t * queue = malloc(sizeof(int64_t) * nv);
  int64_t * found = malloc(sizeof(int64_t) * nv);
  int64_t ftr_start = 0;
  int64_t ftr_stop = 1;
  int64_t queue_top= 1;
  queue[0] = source;
  found[source] = 1;

  while(ftr_start != ftr_stop) {
    /* for each vertex in the frontier in parallel */
    OMP("omp parallel for")
    for(int64_t q = ftr_start; q < ftr_stop; q++) {
      STINGER_FORALL_EDGES_OF_VTX_BEGIN(S, queue[q]) {
	if(!found[STINGER_EDGE_DEST]) {
	  if(0 == stinger_int64_fetch_add(found + STINGER_EDGE_DEST, 1)) {
	    int64_t where = stinger_int64_fetch_add(&queue_top, 1);
	    queue[where] = STINGER_EDGE_DEST;
	  }
	}
      } STINGER_FORALL_EDGES_OF_VTX_END();
    }
    ftr_start = ftr_stop;
    ftr_stop = queue_top;
  }
}


int
main (const int argc, char *argv[])
{
  parse_args (argc, argv, &initial_graph_name, &action_stream_name, &batch_size, &nbatch);
  STATS_INIT();

  load_graph_and_action_stream (initial_graph_name, &nv, &ne, (int64_t**)&off,
	      (int64_t**)&ind, (int64_t**)&weight, (int64_t**)&graphmem,
	      action_stream_name, &naction, (int64_t**)&action, (int64_t**)&actionmem);

  print_initial_graph_stats (nv, ne, batch_size, nbatch, naction);
  batch_size = naction;
  BATCH_SIZE_CHECK();

  R("{\n")
  R("\"type\":\"stinger\",\n")
  R_A("\"nv\":%ld,\n", nv)
  R_A("\"ne\":%ld,\n", ne)
  R("\"results\": {\n")

#if defined(_OPENMP)
  OMP("omp parallel")
  {
  OMP("omp master")
  PRINT_STAT_INT64 ("num_threads", omp_get_num_threads());
  }
#endif

  update_time_trace = xmalloc (nbatch * sizeof(*update_time_trace));

  /* Convert to STINGER */
  tic ();
  S = stinger_new ();
  stinger_set_initial_edges (S, nv, 0, off, ind, weight, NULL, NULL, -2);
  double build_time = toc();
  R("\"build\": {\n")
  R("\"name\":\"stinger-std\",\n")
  R_A("\"time\":%le\n", build_time)
  R("},\n")
  PRINT_STAT_DOUBLE ("time_stinger", build_time);
  fflush(stdout);

  free(graphmem);

  tic ();
  uint32_t errorCode = stinger_consistency_check (S, nv);
  double time_check = toc ();
  PRINT_STAT_HEX64 ("error_code", errorCode);
  PRINT_STAT_DOUBLE ("time_check", time_check);

  int64_t * components = calloc(sizeof(int64_t), nv);
  tic();
  parallel_shiloach_vishkin_components(S, nv, components);

  double sv_time = toc();

  R("\"sv\": {\n")
  R("\"name\":\"stinger-std\",\n")
  R_A("\"time\":%le\n", sv_time)
  R("},\n")
  free(components);

  int64_t * distances = calloc(sizeof(int64_t), nv);
  tic();
  bfs(S, nv, 0, distances);
  double sssv_time = toc();

  R("\"sssp\": {\n")
  R("\"name\":\"stinger-std\",\n")
  R_A("\"time\":%le\n", sssv_time)
  R("},\n")

  double * pr = calloc(sizeof(double), nv);
  tic();
  pagerank(S, nv, pr, 1e-8, 0.85, 100);

  double pr_time = toc();
  free(pr);

  R("\"pr\": {\n")
  R("\"name\":\"stinger-std\",\n")
  R_A("\"time\":%le\n", pr_time)
  R("},\n")


  /* Updates */
  int64_t ntrace = 0;

  for (int64_t actno = 0; actno < nbatch * batch_size; actno += batch_size)
  {
    tic();

    const int64_t endact = (actno + batch_size > naction ? naction : actno + batch_size);
    int64_t *actions = &action[2*actno];
    int64_t numActions = endact - actno;

    MTA("mta assert parallel")
    MTA("mta block dynamic schedule")
    OMP("omp parallel for")
    for(uint64_t k = 0; k < endact - actno; k++) {
      const int64_t i = actions[2 * k];
      const int64_t j = actions[2 * k + 1];

      if (i != j && i < 0) {
	stinger_remove_edge(S, 0, ~i, ~j);
	stinger_remove_edge(S, 0, ~j, ~i);
      }

      if (i != j && i >= 0) {
	stinger_insert_edge (S, 0, i, j, 1, actno+2);
	stinger_insert_edge (S, 0, j, i, 1, actno+2);
      }
    }

    update_time_trace[ntrace] = toc();
    ntrace++;

  } /* End of batch */

  /* Print the times */
  double time_updates = 0;
  for (int64_t k = 0; k < nbatch; k++) {
    time_updates += update_time_trace[k];
  }
  PRINT_STAT_DOUBLE ("time_updates", time_updates);
  PRINT_STAT_DOUBLE ("updates_per_sec", (nbatch * batch_size) / time_updates); 

  double eps = (nbatch * batch_size) / time_updates;

  R("\"update\": {\n")
  R("\"name\":\"stinger-std\",\n")
  R_A("\"time\":%le\n", eps)
  R("}\n")
  R("},\n")

  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);

  R_A("\"na\":%ld,\n", nbatch * batch_size)
  R_A("\"mem\":%ld\n", usage.ru_maxrss)
  R("}\n")

  tic ();
  //errorCode = stinger_consistency_check (S, nv);
  time_check = toc ();
  PRINT_STAT_HEX64 ("error_code", errorCode);
  PRINT_STAT_DOUBLE ("time_check", time_check);

  free(update_time_trace);
  stinger_free_all (S);
  free (actionmem);
  STATS_END();
}
