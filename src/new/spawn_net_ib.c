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

#include "spawn_internal.h"
#include "spawn_net_ib_ud.h"
#include "spawn_net_ib_internal.h"
#include "spawn_net_ib_ud.h"
#include "spawn_net_ib_ud_inline.h"
#include "spawn_net_ib_debug_utils.h"

/* need to block SIGCHLD in comm_thread */
#include <signal.h>

/* need to increase MEMLOCK limit */
#include <sys/resource.h>

/* TODO: bury all of these globals in allocated memory */
static int64_t open_count = 0;
static spawn_net_endpoint* ep = SPAWN_NET_ENDPOINT_NULL;

mv2_proc_info_t proc;
mv2_hca_info_t g_hca_info;
mv2_ud_exch_info_t local_ep_info;
MPIDI_VC_t** ud_vc_info = NULL; /* VC array */

int rdma_num_rails = 1;
int rdma_num_hcas = 1;
int rdma_vbuf_max = -1;
int rdma_enable_hugepage = 1;
int rdma_vbuf_total_size;
int rdma_vbuf_secondary_pool_size = RDMA_VBUF_SECONDARY_POOL_SIZE;
int rdma_max_inline_size = RDMA_DEFAULT_MAX_INLINE_SIZE;
uint16_t rdma_default_ud_mtu = 2048;
uint32_t rdma_default_max_ud_send_wqe = RDMA_DEFAULT_MAX_UD_SEND_WQE;
uint32_t rdma_default_max_ud_recv_wqe = RDMA_DEFAULT_MAX_UD_RECV_WQE;
uint32_t rdma_default_ud_sendwin_size = 400; /* Maximum number of outstanding buffers (waiting for ACK)*/
uint32_t rdma_default_ud_recvwin_size = 2501; /* Maximum number of out-of-order messages that will be buffered */
long rdma_ud_progress_timeout = 25000; /* Time (usec) until ACK status is checked (and ACKs are sent) */
long rdma_ud_retry_timeout = 50000; /* Time (usec) until a message is resent */
long rdma_ud_max_retry_timeout = 20000000;
long rdma_ud_last_check;
static uint16_t rdma_ud_progress_spin = 1200;
uint16_t rdma_ud_max_retry_count = 1000;
uint16_t rdma_ud_max_ack_pending;

static struct timespec remain;
static struct timespec cm_timeout;

/* Tracks an array of virtual channels.  With each new channel created,
 * the id is incremented.  Grows channel array as needed. */
static uint64_t ud_vc_info_id  = 0;    /* next id to be assigned */
static uint64_t ud_vc_infos    = 0;    /* capacity of VC array */

static pthread_mutex_t comm_lock_object;
static pthread_t comm_thread;

void* cm_timeout_handler(void *arg);

/*******************************************
 * Manage VC objects
 ******************************************/

/* initialize UD VC */
static void vc_init(MPIDI_VC_t* vc)
{
    vc->state = MRAILI_INIT;

    vc->seqnum_next_tosend = 0;
    vc->seqnum_next_torecv = 0;
    vc->seqnum_next_toack  = UINT16_MAX;
    vc->ack_need_tosend    = 0;
    vc->ack_pending        = 0;

    vc->cntl_acks          = 0; 
    vc->resend_count       = 0;
    vc->ext_win_send_count = 0;

    MESSAGE_QUEUE_INIT(&(vc->send_window));
    MESSAGE_QUEUE_INIT(&(vc->ext_window));
    MESSAGE_QUEUE_INIT(&(vc->recv_window));
    MESSAGE_QUEUE_INIT(&(vc->app_recv_window));

    return;
}

/* allocate and initialize a new VC */
static MPIDI_VC_t* vc_alloc()
{
    /* get a new id */
    uint64_t id = ud_vc_info_id;

    /* increment our counter for next time */
    ud_vc_info_id++;

    /* check whether we need to allocate more vc strucutres */
    if (id >= ud_vc_infos) {
        /* increase capacity of array */
        if (ud_vc_infos > 0) {
            ud_vc_infos *= 2;
        } else {
            ud_vc_infos = 1;
        }

        /* allocate space to hold vc pointers */
        size_t vcsize = ud_vc_infos * sizeof(MPIDI_VC_t*);
        MPIDI_VC_t** vcs = (MPIDI_VC_t**) SPAWN_MALLOC(vcsize);

        /* copy old values into new array */
        uint64_t i;
        for (i = 0; i < id; i++) {
            vcs[i] = ud_vc_info[i];
        }

        /* free old array and assign it to new copy */
        spawn_free(&ud_vc_info);
        ud_vc_info = vcs;
    }

    /* allocate vc structure */
    MPIDI_VC_t* vc = (MPIDI_VC_t*) SPAWN_MALLOC(sizeof(MPIDI_VC_t));

    /* initialize vc */
    vc_init(vc);

    /* record address of vc in array */
    ud_vc_info[id] = vc;

    /* set our read id, other end of channel will label its outgoing
     * messages with this id when sending to us (our readid is their
     * writeid) */
    vc->readid = id;

    /* return vc to caller */
    return vc;
}

static int vc_set_addr(MPIDI_VC_t* vc, mv2_ud_exch_info_t *rem_info, int port)
{
    /* don't bother to set anything if the state is already connecting
     * or connected */
    if (vc->state == MRAILI_UD_CONNECTING ||
        vc->state == MRAILI_UD_CONNECTED)
    {
        /* duplicate message - return */
        return 0;
    }

    /* clear address handle attribute structure */
    struct ibv_ah_attr ah_attr;
    memset(&ah_attr, 0, sizeof(ah_attr));

    /* initialize attribute values */
    /* TODO: set grh field? */
    ah_attr.dlid          = rem_info->lid;
    ah_attr.sl            = RDMA_DEFAULT_SERVICE_LEVEL;
    ah_attr.src_path_bits = 0; 
    /* TODO: set static_rate field? */
    ah_attr.is_global     = 0; 
    ah_attr.port_num      = port;

    /* create IB address handle and record in vc */
    vc->ah = ibv_create_ah(g_hca_info.pd, &ah_attr);
    if(vc->ah == NULL){    
        /* TODO: man page doesn't say anything about errno */
        SPAWN_ERR("Error in creating address handle (ibv_create_ah errno=%d %s)", errno, strerror(errno));
        return -1;
    }

    /* change vc state to "connecting" */
    vc->state = MRAILI_UD_CONNECTING;

    /* record remote lid and qpn in vc */
    vc->lid = rem_info->lid;
    vc->qpn = rem_info->qpn;

    return 0;
}

