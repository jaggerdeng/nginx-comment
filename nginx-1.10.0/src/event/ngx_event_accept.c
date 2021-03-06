
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


static ngx_int_t ngx_enable_accept_events(ngx_cycle_t *cycle);
static ngx_int_t ngx_disable_accept_events(ngx_cycle_t *cycle, ngx_uint_t all);
static void ngx_close_accepted_connection(ngx_connection_t *c);
#if (NGX_DEBUG)
static void ngx_debug_accepted_connection(ngx_event_conf_t *ecf,
    ngx_connection_t *c);
#endif

/*处理新连接事件的回调函数*/
void
ngx_event_accept(ngx_event_t *ev)  //这里的ev是在ngx_event_process_init函数中监听端口连接的读事件
{
    socklen_t          socklen;
    ngx_err_t          err;
    ngx_log_t         *log;
    ngx_uint_t         level;
    ngx_socket_t       s;
    ngx_event_t       *rev, *wev;
    ngx_listening_t   *ls;
    ngx_connection_t  *c, *lc;
    ngx_event_conf_t  *ecf;
    u_char             sa[NGX_SOCKADDRLEN];
#if (NGX_HAVE_ACCEPT4)
    static ngx_uint_t  use_accept4 = 1;
#endif

	/*如果事件超时，则将监听端口连接的读事件添加到事件驱动模块中*/
    if (ev->timedout) {
        if (ngx_enable_accept_events((ngx_cycle_t *) ngx_cycle) != NGX_OK) {
            return;
        }

        ev->timedout = 0;
    }

	/*获取ngx_event_core_module模块的结构体*/
    ecf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_event_core_module);

    if (!(ngx_event_flags & NGX_USE_KQUEUE_EVENT)) {
        ev->available = ecf->multi_accept;  //available对应的是multi_accept配置项
    }

    /*获取事件对应的连接*/
    lc = ev->data;
    ls = lc->listening;
    ev->ready = 0;  //将事件就绪的标志位清零，因为此次就绪已经正在处理

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "accept on %V, ready: %d", &ls->addr_text, ev->available);

    do {
        socklen = NGX_SOCKADDRLEN;

#if (NGX_HAVE_ACCEPT4)
        if (use_accept4) {
            s = accept4(lc->fd, (struct sockaddr *) sa, &socklen,
                        SOCK_NONBLOCK);
        } else {
            s = accept(lc->fd, (struct sockaddr *) sa, &socklen);
        }
#else
		//lc->fd是监听套接字,s是与客户端通信的套接字
        s = accept(lc->fd, (struct sockaddr *) sa, &socklen);  //与客户端建立连接
#endif

        if (s == (ngx_socket_t) -1) {
            err = ngx_socket_errno;

            if (err == NGX_EAGAIN) {
                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, err,
                               "accept() not ready");
                return;
            }

            level = NGX_LOG_ALERT;

            if (err == NGX_ECONNABORTED) {
                level = NGX_LOG_ERR;

            } else if (err == NGX_EMFILE || err == NGX_ENFILE) {
                level = NGX_LOG_CRIT;
            }

#if (NGX_HAVE_ACCEPT4)
            ngx_log_error(level, ev->log, err,
                          use_accept4 ? "accept4() failed" : "accept() failed");

            if (use_accept4 && err == NGX_ENOSYS) {
                use_accept4 = 0;
                ngx_inherited_nonblocking = 0;
                continue;
            }
#else
            ngx_log_error(level, ev->log, err, "accept() failed");
#endif

            if (err == NGX_ECONNABORTED) {
                if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {
                    ev->available--;
                }

                if (ev->available) {
                    continue;
                }
            }

            if (err == NGX_EMFILE || err == NGX_ENFILE) {
                if (ngx_disable_accept_events((ngx_cycle_t *) ngx_cycle, 1)
                    != NGX_OK)
                {
                    return;
                }

                if (ngx_use_accept_mutex) {
                    if (ngx_accept_mutex_held) {
                        ngx_shmtx_unlock(&ngx_accept_mutex);
                        ngx_accept_mutex_held = 0;
                    }

                    ngx_accept_disabled = 1;

                } else {
                    ngx_add_timer(ev, ecf->accept_mutex_delay);
                }
            }

            return;
        }

