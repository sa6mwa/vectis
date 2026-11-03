#include <lc/lc.h>
#include <libmdf.h>
#include <lql/lql.h>
#include <stdio.h>
#include <string.h>
#include <vectis/compression.h>
#include <vectis/crypto.h>
#include <vectis/curl.h>
#include <vectis/pdf.h>
#include <vectis/pg.h>
#include <vectis/png.h>
#include <vectis/sqlite.h>
#include <vectis/xml.h>
#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!(expression)) {                                                       \
      fprintf(stderr, "installed useful facade failure at line %d\n",          \
              __LINE__);                                                       \
      return __LINE__;                                                         \
    }                                                                          \
  } while (0)
int vectis_install_useful_facades(void);
typedef struct consumer_bytes {
  unsigned char data[2048];
  size_t size;
} consumer_bytes;
static vectis_status collect(void *context, const void *data, size_t size,
                             vectis_error *error) {
  consumer_bytes *bytes = (consumer_bytes *)context;
  (void)error;
  if (size > sizeof(bytes->data) - bytes->size)
    return VECTIS_ERR_INVALID;
  memcpy(bytes->data + bytes->size, data, size);
  bytes->size += size;
  return VECTIS_OK;
}
static vectis_status configure(CURL *easy, void *context, vectis_error *error) {
  (void)context;
  (void)error;
  return curl_easy_setopt(easy, CURLOPT_URL, "vectis-unsupported://probe/") ==
                 CURLE_OK
             ? VECTIS_OK
             : VECTIS_ERR_STATE;
}
int vectis_install_useful_facades(void) {
  mdf_theme_palette palette;
  vectis_error error;
  lql *lql_client = NULL;
  lql_error lql_err;
  lql_selector *lql_selection = NULL;
  vectis_sqlite *sqlite = NULL;
  vectis_sqlite_statement *statement = NULL;
  vectis_sql_value value;
  vectis_pg_connection *pg = NULL;
  vectis_pg_config pg_config;
  const char *keywords[] = {"vectis_invalid_connection_option"};
  const char *values[] = {"x"};
  vectis_pdf *pdf = NULL;
  vectis_pdf_page *page = NULL;
  vectis_pdf_image *pdf_image = NULL;
  vectis_png_info info;
  vectis_png_materialized decoded;
  vectis_mutable_bytes png, bytes, plaintext;
  vectis_crypto_gcm_result encrypted;
  unsigned char pixels[] = {0x12, 0x34, 0x56, 0xff}, key[16], nonce[12];
  const char *algorithms[] = {"zstd", "lz4", "xz"};
  vectis_compression *encoder = NULL, *decoder = NULL;
  consumer_bytes compressed, decompressed;
  vectis_xml_document *document = NULL;
  vectis_xml_xpath_result xpath;
  vectis_curl_session *session = NULL;
  vectis_curl_multi *multi = NULL;
  vectis_curl_request request;
  const vectis_curl_result *result;
  size_t i, running;
  unsigned long id;
  int row;
  lc_collection_read_opts collection_read;
  lc_collection_edit_req collection_edit;
  lc_collection_lease_req collection_lease;
  CHECK(strcmp(lc_version_string(), "0.27.0") == 0);
  CHECK(LC_COLLECTION_EDIT_MAX_RANGES == 64U);
  lc_collection_read_opts_init(&collection_read);
  CHECK(collection_read.expected_head == NULL);
  lc_collection_edit_req_init(&collection_edit);
  CHECK(collection_edit.range_count == 0 &&
        collection_edit.replacement == NULL);
  lc_collection_lease_req_init(&collection_lease);
  CHECK(collection_lease.ttl_seconds == 30 && !collection_lease.private_read);
  CHECK(strcmp(LIBMDF_VERSION, "0.19.0") == 0);
  CHECK(mdf_theme_get_palette("default", &palette) == MDF_OK);
  CHECK(strcmp(palette.name, "default") == 0 &&
        palette.heading[0][0] == '\033');
  CHECK(lql_new(&lql_client, &lql_err) == LQL_STATUS_OK);
  CHECK(strcmp(lql_client->version(lql_client), "0.4.0") == 0);
  CHECK(lql_client->selector_parse(lql_client, "/x=1", &lql_selection,
                                   &lql_err) == LQL_STATUS_OK);
  lql_client->selector_destroy(lql_client, lql_selection);
  lql_client->destroy(lql_client);
  memset(&error, 0, sizeof(error));
  CHECK(vectis_sqlite_open(":memory:", NULL, &sqlite, &error) == VECTIS_OK);
  CHECK(sqlite->prepare(sqlite, "SELECT 7", strlen("SELECT 7"), 1, &statement,
                        &error) == VECTIS_OK);
  CHECK(statement->bind(statement, NULL, 0u, &error) == VECTIS_OK);
  CHECK(statement->next(statement, &row, &error) == VECTIS_OK && row);
  CHECK(vectis_sqlite_value(statement, 0u, &value, &error) == VECTIS_OK &&
        value.type == VECTIS_SQL_INTEGER && value.integer == 7);
  CHECK(statement->next(statement, &row, &error) == VECTIS_OK && !row);
  statement->close(statement);
  CHECK(sqlite->close(sqlite, &error) == VECTIS_OK);
  vectis_pg_config_init(&pg_config);
  pg_config.keywords = keywords;
  pg_config.values = values;
  pg_config.parameter_count = 1u;
  CHECK(vectis_pg_connect_start(&pg_config, &pg, &error) != VECTIS_OK &&
        pg == NULL);
  memset(&info, 0, sizeof(info));
  info.width = info.height = 1u;
  info.format = VECTIS_PNG_RGBA8;
  CHECK(vectis_png_encode_materialized(&info, pixels, sizeof(pixels), NULL,
                                       &png, &error) == VECTIS_OK);
  CHECK(vectis_png_decode_materialized(png.data, png.size, NULL, &decoded,
                                       &error) == VECTIS_OK &&
        decoded.pixels.size == 4u &&
        memcmp(decoded.pixels.data, pixels, 4u) == 0);
  vectis_png_materialized_cleanup(&decoded);
  CHECK(vectis_pdf_new(NULL, &pdf, &error) == VECTIS_OK);
  CHECK(pdf->add_page(pdf, NULL, &page, &error) == VECTIS_OK);
  CHECK(page->text(page, 10.0, 20.0, "installed facade", NULL, &error) ==
        VECTIS_OK);
  CHECK(pdf->load_image_materialized(pdf, png.data, png.size, "png", &pdf_image,
                                     &error) == VECTIS_OK);
  vectis_mutable_bytes_cleanup(&png);
  CHECK(page->image(page, pdf_image, 10.0, 30.0, 10.0, 10.0, &error) ==
        VECTIS_OK);
  CHECK(pdf->bytes_materialized(pdf, 65536u, &bytes, &error) == VECTIS_OK &&
        bytes.size > 5u && memcmp(bytes.data, "%PDF-", 5u) == 0);
  vectis_mutable_bytes_cleanup(&bytes);
  pdf_image->close(pdf_image);
  page->close(page);
  CHECK(pdf->close(pdf, &error) == VECTIS_OK);
  for (i = 0u; i < 3u; ++i) {
    memset(&compressed, 0, sizeof(compressed));
    memset(&decompressed, 0, sizeof(decompressed));
    CHECK(vectis_compression_new(algorithms[i], "compress", NULL, &encoder,
                                 &error) == VECTIS_OK);
    CHECK(encoder->process(encoder, "consumer", 8u, collect, &compressed,
                           &error) == VECTIS_OK);
    CHECK(encoder->finish(encoder, collect, &compressed, &error) == VECTIS_OK);
    CHECK(encoder->close(encoder, &error) == VECTIS_OK);
    CHECK(vectis_compression_new(algorithms[i], "decompress", NULL, &decoder,
                                 &error) == VECTIS_OK);
    CHECK(decoder->process(decoder, compressed.data, compressed.size, collect,
                           &decompressed, &error) == VECTIS_OK);
    CHECK(decoder->finish(decoder, collect, &decompressed, &error) ==
              VECTIS_OK &&
          decompressed.size == 8u &&
          memcmp(decompressed.data, "consumer", 8u) == 0);
    CHECK(decoder->close(decoder, &error) == VECTIS_OK);
  }
  memset(key, 0, sizeof(key));
  memset(nonce, 0, sizeof(nonce));
  CHECK(vectis_crypto_gcm_encrypt_materialized(
            key, sizeof(key), nonce, sizeof(nonce), "a\0b", 3u, NULL, 0u,
            &encrypted, &error) == VECTIS_OK);
  CHECK(vectis_crypto_gcm_decrypt_materialized(
            key, sizeof(key), nonce, sizeof(nonce), encrypted.ciphertext.data,
            encrypted.ciphertext.size, NULL, 0u, encrypted.tag,
            sizeof(encrypted.tag), &plaintext, &error) == VECTIS_OK &&
        plaintext.size == 3u && memcmp(plaintext.data, "a\0b", 3u) == 0);
  vectis_crypto_gcm_result_cleanup(&encrypted);
  vectis_mutable_bytes_cleanup(&plaintext);
  CHECK(vectis_xml_document_parse("<r><x/></r>", strlen("<r><x/></r>"), NULL,
                                  &document, &error) == VECTIS_OK);
  CHECK(document->xpath(document, "count(/r/x)", NULL, 0u, &xpath, &error) ==
            VECTIS_OK &&
        xpath.number == 1.0);
  vectis_xml_xpath_result_cleanup(&xpath);
  CHECK(document->serialize_materialized(document, 0, &bytes, &error) ==
            VECTIS_OK &&
        bytes.size > 0u);
  vectis_mutable_bytes_cleanup(&bytes);
  CHECK(document->close(document, &error) == VECTIS_OK);
  memset(&request, 0, sizeof(request));
  request.configure = configure;
  CHECK(vectis_curl_session_new(&session, &error) == VECTIS_OK);
  CHECK(session->perform(session, &request, &result, &error) == VECTIS_OK &&
        result->code == CURLE_UNSUPPORTED_PROTOCOL);
  CHECK(session->close(session, &error) == VECTIS_OK);
  CHECK(vectis_curl_multi_new(&multi, &error) == VECTIS_OK);
  CHECK(multi->add(multi, &request, &id, &error) == VECTIS_OK);
  CHECK(multi->step(multi, &running, &error) == VECTIS_OK && running == 0u);
  CHECK(multi->completion(multi, 0u, &result, &error) == VECTIS_OK &&
        result != NULL && result->id == id &&
        result->code == CURLE_UNSUPPORTED_PROTOCOL);
  CHECK(multi->consume(multi, 1u, &error) == VECTIS_OK &&
        multi->close(multi, &error) == VECTIS_OK);
  return 0;
}
