/* caldav_alarm.c -- implementation of global CalDAV alarm database
 *
 * Copyright (c) 1994-2012 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <config.h>

#include <syslog.h>
#include <string.h>

#include <libical/ical.h>

#include "caldav_alarm.h"
#include "cyrusdb.h"
#include "exitcodes.h"
#include "httpd.h"
#include "http_dav.h"
#include "libconfig.h"
#include "mboxevent.h"
#include "mboxname.h"
#include "util.h"
#include "xstrlcat.h"
#include "xmalloc.h"

/* generated headers are not necessarily in current directory */
#include "imap/http_err.h"
#include "imap/imap_err.h"

static struct namespace caldav_alarm_namespace;
icaltimezone *utc_zone = NULL;

EXPORTED int caldav_alarm_init(void)
{
    int r;

    utc_zone = icaltimezone_get_utc_timezone();

    /* Set namespace -- force standard (internal) */
    if ((r = mboxname_init_namespace(&caldav_alarm_namespace, 1))) {
        syslog(LOG_ERR, "%s", error_message(r));
        fatal(error_message(r), EC_CONFIG);
    }

    return sqldb_init();
}


EXPORTED int caldav_alarm_done(void)
{
    return sqldb_done();
}


#define CMD_CREATE                                      \
    "CREATE TABLE IF NOT EXISTS alarms ("               \
    " rowid INTEGER PRIMARY KEY AUTOINCREMENT,"         \
    " mailbox TEXT NOT NULL,"                           \
    " resource TEXT NOT NULL,"                          \
    " action INTEGER NOT NULL,"                         \
    " nextalarm TEXT NOT NULL,"                         \
    " tzid TEXT NOT NULL,"                              \
    " start TEXT NOT NULL,"                             \
    " end TEXT NOT NULL"                                \
    ");"                                                \
    "CREATE TABLE IF NOT EXISTS alarm_recipients ("     \
    " rowid INTEGER PRIMARY KEY AUTOINCREMENT,"         \
    " alarmid INTEGER NOT NULL,"                        \
    " email TEXT NOT NULL,"                             \
    " FOREIGN KEY (alarmid) REFERENCES alarms (rowid) ON DELETE CASCADE" \
    ");"                                                \
    "CREATE INDEX IF NOT EXISTS idx_alarm_id ON alarm_recipients ( alarmid );"

#define DBVERSION 1

struct sqldb_upgrade upgrade[] = {
    /* XXX - insert upgrade rows here, i.e.
     * { 2, CMD_UPGRADEv2, &upgrade_v2_callback },
     */
    /* always finish with an empty row */
    { 0, NULL, NULL }
};

static sqldb_t *my_alarmdb;
int refcount;
static struct mboxlock *my_alarmdb_lock;

/* get a database handle to the alarm db */
EXPORTED sqldb_t *caldav_alarm_open()
{
    /* already running?  Bonus */
    if (refcount) {
        refcount++;
        return my_alarmdb;
    }

    /* we need exclusivity! */
    int r = mboxname_lock("$CALDAVALARMDB", &my_alarmdb_lock, LOCK_EXCLUSIVE);
    if (r) {
        syslog(LOG_ERR, "DBERROR: failed to lock $CALDAVALARMDB");
        return NULL;
    }

    char *dbfilename = strconcat(config_dir, "/caldav_alarm.sqlite3", NULL);
    my_alarmdb = sqldb_open(dbfilename, CMD_CREATE, DBVERSION, upgrade);

    if (!my_alarmdb) {
        syslog(LOG_ERR, "DBERROR: failed to open %s", dbfilename);
        mboxname_release(&my_alarmdb_lock);
    }

    free(dbfilename);
    refcount = 1;
    return my_alarmdb;

}

/* close this handle */
EXPORTED int caldav_alarm_close(sqldb_t *alarmdb)
{
    assert(my_alarmdb == alarmdb);

    if (--refcount) return 0;

    sqldb_close(&my_alarmdb);
    mboxname_release(&my_alarmdb_lock);

    return 0;
}

