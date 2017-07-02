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
#include "ympi.h"

typedef struct YMPID_Rdma_buffer {
  void           *buf;
  size_t          bytes;
  struct ibv_mr  *mr;
} YMPID_Rdma_buffer;

typedef struct YMPID_Pending_buffer {
  void                        *buf;
  uint64_t                     len;
  struct YMPID_Pending_buffer *next;
} YMPID_Pending_buffer;

typedef struct YMPID_Pending_buffer_list {
  YMPID_Pending_buffer        *free_pool;
  YMPID_Pending_buffer        *head;
  YMPID_Pending_buffer        *tail;
} YMPID_Pending_buffer_list;

typedef struct YMPID_Recv_win {
  YMPID_Rdma_buffer          buffer;
  YMPID_Pending_buffer_list  pending_free_list;
  YMPID_Pending_buffer_list *pending_recv_queue;
  int                        idx;
  int                        len;
  int                        num_acknowledged;
  int                        num_garbage;
} YMPID_Recv_win;

static inline YMPID_Pending_buffer* YMPID_Pending_buffer_fetch(YMPID_Pending_buffer_list* list) {
  YMPID_Pending_buffer* ret = list->head;
  if(ret != NULL) {
    if(list->head == list->tail) {
      list->head = NULL;
      list->tail = NULL;
    } else {
      list->head = ret->next;
    }
  }
  return ret;
}

static inline void YMPID_Pending_buffer_insert(YMPID_Pending_buffer_list* list, YMPID_Pending_buffer* e) {
  if(list->head == NULL) {
    list->head = e;
    list->tail = e;
  } else {
    list->tail->next = e;
    list->tail = e;
  }
}

typedef union YMPID_Wrid {
  struct {
    uint32_t tag;
    uint32_t id;
  } tagid;
  uint64_t val;
} YMPID_Wrid;

enum {
  RECV_WRID = 1,
  SEND_WRID = 2,
};

/*
 * design choices:
 * 1. all-to-all connection (lazy connection in the future)
 */
typedef struct YMPID_Context {
  struct ibv_context  *context;
  struct ibv_port_attr portinfo;
  struct ibv_pd       *pd;
  struct ibv_cq       *cq;
  struct ibv_srq      *srq;
  YMPID_Recv_win       rx_win;
  struct ibv_qp      **qp_list;
  int                 *qp_rank;

  struct ibv_sge     *recv_sge_list;
  struct ibv_recv_wr *recv_wr_list;

  int      port;
  int      rank;
  int      nprocs;
  int      pending_send_wr;
  uint32_t max_inline_data;

#if YMPI_SW
  int      cgid;
#endif
} YMPID_Context;

static YMPID_Context *ctx;

#if YMPI_SW

extern long sys_m_cgid();

static inline int offset_to_qpn(int cgid, int nprocs, int offset) {
  return 1024 + cgid*((8*1024*1024)-1024)/4 + nprocs + offset;
}

static inline int qpn_to_offset(int cgid, int nprocs, int qpn) {
  return qpn - 1024 - cgid*((8*1024*1024)-1024)/4 - nprocs;
}

static inline int YMPID_Qpn_rank(int qpn) {
  return qpn_to_offset(ctx->cgid, ctx->nprocs, qpn);
}

#else

static inline int YMPID_Qpn_rank(int qpn) {
  int rank = ctx->qp_rank[qpn % YMPI_QPN_HASH_SIZE];
  assert(rank >= 0);
  return rank;
}

#endif //YMPI_SW

