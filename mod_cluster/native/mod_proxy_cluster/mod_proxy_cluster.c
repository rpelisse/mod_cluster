/*
 *  mod_cluster
 *
 *  Copyright(c) 2008 Red Hat Middleware, LLC,
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
 * @author Jean-Frederic Clere
 * @version $Revision$
 */

#include "apr_strings.h"

#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_main.h"
#include "http_request.h"
#include "http_protocol.h"
#include "http_core.h"
#include "mod_proxy.h"

#include "mod_proxy_cluster.h"

#include "slotmem.h"

#include "node.h"
#include "host.h"
#include "context.h"
#include "balancer.h"

struct proxy_cluster_helper {
    int count_active; /* currently active request using the worker */
#if AP_MODULE_MAGIC_AT_LEAST(20051115,4)
#else
    apr_interval_time_t ping_timeout;
    char ping_timeout_set;
#endif
 
};
typedef struct  proxy_cluster_helper proxy_cluster_helper;

static struct node_storage_method *node_storage = NULL; 
static struct host_storage_method *host_storage = NULL; 
static struct context_storage_method *context_storage = NULL; 
static struct balancer_storage_method *balancer_storage = NULL; 

static apr_thread_mutex_t *lock = NULL;

#define WAITFORREMOVE 10 /* seconds */

/*
 * Create/Get the worker before using it
 */
static void create_worker(proxy_server_conf *conf, proxy_balancer *balancer,
                          server_rec *server, proxy_worker **worker,
                          nodeinfo_t *node, apr_pool_t *pool)
{
    char *url;
    char *ptr;
    int reuse = 0;
#if AP_MODULE_MAGIC_AT_LEAST(20051115,4)
#else
    proxy_cluster_helper *helperping;
#endif

    /* build the name (scheme and port) when needed */
    url = apr_pstrcat(pool, node->mess.Type, "://", node->mess.Host, ":", node->mess.Port, NULL);

    *worker = ap_proxy_get_worker(conf->pool, conf, url);
    if ((*worker) == NULL) {

        /* creates it */ 
        proxy_cluster_helper *helper;
        const char *err = ap_proxy_add_worker(worker, conf->pool, conf, url);
        if (err) {
            ap_log_error(APLOG_MARK, APLOG_NOTICE|APLOG_NOERRNO, 0, server,
                         "Created: worker for %s failed: %s", url, err);
            return;
        }
        (*worker)->opaque = apr_pcalloc(conf->pool,  sizeof(proxy_cluster_helper));
        helper = (*worker)->opaque;
        helper->count_active = 0;
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
                     "Created: worker for %s", url);
    } else  if ((*worker)->id == 0) {
        /* We are going to reuse a removed one */
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
                     "Created: reusing worker for %s", url);
        if ((*worker)->cp->pool == NULL) {
            /* Copied for init_conn_pool in modules/proxy/proxy_util.c */
            apr_pool_t *pool;
            proxy_conn_pool *cp;
            apr_pool_create(&pool, conf->pool);
            apr_pool_tag(pool, "proxy_worker_cp");
            cp = (proxy_conn_pool *)apr_pcalloc(conf->pool, sizeof(proxy_conn_pool));
            cp->pool = pool;
            (*worker)->cp = cp;
        }
        reuse = 1;
    } else {
        return; /* Done Already existing */
    }

    /* Get the shared memory for this worker */
    ptr = (char *) node;
    ptr = ptr + node->offset;
    (*worker)->s = (proxy_worker_stat *) ptr;

    (*worker)->id = node->mess.id;
    (*worker)->route = apr_pstrdup(conf->pool, node->mess.JVMRoute);
    (*worker)->redirect = apr_pstrdup(conf->pool, "");
    (*worker)->smax = node->mess.smax;
    (*worker)->ttl = node->mess.ttl;
    if (node->mess.timeout) {
        (*worker)->timeout_set = 1;
        (*worker)->timeout = node->mess.timeout;
    }
    (*worker)->flush_packets = node->mess.flushpackets;
    (*worker)->flush_wait = node->mess.flushwait;
#if AP_MODULE_MAGIC_AT_LEAST(20051115,4)
    (*worker)->ping_timeout = node->mess.ping;
    (*worker)->ping_timeout_set = 1;
    (*worker)->acquire_set = 1;
#else
    helperping = (*worker)->opaque;
    helperping->ping_timeout = node->mess.ping;
    helperping->ping_timeout_set = 1;
#endif
    (*worker)->keepalive = 1;
    (*worker)->keepalive_set = 1;
    (*worker)->is_address_reusable = 1;
    (*worker)->acquire = apr_time_make(0, 2 * 1000); /* 2 ms */
    
    /*
     * The Shared datastatus may already contains a valid information
     */
    if (!(*worker)->s->status) {
        (*worker)->s->status = PROXY_WORKER_INITIALIZED;
        strncpy((*worker)->s->route, node->mess.JVMRoute, PROXY_WORKER_MAX_ROUTE_SIZ);
        (*worker)->s->route[PROXY_WORKER_MAX_ROUTE_SIZ] = '\0';
        /* XXX: We need that information from TC */
        (*worker)->s->redirect[0] = '\0';
        (*worker)->s->lbstatus = 0;
        (*worker)->s->lbfactor = -1; /* prevent using the node using status message */
    }

    if (!reuse) {
        /*
         * Create the corresponding balancer worker information
         * copying for proxy_util.c ap_proxy_add_worker_to_balancer
         */
        proxy_worker *runtime;
        runtime = apr_array_push(balancer->workers);
        memcpy(runtime, (*worker), sizeof(proxy_worker));
    } else {
        /* Update the corresponding balancer worker information */
        proxy_worker *runtime;
        int i;

        runtime = (proxy_worker *)balancer->workers->elts;
        for (i = 0; i < balancer->workers->nelts; i++, runtime++) {
            if (runtime->name) {
                if (strcmp(url, runtime->name) == 0) {
                    memcpy(runtime, (*worker), sizeof(proxy_worker));
                }
            }
        }
    }
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
                 "Created: worker for %s %d (status): %d", url, (*worker)->id, (*worker)->s->status);
}

/*
 * Add a node to the worker conf
 * NOTE: pool is the request pool or any temporary pool. Use conf->pool for any data that live longer.
 */
