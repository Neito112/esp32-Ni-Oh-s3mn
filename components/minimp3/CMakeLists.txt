# Component header-only minimp3 để giải mã MP3 streaming trực tiếp trên RAM
set(MINIMP3_HEADER "${CMAKE_CURRENT_LIST_DIR}/include/minimp3.h")

if(NOT EXISTS "${MINIMP3_HEADER}")
    message(STATUS "[minimp3] Không tìm thấy minimp3.h cục bộ, đang tự động tải từ lieff/minimp3...")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/include")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h"
        "${MINIMP3_HEADER}"
        TLS_VERIFY ON
        STATUS DOWNLOAD_STATUS
        TIMEOUT 20
    )
    list(GET DOWNLOAD_STATUS 0 DOWNLOAD_CODE)
    if(NOT DOWNLOAD_CODE EQUAL 0)
        list(GET DOWNLOAD_STATUS 1 DOWNLOAD_ERR)
        file(REMOVE "${MINIMP3_HEADER}")
        message(FATAL_ERROR
            "[minimp3] Tải tự động thất bại (${DOWNLOAD_ERR}). "
            "Vui lòng tải thủ công file minimp3.h từ https://github.com/lieff/minimp3 "
            "và đặt vào: ${MINIMP3_HEADER}"
        )
    endif()
endif()

idf_component_register(
    INCLUDE_DIRS "include"
)

