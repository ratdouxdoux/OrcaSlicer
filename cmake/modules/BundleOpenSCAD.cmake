# OpenSCAD runs the AI assistant's generate_model code as a separate program
# (src/slic3r/Utils/AIOpenSCAD.cpp). bundle_openscad() downloads the pinned
# Windows release once into the build folder, checks its SHA-256, copies it next
# to the target's executable for development runs and installs it to
# <install>/openscad. Without it, generate_model reports that OpenSCAD is missing.
set(OPENSCAD_VERSION "2026.11-RC0")
set(OPENSCAD_URL "https://files.openscad.org/rc/OpenSCAD-${OPENSCAD_VERSION}-x86-64.zip")
set(OPENSCAD_SHA256 "bc43fe44712bd063b7d8065fc6ca051daf7271f03379f25bdb0f5bbfeed56f63")
set(_bundle_openscad_dir "${CMAKE_CURRENT_LIST_DIR}")

function(bundle_openscad target)
    if (NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        return()
    endif ()
    set(_dir "${CMAKE_BINARY_DIR}/openscad")
    set(_zip "${_dir}/OpenSCAD-${OPENSCAD_VERSION}-x86-64.zip")
    set(_src "${_dir}/OpenSCAD-${OPENSCAD_VERSION}-x86-64")
    set(_hash "")
    if (EXISTS "${_zip}")
        file(SHA256 "${_zip}" _hash)
    endif ()
    if (NOT _hash STREQUAL OPENSCAD_SHA256)
        message(STATUS "Downloading OpenSCAD ${OPENSCAD_VERSION} for the AI assistant")
        file(DOWNLOAD "${OPENSCAD_URL}" "${_zip}" EXPECTED_HASH SHA256=${OPENSCAD_SHA256} STATUS _status)
        list(GET _status 0 _code)
        if (NOT _code EQUAL 0)
            file(REMOVE "${_zip}")
            message(WARNING "OpenSCAD could not be downloaded (${_status}); the AI assistant cannot generate models from code.")
            return()
        endif ()
        file(REMOVE_RECURSE "${_src}")
    endif ()
    if (NOT EXISTS "${_src}/openscad.exe")
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf "${_zip}" WORKING_DIRECTORY "${_dir}" RESULT_VARIABLE _result)
        if (NOT _result EQUAL 0 OR NOT EXISTS "${_src}/openscad.exe")
            message(WARNING "OpenSCAD could not be extracted; the AI assistant cannot generate models from code.")
            return()
        endif ()
    endif ()
    # Only what the command line uses: the GUI's examples, templates and
    # translations are left out, and libraries because include and use are rejected.
    install(FILES "${_src}/openscad.exe" "${_bundle_openscad_dir}/OpenSCAD-NOTICE.txt" DESTINATION openscad)
    install(DIRECTORY "${_src}/fonts" "${_src}/color-schemes" "${_src}/shaders" DESTINATION openscad)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DSOURCE=${_src} -DNOTICE=${_bundle_openscad_dir}/OpenSCAD-NOTICE.txt
                -DDESTINATION=$<TARGET_FILE_DIR:${target}>/openscad -P "${_bundle_openscad_dir}/CopyOpenSCAD.cmake"
        VERBATIM)
endfunction()