static void add_workers_node(nodeinfo_t *node, proxy_server_conf *conf, apr_pool_t *pool, server_rec *server)
{
    char *name = apr_pstrcat(pool, "cluster://", node->mess.balancer, NULL);
    proxy_balancer *balancer = ap_proxy_get_balancer(pool, conf, name);
    proxy_worker *worker = NULL;
    if (!balancer) {
        /* Create one */
        int sizebal, i;
        int *bal;
        ap_log_error(APLOG_MARK, APLOG_NOTICE|APLOG_NOERRNO, 0, server,
                      "add_workers_node: Create balancer %s", name);
        balancer = apr_array_push(conf->balancers);
        memset(balancer, 0, sizeof(proxy_balancer));
        balancer->name = apr_pstrdup(conf->pool, name);
        balancer->lbmethod = ap_lookup_provider(PROXY_LBMETHOD, "cluster_byrequests", "0");
        balancer->workers = apr_array_make(conf->pool, 5, sizeof(proxy_worker));
        /* XXX Is this a right place to create mutex */
#if APR_HAS_THREADS
        if (apr_thread_mutex_create(&(balancer->mutex),
                    APR_THREAD_MUTEX_DEFAULT, conf->pool) != APR_SUCCESS) {
            /* XXX: Do we need to log something here */
            ap_log_error(APLOG_MARK, APLOG_NOTICE|APLOG_NOERRNO, 0, server,
                          "add_workers_node: Can't create lock for balancer");
        }
#endif

        /* Logic to copy the shared memory information to the balancer */
        bal = apr_pcalloc(pool, sizeof(int) * balancer_storage->get_max_size_balancer());
        sizebal = balancer_storage->get_ids_used_balancer(bal);
        for (i=0; i<sizebal; i++) {
            balancerinfo_t *balan;
            balancer_storage->read_balancer(bal[i], &balan);
            /* Something like cluster://cluster1 and cluster1 */
            if (strcmp(balan->balancer, &balancer->name[10]) == 0) {
                /* XXX: StickySession, StickySessionRemove not in */
                balancer->sticky  = apr_psprintf(conf->pool, "%s|%s", balan->StickySessionCookie,
                                                 balan->StickySessionPath);
                balancer->sticky_force = balan->StickySessionForce;
                balancer->timeout = balan->Timeout;

                balancer->max_attempts = balan->Maxattempts;
                balancer->max_attempts_set = 1;
                break;
            }
        }
    }
    if (balancer) {
        create_worker(conf, balancer, server, &worker, node, pool);
    } else {
        ap_log_error(APLOG_MARK, APLOG_NOTICE|APLOG_NOERRNO, 0, server,
                      "add_workers_node: Can't find balancer");
    }
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
             "add_workers_node done");
}
/*
 * Remove a node from the worker conf
 */
static int remove_workers_node(nodeinfo_t *node, proxy_server_conf *conf, apr_pool_t *pool, server_rec *server)
{
    int i;
    proxy_cluster_helper *helper;
    proxy_worker *worker = (proxy_worker *)conf->workers->elts;
    for (i = 0; i < conf->workers->nelts; i++) {
        if (worker->id == node->mess.id)
            break;
        worker++;
    }
    if (i == conf->workers->nelts) {
        /* XXX: Another process may use it, can't do: node_storage->remove_node(node); */
        return 0; /* Done */
    }

    /* prevent other threads using it */
    worker->s->status = worker->s->status |= PROXY_WORKER_IN_ERROR;

    /* apr_reslist_acquired_count */
    i = 0;

#if APU_MAJOR_VERSION > 1 || (APU_MAJOR_VERSION == 1 && APU_MINOR_VERSION >= 3)
    if (worker->cp->res)
        i = apr_reslist_acquired_count(worker->cp->res);
#else
    helper = worker->opaque;
    i = helper->count_active;
#endif
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
             "remove_workers_node %d %s", i, node->mess.JVMRoute);
    if (i == 0) {
        /* No connection in use: clean the worker */
        proxy_balancer *balancer;
        char *name = apr_pstrcat(pool, "cluster://", node->mess.balancer, NULL); 

        /* mark the worker removed in the apr_array of the balancer */
        balancer = (proxy_balancer *)conf->balancers->elts;
        for (i = 0; i < conf->balancers->nelts; i++, balancer++) {
            if (strcmp(balancer->name, name) == 0) {
                int j;
                proxy_worker *searched = (proxy_worker *)balancer->workers->elts;
                for (j = 0; j < balancer->workers->nelts; j++, searched++) {
                    if (searched->id == worker->id) {
                        searched->id = 0; /* mark it removed */
                    }
                }
            }
        }

        /* Clear the connection pool (close the sockets) */
        if (worker->cp->pool) {
            apr_pool_destroy(worker->cp->pool);
            worker->cp->pool = NULL;
        }

        /* XXX: Shouldnn't we remove the mutex too (worker->mutex) */

        worker->id = 0; /* mark it removed */

        return (0);
    } else
        return (1); /* We should retry later */
}
/*
 * Create/Remove workers corresponding to updated nodes.
 */
static void update_workers_node(proxy_server_conf *conf, apr_pool_t *pool, server_rec *server)
{
    int *id, size, i;
    apr_time_t last;
    int notok = 0;

    /* Check if we have to do something */
    apr_thread_mutex_lock(lock);
    last = node_storage->worker_nodes_need_update(server, pool);

    /* nodes_need_update will return 1 if last_updated is zero: first time we are called */
    if (last == 0) {
        apr_thread_mutex_unlock(lock);
        return;
    }

    /* read the ident of the nodes */
    id = apr_pcalloc(pool, sizeof(int) * node_storage->get_max_size_node());
    size = node_storage->get_ids_used_node(id);

    /* XXX: How to skip the balancer that aren't controled by mod_manager */

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
             "update_workers_node starting");

    /* Only process the nodes that have been updated since our last update */
    for (i=0; i<size; i++) {
        nodeinfo_t *ou;
        node_storage->read_node(id[i], &ou);
        if (ou->updatetime >= last) {
            /* The node has changed */
            if (ou->mess.remove)
                notok = notok + remove_workers_node(ou, conf, pool, server);
            else
                add_workers_node(ou, conf, pool, server);
        } 
    } 
    if (! notok) 
        node_storage->worker_nodes_are_updated(server);

    apr_thread_mutex_unlock(lock);
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
             "update_workers_node done");
}

/* reslist constructor */
/* XXX: Should use the proxy_util one. */
static apr_status_t connection_constructor(void **resource, void *params,
                                           apr_pool_t *pool)
{
    apr_pool_t *ctx;
    apr_pool_t *scpool;
    proxy_conn_rec *conn;
    proxy_worker *worker = (proxy_worker *)params;

    /*
     * Create the subpool for each connection
     * This keeps the memory consumption constant
     * when disconnecting from backend.
     */
    apr_pool_create(&ctx, pool);
    conn = apr_pcalloc(pool, sizeof(proxy_conn_rec));

    /*
     * Create another subpool that manages the data for the
     * socket and the connection member of the proxy_conn_rec struct as we
     * destroy this data more frequently than other data in the proxy_conn_rec
     * struct like hostname and addr (at least in the case where we have
     * keepalive connections that timed out).
     */
#if AP_MODULE_MAGIC_AT_LEAST(20051115,13)
    apr_pool_create(&scpool, ctx);
    apr_pool_tag(scpool, "proxy_conn_scpool");
    conn->scpool = scpool;
#endif

    conn->pool   = ctx;
    conn->worker = worker;
#if APR_HAS_THREADS
    conn->inreslist = 1;
#endif
    *resource = conn;

    return APR_SUCCESS;
}