static YMPID_Context* YMPID_Context_create(struct ibv_device *ib_dev, int ib_port, int rank, int nprocs)
{
  ctx = malloc(sizeof(*ctx));
  if (!ctx)
    return NULL;

  ctx->port        = ib_port;
  ctx->rank        = rank;
  ctx->nprocs      = nprocs;
  ctx->pending_send_wr = 0;

#if YMPI_SW
  int cgid         = sys_m_cgid();
  assert(cgid < 4);
  ctx->cgid        = cgid;
#endif

  ctx->context = ibv_open_device(ib_dev);
  if (!ctx->context) {
    fprintf(stderr, "Couldn't get context for %s\n",
      ibv_get_device_name(ib_dev));
    goto clean_ctx;
  }

  if (ibv_query_port(ctx->context, ib_port, &ctx->portinfo)) {
    fprintf(stderr, "Couldn't get port info\n");
    goto clean_ctx;
  }

  ctx->pd = ibv_alloc_pd(ctx->context);
  if (!ctx->pd) {
    fprintf(stderr, "Couldn't allocate PD\n");
    goto clean_device;
  }

  ctx->cq = ibv_create_cq(ctx->context, 4*YMPI_PREPOST_DEPTH, NULL,
           NULL, 0);
  if (!ctx->cq) {
    fprintf(stderr, "Couldn't create CQ\n");
    goto clean_pd;
  }

  // create SRQ
  {
    struct ibv_srq_init_attr attr = {
      .attr = {
        .max_wr  = 2 * YMPI_PREPOST_DEPTH,
        .max_sge = 1
      }
    };

    ctx->srq = ibv_create_srq(ctx->pd, &attr);
    if (!ctx->srq)  {
      fprintf(stderr, "Couldn't create SRQ\n");
      goto clean_cq;
    }
  }

  // create rx_win
  {
    int access_flags = IBV_ACCESS_LOCAL_WRITE;
    void           *buf = NULL;
    struct ibv_mr  *mr  = NULL;

    size_t bytes = YMPI_PREPOST_DEPTH * YMPI_VBUF_BYTES;
    ctx->rx_win.buffer.bytes = bytes;
    ctx->rx_win.idx  = 0;
    ctx->rx_win.len  = 0;
    buf              = memalign(YMPI_PAGE_SIZE, bytes);  NZ(buf);
    memset(buf, 0x7b, bytes);
    ctx->rx_win.buffer.buf   = buf;
    mr = ibv_reg_mr(ctx->pd, buf, bytes, access_flags);
    ctx->rx_win.buffer.mr    = mr;

    if (!mr) {
      fprintf(stderr, "Couldn't register MR\n");
      goto clean_srq;
    }

    // init recv queue linked list
    {
      int i;
      YMPID_Pending_buffer *free_pool = (YMPID_Pending_buffer*)  malloc((YMPI_PREPOST_DEPTH) * sizeof(YMPID_Pending_buffer));
      YMPID_Pending_buffer_list *pending_recv_queue  = (YMPID_Pending_buffer_list*) malloc(nprocs * sizeof(YMPID_Pending_buffer_list));
      NZ(free_pool); memset(free_pool, 0, (YMPI_PREPOST_DEPTH) * sizeof(YMPID_Pending_buffer));
      NZ(pending_recv_queue); memset(pending_recv_queue, 0, nprocs * sizeof(YMPID_Pending_buffer_list));
      for(i=0; i<YMPI_PREPOST_DEPTH; i++) {
        if(i == (YMPI_PREPOST_DEPTH-1)) {
          free_pool[i].next = NULL;
        } else {
          free_pool[i].next = &free_pool[i+1];
        }
      }

      ctx->rx_win.pending_free_list.free_pool = free_pool;
      ctx->rx_win.pending_free_list.head      = &free_pool[0];
      ctx->rx_win.pending_free_list.tail      = &free_pool[YMPI_PREPOST_DEPTH-1];
      ctx->rx_win.pending_recv_queue          = pending_recv_queue;
    }
  }

  // create qp_list & qp_rank
  {
    LOGDS("    creating QP\n");
    int i;
    struct ibv_qp** qp_list = (struct ibv_qp**) malloc(nprocs * sizeof(uintptr_t)); NZ(qp_list);
    ctx->qp_list = qp_list;
    int *qp_rank = (int*) malloc(YMPI_QPN_HASH_SIZE * sizeof(int));
    NZ(qp_rank); memset(qp_rank, 0xFF, YMPI_QPN_HASH_SIZE * sizeof(int));
    ctx->qp_rank = qp_rank;

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
#if YMPI_SW_DIY
      {
        qp = ibv_create_qp_diy(ctx->pd, &init_attr, offset_to_qpn(cgid, nprocs, i));
        // LOGD("    creating QP[%d] with qpn=%d\n", i, offset_to_qpn(cgid, nprocs, i));
      }
#else
      {
        qp = ibv_create_qp(ctx->pd, &init_attr);
      }
#endif
      if (!qp)  {
        fprintf(stderr, "Couldn't create QP[%d], errno=%d[%s]\n", i, errno, strerror(errno));
        goto clean_qp_list;
      }
      ibv_query_qp(qp, &attr, IBV_QP_CAP, &init_attr);
      ctx->max_inline_data = init_attr.cap.max_inline_data;
      qp_list[i] = qp;
      assert(qp_rank[qp->qp_num % YMPI_QPN_HASH_SIZE] == -1);
      qp_rank[qp->qp_num % YMPI_QPN_HASH_SIZE] = i;
    }

    LOGDS("    setting QP to INIT\n");
    for(i=0; i<nprocs; i++) {
      struct ibv_qp* qp = qp_list[i];
      struct ibv_qp_attr attr = {
        .qp_state        = IBV_QPS_INIT,
        .pkey_index      = 0,
        .port_num        = ib_port,
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
    if(ibv_destroy_qp(ctx->qp_list[i])) {
      LOGD("Couldn't destroy QP[%d]\n", i);
    }
    ctx->qp_list[i] = NULL;
  }
}

clean_qp_list:
  free(ctx->qp_list);

clean_rx_win:
{
  if(ibv_dereg_mr(ctx->rx_win.buffer.mr)) {
    LOGD("Couldn't deregister rx_win\n");
  }
  free(ctx->rx_win.buffer.buf);
}

clean_srq:
{
  if(ibv_destroy_srq(ctx->srq)) {
    LOGD("Couldn't destroy srq\n");
  }
}

clean_cq:
  ibv_destroy_cq(ctx->cq);

clean_pd:
  ibv_dealloc_pd(ctx->pd);

clean_device:
  ibv_close_device(ctx->context);

clean_ctx:
  free(ctx);

  return NULL;
}

