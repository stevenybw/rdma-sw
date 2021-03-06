#include <infiniband/driver.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <malloc.h>
#include <assert.h>
#include <mpi.h>

#include "common.h"

#define NZ(STATEMENT) assert(STATEMENT != NULL)
#define ZERO(STATEMENT) assert(STATEMENT == 0)

#define BEGIN_PROFILE(VARIABLE) do{Profiler.VARIABLE ## _total_time -= MPI_Wtime();}while(0)
#define END_PROFILE(VARIABLE) do{Profiler.VARIABLE ## _total_time += MPI_Wtime();}while(0)

struct {
  double send_total_time;
  double   send_post_total_time;
  double wait_recv_total_time;
  double   wait_recv_process_msg_total_time;
  double     wait_recv_process_msg_post_total_time;
  double   wait_recv_mpi_test_total_time;
  double flush_total_time;
} Profiler;

void profiler_init() {
  memset(&Profiler, 0, sizeof(Profiler));
}

void profiler_print(int iters) {
  printf("  send (us/iter) = %lf\n", 1e6 * Profiler.send_total_time / iters);
  printf("    post (us/iter) = %lf\n", 1e6 * Profiler.send_post_total_time / iters);
  printf("  wait_recv (us/iter) = %lf\n", 1e6 * Profiler.wait_recv_total_time / iters);
  printf("    process_msg (us/iter) = %lf\n", 1e6 * Profiler.wait_recv_process_msg_total_time / iters);
  printf("    mpi_test    (us/iter) = %lf\n", 1e6 * Profiler.wait_recv_mpi_test_total_time / iters);
  printf("  flush     (us/iter) = %lf\n", 1e6 * Profiler.flush_total_time / iters);
}

typedef unsigned long long u64Int;

enum {
  RECV_WRID = 1,
  SEND_WRID = 2,
};

union pingpong_wrid {
  struct {
    uint32_t tag;
    uint32_t id;
  } tagid;
  uint64_t val;
};

struct rx_win_s {
  struct ibv_mr *mr;
  u64Int        *buf;
  int            idx;
  int            len;
};

struct pingpong_context {
  struct ibv_context  *context;
  struct ibv_pd   *pd;
  struct ibv_mr   *tx_mr;
  struct ibv_cq   *cq;
  struct ibv_srq  *srq;
  struct ibv_qp  **qp_list;
  struct rx_win_s  rx_win;

  struct ibv_sge     *sge_list;
  struct ibv_send_wr *send_wr_list;
  struct ibv_sge     *recv_sge_list;
  struct ibv_recv_wr *recv_wr_list;

  int      rank;
  int      nprocs;
  void*    tx_buf;
  int      tx_depth;
  int      rx_depth;
  int      send_flags;
  struct ibv_port_attr     portinfo;
  uint64_t     completion_timestamp_mask;
};

#define PAGE_SIZE (1024*1024)
#define BUF_SIZE_PER_RANK (1024*sizeof(u64Int))
#define MAX_SRQ_WR (1024)

