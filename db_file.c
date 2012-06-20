/******************************************************************************
  Copyright (c) 1995, 1996 Xerox Corporation.  All rights reserved.
  Portions of this code were written by Stephen White, aka ghond.
  Use and copying of this software and preparation of derivative works based
  upon this software are permitted.  Any distribution of this software or
  derivative works must comply with all applicable United States export
  control laws.  This software is made available AS IS, and Xerox Corporation
  makes no warranty about the software, its performance or its conformity to
  any specification.  Any person obtaining a copy of this software is requested
  to send their name and post office or electronic mail address to:
    Pavel Curtis
    Xerox PARC
    3333 Coyote Hill Rd.
    Palo Alto, CA 94304
    Pavel@Xerox.Com
 *****************************************************************************/

/*****************************************************************************
 * Routines for initializing, loading, dumping, and shutting down the database
 *****************************************************************************/

#include "my-stat.h"
#include "my-unistd.h"
#include "my-stdio.h"
#include "my-stdlib.h"

#include "collection.h"
#include "config.h"
#include "db.h"
#include "db_io.h"
#include "db_private.h"
#include "exceptions.h"
#include "list.h"
#include "log.h"
#include "options.h"
#include "server.h"
#include "storage.h"
#include "streams.h"
#include "str_intern.h"
#include "tasks.h"
#include "timers.h"
#include "utils.h"
#include "version.h"

static char *input_db_name, *dump_db_name;
static int dump_generation = 0;
static const char *header_format_string
  = "** LambdaMOO Database, Format Version %u **\n";

DB_Version dbio_input_version;


/*********** Format version 4 support ***********/

/*
 * This structure is compatible with the popular database format
 * version 4.
 */
typedef struct Object4 {
    Objid id;
    Objid owner;
    Objid location;
    Objid contents;
    Objid next;

    Objid parent;
    Objid child;
    Objid sibling;

    const char *name;
    int flags;

    Verbdef *verbdefs;
    Proplist propdefs;
    Pval *propval;
} Object4;

static Object4 **objects;
static int num_objects = 0;
static int max_objects = 0;

static void
ensure_new_object(void)
{
    if (max_objects == 0) {
	max_objects = 100;
	objects = mymalloc(max_objects * sizeof(Object4 *), M_OBJECT_TABLE);
    }
    if (num_objects >= max_objects) {
	int i;
	Object4 **new;

	new = mymalloc(max_objects * 2 * sizeof(Object4 *), M_OBJECT_TABLE);
	for (i = 0; i < max_objects; i++)
	    new[i] = objects[i];
	myfree(objects, M_OBJECT_TABLE);
	objects = new;
	max_objects *= 2;
    }
}

static Object4 *
dbv4_new_object(void)
{
    Object4 *o;

    ensure_new_object();
    o = objects[num_objects] = mymalloc(sizeof(Object4), M_OBJECT);
    o->id = num_objects;
    num_objects++;

    return o;
}

static void
dbv4_new_recycled_object(void)
{
    ensure_new_object();
    objects[num_objects++] = 0;
}

static Object4 *
dbv4_find_object(Objid oid)
{
    if (oid < 0 || oid >= num_objects)
	return 0;
    else
	return objects[oid];
}

static int
dbv4_valid(Objid oid)
{
    return dbv4_find_object(oid) != 0;
}

static Objid
dbv4_last_used_objid(void)
{
    return num_objects - 1;
}

static int
dbv4_count_properties(Objid oid)
{
    Object4 *o;
    int nprops = 0;

    for (o = dbv4_find_object(oid); o; o = dbv4_find_object(o->parent))
	nprops += o->propdefs.cur_length;

    return nprops;
}

typedef struct {
    Objid definer;
    Verbdef *verbdef;
} handle;

static db_verb_handle
dbv4_find_indexed_verb(Objid oid, unsigned index)
{
    Object4 *o = dbv4_find_object(oid);
    Verbdef *v;
    unsigned i;
    static handle h;
    db_verb_handle vh;

    for (v = o->verbdefs, i = 0; v; v = v->next)
	if (++i == index) {
	    h.definer = o->id;
	    h.verbdef = v;
	    vh.ptr = &h;

	    return vh;
	}
    vh.ptr = 0;

    return vh;
}

/*
 * The following functions work with both the version 4 database and
 * the latest database version.  If that changes they will need to be
 * replaced.
 *
 * dbpriv_new_propdef
 * dbpriv_build_prep_table
 * db_verb_definer
 * db_verb_names
 * db_set_verb_program
 * dbpriv_set_all_users
 * db_all_users
 * dbpriv_dbio_failed
 * dbpriv_set_dbio_output
 * dbpriv_set_dbio_input
 */

/*********** Verb and property I/O ***********/

