# Copies the bundled OpenSCAD next to a freshly built executable. file(COPY)
# skips files whose timestamps already match, so rebuilds stay fast.
file(COPY "${SOURCE}/openscad.exe" "${SOURCE}/fonts" "${SOURCE}/color-schemes" "${SOURCE}/shaders" "${NOTICE}" DESTINATION "${DESTINATION}")