int pp_get_port_info(struct ibv_context *context, int port,
             struct ibv_port_attr *attr)
{
    return ibv_query_port(context, port, attr);
}

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,
              int tx_depth, int rx_depth, int port, int rank, int nprocs)
{
  struct pingpong_context *ctx;
  int access_flags = IBV_ACCESS_LOCAL_WRITE;
  u64Int         *rx_buf = NULL;
  struct ibv_mr  *rx_mr  = NULL;

  ctx = malloc(sizeof(*ctx));
  if (!ctx)
    return NULL;

  ctx->rank        = rank;
  ctx->nprocs      = nprocs;
  ctx->send_flags  = IBV_SEND_SIGNALED;
  ctx->tx_depth    = tx_depth;
  ctx->rx_depth    = rx_depth;
  ctx->rx_win.idx  = 0;
  ctx->rx_win.len  = 0;

  size_t sendBufBytes = BUF_SIZE_PER_RANK * nprocs;
  size_t recvBufBytes = MAX_SRQ_WR * sizeof(u64Int);
  ctx->tx_buf     = memalign(PAGE_SIZE, sendBufBytes);
  rx_buf          = memalign(PAGE_SIZE, recvBufBytes);
  ctx->rx_win.buf = rx_buf;

  if (!(ctx->tx_buf && rx_buf)) {
    fprintf(stderr, "Couldn't allocate work buf.\n");
    goto clean_ctx;
  }

  memset(ctx->tx_buf, 0x7b, sendBufBytes);
  memset(rx_buf, 0x7b, recvBufBytes);

  ctx->context = ibv_open_device(ib_dev);
  if (!ctx->context) {
    fprintf(stderr, "Couldn't get context for %s\n",
      ibv_get_device_name(ib_dev));
    goto clean_buffer;
  }

  ctx->pd = ibv_alloc_pd(ctx->context);
  if (!ctx->pd) {
    fprintf(stderr, "Couldn't allocate PD\n");
    goto clean_device;
  }

  ctx->tx_mr = ibv_reg_mr(ctx->pd, ctx->tx_buf, sendBufBytes, access_flags);
  rx_mr = ibv_reg_mr(ctx->pd, rx_buf, recvBufBytes, access_flags);
  ctx->rx_win.mr = rx_mr;

  if (!(ctx->tx_mr && rx_mr)) {
    fprintf(stderr, "Couldn't register MR\n");
    goto clean_pd;
  }

  ctx->cq = ibv_create_cq(ctx->context, 4096, NULL,
           NULL, 0);

  if (!ctx->cq) {
    fprintf(stderr, "Couldn't create CQ\n");
    goto clean_mr;
  }

  // create SRQ
  {
    struct ibv_srq_init_attr attr = {
      .attr = {
        .max_wr  = MAX_SRQ_WR,
        .max_sge = 1
      }
    };

    ctx->srq = ibv_create_srq(ctx->pd, &attr);
    if (!ctx->srq)  {
      fprintf(stderr, "Couldn't create SRQ\n");
      goto clean_cq;
    }
  }

  {
    LOGDS("    creating QP\n");
    int i;
    struct ibv_qp** qp_list = (struct ibv_qp**) malloc(nprocs * sizeof(uintptr_t));
    NZ(qp_list);
    ctx->qp_list = qp_list;

    // create qp
    for(i=0; i<nprocs; i++) {
      struct ibv_qp* qp = NULL;
      struct ibv_qp_attr attr;
      struct ibv_qp_init_attr init_attr = {
        .send_cq = ctx->cq,
        .recv_cq = ctx->cq,
        .srq     = ctx->srq,
        .cap     = {
          .max_send_wr  = 1024,
          .max_send_sge = 1,
        },
        .qp_type = IBV_QPT_RC
      };
      qp = ibv_create_qp(ctx->pd, &init_attr);
      if (!qp)  {
        fprintf(stderr, "Couldn't create QP[%d], errno=%d[%s]\n", i, errno, strerror(errno));
        goto clean_cq;
      }
      ibv_query_qp(qp, &attr, IBV_QP_CAP, &init_attr);
      if (init_attr.cap.max_inline_data >= sizeof(u64Int)) {
        ctx->send_flags |= IBV_SEND_INLINE;
      }
      qp_list[i] = qp;
    }

    LOGDS("    setting QP to INIT\n");
    for(i=0; i<nprocs; i++) {
      struct ibv_qp* qp = qp_list[i];
      struct ibv_qp_attr attr = {
        .qp_state        = IBV_QPS_INIT,
        .pkey_index      = 0,
        .port_num        = port,
        .qp_access_flags = 0
      };

      if (ibv_modify_qp(qp, &attr,
            IBV_QP_STATE              |
            IBV_QP_PKEY_INDEX         |
            IBV_QP_PORT               |
            IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "Failed to modify QP[%d] to INIT\n", i);
        goto clean_qp;
      }
    }
  }

  return ctx;

clean_qp:
{
  int i;
  for(i=0; i<nprocs; i++) {
    ibv_destroy_qp(ctx->qp_list[i]);
    ctx->qp_list[i] = NULL;
  }
  free(ctx->qp_list[i]);
}

clean_cq:
  ibv_destroy_cq(ctx->cq);

clean_mr:
  ibv_dereg_mr(ctx->tx_mr);
  ibv_dereg_mr(ctx->rx_win.mr);

clean_pd:
  ibv_dealloc_pd(ctx->pd);

clean_device:
  ibv_close_device(ctx->context);

clean_buffer:
  free(ctx->tx_buf);
  free(ctx->rx_win.buf);

clean_ctx:
  free(ctx);

  return NULL;
}

