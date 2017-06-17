#include <infiniband/driver.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <assert.h>

#define NZ(STATEMENT) assert(STATEMENT != NULL)
#define ZERO(STATEMENT) assert(STATEMENT == 0)
#define SHOW(FMT, OBJECT, ATTRIBUTE) do{printf("  %s = " FMT "\n", #ATTRIBUTE, OBJECT.ATTRIBUTE);} while(0)

#define NUM_CQE 1024

struct pingpong_context {
  struct ibv_context  *context;
  struct ibv_pd   *pd;
  struct ibv_mr   *mr;
  struct ibv_cq   *cq;
  struct ibv_qp   *qp;
  void      *buf;
  int      size;
  int      send_flags;
  int      rx_depth;
  int      pending;
  struct ibv_port_attr     portinfo;
  uint64_t     completion_timestamp_mask;
};

static struct ibv_cq *pp_cq(struct pingpong_context *ctx)
{
  return ctx->cq;
}

#define PAGE_SIZE (256*1024)

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
              int rx_depth, int port,
              int use_event)
{
  struct pingpong_context *ctx;
  int access_flags = IBV_ACCESS_LOCAL_WRITE;

  ctx = malloc(sizeof(*ctx));
  if (!ctx)
    return NULL;

  ctx->size       = size;
  ctx->send_flags = IBV_SEND_SIGNALED;
  ctx->rx_depth   = rx_depth;

  ctx->buf = memalign(PAGE_SIZE, size);
  if (!ctx->buf) {
    fprintf(stderr, "Couldn't allocate work buf.\n");
    goto clean_ctx;
  }

  memset(ctx->buf, 0x7b, size);

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

  ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, access_flags);

  if (!ctx->mr) {
    fprintf(stderr, "Couldn't register MR\n");
    goto clean_pd;
  }

  ctx->cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL,
           NULL, 0);

  if (!pp_cq(ctx)) {
    fprintf(stderr, "Couldn't create CQ\n");
    goto clean_mr;
  }

  {
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr = {
      .send_cq = pp_cq(ctx),
      .recv_cq = pp_cq(ctx),
      .cap     = {
        .max_send_wr  = 1,
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
    if (init_attr.cap.max_inline_data >= size) {
      ctx->send_flags |= IBV_SEND_INLINE;
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
  ibv_destroy_cq(pp_cq(ctx));

clean_mr:
  ibv_dereg_mr(ctx->mr);

clean_pd:
  ibv_dealloc_pd(ctx->pd);

clean_device:
  ibv_close_device(ctx->context);

clean_buffer:
  free(ctx->buf);

clean_ctx:
  free(ctx);

  return NULL;
}

void show_device(struct ibv_device* device) {
  printf("show_device\n");
  printf("  %s = %s\n", "node_type", ibv_node_type_str(device->node_type));
  printf("  %s = %d\n", "transport_type", device->transport_type);
  printf("  %s = %s\n", "name", device->name);
  printf("  %s = %s\n", "dev_name", device->dev_name);
  printf("  %s = %s\n", "dev_path", device->dev_path);
  printf("  %s = %s\n", "ibdev_path", device->ibdev_path);
}

void show_query_device(struct ibv_context *context) {
  printf("show_query_device\n");
  struct ibv_device_attr device_attr;
  ZERO(ibv_query_device(context, &device_attr));
  SHOW("%s", device_attr, fw_ver);
  SHOW("%llu", device_attr, max_mr_size);
  SHOW("%d", device_attr, max_qp);
  SHOW("%d", device_attr, max_qp_wr);
  SHOW("%d", device_attr, max_sge);
  SHOW("%d", device_attr, max_cq);
  SHOW("%d", device_attr, max_cqe);
  printf("  %s = %d\n", "phys_port_cnt", (int) device_attr.phys_port_cnt);
}

void show_query_first_port(struct ibv_context *context) {
  printf("show_query_first_port\n");
  struct ibv_port_attr port_attr;
  ZERO(ibv_query_port(context, 1, &port_attr));
  printf("  %s = %s\n", "state", ibv_port_state_str(port_attr.state));
  SHOW("%d", port_attr, max_mtu);
  SHOW("%d", port_attr, active_mtu);
  SHOW("%d", port_attr, gid_tbl_len);
  SHOW("%d", port_attr, lid);
  SHOW("%d", port_attr, sm_lid);

  // show gid
  {
    union ibv_gid gid;
    ZERO(ibv_query_gid(context, 1, 0, &gid));
    printf("  %s = %llx%llx\n", "gid", gid.global.subnet_prefix, gid.global.interface_id);
  }
}

#define BUFFER_BYTES (64*1024*1024)
#define MEMREGION_PROT (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC | IBV_ACCESS_MW_BIND)
char gbuffer[BUFFER_BYTES]  __attribute((__aligned__(128)));

int main(void) {
  struct ibv_device** devices;
  int num_devices;
  int i;

  devices = ibv_get_device_list(&num_devices);
  printf("get %d devices\n", num_devices);
  for(i=0; i<num_devices; i++) {
    // query
    struct ibv_device* device = devices[i];
    show_device(device);
    struct ibv_context* context;
    context = ibv_open_device(device);
    NZ(context);
    show_query_device(context);
    show_query_first_port(context);

    struct ibv_pd* pd;
    pd = ibv_alloc_pd(context);
    NZ(pd);
    struct ibv_cq* cq;
    cq = ibv_create_cq(context, NUM_CQE, NULL, NULL, 0);
    NZ(cq);

    struct ibv_mr* mr;
    mr = ibv_reg_mr(pd, gbuffer, BUFFER_BYTES, MEMREGION_PROT);
    NZ(mr);
    
    struct ibv_qp_init_attr qp_init_attr;
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    struct ibv_qp* qp;
    qp = ibv_create_qp(pd, &qp_init_attr);
    
    ZERO(ibv_destroy_qp(qp));
    ZERO(ibv_dereg_mr(mr));
    ZERO(ibv_destroy_cq(cq));
    ZERO(ibv_dealloc_pd(pd));
    ZERO(ibv_close_device(context));
  }

  return 0;
}
