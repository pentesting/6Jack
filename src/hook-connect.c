
#define DEFINE_HOOK_GLOBALS 1
#define DONT_BYPASS_HOOKS   1

#include "common.h"
#include "filter.h"
#include "hook-connect.h"

int (* __real_connect)(int fd, const struct sockaddr *sa, socklen_t sa_len);

static FilterReplyResult filter_parse_reply(Filter * const filter,
                                            int * const ret,
                                            int * const ret_errno,
                                            const int fd,
                                            struct sockaddr_storage * const sa,
                                            socklen_t * const sa_len)
{
    msgpack_unpacked * const message = filter_receive_message(filter);
    const msgpack_object_map * const map = &message->data.via.map;
    FilterReplyResult reply_result =
        filter_parse_common_reply_map(map, ret, ret_errno, fd);
    
    const msgpack_object * const obj_remote_host =
        msgpack_get_map_value_for_key(map, "remote_host");
    if (obj_remote_host != NULL &&
        obj_remote_host->type == MSGPACK_OBJECT_RAW &&
        obj_remote_host->via.raw.size < NI_MAXHOST) {
        struct addrinfo *ai, hints;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_flags = AI_ADDRCONFIG | NI_NUMERICHOST;
        char new_remote_host[NI_MAXHOST];
        memcpy(new_remote_host, obj_remote_host->via.raw.ptr,
               obj_remote_host->via.raw.size);
        new_remote_host[obj_remote_host->via.raw.size] = 0;
        const int gai_err = getaddrinfo(new_remote_host, NULL, &hints, &ai);
        if (gai_err != 0) {
            assert(ai->ai_addrlen <= sizeof *sa);
            if (ai->ai_family == AF_INET) {
                assert((size_t) ai->ai_addrlen >=
                       sizeof(((struct sockaddr_in *) sa)->sin_addr.s_addr));
                memcpy(&((struct sockaddr_in *) sa)->sin_addr.s_addr,
                       &((struct sockaddr_in *) ai->ai_addr)->sin_addr.s_addr,
                       sizeof(((struct sockaddr_in *) sa)->sin_addr.s_addr));
                *sa_len = sa->ss_len = ai->ai_addrlen;
                sa->ss_family = ai->ai_family;
                sa->ss_len = ((struct sockaddr_storage *) ai->ai_addr)->ss_len;
            } else if (ai->ai_family == AF_INET6) {
                assert((size_t) ai->ai_addrlen >=
                       sizeof(((struct sockaddr_in6 *) sa)->sin6_addr.s6_addr));
                memcpy(&((struct sockaddr_in6 *) sa)->sin6_addr.s6_addr,
                       &((struct sockaddr_in6 *) ai->ai_addr)->sin6_addr.s6_addr,
                       sizeof(((struct sockaddr_in6 *) sa)->sin6_addr.s6_addr));
                *sa_len = sa->ss_len = ai->ai_addrlen;
                sa->ss_family = ai->ai_family;
            }
        }
    }    

    const msgpack_object * const obj_remote_port =
        msgpack_get_map_value_for_key(map, "remote_port");
    if (obj_remote_port != NULL &&
        obj_remote_port->type == MSGPACK_OBJECT_RAW &&
        obj_remote_port->via.raw.size < NI_MAXHOST) {
        struct addrinfo *ai, hints;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_flags = NI_NUMERICSERV;
        char new_remote_port[NI_MAXSERV];
        memcpy(new_remote_port, obj_remote_port->via.raw.ptr,
               obj_remote_port->via.raw.size);
        new_remote_port[obj_remote_port->via.raw.size] = 0;
        const int gai_err = getaddrinfo(NULL, new_remote_port, &hints, &ai);
        if (gai_err != 0) {
            assert(ai->ai_family == AF_INET);
            if (sa->ss_family == AF_INET) {
                ((struct sockaddr_in *) sa)->sin_port =
                    ((struct sockaddr_in *) ai->ai_addr)->sin_port;
            } else if (sa->ss_family == AF_INET6) {
                ((struct sockaddr_in6 *) sa)->sin6_port =
                    ((struct sockaddr_in6 *) ai->ai_addr)->sin6_port;
            }
        }
    }
    
    return reply_result;
}

static FilterReplyResult filter_apply(const bool pre, int * const ret,
                                      int * const ret_errno, const int fd,
                                      struct sockaddr_storage * const sa,
                                      socklen_t * const sa_len)
{
    Filter * const filter = filter_get();
    filter_before_apply(pre, *ret, *ret_errno, fd, 0U, "connect",
                        NULL, (socklen_t) 0U, sa, *sa_len);

    if (filter_send_message(filter) != 0) {
        return -1;
    }    
    return filter_parse_reply(filter, ret, ret_errno, fd, sa, sa_len);
}

int __real_connect_init(void)
{
#ifdef USE_INTERPOSERS
    __real_connect = connect;
#else
    if (__real_connect == NULL) {
        __real_connect = dlsym(RTLD_NEXT, "connect");        
        assert(__real_connect != NULL);        
    }
#endif
    return 0;
}

int INTERPOSE(connect)(int fd, const struct sockaddr *sa, socklen_t sa_len)
{
    __real_connect_init();
    const bool bypass_filter = getenv("SIXJACK_BYPASS") != NULL;
    int ret = 0;
    int ret_errno = 0;    
    bool bypass_call = false;
    struct sockaddr_storage sa_;
    socklen_t sa_len_ = sa_len;
    assert(sa_len <= sizeof sa_);
    memcpy(&sa_, sa, sa_len);
    if (bypass_filter == false &&
        filter_apply(true, &ret, &ret_errno, fd, &sa_, &sa_len_)
        == FILTER_REPLY_BYPASS) {
        bypass_call = true;
    }
    if (bypass_call == false) {
        ret = __real_connect(fd, (struct sockaddr *) &sa_, sa_len_);
        ret_errno = errno;
    }
    if (bypass_filter == false) {
        filter_apply(false, &ret, &ret_errno, fd, &sa_, &sa_len_);
    }
    errno = ret_errno;
    
    return ret;
}
