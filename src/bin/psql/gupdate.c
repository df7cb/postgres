/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * src/bin/psql/gupdate.c
 */

#include "postgres_fe.h"
#include "catalog/pg_type_d.h"
#include "common.h"
#include "common/logging.h"
#include "pqexpbuffer.h"
#include "psqlscanslash.h"
#include "settings.h"

#include "gupdate.h"

static char *gupdate_table = NULL;
static char **gupdate_key_columns = NULL;
static bool gupdate_expanded;

/*
 * Process a single option for \gupdate
 */
static bool
process_command_gupdate_option(char *option, char *valptr)
{
	if (strcmp(option, "table") == 0)
	{
		if (valptr && *valptr)
		{
			if (gupdate_table)
				free(gupdate_table);

			gupdate_table = pstrdup(valptr);
		}
		else
		{
			pg_log_error("\\gupdate: missing table name in table option");
			return false;
		}
	}
	else if (strcmp(option, "key") == 0)
	{
		if (valptr && *valptr)
		{
			char *p;
			int nkeycols = 1;
			char *saveptr = NULL;
			int i = 0;
			char *tok;

			/* free key columns from last invocation */
			if (gupdate_key_columns)
			{
				char **pp;
				for (pp = gupdate_key_columns; *pp; pp++)
					free(*pp);
				free(gupdate_key_columns);
			}

			/* count number of tokens. Currently, this assumes no commas in table names and no quoting */
			for (p = valptr; *p; p++)
			{
				if (*p == ',')
					nkeycols++;
			}

			gupdate_key_columns = calloc(nkeycols + 1, sizeof(char *));
			if (! gupdate_key_columns)
			{
				pg_log_error("\\gupdate: out of memory");
				return false;
			}

			do {
				tok = strtok_r(saveptr ? NULL : valptr, ",", &saveptr);
				if (tok)
					gupdate_key_columns[i++] = strdup(tok);
			} while (tok);
			gupdate_key_columns[i] = NULL; /* NULL-terminate array */

		}
		else
		{
			pg_log_error("\\gupdate: missing column names in key option");
			return false;
		}
	}
	else if (strcmp(option, "x") == 0 || strcmp(option, "expanded") == 0 || strcmp(option, "vertical") == 0)
	{
		gupdate_expanded = true;
	}
	else
	{
		pg_log_error("\\gupdate: unknown option \"%s\"", option);
		return false;
	}

	return true;
}

/*
 * Process parenthesized options for \gupdate
 */
bool
process_command_gupdate_options(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;
	bool		first_option = true;
	bool		found_r_paren = false;

	/* reset to defaults */
	gupdate_expanded = pset.popt.topt.expanded; /* honor global \x mode */

	do
	{
		char	   *option;
		size_t		optlen;

		option = psql_scan_slash_option(scan_state,
										OT_NORMAL, NULL, false);
		if (!option)
		{
			if (active_branch && !first_option)
			{
				pg_log_error("\\gupdate: missing right parenthesis in options");
				success = false;
			}
			break;
		}

		/* Skip over '(' in first option */
		if (first_option)
		{
			char *tmp;

			if (option[0] != '(')
			{
				pg_log_error("\\gupdate: missing left parenthesis in options");
				success = false;
				break;
			}

			tmp = strdup(option + 1);
			free(option);
			option = tmp;
			first_option = false;
		}

		/* Check for terminating right paren, and remove it from string */
		optlen = strlen(option);
		if (optlen > 0 && option[optlen - 1] == ')')
		{
			option[--optlen] = '\0';
			found_r_paren = true;
		}

		/* If there was anything besides parentheses, parse/execute it */
		if (optlen > 0)
		{
			/* We can have either "name" or "name=value" */
			char	   *valptr = strchr(option, '=');

			if (valptr)
				*valptr++ = '\0';
			if (active_branch)
				success &= process_command_gupdate_option(option, valptr);
		}

		free(option);
	} while (!found_r_paren);

	return success;
}

/*
 * Extract table name from query
 */
static char *
gupdate_table_name(PQExpBuffer query_buf)
{
	char *query = strdup(query_buf->data);
	char *tok;
	char *saveptr = NULL;
	char *res = NULL;

	do {
		tok = strtok_r(saveptr ? NULL : query, " \t", &saveptr);
		if (tok && (strcasecmp(tok, "FROM") == 0 || strcasecmp(tok, "TABLE") == 0))
		{
			if ((res = strtok_r(NULL, " \t", &saveptr))) /* next word or NULL */
			{
				int l = strlen(res);
				Assert (l > 0);

				res = pstrdup(res);
				if (res[l-1] == ';') /* strip trailing ; */
					res[l-1] = '\0';
			}
			break;
		}
	} while (tok != NULL);

	free(query);

	return res;
}

