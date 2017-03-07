/*
 * A tiny HTTPv1.0.3 server.
 *
 * Copyright (C) CZW. 2016
 * maintainer awenchen(czw8528@sina.com)
 *
 * It just likes a file transmission server in HTTP.
 * You could get a file through 'GET' request, or put a file by 'PUT/POST' request.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "list.h"

/*
 * The definition of macro "CONFIG_SCHE_RR" means that schedule policy is round-robin.
 */
#ifdef CONFIG_SCHE_RR
    #define EPOLL_SCHE 0
#else
    #define EPOLL_SCHE EPOLLET
#endif


#define MAX_LISTEN_NUM    1000
#define MAX_URI_LEN       256
#define MAX_BUF_LEN       (10*1024+1)
#define MAX_CLI_LEN       32
#define MAX_ERRCODE_LEN   64

#define METHOD_LEN        16
#define URI_LEN           256
#define KEY_LEN           64
#define VALUE_LEN         256


#define INIT_EVENTS_NUM   8
#define EPOLL_HINTS       32000
#define EPOLL_TIMEOUT     (10*1000)


enum METHOD
{
    GET = 0,
    PUT,
    POST,
    HEAD,
};

enum PHASE
{
    HTTP_HEAD_READ = 0,
    HTTP_HEAD_RESP,
    HTTP_BODY,
    HTTP_END,
};

typedef unsigned long long ull;

typedef struct epoll_base
{
    int epfd;       /* epoll base fd */
    
    int evreg;      /* register events num */
    int evact;      /* current active events num */
    int evcap;      /* epoll base events capacity */
    
    int timeout;    /* dispatch timeout, millisecond */
    int terminal;   /* the flag to terminal the loop */
    
    struct epoll_event *evptr; /* current active events */

    struct list_head evlist;   /* all register event buffer */
    
}epoll_base_t;

typedef struct http_session
{
    int fd;
    char method;
    char phase;
    char quit;
    ull contentlen;
    ull handledlen;
    char uri[MAX_URI_LEN];
}http_session_t;

typedef struct http_status
{
    int code;
    char str[MAX_ERRCODE_LEN];
}http_status_t;

typedef struct buffer
{
    unsigned int total;  /* buffer total len */
    char *start;
    char *end;
    char *buf;
}buffer_t;

typedef struct event_buffer
{
    epoll_base_t *base;
    struct list_head list;

    int fd;
    struct epoll_event event;
    char clientinfo[MAX_CLI_LEN];
    
    int (*cb)(struct event_buffer *);
    
    buffer_t evbuf;

    void *arg;
}event_buffer_t;

typedef int (*epoll_event_cb)(event_buffer_t *);

http_status_t http_code[] = 
{
    { 200, "Ok" },
    { 201, "Created" },
    { 400, "Bad reuqest" },
    { 404, "Not found" },
    { 500, "Internal error" },
    { 501, "Not Implemented" },
};

static event_buffer_t *epoll_event_buffer_new(epoll_base_t *base, int fd, int events, epoll_event_cb cb, void *arg);
static void epoll_event_buffer_free(event_buffer_t *ev);

static epoll_base_t *epoll_base_new(void);
static int epoll_base_init(epoll_base_t *base);
static void epoll_base_dispatch(epoll_base_t *base);
static void epoll_base_free(epoll_base_t *base);

static int epoll_event_add(epoll_base_t *base, event_buffer_t *ev);
static int epoll_event_mod(epoll_base_t *base, event_buffer_t *ev);
static int epoll_event_del(epoll_base_t *base, event_buffer_t *ev);

static void epoll_events_handle(struct epoll_event *events, int evnum);
static void epoll_events_print(epoll_base_t *base);
static void epoll_events_increase(epoll_base_t *base);

static int buffer_new(buffer_t *evbuf, int buflen);
static void buffer_free(buffer_t *evbuf);
static int buffer_get_line(int sock, buffer_t *evbuf);

static void copy_string(const char *start, const char *end, char *dst);
static int nonblocking_recv(int fd, buffer_t *ev);
static int nonblocking_send(int fd, buffer_t *ev);
static int nonblocking_accept(int fd, struct sockaddr * addr, socklen_t *len);
static void copy_string(const char *start, const char *end, char *dst);
static int parse_key_and_value(const char *line, char *key, char *value);

