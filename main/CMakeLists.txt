idf_component_register(
    SRCS "main.c" "http_ui.c" "http_camera.c" "http_firmware.c"
         "http_video_stream.c" "http_audio_stream.c"
         "config.c"
    INCLUDE_DIRS "."
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CAMERA_MODEL_AI_THINKER)