/*
 * Append one array-quoted value to a buffer.
 */
static void
PQExpBufferAppendArrayValue(PQExpBuffer buf, const char *value)
{
	const char *p;
	appendPQExpBufferChar(buf, '"');
	for (p = value; *p; p++)
	{
		if (*p == '"')
			appendPQExpBufferStr(buf, "\\\"");
		else if (*p == '\\')
			appendPQExpBufferStr(buf, "\\\\");
		else
			appendPQExpBufferChar(buf, *p);
	}
	appendPQExpBufferChar(buf, '"');
}

/*
 * Describe a query, and return the list of columns as an array literal.
 */
static bool
gupdate_get_query_columns(PGconn *conn, const char *query, PQExpBuffer query_columns)
{
	PGresult	*result = PQprepare(conn, "", query, 0, NULL);
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		pg_log_error("%s", PQerrorMessage(pset.db));
		PQclear(result);
		return false;
	}
	PQclear(result);

	result = PQdescribePrepared(pset.db, "");
	if (PQresultStatus(result) == PGRES_COMMAND_OK)
	{
		appendPQExpBufferChar(query_columns, '{');
		if (PQnfields(result) > 0)
		{
			int i;
			for (i = 0; i < PQnfields(result); i++)
			{
				char *name;
				name = PQfname(result, i);
				if (i > 0)
					appendPQExpBufferChar(query_columns, ',');
				PQExpBufferAppendArrayValue(query_columns, name);
			}
		}
		appendPQExpBufferChar(query_columns, '}');
	}

	return true;
}

/*
 * Given a query and a table, get a table key that is contained in the query
 * columns.
 */
static char **
gupdate_table_key_columns(PGconn *conn, const char *query, const char *table)
{
	PQExpBuffer query_columns = createPQExpBuffer();
	char		key_query[] =
		"WITH keys AS (SELECT array_agg(attname ORDER BY ordinality) keycols, indisprimary "
		"FROM pg_index, unnest(indkey) with ordinality u(keycol) "
		"JOIN pg_attribute ON keycol = attnum "
		"WHERE indrelid = $1::regclass AND attrelid = $1::regclass AND (indisprimary or indisunique) "
		"GROUP BY indexrelid), "
		"key AS (SELECT keycols FROM keys WHERE keycols <@ $2 " /* all keys contained in the query columns */
		"ORDER BY indisprimary DESC LIMIT 1) " /* prefer primary key over unique indexes */
		"SELECT unnest(keycols) FROM key;";
	Oid			types[] = { NAMEOID, NAMEARRAYOID };
	const char *values[] = { table, NULL };
	char	   **gupdate_key_columns = NULL;
	PGresult   *res;

	if (! gupdate_get_query_columns(pset.db, query, query_columns))
	{
		destroyPQExpBuffer(query_columns);
		return NULL;
	}
	values[1] = query_columns->data;
	res = PQexecParams(conn, key_query, 2, types, values, NULL, NULL, 0);
	destroyPQExpBuffer(query_columns);

	if (PQresultStatus(res) == PGRES_TUPLES_OK)
	{
		int nkeycols = PQntuples(res);
		if (nkeycols > 0)
		{
			int i;
			gupdate_key_columns = calloc(nkeycols + 1, sizeof(char *));
			if (! gupdate_key_columns)
			{
				pg_log_error("\\gupdate: Out of memory.");
				return NULL;
			}

			for (i = 0; i < nkeycols; i++)
			{
				gupdate_key_columns[i] = strdup(PQgetvalue(res, i, 0));
			}
		}
		else
		{
			pg_log_error("\\gupdate: no key of table \"%s\" is contained in the returned query columns.", table);
			pg_log_error_hint("Select more columns or manually specify a key using \\gupdate (key=col1,...)");
		}
	}
	else if (PQresultStatus(res) == PGRES_FATAL_ERROR)
	{
		char *val = PQresultErrorField(res, PG_DIAG_SQLSTATE);
		if (strcmp(val, "42P01") == 0)
			pg_log_error("\\gupdate: table \"%s\" does not exist", table);
		else
			pg_log_error("\\gupdate: error while retrieving key columns of table \"%s\": %s",
					table, PQerrorMessage(pset.db));
	}
	PQclear(res);

	return gupdate_key_columns;
}

/*
 * Check if a query contains the given columns.
 */
