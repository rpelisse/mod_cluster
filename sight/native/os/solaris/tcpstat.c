/*
 *  SIGHT - System information gathering hybrid tool
 *
 *  Copyright(c) 2007 Red Hat Middleware, LLC,
 *  and individual contributors as indicated by the @authors tag.
 *  See the copyright.txt in the distribution for a
 *  full listing of individual contributors.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library in the file COPYING.LIB;
 *  if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 *
 * @author Mladen Turk
 *
 */

#include "sight.h"
#include "sight_local.h"
#include "sight_types.h"

#define SIGHT_WANT_LATE_DLL
#include "sight_private.h"

#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stropts.h>
#include <sys/tihdr.h>

#include <inet/mib2.h>
#include <inet/tcp.h>

/*
 * TCP statistics implementation
 */

J_DECLARE_CLAZZ = {
    NULL,
    NULL,
    SIGHT_CLASS_PATH "TcpStatistics"
};

J_DECLARE_F_ID(0000) = {
    NULL,
    "RtoMin",
    "I"
};

J_DECLARE_F_ID(0001) = {
    NULL,
    "RtoMax",
    "I"
};

J_DECLARE_F_ID(0002) = {
    NULL,
    "MaxConn",
    "I"
};

J_DECLARE_F_ID(0003) = {
    NULL,
    "ActiveOpens",
    "I"
};

J_DECLARE_F_ID(0004) = {
    NULL,
    "PassiveOpens",
    "I"
};

J_DECLARE_F_ID(0005) = {
    NULL,
    "AttemptFails",
    "I"
};

J_DECLARE_F_ID(0006) = {
    NULL,
    "EstabResets",
    "I"
};

J_DECLARE_F_ID(0007) = {
    NULL,
    "CurrEstab",
    "I"
};

J_DECLARE_F_ID(0008) = {
    NULL,
    "InSegs",
    "I"
};

J_DECLARE_F_ID(0009) = {
    NULL,
    "OutSegs",
    "I"
};

J_DECLARE_F_ID(0010) = {
    NULL,
    "RetransSegs",
    "I"
};

J_DECLARE_F_ID(0011) = {
    NULL,
    "InErrs",
    "I"
};

J_DECLARE_F_ID(0012) = {
    NULL,
    "OutRsts",
    "I"
};

J_DECLARE_F_ID(0013) = {
    NULL,
    "NumConns",
    "I"
};

SIGHT_CLASS_LDEF(TcpStatistics)
{
    if (sight_load_class(_E, &_clazzn))
        return 1;
    J_LOAD_IFIELD(0000);
    J_LOAD_IFIELD(0001);
    J_LOAD_IFIELD(0002);
    J_LOAD_IFIELD(0003);
    J_LOAD_IFIELD(0004);
    J_LOAD_IFIELD(0005);
    J_LOAD_IFIELD(0006);
    J_LOAD_IFIELD(0007);
    J_LOAD_IFIELD(0008);
    J_LOAD_IFIELD(0009);
    J_LOAD_IFIELD(0010);
    J_LOAD_IFIELD(0011);
    J_LOAD_IFIELD(0012);
    J_LOAD_IFIELD(0013);

    return 0;
}

SIGHT_CLASS_UDEF(TcpStatistics)
{
    sight_unload_class(_E, &_clazzn);
}

static const char *eiftype = "Unsupported NetworkAddressFamily type";

extern apr_pool_t *sight_temp_pool;

typedef struct tcpconn_enum_t {
    int      type;
    int      idx;
    int      numcons;
    void    *conn; /* mib2_tcp6ConnEntry_t / mib2_tcpConnEntry_t */ 
    void    *stat; /* mib2_tcp_t */
    apr_pool_t *pool;
} tcpconn_enum_t;

