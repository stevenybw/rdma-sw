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
  int device_cap_flags = device_attr.device_cap_flags;
  printf("  DEVICE_CAP_FLAGS:\n");
  if(device_cap_flags | IBV_DEVICE_XRC) {
    printf("    IBV_DEVICE_RAW_MULTI = 1\n");
  } else {
    printf("    IBV_DEVICE_RAW_MULTI = 0\n");
  }
  if(device_cap_flags | IBV_DEVICE_XRC) {
    printf("    IBV_DEVICE_XRC = 1\n");
  } else {
    printf("    IBV_DEVICE_XRC = 0\n");
  }
  SHOW("%s", device_attr, fw_ver);
  SHOW("%llu", device_attr, max_mr_size);
  SHOW("%d", device_attr, max_qp);
  SHOW("%d", device_attr, max_qp_wr);
  SHOW("%d", device_attr, max_sge);
  SHOW("%d", device_attr, max_cq);
  SHOW("%d", device_attr, max_cq);
  SHOW("%d", device_attr, max_mr);
  SHOW("%d", device_attr, max_srq);
  SHOW("%d", device_attr, max_srq_wr);
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
*/
