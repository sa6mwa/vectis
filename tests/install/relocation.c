#include <vectis/vectis.h>

int main(void) {
  vectis_app_config config;
  vectis_app_config_init(&config);
  return config.tls.port != VECTIS_TLS_DEFAULT_PORT;
}
