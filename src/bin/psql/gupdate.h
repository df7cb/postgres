/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * src/bin/psql/gupdate.h
 */

bool process_command_gupdate_options(PsqlScanState scan_state, bool active_branch);
bool gupdate_create_update_query(PQExpBuffer query_buf);
