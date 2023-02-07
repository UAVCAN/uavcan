#
# This treats the doxygen build for libcyphal as a standalone program. In
# reality libcyphal_docs is a doxygen build configured just for this project.
#


find_package(Doxygen QUIET)
find_program(TAR tar)

# +---------------------------------------------------------------------------+
# | DOXYGEN
# +---------------------------------------------------------------------------+

#
# :function: create_docs_target
# Create a target that generates documentation.
#
# :param str ARG_DOCS_TARGET_NAME:  The name to give the target created by this function.
#                                   This is also used as a prefix for sub-targets also
#                                   generated by this function.
# :param bool ARG_ADD_TO_ALL:       If true the target is added to the default build target.
#
function (create_docs_target ARG_DOCS_TARGET_NAME ARG_ADD_TO_ALL)

    set(DOXYGEN_SOURCE ${LIBCYPHAL_PROJECT_ROOT}/doc_source)
    set(DOXYGEN_RDOMAIN org.opencyphal)
    set(DOXYGEN_RDOMAIN_W_PROJECT org.opencyphal.libcyphal)
    set(DOXYGEN_PROJECT_NAME "libcyphal")
    set(DOXYGEN_PROJECT_BRIEF "Portable reference implementation of the Cyphal protocol stack written in C++ for embedded systems, Linux, and POSIX-compliant RTOSs")
    set(DOXYGEN_OUTPUT_DIRECTORY_PARENT ${CMAKE_BINARY_DIR})
    set(DOXYGEN_OUTPUT_DIRECTORY ${DOXYGEN_OUTPUT_DIRECTORY_PARENT}/docs)
    set(DOXYGEN_CONFIG_FILE ${DOXYGEN_OUTPUT_DIRECTORY}/doxygen.config)
    set(DOXYGEN_INPUT "\"${LIBCYPHAL_PROJECT_ROOT}/libcyphal/include\" \
                       \"${LIBCYPHAL_PROJECT_ROOT}/README.md\" \
                       \"${LIBCYPHAL_PROJECT_ROOT}/CONTRIBUTING.md\" \
                       \"${LIBCYPHAL_PROJECT_ROOT}/doc_source/related\" \
                       \"${LIBCYPHAL_PROJECT_ROOT}/libcyphal_validation_suite/include\" \
                       \"${CMAKE_CURRENT_SOURCE_DIR}/linux/example\" \
                       ")
    set(DOXYGEN_MAINPAGE "\"${LIBCYPHAL_PROJECT_ROOT}/README.md\"")
    set(DOXYGEN_LIBCYPHAL_VERSION $ENV{BUILDKITE_BUILD_NUMBER})
    set(DOXYGEN_LIBCYPHAL_INCLUDE_PREFIX_STRIP "\"${LIBCYPHAL_PROJECT_ROOT}/libcyphal/include\"")

    # +-----------------------------------------------------------------------+
    # | HTML (BOOTSTRAPPED)
    # +-----------------------------------------------------------------------+
    set(DOXYGEN_HTML_EXTRA_FILES "${DOXYGEN_SOURCE}/doxygen-bootstrapped/doxy-boot.js ${DOXYGEN_SOURCE}/doxygen-bootstrapped/jquery.smartmenus.js ${DOXYGEN_SOURCE}/doxygen-bootstrapped/addons/bootstrap/jquery.smartmenus.bootstrap.js ${DOXYGEN_SOURCE}/doxygen-bootstrapped/addons/bootstrap/jquery.smartmenus.bootstrap.css ${DOXYGEN_SOURCE}/.nojekyll")
    set(DOXYGEN_HTML_STYLESHEET ${DOXYGEN_OUTPUT_DIRECTORY}/customdoxygen.css)
    set(DOXYGEN_HTML_HEADER ${DOXYGEN_OUTPUT_DIRECTORY}/header.html)
    set(DOXYGEN_HTML_FOOTER ${DOXYGEN_OUTPUT_DIRECTORY}/footer.html)
    set(DOXYGEN_IMAGE_PATH ${DOXYGEN_OUTPUT_DIRECTORY}/doc_source/images)
    set(DOXYGEN_LOGO ${DOXYGEN_OUTPUT_DIRECTORY}/doc_source/images/html/cyphal_logo.svg)

    file(COPY ${DOXYGEN_SOURCE}/images DESTINATION ${DOXYGEN_OUTPUT_DIRECTORY}/doc_source)

    configure_file(${DOXYGEN_SOURCE}/header.html
                    ${DOXYGEN_OUTPUT_DIRECTORY}/header.html
                )
    configure_file(${DOXYGEN_SOURCE}/footer.html
                    ${DOXYGEN_OUTPUT_DIRECTORY}/footer.html
                )
    configure_file(${DOXYGEN_SOURCE}/doxygen-bootstrapped/customdoxygen.css
                    ${DOXYGEN_OUTPUT_DIRECTORY}/customdoxygen.css
                )
    configure_file(${DOXYGEN_SOURCE}/doxygen.ini
                    ${DOXYGEN_CONFIG_FILE}
                )

    add_custom_command(OUTPUT ${DOXYGEN_OUTPUT_DIRECTORY}/html/index.html
                                ${DOXYGEN_OUTPUT_DIRECTORY}/latex/refman.tex
                        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_CONFIG_FILE}
                        DEPENDS ${DOXYGEN_CONFIG_FILE}
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        COMMENT "Generating intermediate documentation."
                    )

    add_custom_target(${ARG_DOCS_TARGET_NAME}-html DEPENDS ${DOXYGEN_OUTPUT_DIRECTORY}/html/index.html)

    add_custom_command(OUTPUT ${DOXYGEN_OUTPUT_DIRECTORY}/html.gz
                        COMMAND ${TAR} -vzcf docs/html.gz docs/html/
                        DEPENDS ${DOXYGEN_OUTPUT_DIRECTORY}/html/index.html
                        WORKING_DIRECTORY ${DOXYGEN_OUTPUT_DIRECTORY_PARENT}
                        COMMENT "Creating html tarball."
                    )

    if (ARG_ADD_TO_ALL)
        add_custom_target(${ARG_DOCS_TARGET_NAME} ALL DEPENDS ${DOXYGEN_OUTPUT_DIRECTORY}/html.gz)
    else()
        add_custom_target(${ARG_DOCS_TARGET_NAME} DEPENDS ${DOXYGEN_OUTPUT_DIRECTORY}/html.gz)
    endif()

endfunction(create_docs_target)


include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(libcyphal_docs
    REQUIRED_VARS DOXYGEN_FOUND
)