/* connection cleanup routine */
/* XXX: Should use the proxy_util one. */
static apr_status_t connection_cleanup(void *theconn)
{
    proxy_conn_rec *conn = (proxy_conn_rec *)theconn;
    proxy_worker *worker = conn->worker;

    /*
     * If the connection pool is NULL the worker
     * cleanup has been run. Just return.
     */
    if (!worker->cp) {
        return APR_SUCCESS;
    }

#if APR_HAS_THREADS
    /* Sanity check: Did we already return the pooled connection? */
    if (conn->inreslist) {
        ap_log_perror(APLOG_MARK, APLOG_ERR, 0, conn->pool,
                      "proxy: Pooled connection 0x%pp for worker %s has been"
                      " already returned to the connection pool.",
                      (void *) conn,
                      worker->name);
        return APR_SUCCESS;
    }
#endif

    /* determine if the connection need to be closed */
    if (conn->close_on_recycle || conn->close) {
        apr_pool_t *p = conn->pool;
        apr_pool_clear(conn->pool);
        memset(conn, 0, sizeof(proxy_conn_rec));
        conn->pool = p;
        conn->worker = worker;
    }
#if APR_HAS_THREADS
    if (worker->hmax && worker->cp->res) {
        conn->inreslist = 1;
        apr_reslist_release(worker->cp->res, (void *)conn);
    }
    else
#endif
    {
        worker->cp->conn = conn;
    }

    /* Always return the SUCCESS */
    return APR_SUCCESS;
}

/* Retrieve the parameter with the given name
 * Something like 'JSESSIONID=12345...N'
 * XXX: Should use the mod_proxy_balancer ones.
 */
static char *get_path_param(apr_pool_t *pool, char *url,
                            const char *name)
{
    char *path = NULL;

    for (path = strstr(url, name); path; path = strstr(path + 1, name)) {
        path += strlen(name);
        if (*path == '=') {
            /*
             * Session path was found, get it's value
             */
            ++path;
            if (strlen(path)) {
                char *q;
                path = apr_strtok(apr_pstrdup(pool, path), "?&", &q);
                return path;
            }
        }
    }
    return NULL;
}

static char *get_cookie_param(request_rec *r, const char *name)
{
    const char *cookies;
    const char *start_cookie;

    if ((cookies = apr_table_get(r->headers_in, "Cookie"))) {
        for (start_cookie = ap_strstr_c(cookies, name); start_cookie;
             start_cookie = ap_strstr_c(start_cookie + 1, name)) {
            if (start_cookie == cookies ||
                start_cookie[-1] == ';' ||
                start_cookie[-1] == ',' ||
                isspace(start_cookie[-1])) {

                start_cookie += strlen(name);
                while(*start_cookie && isspace(*start_cookie))
                    ++start_cookie;
                if (*start_cookie == '=' && start_cookie[1]) {
                    /*
                     * Session cookie was found, get it's value
                     */
                    char *end_cookie, *cookie;
                    ++start_cookie;
                    cookie = apr_pstrdup(r->pool, start_cookie);
                    if ((end_cookie = strchr(cookie, ';')) != NULL)
                        *end_cookie = '\0';
                    if((end_cookie = strchr(cookie, ',')) != NULL)
                        *end_cookie = '\0';
                    return cookie;
                }
            }
        }
    }
    return NULL;
}

/*
 * Check that the request has a sessionid (even invalid)
 */
static int hassession(request_rec *r, proxy_balancer *balancer)
{
    char *route;
    char *uri = r->filename + 6;
    char *sticky_path, *sticky, *path;

    if (balancer->sticky == NULL)
        return 0;

    /* for 2.2.x the sticky parameter may contain 2 values */
    sticky = sticky_path = apr_pstrdup(r->pool, balancer->sticky);
    if ((path = strchr(sticky, '|'))) {
        *path++ = '\0';
         sticky_path = path;
    }
    route = get_path_param(r->pool, uri , sticky_path);
    if (route == NULL)
        route = get_cookie_param(r, sticky);
    if (route) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "mod_proxy_cluster: found route %s", route);
        return 1;
    }
    return 0;
}

/*
 * Check that the worker will handle the host/context.
 * The id of the worker is used to find the (slot) node in the shared
 * memory.
 */ 
static int iscontext_host_ok(request_rec *r, proxy_balancer *balancer,
                             proxy_worker *worker)
{
    nodeinfo_t *node;
    int sizevhost, i;
    int *vhosts;

    node_storage->read_node(worker->id, &node);

    /*
     * check the hosts and contexts
     * A node may have several virtual hosts and
     * each virtual hosts may have several context
     */
    sizevhost = host_storage->get_max_size_host();
    vhosts =  apr_palloc(r->pool, sizeof(int)*sizevhost);
    sizevhost = host_storage->get_ids_used_host(vhosts);
    for (i=0; i<sizevhost; i++) {
        hostinfo_t *vhost;
        host_storage->read_host(vhosts[i], &vhost);
        if (vhost->node == node->mess.id) {
            /* XXX Check the virtual host */

            /* Check the contexts */
            int j, len;
            int sizecontext = context_storage->get_max_size_context();
            int *contexts =  apr_palloc(r->pool, sizeof(int)*sizecontext);
            sizecontext = context_storage->get_ids_used_context(contexts);
            for (j=0; j<sizecontext; j++) {
                contextinfo_t *context;
                context_storage->read_context(contexts[j], &context);
                if (context->vhost != vhost->vhost)
                    continue;

                /* check for /context[/] in the URL */
                len = strlen(context->context);
                if (strncmp(r->uri, context->context, len) == 0) {
                    if (r->uri[len] == '\0' || r->uri[len-1] == '/') {
                        /* Check status */
                        switch (context->status)
                        {
                          case ENABLED: 
                            return 1;
                            break;
                          case DISABLED:
                            /* Only the request with sessionid ok for it */
                            if (hassession(r, balancer))
                                return 1;
                            break;
                        }
                    }
                } 
            }
            
        }
    }
    return 0;
}

/*
 * Check that the request has a sessionid (even invalid)
 * Use the name of the balancer.
 */