static void parse_request_info(const char *buf, char *method, char *uri);
static int http_read_request(event_buffer_t *ev);
static int http_try_send(event_buffer_t *ev);
static void http_encapsulate_resp(buffer_t *evbuf, int code, const char *extra);
static int http_read_header(event_buffer_t *ev);
static int http_dispatch(event_buffer_t *ev);
static int http_get_handler(event_buffer_t *ev);
static int http_put_handler(event_buffer_t *ev);
static int http_accept_request(event_buffer_t *ev);

static int serv_start_up(int *port);
static void install_signal_handler(void);

#define buffer_clear(evbuf)  (evbuf->start = evbuf->end = evbuf->buf)
#define buffer_rest(evbuf)   (evbuf->buf + evbuf->total - evbuf->end)
#define buffer_empty(evbuf)  (evbuf->start == evbuf->end)
#define buffer_occupy(evbuf) (evbuf->end - evbuf->start)
#define buffer_shift(evbuf) \
    if( evbuf->start != evbuf->buf ) \
    { \
        copy_string(evbuf->start, evbuf->end - 1, evbuf->buf); \
        evbuf->end -= evbuf->start - evbuf->buf; \
        evbuf->start = evbuf->buf; \
    }

/* 
 * set socket non-blocking mode
 */
#define set_socket_nonblocking(fd) \
    set_socket_attribute(fd, O_NONBLOCK)

/*
 * set socket close on exec
 */
#define set_socket_closeonexec(fd) \
    set_socket_attribute(fd, FD_CLOEXEC)

static char *get_http_code(int code)
{
    int loop = 0;
    int size = sizeof(http_code)/sizeof(http_status_t);

    for( ; loop < size; ++loop )
    {
        if( http_code[loop].code == code )
            return http_code[loop].str;
    }

    return "General error";
}


static int set_socket_attribute(int fd, int attr)
{
    int flags = 0;
    
    if( (flags = fcntl(fd, F_GETFL, NULL)) < 0 )
    {
        perror("fcntl F_GETFL");
        return -1;
    }
    
    if( fcntl(fd, F_SETFL, flags | attr) < 0 )
    {
        perror("fcntl F_SETFL");
        return -1;
    }

    return 0;
}

static int set_socket_nonlinger(int fd)
{
    struct linger linger;
    
    linger.l_onoff = 1;
    linger.l_linger = 0;
    if( setsockopt(fd, SOL_SOCKET, SO_LINGER, (void *)&linger, sizeof(linger)) < 0 )
    {
        perror("setsockopt SO_LINGER");
        return -1;
    }

    return 0;
}

static event_buffer_t *epoll_event_buffer_new(epoll_base_t *base, int fd, int events, epoll_event_cb cb, void *arg)
{
    event_buffer_t *ev = (event_buffer_t *)malloc(sizeof(event_buffer_t));
    if( !ev )
    {
        perror("malloc");
        return NULL;
    }
    memset(ev, 0, sizeof(event_buffer_t));

    ev->base = base;
    ev->cb = cb;
    ev->fd = fd;
    ev->event.events = events;
    ev->event.data.ptr = ev;
    ev->arg = arg;

    return ev;
}

static void epoll_event_buffer_free(event_buffer_t *ev)
{
    http_session_t *sess = NULL;
    if( ev )
    {
        //printf("[%d] %s\n", ev->fd, __FUNCTION__);
        if( ev->fd >= 0 )
            close(ev->fd);
        
        if( ev->arg )
        {
            sess = (http_session_t *)ev->arg;
            if( sess->fd > 0 )
                close(sess->fd);
            free(sess);
        }
        
        list_del(&ev->list);

        buffer_free(&ev->evbuf);

        free(ev);
    }
}

static void epoll_event_buffer_show(event_buffer_t *ev)
{
    http_session_t *sess = NULL;
    if( ev )
    {
        printf("fd = [%d]\n", ev->fd);
        
        if( ev->arg )
        {
            printf("http session info:\n");
            
            sess = (http_session_t *)ev->arg;
            printf("\tfd = [%d]\n\tContent-Length = [%llu]\n\tMethod = [%d]\n\tURI = [%s]\n",
                sess->fd, sess->contentlen, sess->method, sess->uri);
        }
    }
}

/*
 * epoll base implementations
 */
