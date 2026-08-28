# ---- esp-dl 人脸模型嵌入: 配置期生成 ----
# human_face_detect 组件用 build 期 add_custom_command 生成
# human_face_detect.espdl(.S), 但 PlatformIO 的 SCons 层不执行 CMake 自定义
# 命令 (CI/全新 checkout 报 "Source ... not found")。组件管理器在配置早期
# 已下载 managed_components, 故此处配置期 execute_process 同步补生成,
# 单次构建即可通过 (本地与 CI 一致)。原生 idf.py 构建不受影响 (幂等)。
idf_build_get_property(pvc_build_dir BUILD_DIR)
set(pvc_espdl_dir ${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-dl)
set(pvc_face_dir  ${CMAKE_SOURCE_DIR}/managed_components/espressif__human_face_detect)
set(pvc_packed    ${pvc_build_dir}/espdl_models/human_face_detect.espdl)
set(pvc_sfile     ${pvc_build_dir}/human_face_detect.espdl.S)
if(EXISTS ${pvc_face_dir}/models/s3 AND NOT EXISTS ${pvc_sfile})
    file(MAKE_DIRECTORY ${pvc_build_dir}/espdl_models)
    idf_build_get_property(pvc_python PYTHON)
    execute_process(
        COMMAND ${pvc_python} ${pvc_espdl_dir}/fbs_loader/pack_espdl_models.py
            --model_path
            ${pvc_face_dir}/models/s3/human_face_detect_msr_s8_v1.espdl
            ${pvc_face_dir}/models/s3/human_face_detect_mnp_s8_v1.espdl
            --out_file ${pvc_packed}
        RESULT_VARIABLE pvc_pack_rc)
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -D DATA_FILE=${pvc_packed} -D SOURCE_FILE=${pvc_sfile}
            -D FILE_TYPE=BINARY
            -P ${pvc_espdl_dir}/fbs_loader/cmake/data_file_embed_asm_aligned.cmake
        WORKING_DIRECTORY ${pvc_build_dir}
        RESULT_VARIABLE pvc_embed_rc)
    if(NOT pvc_pack_rc EQUAL 0 OR NOT pvc_embed_rc EQUAL 0)
        message(WARNING "espdl model pre-generation failed "
                        "(pack=${pvc_pack_rc} embed=${pvc_embed_rc})")
    endif()
endif()

idf_component_register(SRCS "main.c" "app_camera.c" "hw2d.c" "hw2d_pie.S"
                            "ui_beauty_camera.c" "pvc_power.c" "pvc_clock.c"
                            "pvc_jpeg.c" "pvc_jpeg_dims.c" "pvc_sound.c" "pvc_algo_test.c" "pvc_face.cpp"
                            "net/pvc_net.c" "net/pvc_store.c" "net/pvc_http.c"
                            "net/pvc_pair.c" "net/pvc_upload.c" "net/pvc_prov.c"
                            "net/pvc_feed.c" "net/pvc_config.c"
                       INCLUDE_DIRS "." "net"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash
                                     esp_http_client esp-tls mbedtls json esp_timer
                                     wifi_provisioning bt driver esp_driver_i2c)
