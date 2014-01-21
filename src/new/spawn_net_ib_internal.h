/* Copyright (c) 2001-2013, The Ohio State University. All rights
 * reserved.
 *
 * This file is part of the MVAPICH2 software package developed by the
 * team members of The Ohio State University's Network-Based Computing
 * Laboratory (NBCL), headed by Professor Dhabaleswar K. (DK) Panda.
 *
 * For detailed copyright and licensing information, please refer to the
 * copyright file COPYRIGHT in the top level MVAPICH2 directory.
 *
 */
#ifndef _SPAWN_NET_IB_INTERNAL_H
#define _SPAWN_NET_IB_INTERNAL_H

#include <netdb.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <spawn_net_ib_clock.h>
#include <spawn_net.h>
#include <spawn_util.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <spawn_net_ib_debug_utils.h>
#include <infiniband/verbs.h>
//#include <infiniband/umad.h>

#ifdef __ia64__
/* Only ia64 requires this */
#define SHMAT_ADDR (void *)(0x8000000000000000UL)
#define SHMAT_FLAGS (SHM_RND)
#else
#define SHMAT_ADDR (void *)(0x0UL)
#define SHMAT_FLAGS (0)
#endif /* __ia64__*/
#define HUGEPAGE_ALIGN  (2*1024*1024)

#define MAX_NUM_PORTS                   (1)
#define MAX_NUM_HCAS                    (1)
#define RDMA_DEFAULT_PSN                (0)
#define RDMA_DEFAULT_PORT               (1)
#define RDMA_DEFAULT_UD_MTU             (2048)
#define RDMA_VBUF_SECONDARY_POOL_SIZE   (512)
#define RDMA_DEFAULT_SERVICE_LEVEL      (0)
#define RDMA_DEFAULT_MAX_CQ_SIZE        (40000)
#define RDMA_DEFAULT_MAX_SG_LIST        (1)
#define RDMA_DEFAULT_MAX_UD_SEND_WQE    (2048)
#define RDMA_DEFAULT_MAX_UD_RECV_WQE    (4096)
#define RDMA_DEFAULT_MAX_INLINE_SIZE    (128)
#define DEFAULT_CM_THREAD_STACKSIZE     (1024*1024)

#define GEN_EXIT_ERR     -1     /* general error which forces us to abort */
#define GEN_ASSERT_ERR   -2     /* general assert error */
#define IBV_RETURN_ERR   -3     /* gen2 function return error */
#define IBV_STATUS_ERR   -4     /* gen2 function status error */

