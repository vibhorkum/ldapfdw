/*-------------------------------------------------------------------------
 *
 *      foreign-data wrapper for LDAP
 *
 * Copyright (c) 2011, PostgreSQL Global Development Group
 *
 * This software is released under the PostgreSQL Licence
 *
 * Author: Dickson S. Guedes <guedes@guedesoft.net>
 *
 *
 *-------------------------------------------------------------------------
 */

#include "ldap_fdw.h"

PG_MODULE_MAGIC;

/*
 * Handler and Validator functions
 */
extern Datum ldap_fdw_handler(PG_FUNCTION_ARGS);
extern Datum ldap_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(ldap_fdw_handler);
PG_FUNCTION_INFO_V1(ldap_fdw_validator);

/*
 * FDW functions implementation
 */

Datum
ldap_fdw_handler(PG_FUNCTION_ARGS)
{
  FdwRoutine *fdw_routine = makeNode(FdwRoutine);

  fdw_routine->GetForeignRelSize   = ldapGetForeignRelSize;
  fdw_routine->GetForeignPaths     = ldapGetForeignPaths;
  fdw_routine->GetForeignPlan      = ldapGetForeignPlan;
  fdw_routine->ExplainForeignScan  = ldapExplainForeignScan;
  fdw_routine->BeginForeignScan    = ldapBeginForeignScan;
  fdw_routine->IterateForeignScan  = ldapIterateForeignScan;
  fdw_routine->ReScanForeignScan   = ldapReScanForeignScan;
  fdw_routine->EndForeignScan      = ldapEndForeignScan;
  fdw_routine->AnalyzeForeignTable = NULL;

  PG_RETURN_POINTER(fdw_routine);
}

Datum
ldap_fdw_validator(PG_FUNCTION_ARGS)
{
  List      *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
  Oid       catalog = PG_GETARG_OID(1);
  ListCell  *cell;

  foreach(cell, options_list)
  {
    DefElem    *def = (DefElem *) lfirst(cell);

    if (!_is_valid_option(def->defname, catalog))
    {
      struct LdapFdwOption *opt;
      StringInfoData buf;

      /*
       * Unknown option specified, complain about it. Provide a hint
       * with list of valid options for the object.
       */
      initStringInfo(&buf);
      for (opt = valid_options; opt->option_name; opt++)
      {
        if (catalog == opt->option_context)
          appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
                   opt->option_name);
      }

      ereport(ERROR,
          (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
           errmsg("invalid option \"%s\"", def->defname),
           errhint("Valid options in this context are: %s",
               buf.data)));
    }
  }

  PG_RETURN_BOOL(true);
}

static void
ldapGetForeignRelSize(PlannerInfo *root,
                      RelOptInfo *baserel,
                      Oid foreigntableid)
{
  baserel->rows = 500;
}
static void
ldapGetForeignPaths(PlannerInfo *root,
                    RelOptInfo *baserel,
                    Oid foreigntableid)
{
  Cost  total_cost, startup_cost;

  startup_cost = 10;
  total_cost = startup_cost + baserel->rows;

  add_path(baserel, (Path *)
           create_foreignscan_path(root, baserel,
#if PG_VERSION_NUM >= 90600
								   NULL,  /* default pathtarget */
#endif
                                   baserel->rows, startup_cost, total_cost,
                                   NIL, NULL, NULL
#if PG_VERSION_NUM >= 90500
                                   , NIL
#endif
                                   ));
}

static ForeignScan *
ldapGetForeignPlan(PlannerInfo *root,
                   RelOptInfo *baserel,
                   Oid foreigntableid,
                   ForeignPath *best_path,
                   List *tlist,
                   List *scan_clauses
#if PG_VERSION_NUM >= 90500
				   , Plan *outer_plan
#endif                                     
                   )
{
  Index scan_relid = baserel->relid;

  scan_clauses = extract_actual_clauses(scan_clauses, false);

  return make_foreignscan(tlist, scan_clauses, scan_relid, NIL, NIL
#if PG_VERSION_NUM >= 90500
							, NIL, NIL, outer_plan
#endif
  );
}

static void
ldapExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
  /* TODO: calculate real values */
  ExplainPropertyText("Foreign Ldap", "ldap", es);

  if (es->costs)
  {
    ExplainPropertyFloat("Foreign Ldap cost", 10.5, 1, es);
  }
}