static void vc_free(MPIDI_VC_t** pvc)
{
    if (pvc == NULL) {
        return;
    }

    //MPIDI_VC_t* vc = *pvc;

    /* TODO: free off any memory allocated for vc */

    spawn_free(pvc);

    return;
}

/*******************************************
 * Communication routines
 ******************************************/

/* this queue tracks a list of pending connect messages,
 * the accept function pulls items from this list */
typedef struct connect_list_t {
    vbuf* v;      /* pointer to vbuf for this message */
    uint32_t lid; /* source lid */
    uint32_t qpn; /* source queue pair number */
    struct connect_list_t* next;
} connect_list;

static connect_list* connect_head = NULL;
static connect_list* connect_tail = NULL;

/* tracks list of accepted connection requests, which is used to
 * filter duplicate connection requests */
typedef struct connected_list_t {
    unsigned int lid; /* remote LID */
    unsigned int qpn; /* remote Queue Pair Number */
    unsigned int id;  /* write id we use to write to remote side */
    MPIDI_VC_t*  vc;  /* open vc to remote side */
    struct connected_list_t* next;
} connected_list;

static connected_list* connected_head = NULL;
static connected_list* connected_tail = NULL;

static int mv2_post_ud_recv_buffers(int num_bufs, mv2_ud_ctx_t *ud_ctx)
{
    /* TODO: post buffers as a linked list to be more efficient? */

//    long start = mv2_get_time_us();

#if 0
    /* post receives one at a time */

    /* post our vbufs */
    int count = 0;
    while (count < num_bufs) {
        /* get a new vbuf */
        vbuf* v = get_ud_vbuf();
        if (v == NULL) {
            break;
        }

        /* initialize vubf for UD */
        vbuf_init_ud_recv(v, rdma_default_ud_mtu, 0);
        v->transport = IB_TRANSPORT_UD;

        /* post vbuf to receive queue */
        struct ibv_recv_wr* bad_wr;
        if (ud_ctx->qp->srq) {
            int ret = ibv_post_srq_recv(ud_ctx->qp->srq, &v->desc.u.rr, &bad_wr);
            if (ret != 0) {
                MRAILI_Release_vbuf(v);
                SPAWN_ERR("Failed to post receive work requests (ibv_post_srq_recv rc=%d %s)", ret, strerror(ret));
                _exit(EXIT_FAILURE);
            }
        } else {
            int ret = ibv_post_recv(ud_ctx->qp, &v->desc.u.rr, &bad_wr);
            if (ret != 0) {
                MRAILI_Release_vbuf(v);
                SPAWN_ERR("Failed to post receive work requests (ibv_post_recv rc=%d %s)", ret, strerror(ret));
                _exit(EXIT_FAILURE);
            }
        }

        /* prepare next recv */
        count++;
    }

#else
    /* post receives in batch as linked list */

    /* we submit the work requests as a linked list */
    struct ibv_recv_wr* head = NULL;
    struct ibv_recv_wr* tail = NULL;

    /* post our vbufs */
    int count = 0;
    while (count < num_bufs) {
        /* get a new vbuf */
        vbuf* v = get_ud_vbuf();
        if (v == NULL) {
            break;
        }

        /* initialize vubf for UD */
        vbuf_init_ud_recv(v, rdma_default_ud_mtu, 0);
        v->transport = IB_TRANSPORT_UD;

        /* get pointer to receive work request */
        struct ibv_recv_wr* cur =  &v->desc.u.rr;
        cur->next = NULL;

        /* link request into chain */
        if (head == NULL) {
            head = cur;
        }
        if (tail != NULL) {
            tail->next = cur;
        }
        tail = cur;

        /* prepare next recv */
        count++;
    }

    /* post vbuf to receive queue */
    if (head != NULL) {
        struct ibv_recv_wr* bad_wr;
        if (ud_ctx->qp->srq) {
            int ret = ibv_post_srq_recv(ud_ctx->qp->srq, head, &bad_wr);
            if (ret != 0) {
                SPAWN_ERR("Failed to post receive work requests (ibv_post_srq_recv rc=%d %s)", ret, strerror(ret));
                _exit(EXIT_FAILURE);
            }
        } else {
            int ret = ibv_post_recv(ud_ctx->qp, head, &bad_wr);
            if (ret != 0) {
                SPAWN_ERR("Failed to post receive work requests (ibv_post_recv rc=%d %s)", ret, strerror(ret));
                _exit(EXIT_FAILURE);
            }
        }
    }
#endif

//    long end = mv2_get_time_us();
//    printf("Posted %d bufs in %lu usecs\n", num_bufs, (end - start));

    PRINT_DEBUG(DEBUG_UD_verbose>0 ,"Posted %d buffers of size:%d to UD QP\n",
        num_bufs, rdma_default_ud_mtu);

    return count;
}

/* iterate over all active vc's and send ACK messages if necessary */
static inline void mv2_ud_send_acks()
{
    /* walk list of connected virtual channels */
    connected_list* elem = connected_head;
    while (elem != NULL) {
        /* get pointer to vc */
        MPIDI_VC_t* vc = elem->vc;

        /* send ack if necessary */
        if (vc->ack_need_tosend) {
            mv2_send_explicit_ack(vc);
        }

        /* go to next virtual channel */
        elem = elem->next;
    }

    return;
}

