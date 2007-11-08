#include <linux/kthread.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/string.h>
#include <net/tcp.h>

#include <linux/ceph_fs.h>
#include <linux/ceph_fs_msgs.h>
#include "messenger.h"
#include "ktcp.h"

static struct workqueue_struct *recv_wq;        /* receive work queue ) */
static struct workqueue_struct *send_wq;        /* send work queue */

struct task_struct *athread;  /* accepter thread, TBD: fill into kmsgr */

/* static tag bytes */
static char tag_ready = CEPH_MSGR_TAG_READY;
static char tag_reject = CEPH_MSGR_TAG_REJECT;
static char tag_msg = CEPH_MSGR_TAG_MSG;
static char tag_ack = CEPH_MSGR_TAG_ACK;
static char tag_close = CEPH_MSGR_TAG_CLOSE;


/*
 * connections
 */

/* 
 * create a new connection.  initial state is NEW.
 */
static struct ceph_connection *new_connection(struct ceph_messenger *msgr)
{
	struct ceph_connection *con;
	con = kmalloc(sizeof(struct ceph_connection), GFP_KERNEL);
	if (con == NULL) return 0;
	memset(&con, 0, sizeof(con));

	con->msgr = msgr;

	spin_lock_init(&con->con_lock);
	INIT_WORK(&con->rwork, try_read);	/* setup work structure */
	INIT_WORK(&con->rwork, try_write);	/* setup work structure */

	atomic_inc(&con->nref);
	return con;
}

/*
 * the radix_tree has an unsigned long key and void * value.  since
 * ceph_entity_addr is bigger than that, we use a trivial hash key, and
 * point to a list_head in ceph_connection, as you would with a hash
 * table.  in the rare event that the trivial hash collides, we just
 * traverse the (short) list.
 */
static unsigned long hash_addr(struct ceph_entity_addr *addr) 
{
	unsigned long key;
	key = *(unsigned long*)&addr->ipaddr.sin_addr.s_addr;
	key ^= addr->ipaddr.sin_port;
	return key;
}

/* 
 * get an existing connection, if any, for given addr
 */
static struct ceph_connection *get_connection(struct ceph_messenger *msgr, struct ceph_entity_addr *addr)
{
	struct ceph_connection *con;
	struct list_head *head, *p;
	unsigned long key = hash_addr(addr);

	/* existing? */
	spin_lock(&msgr->con_lock);
	head = radix_tree_lookup(&msgr->con_open, key);
	if (head) {
		list_for_each(p, head) {
			con = list_entry(p, struct ceph_connection, list_bucket);
			if (memcmp(&con->peer_addr, addr, sizeof(addr)) == 0) {
				atomic_inc(&con->nref);
				goto out;
			}
		}
	}
	con = NULL;
out:
	spin_unlock(&msgr->con_lock);
	return con;
}

/* 
 * drop a reference
 */
static void put_connection(struct ceph_connection *con) 
{
	if (atomic_dec_and_test(&con->nref)) {
		sock_release(con->sock);
		kfree(con);
	}
}

/* 
 * add to connections tree
 */
static void add_connection_accepted(struct ceph_messenger *msgr, struct ceph_connection *con)
{
	struct list_head *head;
	unsigned long key = hash_addr(&con->peer_addr);

	/* inc ref count */
	atomic_inc(&con->nref);

	spin_lock(&msgr->con_lock);
	head = radix_tree_lookup(&msgr->con_open, key);
	if (head) {
		list_add(head, &con->list_bucket);
	} else {
		INIT_LIST_HEAD(&con->list_bucket); /* empty */
		radix_tree_insert(&msgr->con_open, key, &con->list_bucket);
	}
	spin_unlock(&msgr->con_lock);
}

static void add_connection_accepting(struct ceph_messenger *msgr, struct ceph_connection *con)
{
	atomic_inc(&con->nref);
	spin_lock(&msgr->con_lock);
	list_add(&msgr->con_all, &con->list_all);
	list_add(&msgr->con_accepting, &con->list_bucket);
	spin_unlock(&msgr->con_lock);
}

/*
 * remove connection from all list.
 * also, from con_open radix tree, if it should have been there
 */