static int YMPID_Context_destroy(YMPID_Context* ctx) {
  int nprocs = ctx->nprocs;

clean_qp:
{
  int i;
  for(i=0; i<nprocs; i++) {
    if(ibv_destroy_qp(ctx->qp_list[i])) {
      LOGD("Couldn't destroy QP[%d]\n", i);
    }
    ctx->qp_list[i] = NULL;
  }
}

clean_qp_list:
  free(ctx->qp_list);

clean_rx_win:
{
  if(ibv_dereg_mr(ctx->rx_win.buffer.mr)) {
    LOGD("Couldn't deregister rx_win\n");
  }
  free(ctx->rx_win.buffer.buf);
}

clean_srq:
{
  if(ibv_destroy_srq(ctx->srq)) {
    LOGD("Couldn't destroy srq\n");
  }
}

clean_cq:
  ibv_destroy_cq(ctx->cq);

clean_pd:
  ibv_dealloc_pd(ctx->pd);

clean_device:
  ibv_close_device(ctx->context);

clean_ctx:
  free(ctx);

  return 0;
}

/*
 * TODO
 * PHENOMENA the order of request id may be different to the posted order,
 * i.e. assume we post in the 0,1,2,3,... order, we may recv in 2,0,1,3,...
 * EXPLANATION  The QP will fetch a WR from SRQ upon receiving a IBV_SEND,
 * possibly caused by multiple QP can process message concurrently, causing
 * completion order different from fetching order.
 */

static inline int YMPID_Recv_win_recv() {
  int idx = ctx->rx_win.idx;
  int len = ctx->rx_win.len;
  idx = (idx + 1) % YMPI_PREPOST_DEPTH;
  len = len - 1;
  assert(len >= 0);
  ctx->rx_win.idx = idx;
  ctx->rx_win.len = len;

  return 0;
}