static int pp_post_recv_refill(struct pingpong_context *ctx)
{
  int          idx = ctx->rx_win.idx;
  int          len = ctx->rx_win.len;
  u64Int*   rx_buf = (u64Int*) ctx->rx_win.buf;
  
  struct ibv_sge* recv_sge_list = ctx->recv_sge_list;
  struct ibv_recv_wr* recv_wr_list = ctx->recv_wr_list;

  ctx->rx_win.len = MAX_SRQ_WR;

  struct ibv_recv_wr *bad_wr = NULL;

  int i;
  int num_recv = MAX_SRQ_WR - len;
  int id = (idx + len) % MAX_SRQ_WR;
  BEGIN_PROFILE(wait_recv_process_msg_post);
  
  for(i=0; i<num_recv; i++) {
    union pingpong_wrid wr_id = {
      .tagid = {
        .tag  = RECV_WRID,
        .id   = id,
      }
    };
    recv_sge_list[i].addr = (uintptr_t) &rx_buf[id];
    recv_wr_list[i].wr_id = wr_id.val;
    id = (id + 1) % MAX_SRQ_WR;
  }
  recv_wr_list[num_recv-1].next = NULL;

  int err;
  if (err = ibv_post_srq_recv(ctx->srq, recv_wr_list, &bad_wr)) {
    return err;
  }
  if (bad_wr) {
    LOGD("post_srq_recv has bad_wr\n");
    return -1;
  }
  recv_wr_list[num_recv-1].next = &recv_wr_list[num_recv];
  
  END_PROFILE(wait_recv_process_msg_post);

  return 0;
}


static inline int pp_process(u64Int* buf, int idx, int len) {
  return 0;
}

static inline int pp_on_recv(struct pingpong_context *ctx, int id) {
  int err;
  int idx = ctx->rx_win.idx;
  int len = ctx->rx_win.len;
  assert(idx == id);
  idx = (idx + 1) % MAX_SRQ_WR;
  len = len - 1;
  ctx->rx_win.idx = idx;
  ctx->rx_win.len = len;
  if(len == 0) {
    if(err = pp_process(ctx->rx_win.buf, idx, MAX_SRQ_WR)) {
      return err;
    }
    if(err = pp_post_recv_refill(ctx)) {
      return err;
    }
  }
  return 0;
}

static inline int pp_on_flush(struct pingpong_context *ctx) {
  int err;
  int idx = ctx->rx_win.idx;
  int len = ctx->rx_win.len;
  int start = (idx + len) % MAX_SRQ_WR;
  int i;
  if(err = pp_process(ctx->rx_win.buf, idx, MAX_SRQ_WR - len)) {
    return err;
  }
  if(err = pp_post_recv_refill(ctx)) {
    return err;
  }
  return 0;
}

static int pp_connect_ctx(struct pingpong_context *ctx, int port, enum ibv_mtu mtu, 
              int sl, int sgid_idx, int* local_psn_list, int* remote_lid_list, 
              int* remote_psn_list, int* remote_qpn_list)
{
  int i;
  int rank = ctx->rank;
  int nprocs = ctx->nprocs;

  for(i=0; i<nprocs; i++) {
    struct ibv_qp* qp         = ctx->qp_list[i];
    int            local_psn  = local_psn_list[i];
    int            remote_lid = remote_lid_list[i];
    int            remote_psn = remote_psn_list[i];
    int            remote_qpn = remote_qpn_list[i];

    struct ibv_qp_attr attr = {
      .qp_state   = IBV_QPS_RTR,
      .path_mtu   = mtu,
      .dest_qp_num    = remote_qpn,
      .rq_psn     = remote_psn,
      .max_dest_rd_atomic = 1,
      .min_rnr_timer    = 12,
      .ah_attr    = {
        .is_global  = 0,
        .dlid   = remote_lid,
        .sl   = sl,
        .src_path_bits  = 0,
        .port_num = port
      }
    };

    if (ibv_modify_qp(qp, &attr,
          IBV_QP_STATE              |
          IBV_QP_AV                 |
          IBV_QP_PATH_MTU           |
          IBV_QP_DEST_QPN           |
          IBV_QP_RQ_PSN             |
          IBV_QP_MAX_DEST_RD_ATOMIC |
          IBV_QP_MIN_RNR_TIMER)) {
      fprintf(stderr, "Failed to modify QP[%d] to RTR\n", i);
      return 1;
    }

    attr.qp_state     = IBV_QPS_RTS;
    attr.timeout      = 14;
    attr.retry_cnt      = 7;
    attr.rnr_retry      = 7;
    attr.sq_psn     = local_psn;
    attr.max_rd_atomic  = 1;
    if (ibv_modify_qp(qp, &attr,
          IBV_QP_STATE              |
          IBV_QP_TIMEOUT            |
          IBV_QP_RETRY_CNT          |
          IBV_QP_RNR_RETRY          |
          IBV_QP_SQ_PSN             |
          IBV_QP_MAX_QP_RD_ATOMIC)) {
      fprintf(stderr, "Failed to modify QP[%d] to RTS\n", i);
      return 1;
    }
  }

  return 0;
}

/*
 * used to measure performance gain using multiple wr
 */
