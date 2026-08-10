#ifndef VECTIS_KORE_HOOKS_H
#define VECTIS_KORE_HOOKS_H

struct connection;
struct http_request;

int vectis_kore_autoblock_accept(struct connection *connection);
void vectis_kore_autoblock_tcp_stall(struct connection *connection);
void vectis_kore_autoblock_tls_failure(struct connection *connection);
int vectis_kore_autoblock_request_allowed(struct http_request *request);
void vectis_kore_autoblock_http_status(struct http_request *request,
                                       int status);
void vectis_kore_autoblock_connection_status(struct connection *connection,
                                             int status);
void vectis_kore_autoblock_request_event(struct http_request *request,
                                         const char *name);

#endif