#if (NGX_STAT_STUB)
        (void) ngx_atomic_fetch_add(ngx_stat_accepted, 1);
#endif

		/*
		 * ***********初始化负载均衡的值******************
		 * 每来一个新连接事件，负载均衡阈值都会被重新计算，随着时间的推移，新连接事件处理的越来越多，剩余的空闲连接
		 * 数量越来越少，直到ngx_accept_disabled 大于0，这个时候就会开始不接受新连接事件了，即开始负载均衡了
         */
        ngx_accept_disabled = ngx_cycle->connection_n / 8
                              - ngx_cycle->free_connection_n;

		//通过连接句柄s获取连接控制块
        c = ngx_get_connection(s, ev->log);

        if (c == NULL) {
            if (ngx_close_socket(s) == -1) {
                ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_socket_errno,
                              ngx_close_socket_n " failed");
            }

            return;
        }

        /*tcp*/
        c->type = SOCK_STREAM;

#if (NGX_STAT_STUB)
        (void) ngx_atomic_fetch_add(ngx_stat_active, 1);
#endif

		//为这个新建连接创建内存池
        c->pool = ngx_create_pool(ls->pool_size, ev->log);
        if (c->pool == NULL) {
            ngx_close_accepted_connection(c);
            return;
        }

        c->sockaddr = ngx_palloc(c->pool, socklen);
        if (c->sockaddr == NULL) {
            ngx_close_accepted_connection(c);
            return;
        }

        ngx_memcpy(c->sockaddr, sa, socklen);

        log = ngx_palloc(c->pool, sizeof(ngx_log_t));
        if (log == NULL) {
            ngx_close_accepted_connection(c);
            return;
        }

        /* set a blocking mode for iocp and non-blocking mode for others */

        if (ngx_inherited_nonblocking) {
            if (ngx_event_flags & NGX_USE_IOCP_EVENT) {
                if (ngx_blocking(s) == -1) {
                    ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_socket_errno,
                                  ngx_blocking_n " failed");
                    ngx_close_accepted_connection(c);
                    return;
                }
            }

        } else {
            if (!(ngx_event_flags & NGX_USE_IOCP_EVENT)) {
                if (ngx_nonblocking(s) == -1) {   //设置为非阻塞套接字
                    ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_socket_errno,
                                  ngx_nonblocking_n " failed");
                    ngx_close_accepted_connection(c);
                    return;
                }
            }
        }

        /*
         * 获取监听套接口的日志对象，监听套接口的日志对象是在初始化监听套接口时
         * 从配置文件中解析得到的
         */
        *log = ls->log;

        c->recv = ngx_recv;
        c->send = ngx_send;
        c->recv_chain = ngx_recv_chain;
        c->send_chain = ngx_send_chain;

        /* 设置交互套接口连接及连接内存池的日志对象为监听套接口的日志对象 */
        c->log = log;
        c->pool->log = log;

        c->socklen = socklen;
        c->listening = ls;
        /*
         * 从下面的赋值可以看到，当客户端与服务端的连接建立后，是直接把监听对象的地址信息赋值给了
         * 新建立的连接对象中的。如果listen配置命令中的ip是通配符，如listen *:port，那么其中的ip
         * 会以"0.0.0.0"代替，所以在ngx_http_init_connection()中的ngx_connection_local_sockaddr()
         * 中会以判断c->local_sockaddr.sin_addr.s_addr为0来判断是否是未指定ip的情况。
         */
        c->local_sockaddr = ls->sockaddr;
        c->local_socklen = ls->socklen;

        c->unexpected_eof = 1;

