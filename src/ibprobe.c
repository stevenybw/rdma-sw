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

#define NZ(STATEMENT) assert(STATEMENT != NULL)
#define ZERO(STATEMENT) assert(STATEMENT == 0)
#define SHOW(FMT, OBJECT, ATTRIBUTE) do{printf("  %s = " FMT "\n", #ATTRIBUTE, OBJECT.ATTRIBUTE);} while(0)

typedef unsigned long long u64Int;

#define BUF_SIZE (1024*1024*sizeof(u64Int))

enum {
  PINGPONG_RECV_WRID = 0,
  PINGPONG_SEND_WRID = (1LL<<63),
};

enum {
  SEND_TAG = 1,
};

struct pingpong_context {
  int rank;
  int nprocs;
  struct ibv_context  *context;
  struct ibv_pd   *pd;
  struct ibv_mr   *tx_mr;
  struct ibv_mr   *rx_mr;
  struct ibv_cq   *tx_cq;
  struct ibv_cq   *rx_cq;
  struct ibv_qp   *qp;
  struct ibv_sge  *tx_sge;
  struct ibv_sge  *rx_sge;
  struct ibv_send_wr *send_wr;
  struct ibv_recv_wr *recv_wr;

  void      *tx_buf;
  void      *rx_buf;
  int      send_flags;
  int      tx_depth;
  int      rx_depth;
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

int pp_get_port_info(struct ibv_context *context, int port,
             struct ibv_port_attr *attr)
{
    return ibv_query_port(context, port, attr);
}

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,
              int tx_depth, int rx_depth, int port)
{
  struct pingpong_context *ctx;
  int access_flags = IBV_ACCESS_LOCAL_WRITE;

  ctx = malloc(sizeof(*ctx));
  if (!ctx)
    return NULL;

  ctx->send_flags = IBV_SEND_SIGNALED;
  ctx->tx_depth   = tx_depth;
  ctx->rx_depth   = rx_depth;

  ctx->tx_buf = memalign(PAGE_SIZE, BUF_SIZE);
  ctx->rx_buf = memalign(PAGE_SIZE, BUF_SIZE);
  if (!(ctx->tx_buf && ctx->rx_buf)) {
    fprintf(stderr, "Couldn't allocate work buf.\n");
    goto clean_ctx;
  }

  memset(ctx->tx_buf, 0x7b, BUF_SIZE);
  memset(ctx->rx_buf, 0x7b, BUF_SIZE);

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

  ctx->tx_mr = ibv_reg_mr(ctx->pd, ctx->tx_buf, BUF_SIZE, access_flags);
  ctx->rx_mr = ibv_reg_mr(ctx->pd, ctx->rx_buf, BUF_SIZE, access_flags);

  if (!(ctx->tx_mr && ctx->rx_mr)) {
    fprintf(stderr, "Couldn't register MR\n");
    goto clean_pd;
  }

  ctx->tx_cq = ibv_create_cq(ctx->context, tx_depth, NULL,
           NULL, 0);
  ctx->rx_cq = ibv_create_cq(ctx->context, rx_depth, NULL,
           NULL, 0);

  if (!(ctx->tx_cq && ctx->rx_cq)) {
    fprintf(stderr, "Couldn't create CQ\n");
    goto clean_mr;
  }

  {
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr = {
      .send_cq = ctx->tx_cq,
      .recv_cq = ctx->rx_cq,
      .cap     = {
        .max_send_wr  = tx_depth,
        .max_recv_wr  = rx_depth,
        .max_send_sge = 1,
        .max_recv_sge = 1
      },
      .qp_type = IBV_QPT_RC
    };

    ctx->qp = ibv_create_qp(ctx->pd, &init_attr);
    if (!ctx->qp)  {
      fprintf(stderr, "Couldn't create QP\n");
      goto clean_cq;
    }

    ibv_query_qp(ctx->qp, &attr, IBV_QP_CAP, &init_attr);
    if (init_attr.cap.max_inline_data >= sizeof(u64Int)) {
      ctx->send_flags |= IBV_SEND_INLINE;
      fprintf(stderr, "use IBV_SEND_INLINE mode\n");
    }
  }

  {
    struct ibv_qp_attr attr = {
      .qp_state        = IBV_QPS_INIT,
      .pkey_index      = 0,
      .port_num        = port,
      .qp_access_flags = 0
    };

    if (ibv_modify_qp(ctx->qp, &attr,
          IBV_QP_STATE              |
          IBV_QP_PKEY_INDEX         |
          IBV_QP_PORT               |
          IBV_QP_ACCESS_FLAGS)) {
      fprintf(stderr, "Failed to modify QP to INIT\n");
      goto clean_qp;
    }
  }

  return ctx;

clean_qp:
  ibv_destroy_qp(ctx->qp);

clean_cq:
  ibv_destroy_cq(ctx->tx_cq);
  ibv_destroy_cq(ctx->rx_cq);

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

clean_ctx:
  free(ctx);

  return NULL;
}