#define CMD_INSERT_ALARM                                                        \
    "INSERT INTO alarms"                                                        \
    " ( mailbox, resource, action, nextalarm, tzid, start, end )"               \
    " VALUES"                                                                   \
    " ( :mailbox, :resource, :action, :nextalarm, :tzid, :start, :end )"        \
    ";"

#define CMD_INSERT_RECIPIENT            \
    "INSERT INTO alarm_recipients"      \
    " ( alarmid, email )"               \
    " VALUES"                           \
    " ( :alarmid, :email )"             \
    ";"

/* add a calendar alarm */
EXPORTED int caldav_alarm_add(sqldb_t *alarmdb, struct caldav_alarm_data *alarmdata)
{
    assert(alarmdb);
    assert(alarmdata);

    struct sqldb_bindval bval[] = {
        { ":mailbox",   SQLITE_TEXT,    { .s = alarmdata->mailbox                               } },
        { ":resource",  SQLITE_TEXT,    { .s = alarmdata->resource                              } },
        { ":action",    SQLITE_INTEGER, { .i = alarmdata->action                                } },
        { ":nextalarm", SQLITE_TEXT,    { .s = icaltime_as_ical_string(alarmdata->nextalarm)    } },
        { ":tzid",      SQLITE_TEXT,    { .s = alarmdata->tzid                                  } },
        { ":start",     SQLITE_TEXT,    { .s = icaltime_as_ical_string(alarmdata->start)        } },
        { ":end",       SQLITE_TEXT,    { .s = icaltime_as_ical_string(alarmdata->end)          } },
        { NULL,         SQLITE_NULL,    { .s = NULL                                             } }
    };

    int rc = 0;

    rc = sqldb_begin(alarmdb, "addalarm");
    if (rc) return rc;

    /* XXX deal with SQLITE_FULL */
    rc = sqldb_exec(alarmdb, CMD_INSERT_ALARM, bval, NULL, NULL);
    if (rc) {
        sqldb_rollback(alarmdb, "addalarm");
        return rc;
    }

    alarmdata->rowid = sqldb_lastid(alarmdb);

    int i;
    for (i = 0; i < strarray_size(&alarmdata->recipients); i++) {
        const char *email = strarray_nth(&alarmdata->recipients, i);

        struct sqldb_bindval rbval[] = {
            { ":alarmid",   SQLITE_INTEGER, { .i = alarmdata->rowid     } },
            { ":email",     SQLITE_TEXT,    { .s = email                } },
            { NULL,         SQLITE_NULL,    { .s = NULL                 } }
        };

        rc = sqldb_exec(alarmdb, CMD_INSERT_RECIPIENT, rbval, NULL, NULL);
        if (rc) {
            sqldb_rollback(alarmdb, "addalarm");
            return rc;
        }
    }

    return sqldb_commit(alarmdb, "addalarm");
}

#define CMD_DELETE              \
    "DELETE FROM alarms WHERE"  \
    " rowid = :rowid"           \
    ";"

/* delete a single alarm */
static int caldav_alarm_delete_row(sqldb_t *alarmdb, struct caldav_alarm_data *alarmdata)
{
    assert(alarmdb);
    assert(alarmdata);

    struct sqldb_bindval bval[] = {
        { ":rowid", SQLITE_INTEGER, { .i = alarmdata->rowid } },
        { NULL,     SQLITE_NULL,    { .s = NULL             } }
    };

    return sqldb_exec(alarmdb, CMD_DELETE, bval, NULL, NULL);
}

#define CMD_DELETEALL           \
    "DELETE FROM alarms WHERE"  \
    " mailbox = :mailbox AND"   \
    " resource = :resource"     \
    ";"