static int hassession_byname(request_rec *r, char *balancer_name, proxy_server_conf *conf)
{
    proxy_balancer *balancer;
    char *route;
    char *uri = r->filename + 6;
    char *sticky_path, *sticky, *path;
    int i;

    balancer = (proxy_balancer *)conf->balancers->elts;
    for (i = 0; i < conf->balancers->nelts; i++, balancer++) {
        if (strcasecmp(balancer->name, uri) == 0)
            break;
    }

    /* XXX: We don't find the balancer, that is BAD */
    if (i == conf->balancers->nelts)
        return 0;

    if (balancer->sticky == NULL)
        return 0;

    /* for 2.2.x the sticky parameter may contain 2 values */
    sticky = sticky_path = apr_pstrdup(r->pool, balancer->sticky);
    if ((path = strchr(sticky, '|'))) {
        *path++ = '\0';
         sticky_path = path;
    }
    route = get_path_param(r->pool, uri , sticky_path);
    if (route == NULL)
        route = get_cookie_param(r, sticky);
    if (route) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "mod_proxy_cluster: found route %s", route);
        return 1;
    }
    return 0;
}
/*
 * Search the balancer that corresponds to the pair context/host
 */ 
static char *get_context_host_balancer(request_rec *r)
{
    void *sconf = r->server->module_config;
    proxy_server_conf *conf = (proxy_server_conf *)
        ap_get_module_config(sconf, &proxy_module);

    int sizenode = node_storage->get_max_size_node();
    int n;
    int *nodes =  apr_palloc(r->pool, sizeof(int)*sizenode);
    sizenode = node_storage->get_ids_used_node(nodes);
    for (n=0; n<sizenode; n++) {
        nodeinfo_t *node;
        int i;
        int sizevhost;
        int *vhosts;

        node_storage->read_node(nodes[n], &node);

        /*
         * check the hosts and contexts
         * A node may have several virtual hosts and
         * each virtual hosts may have several context
         */
        sizevhost = host_storage->get_max_size_host();
        vhosts =  apr_palloc(r->pool, sizeof(int)*sizevhost);
        sizevhost = host_storage->get_ids_used_host(vhosts);
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "get_context_host_balancer testing node %s for %s", node->mess.JVMRoute, r->uri);
        for (i=0; i<sizevhost; i++) {
            hostinfo_t *vhost;
            host_storage->read_host(vhosts[i], &vhost);
            if (vhost->node == node->mess.id) {
                /* XXX Check the virtual host */

                /* Check the contexts */
                int j;
                int sizecontext = context_storage->get_max_size_context();
                int *contexts =  apr_palloc(r->pool, sizeof(int)*sizecontext);
                sizecontext = context_storage->get_ids_used_context(contexts);
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                             "get_context_host_balancer testing host %s", vhost->host);
                for (j=0; j<sizecontext; j++) {
                    contextinfo_t *context;
                    int len;
                    context_storage->read_context(contexts[j], &context);
                    if (context->vhost != vhost->vhost)
                        continue;
                    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                                 "get_context_host_balancer testing context %s", context->context);

                    /* check for /context[/] in the URL */
                    len = strlen(context->context);
                    if (strncmp(r->uri, context->context, len) == 0) {
                        if (r->uri[len] == '\0' || r->uri[len-1] == '/') {
                            /* Check status */
                            switch (context->status)
                            {
                              case ENABLED: 
                                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                                             "get_context_host_balancer found context %s", context->context);
                                return node->mess.balancer;
                                break;
                              case DISABLED:
                                /* Only the request with sessionid ok for it */
                                if (hassession_byname(r, node->mess.balancer, conf))
                                    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                                                 "get_context_host_balancer found (DISABLED) context %s", context->context);
                                    return node->mess.balancer;
                                break;
                            }
                        }
                    } 
                }
            
            }
        }
    }
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                 "get_context_host_balancer NOT found");
    return NULL;
}

/*
 * The ModClusterService from the cluster fills the lbfactor values.
 * Our logic is a bit different the mod_balancer one. We check the
 * context and host to prevent to route to application beeing redeploy or
 * stopped in one node but not in others.
 */
static proxy_worker *find_best_byrequests(proxy_balancer *balancer,
                                         request_rec *r)
{
    int i;
    int total_factor = 0;
    proxy_worker *worker;
    proxy_worker *mycandidate = NULL;
    int checking_standby;
    int checked_standby;
    void *sconf = r->server->module_config;
    proxy_server_conf *conf = (proxy_server_conf *)
        ap_get_module_config(sconf, &proxy_module);

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                 "proxy: Entering byrequests for CLUSTER (%s)",
                 balancer->name);

    /* create workers for new nodes */
    update_workers_node(conf, r->pool, r->server);

    /* First try to see if we have available candidate */
    checking_standby = checked_standby = 0;
    while (!mycandidate && !checked_standby) {
        worker = (proxy_worker *)balancer->workers->elts;
        for (i = 0; i < balancer->workers->nelts; i++, worker++) {
            if (worker->id == 0)
                continue; /* marked removed */

            /* standby logic
             * lbfactor: -1 broken node.
             *            0 standby.
             *           >0 factor to use.
             */
            if (worker->s->lbfactor < 0 || (worker->s->lbfactor == 0 && !checking_standby))
                continue;

            /* If the worker is in error state the STATUS logic will retry it */
            if (!PROXY_WORKER_IS_USABLE(worker)) {
                continue;
            }

            /* Take into calculation only the workers that are
             * not in error state or not disabled.
             * and that can map the context.
             */
            if (PROXY_WORKER_IS_USABLE(worker) && iscontext_host_ok(r, balancer, worker)) {
                if (worker->s->lbfactor == 0 && checking_standby) {
                    mycandidate = worker;
                    break; /* Done */
                } else {
                    worker->s->lbstatus += worker->s->lbfactor;
                    total_factor += worker->s->lbfactor;
                    if (!mycandidate || worker->s->lbstatus > mycandidate->s->lbstatus) {
                        mycandidate = worker;
                    }
                }
            }
        }
        checked_standby = checking_standby++;
    }

    if (mycandidate) {
        mycandidate->s->lbstatus -= total_factor;
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                             "proxy: byrequests balancer DONE (%s)", mycandidate->name);
    } else {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                             "proxy: byrequests balancer FAILED");
    }
    return mycandidate;
}
static const proxy_balancer_method byrequests =
{
    "byrequests",
    &find_best_byrequests,
    NULL
};

/*
 * Do a ping/pong to the node
 * XXX: ajp_handle_cping_cpong should come from a provider as
 * it is already in modules/proxy/ajp_utils.c
 */
