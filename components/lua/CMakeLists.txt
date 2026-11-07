# Lua 5.5.1 (MIT, see LICENSE), unmodified sources from lua.org
# (lua-5.5.1.tar.gz, sha256 1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce).
# The interpreter/compiler executables (lua.c, luac.c) are left out. Which libraries a
# script gets is decided by components/automation (sandbox), not here.
file(GLOB LUA_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
idf_component_register(
    SRCS ${LUA_SRCS}
    INCLUDE_DIRS "src"
)
# C-stack guard: nested calls and parser levels a script may use. Lua's default (200)
# assumes a desktop C stack; the automation task's stack is sized for this limit.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LUAI_MAXCCALLS=100)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-error)