/* delete all alarms matching the event */
EXPORTED int caldav_alarm_delete_all(sqldb_t *alarmdb, struct caldav_alarm_data *alarmdata)
{
    assert(alarmdb);
    assert(alarmdata);

    struct sqldb_bindval bval[] = {
        { ":mailbox",   SQLITE_TEXT, { .s = alarmdata->mailbox  } },
        { ":resource",  SQLITE_TEXT, { .s = alarmdata->resource } },
        { NULL,         SQLITE_NULL, { .s = NULL                } }
    };

    return sqldb_exec(alarmdb, CMD_DELETEALL, bval, NULL, NULL);
}

#define CMD_DELETEMAILBOX               \
    "DELETE FROM alarms WHERE"  \
    " mailbox = :mailbox"       \
    ";"

/* delete all alarms matching the event */
EXPORTED int caldav_alarm_delmbox(sqldb_t *alarmdb, const char *mboxname)
{
    assert(alarmdb);

    struct sqldb_bindval bval[] = {
        { ":mailbox",   SQLITE_TEXT, { .s = mboxname  } },
        { NULL,         SQLITE_NULL, { .s = NULL        } }
    };

    return sqldb_exec(alarmdb, CMD_DELETEMAILBOX, bval, NULL, NULL);
}

#define CMD_DELETEUSER          \
    "DELETE FROM alarms WHERE"  \
    " mailbox LIKE :prefix"     \
    ";"

/* delete all alarms matching the event */
EXPORTED int caldav_alarm_delete_user(sqldb_t *alarmdb, const char *userid)
{
    assert(alarmdb);
    const char *mboxname = NULL;
    char *prefix;
    mbname_t *mbname = NULL;

    mbname = mbname_from_userid(userid);

    mboxname = mbname_intname(mbname);

    size_t len = strlen(mboxname);
    if (len + 3 > MAX_MAILBOX_NAME) return IMAP_INTERNAL;
    prefix = strconcat(mboxname, ".%", (char *)NULL);

    struct sqldb_bindval bval[] = {
        { ":prefix",    SQLITE_TEXT, { .s = prefix  } },
        { NULL,         SQLITE_NULL, { .s = NULL    } }
    };

    int r = sqldb_exec(alarmdb, CMD_DELETEUSER, bval, NULL, NULL);

    free(prefix);
    mbname_free(&mbname);

    return r;
}

enum trigger_type {
    TRIGGER_RELATIVE_START,
    TRIGGER_RELATIVE_END,
    TRIGGER_ABSOLUTE
};
struct trigger_data {
    icalcomponent               *alarm;
    struct icaltriggertype      trigger;
    enum trigger_type           type;
    enum caldav_alarm_action    action;
};

struct recur_cb_data {
    struct trigger_data         *triggerdata;
    int                         ntriggers;
    struct icaltimetype         now;
    icalcomponent               *nextalarm;
    enum caldav_alarm_action    nextaction;
    struct icaltimetype         nextalarmtime;
    struct icaltimetype         eventstart;
    struct icaltimetype         eventend;
};

/* icalcomponent_foreach_recurrence() callback to find closest future event */
static void recur_cb(icalcomponent *comp, struct icaltime_span *span, void *rock)
{
    struct recur_cb_data *rdata = (struct recur_cb_data *) rock;
    int is_date = icaltime_is_date(icalcomponent_get_dtstart(comp));

    /* is_date true ensures that the time is set to 00:00:00, but
     * we still want to use it as a full datetime later, so we
     * clear the flag once we're done with it */
    struct icaltimetype start =
        icaltime_from_timet_with_zone(span->start, is_date, utc_zone);
    struct icaltimetype end =
        icaltime_from_timet_with_zone(span->end, is_date, utc_zone);
    start.is_date = end.is_date = 0;

    int i;
    for (i = 0; i < rdata->ntriggers; i++) {
        struct trigger_data *tdata = &(rdata->triggerdata[i]);

        struct icaltimetype alarmtime;
        switch (tdata->type) {
            case TRIGGER_RELATIVE_START:
                alarmtime = icaltime_add(start, tdata->trigger.duration);
                break;
            case TRIGGER_RELATIVE_END:
                alarmtime = icaltime_add(end, tdata->trigger.duration);
                break;
            case TRIGGER_ABSOLUTE:
                alarmtime = tdata->trigger.time;
                break;
            default:
                /* doesn't happen */
                continue;
        }

        if (icaltime_compare(alarmtime, rdata->now) > 0 &&
            (!rdata->nextalarm ||
             icaltime_compare(alarmtime, rdata->nextalarmtime) < 0)) {

            rdata->nextalarm = tdata->alarm;

            rdata->nextaction = tdata->action;

            memcpy(&(rdata->nextalarmtime), &alarmtime, sizeof(struct icaltimetype));

            memcpy(&(rdata->eventstart), &start, sizeof(struct icaltimetype));
            memcpy(&(rdata->eventend), &end, sizeof(struct icaltimetype));
        }
    }
}