static int cq_poll()
{
    /* get pointer to completion queue */
    struct ibv_cq* cq = g_hca_info.cq_hndl;

    /* poll cq */
    struct ibv_wc wcs[64];
    int ne = ibv_poll_cq(cq, 64, wcs);

    /* check that we didn't get an error polling */
    if (ne < 0) {
        SPAWN_ERR("poll cq error (ibv_poll_cq rc=%d)", ne);
        exit(-1);
    }

    /* count number of completed sends */
    int sendcnt = 0;

    /* process entries if we got any */
    int i;
    for (i = 0; i < ne; i++) {
        /* get pointer to next entry */
        struct ibv_wc* wc = &wcs[i];

        /* first, check that entry was successful */
        if (IBV_WC_SUCCESS != wc->status) {
            SPAWN_ERR("IBV_WC_SUCCESS != wc.status (%d)", wc->status);
            exit(-1);
        }

        /* get vbuf associated with this work request */
        vbuf* v = (vbuf *) ((uintptr_t) wc->wr_id);

        /* get pointer to packet header in vbuf */
        SET_PKT_LEN_HEADER(v, wcs[i]);
        SET_PKT_HEADER_OFFSET(v);
        MPIDI_CH3I_MRAILI_Pkt_comm_header* p = v->pheader;

        switch (wc->opcode) {
            case IBV_WC_SEND:
            case IBV_WC_RDMA_READ:
            case IBV_WC_RDMA_WRITE:
                /* remember that a send completed to issue more sends later */
                sendcnt++;

                /* if SEND_INPROGRESS and FREE_PENDING flags are set,
                 * release the vbuf */
                if (v->flags & UD_VBUF_SEND_INPROGRESS) {
                    v->flags &= ~(UD_VBUF_SEND_INPROGRESS);

                    if (v->flags & UD_VBUF_FREE_PENIDING) {
                        v->flags &= ~(UD_VBUF_FREE_PENIDING);

                        MRAILI_Release_vbuf(v);
                    }
                }
    
                v = NULL;
                break;
            case IBV_WC_RECV:
                /* we don't have a source id for connect messages */
                if (p->type != MPIDI_CH3_PKT_UD_CONNECT) {
                    /* src field is valid (unless we have a connect message),
                     * use src id to get vc */
                    uint64_t index = p->srcid;
                    if (index >= ud_vc_info_id) {
                        SPAWN_ERR("Packet conext invalid");
                        MRAILI_Release_vbuf(v);
                        v = NULL;
                        break;
                    }

                    /* get pointer to vc */
                    MPIDI_VC_t* vc = ud_vc_info[index];

                    /* for UD packets, check that source lid and source
                     * qpn match expected vc to avoid spoofing */
                    if (v->transport == IB_TRANSPORT_UD) {
                        if (vc->lid != wc->slid ||
                            vc->qpn != wc->src_qp)
                        {
                            SPAWN_ERR("Packet source lid/qpn do not match expected values");
                            MRAILI_Release_vbuf(v);
                            v = NULL;
                            break;
                        }
                    }

                    v->vc     = vc;
                    v->rail   = p->rail;
                    v->seqnum = p->seqnum;

                    MRAILI_Process_recv(v);
                } else {
                    /* a connect message does not have a valid src id field,
                     * so we can't associate msg with a vc yet, we stick this
                     * on the queue that accept looks to later */

                    /* allocate and initialize new element for connect queue */
                    connect_list* elem = (connect_list*) SPAWN_MALLOC(sizeof(connect_list));
                    elem->v    = v;          /* record pointer to vbuf */
                    elem->lid  = wc->slid;   /* record source lid */
                    elem->qpn  = wc->src_qp; /* record source qpn */
                    elem->next = NULL;

                    /* append elem to connect queue */
                    if (connect_head == NULL) {
                        connect_head = elem;
                    }
                    if (connect_tail != NULL) {
                        connect_tail->next = elem;
                    }
                    connect_tail = elem;
                }

                /* decrement the count of number of posted receives,
                 * and if we fall below the low-water limit, post more */ 
                proc.ud_ctx->num_recvs_posted--;
                if(proc.ud_ctx->num_recvs_posted < proc.ud_ctx->credit_preserve) {
                    int remaining = rdma_default_max_ud_recv_wqe - proc.ud_ctx->num_recvs_posted;
                    int posted = mv2_post_ud_recv_buffers(remaining, proc.ud_ctx);
                    proc.ud_ctx->num_recvs_posted += posted;
                }
                break;
            default:
                SPAWN_ERR("Invalid opcode from ibv_poll_cq: %d", wc->opcode);
                break;
        }
    }

    /* if sends completed, issue pending sends if we have any */
    if (sendcnt > 0) {
        mv2_ud_update_send_credits(sendcnt);
    }

    return ne;
}

static int mv2_wait_on_channel()
{
    /* resend messages and send acks if we're due */
    long time = mv2_get_time_us();
    long delay = time - rdma_ud_last_check;
    if (delay > rdma_ud_progress_timeout) {
        mv2_check_resend();
        mv2_ud_send_acks();
        rdma_ud_last_check = mv2_get_time_us();
    }

    /* Unlock before going to sleep */
    comm_unlock();

    /* Wait for the completion event */
    void *ev_ctx = NULL;
    struct ibv_cq *ev_cq = NULL;
    if (ibv_get_cq_event(g_hca_info.comp_channel, &ev_cq, &ev_ctx)) {
        ibv_error_abort(-1, "Failed to get cq_event\n");
    }

    /* Get lock before processing */
    comm_lock();

    /* Ack the event */
    ibv_ack_cq_events(ev_cq, 1);

    /* Request notification upon the next completion event */
    if (ibv_req_notify_cq(ev_cq, 0)) {
        ibv_error_abort(-1, "Couldn't request CQ notification\n");
    }

    cq_poll();

    return 0;
}