#if (NGX_HAVE_UNIX_DOMAIN)
        if (c->sockaddr->sa_family == AF_UNIX) {
            c->tcp_nopush = NGX_TCP_NOPUSH_DISABLED;
            c->tcp_nodelay = NGX_TCP_NODELAY_DISABLED;
#if (NGX_SOLARIS)
            /* Solaris's sendfilev() supports AF_NCA, AF_INET, and AF_INET6 */
            c->sendfile = 0;
#endif
        }
#endif

        rev = c->read;
        wev = c->write;

        /*将写事件就绪，因为这个时候如果有数据可以进行写操作，读事件在上面清零，因为已经在处理了*/
        wev->ready = 1;

        if (ngx_event_flags & NGX_USE_IOCP_EVENT) {
            rev->ready = 1;
        }

        /*
         * deferred_accept标志位为1，表示需要延迟建立tcp连接，即等到接收到客户端发来的实际请求数据
         * 之后才会accept建立连接。deferred_accept标志位最初是在listen配置指令中设置的，即如果listen
         * 命令中配置了deferred_accept，则监听套接口对应的连接对象中的读事件的deferred_accept才会置1，
         * 表示在这个监听套接口建立的socket都要等到有数据之后才会accept。此时会将新建立的连接对应的
         * 读事件的ready标志位置位，表示在连接对应的内核套接字缓冲区已经接收到用户发来的请求数据了。
         */
        if (ev->deferred_accept) {
            rev->ready = 1;
#if (NGX_HAVE_KQUEUE)
            rev->available = 1;
#endif
        }

        rev->log = log;
        wev->log = log;

        /*
         * TODO: MT: - ngx_atomic_fetch_add()
         *             or protection by critical section or light mutex
         *
         * TODO: MP: - allocated in a shared memory
         *           - ngx_atomic_fetch_add()
         *             or protection by critical section or light mutex
         */

        /*
         * 连接使用的次数。ngx_connection_t结构体每次建立一条来自客户端的连接,或者用于主动向后端服务器发起连接时，
         * number都会加1.
         */
        c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);

#if (NGX_STAT_STUB)
        (void) ngx_atomic_fetch_add(ngx_stat_handled, 1);
#endif

        /*将网络地址转换为字符串形式的地址*/
        if (ls->addr_ntop) {
            c->addr_text.data = ngx_pnalloc(c->pool, ls->addr_text_max_len);
            if (c->addr_text.data == NULL) {
                ngx_close_accepted_connection(c);
                return;
            }

            c->addr_text.len = ngx_sock_ntop(c->sockaddr, c->socklen,
                                             c->addr_text.data,
                                             ls->addr_text_max_len, 0);
            if (c->addr_text.len == 0) {
                ngx_close_accepted_connection(c);
                return;
            }
        }

#if (NGX_DEBUG)
        {
        ngx_str_t  addr;
        u_char     text[NGX_SOCKADDR_STRLEN];

        ngx_debug_accepted_connection(ecf, c);

        if (log->log_level & NGX_LOG_DEBUG_EVENT) {
            addr.data = text;
            addr.len = ngx_sock_ntop(c->sockaddr, c->socklen, text,
                                     NGX_SOCKADDR_STRLEN, 1);

            ngx_log_debug3(NGX_LOG_DEBUG_EVENT, log, 0,
                           "*%uA accept: %V fd:%d", c->number, &addr, s);
        }

        }
