# Support for 1-wire based temperature sensors
#
# Copyright (C) 2020 Alan Lord <alanslists@gmail.com>
#
# This file may be distributed under the terms of the GNU GPLv3 license.
import logging

DS18_REPORT_TIME = 3.0
# Temperature can be sampled at any time but conversion time is ~750ms so make
# sure not to read too often.
DS18_MIN_REPORT_TIME = 1.0

class DS18B20:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.name = config.get_name().split()[-1]
        self.sensor_id = config.get("serial_no")
        self.min_temp = config.getfloat('min_temp')
        self.max_temp = config.getfloat('max_temp')
        self.report_time = config.getfloat(
            'ds18_report_time',
            DS18_REPORT_TIME,
            minval=DS18_MIN_REPORT_TIME
        )
        #TODO Will need to somehow identify the correct mcu...?
        self.all_mcus = [
            m for n, m in self.printer.lookup_objects(module='mcu')]
        self.mcu = self.all_mcus[0]
        self.oid = self.mcu.create_oid()
        self.mcu.register_response(self._handle_ds18b20_response,
            "ds18b20_result", self.oid)
        self.mcu.register_config_callback(self._build_config)

    def _build_config(self):
        self.mcu.add_config_cmd("config_ds18b20 oid=%d serial=%s" % (self.oid,
            self.sensor_id.encode("hex")))

        clock = self.mcu.get_query_slot(self.oid)
        self._report_clock = self.mcu.seconds_to_clock(self.report_time)
        self.mcu.add_config_cmd("query_ds18b20 oid=%d clock=%u rest_ticks=%u"
            " min_value=%d max_value=%d" % (
                self.oid, clock, self._report_clock,
                self.min_temp * 1000, self.max_temp * 1000), is_init=True)

    def _handle_ds18b20_response(self, params):
        temp = params['value'] / 1000.0
        logging.info("Temp: %f" % temp)
        next_clock      = self.mcu.clock32_to_clock64(params['next_clock'])
        last_read_clock = next_clock - self._report_clock
        last_read_time  = self.mcu.clock_to_print_time(last_read_clock)
        self._callback(last_read_time, temp)

    def setup_minmax(self, min_temp, max_temp):
        pass

    def fault(self, msg):
        self.printer.invoke_async_shutdown(msg)

    def get_report_time_delta(self):
        return self.report_time

    def setup_callback(self, cb):
        self._callback = cb

    def get_status(self, eventtime):
        return {
            'temperature': self.temp,
        }

def load_config(config):
    # Register sensor
    pheaters = config.get_printer().load_object(config, "heaters")
    pheaters.add_sensor_factory("DS18B20", DS18B20)