static apr_status_t ajp_handle_cping_cpong(apr_socket_t *sock,
                                           request_rec *r,
                                           apr_interval_time_t timeout)
{
    char buf[5];
    apr_size_t written = 5;
    apr_interval_time_t org; 
    apr_status_t status;

    /* built the cping message */
    buf[0] = 0x12;
    buf[1] = 0x34;
    buf[2] = (apr_byte_t) 0;
    buf[3] = (apr_byte_t) 1;
    buf[4] = (unsigned char)10;

    status = apr_socket_send(sock, buf, &written);
    if (status != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, status, NULL,
                      "ajp_handle_cping_cpong(): send failed");
        return status;
    }
    status = apr_socket_timeout_get(sock, &org);
    if (status != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, status, NULL,
                      "ajp_handle_cping_cpong(): apr_socket_timeout_get failed");
        return status;
    }
    status = apr_socket_timeout_set(sock, timeout);
    written = 5;
    status = apr_socket_recv(sock, buf, &written);
    if (status != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
               "ajp_handle_cping_cpong: apr_socket_recv failed");
        goto cleanup;
    }
    if (buf[0] != 0x41 || buf[1] != 0x42 || buf[2] != 0 || buf[3] != 1  || buf[4] != (unsigned char)9) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
               "ajp_handle_cping_cpong: awaited CPONG, received %02x %02x %02x %02x %02x",
               buf[0] & 0xFF,
               buf[1] & 0xFF,
               buf[2] & 0xFF,
               buf[3] & 0xFF,
               buf[4] & 0xFF);
        status = APR_EGENERAL;
    }
cleanup:
    status = apr_socket_timeout_set(sock, org);
    if (status != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
               "ajp_handle_cping_cpong: apr_socket_timeout_set failed");
        return status;
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                         "ajp_handle_cping_cpong: Done");
    return status;
}
static apr_status_t proxy_cluster_try_pingpong(request_rec *r, proxy_worker *worker, char *scheme)
{
    apr_status_t rv;
    proxy_conn_rec *conn;
    apr_interval_time_t timeout;
    apr_interval_time_t savetimeout;
    char savetimeout_set;
#if AP_MODULE_MAGIC_AT_LEAST(20051115,4)
#else
    proxy_cluster_helper *helperping;
#endif
    /* get the proxy_conn_rec: from ap_proxy_acquire_connection */
#if APR_HAS_THREADS
    /* prefork() is not "safe" as we have now 2 threads... */
    if (worker->hmax && worker->cp->res) {
        rv = apr_reslist_acquire(worker->cp->res, (void **)&conn);
    }
    else
#endif
    {
        /* create the new connection if the previous was destroyed */
        rv = APR_SUCCESS;
        if (!worker->cp->conn) {
            /* XXX: What appends is worker->cp->pool is NULL */
            if (worker->cp->pool) 
                connection_constructor((void **)&conn, worker, worker->cp->pool);
            else
                rv = APR_ENOPOOL;
        }
        else {
            conn = worker->cp->conn;
            worker->cp->conn = NULL;
        }
    }

    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "proxy_cluster_try_pingpong: failed acquiring connection");
        if (conn) {
            conn->close = 1;
            ap_proxy_release_connection(scheme, conn, r->server);
        }
        return rv;
    }

    conn->worker = worker;
    conn->close  = 0;
#if APR_HAS_THREADS
    conn->inreslist = 0;
#endif

    /* Replace ap_proxy_determine_connection XXX: Still not ok */
    if (!conn->hostname) {
        conn->hostname = apr_pstrdup(conn->pool, worker->hostname);
        conn->port = worker->port;
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "proxy_cluster_try_pingpong: connection to %s : %d",
                     conn->hostname, conn->port);

        if (conn->sock) {
            apr_socket_close(conn->sock);
            conn->sock = NULL;
        }
        if (conn->connection) {
            apr_pool_cleanup_kill(conn->connection->pool, conn, connection_cleanup);
            conn->connection = NULL;
        }
        rv = apr_sockaddr_info_get(&(conn->addr),
                                    conn->hostname, APR_UNSPEC,
                                    conn->port, 0,
                                    conn->pool);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                         "proxy_cluster_try_pingpong: can't resolve %s",
                         conn->hostname);
            ap_proxy_release_connection(scheme, conn, r->server);
            return rv;
        }
    }

    /* Set the timeout: Note that the default timeout logic in the proxy_util.c is:
     * 1 - worker->timeout (if timeout_set timeout=n in the worker)
     * 2 - conf->timeout (if timeout_set ProxyTimeout 300)
     * 3 - s->timeout (TimeOut 300).
     * We hack it... Via 1
     */
#if AP_MODULE_MAGIC_AT_LEAST(20051115,4)
    timeout = worker->ping_timeout;
#else
    helperping = worker->opaque;
    timeout = helperping->ping_timeout;
#endif
    if (timeout <= 0)
        timeout =  apr_time_from_sec(10); /* 10 seconds */

    savetimeout_set = worker->timeout_set;
    savetimeout = worker->timeout;
    worker->timeout_set = 1;
    worker->timeout = timeout;

    /* Connect to the backend: Check connected/reuse otherwise create new */
    rv = ap_proxy_connect_backend(scheme, conn, worker, r->server);
    worker->timeout_set = savetimeout_set;
    worker->timeout = savetimeout;
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "proxy_cluster_try_pingpong: can't connect to backend");
        ap_proxy_release_connection(scheme, conn, r->server);
        return rv;
    }

    /* XXX: For the moment we support only AJP */
    if (strcasecmp(scheme, "AJP") == 0) {
        rv = ajp_handle_cping_cpong(conn->sock, r, timeout);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                         "proxy_cluster_try_pingpong: cping_cpong failed");
            conn->close++;
        }
        
    }
    ap_proxy_release_connection(scheme, conn, r->server);
    return rv;
}
/*
 * Check that we could connect to the node
 * id   : worker id
 * load : load factor from the cluster manager.
 */