static void remove_connection(struct ceph_messenger *msgr, struct ceph_connection *con)
{
	unsigned long key = hash_addr(&con->peer_addr);

	spin_lock(&msgr->con_lock);
	list_del(&con->list_all);
	if (con->state == CONNECTING ||
	    con->state == OPEN) {
		/* remove from con_open too */
		if (list_empty(&con->list_bucket)) {
			/* last one */
			radix_tree_delete(&msgr->con_open, key);
		} else {
			list_del(&con->list_bucket);
		}
	}
	spin_unlock(&msgr->con_lock);

	put_connection(con);
}


/*
 * replace another connection
 *  (old and new should be for the _same_ peer, and thus in the same pos in the radix tree)
 */
static void replace_connection(struct ceph_messenger *msgr, struct ceph_connection *old, struct ceph_connection *new)
{
	spin_lock(&msgr->con_lock);
	list_add(&new->list_bucket, &old->list_bucket);
	list_del(&old->list_bucket);
	spin_unlock(&msgr->con_lock);
	put_connection(old); /* dec reference count */
}



/*
 * non-blocking versions
 *
 * these should be called while holding con->con_lock
 */

/*
 * write as much of con->out_partial to the socket as we can.
 *  1 -> done; and cleaned up out_partial
 *  0 -> socket full, but more to do
 * <0 -> error
 */
static int write_partial(struct ceph_connection *con)
{
	struct ceph_bufferlist *bl = &con->out_partial;
	struct ceph_bufferlist_iterator *p = &con->out_pos;
	int len, ret;

more:
	len = bl->b_kv[p->i_kv].iov_len - p->i_off;
	/* FIXME */
	ret = kernel_send(con->sock, bl->b_kv[p->i_kv].iov_base + p->i_off, len);
	if (ret < 0) return ret;
	if (ret == 0) return 0;   /* socket full */
	if (ret + p->i_off == bl->b_kv[p->i_kv].iov_len) {
		p->i_kv++;
		p->i_off = 0;
		if (p->i_kv == bl->b_kvlen) 
			return 1;
	} else {
		p->i_off += ret;
	}
	goto more;
}

/*
 * build out_partial based on the next outgoing message in the queue.
 */
static void prepare_write_message(struct ceph_connection *con)
{
	struct ceph_message *m = list_entry(con->out_queue.next, struct ceph_message, list_head);
	int i;

	/* move to sending/sent list */
	list_del(&m->list_head);
	list_add(&m->list_head, &con->out_sent);
	
	ceph_bl_init(&con->out_partial);  
	ceph_bl_iterator_init(&con->out_pos);

	/* always one chunk, for now */
	m->hdr.nchunks = 1;  
	m->chunklens[0] = m->payload.b_len;

	/* tag + header */
	ceph_bl_append_ref(&con->out_partial, &tag_msg, 1);
	ceph_bl_append_ref(&con->out_partial, &m->hdr, sizeof(m->hdr));
	
	/* payload */
	ceph_bl_append_ref(&con->out_partial, &m->chunklens[0], sizeof(__u32));
	for (i=0; i<m->payload.b_kvlen; i++) 
		ceph_bl_append_ref(&con->out_partial, m->payload.b_kv[i].iov_base, 
				   m->payload.b_kv[i].iov_len);
}

/* 
 * prepare an ack for send
 */
static void prepare_write_ack(struct ceph_connection *con)
{
	con->in_seq_acked = con->in_seq;
	
	ceph_bl_init(&con->out_partial);  
	ceph_bl_iterator_init(&con->out_pos);
	ceph_bl_append_ref(&con->out_partial, &tag_ack, 1);
	ceph_bl_append_ref(&con->out_partial, &con->in_seq_acked, sizeof(con->in_seq_acked));
}

static void prepare_write_accept_announce(struct ceph_messenger *msgr, struct ceph_connection *con)
{
	ceph_bl_init(&con->out_partial);  
	ceph_bl_iterator_init(&con->out_pos);
	ceph_bl_append_ref(&con->out_partial, &msgr->addr, sizeof(msgr->addr));
}

static void prepare_write_accept_ready(struct ceph_connection *con)
{
	ceph_bl_init(&con->out_partial);  
	ceph_bl_iterator_init(&con->out_pos);
	ceph_bl_append_ref(&con->out_partial, &tag_ready, 1);
}
static void prepare_write_accept_reject(struct ceph_connection *con)
{
	ceph_bl_init(&con->out_partial);  
	ceph_bl_iterator_init(&con->out_pos);
	ceph_bl_append_ref(&con->out_partial, &tag_reject, 1);
	ceph_bl_append_ref(&con->out_partial, &con->connect_seq, sizeof(con->connect_seq));
}

