cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(chute)

# --- Frontend build (npm — always runs) ---
set(FRONTEND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/frontend")
set(SPIFFS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/spiffs_data")
set(SPIFFS_STAGE "${CMAKE_BINARY_DIR}/spiffs_staging")

add_custom_target(frontend_build
    COMMAND npm install --prefer-offline --no-audit --no-fund
    COMMAND npm run build
    WORKING_DIRECTORY ${FRONTEND_DIR}
    COMMENT "Building frontend (npm run build)"
)

# --- Gzip web content into staging dir, then build SPIFFS image ---
add_custom_target(gzip_web
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SPIFFS_STAGE}
    COMMAND ${CMAKE_COMMAND} -E copy ${SPIFFS_SRC}/index.html ${SPIFFS_SRC}/app.css ${SPIFFS_SRC}/app.js ${SPIFFS_SRC}/favicon.ico ${SPIFFS_STAGE}/
    COMMAND gzip -9 -f ${SPIFFS_STAGE}/index.html ${SPIFFS_STAGE}/app.css ${SPIFFS_STAGE}/app.js ${SPIFFS_STAGE}/favicon.ico
    DEPENDS frontend_build
    COMMENT "Gzipping web assets"
)
spiffs_create_partition_image(spiffs ${SPIFFS_STAGE} FLASH_IN_PROJECT DEPENDS gzip_web)