#endif

		/*将新建立的连接添加到epoll中，这样，在这个连接上如果收到用户请求，epoll_wait就会收集到这个连接的读写事件*/
        /*
         * 这里有一个比较不好理解的地方，那就是将这个连接对应的fd加入到epoll中进行监控，如果有读或写事件发生，按理说
         * 应该调用读或写事件对应的回调方法进行处理，但是这个函数中并没有看到设置读写事件回调函数的地方。那设置读写事件
         * 回调方法的地方在那里呢?我们知道在ngx_listening_t结构体中有一个回调方法，ngx_connection_handler_pt，这个
         * 回调方法调用的时间是tcp连接建立后就会调用(在下面几行就有体现)，其实设置新建立连接读写事件的回调函数就是
         * 在ngx_listening_t->ngx_connection_handler_pt中，如果是http报文，那么这个函数就是ngx_http_init_connection()
         * 在这个函数中会将新建立连接的读事件设置为ngx_http_wait_request_handler()，表示开始处理用户请求;写事件设置为
         * ngx_http_empty_handler()，啥事都不做，因为暂时不回响应，后续会在适当的时候重新设置。
         */
        if (ngx_add_conn && (ngx_event_flags & NGX_USE_EPOLL_EVENT) == 0) {
            if (ngx_add_conn(c) == NGX_ERROR) {
                ngx_close_accepted_connection(c);
                return;
            }
        }

        log->data = NULL;
        log->handler = NULL;

		//当新的tcp连接建立成功后，调用监听端口的回调函数
        ls->handler(c);

        if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {
            ev->available--;
        }

    } while (ev->available);  //如果监听事件的available为1，表示一次性尽量多建立连接
}


#if !(NGX_WIN32)

void
ngx_event_recvmsg(ngx_event_t *ev)
{
    ssize_t            n;
    ngx_log_t         *log;
    ngx_err_t          err;
    ngx_event_t       *rev, *wev;
    struct iovec       iov[1];
    struct msghdr      msg;
    ngx_listening_t   *ls;
    ngx_event_conf_t  *ecf;
    ngx_connection_t  *c, *lc;
    u_char             sa[NGX_SOCKADDRLEN];
    static u_char      buffer[65535];

#if (NGX_HAVE_MSGHDR_MSG_CONTROL)

#if (NGX_HAVE_IP_RECVDSTADDR)
    u_char             msg_control[CMSG_SPACE(sizeof(struct in_addr))];
#elif (NGX_HAVE_IP_PKTINFO)
    u_char             msg_control[CMSG_SPACE(sizeof(struct in_pktinfo))];
#endif

#if (NGX_HAVE_INET6 && NGX_HAVE_IPV6_RECVPKTINFO)
    u_char             msg_control6[CMSG_SPACE(sizeof(struct in6_pktinfo))];
#endif

#endif

    if (ev->timedout) {
        if (ngx_enable_accept_events((ngx_cycle_t *) ngx_cycle) != NGX_OK) {
            return;
        }

        ev->timedout = 0;
    }

    ecf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_event_core_module);

    if (!(ngx_event_flags & NGX_USE_KQUEUE_EVENT)) {
        ev->available = ecf->multi_accept;
    }

    lc = ev->data;
    ls = lc->listening;
    ev->ready = 0;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "recvmsg on %V, ready: %d", &ls->addr_text, ev->available);

    do {
        ngx_memzero(&msg, sizeof(struct msghdr));

        iov[0].iov_base = (void *) buffer;
        iov[0].iov_len = sizeof(buffer);

        msg.msg_name = &sa;
        msg.msg_namelen = sizeof(sa);
        msg.msg_iov = iov;
        msg.msg_iovlen = 1;

#if (NGX_HAVE_MSGHDR_MSG_CONTROL)

        if (ls->wildcard) {

#if (NGX_HAVE_IP_RECVDSTADDR || NGX_HAVE_IP_PKTINFO)
            if (ls->sockaddr->sa_family == AF_INET) {
                msg.msg_control = &msg_control;
                msg.msg_controllen = sizeof(msg_control);
            }
#endif

#if (NGX_HAVE_INET6 && NGX_HAVE_IPV6_RECVPKTINFO)
            if (ls->sockaddr->sa_family == AF_INET6) {
                msg.msg_control = &msg_control6;
                msg.msg_controllen = sizeof(msg_control6);
            }
#endif
        }

#endif

        n = recvmsg(lc->fd, &msg, 0);

        if (n == -1) {
            err = ngx_socket_errno;

            if (err == NGX_EAGAIN) {
                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, err,
                               "recvmsg() not ready");
                return;
            }

            ngx_log_error(NGX_LOG_ALERT, ev->log, err, "recvmsg() failed");

            return;
        }

