cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-cam-audio)

# Gzip web content into staging dir, then build SPIFFS image
set(SPIFFS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/spiffs_data")
set(SPIFFS_STAGE "${CMAKE_BINARY_DIR}/spiffs_staging")
file(GLOB WEB_FILES "${SPIFFS_SRC}/*.html" "${SPIFFS_SRC}/*.css")

set(GZ_FILES)
foreach(SRC_FILE ${WEB_FILES})
    get_filename_component(FNAME ${SRC_FILE} NAME)
    set(GZ_FILE "${SPIFFS_STAGE}/${FNAME}.gz")
    add_custom_command(
        OUTPUT ${GZ_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SPIFFS_STAGE}
        COMMAND ${CMAKE_COMMAND} -E copy ${SRC_FILE} ${SPIFFS_STAGE}/${FNAME}
        COMMAND gzip -9 -f ${SPIFFS_STAGE}/${FNAME}
        DEPENDS ${SRC_FILE}
        COMMENT "Gzipping ${FNAME}"
    )
    list(APPEND GZ_FILES ${GZ_FILE})
endforeach()

add_custom_target(gzip_web DEPENDS ${GZ_FILES})
spiffs_create_partition_image(spiffs ${SPIFFS_STAGE} FLASH_IN_PROJECT DEPENDS gzip_web)
