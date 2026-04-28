#include <vectis/vectis.h>

int main(void) {
  vectis_sftp_config sftp;
  vectis_error error;

  vectis_sftp_config_init(&sftp);
  sftp.url = "sftp://files.example.com:22";
  sftp.username = "deploy";
  sftp.private_key_path = "/etc/vectis/sftp_ed25519";
  sftp.known_hosts_path = "/etc/vectis/known_hosts";

  (void)vectis_sftp_upload_file(&sftp,
                                "build/orders.json",
                                "/incoming/orders.json",
                                &error);
  (void)vectis_sftp_download_file(&sftp,
                                  "/exports/result.json",
                                  "var/result.json",
                                  &error);
  return 0;
}
