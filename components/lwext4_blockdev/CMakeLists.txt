cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# lwext4_blockdev — ESP-IDF component
#
# This component bridges the ESP32 SDMMC driver to the lwext4 library and
# registers the resulting block device with the ESP-IDF VFS so that standard
# POSIX file operations work on EXT4 partitions.
#
# Prerequisites
# -------------
# Clone the lwext4 source into this directory before building:
#
#   cd emmc-fs/components/lwext4_blockdev
#   git clone https://github.com/gkostka/lwext4 lwext4
#
# The CMakeLists.txt will abort with a helpful message if lwext4 is missing.
# ---------------------------------------------------------------------------

set(LWEXT4_DIR "${CMAKE_CURRENT_LIST_DIR}/lwext4")

set(LWEXT4_PATCHES_DIR "${CMAKE_CURRENT_LIST_DIR}/patches")
set(LWEXT4_PATCH_STAMP "${LWEXT4_DIR}/.patches_applied")

if(NOT EXISTS "${LWEXT4_DIR}/src/ext4.c")
    message(STATUS "[lwext4_blockdev] lwext4 not found – cloning from GitHub...")
    execute_process(
        COMMAND git clone --branch master https://github.com/gkostka/lwext4 "${LWEXT4_DIR}"
        RESULT_VARIABLE GIT_RESULT
        OUTPUT_VARIABLE GIT_OUTPUT
        ERROR_VARIABLE  GIT_ERROR
    )
    if(NOT GIT_RESULT EQUAL 0)
        message(FATAL_ERROR
            "[lwext4_blockdev] git clone failed (exit ${GIT_RESULT}).\n"
            "Please clone manually:\n"
            "  cd ${CMAKE_CURRENT_LIST_DIR}\n"
            "  git clone https://github.com/gkostka/lwext4 lwext4\n"
            "stderr: ${GIT_ERROR}")
    endif()
    message(STATUS "[lwext4_blockdev] lwext4 cloned successfully.")
endif()

# Apply local patches to lwext4 (idempotent: skip if stamp file exists).
# Patches live in patches/ and are NOT committed to the lwext4 subtree so
# the upstream source stays clean and can be updated with `git pull`.
if(NOT EXISTS "${LWEXT4_PATCH_STAMP}")
    file(GLOB PATCH_FILES "${LWEXT4_PATCHES_DIR}/*.patch")
    list(SORT PATCH_FILES)
    foreach(PATCH_FILE ${PATCH_FILES})
        get_filename_component(PATCH_NAME "${PATCH_FILE}" NAME)
        message(STATUS "[lwext4_blockdev] Applying patch: ${PATCH_NAME}")
        execute_process(
            COMMAND git apply --whitespace=fix "${PATCH_FILE}"
            WORKING_DIRECTORY "${LWEXT4_DIR}"
            RESULT_VARIABLE PATCH_RESULT
            ERROR_VARIABLE  PATCH_ERROR
        )
        if(NOT PATCH_RESULT EQUAL 0)
            # Already applied? try --check first next time; for now warn and continue.
            message(WARNING
                "[lwext4_blockdev] patch '${PATCH_NAME}' failed (already applied?): "
                "${PATCH_ERROR}")
        endif()
    endforeach()
    file(WRITE "${LWEXT4_PATCH_STAMP}" "patches applied\n")
endif()

# Collect all lwext4 .c sources
file(GLOB LWEXT4_SRCS "${LWEXT4_DIR}/src/*.c")

idf_component_register(
    SRCS
        "lwext4_blockdev.c"
        "lwext4_vfs.c"
        "fat_partition.c"
        ${LWEXT4_SRCS}
    INCLUDE_DIRS
        "include"
        "${LWEXT4_DIR}/include"
    REQUIRES
        sdmmc
        esp_driver_sdmmc
        vfs
        fatfs
        esp_common
        log
)

# lwext4 compile-time configuration
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    # Skip generated/ext4_config.h and use defaults from ext4_config.h directly
    CONFIG_USE_DEFAULT_CFG=1
    # Number of simultaneously registered block devices
    CONFIG_EXT4_BLOCKDEVS_COUNT=1
    # Enable directory iterator caching for better performance
    CONFIG_EXT4_DIRECTORY_ITERATOR_CACHE_BACKEND=1
    # Use system errno (not lwext4's own)
    CONFIG_EXT4_HAVE_OWN_ERRNO=0
    # Disable journaling for benchmark (removes 3x write amplification).
    # Re-enable for production use to ensure crash consistency.
    CONFIG_EXT4_JOURNAL_EN=0
    CONFIG_JOURNALING_ENABLE=0
    # Large block cache: 128 x 4KB = 512KB — reduces cache-miss writeback stalls
    CONFIG_BLOCK_DEV_CACHE_SIZE=128
)

# Suppress some pedantic warnings that exist in upstream lwext4
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-sign-compare
)