static epoll_base_t *epoll_base_new()
{
    epoll_base_t *base = malloc(sizeof(epoll_base_t));
    if( !base )
    {
        perror("malloc");
        return NULL;
    }

    if( epoll_base_init(base) < 0 )
    {
        free(base);
        return NULL;
    }

    return base;
}

static int epoll_base_init(epoll_base_t *base)
{
    memset(base, 0, sizeof(epoll_base_t));
    
    if( (base->epfd = epoll_create(EPOLL_HINTS)) < 0 )
    {
        perror("epoll_create");
        return -1;
    }

    base->evptr = calloc(INIT_EVENTS_NUM, sizeof(struct epoll_event));
    if( !base->evptr )
    {
        perror("calloc");
        close(base->epfd);
        base->epfd = -1;
        return -1;
    }

    set_socket_closeonexec(base->epfd);

    base->timeout = EPOLL_TIMEOUT;

    INIT_LIST_HEAD(&base->evlist);

    base->evcap = INIT_EVENTS_NUM;
    return 0;
}

static void epoll_base_dispatch(epoll_base_t *base)
{
    int nfds = 0;
    
    while( 1 )
    {
        nfds = epoll_wait(base->epfd, base->evptr, base->evcap, base->timeout);
        if( nfds > 0 )
        {
            base->evact = nfds;

            //printf("active events [%d], register events [%d]\n", base->evact, base->evreg);

            epoll_events_handle(base->evptr, base->evact);

            epoll_events_increase(base);
            
        }
        else if( nfds == -1 && errno != EINTR )
        {
            perror("epoll_wait");
            break;
        }

        /* loop terminal */
        if( base->terminal || !base->evreg )
            break;
    }
}

static void epoll_base_dispatch_once(epoll_base_t *base)
{
    base->terminal = 1;
    epoll_base_dispatch(base);
}

static void epoll_base_free(epoll_base_t *base)
{
    event_buffer_t *ev = NULL;
    struct list_head *pos  = NULL;
    struct list_head *next = NULL;

    if( !base )
        return;
    if( base->epfd >= 0 )
        close(base->epfd);

    if( base->evptr )
        free(base->evptr);

    list_for_each_safe(pos, next, &base->evlist)
    {
        ev = list_entry(pos, event_buffer_t, list);
        epoll_event_buffer_free(ev);
    }

    free(base);
}

/*
 * epoll event implementations
 */
static int epoll_event_add(epoll_base_t *base, event_buffer_t *ev)
{
    if( epoll_ctl(base->epfd, EPOLL_CTL_ADD, ev->fd, &ev->event) < 0 )
    {
        if( errno == EEXIST )
        {
            if( epoll_event_mod(base, ev) < 0 )
            {
                return -1;
            }

            return 0;
        }
        else
        {
            perror("epoll_ctl add");
            return -1;
        }
    }
    
    list_add(&ev->list, &base->evlist);
    ++base->evreg;

    return 0;
}

static int epoll_event_mod(epoll_base_t *base, event_buffer_t *ev)
{
    if( epoll_ctl(base->epfd, EPOLL_CTL_MOD, ev->fd, &ev->event) < 0 )
    {
        perror("epoll_ctl mod");
        return -1;
    }

    return 0;
}

static int epoll_event_del(epoll_base_t *base, event_buffer_t *ev)
{
    if( epoll_ctl(base->epfd, EPOLL_CTL_DEL, ev->fd, &ev->event) < 0 )
    {
        perror("epoll_ctl del");
        return -1;
    }

    epoll_event_buffer_free(ev);
    --base->evreg;

    return 0;
}

static void epoll_events_handle(struct epoll_event *events, int evnum)
{
    int loop = 0;
    epoll_base_t *base = NULL;
    event_buffer_t *ev = NULL;

    for( loop = 0; loop < evnum; ++loop )
    {
        ev = (event_buffer_t *)events[loop].data.ptr;
        base = ev->base;
        if( !ev )
            continue;
        
        if( !base )
            continue;

        if( events[loop].events & EPOLLERR ||
            events[loop].events & EPOLLHUP )
        {
            printf("There is something wrong with fd = [%d], close it.\n", ev->fd);

            /* remove it */
            epoll_event_del(base, ev);
            continue;
        }

        //printf("[%d] %s active\n", ev->fd, events[loop].events & EPOLLIN ? "EPOLLIN" : "EPOLLOUT");

        //epoll_events_print(base);

        if( ev->cb )
        {
            ev->event.events = events[loop].events;
            if( ev->cb(ev) < 0 )
            {
                epoll_event_del(base, ev);
            }
        }
    }
}