/* fill alarmdata with data for next alarm for given entry */
EXPORTED int caldav_alarm_prepare(
        icalcomponent *ical, struct caldav_alarm_data *alarmdata,
        enum caldav_alarm_action wantaction, icaltimetype after)
{
    assert(ical);
    assert(alarmdata);

    icalcomponent *comp = icalcomponent_get_first_real_component(ical);

    /* if there's no VALARM on this item then we have nothing to do */
    int nalarms = icalcomponent_count_components(comp, ICAL_VALARM_COMPONENT);
    if (nalarms == 0)
        return 1;

    int ntriggers = 0;
    struct trigger_data *triggerdata =
        (struct trigger_data *) xmalloc(sizeof(struct trigger_data) * nalarms);

    icalcomponent *alarm;
    for ( alarm = icalcomponent_get_first_component(comp, ICAL_VALARM_COMPONENT);
          alarm;
          alarm = icalcomponent_get_next_component(comp, ICAL_VALARM_COMPONENT)) {

        icalproperty *prop;
        icalvalue *val;

        prop = icalcomponent_get_first_property(alarm, ICAL_ACTION_PROPERTY);
        if (!prop)
            /* no action, invalid alarm, skip */
            continue;

        val = icalproperty_get_value(prop);
        enum icalproperty_action action = icalvalue_get_action(val);
        if (!(action == ICAL_ACTION_DISPLAY || action == ICAL_ACTION_EMAIL))
            /* we only want DISPLAY and EMAIL, skip */
            continue;

        if (
            (wantaction == CALDAV_ALARM_ACTION_DISPLAY && action != ICAL_ACTION_DISPLAY) ||
            (wantaction == CALDAV_ALARM_ACTION_EMAIL   && action != ICAL_ACTION_EMAIL)
        )
            /* specific action was requested and this doesn't match, skip */
            continue;

        prop = icalcomponent_get_first_property(alarm, ICAL_TRIGGER_PROPERTY);
        if (!prop)
            /* no trigger, invalid alarm, skip */
            continue;

        val = icalproperty_get_value(prop);

        struct trigger_data *tdata = &(triggerdata[ntriggers]);

        tdata->alarm = alarm;
        tdata->action =
            action ==
                ICAL_ACTION_DISPLAY     ? CALDAV_ALARM_ACTION_DISPLAY   :
                ICAL_ACTION_EMAIL       ? CALDAV_ALARM_ACTION_EMAIL     :
                                          CALDAV_ALARM_ACTION_NONE;

        struct icaltriggertype trigger = icalvalue_get_trigger(val);
        /* XXX validate trigger */
        memcpy(&(tdata->trigger), &trigger, sizeof(struct icaltriggertype));

        if (icalvalue_isa(val) == ICAL_DURATION_VALUE) {
            icalparameter *param = icalproperty_get_first_parameter(prop, ICAL_RELATED_PARAMETER);
            if (param && icalparameter_get_related(param) == ICAL_RELATED_END)
                tdata->type = TRIGGER_RELATIVE_END;
            else
                tdata->type = TRIGGER_RELATIVE_START;
        }
        else
            tdata->type = TRIGGER_ABSOLUTE;

        ntriggers++;
        assert(ntriggers <= nalarms);
    }