static int pp_post_recv(struct pingpong_context *ctx, int n)
{
  struct ibv_sge list = {
    .addr = (uintptr_t) ctx->rx_buf,
    .lkey = ctx->rx_mr->lkey
  };
  struct ibv_recv_wr wr = {
    .wr_id      = PINGPONG_RECV_WRID,
    .next       = NULL,
    .sg_list    = &list,
    .num_sge    = 1,
  };
  struct ibv_recv_wr *bad_wr = NULL;
  int i;

  u64Int* rx_buf = (u64Int*) ctx->rx_buf;
  for (i = 0; i < n; ++i) {
    wr.wr_id = i | PINGPONG_RECV_WRID;
    list.addr = (uint64_t) &rx_buf[i];
    list.length = sizeof(u64Int);
    int err;
    if (err = ibv_post_recv(ctx->qp, &wr, &bad_wr)) {
      fprintf(stderr, "%d> ibv_post_recv returns %d\n", ctx->rank, err);
      return i;
    }
    if (bad_wr) {
      fprintf(stderr, "post_recv failed\n");
      break;
    }
  }
  return i;
}

static int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
        enum ibv_mtu mtu, int sl,
        struct pingpong_dest *dest, int sgid_idx)
{
  struct ibv_qp_attr attr = {
    .qp_state   = IBV_QPS_RTR,
    .path_mtu   = mtu,
    .dest_qp_num    = dest->qpn,
    .rq_psn     = dest->psn,
    .max_dest_rd_atomic = 1,
    .min_rnr_timer    = 12,
    .ah_attr    = {
      .is_global  = 0,
      .dlid   = dest->lid,
      .sl   = sl,
      .src_path_bits  = 0,
      .port_num = port
    }
  };

  if (dest->gid.global.interface_id) {
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.grh.dgid = dest->gid;
    attr.ah_attr.grh.sgid_index = sgid_idx;
  }
  if (ibv_modify_qp(ctx->qp, &attr,
        IBV_QP_STATE              |
        IBV_QP_AV                 |
        IBV_QP_PATH_MTU           |
        IBV_QP_DEST_QPN           |
        IBV_QP_RQ_PSN             |
        IBV_QP_MAX_DEST_RD_ATOMIC |
        IBV_QP_MIN_RNR_TIMER)) {
    fprintf(stderr, "Failed to modify QP to RTR\n");
    return 1;
  }

  attr.qp_state     = IBV_QPS_RTS;
  attr.timeout      = 14;
  attr.retry_cnt      = 7;
  attr.rnr_retry      = 7;
  attr.sq_psn     = my_psn;
  attr.max_rd_atomic  = 1;
  if (ibv_modify_qp(ctx->qp, &attr,
        IBV_QP_STATE              |
        IBV_QP_TIMEOUT            |
        IBV_QP_RETRY_CNT          |
        IBV_QP_RNR_RETRY          |
        IBV_QP_SQ_PSN             |
        IBV_QP_MAX_QP_RD_ATOMIC)) {
    fprintf(stderr, "Failed to modify QP to RTS\n");
    return 1;
  }

  return 0;
}