static int proxy_node_isup(request_rec *r, int id, char *scheme, int load)
{
    void *sconf = r->server->module_config;
    proxy_server_conf *conf = (proxy_server_conf *)
        ap_get_module_config(sconf, &proxy_module);
    int i;
    apr_status_t rv;
    proxy_worker *worker;

    /* create the workers (that could be the first time) */
    update_workers_node(conf, r->pool, r->server);

    /* search for the worker */ 
    worker = (proxy_worker *)conf->workers->elts;
    for (i = 0; i < conf->workers->nelts; i++) {
       if (worker->id == id)
           break;
       worker++;
    }
    if (i == conf->workers->nelts) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "proxy_cluster_isup: Can't find worker for %d", id);
        return 500;
    }

    /* Try a  ping/pong to check the node */
    if (load > 0) {
        /* Only try usuable nodes */
        rv = proxy_cluster_try_pingpong(r, worker, scheme);
        if (rv != APR_SUCCESS) {
            worker->s->status |= PROXY_WORKER_IN_ERROR;
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                         "proxy_cluster_isup: pingpong failed");
            return 500;
        }
    }
    if (load == -1) {
        worker->s->status |= PROXY_WORKER_IN_ERROR;
    }
    else if (load == 0) {
        /*
         * XXX: PROXY_WORKER_HOT_STANDBY Doesn't look supported
         * mark worker in error for the moment
         */
        worker->s->status |= PROXY_WORKER_IN_ERROR;
#if AP_MODULE_MAGIC_AT_LEAST(20051115,4)
        worker->s->status |= PROXY_WORKER_HOT_STANDBY;
#endif
    }
    else {
        worker->s->status &= ~PROXY_WORKER_IN_ERROR;
        worker->s->status &= ~PROXY_WORKER_STOPPED;
        worker->s->status &= ~PROXY_WORKER_DISABLED;
#if AP_MODULE_MAGIC_AT_LEAST(20051115,4)
        worker->s->status &= ~PROXY_WORKER_HOT_STANDBY;
#endif
        worker->s->lbfactor = load;
    }
    return 0;
}
/*
 * For the provider
 */
static const struct balancer_method balancerhandler =
{
    proxy_node_isup
};

/*
 * Remove node that have beeen marked removed for more than 10 seconds.
 */
static void remove_removed_node(apr_pool_t *pool)
{
    int *id, size, i;
    apr_time_t now = apr_time_now();

    /* read the ident of the nodes */
    id = apr_pcalloc(pool, sizeof(int) * node_storage->get_max_size_node());
    size = node_storage->get_ids_used_node(id);
    for (i=0; i<size; i++) {
        nodeinfo_t *ou;
        node_storage->read_node(id[i], &ou);
        if (ou->mess.remove && (now - ou->updatetime) >= apr_time_from_sec(WAITFORREMOVE)) {
            /* remove the node from the shared memory */
            node_storage->remove_node(ou);
        }
    }
}
static void * APR_THREAD_FUNC proxy_cluster_watchdog_func(apr_thread_t *thd, void *data)
{
    apr_pool_t *pool;
    server_rec *s = (server_rec *) data;
    void *sconf = s->module_config;
    proxy_server_conf *conf = (proxy_server_conf *)
        ap_get_module_config(sconf, &proxy_module);

    for (;;) {
        apr_sleep(apr_time_make(0, 1000));
        apr_pool_create(&pool, conf->pool);
        /* Create new workers if the shared memory changes */
        update_workers_node(conf, pool, s);
        /* cleanup removed node in shared memory */
        remove_removed_node(pool);
        apr_pool_destroy(pool);
    }
    apr_thread_exit(thd, 0);
    return NULL;
}

/*
 * Create a thread per process to make maintenance task.
 * and the mutex of the node creation.
 */
static void  proxy_cluster_child_init(apr_pool_t *p, server_rec *s)
{
    apr_status_t rv;
    apr_thread_t *wdt;

    rv = apr_thread_mutex_create(&lock, APR_THREAD_MUTEX_DEFAULT, p);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, s,
                    "proxy_cluster_child_init: apr_thread_mutex_create failed");
    }

    rv = apr_thread_create(&wdt, NULL, proxy_cluster_watchdog_func, s, p);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, s,
                    "proxy_cluster_child_init: apr_thread_create failed");
    }

}

static int proxy_cluster_post_config(apr_pool_t *p, apr_pool_t *plog,
                                     apr_pool_t *ptemp, server_rec *s)
{
    node_storage = ap_lookup_provider("manager" , "shared", "0");
    if (node_storage == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, s,
                    "proxy_cluster_post_config: Can't find mod_manager for nodes");
        return !OK;
    }
    host_storage = ap_lookup_provider("manager" , "shared", "1");
    if (host_storage == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, s,
                    "proxy_cluster_post_config: Can't find mod_manager for hosts");
        return !OK;
    }
    context_storage = ap_lookup_provider("manager" , "shared", "2");
    if (context_storage == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, s,
                    "proxy_cluster_post_config: Can't find mod_manager for contexts");
        return !OK;
    }
    balancer_storage = ap_lookup_provider("manager" , "shared", "3");
    if (balancer_storage == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, s,
                    "proxy_cluster_post_config: Can't find mod_manager for balancers");
        return !OK;
    }

    return OK;
}

/*
 * See if we could map the request.
 */
static int proxy_cluster_trans(request_rec *r)
{
    char *balancer = get_context_host_balancer(r);

    ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r->server,
                "proxy_cluster_trans for %d %s %s uri: %s",
                 r->proxyreq, r->filename, r->handler, r->uri);

    if (balancer) {
        r->filename =  apr_pstrcat(r->pool, "proxy:cluster://", balancer, r->uri, NULL);
        r->handler = "proxy-server";
        r->proxyreq = PROXYREQ_REVERSE;
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r->server,
                    "proxy_cluster_trans using %s uri: %s",
                     balancer, r->filename);
        return OK; /* Mod_proxy will process it */
    }
 
    return DECLINED;
}

/*
 * canonise the url
 */
static int proxy_cluster_canon(request_rec *r, char *url)
{
    if (strncasecmp(url, "balancer:", 9) == 0) {
        return OK; /* XXX: need more */
    }
    return DECLINED;
}

/* Find the worker that has the 'route' defined
 */