static bool
gupdate_check_key_columns(PGconn *conn, const char *query, char **key_columns)
{
	char **key_column;
	PGresult	*result = PQprepare(conn, "", query, 0, NULL);

	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		pg_log_error("\\gupdate: %s", PQerrorMessage(conn));
		PQclear(result);
		return false;
	}
	PQclear(result);

	result = PQdescribePrepared(conn, "");
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		pg_log_error("\\gupdate: %s", PQerrorMessage(conn));
		PQclear(result);
		return false;
	}

	if (PQnfields(result) == 0)
	{
		pg_log_error("\\gupdate: query has no columns");
		PQclear(result);
		return false;
	}

	for (key_column = key_columns; *key_column; key_column++)
	{
		bool found = false;
		int i;
		for (i = 0; i < PQnfields(result); i++)
		{
			if (strcmp(*key_column, PQfname(result, i)) == 0)
			{
				found = true;
				break;
			}
		}

		if (! found)
		{
			pg_log_error("\\gupdate: key column \"%s\" not found in query", *key_column);
			PQclear(result);
			return false;
		}
	}

	PQclear(result);
	return true;
}

/* main code */

/*
 * Check if value consists entirely of alphanumeric chars and the first
 * character is not a digit.
 */
static bool
identifier_needs_no_quoting(const char *ident)
{
	return strspn(ident, "abcdefghijklmnopqrstuvwxyz_0123456789") == strlen(ident) &&
		   strspn(ident, "0123456789") == 0;
}

/*
 * Format an "identifier", using quotes only when necessary.
 */
static void
format_identifier(PQExpBuffer query_buf, char *ident)
{
	if (identifier_needs_no_quoting(ident)) /* skip quoting of simple names */
		appendPQExpBufferStr(query_buf, ident);
	else
	{
		char *p = PQescapeIdentifier(pset.db, ident, strlen(ident));
		appendPQExpBufferStr(query_buf, p);
		free(p);
	}
}

/*
 * Format a given cell as "key" = 'value'.
 */
static void
format_key_value(PQExpBuffer query_buf, PGresult *res, int row, int col)
{
	format_identifier(query_buf, PQfname(res, col));

	appendPQExpBufferStr(query_buf, " = ");

	if (PQgetisnull(res, row, col))
	{
		appendPQExpBufferStr(query_buf, "NULL");
	}
	else
	{
		char *val = PQgetvalue(res, row, col);
		char *p = PQescapeLiteral(pset.db, val, strlen(val));

		appendPQExpBufferStr(query_buf, p);
		free(p);
	}
}

/*-
 * Given an SQL query,
 * 1. determine the table used (unless given with table=...),
 * 2. describe the query and get a table key contained in the result columns
 *    (unless given with key=...),
 * 3. run the query,
 * 4. format the result as "update table set ... where key=...;" statements.
 */
bool
gupdate_create_update_query(PQExpBuffer query_buf)
{
	PGresult   *res;
	int			rows, cols;
	int			r, c;
	bool	   *is_index_column;

	if (! gupdate_table)
	{
		gupdate_table = gupdate_table_name(query_buf);
		if (! gupdate_table)
		{
			pg_log_error("\\gupdate: Could not determine table name from query");
			return false;
		}
	}

	if (gupdate_key_columns)
	{
		if (! gupdate_check_key_columns(pset.db, query_buf->data, gupdate_key_columns))
			return false;
	}
	else
	{
		gupdate_key_columns = gupdate_table_key_columns(pset.db, query_buf->data, gupdate_table);
		if (! gupdate_key_columns)
			return false;
	}

	res = PSQLexec(query_buf->data);
	if (!res)
		return false;

	resetPQExpBuffer(query_buf);

	rows = PQntuples(res);
	cols = PQnfields(res);

	is_index_column = alloca(sizeof(bool) * cols);
	for (c = 0; c < cols; c++) {
		char **cp;

		is_index_column[c] = false;
		for (cp = gupdate_key_columns; *cp; cp++)
			if (strcmp(PQfname(res, c), *cp) == 0)
			{
				is_index_column[c] = true;
				break;
			}
	}

	for (r = 0; r < rows; r++)
	{
		bool need_separator = false;

		appendPQExpBufferStr(query_buf, "UPDATE ");
		format_identifier(query_buf, gupdate_table);
		appendPQExpBufferStr(query_buf, gupdate_expanded ? " SET\n  " : " SET ");

		/* print fields to update */
		for (c = 0; c < cols; c++)
		{
			if (is_index_column[c])
				continue;
			if (need_separator)
				appendPQExpBufferStr(query_buf, gupdate_expanded ? ",\n  " : ", ");
			format_key_value(query_buf, res, r, c);
			need_separator = true;
		}

		if (gupdate_expanded)
			appendPQExpBufferStr(query_buf, "\n ");
		appendPQExpBufferStr(query_buf, " WHERE ");

		/* print WHERE condition */
		need_separator = false;
		for (c = 0; c < cols; c++)
		{
			if (! is_index_column[c])
				continue;
			if (need_separator)
				appendPQExpBufferStr(query_buf, " AND ");
			format_key_value(query_buf, res, r, c);
			need_separator = true;
		}

		appendPQExpBufferStr(query_buf, ";\n");
	}

	PQclear(res);

	return true;
}
