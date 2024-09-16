/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2006 Cisco Systems.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/time.h>
#include <stdlib.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>


#include <infiniband/verbs.h>

#define WC_BATCH (10)


// ours

#define MAX_INLINE 60
#define MEGABIT 1048576
#define MEGA_POWER 20
#define PORT 23330
#define GIGABIT 1073741824
#define KB4 4096
#define NUM_DEF_CLIENTS 4
//

enum {
    PINGPONG_RECV_WRID = 1,
    PINGPONG_SEND_WRID = 2,
};

static int page_size;


struct pingpong_context {
    struct ibv_context		*context;
    struct ibv_comp_channel	*channel;
    struct ibv_pd		*pd;
    struct ibv_mr		*mr;
    struct ibv_cq		*cq;
    struct ibv_qp		*qp;
    char*               buf; //todo need to check that it doesnt screw the other
    // calls to buff
    int				size;
    int				rx_depth;
    int				routs;
    int             set;
    struct ibv_port_attr	portinfo;
};

struct pingpong_dest {
    int lid;
    int qpn;
    int psn;
    union ibv_gid gid;
};

enum ibv_mtu pp_mtu_to_enum(int mtu)
{
  switch (mtu) {
    case 256:  return IBV_MTU_256;
    case 512:  return IBV_MTU_512;
    case 1024: return IBV_MTU_1024;
    case 2048: return IBV_MTU_2048;
    case 4096: return IBV_MTU_4096;
    default:   return -1;
  }
}

uint16_t pp_get_local_lid(struct ibv_context *context, int port)
{
  struct ibv_port_attr attr;

  if (ibv_query_port(context, port, &attr))
    return 0;

  return attr.lid;
}

int pp_get_port_info(struct ibv_context *context, int port,
                     struct ibv_port_attr *attr)
{
  return ibv_query_port(context, port, attr);
}

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
  char tmp[9];
  uint32_t v32;
  int i;

  for (tmp[8] = 0, i = 0; i < 4; ++i) {
    memcpy(tmp, wgid + i * 8, 8);
    sscanf(tmp, "%x", &v32);
    *(uint32_t *)(&gid->raw[i * 4]) = ntohl(v32);
  }
}

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
  int i;

  for (i = 0; i < 4; ++i)
    sprintf(&wgid[i * 8], "%08x", htonl(*(uint32_t *)(gid->raw + i * 4)));
}

static int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
                          enum ibv_mtu mtu, int sl,
                          struct pingpong_dest *dest, int sgid_idx)
{
  struct ibv_qp_attr attr = {
      .qp_state		= IBV_QPS_RTR,
      .path_mtu		= mtu,
      .dest_qp_num		= dest->qpn,
      .rq_psn			= dest->psn,
      .max_dest_rd_atomic	= 1,
      .min_rnr_timer		= 12,
      .ah_attr		= {
          .is_global	= 0,
          .dlid		= dest->lid,
          .sl		= sl,
          .src_path_bits	= 0,
          .port_num	= port
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

  attr.qp_state	    = IBV_QPS_RTS;
  attr.timeout	    = 14;
  attr.retry_cnt	    = 7;
  attr.rnr_retry	    = 7;
  attr.sq_psn	    = my_psn;
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

static struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
                                                 const struct pingpong_dest *my_dest)
{
  struct addrinfo *res, *t;
  struct addrinfo hints = {
      .ai_family   = AF_INET,
      .ai_socktype = SOCK_STREAM
  };
  char *service;
  char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
  int n;
  int sockfd = -1;
  struct pingpong_dest *rem_dest = NULL;
  char gid[33];

  if (asprintf(&service, "%d", port) < 0)
    return NULL;

  n = getaddrinfo(servername, service, &hints, &res);

  if (n < 0) {
    fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
    free(service);
    return NULL;
  }

  for (t = res; t; t = t->ai_next) {
    sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
    if (sockfd >= 0) {
      if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
        break;
      close(sockfd);
      sockfd = -1;
    }
  }

  freeaddrinfo(res);
  free(service);

  if (sockfd < 0) {
    fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
    return NULL;
  }

  gid_to_wire_gid(&my_dest->gid, gid);
  sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid);
  if (write(sockfd, msg, sizeof msg) != sizeof msg) {
    fprintf(stderr, "Couldn't send local address\n");
    goto out;
  }

  if (read(sockfd, msg, sizeof msg) != sizeof msg) {
    perror("client read");
    fprintf(stderr, "Couldn't read remote address\n");
    goto out;
  }

  write(sockfd, "done", sizeof "done");

  rem_dest = malloc(sizeof *rem_dest);
  if (!rem_dest)
    goto out;

  sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
  wire_gid_to_gid(gid, &rem_dest->gid);

  out:
  close(sockfd);
  return rem_dest;
}

static struct pingpong_dest *pp_server_exch_dest(struct pingpong_context *ctx,
                                                 int ib_port, enum ibv_mtu mtu,
                                                 int port, int sl,
                                                 const struct pingpong_dest *my_dest,
                                                 int sgid_idx)
{
  struct addrinfo *res, *t;
  struct addrinfo hints = {
      .ai_flags    = AI_PASSIVE,
      .ai_family   = AF_INET,
      .ai_socktype = SOCK_STREAM
  };
  char *service;
  char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
  int n;
  int sockfd = -1, connfd;
  struct pingpong_dest *rem_dest = NULL;
  char gid[33];

  if (asprintf(&service, "%d", port) < 0)
    return NULL;

  n = getaddrinfo(NULL, service, &hints, &res);

  if (n < 0) {
    fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
    free(service);
    return NULL;
  }

  for (t = res; t; t = t->ai_next) {
    sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
    if (sockfd >= 0) {
      n = 1;

      setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

      if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
        break;
      close(sockfd);
      sockfd = -1;
    }
  }

  freeaddrinfo(res);
  free(service);

  if (sockfd < 0) {
    fprintf(stderr, "Couldn't listen to port %d\n", port);
    return NULL;
  }

  listen(sockfd, 1);
  connfd = accept(sockfd, NULL, 0);
  close(sockfd);
  if (connfd < 0) {
    fprintf(stderr, "accept() failed\n");
    return NULL;
  }

  n = read(connfd, msg, sizeof msg);
  if (n != sizeof msg) {
    perror("server read");
    fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
    goto out;
  }

  rem_dest = malloc(sizeof *rem_dest);
  if (!rem_dest)
    goto out;

  sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
  wire_gid_to_gid(gid, &rem_dest->gid);

  if (pp_connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest, sgid_idx)) {
    fprintf(stderr, "Couldn't connect to remote QP\n");
    free(rem_dest);
    rem_dest = NULL;
    goto out;
  }