static void
read_verbdef(Verbdef * v)
{
    v->name = dbio_read_string_intern();
    v->owner = dbio_read_objid();
    v->perms = dbio_read_num();
    v->prep = dbio_read_num();
    v->next = 0;
    v->program = 0;
}

static void
write_verbdef(Verbdef * v)
{
    dbio_write_string(v->name);
    dbio_write_objid(v->owner);
    dbio_write_num(v->perms);
    dbio_write_num(v->prep);
}

static Propdef
read_propdef()
{
    const char *name = dbio_read_string_intern();
    return dbpriv_new_propdef(name);
}

static void
write_propdef(Propdef * p)
{
    dbio_write_string(p->name);
}

static void
read_propval(Pval * p)
{
    p->var = dbio_read_var();
    p->owner = dbio_read_objid();
    p->perms = dbio_read_num();
}

static void
write_propval(Pval * p)
{
    dbio_write_var(p->var);
    dbio_write_objid(p->owner);
    dbio_write_num(p->perms);
}


/*********** Object I/O ***********/

static int
v4_read_object(void)
{
    Objid oid;
    Object4 *o;
    char s[20];
    int i;
    Verbdef *v, **prevv;
    int nprops;

    if (dbio_scanf("#%d", &oid) != 1 || oid != dbv4_last_used_objid() + 1)
	return 0;
    dbio_read_line(s, sizeof(s));

    if (strcmp(s, " recycled\n") == 0) {
	dbv4_new_recycled_object();
	return 1;
    } else if (strcmp(s, "\n") != 0)
	return 0;

    o = dbv4_new_object();
    o->name = dbio_read_string_intern();
    (void) dbio_read_string();	/* discard old handles string */
    o->flags = dbio_read_num();

    o->owner = dbio_read_objid();

    o->location = dbio_read_objid();
    o->contents = dbio_read_objid();
    o->next = dbio_read_objid();

    o->parent = dbio_read_objid();
    o->child = dbio_read_objid();
    o->sibling = dbio_read_objid();

    o->verbdefs = 0;
    prevv = &(o->verbdefs);
    for (i = dbio_read_num(); i > 0; i--) {
	v = (Verbdef *) mymalloc(sizeof(Verbdef), M_VERBDEF);
	read_verbdef(v);
	*prevv = v;
	prevv = &(v->next);
    }

    o->propdefs.cur_length = 0;
    o->propdefs.max_length = 0;
    o->propdefs.l = 0;
    if ((i = dbio_read_num()) != 0) {
	o->propdefs.l = (Propdef *) mymalloc(i * sizeof(Propdef), M_PROPDEF);
	o->propdefs.cur_length = i;
	o->propdefs.max_length = i;
	for (i = 0; i < o->propdefs.cur_length; i++)
	    o->propdefs.l[i] = read_propdef();
    }
    nprops = dbio_read_num();
    if (nprops)
	o->propval = (Pval *) mymalloc(nprops * sizeof(Pval), M_PVAL);
    else
	o->propval = 0;

    for (i = 0; i < nprops; i++) {
	read_propval(o->propval + i);
    }

    return 1;
}

static int
ng_read_object(void)
{
    Objid oid;
    Object *o;
    char s[20];
    int i;
    Verbdef *v, **prevv;
    int nprops;

    if (dbio_scanf("#%d", &oid) != 1 || oid != db_last_used_objid() + 1)
	return 0;
    dbio_read_line(s, sizeof(s));

    if (strcmp(s, " recycled\n") == 0) {
	dbpriv_new_recycled_object();
	return 1;
    } else if (strcmp(s, "\n") != 0)
	return 0;

    o = dbpriv_new_object();
    o->name = dbio_read_string_intern();
    o->flags = dbio_read_num();

    o->owner = dbio_read_objid();

    o->location = dbio_read_var();
    o->contents = dbio_read_var();

    o->parents = dbio_read_var();
    o->children = dbio_read_var();

    o->verbdefs = 0;
    prevv = &(o->verbdefs);
    for (i = dbio_read_num(); i > 0; i--) {
	v = mymalloc(sizeof(Verbdef), M_VERBDEF);
	read_verbdef(v);
	*prevv = v;
	prevv = &(v->next);
    }

    o->propdefs.cur_length = 0;
    o->propdefs.max_length = 0;
    o->propdefs.l = 0;
    if ((i = dbio_read_num()) != 0) {
	o->propdefs.l = mymalloc(i * sizeof(Propdef), M_PROPDEF);
	o->propdefs.cur_length = i;
	o->propdefs.max_length = i;
	for (i = 0; i < o->propdefs.cur_length; i++)
	    o->propdefs.l[i] = read_propdef();
    }
    nprops = dbio_read_num();
    if (nprops)
	o->propval = mymalloc(nprops * sizeof(Pval), M_PVAL);
    else
	o->propval = 0;

    for (i = 0; i < nprops; i++) {
	read_propval(o->propval + i);
    }

    return 1;
}