static int YMPID_Recv_win_refill(YMPID_Context* ctx) {
  int       idx = ctx->rx_win.idx;
  int       len = ctx->rx_win.len;
  char*     buf = (char*) ctx->rx_win.buffer.buf;
  //size_t    bytes  = ctx->rx_win.buffer.bytes;

  struct ibv_sge* recv_sge_list = ctx->recv_sge_list;
  struct ibv_recv_wr* recv_wr_list = ctx->recv_wr_list;

  struct ibv_recv_wr *bad_wr = NULL;

  int i;
  int num_recv = YMPI_PREPOST_DEPTH - len;
  int id = (idx + len) % YMPI_PREPOST_DEPTH;
  
  for(i=0; i<num_recv; i++) {
    YMPID_Wrid wr_id = {
      .tagid = {
        .tag  = RECV_WRID,
        .id   = id,
      }
    };
    recv_sge_list[i].addr = (uintptr_t) &buf[id * YMPI_VBUF_BYTES];
    recv_wr_list[i].wr_id = wr_id.val;
    id = (id + 1) % YMPI_PREPOST_DEPTH;
  }
  recv_wr_list[num_recv-1].next = NULL;

  int err;
  if ((err = ibv_post_srq_recv(ctx->srq, recv_wr_list, &bad_wr))) {
    LOGD("ibv_post_srq_recv failed with %d, errno=%d [%s]\n", err, errno, strerror(errno));
    return err;
  }
  if (bad_wr) {
    LOGD("post_srq_recv has bad_wr\n");
    return -1;
  }
  recv_wr_list[num_recv-1].next = &recv_wr_list[num_recv];

  ctx->rx_win.len = YMPI_PREPOST_DEPTH;

  return 0;
}

