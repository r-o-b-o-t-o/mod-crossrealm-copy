# Expose the MySQL client headers to module code so the module can open its own
# connection to the source realm's characters database (same approach as
# mod-chat-transmitter). The client library itself is already part of the final
# link through the database project.
if(MYSQL_INCLUDE_DIR)
  target_include_directories(modules PUBLIC "${MYSQL_INCLUDE_DIR}")
endif()

# Optional mod-transmog integration: when both modules are compiled statically
# into the same binary, the copy can refresh mod-transmog's in-memory appearance
# collection cache directly instead of requiring a server restart (or a manual
# ".transmog reload") for the merged appearances to show up. The database-level
# copy of the transmog tables works either way; this only gates the direct call
# into mod-transmog code.
if((MODULE_MOD-CROSSREALM-COPY STREQUAL "static") AND (MODULE_MOD-TRANSMOG STREQUAL "static"))
  target_compile_definitions(modules PRIVATE MOD_CROSSREALM_COPY_WITH_TRANSMOG)
  message(STATUS "mod-crossrealm-copy: mod-transmog found, enabling live transmog collection cache refresh")
endif()