/* empty all events from completion queue queue */
static void drain_cq_events()
{
    int rc = cq_poll();
    while (rc > 0) {
        rc = cq_poll();
    }
    return;
}

/* Get HCA parameters */
static int hca_get_info(int devnum, mv2_hca_info_t *hca_info)
{
    int i;

    /* we need IB routines to still work after launcher forks children */
    int fork_rc = ibv_fork_init();
    if (fork_rc != 0) {
        SPAWN_ERR("Failed to prepare IB for fork (ibv_fork_init errno=%d %s)",
                    fork_rc, strerror(fork_rc));
        return -1;
    }

    /* get list of HCA devices */
    int num_devices;
    struct ibv_device** dev_list = ibv_get_device_list(&num_devices);
    if (dev_list == NULL) {
        SPAWN_ERR("Failed to get device list (ibv_get_device_list errno=%d %s)",
                    errno, strerror(errno));
        return -1;
    }

    /* check that caller's requested device is within range */
    if (devnum >= num_devices) {
        SPAWN_ERR("Requested device number %d higher than max devices %d",
                    devnum, num_devices);
        ibv_free_device_list(dev_list);
        return -1;
    }

    /* pick out device specified by caller */
    struct ibv_device* dev = dev_list[devnum];

    /* Open the HCA for communication */
    struct ibv_context* context = ibv_open_device(dev);
    if (context == NULL) {
        /* TODO: man page doesn't say anything about errno */
        SPAWN_ERR("Cannot create context for HCA");
        ibv_free_device_list(dev_list);
        return -1;
    }

    /* Create a protection domain for communication */
    struct ibv_pd* pd = ibv_alloc_pd(context);
    if (pd == NULL) {
        /* TODO: man page doesn't say anything about errno */
        SPAWN_ERR("Cannot create PD for HCA");
        ibv_close_device(context);
        ibv_free_device_list(dev_list);
        return -1;
    }
    
    /* Get the attributes of the HCA */
    struct ibv_device_attr attr;
    int retval = ibv_query_device(context, &attr);
    if (retval) {
        SPAWN_ERR("Cannot query HCA (ibv_query_device errno=%d %s)", retval,
                    strerror(retval));
        ibv_dealloc_pd(pd);
        ibv_close_device(context);
        ibv_free_device_list(dev_list);
        return -1;
    }

    /* determine number of ports to query */
    int num_ports = attr.phys_port_cnt;
    if (num_ports > MAX_NUM_PORTS) {
        num_ports = MAX_NUM_PORTS;
    }

    /* allocate space to query each port */
    struct ibv_port_attr* ports = (struct ibv_port_attr*) SPAWN_MALLOC(num_ports * sizeof(struct ibv_port_attr));

    /* Get the attributes of the port */
    for (i = 0; i < num_ports; ++i) {
        retval = ibv_query_port(context, i+1, &ports[i]);
        if (retval != 0) {
            SPAWN_ERR("Failed to query port (ibv_query_port errno=%d %s)", retval, strerror(retval));
        }
    }
    
    /* Create completion channel */
    struct ibv_comp_channel* channel = ibv_create_comp_channel(context);
    if (channel == NULL) {
        /* TODO: man page doesn't say anything about errno */
        SPAWN_ERR("Cannot create completion channel");
        spawn_free(&ports);
        ibv_dealloc_pd(pd);
        ibv_close_device(context);
        ibv_free_device_list(dev_list);
        return -1;
    }

    /* Create completion queue */
    struct ibv_cq* cq = ibv_create_cq(
        context, RDMA_DEFAULT_MAX_CQ_SIZE, NULL, channel, 0
    );
    if (cq == NULL) {
        /* TODO: man page doesn't say anything about errno */
        SPAWN_ERR("Cannot create completion queue");
        ibv_destroy_comp_channel(channel);
        spawn_free(&ports);
        ibv_dealloc_pd(pd);
        ibv_close_device(context);
        ibv_free_device_list(dev_list);
        return -1;
    }

    /* copy values into output struct */
    hca_info->pd           = pd;
    hca_info->device       = dev;
    hca_info->context      = context;
    hca_info->cq_hndl      = cq;
    hca_info->comp_channel = channel;
    for (i = 0; i < num_ports; ++i) {
        memcpy(&(hca_info->port_attr[i]), &ports[i], sizeof(struct ibv_port_attr));
    }
    memcpy(&hca_info->device_attr, &attr, sizeof(struct ibv_device_attr));

    /* free temporary objects */
    spawn_free(&ports);
    ibv_free_device_list(dev_list);

    return 0;
}