    struct recur_cb_data rdata = {
        .triggerdata    = triggerdata,
        .ntriggers      = ntriggers,
        .now            = after,
        .nextalarm      = NULL,
        .nextalarmtime  = icaltime_null_time(),
        .eventstart     = icaltime_null_time(),
        .eventend       = icaltime_null_time()
    };

    /* See if its a recurring event */
    if (icalcomponent_get_first_property(comp, ICAL_RRULE_PROPERTY) ||
        icalcomponent_get_first_property(comp, ICAL_RDATE_PROPERTY) ||
        icalcomponent_get_first_property(comp, ICAL_EXDATE_PROPERTY)) {

        icalcomponent_foreach_recurrence(
            comp,
            rdata.now,
            icaltime_from_timet_with_zone(INT_MAX, 0, utc_zone),
            recur_cb,
            &rdata);

        /* Handle overridden recurrences */
        icalcomponent *subcomp = comp;
        while ((subcomp = icalcomponent_get_next_component(subcomp, ICAL_VEVENT_COMPONENT))) {
            struct icalperiodtype period;
            caldav_get_period(subcomp, ICAL_VEVENT_COMPONENT, &period);
            icaltime_span span = icaltime_span_new(period.start, period.end, 0);
            recur_cb(subcomp, &span, &rdata);
        }
    }

    else {
        /* not recurring, use dtstart/dtend instead */
        struct icalperiodtype period;
        caldav_get_period(comp, ICAL_VEVENT_COMPONENT, &period);
        icaltime_span span = icaltime_span_new(period.start, period.end, 0);
        recur_cb(comp, &span, &rdata);
    }

    /* no next alarm, nothing more to do! */
    if (!rdata.nextalarm) {
        free(triggerdata);
        return 1;
    }

    /* now fill out alarmdata with all the stuff from event/ocurrence/alarm */
    alarmdata->action = rdata.nextaction;

    alarmdata->nextalarm = rdata.nextalarmtime;

    /* dtstart timezone is good enough for alarm purposes */
    icalproperty *prop = NULL;
    icalparameter *param = NULL;
    const char *tzid = NULL;

    prop = icalcomponent_get_first_property(comp, ICAL_DTSTART_PROPERTY);
    if (prop) param = icalproperty_get_first_parameter(prop, ICAL_TZID_PARAMETER);
    if (param) tzid = icalparameter_get_tzid(param);
    alarmdata->tzid = xstrdup(tzid ? tzid : "[floating]");

    memcpy(&(alarmdata->start), &(rdata.eventstart), sizeof(struct icaltimetype));
    memcpy(&(alarmdata->end), &(rdata.eventend), sizeof(struct icaltimetype));

    icalproperty *attendee = icalcomponent_get_first_property(rdata.nextalarm, ICAL_ATTENDEE_PROPERTY);
    while (attendee) {
        const char *email = icalproperty_get_value_as_string(attendee);
        if (email)
            strarray_append(&alarmdata->recipients, email);
        attendee = icalcomponent_get_next_property(rdata.nextalarm, ICAL_ATTENDEE_PROPERTY);
    }

    free(triggerdata);
    return 0;
}

/* clean up alarmdata after prepare */
void caldav_alarm_fini(struct caldav_alarm_data *alarmdata)
{
    free((void *) alarmdata->tzid);
    alarmdata->tzid = NULL;
    strarray_fini(&alarmdata->recipients);
}

#define CMD_SELECT_ALARM                                                        \
    "SELECT rowid, mailbox, resource, action, nextalarm, tzid, start, end"      \
    " FROM alarms WHERE"                                                        \
    " nextalarm <= :before"                                                     \
    ";"

#define CMD_SELECT_RECIPIENT            \
    "SELECT email"                      \
    " FROM alarm_recipients WHERE"      \
    " alarmid = :alarmid"               \
    ";"

struct alarmdata_list {
    struct alarmdata_list       *next;
    struct caldav_alarm_data    data;
    int                         do_delete;
};

