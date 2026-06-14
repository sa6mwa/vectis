#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vectis/vectis.h>

static const char *env_or_default(const char *name, const char *fallback) {
  const char *value;

  value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return fallback;
  }
  return value;
}

static unsigned short env_port_or_default(const char *name,
                                          unsigned short fallback) {
  const char *value;
  long port;

  value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return fallback;
  }
  port = strtol(value, NULL, 10);
  if (port <= 0L || port > 65535L) {
    return fallback;
  }
  return (unsigned short)port;
}

static int print_error(const char *operation, const vectis_error *error) {
  fprintf(stderr, "%s failed", operation);
  if (error != NULL && error->message[0] != '\0') {
    fprintf(stderr, ": %s", error->message);
  }
  if (error != NULL && error->detail[0] != '\0') {
    fprintf(stderr, " (%s)", error->detail);
  }
  fprintf(stderr, "\n");
  return 1;
}

static int write_file(const char *path, const char *body) {
  FILE *fp;
  size_t size;

  size = strlen(body);
  fp = fopen(path, "wb");
  if (fp == NULL) {
    return 1;
  }
  if (fwrite(body, 1u, size, fp) != size) {
    (void)fclose(fp);
    return 1;
  }
  return fclose(fp) == 0 ? 0 : 1;
}

static int prepare_assets(const char *root_dir, const char *index_path,
                          const char *app_path) {
  (void)mkdir(root_dir, 0700);
  if (write_file(index_path, "<!doctype html><title>vectis</title>\n") != 0) {
    return 1;
  }
  if (write_file(app_path, "console.log('vectis');\n") != 0) {
    return 1;
  }
  return 0;
}

static void serve_forever(void) {
  for (;;) {
    (void)sleep(3600u);
  }
}

int main(void) {
  vectis_app_config app_config;
  vectis_static_file_config file;
  vectis_static_directory_config directory;
  vectis_error error;
  vectis_app *app;
  const char *root_dir;
  const char *index_path;
  const char *app_path;

  root_dir = env_or_default("VECTIS_STATIC_ROOT", "/tmp/vectis-static-assets");
  index_path = env_or_default("VECTIS_STATIC_INDEX",
                              "/tmp/vectis-static-assets/index.html");
  app_path =
      env_or_default("VECTIS_STATIC_APP", "/tmp/vectis-static-assets/app.js");
  if (prepare_assets(root_dir, index_path, app_path) != 0) {
    fprintf(stderr, "failed to prepare static asset fixtures\n");
    return 1;
  }

  vectis_app_config_init(&app_config);
  app_config.app_name = "static-assets-api";
  app_config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  app_config.tls.bind = env_or_default("VECTIS_KORE_BIND", "127.0.0.1");
  app_config.tls.port = env_port_or_default("VECTIS_KORE_PORT", 28084u);

  app = vectis_app_new(&app_config, &error);
  if (app == NULL) {
    return print_error("vectis_app_new", &error);
  }

  vectis_static_file_config_init(&file);
  file.path = "/";
  file.file_path = index_path;
  file.content_type = "text/html";
  if (app->static_file(app, &file, &error) != VECTIS_OK) {
    (void)print_error("app->static_file", &error);
    app->close(app);
    return 1;
  }

  vectis_static_directory_config_init(&directory);
  directory.path_prefix = "/assets";
  directory.root_dir = root_dir;
  directory.content_type = "application/javascript";
  if (app->static_directory(app, &directory, &error) != VECTIS_OK) {
    (void)print_error("app->static_directory", &error);
    app->close(app);
    return 1;
  }

  if (app->start(app, &error) != VECTIS_OK) {
    (void)print_error("app->start", &error);
    app->close(app);
    return 1;
  }
  serve_forever();
  app->close(app);
  return 0;
}
