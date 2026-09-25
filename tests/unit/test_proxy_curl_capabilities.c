#include <curl/curl.h>

#include <stdio.h>

int main(void) {
  const curl_version_info_data *info;

  info = curl_version_info(CURLVERSION_NOW);
  if (info == NULL) {
    fprintf(stderr, "libcurl returned no version information\n");
    return 1;
  }
  if ((info->features & CURL_VERSION_ASYNCHDNS) == 0) {
    fprintf(stderr,
            "libcurl %s lacks CURL_VERSION_ASYNCHDNS (features=0x%lx)\n",
            info->version, (unsigned long)info->features);
    return 1;
  }
  return 0;
}