#if (NGX_STAT_STUB)
        (void) ngx_atomic_fetch_add(ngx_stat_accepted, 1);
#endif

#if (NGX_HAVE_MSGHDR_MSG_CONTROL)
        if (msg.msg_flags & (MSG_TRUNC|MSG_CTRUNC)) {
            ngx_log_error(NGX_LOG_ALERT, ev->log, 0,
                          "recvmsg() truncated data");
            continue;
        }
#endif

        ngx_accept_disabled = ngx_cycle->connection_n / 8
                              - ngx_cycle->free_connection_n;

        c = ngx_get_connection(lc->fd, ev->log);
        if (c == NULL) {
            return;
        }

        c->shared = 1;
        c->type = SOCK_DGRAM;
        c->socklen = msg.msg_namelen;

#if (NGX_STAT_STUB)
        (void) ngx_atomic_fetch_add(ngx_stat_active, 1);
#endif

        c->pool = ngx_create_pool(ls->pool_size, ev->log);
        if (c->pool == NULL) {
            ngx_close_accepted_connection(c);
            return;
        }

        c->sockaddr = ngx_palloc(c->pool, c->socklen);
        if (c->sockaddr == NULL) {
            ngx_close_accepted_connection(c);
            return;
        }

        ngx_memcpy(c->sockaddr, msg.msg_name, c->socklen);

        log = ngx_palloc(c->pool, sizeof(ngx_log_t));
        if (log == NULL) {
            ngx_close_accepted_connection(c);
            return;
        }

        *log = ls->log;

        c->send = ngx_udp_send;

        c->log = log;
        c->pool->log = log;

        c->listening = ls;
        c->local_sockaddr = ls->sockaddr;
        c->local_socklen = ls->socklen;

#if (NGX_HAVE_MSGHDR_MSG_CONTROL)

        if (ls->wildcard) {
            struct cmsghdr   *cmsg;
            struct sockaddr  *sockaddr;

            sockaddr = ngx_palloc(c->pool, c->local_socklen);
            if (sockaddr == NULL) {
                ngx_close_accepted_connection(c);
                return;
            }

            ngx_memcpy(sockaddr, c->local_sockaddr, c->local_socklen);
            c->local_sockaddr = sockaddr;

            for (cmsg = CMSG_FIRSTHDR(&msg);
                 cmsg != NULL;
                 cmsg = CMSG_NXTHDR(&msg, cmsg))
            {

#if (NGX_HAVE_IP_RECVDSTADDR)

                if (cmsg->cmsg_level == IPPROTO_IP
                    && cmsg->cmsg_type == IP_RECVDSTADDR
                    && sockaddr->sa_family == AF_INET)
                {
                    struct in_addr      *addr;
                    struct sockaddr_in  *sin;

                    addr = (struct in_addr *) CMSG_DATA(cmsg);
                    sin = (struct sockaddr_in *) sockaddr;
                    sin->sin_addr = *addr;

                    break;
                }

#elif (NGX_HAVE_IP_PKTINFO)

                if (cmsg->cmsg_level == IPPROTO_IP
                    && cmsg->cmsg_type == IP_PKTINFO
                    && sockaddr->sa_family == AF_INET)
                {
                    struct in_pktinfo   *pkt;
                    struct sockaddr_in  *sin;

                    pkt = (struct in_pktinfo *) CMSG_DATA(cmsg);
                    sin = (struct sockaddr_in *) sockaddr;
                    sin->sin_addr = pkt->ipi_addr;

                    break;
                }

#endif

#if (NGX_HAVE_INET6 && NGX_HAVE_IPV6_RECVPKTINFO)

                if (cmsg->cmsg_level == IPPROTO_IPV6
                    && cmsg->cmsg_type == IPV6_PKTINFO
                    && sockaddr->sa_family == AF_INET6)
                {
                    struct in6_pktinfo   *pkt6;
                    struct sockaddr_in6  *sin6;

                    pkt6 = (struct in6_pktinfo *) CMSG_DATA(cmsg);
                    sin6 = (struct sockaddr_in6 *) sockaddr;
                    sin6->sin6_addr = pkt6->ipi6_addr;

                    break;
                }

#endif

            }
        }