static void epoll_events_print(epoll_base_t *base)
{
    event_buffer_t *ev = NULL;
    struct list_head *pos  = NULL;
    struct list_head *next = NULL;

    if( !base )
        return;
    printf("\n\n[%s]\n", __FUNCTION__);
    list_for_each_safe(pos, next, &base->evlist)
    {
        ev = list_entry(pos, event_buffer_t, list);
        epoll_event_buffer_show(ev);
    }
    printf("[%s]\n\n", __FUNCTION__);
}

static void epoll_events_increase(epoll_base_t *base)
{
    if( base->evact == base->evcap ) /* double the capacity */
    {
        struct epoll_event *ptr = NULL;

        ptr = (struct epoll_event*)realloc(base->evptr, sizeof(struct epoll_event) * base->evcap * 2);
        if( ptr )
        {
            base->evptr = ptr;
            base->evcap *= 2;
        }
        else /* a non-fatal error */
        {
            perror("realloc");
        }
    }
}

static int buffer_new(buffer_t *evbuf, int buflen)
{
    evbuf->buf = malloc(buflen);
    if( !evbuf->buf )
    {
        perror("malloc");
        return -1;
    }

    evbuf->total = buflen - 1; /* usable length */
    buffer_clear(evbuf);
    
    return 0;
}

static void buffer_free(buffer_t *evbuf)
{
    if( evbuf->buf )
    {
        free(evbuf->buf);
    }
    evbuf->start = evbuf->end = evbuf->buf = NULL;
}

static void copy_string(const char *start, const char *end, char *dst)
{
    while( *start && start <= end )
        *dst++ = *start++;

    *dst = '\0'; /* terminal char */
}

/*
 * parse key and value from a string line "  key  :  value  "
 * we must strip the space in front/end of the key/value
 */
static int parse_key_and_value(const char *line, char *key, char *value)
{
    const char *pks = NULL; /* key start pointer   */
    const char *pke = NULL; /* key end pointer     */
    const char *pvs = NULL; /* value start pointer */
    const char *pve = NULL; /* value end pointer   */

    pks = line;
    while( isspace(*pks) ) ++pks;

    pvs = strchr(pks, ':');
    if( !pvs )
    {
        return -1;
    }
    pke = pvs - 1;
    ++pvs;

    while( isspace(*pvs) ) ++pvs;
    while( isspace(*pke) ) --pke;

    pve = &line[strlen(line) - 1];
    while( isspace(*pve) ) --pve;

    if( pke - pks >= KEY_LEN )
    {
        printf("key length beyond limit!\n");
        return -1;
    }
    
    if( pve - pvs >= VALUE_LEN )
    {
        printf("value length beyond limit!\n");
        return -1;
    }
    
    copy_string(pks, pke, key);
    copy_string(pvs, pve, value);

    return 0;
}

/*
 * try to get a line of HTTP header with the end of "\r\n"
 * and conserve the line without string "\r\n".
 *
 * return value: 0, Ok, get a new line,
 *              >0, Not bad, get something but not a complete line, return the received length
 *              -1, Oops, something wrong with the socket
 */
static int buffer_get_line(int sock, buffer_t *evbuf)
{
    int i = 0;
    int n = 0;
    int rest = buffer_rest(evbuf);
    char c = '\0';

    while( i < rest )
    {
        n = recv(sock, &c, 1, MSG_DONTWAIT);
        if( n > 0 )
        {
            if( c == '\r' )
            {
                do
                {
                    n = recv(sock, &c, 1, MSG_DONTWAIT | MSG_PEEK);
                } while( n < 0 && errno == EINTR );
                
                if( n > 0 )
                {
                    if( c == '\n' )
                    {
                        recv(sock, &c, 1, 0);
                        *evbuf->end = '\0';
                        return 0;
                    }
                }
                else if( errno == EAGAIN )
                {
                    return i+1;
                }
                else
                {
                    perror("recv");
                    return -1;
                }
            }
            else
            {
                *evbuf->end++ = c;
                ++i;
            }
        }
        else if( errno == EAGAIN )
        {
            return i+1;
        }
        else if( errno == EINTR )
        {
            continue;
        }
        else
        {
            perror("recv");
            return -1;
        }
    }
    /*
     * If the program comes here, it means that the buffer rest is less than a line from server.
     * However, it is difficult to predict the size of a line, so we just treat it as an 
     * non-fatal error, and trancates it, then the caller would ignore it when failed to parse.
     */
    
    printf("buffer is full, handle it first!\n");
    
    *evbuf->end = '\0';
    return 0;
}