static void
v4_write_object(Objid oid)
{
    Object4 *o;
    Verbdef *v;
    int i;
    int nverbdefs, nprops;

    if (!dbv4_valid(oid)) {
	dbio_printf("#%d recycled\n", oid);
	return;
    }
    o = dbv4_find_object(oid);

    dbio_printf("#%d\n", oid);
    dbio_write_string(o->name);
    dbio_write_string("");	/* placeholder for old handles string */
    dbio_write_num(o->flags);

    dbio_write_objid(o->owner);

    dbio_write_objid(o->location);
    dbio_write_objid(o->contents);
    dbio_write_objid(o->next);

    dbio_write_objid(o->parent);
    dbio_write_objid(o->child);
    dbio_write_objid(o->sibling);

    for (v = o->verbdefs, nverbdefs = 0; v; v = v->next)
	nverbdefs++;

    dbio_write_num(nverbdefs);
    for (v = o->verbdefs; v; v = v->next)
	write_verbdef(v);

    dbio_write_num(o->propdefs.cur_length);
    for (i = 0; i < o->propdefs.cur_length; i++)
	write_propdef(&o->propdefs.l[i]);

    nprops = dbv4_count_properties(oid);

    dbio_write_num(nprops);
    for (i = 0; i < nprops; i++)
	write_propval(o->propval + i);
}

static void
ng_write_object(Objid oid)
{
    Object *o;
    Verbdef *v;
    int i;
    int nverbdefs, nprops;

    if (!valid(oid)) {
	dbio_printf("#%d recycled\n", oid);
	return;
    }
    o = dbpriv_find_object(oid);

    dbio_printf("#%d\n", oid);
    dbio_write_string(o->name);
    dbio_write_num(o->flags);

    dbio_write_objid(o->owner);

    dbio_write_var(o->location);
    dbio_write_var(o->contents);

    dbio_write_var(o->parents);
    dbio_write_var(o->children);

    for (v = o->verbdefs, nverbdefs = 0; v; v = v->next)
	nverbdefs++;

    dbio_write_num(nverbdefs);
    for (v = o->verbdefs; v; v = v->next)
	write_verbdef(v);

    dbio_write_num(o->propdefs.cur_length);
    for (i = 0; i < o->propdefs.cur_length; i++)
	write_propdef(&o->propdefs.l[i]);

    nprops = dbpriv_count_properties(oid);

    dbio_write_num(nprops);
    for (i = 0; i < nprops; i++)
	write_propval(o->propval + i);
}


/*********** File-level Input ***********/