/* Transition UD QP */
static int qp_transition(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;

    /* Init QP */
    memset(&attr, 0, sizeof(struct ibv_qp_attr));
    attr.qp_state   = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num   = RDMA_DEFAULT_PORT;
    attr.qkey       = 0;
    int rc = ibv_modify_qp(qp, &attr,
        IBV_QP_STATE |
        IBV_QP_PKEY_INDEX |
        IBV_QP_PORT | IBV_QP_QKEY
    );
    if (rc != 0) {
        SPAWN_ERR("Failed to modify QP to INIT (ibv_modify_qp errno=%d %s)", rc, strerror(rc));
        return 1;
    }    
        
    /* set QP to RTR */
    memset(&attr, 0, sizeof(struct ibv_qp_attr));
    attr.qp_state = IBV_QPS_RTR;
    rc = ibv_modify_qp(qp, &attr, IBV_QP_STATE);
    if (rc != 0) {
        SPAWN_ERR("Failed to modify QP to RTR (ibv_modify_qp errno=%d %s)", rc, strerror(rc));
        return 1;
    }   

    /* set QP to RTS */
    memset(&attr, 0, sizeof(struct ibv_qp_attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn   = RDMA_DEFAULT_PSN;
    rc = ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN);
    if (rc != 0) {
        SPAWN_ERR("Failed to modify QP to RTS (ibv_modify_qp errno=%d %s)", rc, strerror(rc));
        return 1;
    }

    return 0;
}

/* Create UD QP */
static struct ibv_qp* qp_create(mv2_ud_qp_info_t *qp_info)
{
    /* zero out all fields of queue pair attribute structure */
    struct ibv_qp_init_attr init_attr;
    memset(&init_attr, 0, sizeof(struct ibv_qp_init_attr));

    /* set attributes */
    init_attr.send_cq = qp_info->send_cq;
    init_attr.recv_cq = qp_info->recv_cq;
    init_attr.cap.max_send_wr = qp_info->cap.max_send_wr;
    
    if (qp_info->srq) {
        init_attr.srq = qp_info->srq;
        init_attr.cap.max_recv_wr = 0;
    } else {    
        init_attr.cap.max_recv_wr = qp_info->cap.max_recv_wr;
    }

    init_attr.cap.max_send_sge = qp_info->cap.max_send_sge;
    init_attr.cap.max_recv_sge = qp_info->cap.max_recv_sge;
    init_attr.cap.max_inline_data = qp_info->cap.max_inline_data;
    init_attr.qp_type = IBV_QPT_UD;

    /* create queue pair */
    struct ibv_qp* qp = ibv_create_qp(qp_info->pd, &init_attr);
    if(qp == NULL) {
        /* TODO: man page doesn't say anything about errno values */
        SPAWN_ERR("error in creating UD qp");
        return NULL;
    }
    
    /* set queue pair to UD */
    if (qp_transition(qp)) {
        ibv_destroy_qp(qp);
        return NULL;
    }

    return qp;
}

/* Destroy UD Context */
static void mv2_ud_destroy_ctx(mv2_ud_ctx_t *ctx)
{
    /* destroy UD QP if we have one */
    if (ctx->qp) {
        ibv_destroy_qp(ctx->qp);
    }

    /* now free context data structure */
    spawn_free(&ctx);

    spawn_free(&ud_vc_info);

    pthread_cancel(comm_thread);

    return;
}

/* Initialize UD Context */
static spawn_net_endpoint* init_ud()
{
    /* Init lock for vbuf */
    init_vbuf_lock();

    /* initialize lock for communication */
    int ret = pthread_mutex_init(&comm_lock_object, 0);
    if (ret != 0) {
        SPAWN_ERR("Failed to init comm_lock_object (pthread_mutex_init ret=%d %s)",
            ret, strerror(ret));
        ibv_error_abort(-1, "Failed to init comm_lock_object\n");
    }   

    rdma_ud_max_ack_pending = rdma_default_ud_sendwin_size / 4;

    /* increase memory locked limit */
    struct rlimit limit;
    ret = getrlimit(RLIMIT_MEMLOCK, &limit);
    if (ret != 0) {
        SPAWN_ERR("Failed to read MEMLOCK limit (getrlimit errno=%d %s)", errno, strerror(errno));
        return SPAWN_NET_ENDPOINT_NULL;
    }
    limit.rlim_cur = limit.rlim_max;
    ret = setrlimit(RLIMIT_MEMLOCK, &limit);
    if (ret != 0) {
        SPAWN_ERR("Failed to increase MEMLOCK limit (setrlimit errno=%d %s)", errno, strerror(errno));
        return SPAWN_NET_ENDPOINT_NULL;
    }

    /* allocate vbufs */
    allocate_ud_vbufs(RDMA_DEFAULT_NUM_VBUFS);

    /* allocate UD context structure */
    mv2_ud_ctx_t* ctx = (mv2_ud_ctx_t*) SPAWN_MALLOC(sizeof(mv2_ud_ctx_t));

    /* initialize context fields */
    ctx->qp               = NULL;
    ctx->hca_num          = 0;
    ctx->send_wqes_avail  = rdma_default_max_ud_send_wqe - 50;
    ctx->num_recvs_posted = 0;
    ctx->credit_preserve  = (rdma_default_max_ud_recv_wqe / 4);
    MESSAGE_QUEUE_INIT(&ctx->ext_send_queue);
    ctx->ext_sendq_count  = 0;

    /* set parameters for UD queue pair */
    mv2_ud_qp_info_t qp_info;
    qp_info.pd                  = g_hca_info.pd;
    qp_info.srq                 = NULL;
    qp_info.sq_psn              = RDMA_DEFAULT_PSN;
    qp_info.send_cq             = g_hca_info.cq_hndl;
    qp_info.recv_cq             = g_hca_info.cq_hndl;
    qp_info.cap.max_send_wr     = rdma_default_max_ud_send_wqe;
    qp_info.cap.max_recv_wr     = rdma_default_max_ud_recv_wqe;
    qp_info.cap.max_send_sge    = RDMA_DEFAULT_MAX_SG_LIST;
    qp_info.cap.max_recv_sge    = RDMA_DEFAULT_MAX_SG_LIST;
    qp_info.cap.max_inline_data = RDMA_DEFAULT_MAX_INLINE_SIZE;

    /* create UD queue pair and attach to context */
    ctx->qp = qp_create(&qp_info);
    if(ctx->qp == NULL) {
        SPAWN_ERR("Error in creating UD QP");
        return SPAWN_NET_ENDPOINT_NULL;
    }

    /* post initial UD recv requests */
    int remaining = rdma_default_max_ud_recv_wqe - ctx->num_recvs_posted;
    int posted = mv2_post_ud_recv_buffers(remaining, ctx);
    ctx->num_recvs_posted = posted;

    /* save context in global proc structure */
    proc.ud_ctx = ctx;

    /* initialize global unack'd queue */
    MESSAGE_QUEUE_INIT(&proc.unack_queue);

    /* record function pointer to send routine */
    proc.post_send = post_ud_send;

    /* create end point */
    local_ep_info.lid = g_hca_info.port_attr[0].lid;
    local_ep_info.qpn = proc.ud_ctx->qp->qp_num;

    /* allocate new endpoint and fill in its fields */
    spawn_net_endpoint* ep = SPAWN_MALLOC(sizeof(spawn_net_endpoint));
    ep->type = SPAWN_NET_TYPE_IBUD;
    ep->name = SPAWN_STRDUPF("IBUD:%04x:%06x", local_ep_info.lid, local_ep_info.qpn);
    ep->data = NULL;

    /* initialize attributes to create comm thread */
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) {
        SPAWN_ERR("Unable to init thread attr");
        return SPAWN_NET_ENDPOINT_NULL;
    }

    /* set stack size for comm thred */
    ret = pthread_attr_setstacksize(&attr, DEFAULT_CM_THREAD_STACKSIZE);
    if (ret && ret != EINVAL) {
        SPAWN_ERR("Unable to set stack size");
        return SPAWN_NET_ENDPOINT_NULL;
    }

    /* disable SIGCHLD while we start comm thread */
    sigset_t sigmask;
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGCHLD);
    ret = pthread_sigmask(SIG_BLOCK, &sigmask, NULL);
    if (ret != 0) {
        SPAWN_ERR("Failed to block SIGCHLD (pthread_sigmask rc=%d %s)", ret, strerror(ret));
    }

    /* start comm thread */
    pthread_create(&comm_thread, &attr, cm_timeout_handler, NULL);

    /* reenable SIGCHLD in main thread */
    ret = pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL);
    if (ret != 0) {
        SPAWN_ERR("Failed to unblock SIGCHLD (pthread_sigmask rc=%d %s)", ret, strerror(ret));
    }

    return ep;
}

