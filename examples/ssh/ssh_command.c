#include <vectis/vectis.h>

int main(void) {
  vectis_ssh_config ssh;
  vectis_ssh_exec_result result = {0};
  vectis_error error;

  vectis_ssh_config_init(&ssh);
  vectis_ssh_exec_result_cleanup(&result);

  ssh.host = "worker-01.internal";
  ssh.username = "vectis";
  ssh.private_key_path = "/etc/vectis/ssh_ed25519";
  ssh.known_hosts_path = "/etc/vectis/known_hosts";
  ssh.timeout_ms = 10000L;

  (void)vectis_ssh_exec(&ssh, "systemctl reload downstream-worker", &result, &error);
  vectis_ssh_exec_result_cleanup(&result);
  return 0;
}