static int alarm_read_cb(sqlite3_stmt *stmt, void *rock)
{
    struct alarmdata_list **list = (struct alarmdata_list **) rock;

    struct alarmdata_list *n = xzmalloc(sizeof(struct alarmdata_list));

    n->data.rowid       = sqlite3_column_int(stmt, 0);
    n->data.mailbox     = xstrdup((const char *) sqlite3_column_text(stmt, 1));
    n->data.resource    = xstrdup((const char *) sqlite3_column_text(stmt, 2));
    n->data.action      = sqlite3_column_int(stmt, 3);
    n->data.nextalarm   = icaltime_from_string((const char *) sqlite3_column_text(stmt, 4));
    n->data.tzid        = xstrdup((const char *) sqlite3_column_text(stmt, 5));
    n->data.start       = icaltime_from_string((const char *) sqlite3_column_text(stmt, 6));
    n->data.end         = icaltime_from_string((const char *) sqlite3_column_text(stmt, 7));

    n->do_delete = 1; // unless something goes wrong, we will delete this alarm

    n->next = *list;
    *list = n;

    return 0;
}

static int recipient_read_cb(sqlite3_stmt *stmt, void *rock)
{
    strarray_t *recipients = (strarray_t *) rock;

    strarray_append(recipients, (const char *) sqlite3_column_text(stmt, 0));

    return 0;
}

/*
 * Extract data from the given ical object
 */
static void mboxevent_extract_icalcomponent(struct mboxevent *event,
                                            icalcomponent *ical,
                                            const char *userid,
                                            const char *calname,
                                            enum caldav_alarm_action action,
                                            icaltimetype alarmtime,
                                            const char *timezone,
                                            icaltimetype start,
                                            icaltimetype end,
                                            strarray_t *recipients)
{
    icalcomponent *comp = icalcomponent_get_first_real_component(ical);

    icalproperty *prop;

    FILL_STRING_PARAM(event, EVENT_CALENDAR_ALARM_TIME,
                      xstrdup(icaltime_as_ical_string(alarmtime)));

    FILL_ARRAY_PARAM(event, EVENT_CALENDAR_ALARM_RECIPIENTS, recipients);

    FILL_STRING_PARAM(event, EVENT_CALENDAR_USER_ID, xstrdup(userid));
    FILL_STRING_PARAM(event, EVENT_CALENDAR_CALENDAR_NAME, xstrdup(calname));

    prop = icalcomponent_get_first_property(comp, ICAL_UID_PROPERTY);
    FILL_STRING_PARAM(event, EVENT_CALENDAR_UID,
                      xstrdup(prop ? icalproperty_get_value_as_string(prop) : ""));

    FILL_STRING_PARAM(event, EVENT_CALENDAR_ACTION, xstrdup(
        action == CALDAV_ALARM_ACTION_DISPLAY   ? "display" :
        action == CALDAV_ALARM_ACTION_EMAIL     ? "email" :
                                                  ""));

    prop = icalcomponent_get_first_property(comp, ICAL_SUMMARY_PROPERTY);
    FILL_STRING_PARAM(event, EVENT_CALENDAR_SUMMARY,
                      xstrdup(prop ? icalproperty_get_value_as_string(prop) : ""));

    prop = icalcomponent_get_first_property(comp, ICAL_DESCRIPTION_PROPERTY);
    FILL_STRING_PARAM(event, EVENT_CALENDAR_DESCRIPTION,
                      xstrdup(prop ? icalproperty_get_value_as_string(prop) : ""));

    prop = icalcomponent_get_first_property(comp, ICAL_LOCATION_PROPERTY);
    FILL_STRING_PARAM(event, EVENT_CALENDAR_LOCATION,
                      xstrdup(prop ? icalproperty_get_value_as_string(prop) : ""));

    prop = icalcomponent_get_first_property(comp, ICAL_ORGANIZER_PROPERTY);
    FILL_STRING_PARAM(event, EVENT_CALENDAR_ORGANIZER,
                      xstrdup(prop ? icalproperty_get_value_as_string(prop) : ""));

    FILL_STRING_PARAM(event, EVENT_CALENDAR_TIMEZONE,
                      xstrdup(timezone));
    FILL_STRING_PARAM(event, EVENT_CALENDAR_START,
                      xstrdup(icaltime_as_ical_string(start)));
    FILL_STRING_PARAM(event, EVENT_CALENDAR_END,
                      xstrdup(icaltime_as_ical_string(end)));
    FILL_UNSIGNED_PARAM(event, EVENT_CALENDAR_ALLDAY,
                        icaltime_is_date(start) ? 1 : 0);

    strarray_t *attendee_names = strarray_new();
    strarray_t *attendee_emails = strarray_new();
    strarray_t *attendee_status = strarray_new();
    prop = icalcomponent_get_first_property(comp, ICAL_ATTENDEE_PROPERTY);
    while (prop) {
        const char *email = icalproperty_get_value_as_string(prop);
        if (!email)
            continue;
        strarray_append(attendee_emails, email);

        const char *name = icalproperty_get_parameter_as_string(prop, "CN");
        strarray_append(attendee_names, name ? name : "");

        const char *partstat = icalproperty_get_parameter_as_string(prop, "PARTSTAT");
        strarray_append(attendee_status, partstat ? partstat : "");

        prop = icalcomponent_get_next_property(comp, ICAL_ATTENDEE_PROPERTY);
    }

    FILL_ARRAY_PARAM(event, EVENT_CALENDAR_ATTENDEE_NAMES, attendee_names);
    FILL_ARRAY_PARAM(event, EVENT_CALENDAR_ATTENDEE_EMAILS, attendee_emails);
    FILL_ARRAY_PARAM(event, EVENT_CALENDAR_ATTENDEE_STATUS, attendee_status);
}