/* interface to lock/unlock connection manager */
void comm_lock(void)
{           
    int rc = pthread_mutex_lock(&comm_lock_object);
    if (rc != 0) {
        SPAWN_ERR("Failed to lock comm mutex (pthread_mutex_lock rc=%d %s)", rc, strerror(rc));
    }
    return;
}
            
void comm_unlock(void)
{           
    int rc = pthread_mutex_unlock(&comm_lock_object);
    if (rc != 0) {
        SPAWN_ERR("Failed to unlock comm mutex (pthread_mutex_unlock rc=%d %s)", rc, strerror(rc));
    }
    return;
}

/* this is the function executed by the communication progress thread */
void* cm_timeout_handler(void *arg)
{
    int nspin = 0;

    /* define sleep time between waking and checking for events */
    cm_timeout.tv_sec = rdma_ud_progress_timeout / 1000000;
    cm_timeout.tv_nsec = (rdma_ud_progress_timeout - cm_timeout.tv_sec * 1000000) * 1000;

    while(1) {
        /* sleep for some time before we look, release lock while
         * sleeping */
        //comm_unlock();
        nanosleep(&cm_timeout, &remain);
        //comm_lock();

#if 0
        /* spin poll for some time to look for new events */
        for (nspin = 0; nspin < rdma_ud_progress_spin; nspin++) {
            cq_poll();
        }
#endif

        /* resend messages and send acks if we're due */
//        long time = mv2_get_time_us();
//        long delay = time - rdma_ud_last_check;
//        if (delay > rdma_ud_progress_timeout) {
            /* time is up, grab lock and process acks */
            comm_lock();

            /* send explicit acks out on all vc's if we need to,
             * this ensures acks flow out even if main thread is
             * busy doing other work */
            mv2_ud_send_acks();

            /* process any messages that may have come in, we may
             * clear messages we'd otherwise try to resend below */
            drain_cq_events();

            /* resend any unack'd packets whose timeout has expired */
            mv2_check_resend();

            /* done sending messages, release lock */
            comm_unlock();

            /* record the last time we checked acks */
//            rdma_ud_last_check = mv2_get_time_us();
//        }
    }

    return NULL;
}

/* given a vbuf used for a send, release vbuf back to pool if we can */
int MRAILI_Process_send(void *vbuf_addr)
{
    vbuf *v = vbuf_addr;

    if (v->padding == NORMAL_VBUF_FLAG) {
        MRAILI_Release_vbuf(v);
    } else {
        printf("Couldn't release VBUF; v->padding = %d\n", v->padding);
    }

    return 0;
}

void MPIDI_CH3I_MRAIL_Release_vbuf(vbuf * v)
{
    /* clear some fields */
    v->eager = 0;
    v->coalesce = 0;
    v->content_size = 0;

    if (v->padding == NORMAL_VBUF_FLAG || v->padding == RPUT_VBUF_FLAG) {
        MRAILI_Release_vbuf(v);
    }
}

/* blocks until packet comes in on specified VC */
static vbuf* packet_read(MPIDI_VC_t* vc)
{
    /* eagerly pull all events from completion queue */
    drain_cq_events();

    /* look for entry in apprecv queue */
    vbuf* v = mv2_ud_apprecv_window_retrieve_and_remove(&vc->app_recv_window);
    while (v == NULL) {
        /* TODO: at this point, we should block for incoming event */

        /* release the lock for some time to let other threads make
         * progress */
        comm_unlock();
//        nanosleep(&cm_timeout, &remain);
        comm_lock();

        /* eagerly pull all events from completion queue */
        drain_cq_events();

        /* look for entry in apprecv queue */
        v = mv2_ud_apprecv_window_retrieve_and_remove(&vc->app_recv_window);
    }
    return v;
}

/* given a virtual channel, a packet type, and payload, construct and
 * send UD packet */