#if 1
static int pp_post_send_rank_count(struct pingpong_context* ctx, struct ibv_sge* sge_list,
                        struct ibv_send_wr* send_wr_list, int rank, int count) {
  int i, err;
  u64Int* tx_buf = (u64Int*) ctx->tx_buf;

  for(i=0; i<count; i++) {
    union pingpong_wrid wr_id = {
      .tagid = {
        .tag = SEND_WRID,
        .id  = i,
      }
    };
    sge_list[i].addr = (uint64_t) &tx_buf[i];
    send_wr_list[i].wr_id = wr_id.val;
  }
  BEGIN_PROFILE(send_post);
  for(i=0; i<count; i++) {
    struct ibv_send_wr *bad_wr = NULL;
    // LOGV("ibv_post_send to %d\n", rank);
    if(err = ibv_post_send(ctx->qp_list[rank], &send_wr_list[i], &bad_wr)) {
      LOGD("%d-th ibv_post_send returned %d, errno = %d[%s]\n", i, err, errno, strerror(errno));
      return -1;
    }
    if(bad_wr) {
      LOGD("bad_wr\n");
      return -1;
    }
  }
  END_PROFILE(send_post);
  return 0;
}
#else
/*
 * non-aggregated version: slower about 2~3 times
 */
static int pp_post_send_rank_count(struct pingpong_context* ctx, struct ibv_sge* sge_list,
                        struct ibv_send_wr* send_wr_list, int rank, int count) {
  int i, err;
  u64Int* tx_buf = (u64Int*) ctx->tx_buf;

  struct ibv_sge list = {
    .addr   = 0,
    .length = sizeof(u64Int),
    .lkey   = ctx->tx_mr->lkey,
  };
  union pingpong_wrid wr_id = {
    .tagid = {
      .tag    = SEND_WRID,
      .id     = 0,
    }
  };
  struct ibv_send_wr wr = {
    .next       = NULL,
    .sg_list    = &list,
    .num_sge    = 1,
    .opcode     = IBV_WR_SEND,
    .send_flags = ctx->send_flags,
  };

  struct ibv_send_wr *bad_wr = NULL;

  for(i=0; i<count; i++) {
    list.addr = &tx_buf[i];
    wr_id.tagid.id  = i;
    wr.wr_id = wr_id.val;

    BEGIN_PROFILE(send_post);
    // LOGV("ibv_post_send to %d\n", rank);
    if(err = ibv_post_send(ctx->qp_list[rank], &wr, &bad_wr)) {
      LOGD("%d-th ibv_post_send returned %d, errno = %d[%s]\n", i, err, errno, strerror(errno));
      return -1;
    }
    END_PROFILE(send_post);
    if(bad_wr) {
      LOGD("bad_wr\n");
      return -1;
    }
  }
  send_wr_list[count-1].next = &send_wr_list[count];
  return 0;
}
#endif

static int pp_post_send_1024(struct pingpong_context* ctx) {
  int i;
  int rank        = ctx->rank;
  int nprocs      = ctx->nprocs;
  int scount = 0;
  int err;
  u64Int* tx_buf  = (u64Int*) ctx->tx_buf;

  struct ibv_sge list = {
    .addr   = 0,
    .length = sizeof(u64Int),
    .lkey   = ctx->tx_mr->lkey,
  };
  union pingpong_wrid wr_id = {
    .tagid = {
      .tag    = SEND_WRID,
      .id     = 0,
    }
  };
  struct ibv_send_wr wr = {
    .wr_id      = (uint64_t) wr_id.val,
    .next       = NULL,
    .sg_list    = &list,
    .num_sge    = 1,
    .opcode     = IBV_WR_SEND,
    .send_flags = ctx->send_flags,
  };
  struct ibv_send_wr *bad_wr = NULL;

  for(i=0; i<1024; i++) {
    int dest = (rank+i) % nprocs;
    if(dest != rank) {
      list.addr = (uint64_t) &tx_buf[i];
      if(err = ibv_post_send(ctx->qp_list[dest], &wr, &bad_wr)) {
        LOGD("%d-th ibv_post_send returned %d, errno = %d[%s]\n", i, err, errno, strerror(errno));
        return -1;
      }
      if(bad_wr) {
        LOGD("bad_wr\n");
        return -1;
      }
      scount++;
    }
  }
  return scount;
}

