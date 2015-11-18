
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


static ngx_int_t ngx_http_postpone_filter_add(ngx_http_request_t *r,
    ngx_chain_t *in);
static ngx_int_t ngx_http_postpone_filter_init(ngx_conf_t *cf);


static ngx_http_module_t  ngx_http_postpone_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_postpone_filter_init,         /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_postpone_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_postpone_filter_module_ctx,  /* module context */
    NULL,                                  /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


static ngx_int_t
ngx_http_postpone_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_connection_t              *c;
    ngx_http_postponed_request_t  *pr;

    c = r->connection;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http postpone filter \"%V?%V\" %p", &r->uri, &r->args, in);

    // c->data保存最新的一个subrequest。 
    // 当前请求不能往out chain发送数据，如果产生了数据，新建一个节点， 
    // 将它保存在当前请求的postponed队尾。这样就保证了数据按序发到客户端
    if (r != c->data) {

        if (in) {
            // 保存数据
            ngx_http_postpone_filter_add(r, in);
            // 这儿不发送任何数据，直接返回OK。最终会在ngx_http_finalize_request中处理
            return NGX_OK;
        }

#if 0
        /* TODO: SSI may pass NULL */
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "http postpone filter NULL inactive request",
                      &r->uri, &r->args);
#endif

        return NGX_OK;
    }

    // 如果r->postponed为空，则说明这是最后一个subrequest，也就是最新那个，因此将它发送出去。
    // 这里，表示当前请求可以往out chain发送数据，如果它的postponed链表中没有子请求，也没有数据， 则直接发送当前产生的数据in或者继续发送out chain中之前没有发送完成的数据
    if (r->postponed == NULL) {

        if (in || c->buffered) {
            return ngx_http_next_body_filter(r->main, in);
        }

        return NGX_OK;
    }

    if (in) {
        // 如果有chain 则保存数据
        ngx_http_postpone_filter_add(r, in);
    }

    do {
        pr = r->postponed;

        // 如果存在request 则说明这个postponed request是sub request，因此需要将其放入mainrequest的post_reqeust中
        if (pr->request) {

            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "http postpone filter wake \"%V?%V\"",
                           &pr->request->uri, &pr->request->args);

            r->postponed = pr->next;

            //按照后续遍历产生的序列，因为当前请求（节点）有未处理的子请求(节点)， 必须先处理完改子请求，才能继续处理后面的子节点。 这里将该子请求设置为可以往out chain发送数据的请求
            c->data = pr->request;

            return ngx_http_post_request(pr->request, NULL);
        }

        if (pr->out == NULL) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "http postpone filter NULL output",
                          &r->uri, &r->args);

        } else {
            // 如果pr->out不为空，此时需要将保存的父request的数据发送
            // 如果该节点保存的是数据，可以直接处理该节点，将它发送到out chain 
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "http postpone filter output \"%V?%V\"",
                           &r->uri, &r->args);

            if (ngx_http_next_body_filter(r->main, pr->out) == NGX_ERROR) {
                return NGX_ERROR;
            }
        }

        r->postponed = pr->next;

    } while (r->postponed);

    return NGX_OK;
}


//拷贝当前需要发送的chain到postponed的out域中
static ngx_int_t
ngx_http_postpone_filter_add(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_postponed_request_t  *pr, **ppr;

    // 如果r->postponed存在，则
    if (r->postponed) {
        // 找到postponed尾部
        for (pr = r->postponed; pr->next; pr = pr->next) { /* void */ }

        // 如果为空，则直接添加到当前的chain
        if (pr->request == NULL) {
            goto found;
        }

        ppr = &pr->next;

    } else {
        ppr = &r->postponed;
    }

    pr = ngx_palloc(r->pool, sizeof(ngx_http_postponed_request_t));
    if (pr == NULL) {
        return NGX_ERROR;
    }

    *ppr = pr;

    // 可以看到request是空
    pr->request = NULL;
    pr->out = NULL;
    pr->next = NULL;

found:

    //最终复制in到pr->out,也就是保存request 需要发送的数据
    if (ngx_chain_add_copy(r->pool, &pr->out, in) == NGX_OK) {
        return NGX_OK;
    }

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_postpone_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_postpone_filter;

    return NGX_OK;
}