#endif

        c->buffer = ngx_create_temp_buf(c->pool, n);
        if (c->buffer == NULL) {
            ngx_close_accepted_connection(c);
            return;
        }

        c->buffer->last = ngx_cpymem(c->buffer->last, buffer, n);

        rev = c->read;
        wev = c->write;

        wev->ready = 1;

        rev->log = log;
        wev->log = log;

        /*
         * TODO: MT: - ngx_atomic_fetch_add()
         *             or protection by critical section or light mutex
         *
         * TODO: MP: - allocated in a shared memory
         *           - ngx_atomic_fetch_add()
         *             or protection by critical section or light mutex
         */

        c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);

#if (NGX_STAT_STUB)
        (void) ngx_atomic_fetch_add(ngx_stat_handled, 1);
#endif

        if (ls->addr_ntop) {
            c->addr_text.data = ngx_pnalloc(c->pool, ls->addr_text_max_len);
            if (c->addr_text.data == NULL) {
                ngx_close_accepted_connection(c);
                return;
            }

            c->addr_text.len = ngx_sock_ntop(c->sockaddr, c->socklen,
                                             c->addr_text.data,
                                             ls->addr_text_max_len, 0);
            if (c->addr_text.len == 0) {
                ngx_close_accepted_connection(c);
                return;
            }
        }

#if (NGX_DEBUG)
        {
        ngx_str_t  addr;
        u_char     text[NGX_SOCKADDR_STRLEN];

        ngx_debug_accepted_connection(ecf, c);

        if (log->log_level & NGX_LOG_DEBUG_EVENT) {
            addr.data = text;
            addr.len = ngx_sock_ntop(c->sockaddr, c->socklen, text,
                                     NGX_SOCKADDR_STRLEN, 1);

            ngx_log_debug4(NGX_LOG_DEBUG_EVENT, log, 0,
                           "*%uA recvmsg: %V fd:%d n:%z",
                           c->number, &addr, c->fd, n);
        }

        }
#endif

        log->data = NULL;
        log->handler = NULL;

        ls->handler(c);

        if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {
            ev->available -= n;
        }

    } while (ev->available);
}

#endif


ngx_int_t
ngx_trylock_accept_mutex(ngx_cycle_t *cycle)
{
	/* 获取同步锁，如果返回1表明获取到了锁 */
    if (ngx_shmtx_trylock(&ngx_accept_mutex)) {

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                       "accept mutex locked");

		/* 之前已经获取到了锁，直接返回 */
        if (ngx_accept_mutex_held && ngx_accept_events == 0) {
            return NGX_OK;
        }

		/* 将所有监听端口的连接的读事件都加入到事件驱动模块中 */
        if (ngx_enable_accept_events(cycle) == NGX_ERROR) {
            ngx_shmtx_unlock(&ngx_accept_mutex);
            return NGX_ERROR;
        }

		/* 获取到锁后，置标志位ngx_accept_mutex_held为1 */
        ngx_accept_events = 0;
        ngx_accept_mutex_held = 1;

        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "accept mutex lock failed: %ui", ngx_accept_mutex_held);

	/*
	 * ngx_shmtx_trylock返回0表明获取锁失败，但ngx_accept_mutex_held为1表明进程在上轮争锁时
	 * 获取到了锁，但是本轮并没有抢到锁，所以需要将所有监听套接口从进程监控机制中移除，即从
	 * epoll中移除，通过调用ngx_disable_accept_events()实现。
     */
	
    if (ngx_accept_mutex_held) {
        if (ngx_disable_accept_events(cycle, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }

        ngx_accept_mutex_held = 0;  // 清空获取到锁的标志位
    }

    return NGX_OK;
}