  gid_to_wire_gid(&my_dest->gid, gid);
  sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid);
  if (write(connfd, msg, sizeof msg) != sizeof msg) {
    fprintf(stderr, "Couldn't send local address\n");
    free(rem_dest);
    rem_dest = NULL;
    goto out;
  }

  read(connfd, msg, sizeof msg);

  out:
  close(connfd);
  return rem_dest;
}

#include <sys/param.h>

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
                                            int rx_depth, int tx_depth, int port,
                                            int use_event, int is_server)
{
  struct pingpong_context *ctx;

  ctx = calloc(1, sizeof *ctx);
  if (!ctx)
    return NULL;

  ctx->size     = size;
  ctx->rx_depth = rx_depth;
  ctx->routs    = rx_depth;

  ctx->buf = malloc(roundup(size, page_size));
  if (!ctx->buf) {
    fprintf(stderr, "Couldn't allocate work buf.\n");
    return NULL;
  }

  memset(ctx->buf, 0x7b + is_server, size);

  ctx->context = ibv_open_device(ib_dev);
  if (!ctx->context) {
    fprintf(stderr, "Couldn't get context for %s\n",
            ibv_get_device_name(ib_dev));
    return NULL;
  }

  if (use_event) {
    ctx->channel = ibv_create_comp_channel(ctx->context);
    if (!ctx->channel) {
      fprintf(stderr, "Couldn't create completion channel\n");
      return NULL;
    }
  } else
    ctx->channel = NULL;

  ctx->pd = ibv_alloc_pd(ctx->context);
  if (!ctx->pd) {
    fprintf(stderr, "Couldn't allocate PD\n");
    return NULL;
  }

  ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE);
  if (!ctx->mr) {
    fprintf(stderr, "Couldn't register MR\n");
    return NULL;
  }

  ctx->cq = ibv_create_cq(ctx->context, rx_depth + tx_depth, NULL,
                          ctx->channel, 0);
  if (!ctx->cq) {
    fprintf(stderr, "Couldn't create CQ\n");
    return NULL;
  }

  {
    struct ibv_qp_init_attr attr = {
        .send_cq = ctx->cq,
        .recv_cq = ctx->cq,
        .cap     = {
            .max_send_wr  = tx_depth,
            .max_recv_wr  = rx_depth,
            .max_send_sge = 1,
            .max_recv_sge = 1
        },
        .qp_type = IBV_QPT_RC
    };

    ctx->qp = ibv_create_qp(ctx->pd, &attr);
    if (!ctx->qp)  {
      fprintf(stderr, "Couldn't create QP\n");
      return NULL;
    }
  }

  {
    struct ibv_qp_attr attr = {
        .qp_state        = IBV_QPS_INIT,
        .pkey_index      = 0,
        .port_num        = port,
        .qp_access_flags = IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE
    };

    if (ibv_modify_qp(ctx->qp, &attr,
                      IBV_QP_STATE              |
                      IBV_QP_PKEY_INDEX         |
                      IBV_QP_PORT               |
                      IBV_QP_ACCESS_FLAGS)) {
      fprintf(stderr, "Failed to modify QP to INIT\n");
      return NULL;
    }
  }

  return ctx;
}

int pp_close_ctx(struct pingpong_context *ctx)
{
  if (ibv_destroy_qp(ctx->qp)) {
    fprintf(stderr, "Couldn't destroy QP\n");
    return 1;
  }

  if (ibv_destroy_cq(ctx->cq)) {
    fprintf(stderr, "Couldn't destroy CQ\n");
    return 1;
  }

  if (ibv_dereg_mr(ctx->mr)) {
    fprintf(stderr, "Couldn't deregister MR\n");
    return 1;
  }

  if (ibv_dealloc_pd(ctx->pd)) {
    fprintf(stderr, "Couldn't deallocate PD\n");
    return 1;
  }

  if (ctx->channel) {
    if (ibv_destroy_comp_channel(ctx->channel)) {
      fprintf(stderr, "Couldn't destroy completion channel\n");
      return 1;
    }
  }

  if (ibv_close_device(ctx->context)) {
    fprintf(stderr, "Couldn't release context\n");
    return 1;
  }

  free(ctx->buf);
  free(ctx);

  return 0;
}

static int pp_post_recv(struct pingpong_context *ctx, int n)
{
  struct ibv_sge list = {
      .addr	= (uintptr_t) ctx->buf,
      .length = ctx->size,
      .lkey	= ctx->mr->lkey
  };
  struct ibv_recv_wr wr = {
      .wr_id	    = PINGPONG_RECV_WRID,
      .sg_list    = &list,
      .num_sge    = 1,
      .next       = NULL
  };
  struct ibv_recv_wr *bad_wr;
  int i;

  for (i = 0; i < n; ++i)
    if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
      break;

  return i;
}