static int pp_post_send(struct pingpong_context *ctx)
{
  int i;
  int tx_depth = ctx->tx_depth;

  struct ibv_sge list = {
    .addr = (uintptr_t) ctx->tx_buf,
    .lkey = ctx->tx_mr->lkey
  };
  struct ibv_send_wr wr = {
    .wr_id      = PINGPONG_SEND_WRID,
    .next       = NULL,
    .sg_list    = &list,
    .num_sge    = 1,
    .opcode     = IBV_WR_SEND,
    .send_flags = ctx->send_flags,
  };
  struct ibv_send_wr *bad_wr = NULL;

  u64Int* tx_buf = ctx->tx_buf;
  for(i=0; i<tx_depth; i++) {
    // printf("ibv_post_sending %d-th element\n", i);
    // tx_buf[i] = i;
    wr.wr_id = i | PINGPONG_SEND_WRID;
    list.addr = (uint64_t) &tx_buf[i];
    list.length = sizeof(u64Int);
    if(ibv_post_send(ctx->qp, &wr, &bad_wr)) {
      printf("ibv_post_send returned nz\n");
      return 1;
    }
    if(bad_wr) {
      printf("bad_wr\n");
      return 2;
    }
  }

  return 0;
}

static int pp_post_send_ack(struct pingpong_context *ctx)
{
  struct ibv_sge list = {
    .addr = (uintptr_t) ctx->tx_buf,
    .lkey = ctx->tx_mr->lkey
  };
  struct ibv_send_wr wr = {
    .wr_id      = PINGPONG_SEND_WRID,
    .next       = NULL,
    .sg_list    = &list,
    .num_sge    = 1,
    .opcode     = IBV_WR_SEND,
    .send_flags = ctx->send_flags,
  };
  struct ibv_send_wr *bad_wr = NULL;

  u64Int* tx_buf = ctx->tx_buf;
  wr.wr_id = 0 | PINGPONG_SEND_WRID;
  list.addr = (uint64_t) &tx_buf[0];
  list.length = sizeof(u64Int);
  if(ibv_post_send(ctx->qp, &wr, &bad_wr)) {
    printf("ibv_post_send returned nz\n");
    return 1;
  }
  if(bad_wr) {
    printf("bad_wr\n");
    return 2;
  }

  return 0;
}