static inline int packet_send(
    MPIDI_VC_t* vc,
    uint8_t type,
    const void* payload,
    size_t payload_size)
{
    /* grab a packet */
    vbuf* v = get_ud_vbuf();
    if (v == NULL) {
        SPAWN_ERR("Failed to get vbuf");
        return SPAWN_FAILURE;
    }

    /* compute size of packet header */
    size_t header_size = sizeof(MPIDI_CH3I_MRAILI_Pkt_comm_header);

    /* check that we have space for payload */
    assert((MRAIL_MAX_UD_SIZE - header_size) >= payload_size);

    /* set packet header fields */
    MPIDI_CH3I_MRAILI_Pkt_comm_header* p = v->pheader;
    memset((void*)p, 0xfc, sizeof(MPIDI_CH3I_MRAILI_Pkt_comm_header));
    p->type = type;

    /* copy in payload */
    if (payload_size > 0) {
        char* ptr = (char*)v->buffer + header_size;
        memcpy(ptr, payload, payload_size);
    }

    /* set packet size */
    v->content_size = header_size + payload_size;

    /* prepare packet for send */
    int rail = 0;
    vbuf_init_send(v, v->content_size, rail);

    /* and send it */
    proc.post_send(vc, v, rail, NULL);

    return SPAWN_SUCCESS;
}

/* blocks until element arrives on connect queue,
 * extracts element and returns its vbuf */
static vbuf* recv_connect_message()
{
    /* eagerly pull all events from completion queue */
    drain_cq_events();

    /* wait until we see at item at head of connect queue */
    while (connect_head == NULL) {
        /* TODO: at this point, we should block for incoming event */

        comm_unlock();
//        nanosleep(&cm_timeout, &remain);
        comm_lock();

        /* eagerly pull all events from completion queue */
        drain_cq_events();
    }

    /* get pointer to element */
    connect_list* elem = connect_head;

    /* extract element from queue */
    connect_head = elem->next;
    if (connect_head == NULL) {
        connect_tail = NULL;
    }

    /* get pointer to vbuf */
    vbuf* v = elem->v;

    /* free element */
    spawn_free(&elem);

    /* return vbuf */
    return v;
}

static int recv_accept_message(MPIDI_VC_t* vc)
{
    /* first incoming packet should be accept */
    vbuf* v = packet_read(vc);

    /* message payload is write id we should use when sending */
    size_t header_size = sizeof(MPIDI_CH3I_MRAILI_Pkt_comm_header);
    char* payload = PKT_DATA_OFFSET(v, header_size);

    /* extract write id from payload */
    int id;
    int parsed = sscanf(payload, "%06x", &id);
    if (parsed != 1) {
        SPAWN_ERR("Couldn't parse write id from accept message");
        MRAILI_Release_vbuf(v);
        return SPAWN_FAILURE;
    }

    /* TODO: avoid casting up from int here */
    /* set our write id */
    uint64_t writeid = (uint64_t) id;
    vc->writeid = writeid;

    /* put vbuf back on free list */
    MRAILI_Release_vbuf(v);

    return SPAWN_SUCCESS;
}

/*******************************************
 * spawn_net API for IBUD
 ******************************************/

spawn_net_endpoint* spawn_net_open_ib()
{
    /* open endpoint if we need to */
    if (open_count == 0) {
        /* Open HCA for communication */
        memset(&g_hca_info, 0, sizeof(mv2_hca_info_t));
        if (hca_get_info(0, &g_hca_info) != 0){
            SPAWN_ERR("Failed to initialize HCA");
            return -1;
        }
        return 0;

        ep = init_ud();
    }

    open_count++;

    return ep;
}

int spawn_net_close_ib(spawn_net_endpoint** pep)
{
    open_count--;
    if (open_count == 0) {
        /* TODO: close down UD endpoint */
    }
}

spawn_net_channel* spawn_net_connect_ib(const char* name)
{
    comm_lock();

    /* extract lid and queue pair address from endpoint name */
    unsigned int lid, qpn;
    int parsed = sscanf(name, "IBUD:%04x:%06x", &lid, &qpn);
    if (parsed != 2) {
        SPAWN_ERR("Couldn't parse ep info from %s", name);
        return SPAWN_NET_CHANNEL_NULL;
    }

    /* allocate and initialize a new virtual channel */
    MPIDI_VC_t* vc = vc_alloc();

    /* store lid and queue pair */
    mv2_ud_exch_info_t ep_info;
    ep_info.lid = lid;
    ep_info.qpn = qpn;

    /* point channel to remote endpoint */
    vc_set_addr(vc, &ep_info, RDMA_DEFAULT_PORT);

    /* build payload for connect message, specify id we want remote
     * side to use when sending to us followed by our lid/qp */
    char* payload = SPAWN_STRDUPF("%06x:%04x:%06x",
        vc->readid, local_ep_info.lid, local_ep_info.qpn
    );
    size_t payload_size = strlen(payload) + 1;

    /* send connect packet */
    int rc = packet_send(vc, MPIDI_CH3_PKT_UD_CONNECT, payload, payload_size);

    /* free payload memory */
    spawn_free(&payload);

    /* wait for accept message and set vc->writeid */
    recv_accept_message(vc);

    /* Change state to connected */
    vc->state = MRAILI_UD_CONNECTED;

    /* allocate spawn net channel data structure */
    spawn_net_channel* ch = SPAWN_MALLOC(sizeof(spawn_net_channel));
    ch->type = SPAWN_NET_TYPE_IBUD;

    /* TODO: include hostname here */
    /* Fill in channel name */
    ch->name = SPAWN_STRDUPF("IBUD:%04x:%06x", ep_info.lid, ep_info.qpn);

    /* record address of vc in channel data field */
    ch->data = (void*) vc;

    comm_unlock();

    return ch;
}