static int
v4_validate_hierarchies()
{
    Objid oid, log_oid;
    Objid size = dbv4_last_used_objid() + 1;
    int broken = 0;
    int fixed_nexts = 0;

    oklog("VALIDATING the object hierarchies ...\n");

#   define PROGRESS_INTERVAL 10000
#   define MAYBE_LOG_PROGRESS					\
    {								\
	if (oid == log_oid) {					\
	    log_oid += PROGRESS_INTERVAL;			\
	    oklog("VALIDATE: Done through #%d ...\n", oid);	\
	}							\
    }

    oklog("VALIDATE: Phase 1: Check for invalid objects ...\n");
    for (oid = 0, log_oid = PROGRESS_INTERVAL; oid < size; oid++) {
	Object4 *o = dbv4_find_object(oid);

	MAYBE_LOG_PROGRESS;
	if (o) {
	    if (o->location == NOTHING && o->next != NOTHING) {
		o->next = NOTHING;
		fixed_nexts++;
	    }
#	    define CHECK(field, name) 					\
	    {								\
	        if (o->field != NOTHING					\
		    && !dbv4_find_object(o->field)) {			\
		    errlog("VALIDATE: #%d.%s = #%d <invalid> ... fixed.\n", \
			   oid, name, o->field);			\
		    o->field = NOTHING;				  	\
		}							\
	    }

	    CHECK(parent, "parent");
	    CHECK(child, "child");
	    CHECK(sibling, "sibling");
	    CHECK(location, "location");
	    CHECK(contents, "contents");
	    CHECK(next, "next");

#	    undef CHECK
	}
    }

    if (fixed_nexts != 0)
	errlog("VALIDATE: Fixed %d should-be-null next pointer(s) ...\n",
	       fixed_nexts);

    oklog("VALIDATE: Phase 2: Check for cycles ...\n");
    for (oid = 0, log_oid = PROGRESS_INTERVAL; oid < size; oid++) {
	Object4 *o = dbv4_find_object(oid);

	MAYBE_LOG_PROGRESS;
	if (o) {
#	    define CHECK(start, field, name)				\
	    {								\
		Objid	oid2 = start;					\
		int	count = 0;					\
		for (; oid2 != NOTHING					\
		     ; oid2 = dbv4_find_object(oid2)->field) {		\
		    if (++count > size)	{				\
			errlog("VALIDATE: Cycle in `%s' chain of #%d\n",\
			       name, oid);				\
			broken = 1;					\
			break;						\
		    }							\
		}							\
	    }

	    CHECK(o->parent, parent, "parent");
	    CHECK(o->child, sibling, "child");
	    CHECK(o->location, location, "location");
	    CHECK(o->contents, next, "contents");

#	    undef CHECK
	}
    }

    if (broken)			/* Can't continue if cycles found */
	return 0;

    oklog("VALIDATE: Phase 3: Check for inconsistencies ...\n");
    for (oid = 0, log_oid = PROGRESS_INTERVAL; oid < size; oid++) {
	Object4 *o = dbv4_find_object(oid);

	MAYBE_LOG_PROGRESS;
	if (o) {
#	    define CHECK(up, up_name, down, down_name, across)		\
	    {								\
		Objid	up = o->up;					\
		Objid	oid2;						\
									\
		/* Is oid in its up's down list? */			\
		if (up != NOTHING) {					\
		    for (oid2 = dbv4_find_object(up)->down;		\
			 oid2 != NOTHING;				\
			 oid2 = dbv4_find_object(oid2)->across) {	\
			if (oid2 == oid) /* found it */			\
			    break;					\
		    }							\
		    if (oid2 == NOTHING) { /* didn't find it */		\
			errlog("VALIDATE: #%d not in %s (#%d)'s %s list.\n", \
			       oid, up_name, up, down_name);	        \
			broken = 1;					\
		    }							\
		}							\
	    }

	    CHECK(parent, "parent", child, "child", sibling);
	    CHECK(location, "location", contents, "contents", next);

#	    undef CHECK

#	    define CHECK(up, down, down_name, across)			\
	    {								\
		Objid	oid2;						\
									\
		for (oid2 = o->down;					\
		     oid2 != NOTHING;					\
		     oid2 = dbv4_find_object(oid2)->across) {		\
		    if (dbv4_find_object(oid2)->up != oid) {		\
			errlog(						\
			    "VALIDATE: #%d erroneously on #%d's %s list.\n", \
			    oid2, oid, down_name);			\
			broken = 1;					\
		    }							\
		}							\
	    }

	    CHECK(parent, child, "child", sibling);
	    CHECK(location, contents, "contents", next);

#	    undef CHECK
	}
    }

#   undef PROGRESS_INTERVAL
#   undef MAYBE_LOG_PROGRESS

    oklog("VALIDATING the object hierarchies ... finished.\n");
    return !broken;
}

