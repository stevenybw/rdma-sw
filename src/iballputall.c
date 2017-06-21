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

typedef unsigned long long u64Int;

enum {
  RECV_WRID = 1,
  SEND_WRID = 2,
};

struct pingpong_context {
  struct ibv_context  *context;
  struct ibv_pd   *pd;
  struct ibv_mr   *tx_mr;
  struct ibv_mr   *rx_mr;
  struct ibv_cq   *cq;
  struct ibv_srq  *srq;
  struct ibv_qp  **qp_list;

  int      rank;
  int      nprocs;
  void*    tx_buf;
  void*    rx_buf;
  int      tx_depth;
  int      rx_depth;
  int     *tx_pending;
  int      rx_received;
  int      send_flags;
  struct ibv_port_attr     portinfo;
  uint64_t     completion_timestamp_mask;
};

struct pingpong_dest {
  int lid;
  int qpn;
  int psn;
  union ibv_gid gid;
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

  ctx = malloc(sizeof(*ctx));
  if (!ctx)
    return NULL;

  ctx->rank       = rank;
  ctx->nprocs     = nprocs;
  ctx->send_flags = IBV_SEND_SIGNALED;
  ctx->tx_depth   = tx_depth;
  ctx->rx_depth   = rx_depth;

  size_t bufBytes = BUF_SIZE_PER_RANK * nprocs;
  ctx->tx_buf     = memalign(PAGE_SIZE, bufBytes);
  ctx->rx_buf     = memalign(PAGE_SIZE, MAX_SRQ_WR * sizeof(u64Int));
  ctx->tx_pending = (int*) malloc(nprocs * sizeof(int));

  if (!(ctx->tx_buf && ctx->rx_buf)) {
    fprintf(stderr, "Couldn't allocate work buf.\n");
    goto clean_ctx;
  }

  memset(ctx->tx_buf, 0x7b, bufBytes);
  memset(ctx->rx_buf, 0x7b, MAX_SRQ_WR * sizeof(u64Int));
  memset(ctx->tx_pending, 0, nprocs * sizeof(int));

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

  ctx->tx_mr = ibv_reg_mr(ctx->pd, ctx->tx_buf, bufBytes, access_flags);
  ctx->rx_mr = ibv_reg_mr(ctx->pd, ctx->rx_buf, bufBytes, access_flags);

  if (!(ctx->tx_mr && ctx->rx_mr)) {
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
    int i;
    struct ibv_qp** qp_list = (struct ibv_qp**) malloc(nprocs * sizeof(uintptr_t));
    NZ(qp_list);

    // create qp
    for(i=0; i<nprocs; i++) {
      struct ibv_qp* qp = NULL;
      struct ibv_qp_attr attr;
      struct ibv_qp_init_attr init_attr = {
        .send_cq = ctx->cq,
        .recv_cq = ctx->cq,
        .srq     = ctx->srq,
        .cap     = {
          .max_send_wr  = tx_depth,
          .max_send_sge = 1,
        },
        .qp_type = IBV_QPT_RC
      };
      qp = ibv_create_qp(ctx->pd, &init_attr);
      if (!qp)  {
        fprintf(stderr, "Couldn't create QP\n");
        goto clean_cq;
      }
      ibv_query_qp(qp, &attr, IBV_QP_CAP, &init_attr);
      if (init_attr.cap.max_inline_data >= sizeof(u64Int)) {
        ctx->send_flags |= IBV_SEND_INLINE;
      }
      ctx->qp_list[i] = qp;
    }

    // qp -> INIT
    for(i=0; i<nprocs; i++) {
      struct ibv_qp* qp = ctx->qp_list[i];
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
    free(ctx->qp_list[i]);
  }
}

clean_cq:
  ibv_destroy_cq(ctx->cq);

clean_mr:
  ibv_dereg_mr(ctx->tx_mr);
  ibv_dereg_mr(ctx->rx_mr);

clean_pd:
  ibv_dealloc_pd(ctx->pd);

clean_device:
  ibv_close_device(ctx->context);

clean_buffer:
  free(ctx->tx_buf);
  free(ctx->rx_buf);
  free(ctx->tx_pending);

clean_ctx:
  free(ctx);

  return NULL;
}

