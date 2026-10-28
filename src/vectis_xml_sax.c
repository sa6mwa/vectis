#include <libxml/entities.h>
#include <libxml/parser.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vectis/xml.h>
typedef struct xml_sax_state {
  vectis_xml_sax receiver;
  xmlSAXHandler handler;
  xmlParserCtxt *native;
  vectis_xml_sax_callbacks callbacks;
  void *context;
  vectis_xml_attribute attributes[VECTIS_XML_SAX_MAX_ATTRIBUTES];
  vectis_xml_namespace namespaces[VECTIS_XML_SAX_MAX_ATTRIBUTES];
  size_t metadata_limit;
  vectis_error failure;
  int busy, violated, finished;
  pid_t process;
  pthread_t thread;
} xml_sax_state;
static vectis_status sax_failure(vectis_error *error, vectis_status status,
                                 const char *message) {
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->code = status;
    error->source = VECTIS_ERROR_SOURCE_CPKT;
    strncpy(error->message, message, sizeof(error->message) - 1u);
  }
  return status;
}
static void sax_stop(xml_sax_state *state, vectis_status status,
                     const char *message) {
  if (state->failure.code == VECTIS_OK)
    sax_failure(&state->failure, status, message);
  xmlStopParser(state->native);
}
static void sax_native_error(void *context, const xmlError *native) {
  xml_sax_state *state = (xml_sax_state *)context;
  if (native == NULL || native->level < XML_ERR_ERROR ||
      state->failure.code != VECTIS_OK)
    return;
  sax_failure(
      &state->failure,
      native->code == XML_ERR_NO_MEMORY ? VECTIS_ERR_NOMEM : VECTIS_ERR_STATE,
      native->message == NULL ? "XML SAX parse failed" : native->message);
  state->failure.dependency_code = native->code;
  (void)snprintf(state->failure.detail, sizeof(state->failure.detail),
                 "line %d, column %d", native->line, native->int2);
}
static xmlParserErrors sax_no_resource(void *context, const char *url,
                                       const char *public_id,
                                       xmlResourceType type,
                                       xmlParserInputFlags flags,
                                       xmlParserInput **out) {
  xml_sax_state *state = (xml_sax_state *)context;
  (void)url;
  (void)public_id;
  (void)type;
  (void)flags;
  *out = NULL;
  sax_stop(state, VECTIS_ERR_INVALID, "external XML resources are disabled");
  return XML_IO_LOAD_ERROR;
}
static void sax_doctype(void *context, const xmlChar *name,
                        const xmlChar *external_id, const xmlChar *system_id) {
  (void)name;
  (void)external_id;
  (void)system_id;
  sax_stop((xml_sax_state *)context, VECTIS_ERR_INVALID,
           "DTD/entity declarations are outside this SAX surface");
}
static xmlEntity *sax_entity(void *context, const xmlChar *name) {
  (void)context;
  return xmlGetPredefinedEntity(name);
}
static int sax_metadata(xml_sax_state *state, size_t *total, const void *data,
                        size_t size) {
  (void)data;
  if (size > state->metadata_limit - *total) {
    sax_stop(state, VECTIS_ERR_INVALID, "SAX event exceeds max_metadata_bytes");
    return 0;
  }
  *total += size;
  return 1;
}
static int sax_metadata_string(xml_sax_state *state, size_t *total,
                               const xmlChar *value) {
  return sax_metadata(state, total, value,
                      value == NULL ? 0u : (size_t)xmlStrlen(value));
}
static void sax_result(xml_sax_state *state, vectis_status status,
                       const vectis_error *error) {
  if (status != VECTIS_OK && state->failure.code == VECTIS_OK) {
    if (error->code != VECTIS_OK)
      state->failure = *error;
    else
      sax_failure(&state->failure, status, "SAX callback failed");
  }
  if (state->violated && state->failure.code == VECTIS_OK)
    sax_failure(&state->failure, VECTIS_ERR_STATE,
                "SAX callback attempted receiver reentry/close");
  if (state->failure.code != VECTIS_OK)
    xmlStopParser(state->native);
}
static void sax_start(void *context, const xmlChar *name, const xmlChar *prefix,
                      const xmlChar *uri, int namespace_count,
                      const xmlChar **namespaces, int attribute_count,
                      int defaulted, const xmlChar **attributes) {
  xml_sax_state *state = (xml_sax_state *)context;
  vectis_xml_name element;
  vectis_error error;
  vectis_status status;
  size_t total = 0u, size;
  int i;
  (void)defaulted;
  if (state->failure.code != VECTIS_OK)
    return;
  if (namespace_count < 0 || attribute_count < 0 ||
      namespace_count > (int)VECTIS_XML_SAX_MAX_ATTRIBUTES ||
      attribute_count > (int)VECTIS_XML_SAX_MAX_ATTRIBUTES - namespace_count) {
    sax_stop(state, VECTIS_ERR_INVALID,
             "SAX attribute/namespace count exceeds 256");
    return;
  }
  if (!sax_metadata_string(state, &total, name) ||
      !sax_metadata_string(state, &total, prefix) ||
      !sax_metadata_string(state, &total, uri))
    return;
  element.name = (const char *)name;
  element.namespace_uri = (const char *)uri;
  element.prefix = (const char *)prefix;
  for (i = 0; i < namespace_count; ++i) {
    if (!sax_metadata_string(state, &total, namespaces[i * 2]) ||
        !sax_metadata_string(state, &total, namespaces[i * 2 + 1]))
      return;
    state->namespaces[i].prefix = (const char *)namespaces[i * 2];
    state->namespaces[i].uri = namespaces[i * 2 + 1] == NULL
                                   ? ""
                                   : (const char *)namespaces[i * 2 + 1];
  }
  for (i = 0; i < attribute_count; ++i) {
    if (attributes[i * 5 + 3] == NULL ||
        attributes[i * 5 + 4] < attributes[i * 5 + 3]) {
      sax_stop(state, VECTIS_ERR_STATE, "invalid native SAX attribute view");
      return;
    }
    size = (size_t)(attributes[i * 5 + 4] - attributes[i * 5 + 3]);
    if (!sax_metadata_string(state, &total, attributes[i * 5]) ||
        !sax_metadata_string(state, &total, attributes[i * 5 + 1]) ||
        !sax_metadata_string(state, &total, attributes[i * 5 + 2]) ||
        !sax_metadata(state, &total, attributes[i * 5 + 3], size))
      return;
    state->attributes[i].name.name = (const char *)attributes[i * 5];
    state->attributes[i].name.prefix = (const char *)attributes[i * 5 + 1];
    state->attributes[i].name.namespace_uri =
        (const char *)attributes[i * 5 + 2];
    state->attributes[i].value = attributes[i * 5 + 3];
    state->attributes[i].size = size;
  }
  if (state->callbacks.start_element == NULL)
    return;
  memset(&error, 0, sizeof(error));
  status = state->callbacks.start_element(
      state->context, &element, state->attributes, (size_t)attribute_count,
      state->namespaces, (size_t)namespace_count, &error);
  sax_result(state, status, &error);
}
static void sax_end(void *context, const xmlChar *name, const xmlChar *prefix,
                    const xmlChar *uri) {
  xml_sax_state *state = (xml_sax_state *)context;
  vectis_xml_name element;
  vectis_error error;
  vectis_status status;
  if (state->failure.code != VECTIS_OK || state->callbacks.end_element == NULL)
    return;
  element.name = (const char *)name;
  element.namespace_uri = (const char *)uri;
  element.prefix = (const char *)prefix;
  memset(&error, 0, sizeof(error));
  status = state->callbacks.end_element(state->context, &element, &error);
  sax_result(state, status, &error);
}
static void sax_text(void *context, const xmlChar *data, int length) {
  xml_sax_state *state = (xml_sax_state *)context;
  vectis_error error;
  vectis_status status;
  size_t remaining, size;
  if (state->failure.code != VECTIS_OK || state->callbacks.text == NULL ||
      length <= 0)
    return;
  remaining = (size_t)length;
  while (remaining != 0u && state->failure.code == VECTIS_OK) {
    size = remaining > VECTIS_XML_SAX_CHUNK_BYTES ? VECTIS_XML_SAX_CHUNK_BYTES
                                                  : remaining;
    if (size < remaining)
      while (size != 0u && (((unsigned char)data[size] & 0xc0u) == 0x80u))
        --size;
    memset(&error, 0, sizeof(error));
    status = state->callbacks.text(state->context, data, size, &error);
    sax_result(state, status, &error);
    data += size;
    remaining -= size;
  }
}
static vectis_status sax_check(xml_sax_state *state, vectis_error *error) {
  if (state == NULL)
    return sax_failure(error, VECTIS_ERR_INVALID, "missing SAX receiver");
  if (state->process != getpid() ||
      !pthread_equal(state->thread, pthread_self()))
    return sax_failure(error, VECTIS_ERR_STATE,
                       "SAX receiver belongs to another process/thread");
  if (state->busy) {
    state->violated = 1;
    return sax_failure(error, VECTIS_ERR_STATE,
                       "cannot reenter or close active SAX receiver");
  }
  if (state->failure.code != VECTIS_OK) {
    if (error != NULL)
      *error = state->failure;
    return state->failure.code;
  }
  return VECTIS_OK;
}
vectis_status vectis_xml_sax_new(const vectis_xml_sax_callbacks *callbacks,
                                 void *context, size_t metadata_limit,
                                 vectis_xml_sax **out, vectis_error *error) {
  xml_sax_state *state;
  if (out != NULL)
    *out = NULL;
  if (callbacks == NULL || out == NULL)
    return sax_failure(error, VECTIS_ERR_INVALID,
                       "missing SAX callbacks/output");
  state = (xml_sax_state *)calloc(1u, sizeof(*state));
  if (state == NULL)
    return sax_failure(error, VECTIS_ERR_NOMEM, "allocating SAX receiver");
  state->callbacks = *callbacks;
  state->context = context;
  state->metadata_limit =
      metadata_limit == 0u ? VECTIS_XML_DEFAULT_MAX_BYTES : metadata_limit;
  state->process = getpid();
  state->thread = pthread_self();
  state->handler.initialized = XML_SAX2_MAGIC;
  state->handler.startElementNs = sax_start;
  state->handler.endElementNs = sax_end;
  state->handler.characters = sax_text;
  state->handler.cdataBlock = sax_text;
  state->handler.ignorableWhitespace = sax_text;
  state->handler.internalSubset = sax_doctype;
  state->handler.getEntity = sax_entity;
  state->native =
      xmlCreatePushParserCtxt(&state->handler, state, NULL, 0, NULL);
  if (state->native == NULL) {
    free(state);
    return sax_failure(error, VECTIS_ERR_NOMEM, "allocating native SAX parser");
  }
  xmlCtxtSetErrorHandler(state->native, sax_native_error, state);
  xmlCtxtSetResourceLoader(state->native, sax_no_resource, state);
  (void)xmlCtxtUseOptions(state->native,
                          XML_PARSE_NONET | XML_PARSE_NO_XXE | XML_PARSE_NOENT);
  state->receiver.feed = vectis_xml_sax_feed;
  state->receiver.finish = vectis_xml_sax_finish;
  state->receiver.close = vectis_xml_sax_close;
  state->receiver.impl = state;
  *out = &state->receiver;
  return VECTIS_OK;
}
static vectis_status sax_process(vectis_xml_sax *receiver, const void *data,
                                 size_t size, int finish, vectis_error *error) {
  xml_sax_state *state =
      receiver == NULL ? NULL : (xml_sax_state *)receiver->impl;
  vectis_status status = sax_check(state, error);
  int code;
  if (status != VECTIS_OK)
    return status;
  if (size > VECTIS_XML_SAX_CHUNK_BYTES || (data == NULL && size != 0u))
    return sax_failure(error, VECTIS_ERR_INVALID,
                       "SAX feed requires <=65536 bytes");
  if (state->finished)
    return finish
               ? VECTIS_OK
               : sax_failure(error, VECTIS_ERR_STATE, "SAX parser is finished");
  state->busy = 1;
  code = xmlParseChunk(state->native, (const char *)data, (int)size, finish);
  state->busy = 0;
  if (code != 0 && state->failure.code == VECTIS_OK) {
    sax_failure(&state->failure, VECTIS_ERR_STATE, "XML SAX parse failed");
    state->failure.dependency_code = code;
  }
  if (state->failure.code != VECTIS_OK) {
    if (error != NULL)
      *error = state->failure;
    return state->failure.code;
  }
  if (finish)
    state->finished = 1;
  return VECTIS_OK;
}
vectis_status vectis_xml_sax_feed(vectis_xml_sax *receiver, const void *data,
                                  size_t size, vectis_error *error) {
  return sax_process(receiver, data, size, 0, error);
}
vectis_status vectis_xml_sax_finish(vectis_xml_sax *receiver,
                                    vectis_error *error) {
  return sax_process(receiver, NULL, 0u, 1, error);
}
vectis_status vectis_xml_sax_close(vectis_xml_sax *receiver,
                                   vectis_error *error) {
  xml_sax_state *state;
  if (receiver == NULL)
    return VECTIS_OK;
  state = (xml_sax_state *)receiver->impl;
  if (state->process != getpid() ||
      !pthread_equal(state->thread, pthread_self()))
    return sax_failure(error, VECTIS_ERR_STATE,
                       "cannot close foreign-process/thread SAX receiver");
  if (state->busy) {
    state->violated = 1;
    return sax_failure(error, VECTIS_ERR_STATE,
                       "cannot close active SAX receiver");
  }
  xmlFreeParserCtxt(state->native);
  free(state);
  return VECTIS_OK;
}