/*
 * call when socket is writeable
 */
static int try_write(struct ceph_messenger *msgr, struct work_struct *work)
{
	int ret;
	struct ceph_connection *con;

	con = container_of(work, struct ceph_connection, swork);

more:
	/* data queued? */
	if (con->out_partial.b_kvlen) {
		ret = write_partial(con);
		if (ret == 0) return 0;

		/* error or success */
		/* clean up */
		ceph_bl_init(&con->out_partial);  
		ceph_bl_iterator_init(&con->out_pos);

		if (con->state == REJECTING) {
			/* FIXME do something else here, pbly? */
			remove_connection(msgr, con);
			con->state = CLOSED;  
			put_connection(con);
		}
		
		if (ret < 0) return ret; /* error */
	}
	
	/* anything else pending? */
	if (con->in_seq > con->in_seq_acked) {
		prepare_write_ack(con);
		goto more;
	}
	if (!list_empty(&con->out_queue)) {
		prepare_write_message(con);
		goto more;
	}
	
	/* hmm, nothing to do! */
	return 0;
}


/*
 * read (part of) a message
 */
static int read_message_partial(struct ceph_connection *con)
{
	struct ceph_message *m = con->in_partial;
	int left, ret, s, chunkbytes, c, did;

	while (con->in_base_pos < sizeof(struct ceph_message_header)) {
		left = sizeof(struct ceph_message_header) - con->in_base_pos;
		ret = _read(con->sock, &m->hdr + con->in_base_pos, left);
		if (ret <= 0) return ret;
		con->in_base_pos += ret;
	}
	if (m->hdr.nchunks == 0) return 1; /* done */

	chunkbytes = sizeof(__u32)*m->hdr.nchunks;
	while (con->in_base_pos < sizeof(struct ceph_message_header) + chunkbytes) {
		int off = con->in_base_pos - sizeof(struct ceph_message_header);
		left = chunkbytes + sizeof(struct ceph_message_header) - con->in_base_pos;
		ret = _read(con->sock, (char*)m->chunklens + off, left);
		if (ret <= 0) return ret;
		con->in_base_pos += ret;
	}
	
	did = 0;
	for (c = 0; c<m->hdr.nchunks; c++) {
	more:
		left = did + m->chunklens[c] - m->payload.b_len;
		if (left <= 0) {
			did += m->chunklens[c];
			continue;
		}
		ceph_bl_prepare_append(&m->payload, left);
		s = min(m->payload.b_append.iov_len, left);
		ret = _read(con->sock, m->payload.b_append.iov_base, s);
		if (ret <= 0) return ret;
		ceph_bl_append_copied(&m->payload, s);
		goto more;
	}
	return 1; /* done! */
}

/*
 * read (part of) an ack
 */
static int read_ack_partial(struct ceph_connection *con)
{
	while (con->in_base_pos < sizeof(con->in_partial_ack)) {
		int left = sizeof(con->in_partial_ack) - con->in_base_pos;
		int ret = _read(con->sock, (char*)&con->in_partial_ack + con->in_base_pos, left);
		if (ret <= 0) return ret;
		con->in_base_pos += ret;
	}
	return 1; /* done */
}


static int read_accept_partial(struct ceph_connection *con)
{
	/* peer addr */
	while (con->in_base_pos < sizeof(con->peer_addr)) {
		int left = sizeof(con->peer_addr) - con->in_base_pos;
		int ret = _read(con->sock, (char*)&con->peer_addr + con->in_base_pos, left);
		if (ret <= 0) return ret;
		con->in_base_pos += ret;
	}

	/* connect_seq */
	while (con->in_base_pos < sizeof(con->peer_addr) + sizeof(con->connect_seq)) {
		int off = con->in_base_pos - sizeof(con->peer_addr);
		int left = sizeof(con->peer_addr) + sizeof(con->connect_seq) - con->in_base_pos;
		ret = _read(con->sock, (char*)&con->connect_seq + off, left);
		if (ret <= 0) return ret;
		con->in_base_pos += ret;
	}
	return 1; /* done */
}

/* 
 * prepare to read a message
 */
static int prepare_read_message(struct ceph_connection *con)
{
	con->in_tag = CEPH_MSGR_TAG_MSG;
	con->in_base_pos = 0;
	con->in_partial = kmalloc(sizeof(struct ceph_message), GFP_KERNEL);
	if (!con->in_partial) return -1;  /* crap */
	ceph_get_msg(con->in_partial);
	ceph_bl_init(&con->in_partial->payload);
	ceph_bl_iterator_init(&con->in_pos);
}