#define ibv_va_error_abort(code, message, args...)  {           \
    if (errno) {                                                \
        PRINT_ERROR_ERRNO( "%s:%d: " message, errno, __FILE__, __LINE__, ##args);     \
    } else {                                                    \
        PRINT_ERROR( "%s:%d: " message "\n", __FILE__, __LINE__, ##args);     \
    }                                                           \
    fflush (stderr);                                            \
    exit(code);                                                 \
}

#define ibv_error_abort(code, message)                          \
{                                                               \
    if (errno) {                                                \
        PRINT_ERROR_ERRNO( "%s:%d: " message, errno, __FILE__, __LINE__);     \
    } else {                                                    \
        PRINT_ERROR( "%s:%d: " message "\n", __FILE__, __LINE__);     \
    }                                                           \
    fflush (stderr);                                            \
    exit(code);                                                 \
}

#define LOG2(_v, _r)                            \
do {                                            \
    (_r) = ((_v) & 0xFF00) ? 8 : 0;             \
    if ( (_v) & ( 0x0F << (_r + 4 ))) (_r)+=4;  \
    if ( (_v) & ( 0x03 << (_r + 2 ))) (_r)+=2;  \
    if ( (_v) & ( 0x01 << (_r + 1 ))) (_r)+=1;  \
} while(0)

/* packet types: must fit within uint8_t, set highest order bit to
 * denote control packets */
#define MPIDI_CH3_PKT_CONTROL_BIT   (0x80)

#define MPIDI_CH3_PKT_UD_CONNECT    (0x80)
#define MPIDI_CH3_PKT_UD_ACCEPT     (0x81)
#define MPIDI_CH3_PKT_UD_DISCONNECT (0x82)
#define MPIDI_CH3_PKT_UD_ACK        (0x83)
#define MPIDI_CH3_PKT_UD_DATA       (0x04)

/* hca_info */
typedef struct _mv2_hca_info_t {
    struct ibv_pd *pd;
    struct ibv_device *device;
    struct ibv_context *context;
    struct ibv_cq  *cq_hndl;
    struct ibv_comp_channel     *comp_channel;
    union  ibv_gid gid[MAX_NUM_PORTS];
    struct ibv_port_attr port_attr[MAX_NUM_PORTS];
    struct ibv_device_attr device_attr;
} mv2_hca_info_t;

extern int rdma_num_rails;
extern int rdma_num_hcas;
extern int rdma_vbuf_max;
extern int rdma_enable_hugepage;
extern int rdma_vbuf_total_size;
extern int rdma_vbuf_secondary_pool_size;
extern int rdma_max_inline_size;
extern uint16_t rdma_default_ud_mtu;

/* Big Picture:
 *
 * A "UD context" data structure tracks info on access to the UD QP
 * opened on an HCA.  This includes fields like a pointer to the QP,
 * the MTU size, the number of available send work queue elements,
 * the number of outstanding receives, and a pointer to a message
 * queue of packets to be sent as soon as send WQEs become available.
 *
 * The UD context manages the list of packets to be submitted to the
 * QP.  There is a maximum number of sends that can be outstanding
 * on the QP at a given time, which is set by the number of send
 * work elements.  If the number of packets ready to be sent exceeds
 * this limit, they are queued in the UD context extended send queue.
 * Whenever a send completes, new packets are sent from the extended
 * send queue.
 *
 * There is also a global "unack'd queue", which tracks messages that
 * have been sent on the UP QP but not yet acknowledged by their
 * destination process.
 *
 * For each remote endpoint that a process "connects" to, we track
 * details in a "virtual connection".  Each packet sent on a
 * virtual connection is assigned a sequence number.  Sequence numbers
 * are 16-bit integers that increment with each packet sent and wrap
 * around.  A sliding window of sequence numbers are valid at any
 * given instant, and processes use ACKS to manage the sliding window.
 * Each VC manages several queues (called windows): send, extended send,
 * in-order received, and out-of-order receieved.
 *
 * - send window - tracks packets handed off to the UD context.
 *
 * - extended send window - tracks packets ready to be sent on the VC,
 *   but not yet handed off to the UD context.
 *
 * - in-order receive window - tracks a list of received packets
 *   ordered by sequence number with no missing packets
 *
 * - out-of-order receive window - tracks a list of received packets
 *   that includes one or more missing packets
 *
 * When sending a packet, it is added to the send window if there is
 * room.  Otherwise, it is added to the extended send window.  When a
 * packet is added to the send window, it is submitted to the UD
 * context.  In this way, the send window enforces a limit on the
 * number of packets a VC can have outstanding on the UD context.
 *
 * When the UD context actually sends a message, the packet is added
 * to the "unack'd queue" (unless it does not have a valid sequence
 * number).  This queue records packets yet to be acknowledged from the
 * destination.  Each entry has a timestamp to record when the packet
 * was last sent.  A thread periodically wakes up to scan the unack'd
 * list and resends any packets that have exceeded their timeout.
 *
 * When a process sends a message to another process, it also records
 * the sequence number for the latest packet it has received from the
 * destination.  Upon receipt of the message, the destination will
 * clear any packets from its send window and unack'd queue up to and
 * including that sequence number.  After removing packets from the
 * send window, more packets can be queued by taking them from the
 * VC extended send window.
 *
 * The in-order receive window records a list of packets ready to be
 * received by the application (apprecv_win).
 *
 * There is also an out-of-order receive window which records packets
 * that have been received but cannot be appended to the in-order queue
 * because one or more packets are missing.  With each received packet,
 * the out-of-order receive queue is checked and packets are moved to
 * the in-order receive queue if possible.
 *
 * In case there are no data packets flowing back from receiver to
 * sender to carry implicit acks, explicit ack messages are sent in
 * different circumstances.
 *
 * The thread that periodically wakes to check whether packets need to
 * be resent will also send "explicit acks" if necessary.  An explicit
 * ack is sent whenever an ack needs to be sent but a piggy-backed ack
 * has not been sent for a certain amount of time.
 *
 * An explicit ACK is also sent if the number of received packets exceeds
 * a threshold since the last ACK was sent. */

/*
** We should check if the ackno had been handled before.
** We process this only if ackno had advanced.
** There are 2 cases to consider:
** 1. ackno_handled < seqnolast (normal case)
** 2. ackno_handled > seqnolast (wraparound case)
*/

/* check whether val is within [start, end] */
#define INCL_BETWEEN(_val, _start, _end)                            \
    (((_start > _end) && (_val >= _start || _val <= _end)) ||       \
     ((_end > _start) && (_val >= _start && _val <= _end)) ||       \
     ((_end == _start) && (_end == _val)))

/* check whether val is within (start, end) */
#define EXCL_BETWEEN(_val, _start, _end)                            \
    (((_start > _end) && (_val > _start || _val < _end)) ||         \
     ((_end > _start) && (_val > _start && _val < _end)))

#define MAX_SEQ_NUM (UINT16_MAX)

typedef struct MPIDI_CH3I_MRAILI_Pkt_comm_header_t {
    uint8_t  type;   /* packet type (see ib_internal.h) */
    uint64_t srcid;  /* source context id to identify sender */
    uint16_t seqnum; /* sequence number from source */
    uint16_t acknum; /* most recent seq number source has received from us */
    uint8_t  rail;   /* rail id to send packet on */
} MPIDI_CH3I_MRAILI_Pkt_comm_header;

/* VC state values */
#define MRAILI_INIT             (0x0040)
#define MRAILI_UD_CONNECTING    (0x0001)
#define MRAILI_UD_CONNECTED     (0x0002)

/* tracks a list of vbufs */
typedef struct message_queue_t {
    struct vbuf *head;
    struct vbuf *tail;
    uint16_t count;
} message_queue_t;

/* initialize fields of a message queue */
#define MESSAGE_QUEUE_INIT(q)   \
{                               \
    (q)->head  = NULL;          \
    (q)->tail  = NULL;          \
    (q)->count = 0 ;            \
}

/* ud context - tracks access to open UD QP on HCA */
typedef struct _mv2_ud_ctx_t {
    struct ibv_qp *qp;        /* UD QP */
    int hca_num;              /* id of HCA to use, starts at 0 */
    int send_wqes_avail;      /* number of available send work queue elements for UD QP */
    int num_recvs_posted;     /* number of receive elements currently posted */
    int credit_preserve;      /* low-water mark for number of posted receives */
    message_queue_t ext_send_queue; /* UD extended send queue */
    uint64_t ext_sendq_count; /* cumulative number of messages sent from UD extended send queue */
} mv2_ud_ctx_t;

/* structure to pass to mv2_ud_create_qp to create an ibv_qp */
typedef struct ud_qp_info {
    struct ibv_cq      *send_cq;
    struct ibv_cq      *recv_cq;
    struct ibv_srq     *srq;
    struct ibv_pd      *pd;
    struct ibv_qp_cap  cap;
    uint32_t           sq_psn;
} ud_qp_info_t;

/* IB address info for ud exhange */
typedef struct _mv2_ud_exch_info_t {
    uint16_t lid; /* lid of process */
    uint32_t qpn; /* queue pair of process */
} mv2_ud_exch_info_t;

/* ud vc info - tracks connection info between process pair */
typedef struct MPIDI_VC
{
    /* remote address info and VC state */
    struct ibv_ah *ah;            /* IB address of remote process */
    uint32_t qpn;                 /* queue pair number of remote process */
    uint16_t lid;                 /* lid of remote process */
    uint16_t state;               /* state of VC */

    /* read/write context ids */
    uint64_t readid;              /* remote proc labels its packets with this id when sending to us */
    uint64_t writeid;             /* we label our outgoing packets with this id */

    /* track sequence numbers and acks */
    uint16_t seqnum_next_tosend;  /* next sequence number to use when sending */
    uint16_t seqnum_next_torecv;  /* next sequence number needed for tail of in-order app receive window */
    uint16_t seqnum_next_toack;   /* sequence number to ACK in next ACK message */
    uint16_t ack_need_tosend;     /* whether we need to send an ACK on this VC */
    uint16_t ack_pending;         /* number of messages we've received w/o sending an ack */

    /* message queues */
    message_queue_t send_window;  /* VC send window */
    message_queue_t ext_window;   /* VC extended send window */
    message_queue_t recv_window;  /* VC out-of-order receive window */
    message_queue_t app_recv_window; /* in-order receive window */

    /* profiling counters */
    uint64_t cntl_acks;          /* number of explicit ACK messages sent */
    uint64_t resend_count;       /* number of resend operations */
    uint64_t ext_win_send_count; /* number of sends from extended send wnidow */
} MPIDI_VC_t;

/* allocated as a global data structure to bind a UD context and
 * unack'd queue */
typedef struct _mv2_proc_info_t {
    mv2_ud_ctx_t*       ud_ctx;      /* pointer to UD context */
    message_queue_t     unack_queue; /* queue of sent packets yet to be ACK'd */
} mv2_proc_info_t;

#endif /* _SPAWN_NET_IB_INTERNAL_H */
