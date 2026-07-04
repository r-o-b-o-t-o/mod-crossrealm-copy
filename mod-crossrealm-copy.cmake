# Expose the MySQL client headers to module code so the module can open its own
# connection to the source realm's characters database (same approach as
# mod-chat-transmitter). The client library itself is already part of the final
# link through the database project.
if(MYSQL_INCLUDE_DIR)
  target_include_directories(modules PUBLIC "${MYSQL_INCLUDE_DIR}")
endif()