static int
ng_validate_hierarchies()
{
    Objid oid, log_oid;
    Objid size = db_last_used_objid() + 1;
    int i, c;
    int broken = 0;

    oklog("VALIDATING the object hierarchies ...\n");

#   define PROGRESS_INTERVAL 10000
#   define MAYBE_LOG_PROGRESS					\
    {								\
        if (oid == log_oid) {					\
	    log_oid += PROGRESS_INTERVAL;			\
	    oklog("VALIDATE: Done through #%d ...\n", oid);	\
	}							\
    }

    oklog("VALIDATE: Phase 1: Check for invalid objects ...\n");
    for (oid = 0, log_oid = PROGRESS_INTERVAL; oid < size; oid++) {
	Object *o = dbpriv_find_object(oid);
	MAYBE_LOG_PROGRESS;
	if (o) {
	    if (!is_obj_or_list_of_objs(o->parents)) {
		errlog("VALIDATE: #%d.parents is not an object or list of objects.\n",
		       oid);
		broken = 1;
	    }
	    if (!is_list_of_objs(o->children)) {
		errlog("VALIDATE: #%d.children is not a list of objects.\n",
		       oid);
		broken = 1;
	    }
	    if (!is_obj(o->location)) {
		errlog("VALIDATE: #%d.location is not an object.\n",
		       oid);
		broken = 1;
	    }
	    if (!is_list_of_objs(o->contents)) {
		errlog("VALIDATE: #%d.contents is not a list of objects.\n",
		       oid);
		broken = 1;
	    }
#	    define CHECK(field, name)					\
	    {								\
		if (TYPE_LIST == o->field.type) {			\
		    Var tmp;						\
		    FOR_EACH(tmp, o->field, i, c) {			\
			if (tmp.v.obj != NOTHING			\
			    && !dbpriv_find_object(tmp.v.obj)) {	\
			    errlog("VALIDATE: #%d.%s = #%d <invalid> ... removed.\n", \
			           oid, name, tmp);			\
			    o->field = setremove(o->field, tmp);	\
			}						\
		    }							\
		}							\
		else {							\
		    if (o->field.v.obj != NOTHING			\
		        && !dbpriv_find_object(o->field.v.obj)) {	\
			errlog("VALIDATE: #%d.%s = #%d <invalid> ... fixed.\n", \
			       oid, name, o->field.v.obj);		\
			o->field.v.obj = NOTHING;			\
		    }							\
		}							\
	    }

	    if (!broken) {
		CHECK(parents, "parent");
		CHECK(children, "child");
		CHECK(location, "location");
		CHECK(contents, "content");
	    }

#	    undef CHECK
	}
    }

    if (broken)		/* Can't continue if invalid objects found */
	return 0;

    oklog("VALIDATE: Phase 2: Check for cycles ...\n");
    for (oid = 0, log_oid = PROGRESS_INTERVAL; oid < size; oid++) {
	Object *o = dbpriv_find_object(oid);
	MAYBE_LOG_PROGRESS;
	if (o) {
#           define CHECK(start, func, name)				\
	    {								\
		Var tmp;						\
		tmp.type = TYPE_OBJ;					\
		tmp.v.obj = start;					\
		Var all = func(start, false);				\
		if (ismember(tmp, all, 1)) {				\
			errlog("VALIDATE: Cycle in %s chain of #%d.\n",	\
			       name, oid);				\
			broken = 1;					\
		}							\
		free_var(all);						\
	    }

	    CHECK(oid, db_ancestors, "parent");
	    CHECK(oid, db_all_locations, "location");

#	    undef CHECK
	}
    }

    if (broken)		/* Can't continue if cycles found */
	return 0;

    oklog("VALIDATE: Phase 3: Check for inconsistencies ...\n");
    for (oid = 0, log_oid = PROGRESS_INTERVAL; oid < size; oid++) {
	Object *o = dbpriv_find_object(oid);
	MAYBE_LOG_PROGRESS;
	if (o) {
#	    define CHECK(up, up_name, down, down_name)			\
	    {								\
		Var tmp, t1, t2, obj;					\
		obj.type = TYPE_OBJ;					\
		obj.v.obj = oid;					\
		t1 = enlist_var(var_ref(o->up));			\
		FOR_EACH(tmp, t1, i, c) {				\
		    if (tmp.v.obj != NOTHING) {				\
			Object *otmp = dbpriv_find_object(tmp.v.obj);	\
			t2 = enlist_var(var_ref(otmp->down));		\
			if (ismember(obj, t2, 1)) {			\
			    free_var(t2);				\
			    continue;					\
			}						\
			else {						\
			    errlog("VALIDATE: #%d not in it's %s's (#%d) %s.\n", \
			           oid, up_name, otmp->id, down_name); \
			    free_var(t2);				\
			    broken = 1;					\
			    break;					\
			}						\
		    }							\
		}							\
		free_var(t1);						\
	    }

	    CHECK(location, "location", contents, "contents");
	    CHECK(contents, "content", location, "location");
	    CHECK(parents, "parent", children, "children");
	    CHECK(children, "child", parents, "parents");

#	    undef CHECK
	}
    }

#   undef PROGRESS_INTERVAL
#   undef MAYBE_LOG_PROGRESS

    oklog("VALIDATING the object hierarchies ... finished.\n");
    return !broken;
}

static int
v4_upgrade_objects()
{
    Objid oid, log_oid;
    Objid size = dbv4_last_used_objid() + 1;

    oklog("UPGRADING objects to new structure ...\n");

#   define PROGRESS_INTERVAL 10000
#   define MAYBE_LOG_PROGRESS					\
    {								\
        if (oid == log_oid) {					\
	    log_oid += PROGRESS_INTERVAL;			\
	    oklog("UPGRADE: Done through #%d ...\n", oid);	\
	}							\
    }

    for (oid = 0, log_oid = PROGRESS_INTERVAL; oid < size; oid++) {
	Object4 *o = dbv4_find_object(oid);

	MAYBE_LOG_PROGRESS;
	if (o) {
	    Object *new = dbpriv_new_object();
	    new->name = o->name;
	    new->flags = o->flags;

	    new->owner = o->owner;

	    Objid iter;

	    new->parents = var_dup(new_obj(o->parent));

	    new->children = new_list(0);
	    for (iter = o->child; iter != NOTHING; iter = objects[iter]->sibling)
		new->children = listappend(new->children, var_dup(new_obj(iter)));

	    new->location = var_dup(new_obj(o->location));

	    new->contents = new_list(0);
	    for (iter = o->contents; iter != NOTHING; iter = objects[iter]->next)
		new->contents = listappend(new->contents, var_dup(new_obj(iter)));

	    new->verbdefs = o->verbdefs;
	    new->propdefs = o->propdefs;
	    new->propval = o->propval;
	}
	else {
	    dbpriv_new_recycled_object();
	}
    }

#   undef PROGRESS_INTERVAL
#   undef MAYBE_LOG_PROGRESS

    for (oid = 0; oid < size; oid++) {
	if (objects[oid]) {
	    myfree(objects[oid], M_OBJECT);
	    objects[oid] = 0;
	}
    }

    num_objects = 0;
    max_objects = 0;
    myfree(objects, M_OBJECT_TABLE);

    oklog("UPGRADING objects to new structure ... finished.\n");
    return 1;
}