static int pp_post_send(struct pingpong_context *ctx)
{
  struct ibv_sge list = {
      .addr	= (uint64_t)ctx->buf,
      .length = ctx->size,
      .lkey	= ctx->mr->lkey
  };

  struct ibv_send_wr *bad_wr, wr = {
      .wr_id	    = PINGPONG_SEND_WRID,
      .sg_list    = &list,
      .num_sge    = 1,
      .opcode     = IBV_WR_SEND,
      .send_flags = IBV_SEND_SIGNALED,
      .next       = NULL
  };

  return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

int pp_wait_completions(struct pingpong_context *ctx, int iters)
{
  int rcnt = 0, scnt = 0;
  while (rcnt + scnt < iters) {
    struct ibv_wc wc[WC_BATCH];
    int ne, i;

    do {
      ne = ibv_poll_cq(ctx->cq, WC_BATCH, wc);
      if (ne < 0) {
        fprintf(stderr, "poll CQ failed %d\n", ne);
        return 1;
      }

    } while (ne < 1);

    for (i = 0; i < ne; ++i) {
      if (wc[i].status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                ibv_wc_status_str(wc[i].status),
                wc[i].status, (int) wc[i].wr_id);
        return 1;
      }

      switch ((int) wc[i].wr_id) {
        case PINGPONG_SEND_WRID:
          ++scnt;
          break;

        case PINGPONG_RECV_WRID:
          if (--ctx->routs <= 10) {
            ctx->routs += pp_post_recv(ctx, ctx->rx_depth - ctx->routs);
            if (ctx->routs < ctx->rx_depth) {
              fprintf(stderr,
                      "Couldn't post receive (%d)\n",
                      ctx->routs);
              return 1;
            }
          }
          ++rcnt;
          break;

        default:
          fprintf(stderr, "Completion for unknown wr_id %d\n",
                  (int) wc[i].wr_id);
          return 1;
      }
    }

  }
  return 0;
}

static void usage(const char* argv0)
{
  printf("Usage:\n");
  printf("  %s            start a server and wait for connection\n", argv0);
  printf("  %s <host>     connect to server at <host>\n", argv0);
  printf("\n");
  printf("Options:\n");
  printf("  -p, --port=<port>      listen on/connect to port <port> (default 18515)\n");
  printf("  -d, --ib-dev=<dev>     use IB device <dev> (default first device found)\n");
  printf("  -i, --ib-port=<port>   use port <port> of IB device (default 1)\n");
  printf("  -s, --size=<size>      size of message to exchange (default 4096)\n");
  printf("  -m, --mtu=<size>       path MTU (default 1024)\n");
  printf("  -r, --rx-depth=<dep>   number of receives to post at a time (default 500)\n");
  printf("  -n, --iters=<iters>    number of exchanges (default 1000)\n");
  printf("  -l, --sl=<sl>          service level value\n");
  printf("  -e, --events           sleep on CQ events (default poll)\n");
  printf("  -g, --gid-idx=<gid index> local port gid index\n");
}

//
//#define MAX_INLINE 60
//#define MEGABIT 1048576
//#define MEGA_POWER 20
//#define PORT 8540
//#define GIGABIT 1073741824
//
//int server(struct pingpong_context *ctx);
//int client(struct pingpong_context *ctx, int tx_depth);
//
//
/////OUR CODE STARTS HERE
//
//
//#define NUM_MESSAGES 8192          // Number of messages to be sent/received during measurement
//#define NUM_WARMUP_CYCLES 1024     // Number of warm-up cycles before actual measurement
//#define MICROSECONDS_IN_SECOND 1e6 // Conversion factor for microseconds to seconds
//#define BYTES_TO_MEGABITS 8388608  // 8 * 1024 * 1024 (to convert bytes to megabits)
//#define SUCCESS 0
//#define FAILURE 1
//
//// Function to calculate the throughput in Mbit/s
//double calculate_throughput(struct timeval start_time, struct timeval end_time, int data_size_bytes) {
//  // Convert seconds and microseconds to total elapsed time in seconds
//  double elapsed_time_seconds = (double)(end_time.tv_sec - start_time.tv_sec) +
//                                (double)(end_time.tv_usec - start_time.tv_usec) / MICROSECONDS_IN_SECOND; // Time in seconds
//
//  // Calculate total data transmitted in megabits (Bytes to Megabits conversion)
//  double total_data_megabits = ((double)data_size_bytes * (double)NUM_MESSAGES * 8) / BYTES_TO_MEGABITS;
//
//  // Calculate throughput in Mbit/s (Megabits per second)
//  double throughput_mbps = total_data_megabits / elapsed_time_seconds;
//
//  return throughput_mbps;
//}
//
//// Function to post a send request with the specified flags
//static int submit_send_request(struct pingpong_context* context, unsigned int send_flags) {
//  // Initialize a scatter-gather element (SGL)
//  struct ibv_sge scatter_gather_entry;
//  scatter_gather_entry.addr = (uint64_t)context->buf;   // Buffer address
//  scatter_gather_entry.length = context->size;          // Size of the buffer
//  scatter_gather_entry.lkey = context->mr->lkey;        // Local key for the memory region
//
//  // Initialize a work request (WR)
//  struct ibv_send_wr send_request;
//  struct ibv_send_wr* bad_request = NULL;
//  send_request.wr_id = PINGPONG_SEND_WRID;    // Work request ID
//  send_request.sg_list = &scatter_gather_entry; // Scatter-gather list
//  send_request.num_sge = 1;                   // Number of scatter-gather elements
//  send_request.opcode = IBV_WR_SEND;          // Operation code (send)
//  send_request.send_flags = send_flags;       // Send flags (e.g., signaling)
//  send_request.next = NULL;                   // Pointer to the next work request
//  struct ibv_send_wr* bad_request = NULL;
//
//  // Post the send request to the queue pair
//  int result = ibv_post_send(context->qp, &send_request, &bad_request);
//  if (result) {
//    fprintf(stderr, "Error: Failed to post send request\n");
//    return FAILURE;
//  }
//
//  return SUCCESS;
//}
//
//// Function to send data multiple times based on the specified depth and size
//int transmit_data(struct pingpong_context* context, int data_size_bytes, int num_iterations, int tx_depth) {
//  // Set the size of the data to be sent in the context
//  context->size = data_size_bytes;
//
//  // Determine the send flag: signaled, and optionally inline if data size allows
//  unsigned int send_flags = IBV_SEND_SIGNALED;
//  if (data_size_bytes < MAX_INLINE) {
//    send_flags |= IBV_SEND_INLINE; // Add the inline flag if applicable
//  }
//
//  for (int iteration = 0; iteration < num_iterations; iteration++) {
//    // After every 'tx_depth' sends, wait for the completions
//    if ((iteration != 0) && (iteration % tx_depth == 0)) {
//      pp_wait_completions(context, tx_depth);
//    }
//
//    // Attempt to post the send request with the current flag
//    int result = submit_send_request(context, send_flags);
//    if (result != SUCCESS) {
//      fprintf(stderr, "Error: Client couldn't post send\n");
//      return FAILURE;
//    }
//  }
//
//  // Ensure all sends have completed
//  pp_wait_completions(context, tx_depth);
//  // Wait for a single completion (likely a response from the server)
//  pp_wait_completions(context, 1);
//
//  return SUCCESS;
//}
//
//// Function to receive data multiple times based on the specified size and iterations
//int receive_data(struct pingpong_context* context, int data_size_bytes, int num_iterations) {
//  // Set the flag to be used for sending the response
//  unsigned int send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
//
//  // Wait for completions for all iterations
//  pp_wait_completions(context, num_iterations);
//
//  // Save the current data size and temporarily set it to 1 for the response
//  int original_data_size = context->size;
//  context->size = 1;
//
//  // Attempt to post the send request with the specified flag
//  int result = submit_send_request(context, send_flags);
//  if (result != SUCCESS) {
//    fprintf(stderr, "Error: Server couldn't post send\n");
//    return FAILURE;
//  }
//
//  // Restore the original data size
//  context->size = original_data_size;
//
//  // Wait for the completion of the send request
//  pp_wait_completions(context, 1);
//
//  return SUCCESS;
//}
//
//// Client function to perform throughput measurement for different data sizes
//int client(int tx_depth, struct pingpong_context* context) {
//  struct timeval start_time, end_time;
//
//  // Allocate memory to store throughput results for different data sizes
//  double* throughput_results = (double*)malloc((MEGA_POWER + 1) * sizeof(double));
//  if (!throughput_results) {
//    fprintf(stderr, "Error: Memory allocation failed\n");
//    return FAILURE;
//  }
//
//  int result_index = 0;
//
//  // Iterate over data sizes from 1 byte to MEGABIT bytes, doubling each time
//  for (int data_size_bytes = 1; data_size_bytes <= MEGABIT; data_size_bytes <<= 1) {
//    // Perform warm-up cycles to stabilize measurements
//    transmit_data(context, data_size_bytes, NUM_WARMUP_CYCLES, tx_depth);
//
//    // Measure the time taken to send the data
//    gettimeofday(&start_time, NULL);        // Record start time
//    transmit_data(context, data_size_bytes, NUM_MESSAGES, tx_depth);
//    gettimeofday(&end_time, NULL);          // Record end time
//
//    // Calculate throughput for the current data size
//    throughput_results[result_index] = calculate_throughput(start_time, end_time, data_size_bytes);
//
//    // Output the throughput for the current data size
//    printf("%d\t%f\tMbit/s\n", data_size_bytes, throughput_results[result_index]);
//    result_index++;
//  }
//
//  // Indicate that the client operation is complete
//  printf("Client: Done.\n");
//
//  // Free the allocated memory
//  free(throughput_results);
//
//  return SUCCESS;
//}
//
//// Server function to receive data for different data sizes
//int server(struct pingpong_context* context) {
//  // Iterate over data sizes from 1 byte to MEGABIT bytes, doubling each time
//  for (int data_size_bytes = 1; data_size_bytes <= MEGABIT; data_size_bytes <<= 1) {
//    // Perform warm-up cycles to stabilize measurements
//    receive_data(context, data_size_bytes, NUM_WARMUP_CYCLES);
//
//    // Receive the actual data for the given size
//    receive_data(context, data_size_bytes, NUM_MESSAGES);
//  }
//
//  // Indicate that the server operation is complete
//  printf("Server: Done.\n");
//  return SUCCESS;
//}