static int pp_post_recv_init(struct pingpong_context *ctx)
{
  int rank = ctx->rank;
  int nprocs = ctx->nprocs;
  u64Int* rx_buf = (u64Int*) ctx->rx_buf;

  struct ibv_sge list = {
    .addr = (uintptr_t) NULL,
    .length = sizeof(u64Int),
    .lkey = ctx->rx_mr->lkey
  };
  struct ibv_recv_wr wr = {
    .wr_id      = RECV_WRID,
    .next       = NULL,
    .sg_list    = &list,
    .num_sge    = 1,
  };
  struct ibv_recv_wr *bad_wr = NULL;
  int i, err;
  for (i = 0; i < MAX_SRQ_WR; ++i) {
    wr.wr_id = i;
    list.addr = (uint64_t) &rx_buf[i];
    if (err = ibv_post_srq_recv(ctx->srq, &wr, &bad_wr)) {
      return err;
    }
    if (bad_wr) {
      LOGD("post_srq_recv has bad_wr\n");
      return -1;
    }
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

static int pp_post_send_1024(struct pingpong_context* ctx) {
  int i;
  int rank        = ctx->rank;
  int nprocs      = ctx->nprocs;
  int err;
  u64Int* tx_buf  = (u64Int*) ctx->tx_buf;

  struct ibv_sge list = {
    .addr   = 0,
    .length = sizeof(u64Int),
    .lkey   = ctx->tx_mr->lkey,
  };
  struct ibv_send_wr wr = {
    .wr_id      = SEND_WRID,
    .next       = NULL,
    .sg_list    = &list,
    .num_sge    = 1,
    .opcode     = IBV_WR_SEND,
    .send_flags = ctx->send_flags,
  };
  struct ibv_send_wr *bad_wr = NULL;

  for(i=0; i<1024; i++) {
    int dest = i % nprocs;
    list.addr = (uint64_t) &tx_buf[i];
    if(err = ibv_post_send(ctx->qp_list[dest], &wr, &bad_wr)) {
      LOGD("ibv_post_send returned %d\n", err);
      return err;
    }
    if(bad_wr) {
      LOGD("bad_wr\n");
      return -1;
    }
  }
}

static int pp_close_ctx(struct pingpong_context *ctx)
{
  if (ibv_destroy_qp(ctx->qp)) {
    fprintf(stderr, "Couldn't destroy QP\n");
    return 1;
  }

  if (ibv_destroy_cq(ctx->cq)) {
    fprintf(stderr, "Couldn't destroy CQ\n");
    return 1;
  }

  if (ibv_dereg_mr(ctx->tx_mr)) {
    fprintf(stderr, "Couldn't deregister MR\n");
    return 1;
  }

  if (ibv_dereg_mr(ctx->rx_mr)) {
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
  free(ctx->rx_buf);
  free(ctx);

  return 0;
}

int main(int argc, char *argv[])
{
  int rank, nprocs;
  MPI_Init(NULL, NULL);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  assert(nprocs == 2);

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
  struct pingpong_dest     my_dest;
  struct pingpong_dest    *all_dest;
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

  ctx = pp_init_ctx(ib_dev, tx_depth, rx_depth, ib_port, rank, nprocs);
  if (!ctx) {
    return 1;
  }

  if (pp_post_recv_init(ctx)) {
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

  // exchange address
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

    LOGDS("  MPI_Alltoall qpn_list...\n");
    MPI_Alltoall(local_qpn_list, 1, MPI_INT, remote_qpn_list, 1, MPI_INT, MPI_COMM_WORLD);
    LOGDS("  MPI_Alltoall Complete\n");

    LOGDS("  MPI_Alltoall psn_list...\n");
    MPI_Alltoall(local_psn_list, 1, MPI_INT, remote_psn_list, 1, MPI_INT, MPI_COMM_WORLD);
    LOGDS("  MPI_Alltoall Complete\n");

    
    LOGDS("  MPI_Allgather lid_list...\n");
    MPI_Allgather(&local_lid, 1, MPI_INT, remote_lid_list, 1, MPI_INT, MPI_COMM_WORLD);
    LOGDS("  MPI_Allgather Complete\n");
  }

  LOGDS("  establishing connection");
  ZERO(pp_connect_ctx(ctx, ib_port, mtu, sl, gidx, local_psn_list, remote_lid_list, remote_psn_list, remote_qpn_list));

  {
    int i;
    u64Int* tx_buf = ctx->tx_buf;
    for(i=0; i<tx_depth; i++) {
      tx_buf[i] = i;
    }
  }

  if (gettimeofday(&start, NULL)) {
    perror("gettimeofday");
    return 1;
  }

  MPI_Barrier(MPI_COMM_WORLD);

  int N;
  while (N < (iters + skip)) {
    // printf("%d> iter %d\n", rank, N);
    int ret;
    int ne, i;
    struct ibv_wc wc[64];

    if(N == skip) {
      if (gettimeofday(&start, NULL)) {
        perror("gettimeofday");
        return 1;
      }
    }

    ZERO(pp_post_send_1024(ctx));

    int scnt = 0;
    int rcnt = 0;
    MPI_Request b_req = MPI_REQUEST_NULL;

    while(1) {
      ne = ibv_poll_cq(ctx->cq, 64, wc);
      if (ne < 0) {
        LOGD("poll CQ failed\n");
        return 1;
      } else if (ne >= 1) {
        int i;
        for(i=0; i<ne; i++) {
          if(wc[i].status != IBV_WC_SUCCESS) {
            LOGD("Failed status %s (%d) for wr_id %d\n", 
              ibv_wc_status_str(wc[i].status), wc[i].status, (int) wc[i].wr_id);
            return 1;
          }
          switch ((int) wc[i].wr_id) {
          case SEND_WRID:
            scnt++;
            if(scnt == 1024) {
              MPI_Ibarrier(MPI_COMM_WORLD, &b_req);
            }
            break;

          case RECV_WRID:
            rcnt++;
            break;

          default:
            LOGD("unknown wr_id = %d\n", wc[i].wr_id);
            return 1;
            break;
          }
        }
      }
      if(b_req != MPI_REQUEST_NULL) {
        int flag;
        MPI_Test(&b_req, &flag, MPI_STATUS_IGNORE);
        if(flag) {
          break;
        }
      }
    }
    N++;
  }

  printf("%d Finished\n", rank);
  MPI_Barrier(MPI_COMM_WORLD);

  if (gettimeofday(&end, NULL)) {
    perror("gettimeofday");
    return 1;
  }

  {
    float usec = (end.tv_sec - start.tv_sec) * 1000000 +
      (end.tv_usec - start.tv_usec);
    printf("%d iters in %.6f seconds = %.6f usec/iter\n",
           iters, usec / 1000000., usec / iters);
  }

  if (pp_close_ctx(ctx))
    return 1;

  ibv_free_device_list(dev_list);

  MPI_Finalize();
  return 0;
}