static void parse_request_info(const char *buf, char *method, char *uri)
{
    int i = 0;
    int j = 0;
    
    /* get method */
    while( buf[j] && !isspace(buf[j]) && i < METHOD_LEN-1 )
    {
        method[i] = buf[j];
        ++i;
        ++j;
    }
    method[i] = '\0';

    /* get uri */
    i = 0;
    while( buf[j] && isspace(buf[j]) ) ++j;
    
    while( buf[j] && !isspace(buf[j]) && i < URI_LEN-1 )
    {
        uri[i] = buf[j];
        ++i;
        ++j;
    }
    uri[i] = '\0';
}

/*
 * try to receive something from socket
 * return value: >0, Ok, it's the length we have received,
 *                0, Not bad, nothing have been received, wait for next active event,
 *               -1, Oops, somethind wrong with the socket, shutdown it.
 */
static int nonblocking_recv(int fd, buffer_t *evbuf)
{
    int rest = 0;
    int recvlen = 0;
    int totalrecv = 0;

    rest = buffer_rest(evbuf);
    while( rest > 0 )
    {
        recvlen = recv(fd, evbuf->end, rest, MSG_DONTWAIT);
        if( recvlen > 0 )
        {
            rest       -= recvlen;
            totalrecv  += recvlen;
            evbuf->end += recvlen;
        }
        else if( recvlen == -1 && errno == EAGAIN ) /* wait for next time active */
        {
            break;
        }
        else if( recvlen == -1 && errno == EINTR ) /* continue */
        {
            continue;
        }
        else
        {
            printf("recv [%d] failed: %s\n", fd, strerror(errno));
            return -1;
        }
    }

    *evbuf->end = '\0'; /* terminal char */
    
    return totalrecv;
}


/*
 * try to send something by socket
 * return value: >0, Ok, it's the length we have sent,
 *                0, Not bad, nothing have been sent, wait for next active event,
 *               -1, Oops, somethind wrong with the socket, shutdown it.
 */
static int nonblocking_send(int fd, buffer_t *evbuf)
{
    int occupy = 0;
    int sendlen = 0;
    int totalsend = 0;

    occupy = buffer_occupy(evbuf);
    while( occupy > 0 )
    {
        sendlen = send(fd, evbuf->start, occupy, MSG_DONTWAIT);
        if( sendlen > 0 )
        {
            occupy       -= sendlen;
            totalsend    += sendlen;
            evbuf->start += sendlen;
        }
        else if( sendlen == -1 && errno == EAGAIN ) /* wait for next time active */
        {
            break;
        }
        else if( sendlen == -1 && errno == EINTR ) /* continue */
        {
            continue;
        }
        else
        {
            printf("send [%d] failed: %s\n", fd, strerror(errno));
            return -1;
        }
    }

    if( !occupy ) /* nothing remained */
    {
        buffer_clear(evbuf);
    }
    else /* remain something */
    {
        /*
         * This is not a good method to move the data in buffer everytime,
         * but I have not got a good idea to abstract a "buffer".
         */
        buffer_shift(evbuf);
    }

    return totalsend;
}


/*
 * try to accept a TCP request
 * return value: >0, Ok, it's the new socket from client,
 *                0, Not bad, just a illusion, wait for next come in,
 *               -1, Oops, somethind wrong with the socket, shutdown it.
 */
static int nonblocking_accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    int acceptfd = 0;
    
    acceptfd = accept(fd, addr, len);
    if( acceptfd == -1 )
    {
        /* non-fatal error */
        if( errno == EAGAIN ||
            errno == EINTR  ||
            errno == ECONNABORTED ||
            errno == EPROTO )
        {
            return 0;
        }
        else
        {
            perror("accept");
            return -1;
        }
    }

    set_socket_nonblocking(acceptfd);

    return acceptfd;
}

