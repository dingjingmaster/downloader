include(ExternalProject)
find_program(DOWNLOADER_MAKE NAMES gmake make REQUIRED)
find_program(DOWNLOADER_AUTORECONF autoreconf REQUIRED)
find_program(DOWNLOADER_PKG_CONFIG pkg-config REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(EXPAT REQUIRED)
find_package(LibXml2 REQUIRED)
include(CheckLibraryExists)
check_library_exists(atomic __atomic_load_8 "" DOWNLOADER_HAVE_LIBATOMIC)

# Keep external configure scripts on the same system headers/libraries selected
# by find_package, including custom CMAKE_PREFIX_PATH / OPENSSL_ROOT_DIR builds.
set(dependency_include_dirs ${OPENSSL_INCLUDE_DIR} ${EXPAT_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS} ${LIBXML2_INCLUDE_DIRS})
list(REMOVE_DUPLICATES dependency_include_dirs)
set(dependency_cppflags "")
foreach(directory IN LISTS dependency_include_dirs)
    string(APPEND dependency_cppflags " -I${directory}")
endforeach()
set(dependency_ldflags "")
set(dependency_pkgconfig "")
foreach(library OpenSSL::SSL OpenSSL::Crypto EXPAT::EXPAT ZLIB::ZLIB LibXml2::LibXml2)
    string(APPEND dependency_ldflags " -L$<TARGET_FILE_DIR:${library}>")
    string(APPEND dependency_pkgconfig "$<TARGET_FILE_DIR:${library}>/pkgconfig:")
endforeach()
foreach(directory IN LISTS CMAKE_PREFIX_PATH)
    string(APPEND dependency_pkgconfig "${directory}/lib/pkgconfig:${directory}/lib64/pkgconfig:${directory}/share/pkgconfig:")
endforeach()
string(APPEND dependency_pkgconfig "$ENV{PKG_CONFIG_PATH}")

set(DOWNLOADER_DEPENDENCY_JOBS 4 CACHE STRING "Parallel jobs for each bundled dependency")
set(dependency_prefix "${CMAKE_CURRENT_BINARY_DIR}/installed")
file(MAKE_DIRECTORY "${dependency_prefix}/include")

foreach(dependency aria2c ffmpeg libsmb2)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dependency}/.git")
        message(FATAL_ERROR "Missing ${dependency} sources: run git submodule update --init --recursive")
    endif()
endforeach()

# Bootstrap a build-tree copy: autoreconf must not modify the pinned checkout.
ExternalProject_Add(bundled_aria2
    SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/aria2-source"
    DOWNLOAD_COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/aria2c" "<SOURCE_DIR>"
    UPDATE_COMMAND ""
    PATCH_COMMAND "${DOWNLOADER_AUTORECONF}" -fi
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
        "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
        "CFLAGS=-O2 -fPIC" "CXXFLAGS=-O2 -fPIC"
        "CPPFLAGS=${dependency_cppflags}" "LDFLAGS=${dependency_ldflags}"
        "PKG_CONFIG=${DOWNLOADER_PKG_CONFIG}" "PKG_CONFIG_PATH=${dependency_pkgconfig}"
        "<SOURCE_DIR>/configure" "--prefix=${dependency_prefix}"
        --enable-libaria2 --enable-static --disable-shared --disable-nls
        --with-openssl --with-libexpat --with-libz
        --without-gnutls --without-libnettle --without-libgmp --without-libgcrypt
        --without-libxml2 --without-libcares --without-libssh2 --without-sqlite3
        --without-libuv --disable-websocket
    BUILD_COMMAND "${DOWNLOADER_MAKE}" -C src -j${DOWNLOADER_DEPENDENCY_JOBS} libaria2.la
    INSTALL_COMMAND "${DOWNLOADER_MAKE}" -C src install-libLTLIBRARIES
        COMMAND "${DOWNLOADER_MAKE}" -C src/includes install
    BUILD_BYPRODUCTS "${dependency_prefix}/lib/libaria2.a"
    LOG_DOWNLOAD ON LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON
)

