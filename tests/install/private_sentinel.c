/* This declaration is deliberately private and must never link from libvectis.
 */
extern int vectis_internal_worker_count(void);

int main(void) { return vectis_internal_worker_count(); }