static int http_accept_request(event_buffer_t *ev)
{
    int fd = ev->fd;
    int acceptfd = 0;
    char client[MAX_CLI_LEN] = {0};
    http_session_t *sess = NULL;
    event_buffer_t *evnew = NULL;
    struct sockaddr_in clientaddr;
    socklen_t clientlen = sizeof(clientaddr);
    
    acceptfd = nonblocking_accept(fd, (struct sockaddr *)&clientaddr, &clientlen);
    if( acceptfd <= 0 )
    {
        return acceptfd;
    }

    /* record the client info */
    snprintf(client, MAX_CLI_LEN, "[%s:%d]", inet_ntoa(clientaddr.sin_addr), ntohs(clientaddr.sin_port));
    printf("recv a connection request %s.\n", client);

    sess = malloc(sizeof(http_session_t));
    if( !sess )
    {
        perror("malloc");
        return 0;
    }
    memset(sess, 0, sizeof(http_session_t));

    evnew = epoll_event_buffer_new(ev->base, acceptfd, EPOLLIN|EPOLL_SCHE, http_dispatch, sess);
    if( !evnew )
    {
        free(sess);
        return 0;
    }

    if( buffer_new(&evnew->evbuf, MAX_BUF_LEN) < 0 )
    {
        free(evnew);
        free(sess);
        return 0;
    }

    if( epoll_event_add(ev->base, evnew) < 0 )
    {
        buffer_free(&evnew->evbuf);
        free(evnew);
        free(sess);
    }

    strcpy(evnew->clientinfo, client);
    //printf("add event ok\n");
    
    return 0;
}

static int http_read_request(event_buffer_t *ev)
{
    int ret = 0;
    char method[METHOD_LEN] = {0};
    char uri[URI_LEN] = {0};
    struct stat fst;
    buffer_t *evbuf = &ev->evbuf;
    http_session_t *sess = (http_session_t *)ev->arg;

    ret = buffer_get_line(ev->fd, evbuf);
    if( !ret )
    {
        parse_request_info(evbuf->start, method, uri);

        buffer_clear(evbuf);

        if( !strcasecmp(method, "GET") )
        {
            sess->method = GET;

            if( access(uri+1, F_OK | R_OK) < 0 )
            {
                return -404;
            }
            
            if( stat(uri+1, &fst) < 0 )
            {
                return -500;
            }

            if( !S_ISREG(fst.st_mode) )
            {
                return -400;
            }

            sess->contentlen = fst.st_size;
        }
        else if( !strcasecmp(method, "PUT") )
        {
            sess->method = PUT;
        }
        else if( !strcasecmp(method, "POST") )
        {
            sess->method = POST;
        }
        else if( !strcasecmp(method, "HEAD") )
        {
            sess->method = HEAD;
        }
        else
        {
            printf("Do not support this method: %s\n", method);
            return -501;
        }

        strcpy(sess->uri, uri+1); /* filter first '/' */
    }

    return ret;
}

static void http_encapsulate_resp(buffer_t *evbuf, int code, const char *extra)
{
    char errstr[1024] = {0};

    if( !extra )
        snprintf(errstr, 1024, "HTTP/1.1 %d %s\r\n\r\n", code, get_http_code(code));
    else
        snprintf(errstr, 1024, "HTTP/1.1 %d %s\r\n%s\r\n", code, get_http_code(code), extra);


    strcpy(evbuf->end, errstr);
    evbuf->end += strlen(errstr);
}

static int http_read_header(event_buffer_t *ev)
{
    int ret = 0;
    char key[KEY_LEN] = {0};
    char value[VALUE_LEN] = {0};
    buffer_t *evbuf = &ev->evbuf;
    http_session_t *sess = (http_session_t *)ev->arg;

    if( !strlen(sess->uri) )
    {
        ret = http_read_request(ev);
        if( ret != 0 )
            return ret;
    }

    while( 1 )
    {
        ret = buffer_get_line(ev->fd, evbuf);
        if( ret != 0 )
        {
            return ret;
        }
        
        if( buffer_empty(evbuf) ) /* read header over */
        {
            switch( sess->method )
            {
                case GET:
                    snprintf(value, VALUE_LEN, "Content-Length: %llu\r\n", sess->contentlen);
                    http_encapsulate_resp(evbuf, 200, value);
                    break;

                case PUT:
                default:
                    http_encapsulate_resp(evbuf, 201, NULL);
                    break;
            }

            ret = http_try_send(ev);
            if( !ret )
            {
                sess->phase = HTTP_BODY;
                
                if( sess->method == GET )
                {
                    ev->event.events = EPOLLOUT | EPOLL_SCHE;
                    return epoll_event_mod(ev->base, ev);
                }

                return 0;
            }
            else if( ret > 0 )
            {
                sess->phase = HTTP_HEAD_RESP;
            }

            return ret;
        }
        
        memset(key, 0, sizeof(key));
        memset(value, 0, sizeof(value));
        
        if( !parse_key_and_value(evbuf->start, key, value) )
        {
            if( !strcasecmp(key, "Content-Length") )
            {
                sess->contentlen = strtoull(value, NULL, 10);
                printf("Content-Length: %llu\n", sess->contentlen);
            }
        }

        buffer_clear(evbuf);
    }
}