static proxy_worker *find_route_worker(proxy_balancer *balancer,
                                       const char *route, request_rec *r)
{
    int i;
    int checking_standby;
    int checked_standby;
    
    proxy_worker *worker;

    checking_standby = checked_standby = 0;
    while (!checked_standby) {
        worker = (proxy_worker *)balancer->workers->elts;
        for (i = 0; i < balancer->workers->nelts; i++, worker++) {
            if (worker->id == 0)
                continue; /* marked removed */

            if ( (checking_standby ? !PROXY_WORKER_IS_STANDBY(worker) : PROXY_WORKER_IS_STANDBY(worker)) )
                continue;
            if (*(worker->s->route) && strcmp(worker->s->route, route) == 0) {
                if (worker && PROXY_WORKER_IS_USABLE(worker)) {
                    return worker;
                } else {
                    /*
                     * If the worker is in error state run
                     * retry on that worker. It will be marked as
                     * operational if the retry timeout is elapsed.
                     * The worker might still be unusable, but we try
                     * anyway.
                     */
                    ap_proxy_retry_worker("BALANCER", worker, r->server);
                    if (PROXY_WORKER_IS_USABLE(worker)) {
                            return worker;
                    } else {
                        /*
                         * We have a worker that is unusable.
                         * It can be in error or disabled, but in case
                         * it has a redirection set use that redirection worker.
                         * This enables to safely remove the member from the
                         * balancer. Of course you will need some kind of
                         * session replication between those two remote.
                         */
                        if (*worker->s->redirect) {
                            proxy_worker *rworker = NULL;
                            rworker = find_route_worker(balancer, worker->s->redirect, r);
                            /* Check if the redirect worker is usable */
                            if (rworker && !PROXY_WORKER_IS_USABLE(rworker)) {
                                /*
                                 * If the worker is in error state run
                                 * retry on that worker. It will be marked as
                                 * operational if the retry timeout is elapsed.
                                 * The worker might still be unusable, but we try
                                 * anyway.
                                 */
                                ap_proxy_retry_worker("BALANCER", rworker, r->server);
                            }
                            if (rworker && PROXY_WORKER_IS_USABLE(rworker))
                                return rworker;
                        }
                    }
                }
            }
        }
        checked_standby = checking_standby++;
    }
    return NULL;
}
static proxy_worker *find_session_route(proxy_balancer *balancer,
                                        request_rec *r,
                                        char **route,
                                        char **sticky_used,
                                        char **url)
{
    proxy_worker *worker = NULL;
    char *sticky, *sticky_path, *path;

    if (!balancer->sticky)
        return NULL;
    sticky = sticky_path = apr_pstrdup(r->pool, balancer->sticky);
    if ((path = strchr(sticky, '|'))) {
        *path++ = '\0';
         sticky_path = path;
    }
    
    /* Try to find the sticky route inside url */
    *sticky_used = sticky_path;
    *route = get_path_param(r->pool, *url, sticky_path);
    if (!*route) {
        *route = get_cookie_param(r, sticky);
        *sticky_used = sticky;
    }
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                            "proxy: CLUSTER: Found value %s for "
                            "stickysession %s", *route, balancer->sticky);
    /*
     * If we found a value for sticksession, find the first '.' within.
     * Everything after '.' (if present) is our route.
     */
    if ((*route) && ((*route = strchr(*route, '.')) != NULL ))
        (*route)++;
    if ((*route) && (**route)) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                                  "proxy: CLUSTER: Found route %s", *route);
        /* We have a route in path or in cookie
         * Find the worker that has this route defined.
         */
        worker = find_route_worker(balancer, *route, r);
        if (worker && strcmp(*route, worker->s->route)) {
            /*
             * Notice that the route of the worker chosen is different from
             * the route supplied by the client.
             */
            apr_table_setn(r->subprocess_env, "BALANCER_ROUTE_CHANGED", "1");
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                         "proxy: CLUSTER: Route changed from %s to %s",
                         *route, worker->s->route);
        }
        return worker;
    }
    else
        return NULL;
}

static proxy_worker *find_best_worker(proxy_balancer *balancer,
                                      request_rec *r)
{
    proxy_worker *candidate = NULL;
    apr_status_t rv;

    if ((rv = PROXY_THREAD_LOCK(balancer)) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
        "proxy: CLUSTER: (%s). Lock failed for find_best_worker()", balancer->name);
        return NULL;
    }

    candidate = (*balancer->lbmethod->finder)(balancer, r);

    if (candidate) {
        proxy_cluster_helper *helper;
        candidate->s->elected++; /* mark it in use */
        helper = (proxy_cluster_helper *) candidate->opaque;
        helper->count_active++;
    }

    if ((rv = PROXY_THREAD_UNLOCK(balancer)) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
        "proxy: CLUSTER: (%s). Unlock failed for find_best_worker()", balancer->name);
    }

    if (candidate == NULL) {
        /* All the workers are in error state or disabled.
         * If the balancer has a timeout sleep for a while
         * and try again to find the worker. The chances are
         * that some other thread will release a connection.
         * By default the timeout is not set, and the server
         * returns SERVER_BUSY.
         */
#if APR_HAS_THREADS
        if (balancer->timeout) {
            /* XXX: This can perhaps be build using some
             * smarter mechanism, like tread_cond.
             * But since the statuses can came from
             * different childs, use the provided algo.
             */
            apr_interval_time_t timeout = balancer->timeout;
            apr_interval_time_t step, tval = 0;
            /* Set the timeout to 0 so that we don't
             * end in infinite loop
             */
            balancer->timeout = 0;
            step = timeout / 100;
            while (tval < timeout) {
                apr_sleep(step);
                /* Try again */
                if ((candidate = find_best_worker(balancer, r)))
                    break;
                tval += step;
            }
            /* restore the timeout */
            balancer->timeout = timeout;
        }
#endif
    }
    return candidate;
}

static int rewrite_url(request_rec *r, proxy_worker *worker,
                        char **url)
{
    const char *scheme = strstr(*url, "://");
    const char *path = NULL;

    if (scheme)
        path = ap_strchr_c(scheme + 3, '/');

    /* we break the URL into host, port, uri */
    if (!worker) {
        return ap_proxyerror(r, HTTP_BAD_REQUEST, apr_pstrcat(r->pool,
                             "missing worker. URI cannot be parsed: ", *url,
                             NULL));
    }

    *url = apr_pstrcat(r->pool, worker->name, path, NULL);

    return OK;
}

/*
 * Find a worker for mod_proxy logic
 */
