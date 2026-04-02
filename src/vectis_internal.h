#ifndef VECTIS_INTERNAL_H
#define VECTIS_INTERNAL_H

#include <vectis/vectis.h>

void vectis_set_error(vectis_error *error, vectis_status code, const char *message);
struct lc_client *vectis_internal_lockd_client(vectis_app *app);

#endif