/*使能所有监听端口连接的读事件*/
static ngx_int_t
ngx_enable_accept_events(ngx_cycle_t *cycle)
{
    ngx_uint_t         i;
    ngx_listening_t   *ls;
    ngx_connection_t  *c;

    ls = cycle->listening.elts;
    for (i = 0; i < cycle->listening.nelts; i++) {

        c = ls[i].connection;

        /*c->read->active为1表明读事件已经添加到事件驱动模块中了*/
        if (c == NULL || c->read->active) {
            continue;
        }

        /*将监听端口连接的读事件添加到事件驱动模块中*/
        if (ngx_add_event(c->read, NGX_READ_EVENT, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

/*移除所有监听端口连接的读事件*/
static ngx_int_t
ngx_disable_accept_events(ngx_cycle_t *cycle, ngx_uint_t all)
{
    ngx_uint_t         i;
    ngx_listening_t   *ls;
    ngx_connection_t  *c;

    ls = cycle->listening.elts;
    for (i = 0; i < cycle->listening.nelts; i++) {

        c = ls[i].connection;

        if (c == NULL || !c->read->active) {
            continue;
        }

#if (NGX_HAVE_REUSEPORT)

        /*
         * do not disable accept on worker's own sockets
         * when disabling accept events due to accept mutex
         */

        if (ls[i].reuseport && !all) {
            continue;
        }

#endif

        /*移除监听端口连接的读事件*/
        if (ngx_del_event(c->read, NGX_READ_EVENT, NGX_DISABLE_EVENT)
            == NGX_ERROR)
        {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static void
ngx_close_accepted_connection(ngx_connection_t *c)
{
    ngx_socket_t  fd;

    ngx_free_connection(c);

    fd = c->fd;
    c->fd = (ngx_socket_t) -1;

    if (!c->shared && ngx_close_socket(fd) == -1) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_socket_errno,
                      ngx_close_socket_n " failed");
    }

    if (c->pool) {
        ngx_destroy_pool(c->pool);
    }

#if (NGX_STAT_STUB)
    (void) ngx_atomic_fetch_add(ngx_stat_active, -1);
#endif
}


u_char *
ngx_accept_log_error(ngx_log_t *log, u_char *buf, size_t len)
{
    return ngx_snprintf(buf, len, " while accepting new connection on %V",
                        log->data);
}


#if (NGX_DEBUG)

static void
ngx_debug_accepted_connection(ngx_event_conf_t *ecf, ngx_connection_t *c)
{
    struct sockaddr_in   *sin;
    ngx_cidr_t           *cidr;
    ngx_uint_t            i;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6  *sin6;
    ngx_uint_t            n;
#endif

    cidr = ecf->debug_connection.elts;
    for (i = 0; i < ecf->debug_connection.nelts; i++) {
        if (cidr[i].family != (ngx_uint_t) c->sockaddr->sa_family) {
            goto next;
        }

        switch (cidr[i].family) {

#if (NGX_HAVE_INET6)
        case AF_INET6:
            sin6 = (struct sockaddr_in6 *) c->sockaddr;
            for (n = 0; n < 16; n++) {
                if ((sin6->sin6_addr.s6_addr[n]
                    & cidr[i].u.in6.mask.s6_addr[n])
                    != cidr[i].u.in6.addr.s6_addr[n])
                {
                    goto next;
                }
            }
            break;
#endif

#if (NGX_HAVE_UNIX_DOMAIN)
        case AF_UNIX:
            break;
#endif

        default: /* AF_INET */
            sin = (struct sockaddr_in *) c->sockaddr;
            if ((sin->sin_addr.s_addr & cidr[i].u.in.mask)
                != cidr[i].u.in.addr)
            {
                goto next;
            }
            break;
        }

        c->log->log_level = NGX_LOG_DEBUG_CONNECTION|NGX_LOG_DEBUG_ALL;
        break;

    next:
        continue;
    }
}

#endif