static void
ldapBeginForeignScan(ForeignScanState *node, int eflags)
{
  LDAP                    *ldap_connection;
  LDAPMessage             *ldap_answer;
  int                     ldap_result;

  char                    *qual_key = NULL;
  char                    *qual_value = NULL;
  bool                    pushdown = false;

  LdapFdwExecutionState   *festate;
  LdapFdwConfiguration    *config;
  StringInfoData          query;

  if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
    return;

  config = (LdapFdwConfiguration *) palloc(sizeof(LdapFdwConfiguration));

  _ldap_get_options(RelationGetRelid(node->ss.ss_currentRelation), config);

  ldap_connection = ldap_init(config->address, config->port);

  if (ldap_connection == NULL)
    ereport(ERROR,
        (errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
         errmsg("failed to create LDAP handler for address '%s' on port '%d'", config->address, config->port)
        ));

  ldap_result = ldap_set_option(ldap_connection, LDAP_OPT_PROTOCOL_VERSION, &config->ldap_version);

  if (ldap_result != LDAP_SUCCESS)
    ereport(ERROR,
        (errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
         errmsg("failed to set version 3 for LDAP server on address '%s' port '%d'. LDAP ERROR: %s", config->address, config->port, ldap_err2string(ldap_result))
        ));

  ldap_result = ldap_simple_bind_s(ldap_connection, config->user_dn, config->password );

  if (ldap_result != LDAP_SUCCESS)
    ereport(ERROR,
        (errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
         errmsg("failed to authenticate to LDAP server using user_dn: %s. LDAP ERROR: %s", config->user_dn, ldap_err2string(ldap_result))
        ));

  initStringInfo(&query);
  /* See if we've got a qual we can push down */
  if (node->ss.ps.plan->qual)
  {
    ListCell *lc;

    foreach (lc, node->ss.ps.qual)
    {
      /* Only the first qual can be pushed down to Redis */
      ExprState  *state = lfirst(lc);

      _ldap_check_quals((Node *) state->expr, node->ss.ss_currentRelation->rd_att, &qual_key, &qual_value, &pushdown);

      if (pushdown)
      {
        node->ss.ps.qual = list_delete(node->ss.ps.qual, (void *) state);
        break;
      }
    }
  }

  /* Execute the query */
  if (qual_value && pushdown)
  {
    appendStringInfo(&query, "(&(%s)%s)", qual_value, (config->query == NULL ? "" : config->query));
  }
  else
    appendStringInfo(&query, "%s", (config->query == NULL ? "(objectClass=*)" : config->query ));

  ldap_result = ldap_search_ext_s(ldap_connection, config->base_dn, LDAP_SCOPE_ONELEVEL, query.data, _string_to_array(config->attributes), 0, NULL, NULL, NULL, LDAP_NO_LIMIT, &ldap_answer);

  if (ldap_result != LDAP_SUCCESS)
    ereport(ERROR,
        (errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
         errmsg("failed to execute the LDAP search '%s' on base_dn '%s'. LDAP ERROR: %s", query.data, config->base_dn, ldap_err2string(ldap_result))
        ));

  config->query = query.data;

  festate = (LdapFdwExecutionState *) palloc(sizeof(LdapFdwExecutionState));
  festate->ldap_connection = ldap_connection;
  festate->row = 0;
  festate->ldap_entry = ldap_first_entry(ldap_connection, ldap_answer);
  festate->att_in_metadata = TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att);
  festate->config = config;

  node->fdw_state = (void *) festate;
}

static TupleTableSlot *
ldapIterateForeignScan(ForeignScanState *node)
{
  LdapFdwExecutionState   *festate  = (LdapFdwExecutionState *) node->fdw_state;
  TupleTableSlot          *slot     = node->ss.ss_ScanTupleSlot;

  BerElement              *ber;
  HeapTuple               tuple;

  int                     i;

  char                    *dn;
  char                    *attribute;

  StringInfoData          object_body;
  char                    **values;
  char                    **temp;

  ExecClearTuple(slot);

  if (festate->ldap_entry != NULL)
  {
    dn = ldap_get_dn(festate->ldap_connection, festate->ldap_entry);

    initStringInfo(&object_body);

    for (attribute = ldap_first_attribute(festate->ldap_connection, festate->ldap_entry, &ber);
          attribute != NULL;
          attribute = ldap_next_attribute(festate->ldap_connection, festate->ldap_entry, ber))
    {

      appendStringInfo(&object_body, "%s => ", attribute);

      if ((temp = ldap_get_values(festate->ldap_connection, festate->ldap_entry, attribute)) != NULL)
      {
        bool is_array = ldap_count_values(temp) > 1;

        if (is_array)
        {
          appendStringInfo(&object_body, "\"{");

          for (i = 0; temp[i] != NULL; i++)
            appendStringInfo(&object_body, "%s\\\"%s\\\"", (i > 0) ? "," : "", temp[i]);

          appendStringInfo(&object_body, "}\"");
        }
        else
        {
          appendStringInfo(&object_body, "\"%s\"", temp[0]);
        }

        appendStringInfo(&object_body, ",\n");
        ldap_value_free(temp);
      }

      ldap_memfree(attribute);
    }

    values = (char **) palloc(sizeof(char *) * 2);
    values[0] = dn;
    values[1] = object_body.data;

    tuple = BuildTupleFromCStrings(festate->att_in_metadata, values);
    ExecStoreTuple(tuple, slot, InvalidBuffer, false);

    festate->ldap_entry = ldap_next_entry(festate->ldap_connection, festate->ldap_entry);
    ldap_memfree(dn);
    if (ber != NULL)
      ber_free(ber, 0);

    pfree(values);
  }

  return slot;
}

static void
ldapReScanForeignScan(ForeignScanState *node)
{

}

