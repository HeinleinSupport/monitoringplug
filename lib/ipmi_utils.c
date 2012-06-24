/***
 * Monitoring Plugin - ipmi_utils.c
 **
 *
 * Copyright (C) 2012 Marius Rieder <marius.rieder@durchmesser.ch>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * $Id$
 */

#include "mp_common.h"
#include "ipmi_utils.h"

#include <string.h>
#include <OpenIPMI/ipmi_smi.h>

/**
 * Privat function
 */
static void mp_ipmi_log(os_handler_t *hnd, const char *format,
        enum ipmi_log_type_e log_type, va_list ap);
void mp_ipmi_setup_done(ipmi_domain_t *domain, int err, unsigned int conn_num,
        unsigned int port_num, int still_connected, void *user_data);
void mp_ipmi_domain_up(ipmi_domain_t *domain, void *cb_data);
static void mp_ipmi_entity_change(enum ipmi_update_e op,
        ipmi_domain_t* domain, ipmi_entity_t *entity, void *cb_data);
static void mp_ipmi_sensor_change(enum ipmi_update_e op, ipmi_entity_t *ent,
        ipmi_sensor_t *sensor, void *cb_data);
static void sensor_read_handler (ipmi_sensor_t *sensor, int err,
        enum ipmi_value_present_e value_present,
        unsigned int __attribute__((unused)) raw_value,
        double value, ipmi_states_t __attribute__((unused)) *states,
        void *user_data);
static void sensor_thresholds_handler(ipmi_sensor_t *sensor, int err,
        ipmi_thresholds_t *th, void *cb_data);

/**
 * Global Varables
 */
int mp_ipmi_init_done = 0;
int mp_ipmi_entity = IPMI_ENTITY_ID_UNSPECIFIED;
int mp_ipmi_open = IPMI_OPEN_OPTION_SDRS;
struct mp_ipmi_sensor_list *mp_ipmi_sensors = NULL;

void mp_ipmi_init(void) {
    int rv;

    /* OS handler allocated first. */
    mp_ipmi_hnd = ipmi_posix_setup_os_handler();

    if (!mp_ipmi_hnd)
        unknown("Can't allocate OpenIPMI OS Handler.");

    /* Initialize the OpenIPMI library. */
    if (mp_verbose > 1)
        printf("Init OpenIPMI OS Handler.\n");
    ipmi_init(mp_ipmi_hnd);

    if (mp_verbose > 1)
        printf("Connect OpenIPMI.\n");
    rv = ipmi_smi_setup_con(0, mp_ipmi_hnd, NULL, &mp_ipmi_con);

    if (rv)
        unknown("Can't setup OpenIPMI connection: %s", strerror(rv));

    ipmi_open_option_t open_option[2];
    memset (open_option, 0, sizeof (open_option));
    open_option[0].option = IPMI_OPEN_OPTION_ALL;
    open_option[0].ival = 0;
    open_option[1].option = mp_ipmi_open;
    open_option[1].ival = 1;

    if (mp_verbose > 1)
        printf("Open OpenIPMI Domain.\n");
    rv = ipmi_open_domain(progname, &mp_ipmi_con, 1,
            mp_ipmi_setup_done, NULL,
            mp_ipmi_domain_up, NULL,
            open_option, sizeof (open_option) / sizeof (open_option[0]), NULL);

    if (rv)
        unknown("Can't open OpenIPMI domain: %s", strerror(rv));

    while(!mp_ipmi_init_done) {
        mp_ipmi_hnd->perform_one_op(mp_ipmi_hnd, NULL);
    }

    return;
}

void mp_ipmi_deinit(void) {
    if (mp_verbose > 1)
        printf("Free OpenIPMI OS Handler.\n");
    mp_ipmi_hnd->free_os_handler(mp_ipmi_hnd);
}

void print_revision_ipmi(void) {
    printf(" OpenIPMI v%s\n", ipmi_openipmi_version());
}

static void mp_ipmi_log(os_handler_t *hnd, const char *format,
                enum ipmi_log_type_e log_type, va_list ap) {
    int nl = 1;

    switch (log_type) {
        case IPMI_LOG_INFO:
            if (mp_verbose < 3) return;
            printf("OpenIPMI Info: ");
            break;
        case IPMI_LOG_WARNING:
            if (mp_verbose < 2) return;
            printf("OpenIPMI Warn: ");
            break;
        case IPMI_LOG_SEVERE:
            printf("OpenIPMI Serv: ");
            break;
        case IPMI_LOG_FATAL:
            printf("OpenIPMI Fatl: ");
            break;
        case IPMI_LOG_ERR_INFO:
            printf("OpenIPMI Einf: ");
            break;
        case IPMI_LOG_DEBUG_START:
            nl = 0;
            /* FALLTHROUGH */
        case IPMI_LOG_DEBUG:
            if (mp_verbose < 4) return;
            printf("OpenIPMI Debg: ");
            break;
        case IPMI_LOG_DEBUG_CONT:
            nl = 0;
            /* FALLTHROUGH */
        case IPMI_LOG_DEBUG_END:
            break;
    }

    vprintf(format, ap);

    if (nl)
        printf("\n");
}