//#############################################################################
//                        Project 3 KV STORE
//#############################################################################


#define NUM_MESSAGES 8192          // Number of messages to be sent/received during measurement
#define NUM_WARMUP_CYCLES 1024     // Number of warm-up cycles before actual measurement
#define MICROSECONDS_IN_SECOND 1e6 // Conversion factor for microseconds to seconds
#define BYTES_TO_MEGABITS 8388608  // 8 * 1024 * 1024 (to convert bytes to megabits)
#define SUCCESS 0
#define FAILURE 1
#define MAX_NUMBER_CLIENTS 50
#define DEFAULT_SERVERNAME "mlx-stud-01"//todo change so we wont have  to use it, default should not exist and should be
// set by us at compilation

// Function to calculate the throughput in Mbit/s
double calculate_throughput(struct timeval start_time, struct timeval end_time, int data_size_bytes) {
  // Convert seconds and microseconds to total elapsed time in seconds
  double elapsed_time_seconds = (double)(end_time.tv_sec - start_time.tv_sec) +
                                (double)(end_time.tv_usec - start_time.tv_usec) / MICROSECONDS_IN_SECOND; // Time in seconds

  // Calculate total data transmitted in megabits (Bytes to Megabits conversion)
  double total_data_megabits = ((double)data_size_bytes * (double)NUM_MESSAGES * 8) / BYTES_TO_MEGABITS;

  // Calculate throughput in Mbit/s (Megabits per second)
  double throughput_mbps = total_data_megabits / elapsed_time_seconds;

  return throughput_mbps;
}

// Function to post a send request with the specified flags
static int submit_send_request(struct pingpong_context* context, unsigned int send_flags) {
  // Initialize a scatter-gather element (SGL)
  struct ibv_sge scatter_gather_entry;
  scatter_gather_entry.addr = (uint64_t)context->buf;   // Buffer address
  scatter_gather_entry.length = context->size;          // Size of the buffer
  scatter_gather_entry.lkey = context->mr->lkey;        // Local key for the memory region

  // Initialize a work request (WR)
  struct ibv_send_wr send_request;
  send_request.wr_id = PINGPONG_SEND_WRID;    // Work request ID
  send_request.sg_list = &scatter_gather_entry; // Scatter-gather list
  send_request.num_sge = 1;                   // Number of scatter-gather elements
  send_request.opcode = IBV_WR_SEND;          // Operation code (send)
  send_request.send_flags = send_flags;       // Send flags (e.g., signaling)
  send_request.next = NULL;                   // Pointer to the next work request
  struct ibv_send_wr* bad_request = NULL;

  // Post the send request to the queue pair
  int result = ibv_post_send(context->qp, &send_request, &bad_request);
  if (result) {
    fprintf(stderr, "Error: Failed to post send request\n");
    return FAILURE;
  }

  return SUCCESS;
}