/* Read the mib2 object and fill the structures */
static void read_mib2(JNIEnv *_E, tcpconn_enum_t *e, apr_pool_t* pool)
{
    apr_status_t rc = APR_ENOTIMPL;
    int sd;
    uintptr_t buf[512 / sizeof (uintptr_t)];
    int flags;
    int i, j, getcode;
    struct strbuf ctlbuf, databuf;

    struct T_optmgmt_req *tor = (struct T_optmgmt_req *)buf;
    struct T_optmgmt_ack *toa = (struct T_optmgmt_ack *)buf;
    struct T_error_ack *tea = (struct T_error_ack *)buf;

    struct opthdr *req;
    int numcons = 0;

    if ((sd = open("/dev/arp", O_RDWR)) < 0) {
        throwAprException(_E, apr_get_os_error());
        return;
    }
    if (ioctl(sd, I_PUSH, "tcp") <0) {
        close(sd);
        throwAprException(_E, apr_get_os_error());
        return;
    }

    tor->PRIM_type = T_SVR4_OPTMGMT_REQ;
    tor->OPT_offset = sizeof (struct T_optmgmt_req);
    tor->OPT_length = sizeof (struct opthdr);
    tor->MGMT_flags = T_CURRENT;
    req = (struct opthdr *)&tor[1];
    req->level = MIB2_IP;		/* any MIB2_xxx value ok here */
    req->name  = 0;
    req->len   = 0;
    
    ctlbuf.buf = (char *)buf;
    ctlbuf.len = tor->OPT_length + tor->OPT_offset;
    flags = 0;
    if (putmsg(sd, &ctlbuf, (struct strbuf *)0, flags) == -1) {
        close(sd);
        throwAprException(_E, apr_get_os_error());
        return;
    }

    for(;;) {
        req = (struct opthdr *)&toa[1];
        ctlbuf.buf = (char *) buf;
        ctlbuf.maxlen = sizeof (buf);
        flags = 0;
        if (getmsg(sd, &ctlbuf, (struct strbuf *)0, &flags) == -1) {
            close(sd);
            throwAprException(_E, apr_get_os_error());
            return;
        }
        if (toa->PRIM_type == T_OPTMGMT_ACK &&
            toa->MGMT_flags == T_SUCCESS &&
            req->len == 0)
            break; /* done */

        /* read the data */
        databuf.maxlen = req->len;
        databuf.buf    = apr_palloc(pool, (int)req->len);
        databuf.len    = 0;
        flags = 0;
        if (getmsg(sd, (struct strbuf *)0, &databuf, &flags) == -1) {
            close(sd);
            throwAprException(_E, apr_get_os_error());
            return;
        }

        /* Process the TPC statics */
        if ((req->level == MIB2_TCP && req->name == 0 && e->type == 1) ||
            (req->level == MIB2_TCP6 && req->name == 0 && e->type == 2)) { 
            e->stat = (void *) databuf.buf;
        } else if (req->level == MIB2_TCP && req->name == MIB2_TCP_CONN && e->type == 1) {
            /* Process the TPC4 statics */
            mib2_tcpConnEntry_t *tcp = (mib2_tcpConnEntry_t *)databuf.buf;
            char *end;
            int numcons = 0;
            end = (char*) tcp + databuf.len;
            while ((char*) tcp < end) {
                numcons++;
                tcp++;
            }
            e->conn = (void *) databuf.buf;
            e->numcons = numcons;
        } else if (req->level == MIB2_TCP6 && req->name == MIB2_TCP6_CONN && e->type == 2) { 
            /* Process the TPC6 statics */
            mib2_tcp6ConnEntry_t *tcp = (mib2_tcp6ConnEntry_t *)databuf.buf;
            char *end;
            int numcons = 0;
            end = (char*) tcp + databuf.len;
            while ((char*) tcp < end) {
                numcons++;
                tcp++;
            }
            e->conn = (void *) databuf.buf;
            e->numcons = numcons;
        } /* else {
            printf("Missing logic info0: level %d name %d\n", req->level, req->name);
        }  */
    }
    close(sd);
}

/* Initialize volume enumeration */
SIGHT_EXPORT_DECLARE(void, TcpStatistics, info0)(SIGHT_STDARGS,
                                                 jobject thiz,
                                                 jint iftype,
                                                 jlong pool)
{
    tcpconn_enum_t *e;
    apr_pool_t* lpool = (apr_pool_t *) pool;
    if (!(e = (tcpconn_enum_t *)sight_calloc(_E,
                               sizeof(tcpconn_enum_t),
                               THROW_FMARK))) {
        return;
    }
    e->type = iftype;
    read_mib2(_E, e, lpool);

    if (e->stat != NULL) {
        mib2_tcp_t	*tcp = (mib2_tcp_t *)e->stat;
        SET_IFIELD_I(0000, thiz, tcp->tcpRtoMin);
        SET_IFIELD_I(0001, thiz, tcp->tcpRtoMax);
        SET_IFIELD_I(0002, thiz, tcp->tcpMaxConn);
        SET_IFIELD_I(0003, thiz, tcp->tcpActiveOpens);
        SET_IFIELD_I(0004, thiz, tcp->tcpPassiveOpens);
        SET_IFIELD_I(0005, thiz, tcp->tcpAttemptFails);
        SET_IFIELD_I(0006, thiz, tcp->tcpEstabResets);
        SET_IFIELD_I(0007, thiz, tcp->tcpCurrEstab);
        SET_IFIELD_I(0008, thiz, tcp->tcpInSegs);
        SET_IFIELD_I(0009, thiz, tcp->tcpOutSegs);
        SET_IFIELD_I(0010, thiz, tcp->tcpRetransSegs);

        SET_IFIELD_I(0012, thiz, tcp->tcpOutRsts);
    }

    SET_IFIELD_I(0013, thiz, e->numcons);
}

