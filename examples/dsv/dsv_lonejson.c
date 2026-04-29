#include <stdio.h>
#include <string.h>

#include <vectis/vectis.h>

typedef struct order_row {
  char id[64];
  char customer[64];
  lonejson_int64 quantity;
  int priority;
} order_row;

typedef struct order_totals {
  size_t rows;
  lonejson_int64 quantity;
  int priority_rows;
} order_totals;

static const lonejson_field order_row_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(order_row, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(order_row, customer, "customer", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_REQ(order_row, quantity, "quantity"),
    LONEJSON_FIELD_BOOL(order_row, priority, "priority")};

LONEJSON_MAP_DEFINE(order_row_map, order_row, order_row_fields);

static vectis_status on_order_row(void *userdata,
                                  size_t row_number,
                                  void *row,
                                  vectis_error *error) {
  order_totals *totals;
  order_row *order;

  (void)error;
  totals = (order_totals *)userdata;
  order = (order_row *)row;
  totals->rows++;
  totals->quantity += order->quantity;
  if (order->priority) {
    totals->priority_rows++;
  }
  printf("row=%lu id=%s customer=%s quantity=%ld priority=%d\n",
         (unsigned long)row_number,
         order->id,
         order->customer,
         (long)order->quantity,
         order->priority);
  return VECTIS_OK;
}

int main(int argc, char **argv) {
  const char fallback_csv[] =
      "ord-1001,acme,3,true\n"
      "\"ord-1002\",northwind,5,false\n";
  vectis_source source;
  vectis_dsv_config csv;
  vectis_mutable_bytes json;
  vectis_error error;
  order_totals totals;
  vectis_status status;

  memset(&totals, 0, sizeof(totals));
  memset(&json, 0, sizeof(json));
  vectis_error_clear(&error);
  csv = vectis_dsv_csv_rows();
  source = argc > 1 ? vectis_source_from_path(argv[1])
                    : vectis_source_from_memory(fallback_csv, sizeof(fallback_csv) - 1u);

  status = vectis_dsv_parse_lonejson_source(&source,
                                            &order_row_map,
                                            &csv,
                                            on_order_row,
                                            &totals,
                                            &error);
  if (status != VECTIS_OK) {
    fprintf(stderr, "failed to parse DSV: %s\n", error.message);
    return 1;
  }

  source = argc > 1 ? vectis_source_from_path(argv[1])
                    : vectis_source_from_memory(fallback_csv, sizeof(fallback_csv) - 1u);
  status = vectis_dsv_source_to_lonejson_array(&source, &order_row_map, &csv, &json, &error);
  if (status != VECTIS_OK) {
    fprintf(stderr, "failed to convert DSV to JSON: %s\n", error.message);
    return 1;
  }

  printf("rows=%lu quantity=%ld priority_rows=%d\n",
         (unsigned long)totals.rows,
         (long)totals.quantity,
         totals.priority_rows);
  printf("%.*s\n", (int)json.size, (const char *)json.data);
  vectis_mutable_bytes_cleanup(&json);
  return 0;
}