static const char *
fmt_verb_name(void *data)
{
  db_verb_handle *h = (db_verb_handle *) data;
    static Stream *s = 0;

    if (!s)
	s = new_stream(40);

    stream_printf(s, "#%d:%s", db_verb_definer(*h), db_verb_names(*h));
    return reset_stream(s);
}

static int
read_db_file(void)
{
    Objid oid;
    int nobjs, nprogs, nusers;
    Var user_list;
    int i, vnum, dummy;
    db_verb_handle h;
    Program *program;

    if (dbio_scanf(header_format_string, &dbio_input_version) != 1)
	dbio_input_version = DBV_Prehistory;

    if (!check_db_version(dbio_input_version)) {
	errlog("READ_DB_FILE: Unknown DB version number: %d\n",
	       dbio_input_version);
	return 0;
    }

    /* I use a `dummy' variable here and elsewhere instead of the `*'
     * assignment-suppression syntax of `scanf' because it allows more
     * straightforward error checking; unfortunately, the standard
     * says that suppressed assignments are not counted in determining
     * the returned value of `scanf'...
     */
    if (dbio_scanf("%d\n%d\n%d\n%d\n",
		   &nobjs, &nprogs, &dummy, &nusers) != 4) {
	errlog("READ_DB_FILE: Bad header\n");
	return 0;
    }

    user_list = new_list(nusers);
    for (i = 1; i <= nusers; i++) {
	user_list.v.list[i].type = TYPE_OBJ;
	user_list.v.list[i].v.obj = dbio_read_objid();
    }
    dbpriv_set_all_users(user_list);

    oklog("LOADING: Reading %d objects ...\n", nobjs);
    for (i = 1; i <= nobjs; i++) {
	if (DBV_NextGen > dbio_input_version) {
	    if (!v4_read_object()) {
		errlog("READ_DB_FILE: Bad object #%d.\n", i - 1);
		return 0;
	    }
	}
	else {
	    if (!ng_read_object()) {
		errlog("READ_DB_FILE: Bad object #%d.\n", i - 1);
		return 0;
	    }
	}
	if (i % 10000 == 0 || i == nobjs)
	    oklog("LOADING: Done reading %d objects ...\n", i);
    }

    if (DBV_NextGen > dbio_input_version) {
	if (!v4_validate_hierarchies()) {
	    errlog("READ_DB_FILE: Errors in object hierarchies.\n");
	    return 0;
	}
    }
    else {
	if (!ng_validate_hierarchies()) {
	    errlog("READ_DB_FILE: Errors in object hierarchies.\n");
	    return 0;
	}
    }

    oklog("LOADING: Reading %d MOO verb programs ...\n", nprogs);
    for (i = 1; i <= nprogs; i++) {
	if (dbio_scanf("#%d:%d\n", &oid, &vnum) != 2) {
	    errlog("READ_DB_FILE: Bad program header, i = %d.\n", i);
	    return 0;
	}
	if (DBV_NextGen > dbio_input_version) {
	    if (!dbv4_valid(oid)) {
		errlog("READ_DB_FILE: Verb for non-existant object: #%d:%d.\n",
		       oid, vnum);
		return 0;
	    }
	    h = dbv4_find_indexed_verb(oid, vnum + 1);	/* DB file is 0-based. */
	    if (!h.ptr) {
		errlog("READ_DB_FILE: Unknown verb index: #%d:%d.\n", oid, vnum);
		return 0;
	    }
	}
	else {
	    if (!valid(oid)) {
		errlog("READ_DB_FILE: Verb for non-existant object: #%d:%d.\n",
		       oid, vnum);
		return 0;
	    }
	    h = db_find_indexed_verb(oid, vnum + 1);	/* DB file is 0-based. */
	    if (!h.ptr) {
		errlog("READ_DB_FILE: Unknown verb index: #%d:%d.\n", oid, vnum);
		return 0;
	    }
	}
	program = dbio_read_program(dbio_input_version, fmt_verb_name, &h);
	if (!program) {
	    errlog("READ_DB_FILE: Unparsable program #%d:%d.\n", oid, vnum);
	    return 0;
	}
	db_set_verb_program(h, program);
	if (i % 5000 == 0 || i == nprogs)
	    oklog("LOADING: Done reading %d verb programs ...\n", i);
    }

    oklog("LOADING: Reading forked and suspended tasks ...\n");
    if (!read_task_queue()) {
	errlog("READ_DB_FILE: Can't read task queue.\n");
	return 0;
    }

    oklog("LOADING: Reading list of formerly active connections ...\n");
    if (!read_active_connections()) {
	errlog("DB_READ: Can't read active connections.\n");
	return 0;
    }

    if (DBV_NextGen > dbio_input_version) {
	if (!v4_upgrade_objects()) {
	    errlog("READ_DB_FILE: Errors upgrading objects.\n");
	    return 0;
	}
    }

    return 1;
}


