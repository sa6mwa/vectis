#ifndef VECTIS_SQL_LIFECYCLE_H
#define VECTIS_SQL_LIFECYCLE_H
#include <vectis/sql.h>

/* Library-internal registry shared by the SQL components. Declarations own
 * their entries; the app detaches every entry before destroying itself. */
typedef struct vectis_sql_app_resource {
  vectis_app *app;
  const char *name;
  struct vectis_sql_app_resource *next;
  void *context;
  vectis_status (*check_idle)(void *, vectis_error *);
  void (*invalidate)(void *);
} vectis_sql_app_resource;
vectis_status vectis_sql_app_attach(vectis_app *, vectis_sql_app_resource *,
                                    vectis_error *);
void vectis_sql_app_detach(vectis_sql_app_resource *);
int vectis_sql_app_running(vectis_app *);
#endif