# Demux/remux only. A small decoder set is needed by stream-info probing (e.g.
# AAC channel layout and H.264 dimensions); output packets are never re-encoded.
ExternalProject_Add(bundled_ffmpeg
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${dependency_pkgconfig}"
        "<SOURCE_DIR>/configure" "--prefix=${dependency_prefix}"
        "--cc=${CMAKE_C_COMPILER}" "--cxx=${CMAKE_CXX_COMPILER}"
        "--pkg-config=${DOWNLOADER_PKG_CONFIG}"
        "--extra-cflags=${dependency_cppflags}" "--extra-ldflags=${dependency_ldflags}"
        --enable-static --disable-shared --enable-pic
        --disable-programs --disable-doc --disable-autodetect --disable-everything
        --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample
        --enable-network --enable-openssl --enable-libxml2 --enable-zlib
        --enable-protocol=file,http,https,tcp,tls,crypto,udp
        --enable-demuxer=hls,dash,mov,mpegts,aac,mp3,matroska
        --enable-muxer=mp4,matroska,mpegts
        --enable-parser=aac,h264,hevc,mpegaudio,opus,vorbis,vp8,vp9,av1
        --enable-decoder=aac,h264,hevc,mp3,opus,vorbis,vp8,vp9
        --enable-bsf=aac_adtstoasc,h264_mp4toannexb,hevc_mp4toannexb,extract_extradata
    BUILD_COMMAND "${DOWNLOADER_MAKE}" -j${DOWNLOADER_DEPENDENCY_JOBS}
    INSTALL_COMMAND "${DOWNLOADER_MAKE}" install
    BUILD_BYPRODUCTS "${dependency_prefix}/lib/libavformat.a"
        "${dependency_prefix}/lib/libavcodec.a" "${dependency_prefix}/lib/libavutil.a"
    LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON
)

ExternalProject_Add(bundled_smb2
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libsmb2"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
    CMAKE_ARGS "-DCMAKE_INSTALL_PREFIX=${dependency_prefix}"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DBUILD_SHARED_LIBS=OFF -DENABLE_EXAMPLES=OFF
    PATCH_COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/apply-patches.sh" "${CMAKE_CURRENT_SOURCE_DIR}/libsmb2"
    BUILD_BYPRODUCTS "${dependency_prefix}/lib/libsmb2.a"
    LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON
)

foreach(library aria2 avformat avcodec avutil smb2)
    add_library(downloader_${library} STATIC IMPORTED GLOBAL)
    set_target_properties(downloader_${library} PROPERTIES
        IMPORTED_LOCATION "${dependency_prefix}/lib/lib${library}.a"
        INTERFACE_INCLUDE_DIRECTORIES "${dependency_prefix}/include")
endforeach()
add_dependencies(downloader_aria2 bundled_aria2)
add_dependencies(downloader_avformat bundled_ffmpeg)
add_dependencies(downloader_avcodec bundled_ffmpeg)
add_dependencies(downloader_avutil bundled_ffmpeg)
add_dependencies(downloader_smb2 bundled_smb2)
target_link_libraries(downloader_aria2 INTERFACE OpenSSL::SSL OpenSSL::Crypto EXPAT::EXPAT ZLIB::ZLIB Threads::Threads)
target_link_libraries(downloader_avformat INTERFACE downloader_avcodec downloader_avutil
    OpenSSL::SSL OpenSSL::Crypto LibXml2::LibXml2 ZLIB::ZLIB)
target_link_libraries(downloader_avcodec INTERFACE downloader_avutil)
target_link_libraries(downloader_avutil INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
if(DOWNLOADER_HAVE_LIBATOMIC)
    target_link_libraries(downloader_avutil INTERFACE atomic)
endif()

install(FILES aria2c/COPYING aria2c/LICENSE.OpenSSL
    DESTINATION share/licenses/downloader/aria2)
install(FILES ffmpeg/COPYING.LGPLv2.1 ffmpeg/LICENSE.md
    DESTINATION share/licenses/downloader/ffmpeg)
install(FILES libsmb2/COPYING libsmb2/LICENCE-LGPL-2.1.txt
    DESTINATION share/licenses/downloader/libsmb2)
install(FILES README.md DESTINATION share/licenses/downloader RENAME THIRD-PARTY.md)