static void
ldapEndForeignScan(ForeignScanState *node)
{
  LdapFdwExecutionState   *festate  = (LdapFdwExecutionState *) node->fdw_state;

  if (!EXEC_FLAG_EXPLAIN_ONLY)
  {
     ldap_memfree(festate->ldap_entry);
	 ldap_unbind( festate->ldap_connection );
  }
}

/*
 * Helper functions
 */

/*
 * Extract the name of attributes from a Relation
 * and return a array of strings where each element
 * is the attribute name. The array is null-terminated
 */
static void
_get_str_attributes(char *attributes[], Relation relation)
{
  int total_attributes = relation->rd_att->natts;
  int i;

  for (i=0; i < total_attributes; i++)
  {
    Name attr_name = &relation->rd_att->attrs[i]->attname;
    attributes[i] = NameStr(*attr_name);
  }

  attributes[i++] = NULL;
}

/*
 * Case insensitive compare a 'name' with 'string'.
 * The string could or not be null-terminated.
 */
static int
_name_str_case_cmp(Name name, const char *str)
{
  if (!name && !str)
    return 0;
  if (!name)
    return -1;        /* NULL < anything */
  if (!str)
    return 1;       /* NULL < anything */
  return pg_strncasecmp(NameStr(*name), str, sizeof(str));
}

/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
_is_valid_option(const char *option, Oid context)
{
  struct LdapFdwOption *opt;

  for (opt = valid_options; opt->option_name; opt++)
  {
    if (context == opt->option_context && strcmp(opt->option_name, option) == 0)
      return true;
  }
  return false;
}

/*
 * Fetch the options for a ldap_fdw foreign table.
 */
static void
_ldap_get_options(Oid foreign_table_id, LdapFdwConfiguration *config)
{
  ForeignTable  *f_table;
  ForeignServer *f_server;
  UserMapping   *f_mapping;
  List          *options;
  ListCell      *lc;

  /*
   * Extract options from FDW objects.
   */
  f_table   = GetForeignTable(foreign_table_id);
  f_server  = GetForeignServer(f_table->serverid);
  f_mapping = GetUserMapping(GetUserId(), f_table->serverid);

  options   = NIL;
  options   = list_concat(options, f_table->options);
  options   = list_concat(options, f_server->options);
  options   = list_concat(options, f_mapping->options);

  config->address       = LDAP_FDW_OPTION_ADDRESS_DEFAULT;
  config->port          = LDAP_FDW_OPTION_PORT_DEFAULT;
  config->ldap_version  = LDAP_FDW_OPTION_LDAP_VERSION_DEFAULT;
  config->user_dn       = NULL;
  config->password      = NULL;
  config->attributes    = NULL;
  config->base_dn       = NULL;
  config->query         = NULL;

  foreach(lc, options)
  {
    DefElem *def = (DefElem *) lfirst(lc);

    if (strcmp(def->defname, LDAP_FDW_OPTION_ADDRESS) == 0)
      config->address = defGetString(def);
    else if (strcmp(def->defname, LDAP_FDW_OPTION_PORT) == 0)
      config->port = atoi(defGetString(def));
    else if (strcmp(def->defname, LDAP_FDW_OPTION_USER_DN) == 0)
      config->user_dn = defGetString(def);
    else if (strcmp(def->defname, LDAP_FDW_OPTION_PASSWORD) == 0)
      config->password = defGetString(def);
    else if (strcmp(def->defname, LDAP_FDW_OPTION_ATTRIBUTES) == 0)
      config->attributes = defGetString(def);
    else if (strcmp(def->defname, LDAP_FDW_OPTION_BASE_DN) == 0)
      config->base_dn = defGetString(def);
    else if (strcmp(def->defname, LDAP_FDW_OPTION_QUERY) == 0)
      config->query = defGetString(def);

  }
}

static void
_ldap_check_quals(Node *node, TupleDesc tupdesc, char **key, char **value, bool *pushdown)
{
  *key = NULL;
  *value = NULL;
  *pushdown = false;

  if (!node)
    return;

  if (IsA(node, OpExpr))
  {
    OpExpr *op = (OpExpr *) node;
    Node *left, *right;
    Index varattno;

    if (list_length(op->args) != 2)
      return;

    left = list_nth(op->args, 0);

    if (!IsA(left, Var))
      return;

    varattno = ((Var *) left)->varattno;

    right = list_nth(op->args, 1);

    if (IsA(right, Const))
    {
      StringInfoData  buf;

      initStringInfo(&buf);

      *key = NameStr(tupdesc->attrs[varattno - 1]->attname);
      *value = TextDatumGetCString(((Const *) right)->constvalue);

      if (op->opfuncid == PROCID_TEXTEQ && strcmp(*key, "dn") == 0)
        *pushdown = true;

      return;
    }
  }

  return;
}

/*
 *
 * FIXME: Weird by now, should change
 *
 */
static char **
_string_to_array(char * str)
{
  char delims[] = " ,\t\n\r";
  static char *res[MAX_ARGS];
  char *token;
  int j = 0;

  if (str == NULL)
    return NULL;

  token = strtok(str, delims);

  while( token != NULL)
  {
    res[j++] = strdup(token);
    token = strtok(NULL, delims);
  }

  return res;
}