/*********** File-level Output ***********/

static int
write_db_file(const char *reason)
{
    Objid oid;
    Objid max_oid = db_last_used_objid();
    Verbdef *v;
    Var user_list;
    int i;
    volatile int nprogs = 0;
    volatile int success = 1;

    for (oid = 0; oid <= max_oid; oid++) {
	if (valid(oid))
	    for (v = dbpriv_find_object(oid)->verbdefs; v; v = v->next)
		if (v->program)
		    nprogs++;
    }

    user_list = db_all_users();

    TRY {
	dbio_printf(header_format_string, current_db_version);

	dbio_printf("%d\n%d\n%d\n%d\n",
	            max_oid + 1, nprogs, 0, user_list.v.list[0].v.num);

	for (i = 1; i <= user_list.v.list[0].v.num; i++)
	    dbio_write_objid(user_list.v.list[i].v.obj);

	oklog("%s: Writing %d objects ...\n", reason, max_oid + 1);
	for (oid = 0; oid <= max_oid; oid++) {
	    ng_write_object(oid);
	    if ((oid + 1) % 10000 == 0 || oid == max_oid)
		oklog("%s: Done writing %d objects ...\n", reason, oid + 1);
	}

	oklog("%s: Writing %d MOO verb programs ...\n", reason, nprogs);
	for (i = 0, oid = 0; oid <= max_oid; oid++) {
	    if (valid(oid)) {
		int vcount = 0;
		for (v = dbpriv_find_object(oid)->verbdefs; v; v = v->next) {
		    if (v->program) {
			dbio_printf("#%d:%d\n", oid, vcount);
			dbio_write_program(v->program);
			if (++i % 5000 == 0 || i == nprogs)
			    oklog("%s: Done writing %d verb programs ...\n",
			          reason, i);
		    }
		    vcount++;
		}
	    }
	}

	oklog("%s: Writing forked and suspended tasks ...\n", reason);
	write_task_queue();

	oklog("%s: Writing list of formerly active connections ...\n", reason);
	write_active_connections();
    }
    EXCEPT(dbpriv_dbio_failed) {
	success = 0;
    } ENDTRY;

    return success;
}

typedef enum {
    DUMP_SHUTDOWN, DUMP_CHECKPOINT, DUMP_PANIC
} Dump_Reason;
const char *reason_names[] =
{"DUMPING", "CHECKPOINTING", "PANIC-DUMPING"};

static int
dump_database(Dump_Reason reason)
{
    Stream *s = new_stream(100);
    char *temp_name;
    FILE *f;
    int success;

  retryDumping:

    stream_printf(s, "%s.#%d#", dump_db_name, dump_generation);
    remove(reset_stream(s));	/* Remove previous checkpoint */

    if (reason == DUMP_PANIC)
	stream_printf(s, "%s.PANIC", dump_db_name);
    else {
	dump_generation++;
	stream_printf(s, "%s.#%d#", dump_db_name, dump_generation);
    }
    temp_name = reset_stream(s);

    oklog("%s on %s ...\n", reason_names[reason], temp_name);

#ifdef UNFORKED_CHECKPOINTS
    reset_command_history();
#else
    if (reason == DUMP_CHECKPOINT) {
	switch (fork_server("checkpointer")) {
	case FORK_PARENT:
	    reset_command_history();
	    free_stream(s);
	    return 1;
	case FORK_ERROR:
	    free_stream(s);
	    return 0;
	case FORK_CHILD:
	    set_server_cmdline("(MOO checkpointer)");
	    break;
	}
    }
#endif

    success = 1;
    if ((f = fopen(temp_name, "w")) != 0) {
	dbpriv_set_dbio_output(f);
	if (!write_db_file(reason_names[reason])) {
	    log_perror("Trying to dump database");
	    fclose(f);
	    remove(temp_name);
	    if (reason == DUMP_CHECKPOINT) {
		errlog("Abandoning checkpoint attempt ...\n");
		success = 0;
	    } else {
		int retry_interval = 60;

		errlog("Waiting %d seconds and retrying dump ...\n",
		       retry_interval);
		timer_sleep(retry_interval);
		goto retryDumping;
	    }
	} else {
	    fflush(f);
	    fsync(fileno(f));
	    fclose(f);
	    oklog("%s on %s finished\n", reason_names[reason], temp_name);
	    if (reason != DUMP_PANIC) {
		remove(dump_db_name);
		if (rename(temp_name, dump_db_name) != 0) {
		    log_perror("Renaming temporary dump file");
		    success = 0;
		}
	    }
	}
    } else {
	log_perror("Opening temporary dump file");
	success = 0;
    }

    free_stream(s);

#ifndef UNFORKED_CHECKPOINTS
    if (reason == DUMP_CHECKPOINT)
	/* We're a child, so we'd better go away. */
	exit(!success);
#endif

    return success;
}