// Function to send data multiple times based on the specified depth and size
int transmit_data(struct pingpong_context* context, int data_size_bytes, int num_iterations, int tx_depth) {
  // Set the size of the data to be sent in the context
  context->size = data_size_bytes;

  // Determine the send flag: signaled, and optionally inline if data size allows
  unsigned int send_flags = IBV_SEND_SIGNALED;
  if (data_size_bytes < MAX_INLINE) {
    send_flags |= IBV_SEND_INLINE; // Add the inline flag if applicable
  }

  for (int iteration = 0; iteration < num_iterations; iteration++) {
    // After every 'tx_depth' sends, wait for the completions
    if ((iteration != 0) && (iteration % tx_depth == 0)) {
      pp_wait_completions(context, tx_depth);
    }

    // Attempt to post the send request with the current flag
    int result = submit_send_request(context, send_flags);
    if (result != SUCCESS) {
      fprintf(stderr, "Error: Client couldn't post send\n");
      return FAILURE;
    }
  }

  // Ensure all sends have completed
  pp_wait_completions(context, tx_depth);
  // Wait for a single completion (likely a response from the server)
  pp_wait_completions(context, 1);

  return SUCCESS;
}

// Function to receive data multiple times based on the specified size and iterations
int receive_data(struct pingpong_context* context, int data_size_bytes, int num_iterations) {
  // Set the flag to be used for sending the response
  unsigned int send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;

  // Wait for completions for all iterations
  pp_wait_completions(context, num_iterations);

  // Save the current data size and temporarily set it to 1 for the response
  int original_data_size = context->size;
  context->size = 1;

  // Attempt to post the send request with the specified flag
  int result = submit_send_request(context, send_flags);
  if (result != SUCCESS) {
    fprintf(stderr, "Error: Server couldn't post send\n");
    return FAILURE;
  }

  // Restore the original data size
  context->size = original_data_size;

  // Wait for the completion of the send request
  pp_wait_completions(context, 1);

  return SUCCESS;
}

// Client function to perform throughput measurement for different data sizes
int client(int tx_depth, struct pingpong_context* context) {
  struct timeval start_time, end_time;

  // Allocate memory to store throughput results for different data sizes
  double* throughput_results = (double*)malloc((MEGA_POWER + 1) * sizeof(double));
  if (!throughput_results) {
    fprintf(stderr, "Error: Memory allocation failed\n");
    return FAILURE;
  }

  int result_index = 0;

  // Iterate over data sizes from 1 byte to MEGABIT bytes, doubling each time
  for (int data_size_bytes = 1; data_size_bytes <= MEGABIT; data_size_bytes <<= 1) {
    // Perform warm-up cycles to stabilize measurements
    transmit_data(context, data_size_bytes, NUM_WARMUP_CYCLES, tx_depth);

    // Measure the time taken to send the data
    gettimeofday(&start_time, NULL);        // Record start time
    transmit_data(context, data_size_bytes, NUM_MESSAGES, tx_depth);
    gettimeofday(&end_time, NULL);          // Record end time

    // Calculate throughput for the current data size
    throughput_results[result_index] = calculate_throughput(start_time, end_time, data_size_bytes);

    // Output the throughput for the current data size
    printf("%d\t%f\tMbit/s\n", data_size_bytes, throughput_results[result_index]);
    result_index++;
  }

  // Indicate that the client operation is complete
  printf("Client: Done.\n");

  // Free the allocated memory
  free(throughput_results);

  return SUCCESS;
}

// Server function to receive data for different data sizes
int server(struct pingpong_context* context) {
  // Iterate over data sizes from 1 byte to MEGABIT bytes, doubling each time
  for (int data_size_bytes = 1; data_size_bytes <= MEGABIT; data_size_bytes <<= 1) {
    // Perform warm-up cycles to stabilize measurements
    receive_data(context, data_size_bytes, NUM_WARMUP_CYCLES);

    // Receive the actual data for the given size
    receive_data(context, data_size_bytes, NUM_MESSAGES);
  }

  // Indicate that the server operation is complete
  printf("Server: Done.\n");
  return SUCCESS;
}



//###############################################################
//                              API
//#############################################################################
int kv_open(char *servername, void **kv_handle); /*Connect to server*/
int kv_set(void *kv_handle, const char *key, const char *value);
int kv_get(void *kv_handle, const char *key, char **value);
void kv_release(char *value);/* Called after get() on value pointer */
int kv_close(void *kv_handle); /* Destroys the QP */


//#############################################################################
//
//#############################################################################

//int server(struct pingpong_context *ctx);
//int client(struct pingpong_context *ctx, int tx_depth);

int kv_open(char *servername, void **kv_handle){/*Connect to server*/
  //todo change default arguments
  //todo ersae later, after everything works, everything that is not needed
  // (most!)
    struct ibv_device      **dev_list;
    struct ibv_device       *ib_dev;
    struct pingpong_context *ctx;
    struct pingpong_dest     my_dest;
    struct pingpong_dest    *rem_dest;
    char                    *ib_devname = NULL;
//    char                    *thisservername;
    int                      port = PORT;
    int                      ib_port = 1;
    enum ibv_mtu             mtu = IBV_MTU_2048;
    int                      rx_depth = 100;
    int                      tx_depth = 100;
//    int                      iters = 256;
    int                      use_event = 0;
    int                      size = MEGABIT;
    int                      sl = 0;
    int                      gidx = -1;
    char                     gid[33];

    srand48(getpid() * time(NULL));


//    //TODO check if we need to do strcpy. if we do change pp_close_ctx to a
//    // new function that alos free thisservername
//    thisservername = servername;


    page_size = sysconf(_SC_PAGESIZE);

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
      perror("Failed to get IB devices list");
      return 1;
    }

    if (!ib_devname) {
      ib_dev = *dev_list;
      if (!ib_dev) {
        fprintf(stderr, "No IB devices found\n");
        return 1;
      }
    } else {
      int i;
      for (i = 0; dev_list[i]; ++i)
        if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
          break;
      ib_dev = dev_list[i];
      if (!ib_dev) {
        fprintf(stderr, "IB device %s not found\n", ib_devname);
        return 1;
      }
    }

    ctx = pp_init_ctx(ib_dev, size, rx_depth, tx_depth, ib_port, use_event, !servername);
    if (!ctx)
      return 1;

    ctx->routs = pp_post_recv(ctx, ctx->rx_depth);
    if (ctx->routs < ctx->rx_depth) {
      fprintf(stderr, "Couldn't post receive (%d)\n", ctx->routs);
      return 1;
    }

