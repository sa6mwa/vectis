#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vectis/xml.h>
static const char document[] =
    "<r xmlns='urn:r' xmlns:q='urn:q'><q:x a=''/><q:x a='2'>a&amp;b</q:x></r>";
static void native_dom(void) {
  vectis_xml_document *doc = NULL;
  vectis_xml_node *root = NULL, *child = NULL, *old = NULL;
  vectis_xml_document_config config;
  vectis_xml_child_config child_config;
  vectis_xml_namespace namespaces[1];
  vectis_xml_xpath_result result;
  vectis_xml_name name;
  vectis_mutable_bytes bytes;
  vectis_error error;
  int found;
  char *input = (char *)malloc(sizeof(document));
  assert(input != NULL);
  memcpy(input, document, sizeof(document));
  vectis_xml_document_config_init(&config);
  assert(vectis_xml_document_parse(input, sizeof(document) - 1u, &config, &doc,
                                   &error) == VECTIS_OK);
  memset(input, 'x', sizeof(document));
  free(input);
  assert(doc->root(doc, &root, &error) == VECTIS_OK);
  assert(root->name(root, &name, &error) == VECTIS_OK &&
         strcmp(name.name, "r") == 0 &&
         strcmp(name.namespace_uri, "urn:r") == 0 && name.prefix == NULL);
  namespaces[0].prefix = "q";
  namespaces[0].uri = "urn:q";
  assert(doc->xpath(doc, "//q:x", namespaces, 1u, &result, &error) ==
             VECTIS_OK &&
         result.count == 2u);
  assert(result.nodes[0]->attribute(result.nodes[0], "a", NULL, &bytes, &found,
                                    &error) == VECTIS_OK &&
         found && bytes.size == 0u);
  vectis_mutable_bytes_cleanup(&bytes);
  assert(result.nodes[1]->text(result.nodes[1], &bytes, &error) == VECTIS_OK &&
         bytes.size == 3u && memcmp(bytes.data, "a&b", 3u) == 0);
  vectis_mutable_bytes_cleanup(&bytes);
  old = result.nodes[0];
  result.nodes[0] = NULL;
  vectis_xml_xpath_result_cleanup(&result);
  assert(doc->xpath(doc, "count(//q:x)", namespaces, 1u, &result, &error) ==
             VECTIS_OK &&
         result.kind == VECTIS_XML_XPATH_NUMBER && result.number == 2.0);
  vectis_xml_xpath_result_cleanup(&result);
  assert(doc->xpath(doc, "boolean(//q:x)", namespaces, 1u, &result, &error) ==
             VECTIS_OK &&
         result.boolean);
  vectis_xml_xpath_result_cleanup(&result);
  assert(doc->xpath(doc, "string(//q:x[2]/@a)", namespaces, 1u, &result,
                    &error) == VECTIS_OK &&
         result.string.size == 1u);
  vectis_xml_xpath_result_cleanup(&result);
  assert(doc->xpath(doc, "//q:x/@a", namespaces, 1u, &result, &error) ==
         VECTIS_ERR_INVALID);
  assert(doc->xpath(doc, "[", NULL, 0u, &result, &error) != VECTIS_OK);
  assert(root->set_attribute(root, "x", "<&", 2u, "urn:q", "q", &error) ==
         VECTIS_OK);
  assert(old->name(old, &name, &error) == VECTIS_ERR_STATE);
  assert(old->close(old, &error) == VECTIS_OK);
  assert(root->name(root, &name, &error) == VECTIS_ERR_STATE);
  assert(root->close(root, &error) == VECTIS_OK);
  assert(doc->root(doc, &root, &error) == VECTIS_OK);
  assert(root->attribute(root, "x", "urn:q", &bytes, &found, &error) ==
             VECTIS_OK &&
         found && bytes.size == 2u && memcmp(bytes.data, "<&", 2u) == 0);
  vectis_mutable_bytes_cleanup(&bytes);
  assert(root->set_attribute(root, "x", "v", 1u, "urn:other", "q", &error) ==
         VECTIS_ERR_CONFLICT);
  assert(root->set_attribute(root, "x", "v", 1u, "urn:other", "xml", &error) ==
         VECTIS_ERR_INVALID);
  assert(root->set_text(root, "\0", 1u, &error) == VECTIS_ERR_INVALID);
  memset(&child_config, 0, sizeof(child_config));
  child_config.text = "<&";
  child_config.text_size = 2u;
  child_config.text_present = 1;
  assert(root->append_child(root, "item", &child_config, &child, &error) ==
         VECTIS_OK);
  assert(child->name(child, &name, &error) == VECTIS_OK &&
         strcmp(name.namespace_uri, "urn:r") == 0);
  assert(root->close(root, &error) == VECTIS_OK);
  assert(child->set_text(child, "literal &value;",
                         sizeof("literal &value;") - 1u, &error) == VECTIS_OK);
  assert(child->close(child, &error) == VECTIS_OK);
  assert(doc->serialize_materialized(doc, 1, &bytes, &error) == VECTIS_OK);
  assert(bytes.size > 0u);
  vectis_mutable_bytes_cleanup(&bytes);
  assert(doc->root(doc, &root, &error) == VECTIS_OK);
  assert(root->remove(root, &error) == VECTIS_ERR_INVALID);
  assert(doc->close(doc, &error) == VECTIS_OK);
  assert(root->name(root, &name, &error) == VECTIS_ERR_STATE);
  assert(root->close(root, &error) == VECTIS_OK);
  assert(vectis_xml_document_parse("<r>", 3u, NULL, &doc, &error) !=
             VECTIS_OK &&
         doc == NULL);
  assert(vectis_xml_document_parse(
             "<!DOCTYPE r [<!ENTITY a 'x'>]><r>&a;</r>",
             sizeof("<!DOCTYPE r [<!ENTITY a 'x'>]><r>&a;</r>") - 1u, NULL,
             &doc, &error) != VECTIS_OK &&
         doc == NULL);
  config.max_bytes = 2u;
  assert(vectis_xml_document_parse("<r/>", 4u, &config, &doc, &error) ==
         VECTIS_ERR_INVALID);
  vectis_xml_document_config_init(&config);
  config.max_xpath_nodes = 1u;
  assert(vectis_xml_document_parse(document, sizeof(document) - 1u, &config,
                                   &doc, &error) == VECTIS_OK);
  assert(doc->xpath(doc, "//*", NULL, 0u, &result, &error) ==
         VECTIS_ERR_INVALID);
  assert(doc->close(doc, &error) == VECTIS_OK);
}
typedef struct events {
  vectis_xml_sax *parser;
  size_t starts, ends, bytes;
  int fail, reenter;
} events;
static vectis_status start(void *context, const vectis_xml_name *name,
                           const vectis_xml_attribute *attrs, size_t count,
                           const vectis_xml_namespace *ns, size_t ns_count,
                           vectis_error *error) {
  events *state = (events *)context;
  (void)error;
  ++state->starts;
  assert(name->name != NULL);
  if (state->starts == 1u && ns_count != 0u) {
    assert(strcmp(ns[0].uri, "urn:r") == 0);
    assert(count == 1u && attrs[0].size == 1u &&
           memcmp(attrs[0].value, "&", 1u) == 0);
  }
  if (state->reenter) {
    assert(state->parser->close(state->parser, error) == VECTIS_ERR_STATE);
    return VECTIS_OK;
  }
  return state->fail ? VECTIS_ERR_STATE : VECTIS_OK;
}
static vectis_status end(void *context, const vectis_xml_name *name,
                         vectis_error *error) {
  events *state = (events *)context;
  (void)name;
  (void)error;
  ++state->ends;
  return VECTIS_OK;
}
static vectis_status text(void *context, const void *data, size_t size,
                          vectis_error *error) {
  events *state = (events *)context;
  (void)error;
  assert(data != NULL && size <= VECTIS_XML_SAX_CHUNK_BYTES);
  state->bytes += size;
  return VECTIS_OK;
}
static void native_sax(void) {
  vectis_xml_sax_callbacks callbacks;
  events state;
  vectis_error error;
  char chunk[4096];
  size_t i;
  memset(&state, 0, sizeof(state));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.start_element = start;
  callbacks.end_element = end;
  callbacks.text = text;
  assert(vectis_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
         VECTIS_OK);
  assert(state.parser->feed(state.parser, "<r xmlns='urn:r' a='&amp;'>",
                            sizeof("<r xmlns='urn:r' a='&amp;'>") - 1u,
                            &error) == VECTIS_OK);
  assert(state.starts == 1u && state.ends == 0u);
  memset(chunk, 'x', sizeof(chunk));
  for (i = 0u; i < 4097u; ++i)
    assert(state.parser->feed(state.parser, chunk, sizeof(chunk), &error) ==
           VECTIS_OK);
  assert(state.bytes > VECTIS_XML_DEFAULT_MAX_BYTES);
  assert(state.parser->feed(state.parser, "</r>", 4u, &error) == VECTIS_OK);
  assert(state.parser->finish(state.parser, &error) == VECTIS_OK &&
         state.ends == 1u);
  assert(state.parser->finish(state.parser, &error) == VECTIS_OK);
  assert(state.parser->feed(state.parser, "", 0u, &error) == VECTIS_ERR_STATE);
  assert(state.parser->close(state.parser, &error) == VECTIS_OK);
  memset(&state, 0, sizeof(state));
  state.fail = 1;
  assert(vectis_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
         VECTIS_OK);
  assert(state.parser->feed(state.parser, "<r/>", 4u, &error) ==
         VECTIS_ERR_STATE);
  assert(state.parser->finish(state.parser, &error) == VECTIS_ERR_STATE &&
         state.starts == 1u);
  assert(state.parser->close(state.parser, &error) == VECTIS_OK);
  memset(&state, 0, sizeof(state));
  state.reenter = 1;
  assert(vectis_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
         VECTIS_OK);
  assert(state.parser->feed(state.parser, "<r/>", 4u, &error) ==
         VECTIS_ERR_STATE);
  assert(state.parser->close(state.parser, &error) == VECTIS_OK);
  memset(&state, 0, sizeof(state));
  assert(vectis_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
         VECTIS_OK);
  assert(state.parser->feed(
             state.parser, "<!DOCTYPE r SYSTEM 'file:///unused'><r/>",
             sizeof("<!DOCTYPE r SYSTEM 'file:///unused'><r/>") - 1u,
             &error) != VECTIS_OK);
  assert(state.starts == 0u &&
         state.parser->close(state.parser, &error) == VECTIS_OK);
  assert(vectis_xml_sax_new(&callbacks, &state, 1u, &state.parser, &error) ==
         VECTIS_OK);
  assert(state.parser->feed(state.parser, "<root/>", 7u, &error) ==
         VECTIS_ERR_INVALID);
  assert(state.parser->close(state.parser, &error) == VECTIS_OK);
  assert(vectis_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
         VECTIS_OK);
  assert(state.parser->feed(state.parser, "<r>", 3u, &error) == VECTIS_OK);
  assert(state.parser->finish(state.parser, &error) != VECTIS_OK);
  assert(state.parser->close(state.parser, &error) == VECTIS_OK);
}
static void *foreign_thread(void *context) {
  vectis_xml_document *doc = (vectis_xml_document *)context;
  vectis_xml_node *node = NULL;
  vectis_error error;
  assert(doc->root(doc, &node, &error) == VECTIS_ERR_STATE && node == NULL);
  assert(doc->close(doc, &error) == VECTIS_ERR_STATE);
  return NULL;
}
static void ownership(void) {
  vectis_xml_document *doc = NULL;
  vectis_xml_node *node = NULL;
  vectis_error error;
  pthread_t thread;
  pid_t child;
  int status;
  assert(vectis_xml_document_parse("<r/>", 4u, NULL, &doc, &error) ==
         VECTIS_OK);
  assert(pthread_create(&thread, NULL, foreign_thread, doc) == 0);
  assert(pthread_join(thread, NULL) == 0);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    foreign_thread(doc);
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0);
  assert(doc->root(doc, &node, &error) == VECTIS_OK);
  assert(node->close(node, &error) == VECTIS_OK);
  assert(doc->close(doc, &error) == VECTIS_OK);
}
int main(void) {
  native_dom();
  native_sax();
  ownership();
  return 0;
}