/*********** External interface ***********/

const char *
db_usage_string(void)
{
    return "input-db-file output-db-file";
}

static FILE *input_db;

int
db_initialize(int *pargc, char ***pargv)
{
    FILE *f;

    if (*pargc < 2)
	return 0;

    input_db_name = str_dup((*pargv)[0]);
    dump_db_name = str_dup((*pargv)[1]);
    *pargc -= 2;
    *pargv += 2;

    if (!(f = fopen(input_db_name, "r"))) {
	fprintf(stderr, "Cannot open input database file: %s\n",
		input_db_name);
	return 0;
    }
    input_db = f;
    dbpriv_build_prep_table();

    return 1;
}

int
db_load(void)
{
    dbpriv_set_dbio_input(input_db);

    str_intern_open(0);

    oklog("LOADING: %s\n", input_db_name);
    if (!read_db_file()) {
	errlog("DB_LOAD: Cannot load database!\n");
	return 0;
    }
    oklog("LOADING: %s done, will dump new database on %s\n",
	  input_db_name, dump_db_name);

    str_intern_close();

    fclose(input_db);
    return 1;
}

int
db_flush(enum db_flush_type type)
{
    int success = 0;

    switch (type) {
    case FLUSH_IF_FULL:
    case FLUSH_ONE_SECOND:
	success = 1;
	break;

    case FLUSH_ALL_NOW:
	success = dump_database(DUMP_CHECKPOINT);
	break;

    case FLUSH_PANIC:
	success = dump_database(DUMP_PANIC);
	break;
    }

    return success;
}

int32
db_disk_size(void)
{
    struct stat st;

    if ((dump_generation == 0 || stat(dump_db_name, &st) < 0)
	&& stat(input_db_name, &st) < 0)
	return -1;
    else
	return st.st_size;
}

void
db_shutdown()
{
    dump_database(DUMP_SHUTDOWN);

    free_str(input_db_name);
    free_str(dump_db_name);
}

char rcsid_db_file[] = "$Id: db_file.c,v 1.7 2010/04/22 21:29:18 wrog Exp $";

/*
 * $Log: db_file.c,v $
 * Revision 1.7  2010/04/22 21:29:18  wrog
 * Free database name strings on shutdown (rob@mars.org)
 * current_version -> current_db_version
 *
 * Revision 1.6  2007/11/12 11:17:03  wrog
 * sync so that checkpoint is physically written before prior checkpoint is unlinked
 *
 * Revision 1.5  2004/05/22 01:25:43  wrog
 * merging in WROGUE changes (W_SRCIP, W_STARTUP, W_OOB)
 *
 * Revision 1.4.8.1  2003/06/01 12:27:35  wrog
 * added braces and fixed indentation on TRY
 *
 * Revision 1.4  1998/12/14 13:17:33  nop
 * Merge UNSAFE_OPTS (ref fixups); fix Log tag placement to fit CVS whims
 *
 * Revision 1.3  1998/02/19 07:36:16  nop
 * Initial string interning during db load.
 *
 * Revision 1.2  1997/03/03 04:18:27  nop
 * GNU Indent normalization
 *
 * Revision 1.1.1.1  1997/03/03 03:44:59  nop
 * LambdaMOO 1.8.0p5
 *
 * Revision 2.5  1996/04/08  01:07:21  pavel
 * Changed a boot-time error message to go directly to stderr, instead of
 * through the logging package.  Release 1.8.0p3.
 *
 * Revision 2.4  1996/02/08  07:20:18  pavel
 * Renamed err/logf() to errlog/oklog().  Updated copyright notice for 1996.
 * Release 1.8.0beta1.
 *
 * Revision 2.3  1995/12/31  03:27:54  pavel
 * Added missing #include "options.h".  Release 1.8.0alpha4.
 *
 * Revision 2.2  1995/12/28  00:51:39  pavel
 * Added db_disk_size().  Added support for printing location of
 * MOO-compilation warnings and errors during loading.  More slight
 * improvements to load-time progress messages.  Added dump-time progress
 * messages.  Added init-time call to build preposition table.
 * Release 1.8.0alpha3.
 *
 * Revision 2.1  1995/12/11  07:55:01  pavel
 * Added missing #include of "my-stdlib.h".  Slightly improved clarity of the
 * progress messages during DB loading.
 *
 * Release 1.8.0alpha2.
 *
 * Revision 2.0  1995/11/30  04:19:37  pavel
 * New baseline version, corresponding to release 1.8.0alpha1.
 *
 * Revision 1.1  1995/11/30  04:19:11  pavel
 * Initial revision
 */
