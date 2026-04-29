#include <stdio.h>
#include <string.h>

#include <lonejson.h>
#include <vectis/vectis.h>

typedef struct invoice_line {
  char sku[32];
  lonejson_int64 quantity;
} invoice_line;

typedef struct money {
  char currency[8];
  double text;
} money;

typedef struct invoice {
  char id[32];
  money amount;
  lonejson_object_array line;
  lonejson_string_array tag;
  int active;
} invoice;

static const lonejson_field invoice_line_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(invoice_line, sku, "sku", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_REQ(invoice_line, quantity, "quantity")};

static const lonejson_field money_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(money, currency, "currency", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_F64_REQ(money, text, "text")};

LONEJSON_MAP_DEFINE(invoice_line_map, invoice_line, invoice_line_fields);
LONEJSON_MAP_DEFINE(money_map, money, money_fields);

static const lonejson_field invoice_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(invoice, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_REQ(invoice, amount, "amount", &money_map),
    LONEJSON_FIELD_OBJECT_ARRAY(invoice,
                                line,
                                "line",
                                invoice_line,
                                &invoice_line_map,
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_ARRAY(invoice, tag, "tag", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_BOOL_REQ(invoice, active, "active")};

LONEJSON_MAP_DEFINE(invoice_map, invoice, invoice_fields);

int main(void) {
  const char xml[] =
      "<invoice>"
      "<id>INV-1001</id>"
      "<amount currency=\"SEK\">123.50</amount>"
      "<line><sku>A-1</sku><quantity>2</quantity></line>"
      "<line><sku>B-2</sku><quantity>3</quantity></line>"
      "<tag>finance</tag><tag>settlement</tag>"
      "<active>true</active>"
      "</invoice>";
  vectis_xml_config config;
  vectis_source source;
  vectis_error error;
  vectis_status status;
  invoice doc;
  invoice_line *lines;

  memset(&doc, 0, sizeof(doc));
  config = vectis_xml_default();
  config.root_element = "invoice";
  source = vectis_source_from_memory(xml, sizeof(xml) - 1u);

  status = vectis_xml_parse_lonejson_source(&source, &invoice_map, &config, &doc, &error);
  if (status != VECTIS_OK) {
    fprintf(stderr, "xml parse failed: %s\n", error.message);
    vectis_error_clear(&error);
    return 1;
  }

  lines = (invoice_line *)doc.line.items;
  printf("%s %.2f %s %lu\n",
         doc.id,
         doc.amount.text,
         doc.amount.currency,
         (unsigned long)doc.line.count);
  if (doc.line.count > 0u) {
    printf("%s x%ld\n", lines[0].sku, (long)lines[0].quantity);
  }

  lonejson_cleanup(&invoice_map, &doc);
  return 0;
}