static int pp_close_ctx(struct pingpong_context *ctx)
{
  int nprocs = ctx->nprocs;
  {
    int i;
    for(i=0; i<nprocs; i++) {
      if(ibv_destroy_qp(ctx->qp_list[i])) {
        LOGD("Couldn't destroy QP[%d]\n", i);
      }
      ctx->qp_list[i] = NULL;
    }
    free(ctx->qp_list);
  }

  if (ibv_destroy_cq(ctx->cq)) {
    fprintf(stderr, "Couldn't destroy CQ\n");
    return 1;
  }

  if (ibv_dereg_mr(ctx->tx_mr)) {
    fprintf(stderr, "Couldn't deregister MR\n");
    return 1;
  }

  if (ibv_dereg_mr(ctx->rx_win.mr)) {
    fprintf(stderr, "Couldn't deregister MR\n");
    return 1;
  }

  if (ibv_dealloc_pd(ctx->pd)) {
    fprintf(stderr, "Couldn't deallocate PD\n");
    return 1;
  }

  if (ibv_close_device(ctx->context)) {
    fprintf(stderr, "Couldn't release context\n");
    return 1;
  }

  free(ctx->tx_buf);
  free(ctx->rx_win.buf);
  free(ctx);

  return 0;
}

#define RESULT_SPACE 16

int benchmark_wr_len(struct pingpong_context* ctx, struct ibv_sge* sge_list, 
                struct ibv_send_wr* send_wr_list, MPI_Comm comm, int show_result) {
  int count;
  int ne, i;
  struct ibv_wc wc[64];
  int rank, size, absolute_rank, wranks[2];
  MPI_Comm_rank(comm, &rank);
  MPI_Comm_rank(MPI_COMM_WORLD, &absolute_rank);
  MPI_Comm_size(comm, &size);
  assert(size == 2);
  MPI_Allgather(&absolute_rank, 1, MPI_INT, wranks, 1, MPI_INT, comm);

  {
    const char* t0 = "count";
    const char* t1 = "post time(us)";
    const char* t2 = "mesg rate(M/s)";
    const char* t3 = "time (us)";
    const char* t4 = "post_time(us)";
    const char* t5 = "proc_time(us)";
    const char* t6 = "flush_time(us)";
    const char* t7 = "post_recv(us)";
    if(show_result) {
      LOGDS("%*s%*s%*s%*s%*s%*s%*s%*s\n", RESULT_SPACE, t0, RESULT_SPACE, t1, RESULT_SPACE, t2, 
        RESULT_SPACE, t3, RESULT_SPACE, t4, RESULT_SPACE, t5, RESULT_SPACE, t6, RESULT_SPACE, t7);
    }
  }

  for(count=1; count<=512; count*=2) {
    profiler_init();
    int scnt=0, rcnt=0;
    int dst = wranks[(rank==0)?1:0];

    double wtime = -MPI_Wtime();
    double  post_time = -MPI_Wtime();
    double  process_time = 0.0;
    pp_post_send_rank_count(ctx, sge_list, send_wr_list, dst, count);
    post_time += MPI_Wtime();
    {
      while(scnt!=count || rcnt!=count) {
        ne = ibv_poll_cq(ctx->cq, 64, wc);
        if (ne < 0) {
          LOGD("poll CQ failed\n");
          return 1;
        } else if (ne >= 1) {
          // LOGV("complete %d request\n", ne);
          process_time -= MPI_Wtime();
          int i;
          for(i=0; i<ne; i++) {
            union pingpong_wrid wr_id;
            wr_id.val = wc[i].wr_id;
            if(wc[i].status != IBV_WC_SUCCESS) {
              LOGD("Failed status %s (%d) for wr_id %d:%d\n", 
                ibv_wc_status_str(wc[i].status), wc[i].status, (int) wr_id.tagid.tag, (int) wr_id.tagid.id);
              return 1;
            }
            switch (wr_id.tagid.tag) {
            case SEND_WRID:
              scnt++;
              // LOGV("send %d complete [%d/%d]\n", wr_id.tagid.id, scnt, count);
              break;
            case RECV_WRID:
              rcnt++;
              pp_on_recv(ctx, wr_id.tagid.id);
              // LOGV("recv %d complete [%d/%d]\n", wr_id.tagid.id, rcnt, count);
              break;
            default:
              LOGD("unknown wr_id = %d:%d\n", wr_id.tagid.tag, wr_id.tagid.id);
              return 1;
              break;
            }
          }
          process_time += MPI_Wtime();
        }
      }
    }

    BEGIN_PROFILE(flush);
    pp_on_flush(ctx);
    END_PROFILE(flush);

    MPI_Barrier(comm);

    wtime += MPI_Wtime();

    if(show_result) {
      double r0 = 1e6 * Profiler.send_post_total_time / count;
      double r1 = 1e-6 * count / wtime;
      double r2 = 1e6 * wtime;
      double r3 = 1e6 * post_time;
      double r4 = 1e6 * process_time;
      double r5 = 1e6 * Profiler.flush_total_time;
      double r6 = 1e6 * Profiler.wait_recv_process_msg_post_total_time / count;

      LOGDS("%*d%*lf%*lf%*lf%*lf%*lf%*lf%*lf\n", RESULT_SPACE, count, RESULT_SPACE, r0, RESULT_SPACE, r1, 
          RESULT_SPACE, r2, RESULT_SPACE, r3, RESULT_SPACE, r4, RESULT_SPACE, r5, RESULT_SPACE, r6);
    }
  }
  return 0;
}