spawn_net_channel* spawn_net_accept_ib(const spawn_net_endpoint* ep)
{
    comm_lock();

    /* NOTE: If we're slow to connect, the process that sent us the
     * connect packet may have timed out and sent a duplicate request.
     * Both packets may be in our connection request queue, and we
     * want to ignore any duplicates.  To do this, we keep track of
     * current connections by recording remote lid/qpn/writeid and
     * then silently drop extras. */

    /* wait for connect message */
    vbuf* v = NULL;
    unsigned int id, lid, qpn;
    while (v == NULL ) {
        /* get next vbuf from connection request queue */
        v = recv_connect_message();

        /* get pointer to payload */
        size_t header_size = sizeof(MPIDI_CH3I_MRAILI_Pkt_comm_header);
        char* connect_payload = PKT_DATA_OFFSET(v, header_size);

        /* TODO: read lid/qpn from vbuf and not payload to avoid
         * spoofing */

        /* get id and endpoint name from message payload */
        int parsed = sscanf(connect_payload, "%06x:%04x:%06x", &id, &lid, &qpn);
        if (parsed != 3) {
            SPAWN_ERR("Couldn't parse ep info from %s", connect_payload);
            return NULL;
        }

        /* check that we don't already have a matching connection */
        connected_list* elem = connected_head;
        while (elem != NULL) {
            /* check whether this connect request matches one we're
             * already connected to */
            if (elem->lid == lid &&
                elem->qpn == qpn &&
                elem->id  == id)
            {
                /* we're already connected to this process, free the
                 * vbuf and look for another request message */
                MRAILI_Release_vbuf(v);
                v = NULL;
                break;
            }

            /* no match so far, try the next item in connected list */
            elem = elem->next;
        }
    }

    /* allocate new vc */
    MPIDI_VC_t* vc = vc_alloc();

    /* allocate and initialize new item for connected list */
    connected_list* elem = (connected_list*) SPAWN_MALLOC(sizeof(connected_list));
    elem->lid  = lid;
    elem->qpn  = qpn;
    elem->id   = id;
    elem->vc   = vc;
    elem->next = NULL;

    /* append item to connected list */
    if (connected_head == NULL) {
        connected_head = elem;
    }
    if (connected_tail != NULL) {
        connected_tail->next = elem;
    }
    connected_tail = elem;

    /* store lid and queue pair */
    mv2_ud_exch_info_t ep_info;
    ep_info.lid = lid;
    ep_info.qpn = qpn;

    /* record lid/qp in VC */
    vc_set_addr(vc, &ep_info, RDMA_DEFAULT_PORT);

    /* record remote id as write id */
    uint64_t writeid = (uint64_t) id;
    vc->writeid = writeid;

    /* record pointer to VC in vbuf (needed by Process_recv) */
    v->vc = vc;

    /* put vbuf back on free list */
    MRAILI_Process_recv(v);

    /* Increment the next expected seq num */
    vc->seqnum_next_torecv++;

    /* build accept message, specify id we want remote side to use when
     * sending to us followed by our lid/qp */
    char* payload = SPAWN_STRDUPF("%06x", vc->readid);
    size_t payload_size = strlen(payload) + 1;

    /* send the accept packet */
    int rc = packet_send(vc, MPIDI_CH3_PKT_UD_ACCEPT, payload, payload_size);

    /* free payload memory */
    spawn_free(&payload);

    /* mark vc as connected */
    vc->state = MRAILI_UD_CONNECTED;

    /* allocate new channel data structure */
    spawn_net_channel* ch = SPAWN_MALLOC(sizeof(spawn_net_channel));
    ch->type = SPAWN_NET_TYPE_IBUD;

    /* record name */
    ch->name = SPAWN_STRDUPF("IBUD:%04x:%06x", ep_info.lid, ep_info.qpn);

    /* record address of vc in channel data field */
    ch->data = (void*) vc;

    comm_unlock();

    return ch;
}

int spawn_net_disconnect_ib(spawn_net_channel** pch)
{
    comm_lock();
    comm_unlock();

    return SPAWN_SUCCESS;
}

int spawn_net_read_ib(const spawn_net_channel* ch, void* buf, size_t size)
{
    /* get pointer to vc from channel data field */
    MPIDI_VC_t* vc = (MPIDI_VC_t*) ch->data;
    if (vc == NULL) {
        return SPAWN_FAILURE;
    }

    comm_lock();

    /* compute header and payload sizes */
    size_t header_size = sizeof(MPIDI_CH3I_MRAILI_Pkt_comm_header);
    assert(MRAIL_MAX_UD_SIZE >= header_size);

    /* read data one packet at a time */
    int ret = SPAWN_SUCCESS;
    size_t nread = 0;
    while (nread < size) {
        /* read a packet from this vc */
        vbuf* v = packet_read(vc);

        /* copy data to user's buffer */
        size_t payload_size = PKT_DATA_SIZE(v, header_size);
        if (payload_size > 0) {
            /* get pointer to message payload */
            char* ptr  = (char*)buf + nread;
            char* data = PKT_DATA_OFFSET(v, header_size);
            memcpy(ptr, data, payload_size);
        }

        /* put vbuf back on free list */
        MRAILI_Release_vbuf(v);

        /* go on to next part of message */
        nread += payload_size;
    }

    comm_unlock();

    return ret;
}

int spawn_net_write_ib(const spawn_net_channel* ch, const void* buf, size_t size)
{
    /* get pointer to vc from channel data field */
    MPIDI_VC_t* vc = (MPIDI_VC_t*) ch->data;
    if (vc == NULL) {
        return SPAWN_FAILURE;
    }

    comm_lock();

    /* compute header and payload sizes */
    size_t header_size = sizeof(MPIDI_CH3I_MRAILI_Pkt_comm_header);
    size_t payload_size = MRAIL_MAX_UD_SIZE - header_size;
    assert(MRAIL_MAX_UD_SIZE >= header_size);

    /* break message up into packets and send each one */
    int ret = SPAWN_SUCCESS;
    size_t nwritten = 0;
    while (nwritten < size) {
        /* determine amount to write in this step */
        size_t bytes = (size - nwritten);
        if (bytes > payload_size) {
            bytes = payload_size;
        }

        /* get pointer to data */
        char* data = (char*)buf + nwritten;

        /* send packet */
        int tmp_rc = packet_send(vc, MPIDI_CH3_PKT_UD_DATA, data, bytes);
        if (tmp_rc != SPAWN_SUCCESS) {
            ret = tmp_rc;
            break;
        }

        /* go to next part of message */
        nwritten += bytes;
    }

    comm_unlock();

    return ret;
}