/* Initialize TCP conn enumeration */
SIGHT_EXPORT_DECLARE(jlong, TcpStatistics, enum0)(SIGHT_STDARGS,
                                                  jint iftype,
                                                  jlong pool)
{
    tcpconn_enum_t *e;
    apr_status_t rc;

    UNREFERENCED_O;
    if (iftype < 1 || iftype > 2) {
        throwOSException(_E, eiftype);
        return 0;
    }

    if (!(e = (tcpconn_enum_t *)sight_calloc(_E,
                               sizeof(tcpconn_enum_t),
                               THROW_FMARK))) {
        return 0;
    }

    if ((rc = sight_pool_create(&e->pool, NULL, sight_temp_pool, 0)) != APR_SUCCESS) {
        throwAprMemoryException(_E, THROW_FMARK, rc);
        return 0;
    }
    e->type = iftype;
    read_mib2(_E, e, e->pool);

    return P2J(e);
}

/* Get the number of entries */
SIGHT_EXPORT_DECLARE(jint, TcpStatistics, enum1)(SIGHT_STDARGS,
                                                 jlong handle)
{
    tcpconn_enum_t *e = J2P(handle, tcpconn_enum_t *);
    UNREFERENCED_STDARGS;
    if (e) {
        e->idx = 1;
        return e->numcons;
    } else {
        return 0;
    }
}

SIGHT_EXPORT_DECLARE(void, TcpStatistics, enum2)(SIGHT_STDARGS,
                                                 jobject conn,
                                                 jint index,
                                                 jlong handle)
{
    tcpconn_enum_t *e = J2P(handle, tcpconn_enum_t *);
    char las[128] = "";
    char ras[128] = "";
    jobject la, ra;
    jint lp    = 0, rp = 0;
    jint st;

    if (!e)
        return;
    if (e->idx > e->numcons) {
        return;
    }
    if (e->type == 1) {
        mib2_tcpConnEntry_t *tcp = (mib2_tcpConnEntry_t *) e->conn;
        int i;
        for (i=1; i<e->idx; i++)
            tcp++;
        inet_ntop(AF_INET, &tcp->tcpConnLocalAddress, las, 64);
        inet_ntop(AF_INET, &tcp->tcpConnRemAddress, ras, 64);
        lp = tcp->tcpConnLocalPort;
        rp = tcp->tcpConnRemPort;
        st = tcp->tcpConnState;
    } else {
        mib2_tcp6ConnEntry_t *tcp = (mib2_tcp6ConnEntry_t *) e->conn;
        int i;
        for (i=1; i<e->idx; i++)
            tcp++;
        sight_inet_ntop6(tcp->tcp6ConnLocalAddress.s6_addr, las, 64);
        sight_inet_ntop6(tcp->tcp6ConnRemAddress.s6_addr, ras, 64);
        lp = tcp->tcp6ConnLocalPort;
        rp = tcp->tcp6ConnRemPort;
        st = tcp->tcp6ConnState;
    }
    sight_tcpconn_set_tmo(_E, conn, 0);
    sight_tcpconn_set_cts(_E, conn, 0);
    sight_tcpconn_set_pid(_E, conn, 0);

    sight_tcpconn_set_state(_E, conn, st);

    la = sight_new_netaddr_class(_E, _O);
    sight_netaddr_set_addr(_E, la, las);
    sight_netaddr_set_port(_E, la, lp);
    sight_tcpconn_set_local(_E, conn, la);
    (*_E)->DeleteLocalRef(_E, la);

    ra = sight_new_netaddr_class(_E, _O);
    sight_netaddr_set_addr(_E, ra, ras);
    sight_netaddr_set_port(_E, ra, rp);
    sight_tcpconn_set_remote(_E, conn, ra);
    (*_E)->DeleteLocalRef(_E, ra);

    e->idx++;
}

/* Close TCP conn enumeration */
SIGHT_EXPORT_DECLARE(void, TcpStatistics, enum3)(SIGHT_STDARGS,
                                                 jlong handle)
{
    tcpconn_enum_t *e = J2P(handle, tcpconn_enum_t *);

     UNREFERENCED_STDARGS;
    if (e) {
        apr_pool_destroy(e->pool);
        free(e);
    }
}