static int YMPID_Context_connect(YMPID_Context *ctx, enum ibv_mtu mtu, 
              int sl, int* local_psn_list, int* remote_lid_list, 
              int* remote_psn_list, int* remote_qpn_list)
{
  int i;
  int port = ctx->port;
  // int rank = ctx->rank;
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


int YMPI_Alloc(YMPI_Rdma_buffer* buffer, size_t bytes) {
  YMPID_Rdma_buffer *buffer_d = NULL;
  int access_flags = IBV_ACCESS_LOCAL_WRITE;

  buffer_d = malloc(sizeof(YMPID_Rdma_buffer)); NZ(buffer_d);
  buffer_d->buf   = memalign(YMPI_PAGE_SIZE, bytes); NZ(buffer_d->buf);
  memset(buffer_d->buf, 0x7b, bytes);
  buffer_d->bytes = bytes;
  buffer_d->mr    = ibv_reg_mr(ctx->pd, buffer_d->buf, bytes, access_flags); NZ(buffer_d->mr);
  (*buffer) = (uintptr_t) buffer_d;

  return 0;
}

int YMPI_Dealloc(YMPI_Rdma_buffer* buffer) {
  YMPID_Rdma_buffer *buffer_d = (YMPID_Rdma_buffer*) (*buffer);
  ZERO(ibv_dereg_mr(buffer_d->mr));
  free(buffer_d->buf);
  free(buffer_d);

  return 0;
}

int YMPI_Get_buffer(YMPI_Rdma_buffer buffer, uintptr_t* buf) {
  YMPID_Rdma_buffer *buffer_d = (YMPID_Rdma_buffer*) buffer;
  (*buf) = (uintptr_t) buffer_d->buf;

  return 0;
}

int YMPI_Init(int *argc, char ***argv) {
  int rank, nprocs;
  int ib_port = 1;
  
  if(MPI_SUCCESS != MPI_Init(argc, argv)) {
    return -1;
  }

  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

  srand48(rank * time(NULL));

  // initialize ctx
  {
    LOGDS("  initialize\n");
    struct ibv_device       **dev_list;
    struct ibv_device       *ib_dev;
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
      LOGD("Failed to get IB devices list");
      return -1;
    }

    ib_dev = *dev_list;
    if (!ib_dev) {
      LOGD("No IB devices found\n");
      return -1;
    }

    ctx = YMPID_Context_create(ib_dev, ib_port, rank, nprocs);
    if (!ctx) {
      return -1;
    }

    struct ibv_sge     *recv_sge_list = NULL;
    struct ibv_recv_wr *recv_wr_list  = NULL;
    {
      int i;
      recv_sge_list = (struct ibv_sge*) malloc(YMPI_PREPOST_DEPTH * sizeof(struct ibv_sge));         NZ(recv_sge_list);
      recv_wr_list = (struct ibv_recv_wr*) malloc(YMPI_PREPOST_DEPTH * sizeof(struct ibv_recv_wr));  NZ(recv_wr_list);
      memset(recv_sge_list, 0, YMPI_PREPOST_DEPTH * sizeof(struct ibv_sge));
      memset(recv_wr_list , 0, YMPI_PREPOST_DEPTH * sizeof(struct ibv_recv_wr));

      for(i=0; i<YMPI_PREPOST_DEPTH; i++) {
        recv_sge_list[i].length = YMPI_VBUF_BYTES;
        recv_sge_list[i].lkey   = ctx->rx_win.buffer.mr->lkey;

        if(i == (YMPI_PREPOST_DEPTH-1)) {
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
    ctx->recv_sge_list = recv_sge_list;
    ctx->recv_wr_list  = recv_wr_list;
  }

  // pre-post receive requests
  {
    LOGDS("  post_receive\n");
    if (YMPID_Recv_win_refill(ctx)) {
      LOGD("post_receive failed\n");
      return -1;
    }
  }

  // exchange address & establish connection
  {
    int i;
    int  local_lid = -1;
    int *local_psn_list = NULL;
    int *local_qpn_list = NULL;
    int *remote_lid_list = NULL;
    int *remote_psn_list = NULL;
    int *remote_qpn_list = NULL;
    LOGDS("  exchange address\n");
    local_lid = ctx->portinfo.lid;
    if (!local_lid) {
      fprintf(stderr, "Couldn't get local LID\n");
      return 1;
    }
    local_psn_list = (int*) malloc(nprocs * sizeof(int)); NZ(local_psn_list);
    local_qpn_list = (int*) malloc(nprocs * sizeof(int)); NZ(local_qpn_list);

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



    LOGDS("  establish connection\n");
    enum ibv_mtu mtu = IBV_MTU_2048;
    int          sl  = 0;
    if (YMPID_Context_connect(ctx, mtu, sl, local_psn_list, remote_lid_list, remote_psn_list, remote_qpn_list)) {
      return -1;
    }
  }

  return 0;
}

int YMPI_Finalize() {
  YMPID_Context_destroy(ctx);
  ctx = NULL;

  if(MPI_SUCCESS != MPI_Finalize()) {
    return -1;
  }

  return 0;
}

// post the **buffer** with specific **length of bytes** to **dest**
int YMPI_Zsend(YMPI_Rdma_buffer buffer, size_t offset, size_t bytes, int dest) {
  int send_flags;
  YMPID_Rdma_buffer* buffer_d = (YMPID_Rdma_buffer*) buffer;
  assert(offset + bytes <= buffer_d->bytes);

  if (bytes < ctx->max_inline_data) {
    send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
  } else {
    send_flags = IBV_SEND_SIGNALED;
  }

  YMPID_Wrid wr_id = {
    .tagid = {
      .tag = SEND_WRID,
      .id  = dest,
    }
  };

  struct ibv_sge sge = {
    .addr   = (uint64_t) &((char*) buffer_d->buf)[offset],
    .length = bytes,
    .lkey   = buffer_d->mr->lkey,
  };

  struct ibv_send_wr wr = {
    .wr_id      = (uint64_t) wr_id.val,
    .next       = NULL,
    .sg_list    = &sge,
    .num_sge    = 1,
    .opcode     = IBV_WR_SEND,
    .send_flags = send_flags,
  };

  if(bytes == 0) {
    wr.sg_list = NULL;
    wr.num_sge = 0;
  }

  struct ibv_send_wr* bad_wr = NULL;

  int err;
  if((err = ibv_post_send(ctx->qp_list[dest], &wr, &bad_wr))) {
    LOGD("ibv_post_send to %d returned %d, errno = %d[%s]\n", dest, err, errno, strerror(errno));
    return -1;
  }

  if(bad_wr) {
    LOGD("bad_wr\n");
    return -1;
  }

  ctx->pending_send_wr++;

  return 0;
}

int YMPI_Zflush() {
  int ne = 0;
  struct ibv_wc wc[64];
  char* rx_buf = ctx->rx_win.buffer.buf;
  
  YMPID_Pending_buffer_list* pending_free_list  = &ctx->rx_win.pending_free_list;
  YMPID_Pending_buffer_list* pending_recv_queue = ctx->rx_win.pending_recv_queue;

  // wait for incoming completions
  int num_recv = 0;
  int rcnt = 0;

  while((ctx->pending_send_wr > 0) || (rcnt < num_recv)) {
    ne = ibv_poll_cq(ctx->cq, 64, wc);
    if (ne < 0) {
      LOGD("pool CQ failed\n");
      exit(-1);
    } else if (ne >= 1) {
      int i;
      for(i=0; i<ne; i++) {
        YMPID_Wrid wr_id = {
          .val = wc[i].wr_id,
        };
        if(wc[i].status != IBV_WC_SUCCESS) {
          LOGD("Failed status %s (%d) for wr_id %d:%d\n", 
            ibv_wc_status_str(wc[i].status), wc[i].status, (int) wr_id.tagid.tag, (int) wr_id.tagid.id);
          exit(-1);
        }
        switch ((int) wr_id.tagid.tag) {
          case SEND_WRID:
          {
            ctx->pending_send_wr--;
            assert(ctx->pending_send_wr>=0);
            break;
          }

          case RECV_WRID:
          {
            int rb_id = wr_id.tagid.id;
            int idx  = ctx->rx_win.idx;
            int len  = ctx->rx_win.len;
            int dist = (rb_id + YMPI_PREPOST_DEPTH - idx) % YMPI_PREPOST_DEPTH;
            if(dist < len) {
              // wr lies in recv window
              num_recv += (dist + 1);
              len = len - dist - 1;
              idx = (rb_id + 1) % YMPI_PREPOST_DEPTH;
            }
            rcnt++;
            assert(len >= 0);
            ctx->rx_win.idx = idx;
            ctx->rx_win.len = len;

            int msg_src = YMPID_Qpn_rank(wc[i].qp_num);
            YMPID_Pending_buffer* pb = YMPID_Pending_buffer_fetch(pending_free_list);
            pb->buf  = &rx_buf[rb_id * YMPI_VBUF_BYTES];
            pb->len  = wc[i].byte_len;
            pb->next = NULL;
            YMPID_Pending_buffer_insert(&pending_recv_queue[msg_src], pb);
            break;
          }
        }
      }
    }
  }
  return 0;
}

int YMPI_Zrecv(void** recv_buffer_ptr, uint64_t* recv_buffer_len_ptr, int source) {
  int ne = 0;
  struct ibv_wc wc[64];
  char* rx_buf = ctx->rx_win.buffer.buf;
  
  YMPID_Pending_buffer_list* pending_free_list  = &ctx->rx_win.pending_free_list;
  YMPID_Pending_buffer_list* pending_recv_queue = ctx->rx_win.pending_recv_queue;

  // if exists in pending buffer
  {
    YMPID_Pending_buffer* pb = YMPID_Pending_buffer_fetch(&pending_recv_queue[source]);
    if(pb != NULL) {
      ctx->rx_win.num_acknowledged++;
      (*recv_buffer_ptr) = pb->buf;
      (*recv_buffer_len_ptr) = pb->len;
      YMPID_Pending_buffer_insert(pending_free_list, pb);
      return 0;
    }
  }

  // otherwise, wait for incoming message
  int done = 0;
  int num_recv = 0;
  int rcnt = 0;

  while((!done) || (rcnt<num_recv)) {
    ne = ibv_poll_cq(ctx->cq, 64, wc);
    if (ne < 0) {
      LOGD("pool CQ failed\n");
      exit(-1);
    } else if (ne >= 1) {
      int i;
      for(i=0; i<ne; i++) {
        YMPID_Wrid wr_id = {
          .val = wc[i].wr_id,
        };
        if(wc[i].status != IBV_WC_SUCCESS) {
          LOGD("Failed status %s (%d) for wr_id %d:%d\n", 
            ibv_wc_status_str(wc[i].status), wc[i].status, (int) wr_id.tagid.tag, (int) wr_id.tagid.id);
          exit(-1);
        }
        switch ((int) wr_id.tagid.tag) {
          case SEND_WRID:
          {
            ctx->pending_send_wr--;
            // LOGD("send completion [pending=%d]\n", ctx->pending_send_wr);
            assert(ctx->pending_send_wr>=0);
            break;
          }

          case RECV_WRID:
          {
            int rb_id = wr_id.tagid.id;
            int idx  = ctx->rx_win.idx;
            int len  = ctx->rx_win.len;
            int dist = (rb_id + YMPI_PREPOST_DEPTH - idx) % YMPI_PREPOST_DEPTH;
            // LOGD("recv completion (%d/%d) [id=%d, idx=%d, len=%d, next_idx=%d, next_len=%d]\n", rcnt, num_recv, rb_id, idx, len, (rb_id + 1) % YMPI_PREPOST_DEPTH, len - dist - 1);
            if(dist < len) {
              // wr lies in recv window
              num_recv += (dist + 1);
              len = len - dist - 1;
              idx = (rb_id + 1) % YMPI_PREPOST_DEPTH;
            }
            rcnt++;
            assert(len >= 0);
            ctx->rx_win.idx = idx;
            ctx->rx_win.len = len;

            int msg_src = YMPID_Qpn_rank(wc[i].qp_num);
            if(msg_src == source && (!done)) {
              ctx->rx_win.num_acknowledged++;
              (*recv_buffer_ptr) = &rx_buf[rb_id * YMPI_VBUF_BYTES];
              (*recv_buffer_len_ptr) = wc[i].byte_len;
              done = 1;
            } else {
              YMPID_Pending_buffer* pb = YMPID_Pending_buffer_fetch(pending_free_list);
              pb->buf  = &rx_buf[rb_id * YMPI_VBUF_BYTES];
              pb->len  = wc[i].byte_len;
              pb->next = NULL;
              YMPID_Pending_buffer_insert(&pending_recv_queue[msg_src], pb);
            }
            break;
          }
        }
      }
    }
  }
  return 0;
}

/*
// wait for exactly **num_message** messages, return the array of pointers, and the length for each message by argument.
int YMPI_Zrecvany(int num_message, void* recv_buffers[], uint64_t recv_buffers_len[]) {
  int ne;
  int rcnt = 0;
  struct ibv_wc wc[64];
  char* rx_buf = ctx->rx_win.buffer.buf;

  while(rcnt < num_message) {
    ne = ibv_poll_cq(ctx->cq, 64, wc);
    if (ne < 0) {
      LOGD("poll CQ failed\n");
      exit(-1);
    } else if (ne >= 1) {
      //LOGV("complete %d request\n", ne);
      int i;
      for(i=0; i<ne; i++) {
        YMPID_Wrid wr_id;
        wr_id.val = wc[i].wr_id;
        if(wc[i].status != IBV_WC_SUCCESS) {
          LOGD("Failed status %s (%d) for wr_id %d:%d\n", 
            ibv_wc_status_str(wc[i].status), wc[i].status, (int) wr_id.tagid.tag, (int) wr_id.tagid.id);
          exit(-1);
        }
        switch ((int) wr_id.tagid.tag) {
        case SEND_WRID:
          ctx->pending_index--;
          assert(ctx->pending_index>=0);
          //LOGV("send complete [%d/%d]\n", scnt, num_sent);
          break;

        case RECV_WRID:
          recv_buffers[rcnt]     = &rx_buf[ctx->rx_win.idx * YMPI_VBUF_BYTES];
          recv_buffers_len[rcnt] = wc[i].byte_len;
          YMPID_Recv_win_recv();
          rcnt++;
          //LOGV("recv complete [%d]\n", rcnt);
          break;

        default:
          LOGD("unknown wr_id = %d:%d\n", wr_id.tagid.tag, wr_id.tagid.id);
          return 1;
          break;
        }
      }
    }
  }

  return 0;
}
*/

// all the returned pointers will be inaccessible after this
int YMPI_Return() {
  YMPID_Recv_win_refill(ctx);
  return 0;
}

