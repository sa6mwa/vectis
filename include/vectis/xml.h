#ifndef VECTIS_XML_H
#define VECTIS_XML_H
#include <vectis/vectis.h>
#ifdef __cplusplus
extern "C" {
#endif
#define VECTIS_XML_DEFAULT_MAX_BYTES 16777216u
#define VECTIS_XML_DEFAULT_MAX_XPATH_NODES 4096u
#define VECTIS_XML_SAX_CHUNK_BYTES 65536u
#define VECTIS_XML_SAX_MAX_ATTRIBUTES 256u
typedef struct vectis_xml_document vectis_xml_document;
typedef struct vectis_xml_node vectis_xml_node;
typedef struct vectis_xml_sax vectis_xml_sax;
typedef struct vectis_xml_document_config {
  size_t max_bytes, max_xpath_nodes;
} vectis_xml_document_config;
typedef struct vectis_xml_name {
  const char *name, *namespace_uri, *prefix;
} vectis_xml_name;
typedef struct vectis_xml_namespace {
  const char *prefix, *uri;
} vectis_xml_namespace;
typedef struct vectis_xml_attribute {
  vectis_xml_name name;
  const void *value;
  size_t size;
} vectis_xml_attribute;
typedef struct vectis_xml_child_config {
  const char *namespace_uri, *prefix;
  const void *text;
  size_t text_size;
  int text_present;
} vectis_xml_child_config;
typedef enum vectis_xml_xpath_kind {
  VECTIS_XML_XPATH_NODES = 1,
  VECTIS_XML_XPATH_STRING = 2,
  VECTIS_XML_XPATH_NUMBER = 3,
  VECTIS_XML_XPATH_BOOLEAN = 4
} vectis_xml_xpath_kind;
typedef struct vectis_xml_xpath_result {
  vectis_xml_xpath_kind kind;
  vectis_xml_node **nodes;
  size_t count;
  vectis_mutable_bytes string;
  double number;
  int boolean;
} vectis_xml_xpath_result;
/* Owned materialized DOM, caller-serialized and process/thread-confined.
 * Views represent elements only and retain parent state. Every mutation
 * invalidates previous views, including its receiver. Name strings are borrowed
 * until mutation/close; returned byte copies are caller-owned. DTDs are outside
 * this slice; external resource loaders are disabled per parser context. */
struct vectis_xml_document {
  vectis_status (*root)(vectis_xml_document *, vectis_xml_node **,
                        vectis_error *);
  vectis_status (*xpath)(vectis_xml_document *, const char *,
                         const vectis_xml_namespace *, size_t,
                         vectis_xml_xpath_result *, vectis_error *);
  vectis_status (*serialize_materialized)(vectis_xml_document *, int,
                                          vectis_mutable_bytes *,
                                          vectis_error *);
  vectis_status (*close)(vectis_xml_document *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
struct vectis_xml_node {
  vectis_status (*name)(vectis_xml_node *, vectis_xml_name *, vectis_error *);
  vectis_status (*text)(vectis_xml_node *, vectis_mutable_bytes *,
                        vectis_error *);
  vectis_status (*attribute)(vectis_xml_node *, const char *, const char *,
                             vectis_mutable_bytes *, int *, vectis_error *);
  vectis_status (*set_text)(vectis_xml_node *, const void *, size_t,
                            vectis_error *);
  vectis_status (*set_attribute)(vectis_xml_node *, const char *, const void *,
                                 size_t, const char *, const char *,
                                 vectis_error *);
  vectis_status (*append_child)(vectis_xml_node *, const char *,
                                const vectis_xml_child_config *,
                                vectis_xml_node **, vectis_error *);
  vectis_status (*remove)(vectis_xml_node *, vectis_error *);
  vectis_status (*close)(vectis_xml_node *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
void vectis_xml_document_config_init(vectis_xml_document_config *);
vectis_status vectis_xml_document_parse(const void *, size_t,
                                        const vectis_xml_document_config *,
                                        vectis_xml_document **, vectis_error *);
vectis_status vectis_xml_document_root(vectis_xml_document *,
                                       vectis_xml_node **, vectis_error *);
vectis_status vectis_xml_document_xpath(vectis_xml_document *, const char *,
                                        const vectis_xml_namespace *, size_t,
                                        vectis_xml_xpath_result *,
                                        vectis_error *);
void vectis_xml_xpath_result_cleanup(vectis_xml_xpath_result *);
vectis_status vectis_xml_document_serialize_materialized(vectis_xml_document *,
                                                         int,
                                                         vectis_mutable_bytes *,
                                                         vectis_error *);
vectis_status vectis_xml_document_close(vectis_xml_document *, vectis_error *);
vectis_status vectis_xml_node_name(vectis_xml_node *, vectis_xml_name *,
                                   vectis_error *);
vectis_status vectis_xml_node_text(vectis_xml_node *, vectis_mutable_bytes *,
                                   vectis_error *);
vectis_status vectis_xml_node_attribute(vectis_xml_node *, const char *,
                                        const char *, vectis_mutable_bytes *,
                                        int *, vectis_error *);
vectis_status vectis_xml_node_set_text(vectis_xml_node *, const void *, size_t,
                                       vectis_error *);
vectis_status vectis_xml_node_set_attribute(vectis_xml_node *, const char *,
                                            const void *, size_t, const char *,
                                            const char *, vectis_error *);
vectis_status vectis_xml_node_append_child(vectis_xml_node *, const char *,
                                           const vectis_xml_child_config *,
                                           vectis_xml_node **, vectis_error *);
vectis_status vectis_xml_node_remove(vectis_xml_node *, vectis_error *);
vectis_status vectis_xml_node_close(vectis_xml_node *, vectis_error *);

typedef struct vectis_xml_sax_callbacks {
  vectis_status (*start_element)(void *, const vectis_xml_name *,
                                 const vectis_xml_attribute *, size_t,
                                 const vectis_xml_namespace *, size_t,
                                 vectis_error *);
  vectis_status (*end_element)(void *, const vectis_xml_name *, vectis_error *);
  vectis_status (*text)(void *, const void *, size_t, vectis_error *);
} vectis_xml_sax_callbacks;
/* Synchronous bounded SAX flow, no DOM or cumulative stream-size cap. Callback
 * data is borrowed only during the call. Callbacks must not longjmp; failure
 * latches the parser. No callback runs from close. Context remains
 * caller-owned. metadata_limit=0 selects 16 MiB per start event;
 * attrs/declarations total<=256.
 */
struct vectis_xml_sax {
  vectis_status (*feed)(vectis_xml_sax *, const void *, size_t, vectis_error *);
  vectis_status (*finish)(vectis_xml_sax *, vectis_error *);
  vectis_status (*close)(vectis_xml_sax *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
vectis_status vectis_xml_sax_new(const vectis_xml_sax_callbacks *, void *,
                                 size_t, vectis_xml_sax **, vectis_error *);
vectis_status vectis_xml_sax_feed(vectis_xml_sax *, const void *, size_t,
                                  vectis_error *);
vectis_status vectis_xml_sax_finish(vectis_xml_sax *, vectis_error *);
vectis_status vectis_xml_sax_close(vectis_xml_sax *, vectis_error *);
#ifdef __cplusplus
}
#endif
#endif