//    if (use_event)
//      if (ibv_req_notify_cq(ctx->cq, 0)) {
//      e  fprintf(stderr, "Couldn't request CQ notification\n");
//        return 1;
//      }


    if (pp_get_port_info(ctx->context, ib_port, &ctx->portinfo)) {
      fprintf(stderr, "Couldn't get port info\n");
      return 1;
    }

    my_dest.lid = ctx->portinfo.lid;
    if (ctx->portinfo.link_layer == IBV_LINK_LAYER_INFINIBAND && !my_dest.lid) {
      fprintf(stderr, "Couldn't get local LID\n");
      return 1;
    }

//    if (gidx >= 0) {
//      if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
//        fprintf(stderr, "Could not get local gid for gid index %d\n", gidx);
//        return 1;
//      }
//    } else
    memset(&my_dest.gid, 0, sizeof my_dest.gid);

    my_dest.qpn = ctx->qp->qp_num;
    my_dest.psn = lrand48() & 0xffffff;
    inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof gid);
    printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
           my_dest.lid, my_dest.qpn, my_dest.psn, gid);

    if (servername)
      rem_dest = pp_client_exch_dest(servername, port, &my_dest);
    else
      rem_dest = pp_server_exch_dest(ctx, ib_port, mtu, port, sl, &my_dest, gidx);

    if (!rem_dest)
      return 1;

    inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
    printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
           rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

    if (servername)
      if (pp_connect_ctx(ctx, ib_port, my_dest.psn, mtu, sl, rem_dest, gidx))
        return 1;

    *kv_handle = ctx;
    ibv_free_device_list(dev_list);
    free(rem_dest);
    return 0;
  }

int kv_close(void *kv_handle){
    pp_close_ctx(kv_handle);
  }/* Destroys the QP */

void kv_release(char *value){
    free(value);
  }
//  struct{
//    char[4096] keyvalue;
//    int isSet;
//}kv_pair;

/* Called after get() on value pointer */
//int kv_set(void *kv_handle, const char *key, const char *value){
//  if(strlen(key)+strlen(value)<KB4){
//    //part 1, eager protocol
//    struct pingpong_context* ctx = (*struct pingpong_context*)kv_handle;
//     char* buf = (char*)ctx->buf;
//     long int available_spot = ctx->available;
//     //  we differentiate the key and the value using the null character '/0'
//     strcpy(buf+available_spot, key);
//     strcpy(buf+available_spot+strlen(key)+1, value);
//     ctx->size = strlen(key)+strlen(value)+2; //len+ null characters
//     //todo: almost definetly not this number, but needs to be send in a
//     // structure of some sort
//     if(pp_post_send (ctx)){
//      //didn't post send
//       return 1;
//     }
//    if (pp_wait_completions(ctx, 1)) {
//      //didnt got back ack
//      return 1;
//    }
//     ctx->available +=strlen(key)+strlen(value)+2;//+2 for the null character
//
//
//  }
int kv_set(void *kv_handle, const char *key, const char *value){
  if(strlen(key)+strlen(value)<KB4)
  {
    //part 1, eager protocol
    struct pingpong_context *ctx = (struct pingpong_context*)kv_handle;
    char *buf = (char *) ctx->buf;
    //  we differentiate the key and the value using the null character '/0'
    strcpy (buf, key);
    strcpy (buf + strlen (key) + 1, value);
    ctx->size = strlen (key) + strlen (value) + 2; //len+ null characters
    ctx->set = 1;
    //todo: almost definetly not this number, but needs to be send in a
    // structure of some sort
    if (pp_post_send (ctx))
    {
      //didn't post send
      return 1;
    }
    if (pp_wait_completions (ctx, 1))
    {
      //didnt got back ack
      return 1;
    }
  }

}
/**
 * send get request to server
 * @param kv_handle the handle for the kv(ctx)
 * @param key the key
 * @param value the value to be loaded to
 * @return 0 upon success, 1 upon failure
 */
int kv_get(void *kv_handle, const char *key, char **value){
    //part 1, eager protocol
    struct pingpong_context *ctx = (struct pingpong_context *) kv_handle;
    char *buf = (char *) ctx->buf;
    //  we differentiate the key and the value using the null character '/0'
    strcpy (buf, key);
    ctx->set = 0;
    ctx->size = strlen (key) +1; //len+ null characters
    //todo: almost definetly not this number, but needs to be send in a
    // structure of some sort
    if (pp_post_send (ctx))
    {
      //didn't post send
      return 1;
    }
    ctx->size = KB4 + 2; //len+ null characters
    if(pp_post_recv (ctx, 1)){
      return 1;// post recv of the context
    }
    if(pp_wait_completions (ctx,1)){
      return 1;// wait to get the number
    }
  strcpy (*value, ctx->buf);// copy to value the data recieved
  return 0;
}



//#############################################################################
//                            SERVER
//#############################################################################

#include <math.h>

/**
 * create the space
 * @param client_space client space
 * @return 0 if good false if failed
 */
int create_space(char **client_space){
  *client_space = malloc((4* KB4)+(sizeof(int)*2));
  if(*client_space == NULL){
    return 1;
  }
  *(int*)*client_space = 0;//represent the curr head
  (int*)*client_space++;//move to next
  *(int*)*client_space = 4* KB4;// represent size
  (int*)*client_space++;
  return 0;
}