static int proxy_cluster_pre_request(proxy_worker **worker,
                                      proxy_balancer **balancer,
                                      request_rec *r,
                                      proxy_server_conf *conf, char **url)
{
    int access_status;
    proxy_worker *runtime;
    char *route = NULL;
    char *sticky = NULL;
    apr_status_t rv;

    *worker = NULL;
    /* Step 1: check if the url is for us
     * The url we can handle starts with 'balancer://'
     * If balancer is already provided skip the search
     * for balancer, because this is failover attempt.
     */
    apr_thread_mutex_lock(lock);
    if (!*balancer &&
        !(*balancer = ap_proxy_get_balancer(r->pool, conf, *url))) {
        apr_thread_mutex_unlock(lock);
        return DECLINED;
    }

    /* Step 2: find the session route */

    runtime = find_session_route(*balancer, r, &route, &sticky, url);
    apr_thread_mutex_unlock(lock);

    /* Lock the LoadBalancer
     * XXX: perhaps we need the process lock here
     */
    if ((rv = PROXY_THREAD_LOCK(*balancer)) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                     "proxy: CLUSTER: (%s). Lock failed for pre_request",
                     (*balancer)->name);
        return DECLINED;
    }
    if (runtime) {
        int i, total_factor = 0;
        proxy_worker *workers;
        /* We have a sticky load balancer
         * Update the workers status
         * so that even session routes get
         * into account.
         */
        workers = (proxy_worker *)(*balancer)->workers->elts;
        for (i = 0; i < (*balancer)->workers->nelts; i++) {
            /* Take into calculation only the workers that are
             * not in error state or not disabled.
             *
             * TODO: Abstract the below, since this is dependent
             *       on the LB implementation
             */
            if (PROXY_WORKER_IS_USABLE(workers)) {
                workers->s->lbstatus += workers->s->lbfactor;
                total_factor += workers->s->lbfactor;
            }
            workers++;
        }
        runtime->s->lbstatus -= total_factor;
        runtime->s->elected++;

        *worker = runtime;
    }
    else if (route && (*balancer)->sticky_force) {
        int i, member_of = 0;
        proxy_worker *workers;
        /*
         * We have a route provided that doesn't match the
         * balancer name. See if the provider route is the
         * member of the same balancer in which case return 503
         */
        workers = (proxy_worker *)(*balancer)->workers->elts;
        for (i = 0; i < (*balancer)->workers->nelts; i++) {
            if (*(workers->s->route) && strcmp(workers->s->route, route) == 0) {
                member_of = 1;
                break;
            }
            workers++;
        }
        if (member_of) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                         "proxy: CLUSTER: (%s). All workers are in error state for route (%s)",
                         (*balancer)->name, route);
            if ((rv = PROXY_THREAD_UNLOCK(*balancer)) != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                             "proxy: CLUSTER: (%s). Unlock failed for pre_request",
                             (*balancer)->name);
            }
            return HTTP_SERVICE_UNAVAILABLE;
        }
    }

    /* Mark the worker used for the cleanup logic */
    if (*worker) {
        proxy_cluster_helper *helper;
        helper = (proxy_cluster_helper *) (*worker)->opaque;
        helper->count_active++;
    }
    if ((rv = PROXY_THREAD_UNLOCK(*balancer)) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                     "proxy: CLUSTER: (%s). Unlock failed for pre_request",
                     (*balancer)->name);
    }
    if (!*worker) {
        runtime = find_best_worker(*balancer, r);
        if (!runtime) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                         "proxy: CLUSTER: (%s). All workers are in error state",
                         (*balancer)->name);

            return HTTP_SERVICE_UNAVAILABLE;
        }
        if ((*balancer)->sticky && runtime) {
            /*
             * This balancer has sticky sessions and the client either has not
             * supplied any routing information or all workers for this route
             * including possible redirect and hotstandby workers are in error
             * state, but we have found another working worker for this
             * balancer where we can send the request. Thus notice that we have
             * changed the route to the backend.
             */
            apr_table_setn(r->subprocess_env, "BALANCER_ROUTE_CHANGED", "1");
        }
        *worker = runtime;
    }

    /* Add balancer/worker info to env. */
    apr_table_setn(r->subprocess_env,
                   "BALANCER_NAME", (*balancer)->name);
    apr_table_setn(r->subprocess_env,
                   "BALANCER_WORKER_NAME", (*worker)->name);
    apr_table_setn(r->subprocess_env,
                   "BALANCER_WORKER_ROUTE", (*worker)->s->route);

    /* Rewrite the url from 'balancer://url'
     * to the 'worker_scheme://worker_hostname[:worker_port]/url'
     * This replaces the balancers fictional name with the
     * real hostname of the elected worker.
     */
    access_status = rewrite_url(r, *worker, url);
    /* Add the session route to request notes if present */
    if (route) {
        apr_table_setn(r->notes, "session-sticky", sticky);
        apr_table_setn(r->notes, "session-route", route);

        /* Add session info to env. */
        apr_table_setn(r->subprocess_env,
                       "BALANCER_SESSION_STICKY", sticky);
        apr_table_setn(r->subprocess_env,
                       "BALANCER_SESSION_ROUTE", route);
    }
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                 "proxy: CLUSTER (%s) worker (%s) rewritten to %s",
                 (*balancer)->name, (*worker)->name, *url);

    return access_status;
}

static int proxy_cluster_post_request(proxy_worker *worker,
                                       proxy_balancer *balancer,
                                       request_rec *r,
                                       proxy_server_conf *conf)
{

    apr_status_t rv;
    proxy_cluster_helper *helper;

    if ((rv = PROXY_THREAD_LOCK(balancer)) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
            "proxy: CLUSTER: (%s). Lock failed for post_request",
            balancer->name);
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    /* mark the work as not use */
    helper = (proxy_cluster_helper *) worker->opaque;
    helper->count_active--;

    if ((rv = PROXY_THREAD_UNLOCK(balancer)) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
            "proxy: CLUSTER: (%s). Unlock failed for post_request",
            balancer->name);
    }
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                 "proxy_cluster_post_request for (%s)", balancer->name);

    return OK;
}

/*
 * Register the hooks on our module.
 */
static void proxy_cluster_hooks(apr_pool_t *p)
{
    static const char * const aszPre[]={ "mod_manager.c", NULL };
    static const char * const aszSucc[]={ "mod_proxy.c", NULL };

    ap_hook_post_config(proxy_cluster_post_config, NULL, NULL, APR_HOOK_MIDDLE);

    /* create the provider for the proxy logic */
    ap_register_provider(p, PROXY_LBMETHOD, "cluster_byrequests", "0", &byrequests);

    /* create the "maintenance" thread */
    ap_hook_child_init(proxy_cluster_child_init, NULL, NULL, APR_HOOK_MIDDLE);

    /* check the url and give the mapping to mod_proxy */
    ap_hook_translate_name(proxy_cluster_trans, aszPre, aszSucc, APR_HOOK_FIRST);

    proxy_hook_canon_handler(proxy_cluster_canon, NULL, NULL, APR_HOOK_FIRST);
 
    proxy_hook_pre_request(proxy_cluster_pre_request, NULL, NULL, APR_HOOK_FIRST);
    proxy_hook_post_request(proxy_cluster_post_request, NULL, NULL, APR_HOOK_FIRST);

    ap_register_provider(p, "proxy_cluster" , "balancer", "0", &balancerhandler);
}

/* XXX: not needed
static void *create_proxy_cluster_dir_config(apr_pool_t *p, char *dir)
{
    return NULL;
}
 */

static void *create_proxy_cluster_server_config(apr_pool_t *p, server_rec *s)
{
    return NULL;
}

module AP_MODULE_DECLARE_DATA proxy_cluster_module = {
    STANDARD20_MODULE_STUFF,
    NULL,    /* per-directory config creator */
    NULL,                               /* dir config merger */
    create_proxy_cluster_server_config, /* server config creator */
    NULL,                               /* server config merger */
    NULL,                               /* command table */
    proxy_cluster_hooks                 /* register hooks */
};