/* 
 * prepare to read an ack
 */
static void prepare_read_ack(struct ceph_connection *con)
{
	con->in_tag = CEPH_MSGR_TAG_ACK;
	con->in_base_pos = 0;
}

static void process_ack(struct ceph_connection *con, __u32 ack)
{
	struct ceph_message *m;
	while (!list_empty(&con->out_sent)) {
		m = list_entry(con->out_sent.next, struct ceph_message, list_head);
		if (m->hdr.seq > ack) break;
		dout(5, "got ack for %d type %d at %p\n", m->hdr.seq, m->hdr.type, m);
		list_del(&m->list_head);
		ceph_put_msg(m);
	}
}

/*
 * call after a new connection's handshake has completed
 */
static void process_accept(struct ceph_connection *con)
{
	struct ceph_connection *existing;

	/* do we already have a connection for this peer? */
	spin_lock(&con->msgr->con_lock);
	existing = get_connection(con->msgr, &con->peer_addr);
	if (existing) {
		spin_lock(&existing->con_lock);
		if ((existing->state == CONNECTING && compare_addr(&con->msgr->addr, &con->peer_addr)) ||
		    (existing->state == OPEN && con->connect_seq == existing->connect_seq)) {
			/* replace existing with new connection */
			replace_connection(con->msgr, existing, con);
			/* steal message queue */
			list_splice_init(&con->out_queue, &existing->out_queue); /* fixme order */
			con->out_seq = existing->out_seq;
			con->state = OPEN;
			existing->state = CLOSED;
		} else {
			/* reject new connection */
			con->state = REJECTING;
			con->connect_seq = existing->connect_seq; /* send this with the reject */
		}
		spin_unlock(&existing->con_lock);
		put_connection(existing);
	} else {
		add_connection_accepted(con->msgr, con);
		con->state = OPEN;
	}
	spin_unlock(&con->msgr->con_lock);

	/* the result? */
	if (con->state == REJECTING)
		prepare_write_accept_reject(con);
	else
		prepare_write_accept_ready(con);
}


/*
 * call when data is available on the socket
 */
static int try_read(struct work_struct *work)
{
	int ret = -1;
	struct ceph_connection *con;

	con = container_of(work, struct ceph_connection, rwork);

more:
	if (con->state == CLOSED) return -1;
	if (con->state == ACCEPTING) {
		ret = read_accept_partial(con);
		if (ret <= 0) return ret;
		/* accepted */
		process_accept(con);
		goto more;
	}

	if (con->in_tag == CEPH_MSGR_TAG_READY) {
		ret = _read(con->sock, &con->in_tag, 1);
		if (ret <= 0) return ret;
		if (con->in_tag == CEPH_MSGR_TAG_MSG) 
			prepare_read_message(con);
		else if (con->in_tag == CEPH_MSGR_TAG_ACK)
			prepare_read_ack(con);
		else {
			printk(KERN_INFO "bad tag %d\n", (int)con->in_tag);
			goto bad;
		}
		goto more;
	}
	if (con->in_tag == CEPH_MSGR_TAG_MSG) {
		ret = read_message_partial(con);
		if (ret <= 0) return ret;
		/* got a full message! */
		msgr->dispatch(con->msgr->parent, con->in_partial);
		ceph_put_msg(con->in_partial);
		con->in_partial = 0;
		con->in_tag = CEPH_MSGR_TAG_READY;
		goto more;
	}
	if (con->in_tag == CEPH_MSGR_TAG_ACK) {
		ret = read_ack_partial(con);
		if (ret <= 0) return ret;
		/* got an ack */
		process_ack(con, con->in_partial_ack);
		con->in_tag = CEPH_MSGR_TAG_READY;
		goto more;
	}
bad:
	BUG_ON(1); /* shouldn't get here */
	return ret;
}


/*
 * Accepter thread
 */