void mp_ipmi_setup_done(ipmi_domain_t *domain, int err, unsigned int conn_num,
                unsigned int port_num, int still_connected, void *user_data) {
    int rv;

    /* Register a callback function entity_change. When a new entities
       is created, entity_change is called */
    rv = ipmi_domain_add_entity_update_handler(domain, mp_ipmi_entity_change, domain);
    if (rv)
        unknown("ipmi_domain_add_entity_update_handler return error: %", rv);
}

void mp_ipmi_domain_up(ipmi_domain_t *domain, void *cb_data) {
    if (mp_verbose > 1)
        printf("OpenIPMI Domain Up.\n");
     mp_ipmi_init_done = 1;
     mp_ipmi_dom = domain;
}

static void mp_ipmi_entity_change(enum ipmi_update_e op, ipmi_domain_t *domain,
        ipmi_entity_t *entity, void *cb_data) {
    int id, rv;

    id = ipmi_entity_get_entity_id(entity);

    if (mp_verbose > 1)
        printf("OpenIPMI Entity Change: %s\n", ipmi_get_entity_id_string(id));

    if (mp_ipmi_entity != IPMI_ENTITY_ID_UNSPECIFIED && mp_ipmi_entity != id)
        return;

    rv = ipmi_entity_add_sensor_update_handler(entity, mp_ipmi_sensor_change,
            entity);

    if (rv)
        unknown("ipmi_entity_set_sensor_update_handler: 0x%x", rv);
}

static void mp_ipmi_sensor_change(enum ipmi_update_e op, ipmi_entity_t *ent,
        ipmi_sensor_t *sensor, void *cb_data) {
    char *name;
    int len;
    struct mp_ipmi_sensor_list *s;
    ipmi_sensor_id_t sid;

    len = ipmi_sensor_get_id_length(sensor);
    name = mp_malloc(len+1);
    ipmi_sensor_get_id(sensor, name, len);

    s = mp_malloc(sizeof(struct mp_ipmi_sensor_list));
    memset (s, 0, sizeof(struct mp_ipmi_sensor_list));
    s->name = name;
    s->next = mp_ipmi_sensors;
    mp_ipmi_sensors = s;

    sid = ipmi_sensor_convert_to_id(sensor);
    ipmi_sensor_id_get_reading(sid, sensor_read_handler, s);
    ipmi_sensor_id_get_thresholds(sid,sensor_thresholds_handler, s);
}

static void sensor_read_handler (ipmi_sensor_t *sensor, int err,
        enum ipmi_value_present_e value_present,
        unsigned int __attribute__((unused)) raw_value,
        double value, ipmi_states_t __attribute__((unused)) *states,
        void *user_data) {
    struct mp_ipmi_sensor_list *s;
    s = (struct mp_ipmi_sensor_list *) user_data;

    s->value = value;
}

static void sensor_thresholds_handler(ipmi_sensor_t *sensor, int err,
        ipmi_thresholds_t *th, void *user_data) {
    struct mp_ipmi_sensor_list *s;
    double val;
    int rv;

    s = (struct mp_ipmi_sensor_list *) user_data;
    setWarn(&s->sensorThresholds, "~:", -1);
    setCrit(&s->sensorThresholds, "~:", -1);

    rv = ipmi_threshold_get(th, IPMI_LOWER_CRITICAL, &val);
    if (rv == 0) {
        s->sensorThresholds->critical->start = val;
        s->sensorThresholds->critical->start_infinity = 0;
    }

    rv = ipmi_threshold_get(th, IPMI_LOWER_NON_CRITICAL, &val);
    if (rv == 0) {
        s->sensorThresholds->warning->start = val;
        s->sensorThresholds->warning->start_infinity = 0;
    }

    rv = ipmi_threshold_get(th, IPMI_UPPER_CRITICAL, &val);
    if (rv == 0) {
        s->sensorThresholds->critical->end = val;
        s->sensorThresholds->critical->end_infinity = 0;
    }

    rv = ipmi_threshold_get(th, IPMI_UPPER_NON_CRITICAL, &val);
    if (rv == 0) {
        s->sensorThresholds->warning->end = val;
        s->sensorThresholds->warning->end_infinity = 0;
    }

    if (mp_verbose > 3)
        print_thresholds(s->name, s->sensorThresholds);
}



/* vim: set ts=4 sw=4 et syn=c : */