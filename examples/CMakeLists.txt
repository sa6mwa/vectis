function(vectis_add_example target_name source_file)
  add_executable(${target_name} ${source_file})
  if(VECTIS_BUILD_STATIC)
    target_link_libraries(${target_name} PRIVATE vectis_static)
  elseif(VECTIS_BUILD_SHARED)
    target_link_libraries(${target_name} PRIVATE vectis_shared)
  endif()
  target_include_directories(${target_name} PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${VECTIS_EXTERNAL_ROOT}/include"
  )
  set_target_properties(${target_name} PROPERTIES
    C_STANDARD 90
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
  )
endfunction()

vectis_add_example(vectis_example_kore_basic kore/kore_basic_server.c)
vectis_add_example(vectis_example_kore_json kore/kore_json_routes.c)
vectis_add_example(vectis_example_kore_regex kore/kore_regex_routes.c)
vectis_add_example(vectis_example_kore_tls_acme kore/kore_tls_acme.c)
vectis_add_example(vectis_example_kore_tls_memory kore/kore_tls_memory_bundles.c)
vectis_add_example(vectis_example_kore_lockd_api kore/kore_lockd_api.c)
vectis_add_example(vectis_example_rest_lockd kore/rest_api_lockd_lonejson.c)

vectis_add_example(vectis_example_lockd_open lockd/lockd_open_client.c)
vectis_add_example(vectis_example_lockd_lease lockd/lockd_acquire_save_load_release.c)
vectis_add_example(vectis_example_lockd_query lockd/lockd_query.c)
vectis_add_example(vectis_example_lockd_attachments lockd/lockd_attachments.c)
vectis_add_example(vectis_example_lockd_enqueue lockd/lockd_enqueue.c)
vectis_add_example(vectis_example_lockd_dequeue lockd/lockd_dequeue_ack_nack.c)
vectis_add_example(vectis_example_lockd_consumer lockd/lockd_consumer_service.c)
vectis_add_example(vectis_example_lockd_consumer_state lockd/lockd_consumer_service_with_state.c)

vectis_add_example(vectis_example_curl_json curl/curl_json_api.c)
vectis_add_example(vectis_example_curl_transfer curl/curl_transfer.c)
vectis_add_example(vectis_example_sftp curl/curl_sftp.c)
vectis_add_example(vectis_example_mqtt curl/mqtt_publish.c)

vectis_add_example(vectis_example_ssh ssh/ssh_command.c)
vectis_add_example(vectis_example_ssh_sftp ssh/ssh2_sftp.c)

vectis_add_example(vectis_example_certs certs/cert_bundle.c)
vectis_add_example(vectis_example_raw_escape raw/raw_escape_hatches.c)