/**
 * allocate the space and allocate it, and change the size accordingly
 * @param length length of the new variable to add
 * @param client_space  the client space
 * @return 1 on failure 0 oterwise
 */
int allocate_space(char** client_space){
  if(*client_space == NULL){
    create_space(client_space);
  }
  int size = ((int*)*client_space)[-1];
  int cur_head = ((int*)*client_space)[-2];
  if(size<cur_head+KB4+2)
  {
    //no more space need to realloc!
    void *tmp = realloc (*client_space, size*2);
    if (tmp == NULL)
    {
      //if no more space can be allocated program failed!
      return 1;
    }
    *client_space = tmp;
    *(((int*)*client_space)-1) = size*2;
  }

}
/**
 * find the key from the client space
 * @param key the key to find
 * @param client_space a space associated to the clients, an array of key
 * values
 * @return key place if found, -1 if not.
 */
int find_key(char* key, void* client_space){
  int real_size = *((int*)client_space-2);
  for (int i =0; i<real_size;i+=(KB4+2)){
    char* cur_key = strtok(((char*)client_space+i), '\0');
    if(strcmp (cur_key, key)==0){
      return i; //return the place the key is in
    }
  }
  return -1; //we didn't find the key
}


/**
 *
 * @param ctx ctx struct as we implemanted on ex2
 * @param start the starting point of the server
 * @return 1 if failed otherwise 0
 */
int set_server(struct pingpong_context *ctx,char** client_space){
  // stage 1: get the key and value
  // they are stored on the ctx->buf, last
  char* key = strtok((char*)ctx->buf, '\0');
  char * value = strtok(NULL, '\0');
  int key_place = find_key(key, *client_space);
  if(key_place != -1){
    //means we find the key
    strcpy((client_space[key_place+strlen(key)+1]), value);
    //copy the value(overwrite) on key
    return 0;
  }
  //stage 2: put in memory
  if(allocate_space(client_space)){
    //allocation failed, go back
    return 1;
  }
  int cur_head = *(((int*)*client_space)-2); // the cur head we are going to
  // insert to
  strcpy(*client_space+cur_head,key);//copy head
  strcpy(*client_space+cur_head+strlen(key)+1, value);//copy value
  *(((int*)*client_space)-2) += KB4 +2; // increment the cur head for later
  // send ack
  //todo check if we want to post send on that size?
  ctx->size = 1;
  if (pp_post_send(ctx)){
    //failed to send ctx
    return 1;
  }
  return 0;//program ended succesfully

}

/**
 * get the value requested by the key
 * @param ctx the ctx
 * @param client_space the space of the client
 * @return the value associated with the key. default is ""
 */
int get_server(struct pingpong_context *ctx,char** client_space){
  //stage 1: get the key
  char* key = strtok(ctx->buf, '\0');
  //stage 2: find the key
  int key_place = find_key(key,*client_space);
  if (key_place == -1){
    //key is not found, return default value of ""
    strcpy(ctx->buf, "");//todo check if this is the best way to transfer ""
  }
  else{
    //stage 3: put key on buf
    strcpy(ctx->buf, (*client_space)+key_place);
  }
  ctx->size = KB4;
  //stage 4: return the key
  if(pp_post_send (ctx)){
    return 1;
  }
  if (pp_wait_completions(ctx, 1)) {
    return 1; // wait to send data
  }

}

int init_client_space(char** client_space){
    *client_space = calloc(1, sizeof(void*));
    if(*client_space == NULL){
      return 1;//couldn't allocate the space
    }
  return 0;

}
void free_client_spaces(char* clients_spaces[], int num_clients){
  for(int i=0;i<num_clients;i++){
    free(clients_spaces[i]);
  }
  free (clients_spaces);//todo check is it needed?
}

void close_ctxs(struct pingpong_context* ctxs[], int num_clients){
  for(int i=0;i<num_clients;i++){
    kv_close (ctxs[i]);
  }
  free (ctxs);//todo check is it needed?
}

#include <infiniband/verbs.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/**
 * Find the correct GID index for a given context and port.
 * @param context: The InfiniBand device context
 * @param ib_port: The InfiniBand port number
 * @param gid: A pointer to store the found GID
 * @return The GID index if found, or -1 on failure
 */
int find_sgid_index(struct ibv_context *context, int ib_port, union ibv_gid *gid) {
  struct ibv_port_attr port_attr;

  // Query the port to get the number of available GIDs
  if (ibv_query_port(context, ib_port, &port_attr)) {
    fprintf(stderr, "Error: Failed to query port info for port %d\n", ib_port);
    return -1;
  }

  // Iterate over all available GID indices
  for (int gid_idx = 0; gid_idx < port_attr.gid_tbl_len; gid_idx++) {
    // todo check if this type of query isnt too much time consuming. can we
    //  do it any other way?
    if (ibv_query_gid(context, ib_port, gid_idx, gid)) {
      fprintf(stderr, "Error: Failed to query GID at index %d for port %d\n", gid_idx, ib_port);
      continue;
    }

    // If GID has a valid interface ID, it's a valid SGID
    if (gid->global.interface_id != 0) {
      printf("Valid GID found at index %d: %016llx:%016llx\n",
             gid_idx,
             (unsigned long long) gid->global.subnet_prefix,
             (unsigned long long) gid->global.interface_id);
      return gid_idx;
    }
  }

  // No valid GID index found
  fprintf(stderr, "Error: No valid GID found for port %d\n", ib_port);
  return -1;
}

/**
 * Add a single client to the server
 * @param clients_spaces: Array to hold client spaces
 * @param ctxs: Array to hold pingpong contexts for each client
 * @param num_clients: The current number of clients
 * @param port: The port number used for communication
 * @param mtu: The Maximum Transmission Unit size
 * @param sl: Service Level value
 * @param dest: Destination structure holding client connection information
 * @return 0 if succeeded, 1 if failed
 */