static int try_accept(struct work_struct *work)
{
	struct socket *sd, *new_sd;
	struct sockaddr saddr;
	struct ceph_connection *con;
        struct ceph_connection *new_con = NULL;

	con = container_of(work, struct ceph_connection, awork);


        printk(KERN_INFO "Entered try_accept\n");


        if(kernel_accept(sd, &new_sd, sd->file->f_flags) < 0) {
        	printk(KERN_INFO "error accepting connection \n");
                goto done;
        }
        printk(KERN_INFO "accepted connection \n");

        /* get the address at the other end */
        memset(&saddr, 0, sizeof(saddr));
        if (new_sd->ops->getname(new_sd, saddr, &len, 2)) {
                printk(KERN_INFO "getname error connection aborted\n");
                sock_release(new_sd);
                goto done;
        }

	/* initialize the msgr connection */
	new_con = new_connection(msgr);
	if (new_con == NULL) {
               	printk(KERN_INFO "malloc failure\n");
		sock_release(new_sd);
		goto done;
       	}
	new_con->sock = new_sd;
	setbit(ACCEPTING, &con->state);
	new_con->in_tag = CEPH_MSGR_TAG_READY;
	new_con->peeraddr = saddr;
/* TBD: may not use this.. */
	new_sd->sk->sk_user_data = con;

	prepare_write_accept_announce(msgr, con);

	add_connection_accepting(msgr, con);

/* add to poll list? or hand off to send workqueue? */

	/* hand off to worker threads , send pending */
	/*?? queue_work(send_wq, &new_con->swork);*/
done:
        return(0);
}


int ceph_work_init(void)
{
        int ret = 0;

	/*
	 * Create a num CPU threads to handle receive requests
	 * note: we can create more threads if needed to even out
	 * the scheduling of multiple requests.. 
	 */
        recv_wq = create_workqueue("ceph recv");
        ret = IS_ERR(recv_wq);
        if (ret) {
		printk(KERN_INFO "receive worker failed to start: %d\n", ret);
                destroy_workqueue(recv_wq);
                return ret;
        }

	/*
	 * Create a single thread to handle send requests 
	 * note: may use same thread pool as receive workers later...
	 */
        send_wq = create_singlethread_workqueue("ceph send");
        ret = IS_ERR(send_wq);
        if (ret) {
		printk(KERN_INFO "send worker failed to start: %d\n", ret);
                destroy_workqueue(send_wq);
                return ret;
        }

        return(ret);
}

void ceph_work_shutdown(void)
{
/* TBD: need to do this during unmount*/
/*
	kthread_stop(msgr->athread);
	wake_up_process(msgr->athread);
*/
	kthread_stop(athread);
	wake_up_process(athread);
	destroy_workqueue(send_wq);
	destroy_workqueue(recv_wq);
}

struct ceph_connection *new_listener(struct ceph_messenger *msgr)
{
	struct ceph_connection *con;
        struct sockaddr saddr;
        memset(&saddr, 0, sizeof(saddr));

        con = kmalloc(sizeof(struct ceph_connection), GFP_KERNEL);
        if (con == NULL) return 0;
        memset(&con, 0, sizeof(con));

	/* create listener connection */
        spin_lock_init(&con->con_lock);
        INIT_WORK(&con->awork, try_accept);       /* setup work structure */
        con->msgr = msgr;
        atomic_inc(&con->nref);

        /* TBD: if address specified by mount */
                /* make my address from user specified address, fill in saddr */

        con->sock = _klisten(&saddr);
	return(con);
}

/*
 * create a new messenger
 */
static struct ceph_messenger *new_messenger()
{
        struct ceph_messenger *msgr;
        struct ceph_connection *con;
	struct ceph_poll_task ptsk;
	struct ceph_poll_task pfiles;

        msgr = kmalloc(sizeof(struct ceph_messenger), GFP_KERNEL);
        if (msgr == NULL) return 0;
        memset(&msgr, 0, sizeof(msgr));

        ptsk = kmalloc(sizeof(struct ceph_poll_task), GFP_KERNEL);
        if (ptsk == NULL) {
		kfree(msgr);
		return 0;
	}
        memset(&ptsk, 0, sizeof(ptsk));

	pfiles =  kmalloc(sizeof(struct ceph_poll_task), GFP_KERNEL);
        if (pfiles == NULL) {
		kfree(msgr);
		kfree(ptsk);
		return 0;
	}
        memset(pfiles, 0, sizeof(pfiles));

	/* create listener connection */
	con = new_listener(msgr);
	pfiles->con = con;
	pfiles->file = con->sock->file;
	ptsk->pfiles = pfiles

	/* start up poll thread */
	ptsk->poll_task = kthread_run(ceph_poll, msgr, "ceph-poll");
	msgr->poll_task = ptsk;
        return msgr;
}