static int pp_close_ctx(struct pingpong_context *ctx)
{
  if (ibv_destroy_qp(ctx->qp)) {
    fprintf(stderr, "Couldn't destroy QP\n");
    return 1;
  }

  if (ibv_destroy_cq(ctx->tx_cq)) {
    fprintf(stderr, "Couldn't destroy CQ\n");
    return 1;
  }

  if (ibv_destroy_cq(ctx->rx_cq)) {
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

  srand(rank * time(NULL));

  struct ibv_device       **dev_list;
  struct ibv_device       *ib_dev;
  struct pingpong_context *ctx;
  struct pingpong_dest     my_dest;
  struct pingpong_dest     rem_dest;
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
    fprintf(stderr, "No IB devices found\n");
    return 1;
  }

  ctx = pp_init_ctx(ib_dev, tx_depth, rx_depth, ib_port);
  if (!ctx)
    return 1;
  ctx->rank = rank;
  ctx->nprocs = nprocs;

  int routs = pp_post_recv(ctx, ctx->rx_depth);
  if (routs < ctx->rx_depth) {
    fprintf(stderr, "Couldn't post receive (%d)\n", routs);
    return 1;
  }

  if (pp_get_port_info(ctx->context, ib_port, &ctx->portinfo)) {
    fprintf(stderr, "Couldn't get port info\n");
    return 1;
  }

  my_dest.lid = ctx->portinfo.lid;
  if (ctx->portinfo.link_layer != IBV_LINK_LAYER_ETHERNET &&
              !my_dest.lid) {
    fprintf(stderr, "Couldn't get local LID\n");
    return 1;
  }

  if (gidx >= 0) {
    if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
      fprintf(stderr, "can't read sgid of index %d\n", gidx);
      return 1;
    }
  } else
    memset(&my_dest.gid, 0, sizeof my_dest.gid);

  my_dest.qpn = ctx->qp->qp_num;
  my_dest.psn = rand() & 0xffffff;
  printf("rank %d local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x\n",
         rank, my_dest.lid, my_dest.qpn, my_dest.psn);

  {
    int q = (rank==0?1:0);
    MPI_Sendrecv(&my_dest, sizeof(my_dest), MPI_CHAR, q, SEND_TAG, 
                &rem_dest, sizeof(rem_dest), MPI_CHAR, q, SEND_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  }

  printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x\n",
         rem_dest.lid, rem_dest.qpn, rem_dest.psn);

  ZERO(pp_connect_ctx(ctx, ib_port, my_dest.psn, mtu, sl, &rem_dest, gidx));

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

    if (rank == 0) {
      pp_post_recv(ctx, 1);
      int scnt = 0;
      int err;
      //fprintf(stderr, "%d> Ready to post send (errno=%d, reason=%s)\n", rank, errno, strerror(errno));
      err = pp_post_send(ctx);
      if (err != 0) {
        fprintf(stderr, "Couldn't post send (return %d, errno=%d, reason=%s)\n", err, errno, strerror(errno));
        return 1;
      }
      // wait for send to complete
      while(1) {
        ne = ibv_poll_cq(ctx->tx_cq, 64, wc);
        if (ne < 0) {
          fprintf(stderr, "poll TX CQ failed %d\n", ne);
          return 1;
        }
        if(ne >= 1) {
          scnt += ne;
          // fprintf(stderr, "%d>   sent %d/%d\n", rank, ne, tx_depth);
          if(scnt == tx_depth) {
            break;
          }
        }
      }
      // wait for ack message
      while(1) {
        ne = ibv_poll_cq(ctx->rx_cq, 64, wc);
        if (ne < 0) {
          fprintf(stderr, "poll RX CQ failed\n");
          return 1;
        }
        if(ne >= 1) {
          break;
        }
      }
      N++;
    } else if (rank == 1) {
      int rcnt = 0;
      while(1) {
        ne = ibv_poll_cq(ctx->rx_cq, 64, wc);
        if (ne < 0) {
          fprintf(stderr, "poll TX CQ failed %d\n", ne);
          return 1;
        }
        if (ne >= 1) {
          rcnt += ne;
          //fprintf(stderr, "%d>   received %d/%d\n", rank, ne, rx_depth);
          if(rcnt == rx_depth) {
            routs = pp_post_recv(ctx, ctx->rx_depth);
            if (routs < ctx->rx_depth) {
              fprintf(stderr, "Couldn't post receive (%d)\n", routs);
              return 1;
            }
            break;
          }
        }
      }
      pp_post_send_ack(ctx);
      // wait for ack message
      while(1) {
        ne = ibv_poll_cq(ctx->tx_cq, 64, wc);
        if (ne < 0) {
          fprintf(stderr, "poll RX CQ failed\n");
          return 1;
        }
        if(ne >= 1) {
          break;
        }
      }
      N++;
    }
    // fprintf(stderr, "%d>   next iter\n", rank);
  }
#define PINGPONG_VERIFY
#ifdef PINGPONG_VERIFY
  printf("VERIFYING...\n");
  if(rank == 1) {
    int i;
    u64Int* rx_buf = (u64Int*) ctx->rx_buf;
    for(i=0; i<rx_depth; i++) {
      if(rx_buf[i] != i) {
        printf("VERIFY FAILED: rx_buf[%d] == %llu\n", i, rx_buf[i]);
      }
    }
  }
#endif

  printf("%d Finished\n", rank);
  MPI_Barrier(MPI_COMM_WORLD);

  if (gettimeofday(&end, NULL)) {
    perror("gettimeofday");
    return 1;
  }

  {
    float usec = (end.tv_sec - start.tv_sec) * 1000000 +
      (end.tv_usec - start.tv_usec);
    long long bytes = (long long) tx_depth * iters * sizeof(u64Int);

    printf("%lld bytes in %.6f seconds = %.6f MB/sec = %.6f Mmesg/sec\n",
           bytes, usec / 1000000., 1.0 * bytes / usec, 1.0*tx_depth*iters/usec);
    printf("%d iters in %.6f seconds = %.6f usec/iter\n",
           iters, usec / 1000000., usec / iters);
  }

  if (pp_close_ctx(ctx))
    return 1;

  ibv_free_device_list(dev_list);

  MPI_Finalize();
  return 0;
}