/*
 * handle http "GET" request
 */
static int http_get_handler(event_buffer_t *ev)
{
    int rest = 0;
    int rdlen = 0;
    int sendlen = 0;
    
    buffer_t *evbuf = &ev->evbuf;
    http_session_t *sess = (http_session_t *)ev->arg;
    
    if( sess->fd == 0 )
    {
        sess->fd = open(sess->uri, O_RDONLY);
        if( sess->fd < 0 )
        {
            perror("open");
            return -500;
        }
    }

#ifndef CONFIG_SCHE_RR
    while( 1 )
    {
#endif
        rest = buffer_rest(evbuf);
        if( rest )
        {
READ_AGAIN:
            rdlen = read(sess->fd, evbuf->end, rest);
            if( rdlen < 0 )
            {
                if( errno == EAGAIN )
                {
                    return 0;
                }
                else if( errno == EINTR )
                {
                    goto READ_AGAIN;
                }
                else
                {
                    perror("read");
                    return -1;
                }
            }
            
            evbuf->end += rdlen;
        }

        sendlen = nonblocking_send(ev->fd, evbuf);
        if( sendlen < 0 )
        {
            return -1;
        }
        else
        {
            sess->handledlen += sendlen;
            if( sess->handledlen == sess->contentlen )
            {
                printf("read [%s] over\n", sess->uri);
                return -1; /* it's over, shutdown */
            }
        }
#ifndef CONFIG_SCHE_RR
    }
#endif

    return 0;
}

/*
 * handle http "PUT" request
 */
static int http_put_handler(event_buffer_t *ev)
{
    int wrlen = 0;
    int occupy = 0;
    int recvlen = 0;
    
    buffer_t *evbuf = &ev->evbuf;
    http_session_t *sess = (http_session_t *)ev->arg;
    
    if( sess->fd == 0 )
    {
        sess->fd = creat(sess->uri, 0644);
        if( sess->fd < 0 )
        {
            perror("creat");
            return -500;
        }
    }
#ifndef CONFIG_SCHE_RR
    while( 1 )
    {
#endif
        recvlen = nonblocking_recv(ev->fd, evbuf);
        if( recvlen < 0 )
        {
            return -1;
        }

        occupy = buffer_occupy(evbuf);
        if( !occupy )
        {
            return 0;
        }
        
WRITE_AGAIN:
        wrlen = write(sess->fd, evbuf->start, occupy);
        if( wrlen < 0 )
        {
            if( errno == EAGAIN )
            {
                return 0;
            }
            else if( errno == EINTR )
            {
                goto WRITE_AGAIN;
            }
            else
            {
                perror("write");
                return -1;
            }
        }
        else
        {
            evbuf->start += wrlen;
            sess->handledlen += wrlen;
            
            if( sess->handledlen == sess->contentlen )
            {
                printf("write [%s] over\n", sess->uri);
                return -1; /* it's over, shutdown */
            }
        }

        if( !buffer_empty(evbuf) )
        {
            buffer_shift(evbuf);
        }
        else
        {
            buffer_clear(evbuf);
        }
#ifndef CONFIG_SCHE_RR
    }
#endif

    return 0;
}

