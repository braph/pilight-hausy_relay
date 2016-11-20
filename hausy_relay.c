/*
   Copyright (C) 2016 Benjamin Abendroth

   hausy_relay module for pilight.

   This file contains code from the pilight project.
*/

/*
   Copyright (C) 2014 CurlyMo

   This file is part of pilight.

   pilight is free software: you can redistribute it and/or modify it under the
   terms of the GNU General Public License as published by the Free Software
   Foundation, either version 3 of the License, or (at your option) any later
   version.

   pilight is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
   A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with pilight. If not, see  <http://www.gnu.org/licenses/>
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../../core/pilight.h"
#include "../../core/common.h"
#include "../../core/dso.h"
#include "../../core/log.h"
#include "../protocol.h"

#include "hausy/hausy.h"
#include "hausy/pilight/pilight.h"
#include "hausy/protocols/relay/relay.h"
#include "hausy_relay.h"

#define REPEATS            30
#define MIN_PULSE_LENGTH   HAUSY_PULSE_LOW  - HAUSY_PULSE_TOLERANCE
#define MAX_PULSE_LENGTH   HAUSY_PULSE_HIGH + HAUSY_PULSE_TOLERANCE
#define MIN_RAW_LENGTH     ((4 * HAUSY_ID_BITLENGTH * 2) + 2)
#define MAX_RAW_LENGTH     (MIN_RAW_LENGTH + (4 * 3 * HAUSY_ID_BITLENGTH))

static int validate(void) {
   if (hausy_is_footer_pulse(hausy_relay->raw[hausy_relay->rawlen-1]) &&
       hausy_is_high_pulse(hausy_relay->raw[hausy_relay->rawlen-2])) {
      return 0;
   }

   return -1;
}

static void createMessage(char *systemcode_str, char *unitcode_str, int state) {
   hausy_relay->message = json_mkobject();

   json_append_member(hausy_relay->message, "systemcode", json_mkstring(systemcode_str));
   json_append_member(hausy_relay->message, "unitcode", json_mkstring(unitcode_str));

   if (state) {
      json_append_member(hausy_relay->message, "state", json_mkstring("on"));
   } else {
      json_append_member(hausy_relay->message, "state", json_mkstring("off"));
   }
}

static void parseCode(void) {
   hausy_bitstorage *data;
   size_t data_size = hausy_pilight_parse_timings(&data, hausy_relay->raw, hausy_relay->rawlen);
   if (! data_size)
      return;

   hausy_id protocol, toSystem, toUnit, command;
   size_t pos = hausy_parse_request(data, data_size, &protocol, &toSystem, &toUnit, &command);
   if (! pos) {
      free(data);
      return;
   }

   if (protocol != (hausy_id) RELAY_PROTOCOL_ID) {
      free(data);
      return;
   }

   if (command != (hausy_id) RELAY_CMD_STATE_INFORM) {
      free(data);
      return;
   }

   if (toSystem != (hausy_id) HAUSY_BROADCAST_ID) {
      free(data);
      return;
   }

   if (toUnit != (hausy_id) HAUSY_BROADCAST_ID) {
      free(data);
      return;
   }

   hausy_id systemcode, unitcode;
   hausy_bool state;

   pos = relay_parse_state_inform(data, data_size, pos, &systemcode, &unitcode, &state);
   if (! pos) {
      free(data);
      return;
   }

   char *systemcode_str = hausy_create_id(systemcode);
   char *unitcode_str   = hausy_create_id(unitcode);

   createMessage(systemcode_str, unitcode_str, state);

   free(systemcode_str);
   free(unitcode_str);
   free(data);
}

static int createCode(struct JsonNode *code) {
   int systemcode = -1;
   int unitcode = -1;
   int command = -1;
   double itmp = 0;
   char *stmp;
   char systemcode_str[10];
   char unitcode_str[10];

   if (json_find_string(code, "systemcode", &stmp) == 0) {
      strcpy(systemcode_str, stmp);
      systemcode = hausy_parse_id(systemcode_str);
   }

   if (json_find_string(code, "unitcode", &stmp) == 0) {
      strcpy(unitcode_str, stmp);
      unitcode = hausy_parse_id(unitcode_str);
   }

   if(json_find_number(code, "off", &itmp) == 0)
      command = RELAY_CMD_OFF;
   else if(json_find_number(code, "on", &itmp) == 0)
      command = RELAY_CMD_ON;
   else if(json_find_number(code, "query", &itmp) == 0)
      command = RELAY_CMD_STATE_QUERY;

   if(systemcode == -1 || unitcode == -1 || command == -1) {
      logprintf(LOG_ERR, "hausy_relay: insufficient number of arguments");
      return EXIT_FAILURE;
   } else if(systemcode > HAUSY_ID_MAX || systemcode < 0) {
      logprintf(LOG_ERR, "hausy_relay: invalid systemcode range");
      return EXIT_FAILURE;
   } else if(unitcode > HAUSY_ID_MAX || unitcode < 0) {
      logprintf(LOG_ERR, "hausy_relay: invalid unitcode range");
      return EXIT_FAILURE;
   }

   size_t data_size;

   // get request size
   if (command == RELAY_CMD_ON)
      data_size = relay_create_on(NULL, 0, 0);
   else if (command == RELAY_CMD_OFF)
      data_size = relay_create_off(NULL, 0, 0);
   else if (command == RELAY_CMD_STATE_QUERY)
      data_size = relay_create_state_query(NULL, 0, 0);

   // allocate space for request
   hausy_bitstorage *data = hausy_allocate(data_size);
   if (data == NULL) {
      logprintf(LOG_ERR, "hausy_relay: malloc()");
      return EXIT_FAILURE;
   }

   // build request, fill data
   if (command == RELAY_CMD_ON)
      data_size = relay_create_on(data, systemcode, unitcode);
   else if (command == RELAY_CMD_OFF)
      data_size = relay_create_off(data, systemcode, unitcode);
   else if (command == RELAY_CMD_STATE_QUERY)
      data_size = relay_create_state_query(data, systemcode, unitcode);


   // transform request to pilight
   hausy_relay->rawlen = hausy_pilight_create_timings(
      data,
      data_size,
      hausy_relay->raw
   );

   hausy_relay->txrpt = REPEATS;

   if (command == RELAY_CMD_ON)
      createMessage(systemcode_str, unitcode_str, 1);
   else if (command == RELAY_CMD_OFF)
      createMessage(systemcode_str, unitcode_str, 0);

   free(data);

   return EXIT_SUCCESS;
}

static void printHelp(void) {
   printf("\t -s --systemcode=systemcode\tcontrol a device with this systemcode\n");
   printf("\t -u --unitcode=unitcode\t\tcontrol a device with this unitcode\n");
   printf("\t -t --on\t\t\tsend an on signal\n");
   printf("\t -f --off\t\t\tsend an off signal\n");
   printf("\t -q --query\t\t\tquery relay states\n");
}

#if !defined(MODULE) && !defined(_WIN32)
__attribute__((weak))
#endif
void hausyRelayInit(void) {

   protocol_register(&hausy_relay);
   protocol_set_id(hausy_relay, "hausy_relay");
   protocol_device_add(hausy_relay, "hausy_relay", "Hausy Relays");
   hausy_relay->devtype = SWITCH;
   hausy_relay->hwtype = RF433;
   hausy_relay->minrawlen = MIN_RAW_LENGTH;
   hausy_relay->maxrawlen = MAX_RAW_LENGTH;
   hausy_relay->txrpt = REPEATS;
   //hausy_relay->maxgaplen = MAX_PULSE_LENGTH*PULSE_DIV;
   //hausy_relay->mingaplen = MIN_PULSE_LENGTH*PULSE_DIV;

   options_add(&hausy_relay->options, 's', "systemcode", OPTION_HAS_VALUE, DEVICES_ID, JSON_STRING, NULL, "^0a[0-9a-zA-Z_@]{1,5}$");
   options_add(&hausy_relay->options, 'u', "unitcode", OPTION_HAS_VALUE, DEVICES_ID, JSON_STRING, NULL, "^0a[0-9a-zA-Z_@]{1,5}$");
   options_add(&hausy_relay->options, 't', "on", OPTION_NO_VALUE, DEVICES_STATE, JSON_STRING, NULL, NULL);
   options_add(&hausy_relay->options, 'f', "off", OPTION_NO_VALUE, DEVICES_STATE, JSON_STRING, NULL, NULL);
   options_add(&hausy_relay->options, 'q', "query", OPTION_NO_VALUE, DEVICES_STATE, JSON_STRING, NULL, NULL);

   options_add(&hausy_relay->options, 0, "readonly", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)0, "^[10]{1}$");
   options_add(&hausy_relay->options, 0, "confirm", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)0, "^[10]{1}$");

   hausy_relay->parseCode  = &parseCode;
   hausy_relay->createCode = &createCode;
   hausy_relay->printHelp  = &printHelp;
   hausy_relay->validate   = &validate;
}

#if defined(MODULE) && !defined(_WIN32)
void compatibility(struct module_t *module) {
   module->name = "hausy_relay";
   module->version = "1.0";
   module->reqversion = "6.0";
   module->reqcommit = "1";
}

void init(void) {
   hausyRelayInit();
}
#endif