int add_client(char* clients_spaces[], struct pingpong_context* ctxs[], int num_clients,
               int port, enum ibv_mtu mtu, int sl, struct pingpong_dest *dest) {

  // Initialize client space
  if (init_client_space(&(clients_spaces[num_clients]))) {
    return 1;
  }

  // Open the key-value store for the client
  if (kv_open(NULL, (void**)(&(ctxs[num_clients])))) {
    free(clients_spaces[num_clients]);
    return 1;
  }

  // Generate a random PSN (Packet Sequence Number)
  int my_psn = lrand48() & 0xffffff;

  // Find the correct SGID index and GID
  union ibv_gid sgid;
  int sgid_idx = find_sgid_index(ctxs[num_clients]->context, port, &sgid);
  if (sgid_idx < 0) {
    fprintf(stderr, "Error: Failed to find a valid SGID index\n");
    free(clients_spaces[num_clients]);
    kv_close((void**)(&(ctxs[num_clients])));
    return 1;
  }

  // Connect the client's QP (Queue Pair) with the server using the found SGID index
  if (pp_connect_ctx(ctxs[num_clients], port, my_psn, mtu, sl, dest, sgid_idx)) {
    free(clients_spaces[num_clients]);
    kv_close((void**)(&(ctxs[num_clients])));
    return 1;
  }

  return 0;
}

//
///**
// * add a single client to the server
// * @param clients_spaces the space to add
// * @param ctxs the contexts list to add
// * @param num_clients the number of clients
// * @return 0 if suceeded 1 if failed
// */
//int add_client(char* clients_spaces[], struct pingpong_context* ctxs[], int
//    num_clients,int port,
//               enum ibv_mtu mtu, int sl,
//               struct pingpong_dest *dest){
//
//  if(init_client_space(&(clients_spaces[num_clients]))){
//    return 1;
//  }
//  if(kv_open(NULL,(void**)(&(ctxs[num_clients]))))
//  {
//    free(clients_spaces[num_clients]);
//    return 1;
//  }
//  //todo for tommorrow: get: my_psn, sgid.
//  int my_psn = lrand48() & 0xffffff;
//  int sgid_idx;
//  if (ibv_query_gid(ctxs[num_clients]->context, port, gid_index,&sgid){
//    free(clients_spaces[num_clients]);
//    kv_close((void**)(&(ctxs[num_clients])));
//    return 1;
//  }
//  if(pp_connect_ctx (ctxs[num_clients], port, my_psn,mtu,sl,dest,sgid_idx)){
//    /**
//     * struct pingpong_context *ctx, int port, int my_psn,
//                          enum ibv_mtu mtu, int sl,
//                          struct pingpong_dest *dest, int sgid_idx)
//{
//     */
//    free(clients_spaces[num_clients]);
//    kv_close((void**)(&(ctxs[num_clients])));
//    return 1;
//  }
//  return 0;
//}
/**
 * free clients from the server
 * @param clients_spaces
 * @param ctxs
 * @param num_clients
 */
void free_clients_from_server(char* clients_spaces[], struct pingpong_context*
    ctxs[], int num_clients){
  free_client_spaces (clients_spaces,num_clients);
  close_ctxs (ctxs,num_clients);
}
/**
 * run server
 * @return o if successes, 1 if fails
 */
int run_server(){
  //stage 1: init server with clients initialised, each with its own space
  // set to NULL.
  char* clients_spaces[MAX_NUMBER_CLIENTS];
  struct pingpong_context* ctxs[MAX_NUMBER_CLIENTS];
  void*                   serverctx;
  struct pingpong_dest    *rem_dest[MAX_NUMBER_CLIENTS];
  struct pingpong_dest    *my_dest;
  char                    *ib_devname = NULL;
  int                      port = PORT;//todo can be changed to be run
  // dependant

  int                      ib_port = 1;
  enum ibv_mtu             mtu = IBV_MTU_4096;
  int                      sl = 0;
  int                      gidx = -1;//
  // stage 2: create the link to the communication and validate it
  printf("going into kv_open\n");
  kv_open(NULL,&serverctx);
  printf("finished kv_open\n");


//  for(int i=0;i<num_clients;i++){
//    kv_open(clients_names[i], (void**)&(ctxs[i]));
//  }
  //stage 3(repeat): check on all clients and handle requests accordingly, and add clients if needed
  int num_clients = 0;
  while(1){
    printf ("inside loop");
      //stage 2: create the link to the communication and validate it
      rem_dest[num_clients] = pp_server_exch_dest(serverctx, ib_port, mtu, port, sl, my_dest, gidx);
      if(rem_dest[num_clients]!=NULL)
      {
        printf ("tried to add client");

        if(add_client (clients_spaces,ctxs,num_clients,port,mtu,
                       sl,rem_dest[num_clients])){
          free_clients_from_server(clients_spaces,ctxs,num_clients);
          return 1;
        }
        printf ("added a new client with  psn: %d and gid %d",
                rem_dest[num_clients]->psn,rem_dest[num_clients]->gid);
        num_clients++;
      }
      for(int i=0;i<num_clients;i++){
        printf("get/set rewquests");
      if(((struct pingpong_context*) ctxs)[i].set){
        //set request
        set_server ((ctxs)[i],
            &(clients_spaces[i]));
      }
      else{
        //get request
        get_server  ((ctxs)[i],
                     &(clients_spaces[i]));
      }
    }
  }
}

int test_client(char* servername){
    //kv open
    void* kv_handle;
    kv_open(servername,&kv_handle);
    //kv set * X_times
    char* keys[] = {"k","g","eitan","eita", "elyashiv"};
    char *values[] = {"yes","not","black","white","secret"};
    int len = sizeof(keys) / sizeof(keys[0]);
    for(int i=0;i<len; i++)
        kv_set(kv_handle, keys[i], values[i]);
    //kv_get -> and print!
    char* string;
    kv_get(kv_handle,"k",&string);
    printf("%s",string);
    //kvclose
    kv_close(kv_handle);
}



int main(int argc, char *argv[]){
//// stage 1: initate server with no clients
//// stage 2: upon request from client add server sapce and client.
////stage 3: continue cycle -> search for get/set from existing clients and add clint sif neededed.
///server
///client servername
    printf ("argc is: %d\n",argc);
    if(argc > 1){
        //client
        printf("client added!_ debug\n");
        test_client(DEFAULT_SERVERNAME);
    }
    else{
        //server
        printf ("server debug\n");
        run_server();
    }
}