static int http_dispatch(event_buffer_t *ev)
{
    int ret = 0;
    buffer_t *evbuf = &ev->evbuf;
    http_session_t *sess = (http_session_t *)ev->arg;

    switch( sess->phase )
    {
        case HTTP_HEAD_READ:
        {
            ret = http_read_header(ev);
            break;
        }
        
        case HTTP_HEAD_RESP:
        {
            ret = http_try_send(ev);
            if( !ret )
            {
                sess->phase = HTTP_BODY; /* transfer state */
                
                if( sess->method == GET )
                {
                    ev->event.events = EPOLLIN|EPOLL_SCHE;
                    ret = epoll_event_mod(ev->base, ev);
                }
            }
            break;
        }
        
        case HTTP_BODY:
        {
            switch( sess->method )
            {
                case GET:
                    ret = http_get_handler(ev);
                    break;
                case PUT:
                    ret = http_put_handler(ev);
                    break;

                default:
                    break;
            }
            break;
        }
        
        case HTTP_END:
        {
            if( buffer_empty(evbuf) )
            {
                return -1;
            }
            else
            {
                return http_try_send(ev);
            }
            break;
        }
        
        default:
        {
            break;
        }
    }

    if( ret < 0 )
    {
        if( ret == -1 )
        {
            return -1;
        }
        
        sess->phase = HTTP_END;
        /*
         * if the return code less than -1, it means the code should be response to peer.
         */
        http_encapsulate_resp(evbuf, -ret, NULL);
        return http_try_send(ev);
    }

    return 0;
}

/*
 * return value: 0, Ok, send over and buffer is empty
 *              -1, Oops, there is something wrong or it's just over
 *               1, Not bad, remain some content, just wait for next event active
 */
static int http_try_send(event_buffer_t *ev)
{
    int ret = 0;
    buffer_t *evbuf = &ev->evbuf;
    http_session_t *sess = (http_session_t *)ev->arg;
    
    ret = nonblocking_send(ev->fd, evbuf);
    if( ret < 0 )
    {
        return -1;
    }

    /*
     * remaining some content to be sent in next round
     */
    if( !buffer_empty(evbuf) )
    {
        if( !(ev->event.events & EPOLLOUT) )
        {
            ev->event.events = EPOLLOUT | EPOLL_SCHE;
            ret = epoll_event_mod(ev->base, ev);
            if( ret < 0 )
                return -1;
        }

        return 1;
    }

    if( sess->phase == HTTP_END ) /* it's end, delete this event */
        return -1;
    else
        return 0;
}

static int serv_start_up(int *port)
{
    int fd = -1;
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));

    /* socket */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if( fd == -1 )
    {
        perror("socket");
        return -1;
    }
    
    if( set_socket_nonblocking(fd) < 0 )
    {
        return -1;
    }
    
    servaddr.sin_family      = AF_INET;
    servaddr.sin_port        = htons(*port);
    servaddr.sin_addr.s_addr = INADDR_ANY;
    
    /* bind */
    if( bind(fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0 )
    {
        perror("bind");
        return -1;
    }

    if( !(*port) )
    {
        socklen_t len = sizeof(servaddr);
        if( getsockname(fd, (struct sockaddr *)&servaddr, &len) == 0 )
        {
            *port = ntohs(servaddr.sin_port);
        }
    }

    /* listen */
    listen(fd, MAX_LISTEN_NUM);
    
    return fd;
}

static void install_signal_handler()
{
    signal(SIGPIPE, SIG_IGN);
}

/*
 * "./httpd port" or just "./httpd" to listen a random port.
 */
int main(int argc, char *argv[])
{
    int fd = -1;
    int ret = 0;
    int port = 0;
    epoll_base_t *base = NULL;
    event_buffer_t *ev = NULL;

    if( argc == 2 )
    {
        port = atoi(argv[1]);
    }

    if( (fd = serv_start_up(&port)) < 0 )
    {
        return -1;
    }
    
    install_signal_handler();

    if( port )
    {
        printf("httpd server is listening on port [%d].\n", port);
    }

    base = epoll_base_new();
    if( !base )
    {
        ret = 1;
        goto OVER;
    }

    ev = epoll_event_buffer_new(base, fd, EPOLLIN, http_accept_request, NULL);
    if( !ev )
    {
        ret = 1;
        goto OVER;
    }

    if( epoll_event_add(base, ev) < 0 )
    {
        ret = 1;
        epoll_event_buffer_free(ev);
        goto OVER;
    }

    /* dispatch */
    epoll_base_dispatch(base);

OVER:
    epoll_base_free(base);
    
    return ret;
}