int main(int argc, char *argv[])
{
  int rank, nprocs;
  MPI_Init(NULL, NULL);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

  srand48(rank * time(NULL));

  struct ibv_device       **dev_list;
  struct ibv_device       *ib_dev;
  struct pingpong_context *ctx;
  int                      local_lid = -1;
  int                     *local_psn_list = NULL;
  int                     *local_qpn_list = NULL;
  int                     *remote_lid_list = NULL;
  int                     *remote_psn_list = NULL;
  int                     *remote_qpn_list = NULL;
  struct timeval           start, end;
  int                      ib_port = 1;
  // unsigned int             size = 4096;
  enum ibv_mtu             mtu = IBV_MTU_256;
  unsigned int             tx_depth = 128;
  unsigned int             rx_depth = 128;
  unsigned int             iters = 5000;
  unsigned int             skip  = 100;
  int                      sl = 0;
  int                      gidx = -1;
  char                     gid[33];

  LOGDS("iballputall: allputall using libverbs\n");

  dev_list = ibv_get_device_list(NULL);
  if (!dev_list) {
    perror("Failed to get IB devices list");
    return 1;
  }

  ib_dev = *dev_list;
  if (!ib_dev) {
    LOGD("No IB devices found\n");
    return 1;
  }

  LOGDS("  initialize\n");
  ctx = pp_init_ctx(ib_dev, tx_depth, rx_depth, ib_port, rank, nprocs);
  if (!ctx) {
    return 1;
  }

  struct ibv_sge     *sge_list      = NULL;
  struct ibv_send_wr *send_wr_list  = NULL;
  struct ibv_sge     *recv_sge_list = NULL;
  struct ibv_recv_wr *recv_wr_list  = NULL;
  {
    int i;
    sge_list    = (struct ibv_sge*) malloc(1024 * sizeof(struct ibv_sge));           NZ(sge_list);
    send_wr_list = (struct ibv_send_wr*) malloc(1024 * sizeof(struct ibv_send_wr));  NZ(send_wr_list);
    recv_sge_list = (struct ibv_sge*) malloc(1024 * sizeof(struct ibv_sge));         NZ(recv_sge_list);
    recv_wr_list = (struct ibv_recv_wr*) malloc(1024 * sizeof(struct ibv_recv_wr));  NZ(recv_wr_list);

    for(i=0; i<1024; i++) {
      sge_list[i].addr = 0;
      sge_list[i].length = sizeof(u64Int);
      sge_list[i].lkey   = ctx->tx_mr->lkey;

      recv_sge_list[i].addr   = 0;
      recv_sge_list[i].length = sizeof(u64Int);
      recv_sge_list[i].lkey   = ctx->rx_win.mr->lkey;

      if(i == 1023) {
        send_wr_list[i].next    = NULL;
        send_wr_list[i].sg_list = &sge_list[i];
        send_wr_list[i].num_sge = 1;
        send_wr_list[i].opcode  = IBV_WR_SEND;
        send_wr_list[i].send_flags = ctx->send_flags;
      } else {
        send_wr_list[i].next    = NULL;
        send_wr_list[i].sg_list = &sge_list[i];
        send_wr_list[i].num_sge = 1;
        send_wr_list[i].opcode  = IBV_WR_SEND;
        send_wr_list[i].send_flags = ctx->send_flags;
      }

      if(i == 1023) {
        recv_wr_list[i].next    = NULL;
        recv_wr_list[i].sg_list = &recv_sge_list[i];
        recv_wr_list[i].num_sge = 1;
      } else {
        recv_wr_list[i].next    = &recv_wr_list[i+1];
        recv_wr_list[i].sg_list = &recv_sge_list[i];
        recv_wr_list[i].num_sge = 1;
      }
    }
  }
  ctx->sge_list      = sge_list;     
  ctx->send_wr_list  = send_wr_list; 
  ctx->recv_sge_list = recv_sge_list;
  ctx->recv_wr_list  = recv_wr_list; 

  LOGDS("  post receive\n");
  if (pp_post_recv_refill(ctx)) {
    LOGD("Couldn't post receive initially\n");
    return 1;
  }

  if (pp_get_port_info(ctx->context, ib_port, &ctx->portinfo)) {
    fprintf(stderr, "Couldn't get port info\n");
    return 1;
  }

  int lid = ctx->portinfo.lid;
  if (!lid) {
    fprintf(stderr, "Couldn't get local LID\n");
    return 1;
  }

  LOGDS("  exchange address\n");
  {
    int i;
    local_psn_list = (int*) malloc(nprocs * sizeof(int));
    NZ(local_psn_list);
    local_qpn_list = (int*) malloc(nprocs * sizeof(int));
    NZ(local_qpn_list);

    local_lid = ctx->portinfo.lid;
    for(i=0; i<nprocs; i++) {
      local_qpn_list[i] = ctx->qp_list[i]->qp_num;
      local_psn_list[i] = lrand48() & 0xffffff;
    }

    remote_lid_list = (int*) malloc(nprocs * sizeof(int));
    NZ(remote_lid_list);
    remote_psn_list = (int*) malloc(nprocs * sizeof(int));
    NZ(remote_psn_list);
    remote_qpn_list = (int*) malloc(nprocs * sizeof(int));
    NZ(remote_qpn_list);

    LOGDS("    MPI_Alltoall qpn_list...\n");
    MPI_Alltoall(local_qpn_list, 1, MPI_INT, remote_qpn_list, 1, MPI_INT, MPI_COMM_WORLD);
    LOGDS("    MPI_Alltoall Complete\n");

    LOGDS("    MPI_Alltoall psn_list...\n");
    MPI_Alltoall(local_psn_list, 1, MPI_INT, remote_psn_list, 1, MPI_INT, MPI_COMM_WORLD);
    LOGDS("    MPI_Alltoall Complete\n");

    
    LOGDS("    MPI_Allgather lid_list...\n");
    MPI_Allgather(&local_lid, 1, MPI_INT, remote_lid_list, 1, MPI_INT, MPI_COMM_WORLD);
    LOGDS("    MPI_Allgather Complete\n");

#if 0
    {
      int i;
      for(i=0; i<nprocs; i++) {
        printf("    %d", remote_qpn_list[i]);
      }
      printf("\n");
      for(i=0; i<nprocs; i++) {
        printf("    %d", remote_psn_list[i]);
      }
      printf("\n");
      for(i=0; i<nprocs; i++) {
        printf("    %d", remote_lid_list[i]);
      }
      printf("\n");
    }
#endif
  }

  LOGDS("  establishing connection\n");
  ZERO(pp_connect_ctx(ctx, ib_port, mtu, sl, gidx, local_psn_list, remote_lid_list, remote_psn_list, remote_qpn_list));

  {
    int i;
    u64Int* tx_buf = ctx->tx_buf;
    for(i=0; i<tx_depth; i++) {
      tx_buf[i] = i;
    }
  }

  {
    LOGDS("  main write benchmark (uint64_t granuarity)\n");
    LOGDS("  %*s%*s%*s\n", RESULT_SPACE, "count", RESULT_SPACE, "time (us)", RESULT_SPACE, "MB/s");
    u64Int* arr = (u64Int*) malloc(1024*1024*sizeof(u64Int));
    int i, count;
    for(count = 1; count < 1024 * 1024; count*=2) {
      double time = -MPI_Wtime();
      for(i=0; i<count; i++) {
        arr[i] = i;
      }
      time += MPI_Wtime();
      LOGDS("%*d%*lf%*lf\n", RESULT_SPACE, count, RESULT_SPACE, 1e6 * time/count, RESULT_SPACE, 1e-6 * sizeof(u64Int) * count / time);
    }
    free(arr);
  }

  {
    MPI_Comm comm_pair;
    MPI_Comm_split(MPI_COMM_WORLD, rank/2, rank%2, &comm_pair);
    MPI_Barrier(MPI_COMM_WORLD);
    profiler_init();
    if(rank < 2) {
      LOGDS("  message rate benchmark (intra-node, 1 pair)\n");
      benchmark_wr_len(ctx, sge_list, send_wr_list, comm_pair, 0); // skip
      benchmark_wr_len(ctx, sge_list, send_wr_list, comm_pair, 1);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Comm_free(&comm_pair);
  }

  if (nprocs >= 4) {
    LOGDS("  message rate benchmark (intra-node, 2 pair)\n");
    MPI_Comm comm_pair;
    MPI_Comm_split(MPI_COMM_WORLD, rank/2, rank%2, &comm_pair);
    MPI_Barrier(MPI_COMM_WORLD);
    profiler_init();
    if(rank < 4) {
      benchmark_wr_len(ctx, sge_list, send_wr_list, comm_pair, 0); // skip
      benchmark_wr_len(ctx, sge_list, send_wr_list, comm_pair, 1);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Comm_free(&comm_pair);
  } else {
    LOGDS("  message rate benchmark (intra-node, 2 pair) skip...\n");
  }

  if (nprocs >= 8) {
    MPI_Comm comm_pair;
    MPI_Comm_split(MPI_COMM_WORLD, rank%4, rank/4, &comm_pair);
    MPI_Barrier(MPI_COMM_WORLD);
    profiler_init();
    if(rank < 8) {
      if(rank%4 < 4) {
        LOGDS("  message rate benchmark (inter-node, 4 pair)\n");
        benchmark_wr_len(ctx, sge_list, send_wr_list, comm_pair, 0); // skip
        benchmark_wr_len(ctx, sge_list, send_wr_list, comm_pair, 1);
      }
    }
    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Comm_free(&comm_pair);
  } else {
    LOGDS("  message rate benchmark (inter-node) skip...\n");
  }

  if (gettimeofday(&start, NULL)) {
    perror("gettimeofday");
    return 1;
  }
  profiler_init();

  int N = 0;
  while (N < (iters + skip)) {
    // printf("%d> iter %d/%d\n", rank, N+1, iters + skip);
    int ret;
    int ne, i;
    struct ibv_wc wc[64];

    if(N == skip) {
      if (gettimeofday(&start, NULL)) {
        perror("gettimeofday");
        return 1;
      }
      profiler_init();
    }

    BEGIN_PROFILE(send);
    int num_sent = pp_post_send_1024(ctx);
    if (num_sent < 0) {
      LOGD("pp_post_send failed\n");
      return 1;
    }
    END_PROFILE(send);

    int scnt = 0;
    int rcnt = 0;
    MPI_Request b_req = MPI_REQUEST_NULL;

    BEGIN_PROFILE(wait_recv);
    while(1) {
      ne = ibv_poll_cq(ctx->cq, 64, wc);
      if (ne < 0) {
        LOGD("poll CQ failed\n");
        return 1;
      } else if (ne >= 1) {
        BEGIN_PROFILE(wait_recv_process_msg);
        //LOGV("complete %d request\n", ne);
        int i;
        for(i=0; i<ne; i++) {
          union pingpong_wrid wr_id;
          wr_id.val = wc[i].wr_id;
          if(wc[i].status != IBV_WC_SUCCESS) {
            LOGD("Failed status %s (%d) for wr_id %d:%d\n", 
              ibv_wc_status_str(wc[i].status), wc[i].status, (int) wr_id.tagid.tag, (int) wr_id.tagid.id);
            return 1;
          }
          switch ((int) wr_id.tagid.tag) {
          case SEND_WRID:
            scnt++;
            if(scnt == num_sent) {
              MPI_Ibarrier(MPI_COMM_WORLD, &b_req);
            }
            //LOGV("send complete [%d/%d]\n", scnt, num_sent);
            break;

          case RECV_WRID:
            rcnt++;
            pp_on_recv(ctx, wr_id.tagid.id);
            //LOGV("recv complete [%d]\n", rcnt);
            break;

          default:
            LOGD("unknown wr_id = %d:%d\n", wr_id.tagid.tag, wr_id.tagid.id);
            return 1;
            break;
          }
        }
        END_PROFILE(wait_recv_process_msg);
      }
      BEGIN_PROFILE(wait_recv_mpi_test);
      if(b_req != MPI_REQUEST_NULL) {
        int flag;
        MPI_Test(&b_req, &flag, MPI_STATUS_IGNORE);
        if(flag) {
          END_PROFILE(wait_recv_mpi_test);
          break;
        }
      }
      END_PROFILE(wait_recv_mpi_test);
    }
    END_PROFILE(wait_recv);

    BEGIN_PROFILE(flush);
    pp_on_flush(ctx);
    END_PROFILE(flush);
    N++;
  }

  if (gettimeofday(&end, NULL)) {
    perror("gettimeofday");
    return 1;
  }

  printf("%d Finished\n", rank);
  MPI_Barrier(MPI_COMM_WORLD);

  {
    float usec = (end.tv_sec - start.tv_sec) * 1000000 +
      (end.tv_usec - start.tv_usec);
    printf("%d iters in %.6f seconds = %.6f usec/iter\n",
           iters, usec / 1000000., usec / iters);
  }

  if(rank == 0) {
    profiler_print(iters);
  }

  // free address
  {
    free(local_psn_list);
    free(local_qpn_list);
    free(remote_lid_list);
    free(remote_psn_list);
    free(remote_qpn_list);
  }

  if (pp_close_ctx(ctx))
    return 1;

  ibv_free_device_list(dev_list);

  MPI_Finalize();
  return 0;
}