/* process alarms with triggers within before a given time */
EXPORTED int caldav_alarm_process(time_t runattime)
{
    syslog(LOG_DEBUG, "processing alarms");

    // all alarms in the past and within the next 60 seconds
    icaltimetype before = runattime
                        ? icaltime_from_timet_with_zone(runattime, 0, utc_zone)
                        : icaltime_current_time_with_zone(utc_zone);
    icaltime_adjust(&before, 0, 0, 0, 60);

    struct sqldb_bindval bval[] = {
        { ":before",    SQLITE_TEXT,    { .s = icaltime_as_ical_string(before)  } },
        { NULL,         SQLITE_NULL,    { .s = NULL                             } }
    };

    struct alarmdata_list *list = NULL, *scan;

    sqldb_t *alarmdb = caldav_alarm_open();
    if (!alarmdb)
        return HTTP_SERVER_ERROR;

    int rc = sqldb_exec(alarmdb, CMD_SELECT_ALARM, bval, &alarm_read_cb, &list);
    if (rc)
        goto done;

    for (scan = list; scan; scan = scan->next) {
        struct mailbox *mailbox = NULL;
        char *userid = NULL;
        struct caldav_db *caldavdb = NULL;
        icalcomponent *ical = NULL;
        struct buf msg_buf = BUF_INITIALIZER;
        struct buf calname_buf = BUF_INITIALIZER;
        static const char *displayname_annot =
            DAV_ANNOT_NS "<" XML_NS_DAV ">displayname";

        syslog(LOG_DEBUG,
               "processing alarm rowid %llu mailbox %s resource %s action %d nextalarm %s tzid %s start %s end %s",
               scan->data.rowid, scan->data.mailbox,
               scan->data.resource, scan->data.action,
               icaltime_as_ical_string(scan->data.nextalarm),
               scan->data.tzid,
               icaltime_as_ical_string(scan->data.start),
               icaltime_as_ical_string(scan->data.end)
        );

        rc = mailbox_open_irl(scan->data.mailbox, &mailbox);
        if (rc == IMAP_MAILBOX_NONEXISTENT)
            /* mailbox was deleted or something, nothing we can do */
            continue;
        if (rc) {
            /* transient open error, don't delete this alarm */
            scan->do_delete = 0;
            continue;
        }

        userid = mboxname_to_userid(mailbox->name);

        caldavdb = caldav_open_mailbox(mailbox);
        if (!caldavdb) {
            /* XXX mailbox exists but caldav structure doesn't? delete event? */
            scan->do_delete = 0;
            goto done_item;
        }

        struct caldav_data *cdata = NULL;
        caldav_lookup_resource(caldavdb, mailbox->name, scan->data.resource, &cdata, 0);
        if (!cdata || !cdata->ical_uid)
            /* resource not found, nothing we can do */
            goto done_item;

        struct index_record record;
        memset(&record, 0, sizeof(struct index_record));
        rc = mailbox_find_index_record(mailbox, cdata->dav.imap_uid, &record);
        if (rc == IMAP_NOTFOUND) {
            /* no record, no worries */
            goto done_item;
        }
        if (rc) {
            /* XXX no index record? item deleted or transient error? */
            scan->do_delete = 0;
            goto done_item;
        }
        if (record.system_flags & FLAG_EXPUNGED) {
            /* no longer exists?  nothing to do */
            goto done_item;
        }

        rc = mailbox_map_record(mailbox, &record, &msg_buf);
        if (rc) {
            /* XXX no message? index is wrong? yikes */
            scan->do_delete = 0;
            goto done_item;
        }

        ical = icalparser_parse_string(buf_cstring(&msg_buf) + record.header_size);

        rc = annotatemore_lookupmask(mailbox->name, displayname_annot, userid, &calname_buf);
        if (rc || !calname_buf.len) buf_setcstr(&calname_buf, strrchr(mailbox->name, '.') + 1);

        mailbox_close(&mailbox);
        mailbox = NULL;

        caldav_close(caldavdb);
        caldavdb = NULL;

        if (!ical)
            /* XXX log error */
            goto done_item;

        /* fill out recipients */
        struct sqldb_bindval rbval[] = {
            { ":alarmid",       SQLITE_INTEGER, { .i = scan->data.rowid } },
            { NULL,             SQLITE_NULL,    { .s = NULL             } }
        };

        rc = sqldb_exec(alarmdb, CMD_SELECT_RECIPIENT, rbval, recipient_read_cb,
                      &scan->data.recipients);

        struct mboxevent *event = mboxevent_new(EVENT_CALENDAR_ALARM);
        mboxevent_extract_icalcomponent(event, ical, userid, buf_cstring(&calname_buf),
                                        scan->data.action, scan->data.nextalarm,
                                        scan->data.tzid,
                                        scan->data.start, scan->data.end,
                                        &scan->data.recipients);
        mboxevent_notify(event);
        mboxevent_free(&event);

        /* set up for next alarm */
        struct caldav_alarm_data alarmdata = {
            .mailbox  = scan->data.mailbox,
            .resource = scan->data.resource,
        };

        if (!caldav_alarm_prepare(ical, &alarmdata, scan->data.action, before)) {
            rc = caldav_alarm_add(alarmdb, &alarmdata);
            caldav_alarm_fini(&alarmdata);
            /* report error, but don't do anything */
        }

done_item:
        buf_free(&calname_buf);
        buf_free(&msg_buf);
        if (ical) icalcomponent_free(ical);
        if (caldavdb) caldav_close(caldavdb);
        if (userid) free(userid);
        if (mailbox) mailbox_close(&mailbox);
    }

    for (scan = list; scan; scan = scan->next)
        if (scan->do_delete)
            caldav_alarm_delete_row(alarmdb, &scan->data);

done:
    caldav_alarm_close(alarmdb);

    scan = list;
    while (scan) {
        struct alarmdata_list *next = scan->next;

        free((void*) scan->data.mailbox);
        free((void*) scan->data.resource);
        free((void*) scan->data.tzid);
        free(scan);

        scan = next;
    }

    return rc;
}
