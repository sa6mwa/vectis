#include <vectis/vectis.h>

int main(void) {
  vectis_ssh_config ssh;
  vectis_error error;

  vectis_ssh_config_init(&ssh);
  ssh.host = "files.example.com";
  ssh.username = "deploy";
  ssh.private_key_path = "/etc/vectis/ssh_ed25519";
  ssh.known_hosts_path = "/etc/vectis/known_hosts";
  ssh.timeout_ms = 30000L;

  (void)vectis_ssh_sftp_upload_file(&ssh,
                                    "build/orders.json",
                                    "/incoming/orders.json",
                                    &error);
  (void)vectis_ssh_sftp_download_file(&ssh,
                                      "/exports/result.json",
                                      "var/result.json",
                                      &error);
  return 0;
}
